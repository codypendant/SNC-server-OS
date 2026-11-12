#!/usr/bin/env python3
"""Deterministic checks of capstate.py before any data. Starts no process, installs nothing, runs no perf/sar.

  C1 constants and the sudoers file: K 5; order S0 C1 C2 C3 S1; sampler duration 80 s uniform; boundary 15.0; sampler
     CPUs 34,70 are neither benchmark CPUs nor their SMT siblings; the three perf commands equal the proposed sudoers lines
     (a changed sleep / CPU list / event is caught); no strace / bpftrace / epoll_wait / perf trace anywhere in them
  C2 classification on the real A2-a server logs: the 5 session-2 capacity runs are large-batch, the other 20 normal;
     15.0 is large, 14.99 normal
  C3 timelines from the real A2-a client command lines: capacity t_m = t0, anchor t_m = t0 + 10 (warm-up); the load step is
     not intercepted; analysed intervals 1..60 (capacity) and 11..70 (anchor)
  C4 parsers on real-format captures (perf -I -x, csv, power csv, numa_maps, smaps_rollup, nstat, filefrag, sar -n DEV)
  C5 the primary metrics on synthetic series with known answers; separation rule cases
  C6 the instrumented run with spawn / clock stubbed: samplers under `taskset -c 34,70` + `sudo -n` + the exact perf
     argv; snapshots at t_m, t_m+30, t_m+60 for a capacity run (t0, +30, +60) and an anchor (t0+10, +40, +70); every sampler
     waited; a failed sampler, a client timeout; the load step passes through the shim untouched
  C7 execute(): a2a.ORDER / PHASE patched to the 5-slot order, K sessions, per-session env capture, an abort recorded
  C8 analyze() end to end on a synthetic output directory
"""
import json, os, re, shutil, subprocess, sys, tempfile, types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import capstate as cs  # noqa: E402

A2A_OUT = os.path.join(HERE, "..", "a2a-storage-repro-r640", "out")
FX = os.path.join(HERE, "fixtures")
fails = []


def check(name, cond, detail=""):
    if not cond:
        fails.append(name)
        print(f"FAIL {name} {detail}")


def fx(n):
    return open(os.path.join(FX, n)).read()


# ---------------- C1
check("C1 K", cs.K == 5)
check("C1 order", cs.ORDER == ["S0", "C1", "C2", "C3", "S1"])
check("C1 sample_s", cs.SAMPLE_S == 80 and all(c[-1] == "80" for c in cs.PERF.values()))
check("C1 boundary", cs.BOUNDARY == 15.0)
for c in [int(x) for x in cs.SAMPLER_CPUS.split(",")]:
    check(f"C1 sampler cpu {c} free", c not in cs.PINNED and c not in cs.SIBLINGS)
check("C1 siblings", cs.SIBLINGS == [c + 36 for c in range(2, 33, 2)])
sud = open(cs.SUDOERS).read()
check("C1 sudoers == driver", cs.sudoers_problems(sud) == [], cs.sudoers_problems(sud))
for what, bad in (("sleep", sud.replace("sleep 80", "sleep 65")), ("cpus", sud.replace("2\\,4\\,6", "2\\,4\\,8", 1)),
                  ("event", sud.replace("msr/tsc/\\,", "", 1)), ("extra line", sud + "solo ALL=(root) NOPASSWD: /usr/bin/perf trace\n")):
    check(f"C1 sudoers mutation {what} caught", cs.sudoers_problems(bad) != [])
flat = " ".join(" ".join(a) for a in cs.PERF.values())
for banned in ("strace", "bpftrace", "epoll_wait", "perf trace", "record", " trace "):
    check(f"C1 no {banned}", banned not in flat)
check("C1 events", cs.EVENTS.split(",") == ["cycles", "ref-cycles", "instructions", "task-clock", "context-switches", "cpu-migrations",
                                           "msr/aperf/", "msr/mperf/", "msr/tsc/", "syscalls:sys_enter_recvfrom",
                                           "syscalls:sys_enter_sendto"])
check("C1 no sdX in driver", not re.search(r"\bsd[a-z]\b", open(os.path.join(HERE, "capstate.py")).read()))

# ---------------- C2
large = normal = 0
for k in range(1, 6):
    for c in range(1, 6):
        fr, fs = cs.frames_per_recv(open(f"{A2A_OUT}/raw/A-a2a-k{k}-C{c}.server.log").read())
        st = cs.classify(fr)
        if k == 2:
            large += st == "large-batch"
        else:
            normal += st == "normal-batch"
check("C2 real logs: 5 large (session 2), 20 normal", (large, normal) == (5, 20), (large, normal))
check("C2 boundary", cs.classify(15.0) == "large-batch" and cs.classify(14.99) == "normal-batch" and cs.classify(None) is None)
check("C2 fixture log", cs.frames_per_recv(fx("server-k2-C3.log")) == (15.7, 17.7))

# ---------------- C3
def real_cmd(slot):
    return open(f"{A2A_OUT}/raw/A-a2a-k1-{slot}.client.log").read().splitlines()[1].split()


cap, anc = real_cmd("C1"), real_cmd("S0")
tc, ta = cs.timeline(cap), cs.timeline(anc)
check("C3 capacity", (tc["kind"], tc["t_m_offset"], tc["window_s"], tc["name"]) == ("capacity", 0.0, 60.0, "A-a2a-k1-C1"), tc)
check("C3 anchor", (ta["kind"], ta["t_m_offset"], ta["window_s"], ta["name"]) == ("anchor", 10.0, 60.0, "A-a2a-k1-S0"), ta)
load = cap[:cap.index("--seconds") + 1] + ["0"]
check("C3 measure vs load", cs.is_measure(cap) and cs.is_measure(anc) and not cs.is_measure(load) and not cs.is_measure(["ls"]))
check("C3 intervals", cs.analysed_intervals("capacity") == (1, 60) and cs.analysed_intervals("anchor") == (11, 70))

# ---------------- C4
ps = cs.parse_perf_csv(fx("perf-server.csv"))
check("C4 perf events", set(ps) == set(cs.EVENTS.split(",")), sorted(ps))
check("C4 perf intervals 1..3", sorted(ps["cycles"]) == [1, 2, 3] and 0.9 < ps["cycles"][2][1] < 1.1)
check("C4 perf value", ps["cycles"][1][0] == 5770847)
try:
    cs.parse_perf_csv("1.0,<not counted>,,cycles,0,0.00,,\n")
    check("C4 not counted refused", False)
except ValueError:
    pass
pw = cs.parse_perf_csv(fx("perf-power.csv"))
check("C4 power", set(pw) == {"power/energy-pkg/", "power/energy-ram/"} and pw["power/energy-pkg/"][1][0] == 82.69)
nm = fx("numa_maps.txt")
toks = [int(t.split("=")[1]) for l in nm.splitlines() for t in l.split() if re.match(r"^N\d+=", t)]
n1 = sum(int(t.split("=")[1]) for l in nm.splitlines() for t in l.split() if t.startswith("N1="))
check("C4 numa fraction", abs(cs.numa_fraction_node1(nm) - n1 / sum(toks)) < 1e-12 and cs.numa_fraction_node1("none") is None)
check("C4 smaps", cs.smaps_kb(fx("smaps_rollup.txt"), "AnonHugePages") == 0 and cs.smaps_kb(fx("smaps_rollup.txt"), "Rss") > 0)
check("C4 nstat", cs.nstat_value(fx("nstat.txt"), "TcpExtTCPAutoCorking") is not None and cs.nstat_value("x 1", "y") is None)
check("C4 filefrag", cs.wal_extents(fx("filefrag.txt")) == 1.0 and cs.wal_extents("a: 3 extents found\nb: 5 extents found\n") == 4.0
      and cs.wal_extents("NO WAL FILES\n") is None)
dev = "# h;i;t;IFACE;rxpck/s;txpck/s;rxkB/s;txkB/s\n" + "".join(f"h;1;t;lo;{p};0;{kb};0\nh;1;t;eno1;9;9;9;9\n" for p, kb in ((100, 4), (300, 8)))
check("C4 lo bytes/packet", abs(cs.lo_bytes_per_packet(dev, 1, 2) - (12 * 1024 / 400)) < 1e-9 and cs.lo_bytes_per_packet(fx("sar-dev.csv"), 1, 2) is None)

# ---------------- C5
def series(vals, n=65, dt=1.0):
    return {ev: {i: (v(i) if callable(v) else v, dt) for i in range(1, n + 1)} for ev, v in vals.items()}


srv = series({"msr/tsc/": 8 * 2.7e9, "msr/aperf/": 3.4e9 * 8 * .5, "msr/mperf/": 2.7e9 * 8 * .5, "instructions": 1e10,
              "context-switches": 5000, "cycles": 1, "ref-cycles": 1})
cli = series({"syscalls:sys_enter_sendto": 200000, "syscalls:sys_enter_recvfrom": 100000})
pwr = series({"power/energy-pkg/": 190.0, "power/energy-ram/": 20.0})
rec = dict(exec_ops=230_000_000, index_ok=10_000_000, write_bytes_per_op=70.0)
d = tempfile.mkdtemp()
open(f"{d}/tm30-server-numa_maps.txt", "w").write("a N0=3 N1=1\nb N0=4\n")
open(f"{d}/tm30-server-smaps_rollup.txt", "w").write("AnonHugePages:      2048 kB\n")
open(f"{d}/tm-nstat.txt", "w").write("TcpExtTCPAutoCorking 100 0.0\n")
open(f"{d}/tm60-nstat.txt", "w").write("TcpExtTCPAutoCorking 2500100 0.0\n")
open(f"{d}/tm30-filefrag.txt", "w").write("x: 2 extents found\ny: 4 extents found\n")
m = cs.capacity_metrics(d, rec, srv, cli, pwr, "".join(f"h;1;t;lo;{1000};0;{50};0\n" for _ in range(65)), 1, 60)
ops = 240_000_000
exp = {"P1a_server_effective_mhz": 2700 * (3.4e9 / 2.7e9), "P1b_package_watts": 190.0, "P2a_server_instructions_per_op": 60 * 1e10 / ops,
       "P2b_server_ctx_switches_per_s": 5000, "P3a_client_sendto_per_op": 60 * 200000 / ops, "P3b_client_recvfrom_per_op": 60 * 100000 / ops,
       "P4a_server_node1_page_fraction": 1 / 8, "P4b_server_anon_huge_kb": 2048, "P5a_lo_bytes_per_packet": 50 * 1024 / 1000,
       "P5b_autocork_per_op": 2_500_000 / ops, "P6a_wal_extents_per_file": 3.0, "P6b_write_bytes_per_op": 70.0}
for k, v in exp.items():
    check(f"C5 {k}", m[k] is not None and abs(m[k] - v) <= 1e-9 * max(1, abs(v)), (m[k], v))
check("C5 window respects lo..hi", abs(cs.capacity_metrics(d, rec, series({**{k: 1 for k in srv}, "instructions": lambda i: 1e10 if i <= 30 else 0,
      "msr/tsc/": 1, "msr/aperf/": 1, "msr/mperf/": 1}), cli, pwr, "", 1, 30)["P2a_server_instructions_per_op"] - 30e10 / ops) < 1e-9)
S = cs.separation
check("C5 sep clean", S([10, 11], [1, 2]) ["separates"] and S([1, 2], [10, 11])["separates"])
check("C5 sep overlap", not S([1, 5], [4, 9])["separates"] and not S([1, 5], [4, 9])["disjoint"])
check("C5 sep gap < within", S([10, 20], [1, 9.5])["disjoint"] and not S([10, 20], [1, 9.5])["separates"])
check("C5 sep needs 2+2", not S([10], [1, 2])["evaluated"] and not S([], [])["evaluated"] and not S([1, None], [3, 4])["evaluated"])

# ---------------- C6 (spawn / clock / sleep / snapshots stubbed)
class Clock:
    t = 1000.0


class Proc:
    def __init__(self, argv, end, rc=0, **kw):
        self.argv, self.end, self.rc, self.pid = argv, end, rc, 4000 + len(spawned)
        self.returncode = None
        self.killed = False

    def poll(self):
        if Clock.t >= self.end and self.returncode is None:
            self.returncode = self.rc
        return self.returncode

    def wait(self, timeout=None):
        Clock.t = max(Clock.t, self.end)
        self.returncode = self.rc
        return self.rc

    def kill(self):
        self.killed = True


spawned, calls = [], []


def fake_spawn(argv, **kw):
    is_client = "net_bench" in " ".join(argv) and "taskset" not in argv[0]
    end = Clock.t + (client_len if is_client else 80)
    rc = 0 if is_client else (bad_rc if "sar" in argv else 0)
    p = Proc(argv, end, rc)
    spawned.append(p)
    if is_client:
        kw["stdout"].write("run output\n")
    return p


def setup_stubs(client_seconds, rc_sar=0):
    global client_len, bad_rc
    client_len, bad_rc = client_seconds, rc_sar
    spawned.clear(), calls.clear()
    Clock.t = 1000.0
    cs.spawn, cs.now = fake_spawn, lambda: Clock.t
    cs.sleep = lambda s: setattr(Clock, "t", Clock.t + s)
    cs.snap_counters = lambda dd, tag: calls.append((tag, round(Clock.t - 1000.0, 2)))
    cs.snap_process = lambda dd, tag, cmd, pid, ddir: calls.append((tag, round(Clock.t - 1000.0, 2)))
    cs.sleep(0)


out = tempfile.mkdtemp()
setup_stubs(60.1)
r = cs.measured_run(cap, 2000, out, data_dir="/nowhere")
check("C6 capacity snapshots at t0, +30, +60", [c[0] for c in calls] == ["tm", "tm30", "tm60"]
      and abs(calls[0][1]) < .2 and abs(calls[1][1] - 30) < .2 and abs(calls[2][1] - 60) < .2, calls)
sam = [p for p in spawned if p.argv[0] == "taskset"]
check("C6 four samplers under taskset 34,70 + sudo -n + exact perf", len(sam) == 4 and all(p.argv[:3] == ["taskset", "-c", "34,70"] for p in sam)
      and [p.argv[3:] for p in sam[:3]] == [["sudo", "-n"] + a for a in cs.PERF.values()], [p.argv[:5] for p in sam])
check("C6 sar", sam[3].argv[3] == "sar" and sam[3].argv[-2:] == ["1", "80"] and "-n" in sam[3].argv and "DEV,SOFT" in sam[3].argv)
check("C6 returns client output", r.stdout == "run output\n" and r.returncode == 0)
meta = json.load(open(f"{out}/instr/A-a2a-k1-C1/meta.json"))
check("C6 meta", meta["timeline"]["kind"] == "capacity" and meta["sampler_cpus"] == "34,70" and abs(meta["offset_s"]) < 1
      and all(s["rc"] == 0 for s in meta["samplers"].values()), meta)
check("C6 samplers waited (clock >= sampler end)", Clock.t >= 1000 + 80)
setup_stubs(80.0)
cs.measured_run(anc, 2000, out, data_dir="/nowhere")
check("C6 anchor snapshots at t0+10, +40, +70", [c[0] for c in calls] == ["tm", "tm30", "tm60"]
      and abs(calls[0][1] - 10) < .2 and abs(calls[1][1] - 40) < .2 and abs(calls[2][1] - 70) < .2, calls)
meta = json.load(open(f"{out}/instr/A-a2a-k1-S0/meta.json"))
check("C6 anchor offset records the drain", abs(meta["offset_s"] - 10) < .5, meta["offset_s"])
setup_stubs(60.1, rc_sar=2)
try:
    cs.measured_run(cap, 2000, out, data_dir="/nowhere")
    check("C6 failed sampler -> Incomplete", False)
except cs.a2a.Incomplete as e:
    check("C6 failed sampler -> Incomplete", "sampler sar rc 2" in str(e), str(e))
setup_stubs(5000)
try:
    cs.measured_run(cap, 300, out, data_dir="/nowhere")
    check("C6 client timeout", False)
except subprocess.TimeoutExpired:
    check("C6 client timeout", spawned[0].killed)
# the shim: load step untouched, measure intercepted
setup_stubs(60.1)
seen = []
orig_run = subprocess.run
subprocess.run = lambda cmd, *a, **kw: (seen.append(cmd), subprocess.CompletedProcess(cmd, 0, "L", ""))[1]
try:
    sh = cs.Shim(out, "/srv/x")
    lr = sh.run(load, capture_output=True, text=True, timeout=5)
    n_before = len(spawned)
    mr = sh.run(cap, capture_output=True, text=True, timeout=2000)
finally:
    subprocess.run = orig_run
check("C6 shim passes the load step through", seen == [load] and lr.stdout == "L" and n_before == 0, (seen, n_before))
check("C6 shim intercepts the measured run", mr.stdout == "run output\n" and len(spawned) == 5)
check("C6 shim delegates attributes", cs.Shim(out, "x").PIPE == subprocess.PIPE)

# ---------------- C7
class FakeP:
    def __init__(self, out):
        self.a = types.SimpleNamespace(out=out)
        self.results, self.n = {}, 0

    def write(self):
        self.n += 1


order_seen = []
orig_session = cs.a2a.session


def fake_session(p, plan, k, want, rec):
    order_seen.append((k, list(cs.a2a.ORDER), cs.a2a.PHASE))
    if k == 3 and abort_at3:
        raise cs.pilot.Abort("boom")
    rec["complete"] = True


cs.a2a.session = fake_session
env_dirs = []
cs.session_env = lambda dd: env_dirs.append(os.path.basename(dd))
try:
    abort_at3 = False
    p = FakeP(tempfile.mkdtemp())
    res = cs.execute(p, {}, {})
    check("C7 five sessions, order and phase patched", [x[0] for x in order_seen] == [1, 2, 3, 4, 5]
          and all(x[1] == ["S0", "C1", "C2", "C3", "S1"] and x[2] == "capstate" for x in order_seen))
    check("C7 env per session", env_dirs == [f"k{k}-session-env" for k in range(1, 6)] and res["complete"] is True)
    order_seen.clear(), env_dirs.clear()
    abort_at3 = True
    p = FakeP(tempfile.mkdtemp())
    res = cs.execute(p, {}, {})
    check("C7 abort recorded, no replacement session", res["aborted"] == "boom" and res["complete"] is False
          and [x[0] for x in order_seen] == [1, 2, 3] and len(res["sessions"]) == 3)
finally:
    cs.a2a.session = orig_session

# ---------------- C8 analyze() on a synthetic output directory
def perf_csv(events, n=65):
    rows = []
    for i in range(1, n + 1):
        for ev, v in events.items():
            val = v(i) if callable(v) else v
            rows.append(f"{i + 0.001:.9f},{val},{'Joules' if ev.startswith('power') else ''},{ev},8000000000,100.00,,\n")
    return "".join(rows)


def server_log(fr, fs, req):
    return f"total: requests {req} forwarded 0.0% frames/recv {fr} frames/send {fs} ring msgs/request 1.12\n"


o = tempfile.mkdtemp()
runs, sess = [], []
for k in (1, 2):
    sess.append(dict(k=k, complete=True, start=dict(smart=dict(temperature_c=34 + k))))
    for c in (1, 2, 3):
        large_run = k == 2
        slot = f"C{c}"
        name = f"A-capstate-k{k}-{slot}"
        os.makedirs(f"{o}/raw", exist_ok=True)
        os.makedirs(f"{o}/instr/{name}", exist_ok=True)
        fr = 15.7 if large_run else 13.7
        req = 300_000_000
        open(f"{o}/raw/{name}.server.log", "w").write(server_log(fr, fr + 2, req))
        recv = req / fr
        base = dict(tsc=8 * 2.7e9)
        open(f"{o}/instr/{name}/perf-server.csv", "w").write(perf_csv({
            "msr/tsc/": 8 * 2.7e9, "msr/aperf/": 1.25 * 8 * 2.7e9 * (1.0 if large_run else 1.0), "msr/mperf/": 8 * 2.7e9,
            "instructions": 1e10 * (0.9 if large_run else 1.0), "context-switches": 4000, "cycles": 1, "ref-cycles": 1,
            "syscalls:sys_enter_recvfrom": recv / 60 * (1.10 if (k, c) == (1, 3) else (1.02 if (k, c) == (1, 2) else 1.0)),
            "syscalls:sys_enter_sendto": 1}))
        open(f"{o}/instr/{name}/perf-client.csv", "w").write(perf_csv({"syscalls:sys_enter_sendto": 100000, "syscalls:sys_enter_recvfrom": 90000}))
        open(f"{o}/instr/{name}/perf-power.csv", "w").write(perf_csv({"power/energy-pkg/": 190.0, "power/energy-ram/": 20.0}))
        for f, t in (("tm30-server-numa_maps.txt", "x N0=10\n"), ("tm30-server-smaps_rollup.txt", "AnonHugePages: 0 kB\n"),
                     ("tm-nstat.txt", "TcpExtTCPAutoCorking 1 0.0\n"), ("tm60-nstat.txt", "TcpExtTCPAutoCorking 11 0.0\n"),
                     ("tm30-filefrag.txt", "w: 2 extents found\n")):
            open(f"{o}/instr/{name}/{f}", "w").write(t)
        runs.append(dict(phase="capstate", session=k, slot=slot, total_ops_s=4.36e6 if large_run else 4.0e6, exec_ops=2.2e8, index_ok=1e7,
                         write_bytes_per_op=70.0))
runs.append(dict(phase="capstate", session=1, slot="S0"))     # anchors are not classified
json.dump(dict(runs=runs, results=dict(CapState=dict(sessions=sess))), open(f"{o}/pilot.json", "w"))
cs.sadf_dev = lambda path: "".join(f"h;1;t;lo;{1000 if i % 2 else 1000};0;{50 if '-k2-' in path else 40};0\n" for i in range(65))
cs.analyze(types.SimpleNamespace(out=o))
res = json.load(open(f"{o}/characterization.json"))
check("C8 counts", (res["n_large"], res["n_normal"], res["large_sessions"], res["normal_sessions"]) == (3, 3, [2], [1]), res["n_large"])
check("C8 states", all(r["state"] == ("large-batch" if r["session"] == 2 else "normal-batch") for r in res["runs"]))
check("C8 instrument validity: +2% ok, +10% flagged, others ok",
      {r["run"]: r["instrument_ok"] for r in res["runs"]} == {f"A-capstate-k{k}-C{c}": (k, c) != (1, 3) for k in (1, 2) for c in (1, 2, 3)},
      {r["run"]: round(r["instrument_recv_ratio"], 3) for r in res["runs"]})
sp = res["separation"]
check("C8 P2a separates, P1a does not", sp["P2a_server_instructions_per_op"]["separates"] and not sp["P1a_server_effective_mhz"]["separates"],
      (sp["P2a_server_instructions_per_op"], sp["P1a_server_effective_mhz"]))
check("C8 P5a separates (50 vs 40 kB over 1000 pkt)", sp["P5a_lo_bytes_per_packet"]["separates"])
check("C8 negative controls reported", "N1_ssd_temp_c_at_start" in sp and sp["N1_ssd_temp_c_at_start"]["separates"] and "N2_session_index" in sp)
check("C8 no verdict key", "verdict" not in res)

shutil.rmtree(o, ignore_errors=True)
print(f"{len(fails)} failures" if fails else "ALL OK")
sys.exit(1 if fails else 0)
