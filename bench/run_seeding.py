#!/usr/bin/env python3
"""Seeding and catching up a standby (docs/REPLICATION.md).

Primary: snc_server on cores 0-1, async replication to a standby that is
not running yet, a checkpoint every 4 s, 64 MiB of WAL kept in memory per
shard. Client: one net_bench thread on core 3 (1M accounts, 50% reads,
owner-routed, depth 512). Standby: snc_server --standby --seed-from on
core 2, started on an empty directory.

  idle:     the standby starts after the client is done. Measures the seed
            (the latest checkpoint's files) and the catch-up from WAL files
            and memory, until every shard's standby has acked all.
  loaded:   the standby starts 2 s into the client's run. Measures the
            client's throughput against a run with no standby, and the
            catch-up time under load.
  loaded-N: the same with catch-up from disk limited to N MiB/s per shard
            (--catchup-mib-s).

Prints CSV rows; usage: run_seeding.py [--reps N] [--out FILE].
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
WORK = "/tmp/snc-seed-bench"


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


def run(mode, port, seconds):
    limit = mode.split("-")[1] if "-" in mode else None
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    pport, sport, seedport = port, port + 100, port + 200
    prim = subprocess.Popen(
        [f"{BUILD}/snc_server", "--cpus", "0,1", "--port", str(pport), "--objects", "1000000",
         "--data-dir", f"{WORK}/p", "--replica-of", f"127.0.0.1:{sport}", "--async-replica",
         "--seed-port", str(seedport), "--checkpoint-s", "4", "--replica-retain-mib", "64",
         "--report-ms", "100"] + (["--catchup-mib-s", limit] if limit else []),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    pr = Reader(prim)
    wait_port(pport)
    sb = None
    started = None

    def start_standby():
        nonlocal sb, started
        started = time.time()
        sb = subprocess.Popen(
            [f"{BUILD}/snc_server", "--cpus", "2,2", "--port", str(port + 300), "--standby",
             str(sport), "--data-dir", f"{WORK}/s", "--seed-from", f"127.0.0.1:{seedport}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    timer = None
    if mode.startswith("loaded"):
        # The client loads 1M accounts first (about 3 s), then runs.
        timer = threading.Timer(5.0, start_standby)
        timer.start()
    out = subprocess.run(
        [f"{BUILD}/net_bench", "--port", str(pport), "--threads", "1", "--cpus", "3",
         "--objects", "1000000", "--seconds", str(seconds), "--route", "owner", "--reads", "50",
         "--depth", "512"], capture_output=True, text=True, timeout=600)
    ops = float(re.search(r"ops/s (\d+)", out.stdout)[1])
    if mode == "idle":
        start_standby()
    row = dict(mode=mode, client_ops_s=round(ops))
    if mode != "none":
        # Caught up: live, nothing unacked, after the standby started.
        done = None
        t_end = time.time() + 300
        while time.time() < t_end and done is None:
            time.sleep(0.1)
            for t, line in list(pr.lines):
                if t > started + 0.2 and "live=1" in line and " lag=0 " in line:
                    done = t
                    break
        last = [l for _, l in pr.lines if l.startswith("report")][-1]
        row["catch_up_s"] = round(done - started, 2) if done else None
        row["shipped_mib"] = float(re.search(r"shipped_mib=([\d.]+)", last)[1])
        row["from_disk_mib"] = float(re.search(r"from_disk_mib=([\d.]+)", last)[1])
    if timer:
        timer.cancel()
    for p in (prim, sb):
        if p:
            p.send_signal(signal.SIGTERM)
    if sb:
        sout = sb.communicate(timeout=120)[0]
        m = re.search(r"seeded from \S+: checkpoint \d+, \d+ files, ([\d.]+) MiB", sout)
        row["seed_mib"] = float(m[1]) if m else None
    prim.wait(timeout=120)
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=12)
    ap.add_argument("--out", default="bench/results/seeding.csv")
    ap.add_argument("--modes", default="none,loaded,idle")
    a = ap.parse_args()
    rows, port = [], 31000
    for rep in range(a.reps):
        for mode in a.modes.split(","):
            port += 10
            r = run(mode, port, a.seconds)
            r["rep"] = rep
            rows.append(r)
            print(r, flush=True)
    fields = ["mode", "rep", "client_ops_s", "catch_up_s", "seed_mib", "shipped_mib", "from_disk_mib"]
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k) for k in fields})
    shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
