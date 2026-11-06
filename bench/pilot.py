#!/usr/bin/env python3
"""SNC-only within-system pilot (docs/PILOT_PLAN.md; CAMPAIGN_PREP CP-1A).

Every parameter comes from a committed plan file (default
bench/pilot_plan.json); its SHA-256 is recorded with the results. Per
profile, in this order:

  calibration  one closed-loop run at calibration_depth: P0, used only to
               place the rate grid
  duration     open-loop runs at one rate for several window lengths (check
               that the chosen window is long enough)
  sweep        the open-loop rate grid R_k = floor(k * grid_step * P0),
               k = 1..grid_points, reps each, in fixed-seed shuffled order;
               then the stable peak, the objective and the knee (exact rule
               below and in PILOT_PLAN.md)
  fixed        fixed_reps open-loop runs at the objective rate (CV of p99),
               with memory measured after settling
  capacity     capacity_reps closed-loop runs (CV of throughput)
  shards       closed-loop capacity at each shard count (throughput only)

A real pilot refuses to start unless the tree is clean, a manifest is given
and every CPU's governor is `performance`. --quick runs a tiny version on
any host for checking the harness; its output is never evidence.

  bench/pilot.py --out DIR --manifest FILE [--allocation PILOT-L|PILOT-X]
                 [--profiles A,C] [--build build] [--data-root DIR]
                 [--plan bench/pilot_plan.json] [--quick]
"""
import argparse, hashlib, json, os, random, re, shutil, signal, socket, statistics, subprocess, sys, time
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_check

CLK = os.sysconf("SC_CLK_TCK")


# --- the exact rules (tests/test_pilot.py) ---

def cv(values):
    """Sample standard deviation / mean; None with under two values or a zero mean."""
    if len(values) < 2 or statistics.mean(values) == 0:
        return None
    return statistics.stdev(values) / statistics.mean(values)


def grid(p0, step, points):
    """The offered rates: R_k = floor(k * step * P0) ops/s, k = 1..points (step a Fraction)."""
    return [int(k * step * p0) for k in range(1, points + 1)]


def sustained(rep, plan):
    """(True, []) if one open-loop rep sustained its offered rate, else (False, reasons).

    Every condition is exact integer arithmetic on net_bench's counts:
    no errors of any kind (EXEC, INDEX, stale); nothing refused (E_OVERLOADED
    on EXEC or INDEX); completed in the window >= min_completed_fraction of
    offered x window; end backlog <= max_end_backlog_fraction of the ops
    scheduled in the measured window (the warm-up does not dilute it);
    and everything owed was issued and answered within the drain limit."""
    if rep.get("failed"):
        return False, ["run failed: " + rep.get("why", "")]
    why = []
    if rep["errors"] or rep["index_errors"] or rep["stale"]:
        why.append("errors")
    if rep["overloaded"] or rep["index_refused"]:
        why.append("refused (overloaded)")
    need = Fraction(plan["min_completed_fraction"]) * rep["offered"] * rep["window_s"]
    if rep["completed_in_window"] < need:
        why.append(f"completed {rep['completed_in_window']} < {float(need):.0f}")
    if rep["end_backlog"] > Fraction(plan["max_end_backlog_fraction"]) * rep["scheduled_in_window"]:
        why.append(f"end backlog {rep['end_backlog']} of {rep['scheduled_in_window']} scheduled in the window")
    if not rep["drained"]:
        why.append(f"not drained: {rep['remaining_unissued']} unissued, {rep['remaining_in_flight']} in flight")
    return not why, why


def sweep_result(points, plan):
    """The stable peak S, the objective and the informational knee, from
    {offered rate: [rep, ...]}.

    1. A point is sustained if every one of its reps is (sustained()). A
       point with a missing or failed rep is not.
    2. S = the largest rate r such that every point at or below r is
       sustained. None if the lowest point is not.
    3. Objective rate = the largest grid rate <= f * S (exact fractions);
       objective p99 = the median of its reps' total p99. reps is odd, so
       the median is one measured value.
    4. Knee (reported only) = the smallest rate <= S whose median p99 is
       more than knee_p99_multiple x the median p99 at the lowest rate.
    5. Client limit (PR #61 review C3). A failed point is
       client_limited_suspect if none of its reps had an error or a refusal
       and their median issue-lateness p99 is over 1 ms: the load generator
       may have fallen behind, not SNC. If the boundary point (the lowest
       unsustained one, which sets S) is suspect, the sweep is INVALID: S,
       objective and knee are None and `invalid` says why. A suspect point
       above the boundary keeps its flag as a diagnostic and changes nothing.
    Rates are distinct integers, so no step has a tie."""
    f = Fraction(plan["objective_fraction"])
    rows = []
    for r in sorted(points):
        reps = points[r]
        checks = [sustained(x, plan) for x in reps]
        ok = len(reps) == plan["reps"] and all(c[0] for c in checks)
        good = [x for x in reps if not x.get("failed")]
        # Informational: a failed point with no refusal or error, whose ops
        # were issued late, may be limited by the client, not the server.
        suspect = (not ok and bool(good) and not any(x["overloaded"] or x["index_refused"] or x["errors"]
                                                      or x["index_errors"] for x in good)
                   and statistics.median(x.get("issue_late_p99_ns", 0) for x in good) > 1_000_000)
        rows.append(dict(rate=r, sustained=ok, reasons=sorted({w for c in checks for w in c[1]}),
                         client_limited_suspect=suspect,
                         median_p99_ns=statistics.median(x["p99_ns"] for x in good) if good else None,
                         median_achieved=statistics.median(x["achieved"] for x in good) if good else None))
    peak = None
    for row in rows:
        if not row["sustained"]:
            break
        peak = row["rate"]
    out = dict(points=rows, stable_peak=peak, objective_fraction=str(f), objective_rate=None,
               objective_p99_ns=None, knee_rate=None, invalid=None)
    boundary = next((row for row in rows if not row["sustained"]), None)
    if boundary is not None and boundary["client_limited_suspect"]:
        out.update(stable_peak=None,
                   invalid=f"client_limited_suspect at the boundary point {boundary['rate']} ops/s: "
                           "the load generator, not SNC, may have set S")
        return out
    if peak is None:
        return out
    below = [row for row in rows if row["rate"] <= peak]
    obj = [row for row in below if row["rate"] <= f * peak]
    if obj:
        out.update(objective_rate=obj[-1]["rate"], objective_p99_ns=obj[-1]["median_p99_ns"])
    base = below[0]["median_p99_ns"]
    knee = [row for row in below if row["median_p99_ns"] > plan["knee_p99_multiple"] * base]
    out["knee_rate"] = knee[0]["rate"] if knee else None
    return out


def per_op(before, after, ops):
    """Section 4 metrics over a window, from two samples of one server.
    Loopback RX and TX are kept apart and are diagnostic only: on loopback
    every byte is both sent and received, so neither is network cost."""
    keys = ("cpu_us_per_op", "write_bytes_per_op", "loopback_rx_bytes_per_op", "loopback_tx_bytes_per_op")
    if ops <= 0:
        return dict.fromkeys(keys)
    return dict(cpu_us_per_op=(after["cpu_s"] - before["cpu_s"]) * 1e6 / ops,
                write_bytes_per_op=(after["write_bytes"] - before["write_bytes"]) / ops,
                loopback_rx_bytes_per_op=(after["lo_rx"] - before["lo_rx"]) / ops,
                loopback_tx_bytes_per_op=(after["lo_tx"] - before["lo_tx"]) / ops)


def parse_client(out):
    """net_bench's report: totals, each kind's latency, and the --rate block."""
    r = {}
    m = re.search(r"\n  ops/s \d+  errors (\d+)  overloaded (\d+)  stale (\d+)", out)
    r.update(errors=int(m[1]), overloaded=int(m[2]), stale=int(m[3]))
    m = re.search(r"index ops/s \d+  ok (\d+)  empty (\d+)  refused (\d+)  errors (\d+)", out)
    r.update(index_ok=int(m[1]), index_empty=int(m[2]), index_refused=int(m[3]), index_errors=int(m[4]))
    m = re.search(r"total ops/s (\d+)  \(exec (\d+) \+ index (\d+) in ([\d.]+)s\)  served_all (\d+)", out)
    r.update(total_ops_s=int(m[1]), exec_ops=int(m[2]), run_s=float(m[4]), served_all=int(m[5]))
    for kind in ("total", "read", "deposit", "transfer", "index"):
        m = re.search(rf"\n  {kind} latency ns  n (\d+)  p50 (\d+)  p95 (\d+)  p99 (\d+)  p99.9 (\d+)  max (\d+)", out)
        if not m:
            continue
        pre = "" if kind == "total" else kind + "_"
        r.update({pre + "n": int(m[1]), pre + "p50_ns": int(m[2]), pre + "p95_ns": int(m[3]),
                  pre + "p99_ns": int(m[4]), pre + "p999_ns": int(m[5]), pre + "max_ns": int(m[6])})
    m = re.search(r"\nrate: offered (\d+) ops/s  warmup ([\d.]+)s  window ([\d.]+)s", out)
    if m:
        r.update(offered=int(m[1]), warmup_s=float(m[2]))
        m = re.search(r"scheduled (\d+)  scheduled_in_window (\d+)  issued_by_end (\d+)  completed_in_window (\d+)  "
                      r"achieved (\d+) ops/s", out)
        r.update(scheduled=int(m[1]), scheduled_in_window=int(m[2]), issued_by_end=int(m[3]),
                 completed_in_window=int(m[4]), achieved=int(m[5]))
        m = re.search(r"end_backlog (\d+) \([\d.]+% of scheduled_in_window\)  in_flight_at_end (\d+)", out)
        r.update(end_backlog=int(m[1]), in_flight_at_end=int(m[2]))
        m = re.search(r"issue lateness ns  p50 (\d+)  p99 (\d+)  p99.9 (\d+)  max (\d+)  late_over_1ms (\d+)", out)
        r.update(issue_late_p50_ns=int(m[1]), issue_late_p99_ns=int(m[2]), issue_late_max_ns=int(m[4]),
                 late_over_1ms=int(m[5]))
        m = re.search(r"drained (yes|no)  remaining_unissued (\d+)  remaining_in_flight (\d+)", out)
        r.update(drained=m[1] == "yes", remaining_unissued=int(m[2]), remaining_in_flight=int(m[3]))
    return r


# --- measurement ---

def sample(pid):
    f = open(f"/proc/{pid}/stat").read()
    fields = f[f.rindex(")") + 2:].split()
    cpu = (int(fields[11]) + int(fields[12])) / CLK  # utime + stime, all threads
    io = dict(l.split(": ") for l in open(f"/proc/{pid}/io").read().splitlines())
    lo = {x: int(open(f"/sys/class/net/lo/statistics/{x}_bytes").read()) for x in ("rx", "tx")}
    return dict(cpu_s=cpu, write_bytes=int(io["write_bytes"]), lo_rx=lo["rx"], lo_tx=lo["tx"])


def memory_kb(pid):
    """VmRSS and VmHWM from status, Pss from smaps_rollup (kB)."""
    m = {}
    for line in open(f"/proc/{pid}/status"):
        k = line.split(":")[0]
        if k in ("VmRSS", "VmHWM"):
            m[k] = int(line.split()[1])
    try:
        for line in open(f"/proc/{pid}/smaps_rollup"):
            if line.startswith("Pss:"):
                m["Pss"] = int(line.split()[1])
    except OSError:
        m["Pss"] = None
    return m


def wait_port(port, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"server did not open port {port}")


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout.strip()


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


class Abort(Exception):
    pass


class Pilot:
    def __init__(self, a, plan):
        self.a, self.plan = a, plan
        self.alloc = plan["allocations"][a.allocation] if a.allocation != "none" else None
        self.port = 22000
        self.runs, self.results = [], {}
        os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
        self.env = self.environment()

    def environment(self):
        a = self.a
        # Build the two binaries from this tree first: with a clean tree the
        # binaries then match HEAD (ninja rebuilds anything stale).
        b = subprocess.run(["cmake", "--build", a.build, "--target", "snc_server", "net_bench"],
                           capture_output=True, text=True)
        if b.returncode:
            raise SystemExit(f"build failed:\n{b.stdout}\n{b.stderr}")
        build_type = sh(f"grep '^CMAKE_BUILD_TYPE:' {a.build}/CMakeCache.txt").split("=")[-1]
        flags = sh(f"grep '^CMAKE_CXX_FLAGS:' {a.build}/CMakeCache.txt").split("=", 1)[-1]
        govs = sorted(set(sh("cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor").split()))
        env = dict(git_head=sh("git rev-parse HEAD"), git_status=sh("git status --short"),
                   plan=a.plan, plan_sha256=sha(a.plan), plan_values=self.plan,
                   manifest=a.manifest, manifest_sha256=sha(a.manifest) if a.manifest else None,
                   snc_server_sha256=sha(f"{a.build}/snc_server"), net_bench_sha256=sha(f"{a.build}/net_bench"),
                   governors=govs, numa_balancing=sh("cat /proc/sys/kernel/numa_balancing"),
                   build_type=build_type, cxx_flags=flags, kernel=sh("uname -r"), started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                   allocation=a.allocation, allocation_values=self.alloc, quick=a.quick,
                   evidence=False if a.quick else None)
        if not a.quick:
            problems = []
            if env["git_status"]:
                problems.append("the git tree is not clean")
            want = self.plan.get("approved_manifest_sha256")
            if not a.manifest:
                problems.append("no --manifest")
            elif not want:
                problems.append("the plan has no approved_manifest_sha256 yet")
            elif env["manifest_sha256"] != want:
                problems.append(f"{a.manifest} is not the approved manifest")
            if build_type not in ("", "RelWithDebInfo") or flags:
                problems.append(f"{a.build} is not a plain RelWithDebInfo build ({build_type!r}, {flags!r})")
            if govs != ["performance"]:
                problems.append(f"CPU governors are {govs}, not all performance")
            if env["numa_balancing"] != "0":
                problems.append("kernel.numa_balancing is not 0")
            if not sh(f"git ls-files {a.plan}"):
                problems.append(f"{a.plan} is not committed")
            if problems:
                raise SystemExit("refusing a real pilot: " + "; ".join(problems))
        return env

    def objects(self):
        return 20_000 if self.a.quick else self.plan["objects"]

    def shards(self):
        return 2 if self.a.quick else self.plan["shards"]

    def threads(self):
        return 2 if self.a.quick else self.plan["client_threads"]

    def bind(self, node, cpus):
        if not self.alloc:
            return []
        return ["numactl", f"--physcpubind={','.join(map(str, cpus))}", f"--membind={node}"]

    def one(self, profile, phase, label, shards=None, rate=None, depth=None, window=None, memory=False):
        """One run: new server and data dir, load, optional memory settling,
        then one measured net_bench run. Returns the parsed result."""
        a, plan = self.a, self.plan
        shards = shards or self.shards()
        self.port += 40
        name = f"{profile}-{phase}-{label}"
        data = os.path.join(a.data_root, name)
        ids = os.path.join(a.data_root, name + ".ids")  # never under --out
        srv_cpus = self.alloc["server_cpus"][:shards] if self.alloc else []
        cli_cpus = self.alloc["client_cpus"][:self.threads()] if self.alloc else []
        if self.alloc and len(srv_cpus) < shards:
            raise Abort(f"{a.allocation} has {len(srv_cpus)} server CPUs, not {shards}")
        srv = [f"{a.build}/snc_server", "--shards", str(shards), "--port", str(self.port),
               "--objects", str(self.objects() * 2)]
        if srv_cpus:
            srv += ["--cpus", ",".join(map(str, srv_cpus))]
        if profile == "A":
            srv += ["--data-dir", data, "--checkpoint-s", str(plan["checkpoint_s"])]
        shutil.rmtree(data, ignore_errors=True)
        profile_check.Guard(profile).server(srv, dir_exists=os.path.exists)
        client = self.bind(self.alloc["client_node"], cli_cpus) if self.alloc else []
        client += [f"{a.build}/net_bench", "--port", str(self.port), "--threads", str(self.threads()),
                   "--objects", str(self.objects()), "--ids", ids, "--mix", plan["mix"], "--route", plan["route"]]
        if cli_cpus:
            client += ["--cpus", ",".join(map(str, cli_cpus))]
        if rate:
            measure = client + ["--rate", str(rate), "--depth", str(depth), "--warmup", str(window[0]),
                                "--seconds", str(window[1]), "--drain-s", str(plan["drain_s"])]
            limit = window[0] + window[1] + plan["drain_s"] + 600
        else:
            measure = client + ["--depth", str(depth), "--seconds", str(window)]
            limit = window + 600
        os.makedirs(a.data_root, exist_ok=True)
        log = open(os.path.join(a.out, "raw", name + ".server.log"), "w")
        p = subprocess.Popen(self.bind(self.alloc["server_node"], srv_cpus) + srv if self.alloc else srv,
                             stdout=log, stderr=subprocess.STDOUT)
        r = dict(profile=profile, phase=phase, label=label, shards=shards, rate=rate, depth=depth,
                 server_cpus=srv_cpus, client_cpus=cli_cpus)
        load = run = None
        try:
            wait_port(self.port)
            mem = {}
            if memory:
                time.sleep(plan["memory_empty_settle_s"] if not a.quick else 0.5)
                mem["empty"] = memory_kb(p.pid)
            load = subprocess.run(client + ["--depth", "64", "--seconds", "0"], capture_output=True, text=True,
                                  timeout=1800)
            if load.returncode:
                raise Abort(f"{name}: load failed\n{load.stdout}\n{load.stderr}")
            if memory:
                time.sleep(plan["memory_steady_settle_s"] if not a.quick else 0.5)
                mem["steady"] = memory_kb(p.pid)
            before = sample(p.pid)
            run = subprocess.run(measure, capture_output=True, text=True, timeout=limit)
            after = sample(p.pid)
            mem["after_run"] = memory_kb(p.pid)
            if p.poll() is not None:
                raise Abort(f"{name}: the server exited during the run ({p.returncode})")
        finally:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            log.close()
            with open(os.path.join(a.out, "raw", name + ".client.log"), "w") as f:
                f.write(" ".join(srv) + "\n" + " ".join(measure) + "\n--- load\n"
                        + (load.stdout + load.stderr if load else "") + "--- run\n"
                        + (run.stdout + run.stderr if run else ""))
            shutil.rmtree(data, ignore_errors=True)
            if os.path.exists(ids):
                os.remove(ids)
        r["returncode"] = run.returncode
        if rate:
            r["window_s"] = window[1]
        try:
            r.update(parse_client(run.stdout))
        except (TypeError, AttributeError):
            r.update(failed=True, why=f"unparsable net_bench output (exit {run.returncode})")
        if not r.get("failed"):
            # The samples span the whole net_bench run (warm-up, window and
            # drain), so divide by every op served in it, not the measured ones.
            r.update(per_op(before, after, r["served_all"]))
            if memory:
                n = self.objects()
                r.update(memory_kb=mem,
                         rss_bytes_per_object=(mem["steady"]["VmRSS"] - mem["empty"]["VmRSS"]) * 1024 / n,
                         pss_bytes_per_object=((mem["steady"]["Pss"] - mem["empty"]["Pss"]) * 1024 / n
                                               if mem["steady"]["Pss"] is not None else None),
                         peak_rss_kb=mem["after_run"]["VmHWM"])
        self.runs.append(r)
        p99 = r.get("p99_ns", 0) / 1000
        print(f"{name}: total {r.get('total_ops_s')} ops/s achieved {r.get('achieved')} p99 {p99:.0f} us "
              f"drained {r.get('drained')} errors {r.get('errors')}", flush=True)
        self.write()
        return r

    def write(self):
        tmp = os.path.join(self.a.out, "pilot.json.tmp")
        with open(tmp, "w") as f:
            json.dump(dict(environment=self.env, results=self.results, runs=self.runs), f, indent=1)
        os.replace(tmp, os.path.join(self.a.out, "pilot.json"))

    def shuffled(self, cells):
        cells = list(cells)
        random.Random(self.plan["order_seed"]).shuffle(cells)
        return cells

    def profile(self, prof):
        a, plan = self.a, self.plan
        pp = plan["profiles"][prof]
        q = a.quick
        win = 2 if q else pp["window_s"]
        warm = 1 if q else plan["warmup_s"]
        reps = 3 if q else plan["reps"]
        res = self.results.setdefault(prof, {})
        # 1. Calibration: places the grid, nothing else.
        cal = self.one(prof, "calibration", "c", depth=plan["calibration_depth"], window=2 if q else pp["calibration_s"])
        if cal.get("failed") or cal["errors"]:
            raise Abort(f"{prof}: calibration failed")
        p0 = cal["total_ops_s"]
        rates = grid(p0, Fraction(plan["grid_step"]), 6 if q else plan["grid_points"])
        res["calibration"] = dict(p0=p0, grid=rates)
        # 2. Duration check at the largest grid rate <= duration_check_rate_fraction x P0.
        dur_rate = max(r for r in rates if r <= Fraction(plan["duration_check_rate_fraction"]) * p0)
        res["duration"] = [dict(window_s=r["window_s"], p99_ns=r.get("p99_ns"), achieved=r.get("achieved"),
                                sustained=sustained(r, plan)[0])
                           for r in (self.one(prof, "duration", f"{s}s", rate=dur_rate, depth=plan["rate_depth"],
                                              window=(warm, s))
                                     for s in self.shuffled([2] if q else pp["duration_check_s"]))]
        # 3. The open-loop sweep, then the exact rule.
        points = {}
        for rate, i in self.shuffled([(r, i) for r in rates for i in range(reps)]):
            points.setdefault(rate, []).append(
                self.one(prof, "sweep", f"{rate}-r{i}", rate=rate, depth=plan["rate_depth"], window=(warm, win)))
        res["sweep"] = sweep_result(points, dict(plan, reps=reps))
        self.write()
        obj = res["sweep"]["objective_rate"]
        if res["sweep"]["invalid"]:
            raise Abort(f"{prof}: sweep INVALID: {res['sweep']['invalid']}; stop and review")
        if obj is None:
            raise Abort(f"{prof}: no stable peak or objective rate; stop and review")
        # 4. Fixed reps at the objective rate (CV of p99), with memory.
        fixed = [self.one(prof, "fixed", f"r{i}", rate=obj, depth=plan["rate_depth"], window=(warm, win), memory=True)
                 for i in range(2 if q else plan["fixed_reps"])]
        ok = [r for r in fixed if sustained(r, plan)[0]]
        res["fixed"] = dict(rate=obj, reps=len(fixed), sustained_reps=len(ok),
                            cv_p99=cv([r["p99_ns"] for r in ok]),
                            median_p99_ns=statistics.median(r["p99_ns"] for r in ok) if ok else None,
                            median_rss_bytes_per_object=statistics.median(r["rss_bytes_per_object"] for r in ok)
                            if ok else None,
                            median_pss_bytes_per_object=statistics.median(r["pss_bytes_per_object"] for r in ok)
                            if ok and all(r["pss_bytes_per_object"] is not None for r in ok) else None)
        # 5. Capacity reps (CV of throughput): closed loop, throughput only.
        cap = [self.one(prof, "capacity", f"r{i}", depth=plan["calibration_depth"], window=win)
               for i in range(2 if q else plan["capacity_reps"])]
        good = [r for r in cap if not r.get("failed") and not r["errors"]]
        res["capacity"] = dict(reps=len(cap), cv_throughput=cv([r["total_ops_s"] for r in good]),
                               median_total_ops_s=statistics.median(r["total_ops_s"] for r in good) if good else None)
        ct, cp = res["capacity"]["cv_throughput"], res["fixed"]["cv_p99"]
        res["noise_input_3x_max_cv"] = 3 * max(ct, cp) if ct is not None and cp is not None else None
        # 6. Shard sweep: closed-loop capacity, throughput only.
        counts = [1, 2] if q else plan["shard_counts"]
        sweep = {}
        for n, i in self.shuffled([(n, i) for n in counts for i in range(2 if q else plan["shard_reps"])]):
            sweep.setdefault(n, []).append(self.one(prof, "shards", f"s{n}-r{i}", shards=n,
                                                    depth=plan["calibration_depth"], window=win))
        res["shards"] = [dict(shards=n, median_total_ops_s=statistics.median(r["total_ops_s"] for r in v),
                              ops_s_per_core=statistics.median(r["total_ops_s"] for r in v) / n,
                              errors=sum(r["errors"] for r in v))
                         for n, v in sorted(sweep.items()) if all(not r.get("failed") for r in v)]
        self.write()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--build", default="build")
    ap.add_argument("--data-root", default="/srv/snc-data/pilot")
    ap.add_argument("--plan", default="bench/pilot_plan.json")
    ap.add_argument("--manifest", default="")
    ap.add_argument("--allocation", default=None, help="PILOT-L (default: the plan's baseline), PILOT-X, or none")
    ap.add_argument("--profiles", default="A,C")
    ap.add_argument("--quick", action="store_true", help="tiny harness check on any host; never evidence")
    a = ap.parse_args()
    plan = json.load(open(a.plan))
    a.allocation = a.allocation or plan["baseline_allocation"]
    if a.allocation == "none" and not a.quick:
        raise SystemExit("--allocation none is for --quick only")
    p = Pilot(a, plan)
    try:
        for prof in a.profiles.split(","):
            p.profile(prof)
    except Abort as e:
        p.results["aborted"] = str(e)
        p.write()
        print(f"ABORTED: {e}", file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(a.data_root, ignore_errors=True)
    print(json.dumps(p.results, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
