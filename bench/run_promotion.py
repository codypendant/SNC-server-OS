#!/usr/bin/env python3
"""Promoting a standby in place (docs/REPLICATION.md).

Primary: snc_server on cores 0-1, sync replication to a standby on core 2.
Client: one net_bench thread on core 3 creates N accounts and runs 50%
reads for a few seconds. Then the primary is SIGKILLed and the standby gets
SIGUSR1. Measures the promotion as the standby reports it (settle, new term,
index rebuild, resume) and as seen from outside (signal to the
"promoted" line), then runs a short net_bench against the promoted node.

Prints CSV rows; usage: run_promotion.py [--reps N] [--objects N] [--out FILE].
"""
import argparse
import csv
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

BUILD = "build"
WORK = "/tmp/snc-promote-bench"
UUID = "5a" * 16


def wait_port(port, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    sys.exit(f"port {port} never opened")


class Reader(threading.Thread):
    """Collects a process's output lines with arrival times."""

    def __init__(self, proc):
        super().__init__(daemon=True)
        self.proc, self.lines = proc, []
        self.start()

    def run(self):
        for line in self.proc.stdout:
            self.lines.append((time.time(), line.rstrip()))


def net_bench(port, objects, seconds):
    out = subprocess.run(
        [f"{BUILD}/net_bench", "--port", str(port), "--threads", "1", "--cpus", "3",
         "--objects", str(objects), "--seconds", str(seconds), "--route", "owner", "--reads", "50",
         "--depth", "512"], capture_output=True, text=True, timeout=600)
    return float(re.search(r"ops/s (\d+)", out.stdout)[1])


def run(port, objects, seconds):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    pport, rport, sport = port, port + 100, port + 200
    sb = subprocess.Popen(
        [f"{BUILD}/snc_server", "--cpus", "2,2", "--port", str(sport), "--standby", str(rport),
         "--data-dir", f"{WORK}/s", "--objects", str(objects), "--space-uuid", UUID],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sr = Reader(sb)
    prim = subprocess.Popen(
        [f"{BUILD}/snc_server", "--cpus", "0,1", "--port", str(pport), "--objects", str(objects),
         "--data-dir", f"{WORK}/p", "--replica-of", f"127.0.0.1:{rport}", "--space-uuid", UUID,
         "--replica-timeout-ms", "0"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    Reader(prim)
    wait_port(pport)
    before = net_bench(pport, objects, seconds)
    prim.send_signal(signal.SIGKILL)
    prim.wait()
    t0 = time.time()
    sb.send_signal(signal.SIGUSR1)
    line = None
    while time.time() < t0 + 120 and line is None:
        time.sleep(0.005)
        for t, l in list(sr.lines):
            if l.startswith("promoted to primary"):
                line, seen = l, t
    m = re.search(r"term (\d+), in ([\d.]+)s", line)
    after = net_bench(sport, 4000, 3)
    sb.send_signal(signal.SIGTERM)
    sb.wait(timeout=120)
    return dict(objects=objects, before_ops_s=round(before), promote_s=float(m[2]),
                signal_to_serving_s=round(seen - t0, 3), term=int(m[1]), after_ops_s=round(after))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--objects", type=int, default=1000000)
    ap.add_argument("--seconds", type=int, default=5)
    ap.add_argument("--out", default="bench/results/promotion.csv")
    a = ap.parse_args()
    rows, port = [], 32000
    for rep in range(a.reps):
        port += 10
        r = run(port, a.objects, a.seconds)
        r["rep"] = rep
        rows.append(r)
        print(r, flush=True)
    fields = ["objects", "rep", "before_ops_s", "promote_s", "signal_to_serving_s", "term", "after_ops_s"]
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k) for k in fields})
    shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
