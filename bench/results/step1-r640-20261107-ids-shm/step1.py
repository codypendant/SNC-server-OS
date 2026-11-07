#!/usr/bin/env python3
"""Runs PREREG.md: 3 reps, once each, then evaluates the outcome rule. Any failure stops."""
import gzip, hashlib, json, os, re, shlex, shutil, socket, subprocess, sys, threading, time
OUT = "bench/results/step1-r640-20261107-ids-shm"; OLD = "bench/results/diag2-r640-20261106-telemetry/raw"
DATA = "/srv/snc-data/step1"; SHM = "/dev/shm"; DEV = "sda"; BUILD = "build-pilot"
SRV = "2,4,6,8,10,12,14,16"; CLI = "18,20,22,24,26,28,30,32"; OBJ = 5000000; MIX = "read=70,deposit=15,transfer=10,index=5"
RATE = 2183233; WIN = 30; CKPT = 60; BASEPORT = 25600
SHA = {"snc_server": "04030ccb310bb99beddf9ef166f37f6f430ab6daa3aa2d758887112f5bad3226",
       "net_bench": "a5f085d4dcfa4cb2a272eb95987e4626888c1a44307a67e4b56cf3b61d4f3863"}

def bind(c): return ["numactl", f"--physcpubind={c}", "--membind=0"]

def cmds(name, p):
    data, ids = f"{DATA}/{name}", f"{SHM}/step1-{name}.ids"
    srv = bind(SRV) + [f"{BUILD}/snc_server", "--shards", "8", "--port", str(p), "--objects", str(OBJ * 2), "--cpus", SRV, "--data-dir", data, "--checkpoint-s", str(CKPT)]
    cli = bind(CLI) + [f"{BUILD}/net_bench", "--port", str(p), "--threads", "8", "--objects", str(OBJ), "--ids", ids, "--mix", MIX, "--route", "owner", "--cpus", CLI]
    run = cli + ["--rate", str(RATE), "--depth", "1024", "--warmup", "10", "--seconds", str(WIN), "--drain-s", "10"]
    return srv, cli, run, data, ids

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
                if p[2] == DEV: rows.append(f"{time.monotonic_ns()},{p[7]},{p[9]},{p[10]},{p[11]},{p[12]},{p[13]},{p[18]},{p[19]}"); break
        stop.wait(0.1)
    with gzip.open(path, "wt") as f:
        f.write("t_ns,w_ios,w_sect,w_ms,inflight,io_ms,wq_ms,flush,flush_ms\n" + "\n".join(rows) + "\n")

def preflight():
    g = {open(f"/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_governor").read().strip() for i in range(72)}
    nb = open("/proc/sys/kernel/numa_balancing").read().strip()
    if g != {"performance"} or nb != "0": raise SystemExit(f"settings differ: {g} {nb}")
    src = subprocess.run(["findmnt", "-no", "SOURCE", "/srv/snc-data"], capture_output=True, text=True).stdout.strip()
    if src != f"/dev/{DEV}1": raise SystemExit(f"data device is {src}")
    for b, want in SHA.items():
        if hashlib.sha256(open(f"{BUILD}/{b}", "rb").read()).hexdigest() != want: raise SystemExit(f"{b} hash differs")
    st = os.statvfs(SHM)
    if st.f_bavail * st.f_frsize < 200 << 20: raise SystemExit("/dev/shm under 200 MB free")
    if subprocess.run(["pgrep", "-x", "snc_server|net_bench"], capture_output=True).returncode == 0: raise SystemExit("snc_server or net_bench already running")
    print("preflight ok", flush=True)

def rep(i):
    name = f"S-w30-r{i}"; p = BASEPORT + 40 * i; b = f"{OUT}/raw/{name}"
    srv, cli, run_cmd, data, ids = cmds(name, p)
    shutil.rmtree(data, ignore_errors=True); os.makedirs(DATA, exist_ok=True)
    if os.path.exists(ids): os.remove(ids)
    slog = open(b + ".server.log", "w"); P = subprocess.Popen(srv, stdout=slog, stderr=subprocess.STDOUT)
    stop = threading.Event(); th = None; T = {}
    try:
        wait_port(p)
        th = threading.Thread(target=disk, args=(b + ".disk.csv.gz", stop)); th.start()
        T["server_up"] = time.monotonic_ns()
        load = subprocess.run(cli + ["--depth", "64", "--seconds", "0"], capture_output=True, text=True, timeout=600)
        if load.returncode: raise SystemExit(f"{name}: load failed {load.stdout}{load.stderr}")
        if not os.path.getsize(ids) == OBJ * 16: raise SystemExit(f"{name}: ids file wrong size")
        T["run_start"] = time.monotonic_ns()
        run = subprocess.run(run_cmd, capture_output=True, text=True, timeout=WIN + 660)
        T["run_end"] = time.monotonic_ns()
        open(b + ".client.log", "w").write(shlex.join(srv) + "\n" + shlex.join(run_cmd) + "\n--- load\n" + load.stdout + load.stderr + "--- run\n" + run.stdout + run.stderr)
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
    print(name, "done", flush=True)

def metrics(base, run_only=True):
    log = open(base + ".client.log").read().split("--- run\n", 1)[1]; g = lambda r: re.search(r, log)
    t = g(r"total latency ns\s+n \d+\s+p50 \d+\s+p95 \d+\s+p99 (\d+)\s+p99.9 (\d+)\s+max (\d+)")
    m = dict(exec_refused=int(g(r"overloaded (\d+)")[1]), index_refused=int(g(r"refused (\d+)")[1]), p99_ns=int(t[1]), p999_ns=int(t[2]), max_ns=int(t[3]),
             achieved=int(g(r"achieved (\d+) ops/s")[1]), late_over_1ms=int(g(r"late_over_1ms (\d+)")[1]), scheduled_in_window=int(g(r"scheduled_in_window (\d+)")[1]))
    srv = open(base + ".server.log").read()
    m["checkpoints"] = len(re.findall(r"^checkpoint:.*$", srv, re.M))
    return m

def storage(base):
    tm = json.load(open(base + ".times.json")); rows = gzip.open(base + ".disk.csv.gz", "rt").read().splitlines()
    h = rows[0].split(","); R = [dict(zip(h, l.split(","))) for l in rows[1:]]; A = []
    for a, c in zip(R, R[1:]):
        dw = int(c["w_ios"]) - int(a["w_ios"])
        if dw: A.append(((int(c["t_ns"]) - tm["run_start"]) / 1e9, (int(c["w_ms"]) - int(a["w_ms"])) / dw))
    return dict(samples=len(A), max_await_ms=round(max(x[1] for x in A), 3), events=[(round(t, 2), round(v, 2)) for t, v in A if v > 1.0])

def evaluate():
    res = {}
    for i in range(3):
        b = f"{OUT}/raw/S-w30-r{i}"; res[f"r{i}"] = {**metrics(b), **storage(b)}
    clean = all(r["exec_refused"] + r["index_refused"] == 0 and r["late_over_1ms"] == 0 and not r["events"] for r in res.values())
    out = dict(reps=res, verdict="ELIMINATED" if clean else "REJECTED", confounded=[k for k, r in res.items() if r["checkpoints"]])
    json.dump(out, open(f"{OUT}/summary.json", "w"), indent=1); print(json.dumps(out, indent=1), flush=True)

def dry_run():
    norm = lambda s: re.sub(r"--port \d+", "--port P", re.sub(r"/srv/snc-data/\w+/[\w-]+?(\.ids)?(?= |$)", lambda m: "IDS" if m[1] else "DATA", re.sub(r"/dev/shm/[\w.-]+\.ids", "IDS", s)))
    srv, _, run_cmd, _, _ = cmds("S-w30-r0", BASEPORT)
    old = open(f"{OLD}/B-w30-r0.client.log").read().splitlines()[:2]
    for label, new, was in (("server", shlex.join(srv), old[0]), ("client", shlex.join(run_cmd), old[1])):
        print(label, "identical after normalising port/paths:", norm(new) == norm(was))
        if norm(new) != norm(was): print(" new", norm(new), "\n old", norm(was)); raise SystemExit(1)
    print("new client:", shlex.join(run_cmd))

if "--dry-run" in sys.argv: dry_run()
elif "--evaluate" in sys.argv: evaluate()
else:
    preflight(); os.makedirs(OUT + "/raw", exist_ok=True)
    if os.listdir(OUT + "/raw"): raise SystemExit("raw/ not empty: no reruns")
    for i in range(3): rep(i)
    evaluate(); print("ALL DONE", flush=True)
