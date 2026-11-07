#!/usr/bin/env python3
"""Corrected B/T non-perturbation proof, then the approved causal cells only
if every gate passes. The rules are in PREREG.md in this directory; the
reviews are docs/handoff/PR62_Codex_Review.txt and
docs/handoff/PR64_Reconciliation_Review.txt.

What changed from ../diag2-r640-20261106-telemetry/diag2.py:
- benchmark scratch isolation (bench/pilot.py, PR #64): both arms put the
  ids files under the same --scratch, which must share no physical disk
  with the data directory or with --out; unknown mappings refuse;
- G0 drops its zero-refusal clause and checks machine identity;
- G2 is a refusal-fraction tolerance against B, in exact arithmetic;
- no ctest inside the driver (software validation belongs to the frozen
  commit), no reruns, and any failure stops.

  diag3.py OUT --scratch DIR            the proof, then causal cells if G0-G4 pass
  diag3.py OUT --scratch DIR --dry-run  preflight and the commands, no load
"""
import argparse, gzip, hashlib, json, os, re, shutil, socket, statistics, subprocess, sys, threading, time
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "bench"))
import pilot  # noqa: E402  (the scratch-isolation resolver, PR #64)

PLAN = json.load(open(os.path.join(REPO, "bench", "pilot_plan.json")))
ALLOC = PLAN["allocations"]["PILOT-L"]
SRV = ",".join(map(str, ALLOC["server_cpus"]))
CLI = ",".join(map(str, ALLOC["client_cpus"]))
OBJ = PLAN["objects"]
MIX = PLAN["mix"]
DATA = "/srv/snc-data/diag3"
MANIFEST = os.path.join(REPO, "bench", "manifests", "r640-main-20261106d.txt")
BUILDS = {"B": "build-pilot", "T": "build-diag"}
DATA_DISK_MODEL = "ARFX3840S5xnFTRI"

# References fixed before any data (PREREG.md).
REF_CLOSED = 4366466  # the pilot's P0
REF_P99 = 1048576  # the pilot's 2,183,233 w30 total p99 (1.05 ms; histogram bucket edge)
B_SHA256 = {"snc_server": "04030ccb310bb99beddf9ef166f37f6f430ab6daa3aa2d758887112f5bad3226",
            "net_bench": "a5f085d4dcfa4cb2a272eb95987e4626888c1a44307a67e4b56cf3b61d4f3863"}
REPS = 3
PROOF = [("closed", None, 60, 60), ("w30", 2183233, 30, 60)]  # kind, rate, window s, checkpoint s
CAUSAL = [("r436646-ckpt", 436646, 60, 60), ("r436646-nockpt", 436646, 60, 0), ("r2183233-w60", 2183233, 60, 60)]


def bind(cpus):
    return ["numactl", f"--physcpubind={cpus}", "--membind=0"]


def commands(arm, kind, rep, rate, win, ckpt, port, scratch, raw):
    """Every command of one cell. B and T differ only in the build directory
    and T's telemetry flags; the ids file is under the same scratch."""
    name = f"{arm}-{kind}-r{rep}"
    build = BUILDS[arm]
    data, ids = f"{DATA}/{name}", os.path.join(scratch, name + ".ids")
    srv = [f"{build}/snc_server", "--shards", str(PLAN["shards"]), "--port", str(port), "--objects", str(OBJ * 2),
           "--cpus", SRV, "--data-dir", data]
    if ckpt:
        srv += ["--checkpoint-s", str(ckpt)]  # 0: the server never checkpoints
    if arm == "T":
        srv += ["--diag-gauges", os.path.join(raw, name + ".gauges.csv"), "--diag-cpu", "0"]
    cli = bind(CLI) + [f"{build}/net_bench", "--port", str(port), "--threads", str(PLAN["client_threads"]),
                       "--objects", str(OBJ), "--ids", ids, "--mix", MIX, "--route", PLAN["route"], "--cpus", CLI]
    if rate:
        run = cli + ["--rate", str(rate), "--depth", str(PLAN["rate_depth"]), "--warmup", str(PLAN["warmup_s"]),
                     "--seconds", str(win), "--drain-s", str(PLAN["drain_s"])]
    else:
        run = cli + ["--depth", str(PLAN["calibration_depth"]), "--seconds", str(win)]
    if arm == "T":
        run += ["--diag-buckets", os.path.join(raw, name + ".buckets.csv")]
    return dict(name=name, data=data, ids=ids, server=bind(SRV) + srv, load=cli + ["--depth", "64", "--seconds", "0"],
                run=run)


def parse(out):
    g = lambda p: re.search(p, out)
    t = g(r"total latency ns\s+n \d+\s+p50 \d+\s+p95 \d+\s+p99 (\d+)\s+p99.9 (\d+)\s+max (\d+)")
    r = dict(total=int(g(r"total ops/s (\d+)")[1]), over=int(g(r"overloaded (\d+)")[1]),
             idx=int(g(r"index ops/s \d+\s+ok \d+\s+empty \d+\s+refused (\d+)")[1]),
             p99=int(t[1]), p999=int(t[2]), maxl=int(t[3]))
    s = g(r"scheduled_in_window (\d+)")
    if s:
        r["sched"] = int(s[1])
        r["late_over_1ms"] = int(g(r"late_over_1ms (\d+)")[1])
        r["achieved"] = int(g(r"achieved (\d+) ops/s")[1])
    return r


def refusal_fraction(x):
    """(EXEC + INDEX refusals) / scheduled_in_window. The refusal counts are
    net_bench's whole-run counts (warm-up, window, drain): the stricter reading."""
    return Fraction(x["over"] + x["idx"], x["sched"])


def gates(res, identity_ok):
    """G0-G4 (PREREG.md). Exact arithmetic; a missing rep fails every gate."""
    def sel(a, k):
        return [x for x in res if x["arm"] == a and x["kind"] == k]
    if any(len(sel(a, k)) != REPS for a in "BT" for k in ("closed", "w30")):
        return dict(G0=False, G1=False, G2=False, G3=False, G4=False), {"incomplete": True}

    def med(a, k, f):
        return statistics.median(Fraction(x[f]) for x in sel(a, k))
    bw, tw = sel("B", "w30"), sel("T", "w30")
    bf, tf = [refusal_fraction(x) for x in bw], [refusal_fraction(x) for x in tw]
    g0a = abs(med("B", "closed", "total") / REF_CLOSED - 1) <= Fraction(5, 100)
    g0b = med("B", "w30", "p99") <= Fraction(3, 2) * REF_P99
    g2a = statistics.median(tf) <= statistics.median(bf) + Fraction(1, 10000)
    g2b = all(f <= max(bf) + Fraction(2, 10000) for f in tf)
    G = dict(
        G0=bool(g0a and g0b and identity_ok),
        G1=med("T", "closed", "total") >= Fraction(97, 100) * med("B", "closed", "total"),
        G2=bool(g2a and g2b),
        G3=all(med("T", "w30", f) <= max(Fraction(5, 4) * med("B", "w30", f), max(x[f] for x in bw))
               for f in ("p99", "p999")),
        G4=med("T", "w30", "total") >= Fraction(995, 1000) * med("B", "w30", "total"))
    detail = dict(G0_throughput=bool(g0a), G0_p99=bool(g0b), G0_identity=bool(identity_ok),
                  G2_median=bool(g2a), G2_per_rep=bool(g2b),
                  B_refusal_fraction=[str(f) for f in bf], T_refusal_fraction=[str(f) for f in tf],
                  B_closed_median=str(med("B", "closed", "total")), B_w30_p99_median=str(med("B", "w30", "p99")))
    return {k: bool(v) for k, v in G.items()}, detail


def manifest_values(text):
    v = {}
    m = re.search(r"### uname -a\n\S+ \S+ (\S+)", text)
    v["kernel"] = m[1] if m else None
    m = re.search(r"^CPU\(s\):\s+(\d+)", text, re.M)
    v["cpus"] = int(m[1]) if m else None
    m = re.search(r"^Model name:\s+(.+)$", text, re.M)
    v["model"] = m[1].strip() if m else None
    m = re.search(r"^NUMA node0 CPU\(s\):\s+(.+)$", text, re.M)
    v["node0"] = m[1].strip() if m else None
    m = re.search(r"^MemTotal:\s+(\d+) kB", text, re.M)
    v["mem_kb"] = int(m[1]) if m else None
    return v


def cpulist(s):
    out = []
    for part in s.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    return out


def sha(p):
    try:
        return hashlib.sha256(open(p, "rb").read()).hexdigest()
    except OSError:
        return None


def rd(p):
    try:
        return open(p).read()
    except OSError:
        return ""


def identity(data_disks):
    """G0 part C: the machine, settings, allocation and B build are the
    pilot's. Each check is recorded; all must hold."""
    want = manifest_values(open(MANIFEST).read())
    m = re.search(r"^model name\s*:\s*(.+)$", rd("/proc/cpuinfo"), re.M)
    model = m[1].strip() if m else None
    mem = int(re.search(r"^MemTotal:\s+(\d+) kB", rd("/proc/meminfo"), re.M)[1])
    n = os.cpu_count()
    govs = {rd(f"/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_governor").strip() for i in range(n)}
    c = dict(
        kernel=os.uname().release == want["kernel"],
        cpus=n == want["cpus"],
        model=model == want["model"],
        node0=rd("/sys/devices/system/node/node0/cpulist").strip() != ""
        and cpulist(rd("/sys/devices/system/node/node0/cpulist").strip()) == cpulist(want["node0"]),
        memory=abs(mem - want["mem_kb"]) <= want["mem_kb"] // 1000,
        governors=govs == {"performance"},
        numa_balancing=rd("/proc/sys/kernel/numa_balancing").strip() == "0",
        allocation=not set(ALLOC["housekeeping_cpus"]) & set(cpulist(SRV) + cpulist(CLI)),
        b_build=all(sha(f"{BUILDS['B']}/{b}") == h for b, h in B_SHA256.items()))
    disk = sorted(data_disks)[0] if data_disks and len(data_disks) == 1 else None
    if disk:
        q = f"/sys/block/{disk}"
        c["data_disk_model"] = rd(f"{q}/device/model").strip() == DATA_DISK_MODEL
        c["data_disk_scheduler"] = "[mq-deadline]" in rd(f"{q}/queue/scheduler")
        c["data_disk_write_cache"] = rd(f"{q}/queue/write_cache").strip() == "write back"
    else:
        c["data_disk_model"] = False
    values = dict(kernel=os.uname().release, model=model, mem_kb=mem, governors=sorted(govs), data_disk=disk,
                  t_build={b: sha(f"{BUILDS['T']}/{b}") for b in ("snc_server", "net_bench")})
    return all(c.values()), c, values


def isolation(scratch, out, disks_of=pilot.disks_of):
    """Scratch isolation for BOTH arms: one --scratch, on no physical disk of
    the data directory; --out likewise; the data directory on exactly one
    disk (the observer reads that disk). Unknown mappings refuse."""
    disks = {role: disks_of(p) for role, p in (("data", DATA), ("scratch", scratch), ("out", out))}
    problems = pilot.isolation_problems(disks["data"], {k: disks[k] for k in ("scratch", "out")})
    if disks["data"] is not None and len(disks["data"]) != 1:
        problems.append(f"the data directory is on {sorted(disks['data'])}, not exactly one disk")
    return problems, {k: sorted(v) if v is not None else None for k, v in disks.items()}


def wait_port(p):
    for _ in range(600):
        try:
            socket.create_connection(("127.0.0.1", p), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit(f"port {p} never opened")


def observer(dev, path, stop):
    os.sched_setaffinity(0, {ALLOC["housekeeping_cpus"][1]})
    rows = []
    while not stop.is_set():
        with open("/proc/diskstats") as f:
            for line in f:
                p = line.split()
                if p[2] == dev:
                    rows.append(f"{time.monotonic_ns()},{p[7]},{p[9]},{p[10]},{p[11]},{p[12]},{p[13]},{p[18]},{p[19]}")
                    break
        stop.wait(0.1)
    with gzip.open(path, "wt") as f:
        f.write("t_ns,w_ios,w_sect,w_ms,inflight,io_ms,wq_ms,flush,flush_ms\n" + "\n".join(rows) + "\n")


class Driver:
    def __init__(self, out, scratch, dev):
        self.out, self.scratch, self.dev = out, scratch, dev
        self.raw = os.path.join(out, "raw")
        self.port = 26000
        self.res = []

    def cell(self, arm, kind, rep, rate, win, ckpt):
        self.port += 40
        c = commands(arm, kind, rep, rate, win, ckpt, self.port, self.scratch, self.raw)
        b = os.path.join(self.raw, c["name"])
        shutil.rmtree(c["data"], ignore_errors=True)
        os.makedirs(DATA, exist_ok=True)
        slog = open(b + ".server.log", "w")
        P = subprocess.Popen(c["server"], stdout=slog, stderr=subprocess.STDOUT)
        stop, th, T = threading.Event(), None, {}
        try:
            wait_port(self.port)
            if arm == "T":
                th = threading.Thread(target=observer, args=(self.dev, b + ".disk.csv.gz", stop))
                th.start()
            T["server_up"] = time.monotonic_ns()
            load = subprocess.run(c["load"], capture_output=True, text=True, timeout=600)
            if load.returncode:
                raise SystemExit(f"STOP: {c['name']}: load failed {load.stdout}{load.stderr}")
            T["run_start"] = time.monotonic_ns()
            run = subprocess.run(c["run"], capture_output=True, text=True, timeout=win + 660)
            T["run_end"] = time.monotonic_ns()
            open(b + ".client.log", "w").write(" ".join(c["server"]) + "\n" + " ".join(c["run"]) + "\n--- load\n"
                                               + load.stdout + load.stderr + "--- run\n" + run.stdout + run.stderr)
            if run.returncode or P.poll() is not None:
                raise SystemExit(f"STOP: {c['name']}: exit {run.returncode}, server {P.poll()}")
        finally:
            stop.set()
            if th:
                th.join()
            P.terminate()
            try:
                P.wait(timeout=120)
            except subprocess.TimeoutExpired:
                P.kill()
                P.wait()
            slog.close()
            json.dump(T, open(b + ".times.json", "w"))
            shutil.rmtree(c["data"], ignore_errors=True)
            if os.path.exists(c["ids"]):
                os.remove(c["ids"])
        for f in (b + ".gauges.csv", b + ".buckets.csv"):
            if os.path.exists(f):
                with open(f, "rb") as i, gzip.open(f + ".gz", "wb") as o:
                    shutil.copyfileobj(i, o)
                os.remove(f)
        r = dict(arm=arm, kind=kind, rep=rep, rate=rate, window_s=win, checkpoint_s=ckpt, **parse(run.stdout))
        self.res.append(r)
        json.dump(self.res, open(os.path.join(self.out, "summary.json"), "w"), indent=1)
        print(json.dumps(r), flush=True)
        return r


def preflight(out, scratch, git_status):
    """Everything that must hold before the first run. Returns (ok, record).
    git_status is taken before the driver creates anything."""
    problems, disks = isolation(scratch, out)
    free = os.statvfs(scratch)
    if free.f_bavail * free.f_frsize < 2 * 16 * OBJ:
        problems.append(f"{scratch} has less than twice the ids file's size free")
    if git_status.strip():
        problems.append("the git tree is not clean")
    if subprocess.run(["pgrep", "-x", "snc_server|net_bench"], capture_output=True).returncode == 0:
        problems.append("snc_server or net_bench is already running")
    data_disks = set(disks["data"] or [])
    ok_id, checks, values = identity(data_disks)
    if not ok_id:
        problems.append("G0 identity: " + ", ".join(k for k, v in checks.items() if not v))
    rec = dict(git_head=subprocess.run(["git", "-C", REPO, "rev-parse", "HEAD"], capture_output=True,
                                       text=True).stdout.strip(),
               scratch=os.path.realpath(scratch), data=DATA, out=os.path.realpath(out), disks=disks,
               identity_checks=checks, identity_values=values, problems=problems,
               utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    return not problems, rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if not a.dry_run and os.path.exists(a.out):
        raise SystemExit(f"{a.out} exists: each run happens once")
    git_status = subprocess.run(["git", "-C", REPO, "status", "--porcelain"], capture_output=True, text=True).stdout
    os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
    os.makedirs(a.scratch, exist_ok=True)
    os.makedirs(DATA, exist_ok=True)
    ok, rec = preflight(a.out, a.scratch, git_status)
    if a.dry_run:
        for arm in "BT":
            c = commands(arm, "w30", 0, 2183233, 30, 60, 26040, a.scratch, os.path.join(a.out, "raw"))
            print(arm, json.dumps(c, indent=1))
        print(json.dumps(rec, indent=1))
        print("dry run: preflight", "ok" if ok else "REFUSED", flush=True)
        return 0
    json.dump(rec, open(os.path.join(a.out, "preflight.json"), "w"), indent=1)
    if not ok:
        raise SystemExit("REFUSED before any run: " + "; ".join(rec["problems"]))
    d = Driver(a.out, a.scratch, rec["identity_values"]["data_disk"])
    for rep in range(REPS):
        for arm in "BT":
            for kind, rate, win, ckpt in PROOF:
                d.cell(arm, kind, rep, rate, win, ckpt)
    ok_post, checks_post, values_post = identity(set(rec["disks"]["data"]))
    rec_post = dict(identity_checks=checks_post, identity_values=values_post)
    json.dump(rec_post, open(os.path.join(a.out, "postflight.json"), "w"), indent=1)
    t_same = values_post["t_build"] == rec["identity_values"]["t_build"]
    G, detail = gates(d.res, ok_post and t_same)
    detail["T_build_unchanged"] = t_same
    json.dump(dict(gates=G, detail=detail), open(os.path.join(a.out, "gates.json"), "w"), indent=1)
    print("GATES", G, flush=True)
    if not all(G.values()):
        print("STOP: a gate failed; no causal cell runs. Commit the evidence and return for review.", flush=True)
        return 1
    for rep in range(REPS):
        for kind, rate, win, ckpt in CAUSAL:
            d.cell("T", kind, rep, rate, win, ckpt)
    print("ALL DONE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
