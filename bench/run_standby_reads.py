#!/usr/bin/env python3
"""Reads on the standby (docs/REPLICATION.md).

Primary snc_server on cores 0-1, sync replication to a standby whose two
shards share core 2, with --standby-reads 200 (200 ms read leases). 1M
accounts, created once through the primary. Two net_bench clients share
core 3 for 8 s, each with one thread and 512 requests in flight:

  on-primary:  a writer (deposits) and a reader (get_balance), both on the
               primary.
  split:       the same writer on the primary, the reader on the standby.

Reports each client's ops/s and the reader's stale answers (E_STALE).
Prints CSV rows; usage: run_standby_reads.py [--reps N] [--out FILE].
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
import time

BUILD = "build"
WORK = "/tmp/snc-standby-reads-bench"
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


def bench_args(port, reads, seconds, ids):
    return [f"{BUILD}/net_bench", "--port", str(port), "--threads", "1", "--cpus", "3",
            "--objects", "1000000", "--seconds", str(seconds), "--route", "owner", "--reads",
            str(reads), "--depth", "512", "--ids", ids]


def parse(out):
    m = re.search(r"ops/s (\d+)\s+errors (\d+)\s+overloaded (\d+)\s+stale (\d+)", out)
    if not m:
        sys.exit("net_bench failed:\n" + out)
    return int(m[1]), int(m[4]), int(m[2])


def run(case, port, seconds):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    p_cli, s_cli, repl = port, port + 10, port + 20
    ids = f"{WORK}/ids"
    sb = subprocess.Popen(
        [f"{BUILD}/snc_server", "--cpus", "2,2", "--port", str(s_cli), "--standby", str(repl),
         "--data-dir", f"{WORK}/s", "--space-uuid", UUID, "--standby-reads", "200"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_port(s_cli)
    prim = subprocess.Popen(
        [f"{BUILD}/snc_server", "--cpus", "0,1", "--port", str(p_cli), "--data-dir", f"{WORK}/p",
         "--space-uuid", UUID, "--replica-of", f"127.0.0.1:{repl}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_port(p_cli)
    # Create the accounts (and the ID list) through the primary.
    subprocess.run(bench_args(p_cli, 100, 0.1, ids), capture_output=True, check=True, timeout=300)
    writer = subprocess.Popen(bench_args(p_cli, 0, seconds, ids), stdout=subprocess.PIPE, text=True)
    reader = subprocess.Popen(bench_args(s_cli if case == "split" else p_cli, 100, seconds, ids),
                              stdout=subprocess.PIPE, text=True)
    w_out, r_out = writer.communicate(timeout=300)[0], reader.communicate(timeout=300)[0]
    w_ops, _, _ = parse(w_out)
    r_ops, r_stale, r_err = parse(r_out)
    for p in (prim, sb):
        p.send_signal(signal.SIGTERM)
    for p in (prim, sb):
        p.wait(timeout=120)
    return dict(case=case, writer_ops_s=w_ops, reader_ops_s=r_ops, total_ops_s=w_ops + r_ops,
                reader_stale=r_stale, reader_errors=r_err)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--out", default="bench/results/standby_reads.csv")
    a = ap.parse_args()
    rows, port = [], 27000  # below the ephemeral range (see run_failover.py)
    for rep in range(a.reps):
        for case in ("on-primary", "split"):
            port += 100
            r = run(case, port, a.seconds)
            r["rep"] = rep
            rows.append(r)
            print(r, flush=True)
    fields = ["case", "rep", "writer_ops_s", "reader_ops_s", "total_ops_s", "reader_stale",
              "reader_errors"]
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k) for k in fields})
    shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()
