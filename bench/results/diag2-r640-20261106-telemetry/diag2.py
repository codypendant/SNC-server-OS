#!/usr/bin/env python3
"""Runs PREREG.md: tests, proof, gates, then causal cells only if all gates pass. Stops on any failure."""
import gzip, json, os, re, shutil, socket, statistics as st, subprocess, sys, threading, time
OUT = sys.argv[1]; SMOKE = "--smoke" in sys.argv
DATA = "/srv/snc-data/diag2"; SRV = "2,4,6,8,10,12,14,16"; CLI = "18,20,22,24,26,28,30,32"
OBJ = 5000000; MIX = "read=70,deposit=15,transfer=10,index=5"; port = [25000]
res = []

def wait_port(p):
    for _ in range(600):
        try: socket.create_connection(("127.0.0.1", p), timeout=1).close(); return
        except OSError: time.sleep(0.1)
    raise SystemExit(f"port {p} never opened")

def disk(path, stop):
    os.sched_setaffinity(0, {36}); rows = []
    while not stop.is_set():
        with open("/proc/diskstats") as f:
            for l in f:
                p = l.split()
                if p[2] == "sda": rows.append(f"{time.monotonic_ns()},{p[7]},{p[9]},{p[10]},{p[11]},{p[12]},{p[13]},{p[18]},{p[19]}"); break
        stop.wait(0.1)
    with gzip.open(path, "wt") as f:
        f.write("t_ns,w_ios,w_sect,w_ms,inflight,io_ms,wq_ms,flush,flush_ms\n" + "\n".join(rows) + "\n")

def parse(run):
    g = lambda p: re.search(p, run)
    t = g(r"total latency ns\s+n \d+\s+p50 \d+\s+p95 \d+\s+p99 (\d+)\s+p99.9 (\d+)\s+max (\d+)")
    return dict(total=int(g(r"total ops/s (\d+)")[1]), over=int(g(r"overloaded (\d+)")[1]), idx=int(g(r"refused (\d+)")[1]),
                p99=int(t[1]), p999=int(t[2]), maxl=int(t[3]))

def cell(arm, kind, rep, rate, win, ckpt):
    name = f"{arm}-{kind}-r{rep}"; port[0] += 40; p = port[0]; b = f"{OUT}/raw/{name}"
    build = "build-pilot" if arm == "B" else "build-diag"
    data, ids = f"{DATA}/{name}", f"{DATA}/{name}.ids"
    shutil.rmtree(data, ignore_errors=True); os.makedirs(DATA, exist_ok=True)
    srv = [f"{build}/snc_server", "--shards", "8", "--port", str(p), "--objects", str(OBJ * 2), "--cpus", SRV, "--data-dir", data]
    if ckpt: srv += ["--checkpoint-s", str(ckpt)]
    if arm == "T": srv += ["--diag-gauges", b + ".gauges.csv", "--diag-cpu", "0"]
    bind = lambda c: ["numactl", f"--physcpubind={c}", "--membind=0"]
    cli = bind(CLI) + [f"{build}/net_bench", "--port", str(p), "--threads", "8", "--objects", str(OBJ), "--ids", ids, "--mix", MIX, "--route", "owner", "--cpus", CLI]
    m = cli + (["--rate", str(rate), "--depth", "1024", "--warmup", "10", "--seconds", str(win), "--drain-s", "10"] if rate else ["--depth", "512", "--seconds", str(win)])
    if arm == "T": m += ["--diag-buckets", b + ".buckets.csv"]
    slog = open(b + ".server.log", "w"); P = subprocess.Popen(bind(SRV) + srv, stdout=slog, stderr=subprocess.STDOUT)
    stop = threading.Event(); th = None; T = {}
    try:
        wait_port(p)
        if arm == "T": th = threading.Thread(target=disk, args=(b + ".disk.csv.gz", stop)); th.start()
        T["server_up"] = time.monotonic_ns()
        load = subprocess.run(cli + ["--depth", "64", "--seconds", "0"], capture_output=True, text=True, timeout=600)
        if load.returncode: raise SystemExit(f"{name}: load failed {load.stdout}{load.stderr}")
        T["run_start"] = time.monotonic_ns()
        run = subprocess.run(m, capture_output=True, text=True, timeout=win + 660)
        T["run_end"] = time.monotonic_ns()
        open(b + ".client.log", "w").write(" ".join(bind(SRV) + srv) + "\n" + " ".join(m) + "\n--- load\n" + load.stdout + load.stderr + "--- run\n" + run.stdout + run.stderr)
        if run.returncode or P.poll() is not None: raise SystemExit(f"{name}: exit {run.returncode}, server {P.poll()}")
    finally:
        stop.set()
        if th: th.join()
        P.terminate()
        try: P.wait(timeout=120)
        except subprocess.TimeoutExpired: P.kill(); P.wait()
        slog.close(); json.dump(T, open(b + ".times.json", "w"))
        shutil.rmtree(data, ignore_errors=True)
        if os.path.exists(ids): os.remove(ids)
    for f in (b + ".gauges.csv", b + ".buckets.csv"):
        if os.path.exists(f):
            with open(f, "rb") as i, gzip.open(f + ".gz", "wb") as o: shutil.copyfileobj(i, o)
            os.remove(f)
    r = dict(arm=arm, kind=kind, rep=rep, **parse(run.stdout)); res.append(r)
    json.dump(res, open(f"{OUT}/summary.json", "w"), indent=1); print(json.dumps(r), flush=True); return r

def med(a, k, f): return st.median(x[f] for x in res if x["arm"] == a and x["kind"] == k)

def main():
    os.makedirs(OUT + "/raw", exist_ok=True)
    gov = open("/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor").read().strip(); nb = open("/proc/sys/kernel/numa_balancing").read().strip()
    print(f"governor {gov} numa_balancing {nb}", flush=True)
    if gov != "performance" or nb != "0": raise SystemExit("settings differ from the pilot's")
    if SMOKE:
        cell("T", "smoke", 0, 218323, 3, 60); return
    t = subprocess.run(["ctest", "--test-dir", "build-diag", "-j", "8", "--timeout", "600"], capture_output=True, text=True)
    open(f"{OUT}/ctest-build-diag.log", "w").write(t.stdout + t.stderr)
    print("ctest exit", t.returncode, t.stdout.strip().splitlines()[-3:] if t.stdout else "", flush=True)
    if t.returncode: raise SystemExit("STOP: tests failed on build-diag")
    for rep in range(3):
        for arm in "BT":
            cell(arm, "closed", rep, None, 60, 60); cell(arm, "w30", rep, 2183233, 30, 60)
    bw = [x for x in res if x["arm"] == "B" and x["kind"] == "w30"]; tw = [x for x in res if x["arm"] == "T" and x["kind"] == "w30"]
    G = dict(
        G0=abs(med("B", "closed", "total") / 4366466 - 1) <= 0.05 and med("B", "w30", "p99") <= 1.5 * 1048576 and all(x["over"] + x["idx"] == 0 for x in bw),
        G1=med("T", "closed", "total") >= 0.97 * med("B", "closed", "total"),
        G2=all(x["over"] + x["idx"] == 0 for x in tw),
        G3=all(med("T", "w30", f) <= max(1.25 * med("B", "w30", f), max(x[f] for x in bw)) for f in ("p99", "p999")),
        G4=med("T", "w30", "total") >= 0.995 * med("B", "w30", "total"))
    json.dump(G, open(f"{OUT}/gates.json", "w"), indent=1); print("GATES", G, flush=True)
    if not all(G.values()): print("STOP: gate failed, no causal cells run", flush=True); return
    for rep in range(3):
        cell("T", "r436646-ckpt", rep, 436646, 60, 60); cell("T", "r436646-nockpt", rep, 436646, 60, 0); cell("T", "r2183233-w60", rep, 2183233, 60, 60)
    print("ALL DONE", flush=True)
main()
