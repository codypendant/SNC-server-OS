#!/usr/bin/env python3
"""Safe client retries across a failover (docs/RETRIES.md).

Same layout as run_failover.py: a primary (cores 0-1, sync, --witness), a
standby (core 2) and snc_witness. One net_bench thread on core 3 runs
deposits only (100K accounts, 256 in flight, 8 s) with --verify, which
reads every account at the end and checks it holds its start plus exactly
the deposits sent to it.

  plain:     no session, no failure (the cost baseline).
  session:   sequenced requests (--session), no failure.
  failover:  sequenced, with the standby as a second endpoint; the primary
             is SIGKILLed 3 s into the run. The client fails over, resends
             what was unanswered, and --verify must find no account off.

Prints CSV rows; usage: run_retries.py [--reps N] [--out FILE].
"""
import argparse
import csv
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, "bench")
import run_failover as fo  # noqa: E402


def client(port, standby_port, session, seconds=8):
    args = [f"{fo.BUILD}/net_bench", "--port", str(port), "--threads", "1", "--cpus", "3",
            "--objects", "100000", "--seconds", str(seconds), "--route", "owner", "--reads", "0",
            "--depth", "256", "--verify"]
    if session:
        args += ["--session", "1000", "--endpoint", f"127.0.0.1:{standby_port}"]
    return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def run(case, port):
    prim, sb, w, p_cli, s_cli = fo.start(port)
    c = client(p_cli, s_cli, case != "plain")
    t_kill = None
    if case == "failover":
        # The client loads 100K accounts first (about half a second).
        time.sleep(3.5)
        t_kill = time.time()
        prim.kill()
    out = c.communicate(timeout=300)[0]
    ok = c.returncode == 0
    m = re.search(r"ops/s (\d+)", out)
    v = re.search(r"verify: (\d+) accounts, (\d+) deposits, (\d+) mismatches", out)
    f = re.search(r"failovers (\d+)\s+replayed (\d+)", out)
    row = dict(case=case, ok=int(ok and v is not None and int(v[3]) == 0),
               ops_s=int(m[1]) if m else None, deposits=int(v[2]) if v else None,
               mismatches=int(v[3]) if v else None, failovers=int(f[1]) if f else 0,
               replayed=int(f[2]) if f else 0)
    if t_kill:
        t, _ = w.wait_for(r"failover:", 1)
        row["kill_to_promoted_s"] = round(t - t_kill, 3) if t else None
    if not row["ok"]:
        print(out, file=sys.stderr)
    fo.stop_all(*(p for p in (prim, sb, w) if p.p.poll() is None))
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--out", default="bench/results/retries.csv")
    a = ap.parse_args()
    rows, port = [], 35000
    for rep in range(a.reps):
        for case in ("plain", "session", "failover"):
            port += 100
            r = run(case, port)
            r["rep"] = rep
            rows.append(r)
            print(r, flush=True)
    fields = ["case", "rep", "ok", "ops_s", "deposits", "mismatches", "failovers", "replayed",
              "kill_to_promoted_s"]
    with open(a.out, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        wr.writeheader()
        for r in rows:
            wr.writerow({k: r.get(k) for k in fields})
    shutil.rmtree(fo.WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
