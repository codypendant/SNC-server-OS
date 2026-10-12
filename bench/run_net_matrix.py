#!/usr/bin/env python3
"""Runs the routing benchmark matrix (docs/ROUTING.md): snc_server and
baseline_server behind the same TCP front end, driven by net_bench from
separate cores, each configuration REPS times. Writes every run to a CSV.

The server gets --server-cpus, the client threads get --client-cpus, so the
load generator never shares a core with a shard.

Usage: bench/run_net_matrix.py [--build build] [--reps 3] [--seconds 4]
                               [--server-cpus 0,1] [--client-cpus 2,3]
                               [--data-dir /tmp/snc-net-matrix]
                               [--out bench/results/net_matrix.csv]
"""
import argparse, csv, os, re, shutil, signal, subprocess, sys, time

def parse(out):
    r = {}
    m = re.search(r"load: (\d+) objects in ([\d.]+)s \((\d+) creates/s\)", out)
    r.update(load_s=float(m[2]), creates_s=int(m[3]))
    m = re.search(r"ops/s (\d+)", out); r["ops_s"] = int(m[1])
    m = re.search(r"p50 (\d+)\s+p95 (\d+)\s+p99 (\d+)\s+p99.9 (\d+)", out)
    r.update(p50_ns=int(m[1]), p95_ns=int(m[2]), p99_ns=int(m[3]), p999_ns=int(m[4]))
    m = re.search(r"sent to non-owner ([\d.]+)%\s+forwarded by server ([\d.]+)%", out)
    r.update(misrouted_pct=float(m[1]), forwarded_pct=float(m[2]))
    return r

def wait_port(port, timeout=10):
    import socket
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    sys.exit(f"server did not open port {port}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=4)
    ap.add_argument("--server-cpus", default="0,1")
    ap.add_argument("--client-cpus", default="2,3")
    ap.add_argument("--data-dir", default="/tmp/snc-net-matrix")
    ap.add_argument("--out", default="bench/results/net_matrix.csv")
    ap.add_argument("--groups", default="", help="comma-separated groups to run (default: all)")
    a = ap.parse_args()
    n_client = len(a.client_cpus.split(","))

    # (group, engine, server flags, client flags)
    runs = []
    for reads in (50, 95):
        runs.append(("memory", "snc", "", f"--route owner --reads {reads} --depth 64"))
        runs.append(("memory", "snc", "", f"--route any --reads {reads} --depth 64"))
        runs.append(("memory", "baseline", "", f"--route any --reads {reads} --depth 64"))
    runs.append(("memory", "snc", "", "--route owner --misroute 100 --reads 50 --depth 64"))
    runs.append(("memory", "snc", "--no-mint-local", "--route owner --reads 50 --depth 64"))
    for reads in (50, 0):
        runs.append(("durable", "snc", "--data-dir D", f"--route owner --reads {reads} --depth 256"))
        runs.append(("durable", "snc", "--data-dir D", f"--route any --reads {reads} --depth 256"))
        n_srv = len(a.server_cpus.split(","))
        for logs in sorted({1, n_srv}):
            runs.append(("durable", "baseline", f"--data-dir D --logs {logs}",
                         f"--route any --reads {reads} --depth 256"))
    runs.append(("durable", "snc", "--data-dir D", "--route owner --misroute 100 --reads 50 --depth 256"))
    # Hot keys: Zipf key choice over all objects.
    for theta in (0.99, 1.5):
        for reads in (50, 0):
            runs.append(("skew", "snc", "", f"--route owner --zipf {theta} --reads {reads} --depth 64"))
            runs.append(("skew", "baseline", "", f"--route any --zipf {theta} --reads {reads} --depth 64"))
    runs.append(("skew-durable", "snc", "--data-dir D", "--route owner --zipf 1.2 --reads 50 --depth 256"))
    runs.append(("skew-durable", "baseline", f"--data-dir D --logs {n_srv}",
                 "--route any --zipf 1.2 --reads 50 --depth 256"))
    # Transfers between two random accounts (docs/TRANSFERS.md).
    runs.append(("transfer", "snc", "", "--route owner --transfer 100 --depth 64"))
    runs.append(("transfer", "baseline", "", "--route any --transfer 100 --depth 64"))
    runs.append(("transfer-durable", "snc", "--data-dir D", "--route owner --transfer 100 --depth 256"))
    runs.append(("transfer-durable", "baseline", f"--data-dir D --logs {n_srv}",
                 "--route any --transfer 100 --depth 256"))
    if a.groups:
        runs = [r for r in runs if r[0] in a.groups.split(",")]

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    fields = ["group", "engine", "server_flags", "client_flags", "rep", "ops_s", "p50_ns", "p95_ns",
              "p99_ns", "p999_ns", "misrouted_pct", "forwarded_pct", "load_s", "creates_s"]
    port = 7600
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for i, (group, engine, sflags, cflags) in enumerate(runs):
            for rep in range(a.reps):
                shutil.rmtree(a.data_dir, ignore_errors=True)
                port += 10  # fresh ports: no TIME_WAIT surprises
                binary = "snc_server" if engine == "snc" else "baseline_server"
                sargv = [os.path.join(a.build, binary), "--cpus", a.server_cpus, "--port", str(port),
                         "--objects", "1000000"]
                sargv += [a.data_dir if x == "D" else x for x in sflags.split()]
                srv = subprocess.Popen(sargv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                try:
                    wait_port(port)
                    cargv = [os.path.join(a.build, "net_bench"), "--port", str(port),
                             "--threads", str(n_client), "--cpus", a.client_cpus,
                             "--objects", "1000000", "--seconds", str(a.seconds)] + cflags.split()
                    out = subprocess.run(cargv, capture_output=True, text=True, timeout=600)
                finally:
                    srv.send_signal(signal.SIGTERM)
                    srv_out = srv.communicate(timeout=60)[0]
                if out.returncode != 0:
                    sys.exit(f"failed: {' '.join(cargv)}\n{out.stdout}\n{out.stderr}\n{srv_out}")
                row = dict(group=group, engine=engine, server_flags=sflags, client_flags=cflags,
                           rep=rep, **parse(out.stdout))
                w.writerow(row)
                f.flush()
                print(f"[{i + 1}/{len(runs)}] {engine} {sflags} {cflags} rep {rep}: "
                      f"{row['ops_s']} ops/s p99 {row['p99_ns'] / 1000:.0f} us", flush=True)
    shutil.rmtree(a.data_dir, ignore_errors=True)

if __name__ == "__main__":
    main()
