#!/usr/bin/env python3
"""Same accounts workload on PostgreSQL and Redis (docs/CONVENTIONAL.md).

Servers run on --server-cpus, load generators on --client-cpus, as in
run_net_matrix.py. Each server gets 1M accounts. 50% reads (balance and
op count of one account), 50% deposits (balance + 1, op_count + 1,
updated_ns set, returning the new balance and op count). Durable means a
reply only after the write is fsynced: synchronous_commit/fsync on for
PostgreSQL, appendfsync always for Redis. Memory means those are off.

--workload transfer instead moves 1 cent between two random accounts per
op (docs/TRANSFERS.md): one UPDATE of both rows in PostgreSQL, one Lua
script in Redis.

Needs: postgresql (16), redis-server, pgbench, redis-benchmark. Run as root
(PostgreSQL runs as the postgres user).
"""
import argparse
import csv
import os
import re
import shutil
import statistics
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, check=True, capture_output=True, text=True, **kw).stdout


class Postgres:
    def __init__(self, a):
        self.a = a
        self.bin = a.pg_bin
        self.dir = os.path.join(a.work, "pgdata")
        self.psql = f"psql -q -h 127.0.0.1 -p {a.pg_port} -U postgres"

    def start(self):
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        shutil.chown(self.dir, "postgres")
        sh(f"su postgres -c '{self.bin}/initdb -D {self.dir} -A trust'")
        with open(os.path.join(self.dir, "postgresql.conf"), "a") as f:
            f.write(f"listen_addresses = '127.0.0.1'\nport = {self.a.pg_port}\nmax_connections = 300\n"
                    "shared_buffers = 2GB\nwal_buffers = 64MB\nmax_wal_size = 8GB\n"
                    "checkpoint_timeout = 30min\nunix_socket_directories = '/tmp'\n")
        sh(f"su postgres -c 'taskset -c {self.a.server_cpus} {self.bin}/pg_ctl -D {self.dir} "
           f"-l {self.a.work}/pg.log start -w'")
        sh(f"""{self.psql} <<'EOF'
CREATE EXTENSION pg_prewarm;
CREATE TABLE accounts (id bigint PRIMARY KEY, balance bigint NOT NULL, op_count bigint NOT NULL,
  created_ns bigint NOT NULL, updated_ns bigint NOT NULL, email text NOT NULL, name text NOT NULL)
  WITH (fillfactor = 80);
INSERT INTO accounts SELECT i, 1000, 0, 0, 0, 'user' || i || '@example.com', 'User ' || i
  FROM generate_series(1, 1000000) i;
CREATE INDEX accounts_email ON accounts (email);
VACUUM ANALYZE accounts;
SELECT pg_prewarm('accounts'), pg_prewarm('accounts_pkey');
EOF""")

    def durable(self, on):
        v = "on" if on else "off"
        sh(f"{self.psql} -c \"ALTER SYSTEM SET synchronous_commit = {v}\" -c \"ALTER SYSTEM SET fsync = {v}\" "
           f"-c \"ALTER SYSTEM SET full_page_writes = {v}\" -c \"SELECT pg_reload_conf()\"")
        sh(f"{self.psql} -c CHECKPOINT")
        time.sleep(1)

    def run(self, script, clients, seconds, per_script):
        out = sh(f"cd /tmp && taskset -c {self.a.client_cpus} pgbench -h 127.0.0.1 -p {self.a.pg_port} "
                 f"-U postgres -n -M {'extended' if per_script > 1 else 'prepared'} -c {clients} -j 2 "
                 f"-T {seconds} -D reads=50 -f {os.path.join(HERE, script)} postgres")
        tps = float(re.search(r"tps = ([\d.]+)", out)[1])
        lat = float(re.search(r"latency average = ([\d.]+) ms", out)[1])
        # Latency is per script run: one statement, or a whole pipeline.
        return tps * per_script, lat * 1000

    def stop(self):
        sh(f"su postgres -c '{self.bin}/pg_ctl -D {self.dir} stop -m fast'")


class Redis:
    SCRIPT = ("local b = redis.call('HINCRBY', KEYS[1], 'balance', 1); "
              "local c = redis.call('HINCRBY', KEYS[1], 'op_count', 1); "
              "redis.call('HSET', KEYS[1], 'updated_ns', ARGV[1]); return {b, c}")
    XFER = ("if tonumber(redis.call('HGET', KEYS[1], 'balance')) < 1 then return 0 end; "
            "redis.call('HINCRBY', KEYS[1], 'balance', -1); redis.call('HINCRBY', KEYS[1], 'op_count', 1); "
            "redis.call('HINCRBY', KEYS[2], 'balance', 1); redis.call('HINCRBY', KEYS[2], 'op_count', 1); "
            "return 1")

    def __init__(self, a):
        self.a = a
        self.cli = f"redis-cli -p {a.redis_port}"

    def start(self):
        d = os.path.join(self.a.work, "redisdata")
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        sh(f"taskset -c {self.a.server_cpus} redis-server --port {self.a.redis_port} --dir {d} --save '' "
           f"--appendonly no --io-threads 2 --io-threads-do-reads yes --daemonize yes "
           f"--logfile {self.a.work}/redis.log")
        time.sleep(1)
        gen = ("import sys\nw = sys.stdout.write\nfor i in range(0, 1000000):\n"
               "    p = ['HSET', 'acct:%012d' % i, 'balance', '1000', 'op_count', '0', 'created_ns', '0',\n"
               "         'updated_ns', '0', 'email', 'user%d@example.com' % i, 'name', 'User %d' % i]\n"
               "    w('*%d\\r\\n' % len(p) + ''.join('$%d\\r\\n%s\\r\\n' % (len(x), x) for x in p))\n")
        sh(f"python3 -c \"{gen}\" | {self.cli} --pipe")
        self.sha = sh(f"{self.cli} SCRIPT LOAD \"{self.SCRIPT}\"").strip()
        self.xfer_sha = sh(f"{self.cli} SCRIPT LOAD \"{self.XFER}\"").strip()

    def durable(self, on):
        if on:
            sh(f"{self.cli} CONFIG SET appendonly yes")
            while "aof_rewrite_in_progress:1" in sh(f"{self.cli} info persistence") or \
                    "aof_rewrite_scheduled:1" in sh(f"{self.cli} info persistence"):
                time.sleep(0.5)
            sh(f"{self.cli} CONFIG SET appendfsync always")
        else:
            sh(f"{self.cli} CONFIG SET appendonly no")

    def run(self, n_each, workload):
        # One load generator per client core: reads on one, deposits on the
        # other, so the server sees a 50/50 mix; or transfers on both.
        cpus = self.a.client_cpus.split(",")
        base = f"redis-benchmark -p {self.a.redis_port} -c 32 -P 16 -n {n_each} -r 1000000 -q"
        if workload == "transfer":
            cmds = [f"EVALSHA {self.xfer_sha} 2 acct:__rand_int__ acct:__rand_int__"] * 2
        else:
            cmds = ["HMGET acct:__rand_int__ balance op_count", f"EVALSHA {self.sha} 1 acct:__rand_int__ 7"]
        procs = [
            subprocess.Popen(f"taskset -c {cpus[0]} {base} {cmds[0]}",
                             shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True),
            subprocess.Popen(f"taskset -c {cpus[-1]} {base} {cmds[1]}",
                             shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True),
        ]
        total, p50 = 0.0, []
        for p in procs:
            out = p.communicate()[0].replace("\r", "\n")
            m = re.findall(r"([\d.]+) requests per second, p50=([\d.]+) msec", out)[-1]
            total += float(m[0])
            p50.append(float(m[1]) * 1000)
        return total, statistics.mean(p50)

    def stop(self):
        sh(f"{self.cli} shutdown nosave || true")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--server-cpus", default="0,1")
    ap.add_argument("--client-cpus", default="2,3")
    ap.add_argument("--work", default="/tmp/snc-conventional")
    ap.add_argument("--pg-bin", default="/usr/lib/postgresql/16/bin")
    ap.add_argument("--pg-port", type=int, default=5440)
    ap.add_argument("--redis-port", type=int, default=6390)
    ap.add_argument("--workload", choices=("mixed", "transfer"), default="mixed")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    if not a.out:
        a.out = "bench/results/conventional.csv" if a.workload == "mixed" else \
            "bench/results/conventional_transfer.csv"
    scripts = (("pg_mixed.sql", 16, 1), ("pg_mixed_pipeline16.sql", 8, 16)) if a.workload == "mixed" \
        else (("pg_transfer.sql", 16, 1), ("pg_transfer_pipeline16.sql", 8, 16))
    os.makedirs(a.work, exist_ok=True)
    os.chmod(a.work, 0o777)

    rows = []
    pg = Postgres(a)
    pg.start()
    try:
        for durable in (False, True):
            pg.durable(durable)
            for script, clients, per in scripts:
                for rep in range(a.reps):
                    ops, lat = pg.run(script, clients, a.seconds, per)
                    rows.append(dict(engine="postgres", mode="durable" if durable else "memory",
                                     config=f"{script} -c {clients}", rep=rep, ops_s=round(ops),
                                     latency_us=round(lat, 1)))
                    print(rows[-1], flush=True)
    finally:
        pg.stop()

    rd = Redis(a)
    rd.start()
    try:
        for durable in (False, True):
            rd.durable(durable)
            for rep in range(a.reps):
                ops, p50 = rd.run(4000000 if not durable else 1000000, a.workload)
                rows.append(dict(engine="redis", mode="durable" if durable else "memory",
                                 config="-c 32 -P 16 per generator, io-threads 2", rep=rep,
                                 ops_s=round(ops), latency_us=round(p50, 1)))
                print(rows[-1], flush=True)
    finally:
        rd.stop()

    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


if __name__ == "__main__":
    main()
