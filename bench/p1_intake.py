#!/usr/bin/env python3
"""P1: follow-up intake (D.1) regression matrix (docs/FOLLOWUP_INTAKE_DESIGN.md
section 9, P1; PR #87 review). A development-host regression check, not
campaign evidence: it reports the overhead of the credit (W) and budget (X)
checks and the H gate against the same build with every D.1 gate off. No
production default is tuned from it.

Arms (the same Release build; only environment overrides differ):
  w{W}-x{X}   W in {16, 64, 256} x X in {256, 1024, 4096}, H and L default
  no-window   W = 0, X = 1024 (the default), H and L default
  rollback    SNC_FOLLOWUP_INTAKE=off: H, W, X, owner gate and L off
              (the contract checks stay on, as in production rollback)

Workloads (snc_bench, in process, durable WAL with RWF_DSYNC):
  uniform     100% transfers to a uniform random account
  cross       100% transfers, every one to another shard's account
              (--xfer-local 0): one follow-up exchange per transfer

Every rep runs every (workload, arm) cell once, in a fresh random order,
on a fresh data directory that is removed afterwards. Per cell it reports
the median and range of ops/s, p99 latency and turned-away (E_OVERLOADED)
ops over reps, and the ops/s ratio to rollback.

Usage: p1_intake.py BUILD_DIR OUT_DIR [--reps 5] [--seconds 8] [--shards 2]
       [--objects 100000] [--scratch DIR]
"""
import argparse
import json
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ARMS = {}
for w in (16, 64, 256):
    for x in (256, 1024, 4096):
        ARMS[f"w{w}-x{x}"] = {"SNC_FOLLOWUP_WINDOW": str(w), "SNC_EXCHANGE_BUDGET": str(x)}
ARMS["no-window"] = {"SNC_FOLLOWUP_WINDOW": "0", "SNC_EXCHANGE_BUDGET": "1024"}
ARMS["rollback"] = {"SNC_FOLLOWUP_INTAKE": "off"}
WORKLOADS = {"uniform": ["--transfer", "100"], "cross": ["--transfer", "100", "--xfer-local", "0"]}
OVERRIDES = ("SNC_FOLLOWUP_INTAKE", "SNC_FOLLOWUP_WINDOW", "SNC_EXCHANGE_BUDGET", "SNC_LINK_HOLD_MAX", "SNC_X")


def parse(out):
    r = {}
    m = re.search(r"ops/s (\d+)", out)
    r["ops"] = int(m.group(1)) if m else None
    m = re.search(r"p99 (\d+)", out)
    r["p99_ns"] = int(m.group(1)) if m else None
    m = re.search(r"rejected (\d+), turned away (\d+), follow-ups/op ([\d.]+)", out)
    if m:
        r["rejected"], r["turned_away"], r["followups_per_op"] = int(m.group(1)), int(m.group(2)), float(m.group(3))
    m = re.search(r"errors (\d+)", out)
    r["errors"] = int(m.group(1)) if m else None
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("out")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--shards", type=int, default=2)
    ap.add_argument("--objects", type=int, default=100000)
    ap.add_argument("--scratch", default=None)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    base_env = {k: v for k, v in os.environ.items() if k not in OVERRIDES}
    runs = []
    log = open(os.path.join(a.out, "runs.log"), "a")
    rng = random.Random(20261110)
    for rep in range(a.reps):
        cells = [(w, arm) for w in WORKLOADS for arm in ARMS]
        rng.shuffle(cells)
        for wl, arm in cells:
            work = tempfile.mkdtemp(prefix="snc-p1-", dir=a.scratch)
            try:
                cmd = [f"{a.build}/snc_bench", "--shards", str(a.shards), "--objects", str(a.objects), "--seconds",
                       str(a.seconds), "--data-dir", f"{work}/d", "--no-restart"] + WORKLOADS[wl]
                env = dict(base_env, **ARMS[arm])
                t0 = time.time()
                p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=600)
                r = parse(p.stdout)
                r.update(rep=rep, workload=wl, arm=arm, rc=p.returncode, wall_s=round(time.time() - t0, 1))
                runs.append(r)
                log.write(f"=== rep {rep} {wl} {arm} rc={p.returncode} env={ARMS[arm]}\n{p.stdout}{p.stderr}\n")
                log.flush()
                print(f"rep {rep} {wl:7s} {arm:10s} rc={p.returncode} ops/s {r['ops']} p99 {r['p99_ns']} "
                      f"turned_away {r.get('turned_away')}", flush=True)
            finally:
                shutil.rmtree(work, ignore_errors=True)
    with open(os.path.join(a.out, "runs.json"), "w") as f:
        json.dump(runs, f, indent=1)
    # Summary: medians over reps; ratio to rollback's median.
    lines = [f"P1 follow-up intake matrix: {a.reps} reps x {a.seconds}s, shards {a.shards}, objects {a.objects}",
             "cell: ops/s median [min..max]  ratio-to-rollback  p99 us median  turned-away median  bad runs"]
    summary = {}
    for wl in WORKLOADS:
        ok_ref = [r["ops"] for r in runs if r["workload"] == wl and r["arm"] == "rollback" and r["rc"] == 0 and r["ops"]]
        ref = statistics.median(ok_ref) if ok_ref else None
        for arm in ARMS:
            rs = [r for r in runs if r["workload"] == wl and r["arm"] == arm]
            good = [r for r in rs if r["rc"] == 0 and r["ops"] and r.get("errors") == 0]
            ops = [r["ops"] for r in good]
            if not ops:
                lines.append(f"{wl:7s} {arm:10s} NO VALID RUNS ({len(rs)} attempted)")
                continue
            med = statistics.median(ops)
            p99 = statistics.median(r["p99_ns"] for r in good) / 1000
            ta = statistics.median(r.get("turned_away", 0) for r in good)
            ratio = med / ref if ref else float("nan")
            summary[f"{wl}/{arm}"] = dict(ops_median=med, ops_min=min(ops), ops_max=max(ops), ratio=ratio,
                                          p99_us_median=p99, turned_away_median=ta, valid=len(good), runs=len(rs))
            lines.append(f"{wl:7s} {arm:10s} {med:9.0f} [{min(ops)}..{max(ops)}]  {ratio:6.3f}  {p99:8.0f}  "
                         f"{ta:8.0f}  {len(rs) - len(good)}")
    with open(os.path.join(a.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
