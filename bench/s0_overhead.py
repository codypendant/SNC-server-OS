#!/usr/bin/env python3
"""S0 telemetry perturbation micro-check (docs/CHECKPOINT_BACKPRESSURE_DESIGN.md
section 11; PR #68 review section 12). NOT a campaign benchmark: a
development-host check meant to catch obvious perturbation, from which no
performance conclusion is drawn. Never run it on the R640 for S0.

Arms, interleaved over reps (same host, same build flags):
  main         the build before S0 (gauges absent), never polled
  s0-unread    the S0 build, GAUGES never read
  s0-rare      the S0 build, GAUGES read once a second
  s0-hot       the S0 build, GAUGES and HEALTH read every 10 ms (far more
               than any operator would; a stress case)

Each run: snc_server (--shards N, durable WAL, --checkpoint-s C) on a
fresh data directory, then net_bench closed loop for S seconds. Reports
ops/s and latency p99 per run and the median per arm.

Usage: s0_overhead.py MAIN_BUILD S0_BUILD [--reps 3] [--seconds 10]
       [--shards 2] [--threads 2] [--checkpoint-s 5] [--out FILE.json]
"""
import argparse
import json
import os
import random
import re
import shutil
import socket
import statistics
import subprocess
import tempfile
import threading
import time


def free_block(n):
    rnd = random.Random(os.getpid() * 7919 + time.time_ns())
    while True:
        base = rnd.randrange(20000, 31000)
        socks, ok = [], True
        for i in range(n):
            s = socket.socket()
            try:
                s.bind(("127.0.0.1", base + i))
            except OSError:
                ok = False
            socks.append(s)
        for s in socks:
            s.close()
        if ok:
            return base


def ask(port, line, timeout=2):
    with socket.create_connection(("127.0.0.1", port), timeout) as s:
        s.settimeout(timeout)
        s.sendall((line + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            c = s.recv(65536)
            if not c:
                break
            buf += c
        return buf.decode()


def wait_port(port, proc, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            if proc.poll() is not None:
                return False
            time.sleep(0.05)
    return False


def run_one(build, poll_s, verbs, a):
    work = tempfile.mkdtemp(prefix="snc-s0ovh-")
    base = free_block(a.shards + 2)
    cli, adm = base, base + a.shards + 1
    srv = subprocess.Popen([f"{build}/snc_server", "--shards", str(a.shards), "--port", str(cli),
                            "--data-dir", f"{work}/d", "--admin-port", str(adm), "--objects", "400000",
                            "--checkpoint-s", str(a.checkpoint_s)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stop = threading.Event()
    polls = [0]

    def poller():
        while not stop.is_set():
            for v in verbs:
                try:
                    ask(adm, v)
                    polls[0] += 1
                except OSError:
                    pass
            stop.wait(poll_s)

    try:
        assert wait_port(adm, srv), "snc_server did not start"
        t = threading.Thread(target=poller, daemon=True) if verbs else None
        if t:
            t.start()
        out = subprocess.run([f"{build}/net_bench", "--port", str(cli), "--threads", str(a.threads),
                              "--objects", "200000", "--seconds", str(a.seconds), "--reads", "50"],
                             capture_output=True, text=True, timeout=600).stdout
        stop.set()
        if t:
            t.join()
        ops = float(re.search(r"ops/s (\d+)", out).group(1))
        p99 = float(re.search(r"latency ns\s+p50 \S+\s+p95 \S+\s+p99 (\S+)", out).group(1))
        return {"ops_s": ops, "p99_ns": p99, "polls": polls[0]}
    finally:
        stop.set()
        srv.kill()
        srv.wait()
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("main_build")
    ap.add_argument("s0_build")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--shards", type=int, default=2)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--checkpoint-s", type=int, default=5)
    ap.add_argument("--out")
    a = ap.parse_args()
    arms = [
        ("main", a.main_build, 0, []),
        ("s0-unread", a.s0_build, 0, []),
        ("s0-rare", a.s0_build, 1.0, ["GAUGES"]),
        ("s0-hot", a.s0_build, 0.01, ["GAUGES", "HEALTH"]),
    ]
    res = {name: [] for name, *_ in arms}
    for rep in range(a.reps):
        order = arms[:] if rep % 2 == 0 else arms[::-1]  # alternate order against drift
        for name, build, poll_s, verbs in order:
            r = run_one(build, poll_s, verbs, a)
            res[name].append(r)
            print(f"rep {rep} {name:10s} ops/s {r['ops_s']:.0f}  p99 {r['p99_ns'] / 1000:.0f} us  polls {r['polls']}",
                  flush=True)
    summary = {}
    for name in res:
        ops = statistics.median(x["ops_s"] for x in res[name])
        p99 = statistics.median(x["p99_ns"] for x in res[name])
        summary[name] = {"median_ops_s": ops, "median_p99_ns": p99}
    ref = summary["main"]["median_ops_s"]
    print("\nmedian per arm (ratio to main):")
    for name, s in summary.items():
        print(f"  {name:10s} ops/s {s['median_ops_s']:.0f} ({s['median_ops_s'] / ref:.3f})  "
              f"p99 {s['median_p99_ns'] / 1000:.0f} us")
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"args": vars(a), "runs": res, "summary": summary}, f, indent=1)


if __name__ == "__main__":
    main()
