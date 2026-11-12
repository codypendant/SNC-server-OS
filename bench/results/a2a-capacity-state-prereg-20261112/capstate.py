#!/usr/bin/env python3
"""A2-a capacity-state characterization driver (PREREG.md revision 2). NOT RUN: written and tested offline only.

Purpose: observe, never force, the two saturation-batching states seen in A2-a (frames/recv 13.2-13.9 vs 15.6-15.8).
It imports a2a.py and pilot.py (frozen, unchanged) and reuses a2a.session() for the storage reset, with a different run
order (S0 C1 C2 C3 S1) and an identical, observational instrumentation around every measured client:

  perf stat counting on the server CPUs, the client CPUs and package energy (1 s, 80 s each), sar 1 s, /proc snapshots at the
  measured-window start t_m, t_m+30 s and t_m+60 s, numa_maps/status/smaps_rollup of the server and client, ss, nstat, WAL extents.
  Samplers run under `taskset -c 34,70` (never a benchmark CPU or an SMT sibling of one). Nothing changes any setting.

The instrumentation is injected by wrapping `pilot.subprocess.run` (pilot.py is not edited): only the measured net_bench
(--seconds > 0) is intercepted; the load step and everything else pass through.

  capstate.py run OUT --scratch DIR --manifest M [--build build-s6]
  capstate.py analyze OUT          # descriptive classification + preregistered metrics + separation rule; no verdict
"""
import argparse, glob, json, os, re, shutil, statistics, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "a2a-storage-repro-r640"))
import a2a  # noqa: E402  (also puts bench/ on sys.path)
pilot = a2a.pilot

K = 5
ORDER = ["S0", "C1", "C2", "C3", "S1"]
PHASE = "capstate"
BOUNDARY = 15.0                       # PREREG section 4: frames/recv >= 15.0 is large-batch (in the observed gap)
SAMPLER_CPUS = "34,70"                # one physical core, node 0; neither a benchmark CPU nor an SMT sibling of one
SAMPLE_S = 80                         # uniform: anchors are 10 s warm-up + 60 s window + up to 10 s drain
WINDOW_S = 60
SERVER_CPUS = [2, 4, 6, 8, 10, 12, 14, 16]
CLIENT_CPUS = [18, 20, 22, 24, 26, 28, 30, 32]
PINNED = SERVER_CPUS + CLIENT_CPUS
SIBLINGS = [c + 36 for c in PINNED]   # SMT siblings on this host (cpu N pairs with N+36)
EVENTS = ("cycles,ref-cycles,instructions,task-clock,context-switches,cpu-migrations,msr/aperf/,msr/mperf/,msr/tsc/,"
          "syscalls:sys_enter_recvfrom,syscalls:sys_enter_sendto")
CSV = lambda xs: ",".join(map(str, xs))
PERF = {
    "server": ["/usr/bin/perf", "stat", "-a", "-C", CSV(SERVER_CPUS), "-e", EVENTS, "-I", "1000", "-x,", "--", "sleep", str(SAMPLE_S)],
    "client": ["/usr/bin/perf", "stat", "-a", "-C", CSV(CLIENT_CPUS), "-e", EVENTS, "-I", "1000", "-x,", "--", "sleep", str(SAMPLE_S)],
    "power": ["/usr/bin/perf", "stat", "-a", "-e", "power/energy-pkg/,power/energy-ram/", "-I", "1000", "-x,", "--", "sleep", str(SAMPLE_S)],
}
SUDOERS = os.path.join(HERE, "sudoers-snc-capstate.proposed")
SYSCTLS = ["net.ipv4.tcp_rmem", "net.ipv4.tcp_wmem", "net.core.rmem_max", "net.core.wmem_max", "net.core.netdev_budget",
           "net.core.netdev_budget_usecs", "net.core.busy_poll", "net.core.busy_read", "net.core.rps_sock_flow_entries",
           "net.ipv4.tcp_autocorking", "net.core.somaxconn"]

# injectable for the offline test
spawn = subprocess.Popen
now = time.time
sleep = time.sleep
run_real = subprocess.run
_real_subprocess = subprocess


def utc():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now()))


# ------------------------------------------------------------------ the sudoers file == the commands in this file

def sudoers_commands(text):
    """The argv lists of the NOPASSWD lines of a sudoers file (backslash-escaped commas and colons undone)."""
    out = []
    for line in text.splitlines():
        if line.startswith("#") or "NOPASSWD:" not in line:
            continue
        cmd = line.split("NOPASSWD:", 1)[1].strip()
        out.append(re.sub(r"\\(.)", r"\1", cmd).split())
    return out


def sudoers_problems(text):
    got, want = sudoers_commands(text), list(PERF.values())
    return [] if got == want else [f"sudoers commands {got} != the driver's {want}"]


# ------------------------------------------------------------------ classification (PREREG section 4)

def frames_per_recv(server_log):
    for line in server_log.splitlines():
        m = re.match(r"total: requests \d+ forwarded [0-9.]+% frames/recv ([0-9.]+) frames/send ([0-9.]+)", line)
        if m:
            return float(m[1]), float(m[2])
    return None, None


def classify(frames_recv):
    return None if frames_recv is None else ("large-batch" if frames_recv >= BOUNDARY else "normal-batch")


# ------------------------------------------------------------------ run-kind timeline (PREREG section 3, revision 2)

def timeline(cmd):
    """(kind, name, t_m offset, window s) of a measured net_bench command line, from the command itself."""
    anchor = "--rate" in cmd
    warm = float(cmd[cmd.index("--warmup") + 1]) if anchor else 0.0
    window = float(cmd[cmd.index("--seconds") + 1])
    name = os.path.basename(cmd[cmd.index("--ids") + 1])[:-len(".ids")]
    return dict(kind="anchor" if anchor else "capacity", name=name, t_m_offset=warm, window_s=window)


def is_measure(cmd):
    try:
        return (any(os.path.basename(c) == "net_bench" for c in cmd) and "--seconds" in cmd
                and float(cmd[cmd.index("--seconds") + 1]) > 0)
    except (ValueError, IndexError):
        return False


def analysed_intervals(kind):
    """1-based one-second intervals after t0 that make up the measured window: 1..60 capacity, 11..70 anchor."""
    return (1, WINDOW_S) if kind == "capacity" else (11, 10 + WINDOW_S)


# ------------------------------------------------------------------ snapshots (observation only)

def readf(path):
    try:
        return open(path).read()
    except OSError as e:
        return f"UNREADABLE {path}: {e}\n"


def cmd_out(cmd):
    try:
        p = run_real(cmd, capture_output=True, text=True, timeout=30)
        return f"$ {' '.join(cmd)}\n[rc {p.returncode}]\n{p.stdout}{p.stderr}"
    except Exception as e:  # noqa: BLE001 observation must never stop a run
        return f"$ {' '.join(cmd)}\nERROR {e}\n"


def server_pid(port):
    for d in glob.glob("/proc/[0-9]*"):
        try:
            argv = open(d + "/cmdline", "rb").read().split(b"\0")
        except OSError:
            continue
        if any(a.endswith(b"snc_server") for a in argv) and b"--port" in argv and argv[argv.index(b"--port") + 1] == str(port).encode():
            return int(d.rsplit("/", 1)[1])
    return None


def snap_counters(d, tag):
    """nstat, softirqs and pressure at one instant."""
    os.makedirs(d, exist_ok=True)
    open(f"{d}/{tag}-time.txt", "w").write(f"{now():.6f}\n")
    open(f"{d}/{tag}-nstat.txt", "w").write(cmd_out(["nstat", "-az"]))
    open(f"{d}/{tag}-softirqs.txt", "w").write(readf("/proc/softirqs"))
    for r in ("cpu", "io", "memory"):
        open(f"{d}/{tag}-psi-{r}.txt", "w").write(readf(f"/proc/pressure/{r}"))


def snap_process(d, tag, cmd, client_pid, data_dir):
    """numa_maps/status/smaps_rollup of the server and the client, THP, ss, WAL extents."""
    os.makedirs(d, exist_ok=True)
    port = int(cmd[cmd.index("--port") + 1])
    open(f"{d}/{tag}-time.txt", "w").write(f"{now():.6f}\n")
    for who, pid in (("server", server_pid(port)), ("client", client_pid)):
        open(f"{d}/{tag}-{who}-pid.txt", "w").write(f"{pid}\n")
        for f in ("numa_maps", "status", "smaps_rollup"):
            open(f"{d}/{tag}-{who}-{f}.txt", "w").write(readf(f"/proc/{pid}/{f}") if pid else "NO PID\n")
    open(f"{d}/{tag}-thp.txt", "w").write(readf("/sys/kernel/mm/transparent_hugepage/enabled")
                                           + readf("/sys/kernel/mm/transparent_hugepage/defrag"))
    open(f"{d}/{tag}-ss.txt", "w").write(cmd_out(["ss", "-H", "-tin", "state", "established",
                                                  f"( sport >= :{port} and sport <= :{port + 7} )"]))
    wals = sorted(glob.glob(os.path.join(data_dir, "**", "*.wal"), recursive=True))
    open(f"{d}/{tag}-filefrag.txt", "w").write("".join(cmd_out(["filefrag", w]) for w in wals) if wals else "NO WAL FILES\n")


def session_env(d):
    """Before each session (PREREG item E): offload, RPS/XPS, sysctls, THP; no setting is changed."""
    os.makedirs(d, exist_ok=True)
    open(f"{d}/ethtool-lo.txt", "w").write(cmd_out(["ethtool", "-k", "lo"]))
    open(f"{d}/lo-queues.txt", "w").write("".join(f"{f}: {readf(f)}" for f in sorted(glob.glob("/sys/class/net/lo/queues/*/*ps_cpus"))
                                                   + sorted(glob.glob("/sys/class/net/lo/queues/*/rps_flow_cnt"))))
    open(f"{d}/sysctl.txt", "w").write(cmd_out(["sysctl"] + SYSCTLS))
    open(f"{d}/nstat.txt", "w").write(cmd_out(["nstat", "-az"]))
    open(f"{d}/thp.txt", "w").write(readf("/sys/kernel/mm/transparent_hugepage/enabled")
                                    + readf("/sys/kernel/mm/transparent_hugepage/defrag"))


# ------------------------------------------------------------------ the instrumented measured run

def sampler_cmds(d):
    """name -> (argv, stderr file). The perf commands are exactly those of sudoers-snc-capstate.proposed."""
    taskset = ["taskset", "-c", SAMPLER_CPUS]
    c = {n: (taskset + ["sudo", "-n"] + argv, f"{d}/perf-{n}.csv") for n, argv in PERF.items()}
    c["sar"] = (taskset + ["sar", "-o", f"{d}/sar.sa", "-u", "-P", CSV(PINNED), "-q", "-w", "-B", "-d", "-n", "DEV,SOFT",
                           "1", str(SAMPLE_S)], f"{d}/sar.err")
    return c


def measured_run(cmd, timeout, out, **kw):
    """Run one measured net_bench as subprocess.run would, with the PREREG instrumentation. Returns CompletedProcess."""
    tl = timeline(cmd)
    d = os.path.join(out, "instr", tl["name"])
    os.makedirs(d, exist_ok=True)
    fo, fe = tempfile.TemporaryFile("w+"), tempfile.TemporaryFile("w+")
    meta = dict(timeline=tl, sampler_cpus=SAMPLER_CPUS, samplers={})
    cl = spawn(cmd, stdout=fo, stderr=fe, text=True)
    t0 = now()
    meta["t0_utc_epoch"] = t0
    procs = {}
    for n, (argv, errf) in sampler_cmds(d).items():
        ef = open(errf, "w")
        procs[n] = (spawn(argv, stdout=subprocess.DEVNULL, stderr=ef), ef)
        meta["samplers"][n] = dict(argv=argv, started_after_t0_s=now() - t0, pid=procs[n][0].pid)
    try:
        for n, (p, _) in procs.items():
            try:
                meta["samplers"][n]["affinity"] = sorted(os.sched_getaffinity(p.pid))
            except OSError:
                meta["samplers"][n]["affinity"] = None
    except Exception:  # noqa: BLE001
        pass
    tm = t0 + tl["t_m_offset"]
    port_cmd = cmd
    ddir = kw.get("data_dir")
    events = [(tm, "start", lambda: snap_counters(d, "tm")),
              (tm + 30, "mid", lambda: snap_process(d, "tm30", port_cmd, cl.pid, ddir or "")),
              (tm + tl["window_s"], "end", lambda: snap_counters(d, "tm60"))]
    done = []
    deadline = t0 + timeout
    while True:
        t = now()
        for ev in events:
            if ev[1] not in done and t >= ev[0]:
                ev[2]()
                done.append(ev[1])
        if cl.poll() is not None and len(done) == len(events):
            break
        if t > deadline:
            cl.kill()
            cl.wait()
            raise _real_subprocess.TimeoutExpired(cmd, timeout)
        sleep(0.05)
    t_end = now()
    meta["client_exit_after_t0_s"] = t_end - t0
    meta["offset_s"] = (t_end - t0) - tl["t_m_offset"] - tl["window_s"]     # recorded; >= 0 (anchor: plus drain)
    problems = []
    for n, (p, ef) in procs.items():
        try:
            rc = p.wait(timeout=SAMPLE_S + 60)
        except _real_subprocess.TimeoutExpired:
            p.kill()
            rc = "timeout"
        ef.close()
        meta["samplers"][n]["rc"] = rc
        if rc != 0:
            problems.append(f"sampler {n} rc {rc}")
    fo.seek(0), fe.seek(0)
    so, se = fo.read(), fe.read()
    open(f"{d}/meta.json", "w").write(json.dumps(meta, indent=1))
    if problems:
        raise a2a.Incomplete(f"{tl['name']}: instrumentation failed: {'; '.join(problems)}")
    return _real_subprocess.CompletedProcess(cmd, cl.returncode, so, se)


class Shim:
    """Stands in for `pilot.subprocess`: everything passes through except the measured net_bench run."""

    def __init__(self, out, data_root):
        self.out, self.data_root = out, data_root

    def __getattr__(self, k):
        return getattr(_real_subprocess, k)

    def run(self, cmd, *a, **kw):
        if is_measure(cmd):
            name = timeline(cmd)["name"]
            return measured_run(cmd, kw.get("timeout") or 1e9, self.out, data_dir=os.path.join(self.data_root, name))
        return _real_subprocess.run(cmd, *a, **kw)


# ------------------------------------------------------------------ the run

def preflight(p, ref, dev, sudoers_text):
    bad = a2a.extra_preflight(p, ref, dev)
    bad += sudoers_problems(sudoers_text)
    for n, argv in PERF.items():
        if a2a.sh(["sudo", "-n", "-l"] + argv)[0] != 0:
            bad.append(f"sudo -n cannot run the {n} perf command (install sudoers-snc-capstate after review)")
    for tool in ("perf", "sar", "sadf", "nstat", "ss", "ethtool", "filefrag", "taskset", "sysctl"):
        if not shutil.which(tool):
            bad.append(f"{tool} not found")
    for c in [int(x) for x in SAMPLER_CPUS.split(",")]:
        if c in PINNED or c in SIBLINGS:
            bad.append(f"sampler cpu {c} is a benchmark CPU or sibling")
    return bad


def execute(p, plan, want):
    res = p.results.setdefault("CapState", dict(sessions=[]))
    a2a.ORDER, a2a.PHASE = ORDER, PHASE
    try:
        for k in range(1, K + 1):
            rec = dict(k=k, complete=False)
            res["sessions"].append(rec)
            session_env(os.path.join(p.a.out, "instr", f"k{k}-session-env"))
            a2a.session(p, plan, k, want, rec)
            p.write()
    except pilot.Abort as e:            # Incomplete, a failed sampler, or pilot's own abort
        res["aborted"] = str(e)
        print(f"ABORTED: {e}", file=sys.stderr)
    res["complete"] = "aborted" not in res and len(res["sessions"]) == K and all(s.get("complete") for s in res["sessions"])
    p.write()
    return res


def run(a):
    plan = json.load(open(a.plan))
    ref = a2a.cp1a_record()
    a.quick, a.allocation, a.data_root, a.scratch_given = False, plan["baseline_allocation"], a2a.DATA_ROOT, True
    os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
    p = pilot.Pilot(a, plan)
    o = a2a.observe(a.build)
    bad = preflight(p, ref, (o["identity"] or {}).get("disk"), open(SUDOERS).read() if os.path.exists(SUDOERS) else "")
    want = dict(ref, boot_id=o["boot_id"], head=o["head"], identity=o["identity"])
    bad += a2a.guard_problems(o, want)
    if bad:
        raise SystemExit("refusing: " + "; ".join(bad))
    p.env.update(validation="a2a-capacity-state", cp1a_identity=ref, guard_reference=want, K=K, order=ORDER,
                 boundary_frames_per_recv=BOUNDARY, sampler_cpus=SAMPLER_CPUS, sample_s=SAMPLE_S,
                 perf_commands=PERF, anchor_rate=a2a.ANCHOR, settle_s=a2a.SETTLE_S, root_commands=a2a.ROOT,
                 approved_device=a2a.APPROVED, device_identity_at_start=o["identity"])
    p.write()
    pilot.subprocess = Shim(a.out, a2a.DATA_ROOT)
    try:
        res = execute(p, plan, want)
    finally:
        pilot.subprocess = _real_subprocess
        shutil.rmtree(a2a.DATA_ROOT, ignore_errors=True)
        try:
            os.rmdir(a.scratch)
        except OSError:
            pass
    print("DONE " + ("complete (no verdict: characterization only)" if res["complete"] else f"INCOMPLETE: {res.get('aborted')}"))
    return 0


# ------------------------------------------------------------------ analysis (pure; frozen before data)

def parse_perf_csv(text):
    """perf stat -I -x, output -> {event: {interval_index(1-based): (value, dt)}}; errors on <not counted>."""
    out, last = {}, {}
    for line in text.splitlines():
        f = line.split(",")
        if len(f) < 4 or not re.match(r"^\d+\.\d+$", f[0]):
            continue
        if not re.match(r"^[0-9.]+$", f[1]):
            raise ValueError(f"unusable perf value {f[1]!r} for {f[3]}")
        t, ev = float(f[0]), f[3]
        i = int(round(t))
        out.setdefault(ev, {})[i] = (float(f[1]), t - last.get(ev, 0.0))
        last[ev] = t
    return out


def wsum(series, ev, lo, hi):
    return sum(v for i, (v, _) in series[ev].items() if lo <= i <= hi)


def wdt(series, ev, lo, hi):
    return sum(dt for i, (_, dt) in series[ev].items() if lo <= i <= hi)


def numa_fraction_node1(text):
    tot = n1 = 0
    for line in text.splitlines():
        for node, n in re.findall(r"\bN(\d+)=(\d+)", line):
            tot += int(n)
            n1 += int(n) if node == "1" else 0
    return None if tot == 0 else n1 / tot


def smaps_kb(text, key):
    m = re.search(rf"^{key}:\s+(\d+) kB", text, re.M)
    return int(m[1]) if m else None


def nstat_value(text, key):
    m = re.search(rf"^{key}\s+(\d+)", text, re.M)
    return int(m[1]) if m else None


def wal_extents(text):
    ex = [int(m[1]) for m in re.finditer(r": (\d+) extents? found", text)]
    return None if not ex else sum(ex) / len(ex)


def lo_bytes_per_packet(sadf_dev_csv, lo_idx, hi_idx):
    """sar -n DEV lo rows (sadf -d), records lo_idx..hi_idx (1-based, in order): rxkB*1024 / rxpck."""
    rows = [l.split(";") for l in sadf_dev_csv.splitlines() if l and not l.startswith("#") and l.split(";")[3] == "lo"]
    sel = rows[lo_idx - 1:hi_idx]
    pck = sum(float(r[4]) for r in sel)
    kb = sum(float(r[6]) for r in sel)
    return None if pck == 0 else kb * 1024 / pck


def sadf_dev(path):
    return run_real(["sadf", "-d", path, "--", "-n", "DEV"], capture_output=True, text=True).stdout


def capacity_metrics(d, rec, perf_server, perf_client, perf_power, sar_dev, lo, hi):
    """The PREREG section 5 primary metrics for one capacity run (intervals lo..hi = 1..60). Missing -> None."""
    ops = rec["exec_ops"] + rec["index_ok"]
    m = {}
    s, c, w = perf_server, perf_client, perf_power
    m["P1a_server_effective_mhz"] = (wsum(s, "msr/tsc/", lo, hi) / (len(SERVER_CPUS) * wdt(s, "msr/tsc/", lo, hi)) / 1e6
                                     * wsum(s, "msr/aperf/", lo, hi) / wsum(s, "msr/mperf/", lo, hi))
    m["P1b_package_watts"] = wsum(w, "power/energy-pkg/", lo, hi) / wdt(w, "power/energy-pkg/", lo, hi)
    m["P2a_server_instructions_per_op"] = wsum(s, "instructions", lo, hi) / ops
    m["P2b_server_ctx_switches_per_s"] = wsum(s, "context-switches", lo, hi) / wdt(s, "context-switches", lo, hi)
    m["P3a_client_sendto_per_op"] = wsum(c, "syscalls:sys_enter_sendto", lo, hi) / ops
    m["P3b_client_recvfrom_per_op"] = wsum(c, "syscalls:sys_enter_recvfrom", lo, hi) / ops
    m["P4a_server_node1_page_fraction"] = numa_fraction_node1(readf(f"{d}/tm30-server-numa_maps.txt"))
    m["P4b_server_anon_huge_kb"] = smaps_kb(readf(f"{d}/tm30-server-smaps_rollup.txt"), "AnonHugePages")
    m["P5a_lo_bytes_per_packet"] = lo_bytes_per_packet(sar_dev, lo, hi)
    a0, a1 = (nstat_value(readf(f"{d}/{t}-nstat.txt"), "TcpExtTCPAutoCorking") for t in ("tm", "tm60"))
    m["P5b_autocork_per_op"] = None if a0 is None or a1 is None else (a1 - a0) / ops
    m["P6a_wal_extents_per_file"] = wal_extents(readf(f"{d}/tm30-filefrag.txt"))
    m["P6b_write_bytes_per_op"] = rec["write_bytes_per_op"]
    return m


def separation(large, normal):
    """PREREG section 6. Needs >= 2 large and >= 2 normal runs. Separates iff ranges are disjoint and the gap is at least the
    larger within-state range."""
    if len(large) < 2 or len(normal) < 2 or any(v is None for v in large + normal):
        return dict(evaluated=False)
    lr, nr = (min(large), max(large)), (min(normal), max(normal))
    gap = max(lr[0] - nr[1], nr[0] - lr[1])
    within = max(lr[1] - lr[0], nr[1] - nr[0])
    return dict(evaluated=True, large=lr, normal=nr, disjoint=gap > 0, gap=gap, within=within, separates=gap > 0 and gap >= within)


def analyze(a):
    d = json.load(open(os.path.join(a.out, "pilot.json")))
    runs = [r for r in d["runs"] if r.get("phase") == PHASE and r["slot"].startswith("C")]
    sessions = {s["k"]: s for s in d["results"]["CapState"]["sessions"]}
    table = []
    for r in runs:
        name = f"A-{PHASE}-k{r['session']}-{r['slot']}"
        fr, fs = frames_per_recv(readf(os.path.join(a.out, "raw", name + ".server.log")))
        idir = os.path.join(a.out, "instr", name)
        series = {n: parse_perf_csv(readf(f"{idir}/perf-{n}.csv")) for n in ("server", "client", "power")}
        sadev = sadf_dev(f"{idir}/sar.sa")
        lo, hi = analysed_intervals("capacity")
        row = dict(run=name, session=r["session"], slot=r["slot"], frames_recv=fr, frames_send=fs, state=classify(fr),
                   throughput=r["total_ops_s"],
                   **capacity_metrics(idir, r, series["server"], series["client"], series["power"], sadev, lo, hi))
        # instrument validity (PREREG section 3): server recvfrom over the window vs requests / (frames/recv) within 3%
        req = int(re.search(r"total: requests (\d+)", readf(os.path.join(a.out, "raw", name + ".server.log")))[1])
        got = wsum(series["server"], "syscalls:sys_enter_recvfrom", lo, hi)
        row["instrument_recv_ratio"] = got / (req / fr) if fr else None
        row["instrument_ok"] = row["instrument_recv_ratio"] is not None and abs(row["instrument_recv_ratio"] - 1) <= 0.03
        row["N1_ssd_temp_c_at_start"] = sessions[r["session"]]["start"]["smart"]["temperature_c"]
        row["N2_session_index"] = r["session"]
        table.append(row)
    names = [k for k in table[0] if k[0] in "PN" and k[1].isdigit()] if table else []
    large = [r for r in table if r["state"] == "large-batch"]
    normal = [r for r in table if r["state"] == "normal-batch"]
    sep = {m: separation([r[m] for r in large], [r[m] for r in normal]) for m in names}
    res = dict(runs=table, n_large=len(large), n_normal=len(normal),
               large_sessions=sorted({r["session"] for r in large}), normal_sessions=sorted({r["session"] for r in normal}),
               separation=sep, note="descriptive only; no verdict; see PREREG sections 6 and 7")
    json.dump(res, open(os.path.join(a.out, "characterization.json"), "w"), indent=1, sort_keys=True)
    print(json.dumps({k: res[k] for k in ("n_large", "n_normal", "large_sessions", "normal_sessions")}))
    for m, s in sep.items():
        print(m, "separates" if s.get("separates") else ("not evaluated" if not s["evaluated"] else "does not separate"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("out")
    r.add_argument("--scratch", required=True)
    r.add_argument("--manifest", required=True)
    r.add_argument("--build", default="build-s6")
    r.add_argument("--plan", default="bench/pilot_plan.json")
    j = sub.add_parser("analyze")
    j.add_argument("out")
    a = ap.parse_args()
    return run(a) if a.cmd == "run" else analyze(a)


if __name__ == "__main__":
    sys.exit(main())
