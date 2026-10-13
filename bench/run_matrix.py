#!/usr/bin/env python3
"""Runs the step 12 benchmark matrix: snc_bench and baseline_bench under the
same workloads, each configuration REPS times, and writes every run to a CSV.

Usage: bench/run_matrix.py [--build build] [--reps 3] [--seconds 4]
                           [--data-dir /tmp/snc-matrix] [--out bench/results/matrix.csv]
"""
import argparse, csv, os, re, shutil, subprocess, sys

def parse(out):
    r = {}
    m = re.search(r"load: (\d+) objects in ([\d.]+)s .*?RSS ([\d.]+) B/object", out)
    if m: r.update(load_s=float(m[2]), rss_per_obj=float(m[3]))
    m = re.search(r"ops/s (\d+)", out); r["ops_s"] = int(m[1])
    m = re.search(r"p50 (\d+)\s+p95 (\d+)\s+p99 (\d+)\s+p99.9 (\d+)", out)
    r.update(p50_ns=int(m[1]), p95_ns=int(m[2]), p99_ns=int(m[3]), p999_ns=int(m[4]))
    m = re.search(r"pages: asked \w+, map (\w+), heap (\w+)", out)
    if m: r["pages_got"] = f"map {m[1]} heap {m[2]}"
    m = re.search(r"cross-shard msgs/op ([\d.]+)", out)
    if m: r["msgs_per_op"] = float(m[1])
    m = re.search(r"contended stripe locks ([\d.]+)%", out)
    if m: r["contended_pct"] = float(m[1])
    m = re.search(r"recovery: \d+ objects .* in ([\d.]+)s: checkpoint load ([\d.]+)s, WAL replay ([\d.]+)s", out)
    if m: r.update(recovery_s=float(m[1]), ckpt_load_s=float(m[2]), wal_replay_s=float(m[3]))
    m = re.search(r"checkpoint: \d+ objects, ([\d.]+) MiB in ([\d.]+)s", out)
    if m: r.update(checkpoint_s=float(m[2]))
    return r

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=4)
    ap.add_argument("--data-dir", default="/tmp/snc-matrix")
    ap.add_argument("--out", default="bench/results/matrix.csv")
    ap.add_argument("--seastar-build", default="build-seastar",
                    help="build dir with snc_bench_seastar (-DSNC_SEASTAR=ON), for the seastar group")
    ap.add_argument("--groups", default="", help="comma-separated groups to run (default: all)")
    a = ap.parse_args()

    runs = []  # (group, engine, flags)
    for engine in ("snc", "baseline"):
        for remote in (0, 50, 100):
            for reads in (50, 95):
                runs.append(("memory", engine, f"--remote {remote} --reads {reads} --depth 32"))
        for n in (1, 2):
            runs.append(("scaling", engine, f"--{'shards' if engine == 'snc' else 'threads'} {n} --remote 50 --reads 50 --depth 32"))
    for remote in (0, 50, 100):
        runs.append(("durable", "snc", f"--remote {remote} --reads 50 --depth 256 --data-dir D"))
        for logs in (1, 4):
            runs.append(("durable", "baseline", f"--remote {remote} --reads 50 --depth 256 --data-dir D --logs {logs}"))
    runs.append(("durable", "snc", "--remote 0 --reads 0 --depth 256 --data-dir D"))
    runs.append(("durable", "baseline", "--remote 0 --reads 0 --depth 256 --data-dir D --logs 4"))
    # Hot keys: Zipf key choice over all objects (--remote no longer applies).
    for theta in (0.99, 1.2, 1.5):
        for reads in (50, 0):
            runs.append(("skew", "snc", f"--zipf {theta} --reads {reads} --depth 32"))
            runs.append(("skew", "baseline", f"--zipf {theta} --reads {reads} --depth 32"))
    for theta in (0.99, 1.5):
        runs.append(("skew-durable", "snc", f"--zipf {theta} --reads 50 --depth 256 --data-dir D"))
        runs.append(("skew-durable", "baseline", f"--zipf {theta} --reads 50 --depth 256 --data-dir D --logs 4"))
    # Transfers between two random accounts (docs/TRANSFERS.md).
    for engine in ("snc", "baseline"):
        for pct in (100, 20):
            runs.append(("transfer", engine, f"--transfer {pct} --reads 50 --depth 32"))
    runs.append(("transfer-durable", "snc", "--transfer 100 --depth 256 --data-dir D"))
    runs.append(("transfer-durable", "baseline", "--transfer 100 --depth 256 --data-dir D --logs 4"))
    # Cross-core ops at increasing pipeline depth, with and without batched
    # ring messages (docs/CROSSCORE.md).
    for depth in (32, 128, 512):
        runs.append(("remote-depth", "snc", f"--remote 100 --reads 50 --depth {depth}"))
        runs.append(("remote-depth", "snc", f"--remote 100 --reads 50 --depth {depth} --no-batch"))
        runs.append(("remote-depth", "baseline", f"--remote 100 --reads 50 --depth {depth}"))
    # Share of transfers whose destination is on the source's shard (one op,
    # no follow-ups); the baseline has no notion of placement.
    for local in (0, 50, 90, 100):
        runs.append(("transfer-local", "snc", f"--transfer 100 --xfer-local {local} --depth 32"))
        runs.append(("transfer-local-durable", "snc",
                     f"--transfer 100 --xfer-local {local} --depth 256 --data-dir D"))
    # Page size for the object map and heap (docs/HUGEPAGES.md). The baseline
    # has no page option, so it runs with system-wide THP instead.
    pages_loads = ["--remote 0 --reads 50 --depth 256", "--remote 100 --reads 50 --depth 128",
                   "--transfer 100 --reads 50 --depth 32",
                   "--remote 0 --reads 50 --depth 256 --data-dir D"]
    for load in pages_loads:
        for mode in ("4k", "thp", "hugetlb"):
            runs.append(("pages", "snc", f"{load} --pages {mode}"))
        for thp in ("madvise", "always"):
            logs = " --logs 4" if "--data-dir" in load else ""
            runs.append(("pages", "baseline", f"{load}{logs} THP={thp}"))
    # The same SNC core on the Seastar backend (docs/SEASTAR.md). One runtime
    # per process there, so durable runs skip the timed restart on both.
    ss_loads = ["--remote 0 --reads 50 --depth 256", "--remote 100 --reads 50 --depth 32",
                "--remote 100 --reads 50 --depth 128", "--remote 100 --reads 50 --depth 512",
                "--transfer 100 --reads 50 --depth 32",
                "--remote 0 --reads 50 --depth 256 --data-dir D --no-restart",
                "--remote 100 --reads 50 --depth 256 --data-dir D --no-restart"]
    for load in ss_loads:
        for engine in ("snc", "snc-seastar", "snc-seastar-rings"):
            runs.append(("seastar", engine, load))
    runs.append(("recovery", "snc", "--remote 0 --reads 50 --depth 256 --data-dir D --no-checkpoint"))

    if a.groups:
        runs = [r for r in runs if r[0] in a.groups.split(",")]

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    fields = ["group", "engine", "flags", "rep", "ops_s", "p50_ns", "p95_ns", "p99_ns", "p999_ns",
              "msgs_per_op", "contended_pct", "pages_got", "load_s", "rss_per_obj", "checkpoint_s",
              "recovery_s", "ckpt_load_s", "wal_replay_s"]
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for i, (group, engine, flags) in enumerate(runs):
            for rep in range(a.reps):
                shutil.rmtree(a.data_dir, ignore_errors=True)
                binary = {"snc": os.path.join(a.build, "snc_bench"),
                          "baseline": os.path.join(a.build, "baseline_bench"),
                          "snc-seastar": os.path.join(a.seastar_build, "snc_bench_seastar"),
                          "snc-seastar-rings": os.path.join(a.seastar_build, "snc_bench_seastar")}[engine]
                env = dict(os.environ, SNC_SEASTAR_MSG="rings" if engine == "snc-seastar-rings" else "smp")
                argv = [binary, "--objects", "1000000", "--seconds", str(a.seconds)]
                argv += [a.data_dir if x == "D" else x for x in flags.split() if not x.startswith("THP=")]
                set_thp(re.search(r"THP=(\w+)", flags)[1] if "THP=" in flags else "madvise")
                if "hugetlb" in flags: set_hugepages(1100)
                out = subprocess.run(argv, capture_output=True, text=True, timeout=600, env=env)
                if out.returncode != 0:
                    sys.exit(f"failed: {' '.join(argv)}\n{out.stdout}\n{out.stderr}")
                row = dict(group=group, engine=engine, flags=flags, rep=rep, **parse(out.stdout))
                w.writerow(row)
                f.flush()
                print(f"[{i + 1}/{len(runs)}] {engine} {flags} rep {rep}: {row['ops_s']} ops/s", flush=True)
    shutil.rmtree(a.data_dir, ignore_errors=True)
    set_thp("madvise")

THP = "/sys/kernel/mm/transparent_hugepage/enabled"

def set_thp(mode):
    """System-wide transparent huge pages (needs root; skipped otherwise)."""
    try:
        with open(THP, "w") as f: f.write(mode)
    except OSError:
        pass

def set_hugepages(n):
    """Reserves n 2 MiB pages for --pages hugetlb (needs root); without them
    SNC falls back to THP and says so in its output."""
    try:
        with open("/proc/sys/vm/nr_hugepages") as f:
            if int(f.read()) >= n: return
        with open("/proc/sys/vm/nr_hugepages", "w") as f: f.write(str(n))
    except OSError:
        pass

if __name__ == "__main__":
    main()
