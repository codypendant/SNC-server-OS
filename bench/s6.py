#!/usr/bin/env python3
"""S6 R640 checkpoint regression runner (docs/S6_PREREG.md).

Authorized by docs/handoff/PR99_S6_Pacing_CLI_Final_Review.md ("Post-merge
next authorized task"). It runs the preregistered cells once, in the
preregistered order, and judges them by the preregistered rules. Nothing
here is tuned after data.

  bench/s6.py OUT --scratch DIR [--dry-run]

A real run refuses unless all of these hold:
- bench/s6_frozen.json is frozen: it names the runner commit, the binary
  hashes and a filed GO record;
- HEAD differs from the runner commit only in docs (no code change after
  the freeze);
- the tree is clean;
- the binaries match the frozen hashes;
- the machine matches manifest e;
- scratch and output are on no disk of the data root.

--dry-run prints every command of every cell and the preflight record, and
starts nothing. It is allowed before the freeze.

Raw output kept per rep under OUT/raw/:
- server log and client log (load and run);
- times;
- the 100 ms GAUGES/HEALTH samples, verbatim, with their monotonic times;
- the disk observer's /proc/diskstats rows;
- final GAUGES and HEALTH.
OUT/summary.json holds every parsed rep; OUT/verdict.json the verdict.
"""
import argparse, gzip, hashlib, json, os, re, shutil, socket, statistics, subprocess, sys, threading, time
from fractions import Fraction

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "bench"))
import pilot  # noqa: E402  (disks_of, isolation_problems: PR #62/#64 scratch isolation)

PLAN = json.load(open(os.path.join(REPO, "bench", "pilot_plan.json")))
FROZEN_PATH = os.path.join(REPO, "bench", "s6_frozen.json")

# --- the preregistration (docs/S6_PREREG.md); changing any of these is a new preregistration ---

ALLOC = PLAN["allocations"]["PILOT-L"]
SRV_CPUS = ALLOC["server_cpus"]
CLI_CPUS = ALLOC["client_cpus"]
SAMPLER_CPU, OBSERVER_CPU = ALLOC["housekeeping_cpus"]   # 0 and 36 (PR #98 review section 6)
OBJ = PLAN["objects"]                                     # 5,000,000
MIX, ROUTE, THREADS, SHARDS = PLAN["mix"], PLAN["route"], PLAN["client_threads"], PLAN["shards"]
WARMUP_S, WINDOW_S, DRAIN_S = PLAN["warmup_s"], 60, PLAN["drain_s"]
OPEN_DEPTH, CLOSED_DEPTH = PLAN["rate_depth"], PLAN["calibration_depth"]   # 1024, 512
DATA_ROOT = "/srv/snc-data/s6"
DATA_DISK_MODEL = "ARFX3840S5xnFTRI"
DATA_FS_UUID = "3bf857c8-0cb4-4c57-b9a5-14328e70b061"
DATA_MOUNT_OPTS = "rw,relatime"   # plus stripe=2 in the superblock options (manifest e)
REPS = 3
SAMPLE_S = Fraction(1, 10)        # 100 ms controller sampling
CHECKPOINT_S = 60
PACING_FLAGS = ["--ckpt-pacing", "on", "--ckpt-walk-batch", "16", "--ckpt-walk-min-us", "20",
                "--ckpt-walk-max-us", "65", "--ckpt-walk-ceiling-us", "155", "--ckpt-walk-step-us", "5",
                "--ckpt-target-ms", "30000", "--ckpt-max-age-ms", "60000"]
MAX_AGE_MS = 60000
EMIT_GATE = 1024                  # kEmitBacklog
P99_FACTOR = Fraction(3, 2)
P999_LIMIT_NS = 10_000_000
LATE_FRACTION = Fraction(1, 1000)
CAPACITY_FRACTION = Fraction(95, 100)
WAL_HIGH_PCT = 200                # the pacer's WAL high mark (inherited hypothesis)

# Cells: name -> (server, rate or None for closed loop, checkpoint).
CELLS = {
    "N": ("current", 436646, False),
    "P": ("current", 436646, True),
    "H": ("current", 2183233, True),
    "C": ("current", None, True),
    "C0": ("pres1", None, True),
}
ORDER = ["N", "P", "H", "C", "C0"]   # rep-major: for each rep, this order


# --- parsing ---

def parse_client(out):
    """net_bench's run section. None for a field it did not print."""
    def num(p):
        m = re.search(p, out)
        return int(m[1]) if m else None
    t = re.search(r"total latency ns\s+n \d+\s+p50 \d+\s+p95 \d+\s+p99 (\d+)\s+p99\.9 (\d+)\s+max (\d+)", out)
    d = re.search(r"drained (yes|no)", out)
    return dict(
        total=num(r"total ops/s (\d+)"), errors=num(r"ops/s \d+\s+errors (\d+)"),
        overloaded=num(r"overloaded (\d+)"), stale=num(r"stale (\d+)"),
        index_refused=num(r"index ops/s \d+\s+ok \d+\s+empty \d+\s+refused (\d+)"),
        index_errors=num(r"index ops/s \d+\s+ok \d+\s+empty \d+\s+refused \d+\s+errors (\d+)"),
        p99=int(t[1]) if t else None, p999=int(t[2]) if t else None, max=int(t[3]) if t else None,
        scheduled_in_window=num(r"scheduled_in_window (\d+)"), late_over_1ms=num(r"late_over_1ms (\d+)"),
        completed_in_window=num(r"completed_in_window (\d+)"), end_backlog=num(r"end_backlog (\d+)"),
        drained=None if d is None else d[1] == "yes")


def parse_kv(line):
    """A GAUGES or HEALTH reply as {key: str}. Keys absent from the reply are
    absent from the dict: an old server's missing gauge is never a zero."""
    out = {}
    for tok in line.split():
        k, sep, v = tok.partition("=")
        if sep:
            out[k] = v
    return out


def shard_values(kv, name, shards=SHARDS):
    """[int or None] per shard for gauge `name` (s<i>.<name>)."""
    vals = []
    for s in range(shards):
        v = kv.get(f"s{s}.{name}")
        vals.append(int(v) if v is not None and v.isdigit() else None)
    return vals


CKPT_LINE = re.compile(r"^checkpoint: (\d+) objects in (\d+(?:\.\d+)?)s", re.M)
BAD_LOG = re.compile(r"fail-stop|terminate called|Aborted|AddressSanitizer|runtime error|CHECK failed|"
                     r"Segmentation fault", re.I)


def checkpoints_completed(server_log):
    """[(objects, seconds as Fraction)] from the server's completion lines."""
    return [(int(m[1]), Fraction(m[2])) for m in CKPT_LINE.finditer(server_log)]


# --- the rules (exact; docs/S6_PREREG.md section 6) ---

def median(xs):
    return statistics.median(Fraction(x) for x in xs)


def correctness_ok(rep, open_loop):
    """Rule 7: no error of any kind; drained (open loop); server alive at the
    end of the run and exited 0; no failure marker in its log."""
    c = rep["client"]
    why = []
    for k in ("errors", "stale", "index_errors"):
        if c.get(k) is None:
            why.append(f"{k} not reported")
        elif c[k] != 0:
            why.append(f"{k} {c[k]}")
    if open_loop and c.get("drained") is not True:
        why.append("not drained")
    if not rep.get("server_alive_at_end"):
        why.append("server not running at the end of the run")
    if rep.get("server_rc") != 0:
        why.append(f"server exit {rep.get('server_rc')}")
    if rep.get("log_markers"):
        why.append("failure marker in the server log: " + ", ".join(rep["log_markers"][:3]))
    return not why, why


def refusal_free(c):
    return c.get("overloaded") == 0 and c.get("index_refused") == 0


def p_rules(rep, ref_p99):
    """The seven rules for one P rep. Returns ({rule: bool}, reasons)."""
    c = rep["client"]
    r, why = {}, []
    r["1_no_refusal"] = refusal_free(c)
    s, late = c.get("scheduled_in_window"), c.get("late_over_1ms")
    r["2_lateness"] = s is not None and late is not None and late * 1000 <= s       # late <= 0.1% of s
    r["3_p99"] = c.get("p99") is not None and ref_p99 is not None and Fraction(c["p99"]) <= P99_FACTOR * ref_p99
    r["4_p999"] = c.get("p999") is not None and c["p999"] <= P999_LIMIT_NS
    peaks = rep.get("final_emits_peak") or []
    r["5_backlog"] = len(peaks) == SHARDS and all(p is not None and p < EMIT_GATE for p in peaks)
    done = rep.get("checkpoints") or []
    degraded = rep.get("final_degraded") or []
    r["6_checkpoint"] = (len(done) >= 1 and all(sec * 1000 < MAX_AGE_MS for _, sec in done)
                         and len(degraded) == SHARDS and all(d == 0 for d in degraded))
    r["7_correctness"], cwhy = correctness_ok(rep, open_loop=True)
    why = [k for k, v in r.items() if not v] + cwhy
    return r, why


def n_usable(reps):
    """The N reference is usable only if every N rep is refusal-free and correct."""
    return all(refusal_free(x["client"]) and correctness_ok(x, open_loop=True)[0] for x in reps)


def capacity_guard(c_reps, c0_reps):
    """median(C) >= 95% x median(C0), and every C and C0 rep correct (rule 7)."""
    if len(c_reps) != REPS or len(c0_reps) != REPS:
        return False, ["capacity cells incomplete"]
    bad = [f"{x['cell']} r{x['rep']}" for x in c_reps + c0_reps if not correctness_ok(x, open_loop=False)[0]]
    if any(x["client"].get("total") is None for x in c_reps + c0_reps):
        return False, ["capacity: total ops/s not reported"]
    mc, m0 = median(x["client"]["total"] for x in c_reps), median(x["client"]["total"] for x in c0_reps)
    ok = mc >= CAPACITY_FRACTION * m0 and not bad
    return ok, ([] if ok else [f"median C {mc} vs 95% of median C0 {m0}"] + [f"rule 7 failed: {b}" for b in bad])


def verdict(reps, void_reasons):
    """PASS / FAIL / VOID for a completed session (docs/S6_PREREG.md 6-7)."""
    out = dict(void=list(void_reasons))
    by = lambda cell: sorted((x for x in reps if x["cell"] == cell and not x.get("void")), key=lambda x: x["rep"])
    n = by("N")
    if len(n) == REPS and not n_usable(n):
        out["void"].append("N reference unusable (a refusal or a rule-7 failure in an N rep)")
    if out["void"]:
        out["verdict"] = "VOID"
        return out
    p = by("P")
    if len(n) != REPS or len(p) != REPS:
        out["verdict"] = "VOID"
        out["void"].append("cells incomplete")
        return out
    ref = median(x["client"]["p99"] for x in n)
    out["p99_reference_ns"] = str(ref)
    out["p_reps"] = []
    for x in p:
        rules, why = p_rules(x, ref)
        out["p_reps"].append(dict(rep=x["rep"], rules=rules, failed=why))
    out["capacity_ok"], out["capacity_why"] = capacity_guard(by("C"), by("C0"))
    ok = all(all(r["rules"].values()) for r in out["p_reps"]) and out["capacity_ok"]
    out["verdict"] = "PASS" if ok else "FAIL"
    return out


# --- VOID and rerun policy (section 7) ---

RERUNNABLE = ("window", "harness")   # the only VOID causes that allow a rerun of the rep


def run_session(execute, postflight_ok=lambda: (True, [])):
    """Runs the plan with `execute(cell, rep) -> dict` and applies the rerun
    policy. A rep dict may carry void = None | "window" | "harness".
    - window: no checkpoint began inside the measured window (P, H only).
      Rerun once, at the end. A second window-VOID rep, or a rerun that is
      VOID again, makes the session VOID.
    - harness: a start failure or a taken port. Rerun once, at the end; VOID
      again makes the session VOID.
    - Anything else, a failed rule included, is a result: never rerun.
    Returns (reps, void_reasons)."""
    reps, again, void, window_voids = [], [], [], 0
    for rep in range(REPS):
        for cell in ORDER:
            r = execute(cell, rep)
            reps.append(r)
            if r.get("void") == "window":
                window_voids += 1
                if window_voids >= 2:
                    void.append(f"second checkpoint-outside-window rep ({cell} r{rep})")
                    return reps, void
            if r.get("void") in RERUNNABLE:
                again.append((cell, rep))
            elif r.get("void"):
                void.append(f"{cell} r{rep}: {r['void']}")
                return reps, void
    for cell, rep in again:
        r = execute(cell, rep)
        r["rerun"] = True
        reps.append(r)
        if r.get("void"):
            void.append(f"{cell} r{rep} VOID again on its one rerun ({r['void']})")
            return reps, void
    ok, why = postflight_ok()
    if not ok:
        void.append("machine or build drift: " + "; ".join(why))
    return reps, void


# --- controller evidence from timestamped samples (section 6, reported) ---

def time_above(series, pred, max_gap_ns):
    """series: [(t_ns, value or None)] in time order (one shard's gauge).
    Each value holds until the next sample (sample-and-hold). An interval
    longer than max_gap_ns, or one that starts at a failed/absent sample
    (None), is a gap: not attributed either way. Returns
    (above_ns, covered_ns, gap_ns), all from timestamps."""
    above = covered = gap = 0
    for (t0, v), (t1, _) in zip(series, series[1:]):
        dt = t1 - t0
        if dt <= 0:
            continue
        if v is None or dt > max_gap_ns:
            gap += dt
            continue
        covered += dt
        if pred(v):
            above += dt
    return above, covered, gap


def controller_summary(samples, shards=SHARDS):
    """samples: [{"t_ns", "ok", "gauges": dict or None}] from the GAUGES
    sampler. A gauge the server does not publish is reported as absent
    (None), never as 0."""
    max_gap = int(2 * SAMPLE_S * 1_000_000_000)
    good = [s for s in samples if s["ok"] and s["gauges"] is not None]
    out = dict(samples=len(samples), failed_samples=len(samples) - len(good))
    ts = [s["t_ns"] for s in samples]
    out["max_interval_ms"] = max((b - a for a, b in zip(ts, ts[1:])), default=0) / 1e6

    def per_shard(name):
        if not good or not any(f"s0.{name}" in s["gauges"] for s in good):
            return None
        return [[(s["t_ns"], shard_values(s["gauges"], name, shards)[i] if s["ok"] and s["gauges"] else None)
                 for s in samples] for i in range(shards)]

    def peak(name):
        ser = per_shard(name)
        if ser is None:
            return None
        vals = [v for sh in ser for _, v in sh if v is not None]
        return max(vals) if vals else None

    def above(name, pred):
        ser = per_shard(name)
        if ser is None:
            return None
        res = [time_above(sh, pred, max_gap) for sh in ser]
        return dict(max_shard_ms=max(r[0] for r in res) / 1e6, sum_ms=sum(r[0] for r in res) / 1e6,
                    gap_ms_max=max(r[2] for r in res) / 1e6)

    out["wal_ewma_peak_us"] = peak("wal_flush_ewma_us")
    out["emits_now_peak"] = peak("emits_now")
    out["emits_peak"] = peak("emits_peak")
    out["backlog_above_25pct"] = above("emits_now", lambda v: v > EMIT_GATE // 4)
    out["backlog_above_50pct"] = above("emits_now", lambda v: v > EMIT_GATE // 2)
    out["catchup"] = above("ckpt_mode", lambda v: v == 1)
    budget = per_shard("ckpt_walk_budget_ns")
    out["walk_budget_ns_range"] = None if budget is None else (
        [min((v for sh in budget for _, v in sh if v), default=None),
         max((v for sh in budget for _, v in sh if v is not None), default=None)])
    # WAL above high: ewma > 200% of the shard's learned baseline (baseline 0 = unknown: not above).
    ew, base = per_shard("wal_flush_ewma_us"), per_shard("ckpt_wal_baseline_us")
    if ew is None or base is None:
        out["wal_above_high"] = None
    else:
        res = []
        for e_sh, b_sh in zip(ew, base):
            ser = [(t, None if e is None or b is None else (e, b)) for (t, e), (_, b) in zip(e_sh, b_sh)]
            res.append(time_above(ser, lambda eb: eb[1] > 0 and eb[0] * 100 > WAL_HIGH_PCT * eb[1], max_gap))
        out["wal_above_high"] = dict(max_shard_ms=max(r[0] for r in res) / 1e6, sum_ms=sum(r[0] for r in res) / 1e6,
                                     gap_ms_max=max(r[2] for r in res) / 1e6)
    return out


def checkpoint_begin(samples):
    """(lo_ns, hi_ns) bounding the first checkpoint's begin, from the node's
    ckpt_seq in GAUGES: it advanced between the last sample with the old seq
    (lo) and the first with a higher one (hi). None if it never advanced."""
    prev_t, prev_seq = None, None
    for s in samples:
        if not s["ok"] or s["gauges"] is None or "ckpt_seq" not in s["gauges"]:
            continue
        seq = int(s["gauges"]["ckpt_seq"])
        if prev_seq is not None and seq > prev_seq:
            return prev_t, s["t_ns"]
        if s["gauges"].get("ckpt", "idle") != "idle" and prev_seq is None:
            return None, s["t_ns"]   # already running at the first sample: begin unknown, not inside
        prev_t, prev_seq = s["t_ns"], seq
    return None


def began_in_window(bounds, win_lo, win_hi):
    """True only if the begin is bounded, and both bounds are inside the window."""
    return bounds is not None and bounds[0] is not None and win_lo <= bounds[0] and bounds[1] <= win_hi


# --- commands ---

def bind(cpus):
    return ["numactl", f"--physcpubind={','.join(map(str, cpus))}", "--membind=0"]


def commands(cell, rep, port, admin, scratch, frozen):
    kind, rate, ckpt = CELLS[cell]
    name = f"{cell}-r{rep}"
    data, ids = f"{DATA_ROOT}/{name}", os.path.join(scratch, name + ".ids")
    server = os.path.join(REPO, frozen["server_build"], "snc_server") if kind == "current" \
        else os.path.join(REPO, frozen["pres1_server"])
    client = os.path.join(REPO, frozen["server_build"], "net_bench")   # one load generator for every cell
    srv = bind(SRV_CPUS) + [server, "--shards", str(SHARDS), "--port", str(port), "--objects", str(OBJ * 2),
                            "--cpus", ",".join(map(str, SRV_CPUS)), "--data-dir", data, "--admin-port", str(admin)]
    if ckpt:
        srv += ["--checkpoint-s", str(CHECKPOINT_S)]
    if kind == "current":
        srv += PACING_FLAGS   # stated in every current-server cell, N included; the pre-S1 server has none
    cli = bind(CLI_CPUS) + [client, "--port", str(port), "--threads", str(THREADS), "--objects", str(OBJ),
                            "--ids", ids, "--mix", MIX, "--route", ROUTE, "--cpus", ",".join(map(str, CLI_CPUS))]
    if rate:
        run = cli + ["--rate", str(rate), "--depth", str(OPEN_DEPTH), "--warmup", str(WARMUP_S),
                     "--seconds", str(WINDOW_S), "--drain-s", str(DRAIN_S)]
    else:
        run = cli + ["--depth", str(CLOSED_DEPTH), "--seconds", str(WINDOW_S)]
    return dict(name=name, data=data, ids=ids, server=srv, load=cli + ["--depth", "64", "--seconds", "0"], run=run)


# --- identity: machine (manifest e) and build ---

def rd(p):
    try:
        return open(p).read()
    except OSError:
        return ""


def sha(p):
    try:
        return hashlib.sha256(open(p, "rb").read()).hexdigest()
    except OSError:
        return None


def cpulist(s):
    out = []
    for part in s.strip().split(","):
        if part:
            a, _, b = part.partition("-")
            out += list(range(int(a), int(b or a) + 1))
    return out


def manifest_values(text):
    def g(p, f=lambda x: x.strip()):
        m = re.search(p, text, re.M)
        return f(m[1]) if m else None
    return dict(kernel=g(r"### uname -a\n\S+ \S+ (\S+)"), cpus=g(r"^CPU\(s\):\s+(\d+)", int),
                model=g(r"^Model name:\s+(.+)$"), node0=g(r"^NUMA node0 CPU\(s\):\s+(.+)$"),
                node1=g(r"^NUMA node1 CPU\(s\):\s+(.+)$"), mem_kb=g(r"^MemTotal:\s+(\d+) kB", int))


def machine_identity(frozen, data_disks):
    """Each check recorded; all must hold, before and after the session."""
    want = manifest_values(open(os.path.join(REPO, frozen["manifest"])).read())
    m = re.search(r"^model name\s*:\s*(.+)$", rd("/proc/cpuinfo"), re.M)
    model = m[1].strip() if m else None
    mm = re.search(r"^MemTotal:\s+(\d+) kB", rd("/proc/meminfo"), re.M)
    mem = int(mm[1]) if mm else 0
    n = os.cpu_count()
    govs = sorted({rd(f"/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_governor").strip() for i in range(n)})
    c = dict(
        manifest_sha256=sha(os.path.join(REPO, frozen["manifest"])) == frozen["manifest_sha256"],
        kernel=os.uname().release == want["kernel"], cpus=n == want["cpus"], model=model == want["model"],
        node0=cpulist(rd("/sys/devices/system/node/node0/cpulist")) == cpulist(want["node0"] or ""),
        node1=cpulist(rd("/sys/devices/system/node/node1/cpulist")) == cpulist(want["node1"] or ""),
        memory=want["mem_kb"] is not None and abs(mem - want["mem_kb"]) <= want["mem_kb"] // 1000,
        governors=govs == ["performance"],
        numa_balancing=rd("/proc/sys/kernel/numa_balancing").strip() == "0",
        allocation=not set(ALLOC["housekeeping_cpus"]) & set(SRV_CPUS + CLI_CPUS))
    disk = sorted(data_disks)[0] if data_disks and len(data_disks) == 1 else None
    c["data_disk_model"] = disk is not None and rd(f"/sys/block/{disk}/device/model").strip() == DATA_DISK_MODEL
    uuid_dev = os.path.realpath(f"/dev/disk/by-uuid/{DATA_FS_UUID}")
    mount = next((l.split() for l in rd("/proc/self/mounts").splitlines()
                  if l.split()[1:2] == [os.path.dirname(DATA_ROOT)]), None)
    c["data_fs_uuid"] = mount is not None and os.path.realpath(mount[0]) == uuid_dev
    c["data_mount"] = mount is not None and mount[2] == "ext4" and mount[3].startswith(DATA_MOUNT_OPTS)
    values = dict(kernel=os.uname().release, model=model, mem_kb=mem, governors=govs, data_disk=disk,
                  data_mount=mount, numa_balancing=rd("/proc/sys/kernel/numa_balancing").strip())
    return c, values


def build_identity(frozen):
    got = dict(snc_server=sha(os.path.join(REPO, frozen["server_build"], "snc_server")),
               net_bench=sha(os.path.join(REPO, frozen["server_build"], "net_bench")),
               pres1_server=sha(os.path.join(REPO, frozen["pres1_server"])))
    want = dict(snc_server=frozen["snc_server_sha256"], net_bench=frozen["net_bench_sha256"],
                pres1_server=frozen["pres1_server_sha256"])
    return {k: got[k] is not None and got[k] == want[k] for k in got}, got


def git(*a):
    return subprocess.run(["git", "-C", REPO, *a], capture_output=True, text=True)


def frozen_problems(frozen):
    """A real run needs a complete freeze, a filed GO, and no code change
    after the runner commit (docs, the freeze file and results may change)."""
    p = []
    for k in ("runner_commit", "go_record", "snc_server_sha256", "net_bench_sha256", "pres1_server_sha256"):
        if not frozen.get(k):
            p.append(f"bench/s6_frozen.json: {k} is not set (not frozen / no GO)")
    if frozen.get("go_record") and not os.path.exists(os.path.join(REPO, frozen["go_record"])):
        p.append(f"GO record {frozen['go_record']} does not exist")
    rc = frozen.get("runner_commit")
    if rc:
        if git("merge-base", "--is-ancestor", rc, "HEAD").returncode != 0:
            p.append(f"runner commit {rc} is not an ancestor of HEAD")
        d = git("diff", "--name-only", rc, "HEAD", "--", ".", ":!docs", ":!AGENTS.md", ":!bench/s6_frozen.json",
                ":!bench/results").stdout.split()
        if d:
            p.append("code changed after the runner commit: " + ", ".join(d[:5]))
    return p


def isolation(scratch, out, disks_of=pilot.disks_of):
    disks = {role: disks_of(p) for role, p in (("data", DATA_ROOT), ("scratch", scratch), ("out", out))}
    problems = pilot.isolation_problems(disks["data"], {k: disks[k] for k in ("scratch", "out")})
    if disks["data"] is not None and len(disks["data"]) != 1:
        problems.append(f"the data root is on {sorted(disks['data'])}, not exactly one disk")
    return problems, {k: sorted(v) if v is not None else None for k, v in disks.items()}


def preflight(out, scratch, frozen, real):
    problems, disks = isolation(scratch, out)
    if git("status", "--porcelain").stdout.strip():
        problems.append("the git tree is not clean")
    if subprocess.run(["pgrep", "-x", "snc_server|net_bench"], capture_output=True).returncode == 0:
        problems.append("snc_server or net_bench is already running")
    free = os.statvfs(scratch)
    if free.f_bavail * free.f_frsize < 2 * 16 * OBJ:
        problems.append(f"{scratch} has less than twice the ids file's size free")
    mc, mv = machine_identity(frozen, set(disks["data"] or []))
    bc, bv = build_identity(frozen)
    problems += [f"machine: {k}" for k, v in mc.items() if not v] + [f"build: {k}" for k, v in bc.items() if not v]
    if real:
        problems += frozen_problems(frozen)
    rec = dict(git_head=git("rev-parse", "HEAD").stdout.strip(), frozen=frozen, scratch=os.path.realpath(scratch),
               data_root=DATA_ROOT, out=os.path.realpath(out), disks=disks, machine_checks=mc, machine_values=mv,
               build_checks=bc, build_values=bv, problems=problems,
               utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    return not problems, rec


# --- the process driver ---

def ask(port, req, timeout=1.0):
    """One admin request on its own connection. Returns the line, or None if
    the reply did not arrive whole (no newline: cut, S4.1) or failed."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.sendall((req + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    return None
                buf += chunk
            return buf.decode(errors="replace").rstrip("\n")
    except OSError:
        return None


class Sampler(threading.Thread):
    """GAUGES and HEALTH every 100 ms on fixed deadlines, pinned to CPU 0.
    Raw replies kept verbatim with their monotonic times."""
    def __init__(self, admin, path):
        super().__init__(daemon=True)
        self.admin, self.path, self.stop = admin, path, threading.Event()
        self.samples = []   # parsed GAUGES samples, for the summary

    def run(self):
        os.sched_setaffinity(0, {SAMPLER_CPU})
        period = int(SAMPLE_S * 1_000_000_000)
        nxt = time.monotonic_ns()
        with gzip.open(self.path, "wt") as f:
            while not self.stop.is_set():
                for req in ("GAUGES", "HEALTH"):
                    t = time.monotonic_ns()
                    r = ask(self.admin, req)
                    ok = r is not None and not r.startswith("NO ")
                    f.write(json.dumps(dict(t_ns=t, req=req, ok=ok, reply=r)) + "\n")
                    if req == "GAUGES":
                        self.samples.append(dict(t_ns=t, ok=ok, gauges=parse_kv(r) if ok else None))
                nxt += period
                self.stop.wait(max(0, nxt - time.monotonic_ns()) / 1e9)


def observer(dev, path, stop):
    os.sched_setaffinity(0, {OBSERVER_CPU})
    rows = []
    while not stop.is_set():
        for line in rd("/proc/diskstats").splitlines():
            p = line.split()
            if len(p) > 19 and p[2] == dev:
                rows.append(f"{time.monotonic_ns()},{p[7]},{p[9]},{p[10]},{p[11]},{p[12]},{p[13]},{p[18]},{p[19]}")
                break
        stop.wait(0.1)
    with gzip.open(path, "wt") as f:
        f.write("t_ns,w_ios,w_sect,w_ms,inflight,io_ms,wq_ms,flush,flush_ms\n" + "\n".join(rows) + "\n")


def wait_port(p, proc, seconds=60):
    t_end = time.time() + seconds
    while time.time() < t_end:
        if proc.poll() is not None:
            return False
        try:
            socket.create_connection(("127.0.0.1", p), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


class Driver:
    def __init__(self, out, scratch, frozen, dev):
        self.out, self.scratch, self.frozen, self.dev = out, scratch, frozen, dev
        self.raw = os.path.join(out, "raw")
        self.port = 26000
        self.reps = []
        self.attempts = {}

    def execute(self, cell, rep):
        self.port += 40
        admin = self.port + 30
        c = commands(cell, rep, self.port, admin, self.scratch, self.frozen)
        k = self.attempts[(cell, rep)] = self.attempts.get((cell, rep), 0) + 1
        tag = c["name"] + ("-rerun" if k > 1 else "")   # raw files of a VOID attempt are kept, not overwritten
        b = os.path.join(self.raw, tag)
        shutil.rmtree(c["data"], ignore_errors=True)
        os.makedirs(DATA_ROOT, exist_ok=True)
        T, r = {}, dict(cell=cell, rep=rep, tag=tag, void=None, server_cmd=c["server"], run_cmd=c["run"])
        slog = open(b + ".server.log", "w")
        P = subprocess.Popen(c["server"], stdout=slog, stderr=subprocess.STDOUT)
        stop, obs, smp = threading.Event(), None, None
        load = run = None
        try:
            T["server_start"] = time.monotonic_ns()
            if not wait_port(self.port, P) or ask(admin, "HEALTH", timeout=5) is None:
                r["void"] = "harness"
                r["why"] = "server or admin port did not come up"
                self.reps.append(r)
                return r
            smp = Sampler(admin, b + ".samples.jsonl.gz")
            smp.start()
            obs = threading.Thread(target=observer, args=(self.dev, b + ".disk.csv.gz", stop))
            obs.start()
            load = subprocess.run(c["load"], capture_output=True, text=True, timeout=900)
            if load.returncode:
                r["void"] = "harness"
                r["why"] = f"load failed rc {load.returncode}"
                self.reps.append(r)
                return r
            T["run_start"] = time.monotonic_ns()
            run = subprocess.run(c["run"], capture_output=True, text=True, timeout=WINDOW_S + WARMUP_S + 900)
            T["run_end"] = time.monotonic_ns()
            r["server_alive_at_end"] = P.poll() is None
            r["client_rc"] = run.returncode
            # A started checkpoint finishes (or reaches the hard maximum) before the stop.
            t_end = time.monotonic_ns() + (MAX_AGE_MS + 5000) * 1_000_000
            while P.poll() is None and time.monotonic_ns() < t_end:
                h = parse_kv(ask(admin, "HEALTH") or "")
                if h.get("ckpt", "idle") == "idle":
                    break
                time.sleep(0.1)
            T["ckpt_wait_end"] = time.monotonic_ns()
            r["final_gauges"] = ask(admin, "GAUGES", timeout=5)
            r["final_health"] = ask(admin, "HEALTH", timeout=5)
        finally:
            if smp:
                smp.stop.set()
                smp.join()
            stop.set()
            if obs:
                obs.join()
            P.terminate()
            try:
                r["server_rc"] = P.wait(timeout=180)
            except subprocess.TimeoutExpired:
                P.kill()
                r["server_rc"] = P.wait()
            slog.close()
            json.dump(T, open(b + ".times.json", "w"))
            with open(b + ".client.log", "w") as f:
                f.write(" ".join(c["server"]) + "\n" + " ".join(c["run"]) + "\n--- load\n"
                        + ((load.stdout + load.stderr) if load else "") + "--- run\n"
                        + ((run.stdout + run.stderr) if run else ""))
            for k in ("final_gauges", "final_health"):
                open(f"{b}.{k}.txt", "w").write((r.get(k) or "") + "\n")
            shutil.rmtree(c["data"], ignore_errors=True)
            if os.path.exists(c["ids"]):
                os.remove(c["ids"])
        slog_text = rd(b + ".server.log")
        r["client"] = parse_client(run.stdout if run else "")
        r["checkpoints"] = checkpoints_completed(slog_text)
        r["log_markers"] = sorted(set(m[0] for m in BAD_LOG.finditer(slog_text)))
        fg = parse_kv(r.get("final_gauges") or "")
        r["final_emits_peak"] = shard_values(fg, "emits_peak")
        r["final_degraded"] = shard_values(fg, "ckpt_degraded_count")   # None per shard on the pre-S1 server
        r["controller"] = controller_summary(smp.samples) if smp else None
        if CELLS[cell][2] and CELLS[cell][1]:   # P and H: the checkpoint must begin inside the window
            win_lo = T["run_start"] + WARMUP_S * 1_000_000_000
            r["window_ns"] = [win_lo, win_lo + WINDOW_S * 1_000_000_000]
            r["checkpoint_begin_ns"] = checkpoint_begin(smp.samples)
            if not began_in_window(r["checkpoint_begin_ns"], *r["window_ns"]):
                r["void"] = "window"
        if run is not None and run.returncode != 0 and not r["void"]:
            r["client_failed"] = True   # a result (rule 7 via missing fields), not a rerun
        r["checkpoints"] = [(o, str(s)) for o, s in r["checkpoints"]]
        self.reps.append(r)
        json.dump(self.reps, open(os.path.join(self.out, "summary.json"), "w"), indent=1, default=str)
        print(json.dumps(dict(cell=cell, rep=rep, void=r["void"], client=r["client"])), flush=True)
        r["checkpoints"] = [(o, Fraction(s)) for o, s in r["checkpoints"]]
        return r


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    frozen = json.load(open(FROZEN_PATH))
    if not a.dry_run and os.path.exists(a.out):
        raise SystemExit(f"{a.out} exists: each S6 session happens once")
    os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
    os.makedirs(a.scratch, exist_ok=True)
    os.makedirs(DATA_ROOT, exist_ok=True)
    ok, rec = preflight(a.out, a.scratch, frozen, real=not a.dry_run)
    if a.dry_run:
        port = 26000
        for rep in range(REPS):
            for cell in ORDER:
                port += 40
                c = commands(cell, rep, port, port + 30, a.scratch, frozen)
                print(f"# {c['name']}\n  server: {' '.join(c['server'])}\n  load:   {' '.join(c['load'])}\n"
                      f"  run:    {' '.join(c['run'])}")
        print(json.dumps(rec, indent=1, default=str))
        print("dry run: preflight", "ok" if ok else "REFUSED: " + "; ".join(rec["problems"]), flush=True)
        return 0
    json.dump(rec, open(os.path.join(a.out, "preflight.json"), "w"), indent=1, default=str)
    if not ok:
        raise SystemExit("REFUSED before any run: " + "; ".join(rec["problems"]))
    d = Driver(a.out, a.scratch, frozen, rec["machine_values"]["data_disk"])

    def post():
        mc, mv = machine_identity(frozen, set(rec["disks"]["data"]))
        bc, bv = build_identity(frozen)
        json.dump(dict(machine_checks=mc, machine_values=mv, build_checks=bc, build_values=bv),
                  open(os.path.join(a.out, "postflight.json"), "w"), indent=1, default=str)
        why = [f"machine {k}" for k, v in mc.items() if not v] + [f"build {k}" for k, v in bc.items() if not v]
        if mv != rec["machine_values"]:
            why.append("machine values changed")
        return not why, why

    reps, void = run_session(d.execute, post)
    v = verdict(reps, void)
    json.dump(v, open(os.path.join(a.out, "verdict.json"), "w"), indent=1, default=str)
    print("VERDICT", v["verdict"], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
