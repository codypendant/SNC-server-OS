#!/usr/bin/env python3
"""SNC-only within-system pilot (docs/CAMPAIGN_PREP.md CP-1A, G1, G3, G7).

Runs workload A on one host, per bench/results/<campaign>/PILOT_PLAN.md: the
duration check, the repeated fixed run, the depth sweep and the shard sweep,
in Profiles A and C. Keeps every run's raw output and rewrites pilot.json
after each run, so a stopped pilot keeps what it measured.

  bench/pilot.py --out bench/results/pilot-r640-20261106 [--build build]
                 [--data-root /srv/snc-data/pilot] [--manifest FILE] [--quick]
                 [--profiles A,C] [--phases duration,fixed,depth,shards]
"""
import argparse, hashlib, json, os, random, re, shutil, signal, socket, statistics, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_check

OBJECTS = 5_000_000
CLIENT_CPUS = [1, 3, 5, 7, 9, 11, 13, 15]
SERVER_CPUS = list(range(2, 36, 2))  # node 0 physical cores, not CPU 0
MIX = ["--index", "5", "--transfer", "10", "--reads", "70"]
DEPTHS = [1, 4, 16, 64, 128, 256, 512]
SHARDS = [1, 2, 4, 8, 16]
SECONDS = {"A": {"fixed": 120, "sweep": 90}, "C": {"fixed": 60, "sweep": 30}}
CLK = os.sysconf("SC_CLK_TCK")


# --- arithmetic (tests/test_pilot.py) ---

def cv(values):
    """Sample standard deviation / mean; None with under two values or a zero mean."""
    if len(values) < 2 or statistics.mean(values) == 0:
        return None
    return statistics.stdev(values) / statistics.mean(values)


def knee(depths, fraction=0.70, stable_cv=0.10):
    """The exact rule of PILOT_PLAN.md. depths: {depth: [{total_ops_s, p99_ns, errors}, ...]}."""
    points = []
    for d in sorted(depths):
        reps = depths[d]
        c = cv([r["total_ops_s"] for r in reps])
        stable = all(r["errors"] == 0 for r in reps) and (c is None or c <= stable_cv)
        points.append(dict(depth=d, cv=c, stable=stable,
                           t=statistics.median(r["total_ops_s"] for r in reps),
                           l=statistics.median(r["p99_ns"] for r in reps)))
    ok = [p for p in points if p["stable"]]
    if not ok:
        return dict(points=points, peak=None, knee_depth=None, objective_depth=None, objective_p99_ns=None)
    peak = max(p["t"] for p in ok)
    at_knee = next(p for p in ok if p["t"] >= 0.90 * peak)
    at_obj = next(p for p in ok if p["t"] >= fraction * peak)
    return dict(points=points, peak=peak, knee_depth=at_knee["depth"], fraction=fraction,
                objective_depth=at_obj["depth"], objective_p99_ns=at_obj["l"])


def per_op(before, after, ops):
    """Section 4 metrics over a window, from two samples of one server."""
    if ops <= 0:
        return dict(cpu_us_per_op=None, write_bytes_per_op=None, loopback_bytes_per_op=None)
    return dict(cpu_us_per_op=(after["cpu_s"] - before["cpu_s"]) * 1e6 / ops,
                write_bytes_per_op=(after["write_bytes"] - before["write_bytes"]) / ops,
                loopback_bytes_per_op=(after["lo_bytes"] - before["lo_bytes"]) / ops)


def parse_client(out):
    r = {}
    m = re.search(r"seconds=([\d.]+)", out); r["run_s"] = float(m[1])
    m = re.search(r"ops/s (\d+)\s+errors (\d+)\s+overloaded (\d+)\s+stale (\d+)", out)
    r.update(ops_s=int(m[1]), errors=int(m[2]), overloaded=int(m[3]), stale=int(m[4]))
    m = re.search(r"\n\s+latency ns\s+p50 (\d+)\s+p95 (\d+)\s+p99 (\d+)\s+p99.9 (\d+)", out)
    r.update(p50_ns=int(m[1]), p95_ns=int(m[2]), p99_ns=int(m[3]), p999_ns=int(m[4]))
    m = re.search(r"index ops/s (\d+)\s+ok (\d+)\s+empty (\d+)\s+refused (\d+)\s+errors (\d+)", out)
    r.update(index_ops_s=int(m[1]), index_ok=int(m[2]), index_empty=int(m[3]),
             index_refused=int(m[4]), index_errors=int(m[5]))
    m = re.search(r"index latency ns\s+p50 (\d+)\s+p95 (\d+)\s+p99 (\d+)\s+p99.9 (\d+)", out)
    r.update(index_p50_ns=int(m[1]), index_p95_ns=int(m[2]), index_p99_ns=int(m[3]), index_p999_ns=int(m[4]))
    r["total_ops_s"] = r["ops_s"] + r["index_ops_s"]
    r["errors"] += r["index_errors"]
    return r


# --- measurement ---

def sample(pid):
    f = open(f"/proc/{pid}/stat").read()
    fields = f[f.rindex(")") + 2:].split()
    cpu = (int(fields[11]) + int(fields[12])) / CLK
    io = dict(l.split(": ") for l in open(f"/proc/{pid}/io").read().splitlines())
    lo = sum(int(open(f"/sys/class/net/lo/statistics/{x}_bytes").read()) for x in ("rx", "tx"))
    return dict(cpu_s=cpu, write_bytes=int(io["write_bytes"]), lo_bytes=lo)


def status_kb(pid, key):
    for line in open(f"/proc/{pid}/status"):
        if line.startswith(key + ":"):
            return int(line.split()[1])
    return 0


def wait_port(port, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"server did not open port {port}")


class Pilot:
    def __init__(self, a):
        self.a = a
        self.port = 22000
        self.runs = []
        os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
        self.env = self.environment()

    def environment(self):
        def sh(c):
            return subprocess.run(c, shell=True, capture_output=True, text=True).stdout.strip()
        def sha(p):
            return hashlib.sha256(open(p, "rb").read()).hexdigest()
        a = self.a
        open(os.path.join(a.out, "uncommitted.diff"), "w").write(sh("git diff HEAD"))
        return dict(
            git_head=sh("git rev-parse HEAD"), git_status=sh("git status --short"),
            manifest=a.manifest, manifest_sha256=sha(a.manifest) if a.manifest else None,
            snc_server_sha256=sha(f"{a.build}/snc_server"), net_bench_sha256=sha(f"{a.build}/net_bench"),
            numa_balancing=sh("cat /proc/sys/kernel/numa_balancing"),
            governor=sh("cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor"),
            kernel=sh("uname -r"), started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            objects=self.objects(), mix=MIX, client_cpus=CLIENT_CPUS,
            memory="server --membind=0, client --membind=1", quick=a.quick)

    def objects(self):
        return 200_000 if self.a.quick else OBJECTS

    def one(self, profile, phase, label, shards, depth, seconds):
        a = self.a
        self.port += 40
        name = f"{profile}-{phase}-{label}"
        data = os.path.join(a.data_root, name)
        ids = os.path.join(a.out, "raw", name + ".ids")
        cpus = ",".join(map(str, SERVER_CPUS[:shards]))
        srv = [f"{a.build}/snc_server", "--shards", str(shards), "--cpus", cpus, "--port", str(self.port),
               "--objects", str(self.objects())]
        if profile == "A":
            srv += ["--data-dir", data, "--checkpoint-s", "60"]
        shutil.rmtree(data, ignore_errors=True)
        profile_check.Guard(profile).server(srv, dir_exists=os.path.exists)
        client = ["numactl", "--cpunodebind=1", "--membind=1", f"{a.build}/net_bench", "--port", str(self.port),
                  "--threads", str(len(CLIENT_CPUS)), "--cpus", ",".join(map(str, CLIENT_CPUS)),
                  "--objects", str(self.objects()), "--ids", ids, "--depth", str(depth)] + MIX
        log = open(os.path.join(a.out, "raw", name + ".server.log"), "w")
        p = subprocess.Popen(["numactl", "--cpunodebind=0", "--membind=0"] + srv, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_port(self.port)
            empty_kb = status_kb(p.pid, "VmRSS")
            load = subprocess.run(client + ["--seconds", "0"], capture_output=True, text=True, timeout=1800)
            if load.returncode:
                raise RuntimeError(f"{name}: load failed\n{load.stdout}\n{load.stderr}")
            steady_kb = status_kb(p.pid, "VmRSS")
            before = sample(p.pid)
            run = subprocess.run(client + ["--seconds", str(seconds)], capture_output=True, text=True,
                                 timeout=seconds + 600)
            after = sample(p.pid)
            hwm_kb = status_kb(p.pid, "VmHWM")
        finally:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            log.close()
        with open(os.path.join(a.out, "raw", name + ".client.log"), "w") as f:
            f.write(" ".join(srv) + "\n" + " ".join(client) + "\n--- load\n" + load.stdout + "--- run\n"
                    + run.stdout + run.stderr)
        r = dict(profile=profile, phase=phase, label=label, shards=shards, depth=depth, seconds=seconds,
                 returncode=run.returncode, server_cpus=cpus)
        if run.returncode or "ops/s" not in run.stdout:
            r.update(failed=True, errors=1, total_ops_s=0, p99_ns=0)
        else:
            r.update(parse_client(run.stdout))
            n = r["total_ops_s"] * r["run_s"]
            r.update(per_op(before, after, n))
            r.update(load_cpu_s=before["cpu_s"], empty_rss_kb=empty_kb, steady_rss_kb=steady_kb, peak_rss_kb=hwm_kb,
                     rss_bytes_per_object=(steady_kb - empty_kb) * 1024 / self.objects(),
                     data_dir_bytes=dir_bytes(data) if profile == "A" else 0)
        shutil.rmtree(data, ignore_errors=True)
        os.path.exists(ids) and os.remove(ids)
        self.runs.append(r)
        print(f"{name}: {r['total_ops_s']} ops/s p99 {r['p99_ns'] / 1000:.0f} us errors {r['errors']}", flush=True)
        self.write()
        return r

    def write(self):
        with open(os.path.join(self.a.out, "pilot.json.tmp"), "w") as f:
            json.dump(dict(environment=self.env, summary=summarize(self.runs), runs=self.runs), f, indent=1)
        os.replace(os.path.join(self.a.out, "pilot.json.tmp"), os.path.join(self.a.out, "pilot.json"))

    def phase(self, profile, phase, cells):
        cells = list(cells)
        random.Random(20261106).shuffle(cells)
        for c in cells:
            self.one(profile, phase, *c)


def dir_bytes(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def summarize(runs):
    out = {}
    for prof in sorted({r["profile"] for r in runs}):
        here = [r for r in runs if r["profile"] == prof]
        s = {}
        fixed = [r for r in here if r["phase"] == "fixed" and not r.get("failed")]
        if fixed:
            ct, cp = cv([r["total_ops_s"] for r in fixed]), cv([r["p99_ns"] for r in fixed])
            s["fixed"] = dict(reps=len(fixed), cv_throughput=ct, cv_p99=cp,
                              noise_input_3x_max_cv=3 * max(ct, cp) if ct is not None and cp is not None else None,
                              median_total_ops_s=statistics.median(r["total_ops_s"] for r in fixed),
                              median_p50_ns=statistics.median(r["p50_ns"] for r in fixed),
                              median_p99_ns=statistics.median(r["p99_ns"] for r in fixed),
                              median_p999_ns=statistics.median(r["p999_ns"] for r in fixed),
                              median_index_p99_ns=statistics.median(r["index_p99_ns"] for r in fixed),
                              median_cpu_us_per_op=statistics.median(r["cpu_us_per_op"] for r in fixed),
                              median_write_bytes_per_op=statistics.median(r["write_bytes_per_op"] for r in fixed),
                              median_rss_bytes_per_object=statistics.median(r["rss_bytes_per_object"] for r in fixed))
        depth = {}
        for r in here:
            if r["phase"] == "depth":
                depth.setdefault(r["depth"], []).append(r)
        if depth:
            s["depth_sweep"] = knee(depth)
        shards = {}
        for r in here:
            if r["phase"] == "shards":
                shards.setdefault(r["shards"], []).append(r)
        if shards:
            s["shard_sweep"] = [dict(shards=n, median_total_ops_s=statistics.median(r["total_ops_s"] for r in v),
                                     median_p99_ns=statistics.median(r["p99_ns"] for r in v),
                                     ops_s_per_core=statistics.median(r["total_ops_s"] for r in v) / n)
                                for n, v in sorted(shards.items())]
        s["duration"] = [dict(seconds=r["seconds"], total_ops_s=r["total_ops_s"], p99_ns=r["p99_ns"])
                         for r in sorted((x for x in here if x["phase"] == "duration"), key=lambda x: x["seconds"])]
        out[prof] = s
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--build", default="build")
    ap.add_argument("--data-root", default="/srv/snc-data/pilot")
    ap.add_argument("--manifest", default="")
    ap.add_argument("--profiles", default="A,C")
    ap.add_argument("--phases", default="duration,fixed,depth,shards")
    ap.add_argument("--quick", action="store_true", help="tiny harness check; not evidence")
    a = ap.parse_args()
    p = Pilot(a)
    phases = a.phases.split(",")
    for prof in a.profiles.split(","):
        fixed, sweep = SECONDS[prof]["fixed"], SECONDS[prof]["sweep"]
        reps, sw_reps = 5, 3
        durations, depths, shards = [30, 60, 120, 240], DEPTHS, SHARDS
        if a.quick:
            fixed, sweep, reps, sw_reps, durations, depths, shards = 2, 2, 2, 2, [2], [4, 64], [1, 2]
        if "duration" in phases:
            p.phase(prof, "duration", [(f"{s}s", 8, 64, s) for s in durations])
        if "fixed" in phases:
            p.phase(prof, "fixed", [(f"r{i}", 8, 64, fixed) for i in range(reps)])
        if "depth" in phases:
            p.phase(prof, "depth", [(f"d{d}-r{i}", 8, d, sweep) for d in depths for i in range(sw_reps)])
        if "shards" in phases:
            p.phase(prof, "shards", [(f"s{s}-r{i}", s, 64, sweep) for s in shards for i in range(sw_reps)])
    shutil.rmtree(a.data_root, ignore_errors=True)
    print(json.dumps(summarize(p.runs), indent=1))


if __name__ == "__main__":
    main()
