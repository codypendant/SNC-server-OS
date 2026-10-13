#!/usr/bin/env python3
"""Durable throughput with and without a replica, on SNC, Redis and
PostgreSQL (docs/REPLICATION.md). Same accounts workload and placement as
run.py: primary on --server-cpus (0-1), the replica on --replica-cpus
(2), clients on --client-cpus (2-3). SNC's load generator spins and
needs its cores whole, so for SNC (every mode) it runs one thread on the
last client core; sharing a core with it stalled the standby for seconds
(its io_uring workers starve). The Redis and PostgreSQL clients use
12-21% of their cores (docs/CONVENTIONAL.md) and keep both. Every primary
and replica fsyncs before acknowledging.

  SNC:        snc_server primary, snc_server --standby, net_bench owner-routed.
              none / async / sync (durable acks wait for the standby's fsync).
  Redis:      AOF appendfsync always on both; replication is asynchronous
              only (WAIT cannot run inside the deposit script).
  PostgreSQL: streaming replica from pg_basebackup. async, or sync with
              synchronous_standby_names = '*' (commit waits for the
              standby's flush, synchronous_commit = on).

Needs the tools run.py needs. Run as root.
"""
import argparse
import csv
import os
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run import Postgres, Redis, sh  # noqa: E402

UUID = "5a" * 16


def wait_port(port, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return
        except OSError:
            time.sleep(0.1)
    sys.exit(f"port {port} never opened")


def snc_run(a, mode, port):
    work = os.path.join(a.work, "snc")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    procs = []
    sargv = [os.path.join(a.build, "snc_server"), "--cpus", a.server_cpus, "--port", str(port),
             "--objects", "1000000", "--data-dir", os.path.join(work, "p")]
    if mode != "none":
        sb = subprocess.Popen([os.path.join(a.build, "snc_server"),
                               "--cpus", ",".join([a.replica_cpus] * 2),
                               "--port", str(port + 100), "--standby", str(port + 200),
                               "--data-dir", os.path.join(work, "s"), "--space-uuid", UUID],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        procs.append(sb)
        wait_port(port + 100)
        sargv += ["--space-uuid", UUID, "--replica-of", f"127.0.0.1:{port + 200}",
                  "--replica-timeout-ms", "0"]
        if mode == "async":
            sargv.append("--async-replica")
    srv = subprocess.Popen(sargv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.insert(0, srv)
    try:
        wait_port(port)
        out = subprocess.run([os.path.join(a.build, "net_bench"), "--port", str(port),
                              "--threads", "1", "--cpus", a.client_cpus.split(",")[-1],
                              "--objects", "1000000", "--seconds", str(a.seconds),
                              "--route", "owner", "--reads", "50", "--depth", "512"],
                             capture_output=True, text=True, timeout=900)
    finally:
        for p in procs:
            p.send_signal(signal.SIGTERM)
            p.communicate(timeout=120)
    if out.returncode != 0:
        sys.exit(f"net_bench failed:\n{out.stdout}\n{out.stderr}")
    ops = float(re.search(r"ops/s (\d+)", out.stdout)[1])
    p50 = float(re.search(r"p50 (\d+)", out.stdout)[1]) / 1000
    return ops, p50


class RedisReplica:
    def __init__(self, a, primary):
        self.a = a
        self.port = a.redis_port + 1
        self.primary = primary

    def start(self):
        d = os.path.join(self.a.work, "redisreplica")
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        sh(f"taskset -c {self.a.replica_cpus} redis-server --port {self.port} --dir {d} --save '' "
           f"--appendonly yes --appendfsync always --replicaof 127.0.0.1 {self.a.redis_port} "
           f"--daemonize yes --logfile {self.a.work}/redis-replica.log")
        cli = f"redis-cli -p {self.port}"
        while "master_link_status:up" not in sh(f"{cli} info replication") or \
                "master_sync_in_progress:1" in sh(f"{cli} info replication"):
            time.sleep(0.5)

    def stop(self):
        sh(f"redis-cli -p {self.port} shutdown nosave || true")


class PgReplica:
    def __init__(self, a, pg):
        self.a = a
        self.pg = pg
        self.dir = os.path.join(a.work, "pgreplica")
        self.port = a.pg_port + 1

    def start(self):
        psql = self.pg.psql
        sh(f"{psql} -c \"ALTER SYSTEM SET wal_level = replica\" -c \"ALTER SYSTEM SET max_wal_senders = 4\"")
        with open(os.path.join(self.pg.dir, "pg_hba.conf"), "a") as f:
            f.write("host replication all 127.0.0.1/32 trust\n")
        sh(f"su postgres -c '{self.pg.bin}/pg_ctl -D {self.pg.dir} restart -w -l {self.a.work}/pg.log'")
        shutil.rmtree(self.dir, ignore_errors=True)
        sh(f"su postgres -c '{self.pg.bin}/pg_basebackup -h 127.0.0.1 -p {self.a.pg_port} -U postgres "
           f"-D {self.dir} -R -X stream'")
        with open(os.path.join(self.dir, "postgresql.conf"), "a") as f:
            f.write(f"port = {self.port}\nshared_buffers = 1GB\n")
        sh(f"su postgres -c 'taskset -c {self.a.replica_cpus} {self.pg.bin}/pg_ctl -D {self.dir} "
           f"-l {self.a.work}/pgreplica.log start -w'")

    def sync(self, on):
        v = "'*'" if on else "''"
        sh(f"{self.pg.psql} -c \"ALTER SYSTEM SET synchronous_standby_names = {v}\" "
           f"-c \"SELECT pg_reload_conf()\"")
        time.sleep(1)

    def stop(self):
        sh(f"su postgres -c '{self.pg.bin}/pg_ctl -D {self.dir} stop -m fast'")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--server-cpus", default="0,1")
    ap.add_argument("--client-cpus", default="2,3")
    ap.add_argument("--replica-cpus", default="2")
    ap.add_argument("--build", default="build")
    ap.add_argument("--work", default="/tmp/snc-conventional")
    ap.add_argument("--pg-bin", default="/usr/lib/postgresql/16/bin")
    ap.add_argument("--pg-port", type=int, default=5440)
    ap.add_argument("--redis-port", type=int, default=6390)
    ap.add_argument("--workload", default="mixed")
    ap.add_argument("--only", default="snc,redis,postgres")
    ap.add_argument("--out", default="bench/results/replication_stack.csv")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    os.chmod(a.work, 0o777)
    only = a.only.split(",")
    rows = []

    def add(engine, mode, config, rep, ops, lat):
        rows.append(dict(engine=engine, replica=mode, config=config, rep=rep, ops_s=round(ops),
                         latency_us=round(lat, 1)))
        print(rows[-1], flush=True)

    if "snc" in only:
        port = 7800
        for rep in range(a.reps):
            for mode in ("none", "async", "sync"):
                port += 10
                ops, p50 = snc_run(a, mode, port)
                add("snc", mode, "net_bench 1 thread owner d512, p50", rep, ops, p50)

    if "redis" in only:
        rd = Redis(a)
        rd.start()
        try:
            rd.durable(True)
            for rep in range(a.reps):
                ops, p50 = rd.run(1000000, "mixed")
                add("redis", "none", "AOF always, p50", rep, ops, p50)
            rr = RedisReplica(a, rd)
            rr.start()
            try:
                for rep in range(a.reps):
                    ops, p50 = rd.run(1000000, "mixed")
                    add("redis", "async", "AOF always on both, p50", rep, ops, p50)
            finally:
                rr.stop()
        finally:
            rd.stop()

    if "postgres" in only:
        pg = Postgres(a)
        pg.start()
        try:
            pg.durable(True)
            script, clients, per = "pg_mixed_pipeline16.sql", 8, 16
            for rep in range(a.reps):
                ops, lat = pg.run(script, clients, a.seconds, per)
                add("postgres", "none", f"{script} -c {clients}, mean", rep, ops, lat)
            pr = PgReplica(a, pg)
            pr.start()
            try:
                for mode in ("async", "sync"):
                    pr.sync(mode == "sync")
                    for rep in range(a.reps):
                        ops, lat = pg.run(script, clients, a.seconds, per)
                        add("postgres", mode, f"{script} -c {clients}, mean", rep, ops, lat)
            finally:
                pr.stop()
        finally:
            pg.stop()

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print("\nmedians (ops/s):")
    keys = sorted({(r["engine"], r["replica"]) for r in rows}, key=lambda k: (k[0], ["none", "async", "sync"].index(k[1])))
    for k in keys:
        v = [r["ops_s"] for r in rows if (r["engine"], r["replica"]) == k]
        print(f"  {k[0]:9s} {k[1]:6s} {statistics.median(v):>10.0f}")


if __name__ == "__main__":
    main()
