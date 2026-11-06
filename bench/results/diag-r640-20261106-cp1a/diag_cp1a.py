#!/usr/bin/env python3
"""CP-1A abort diagnosis: targeted characterization. NOT a pilot rerun.

Same binaries, CPUs, memory binding, objects, mix, route and depths as the
pilot's Profile A; adds time-series telemetry only (admin RESOURCES,
--report-ms, /proc/diskstats, iostat, pidstat). Cells are fixed below, run once
each, in order. Any failure stops the whole script. A no-checkpoint cell is
diagnostic only and cannot replace Profile A.

Usage: diag_cp1a.py OUT [--smoke]
"""
import json, os, shutil, socket, subprocess, sys, threading, time

BUILD = "build-pilot"
DATA = "/srv/snc-data/diag"
SRV_CPUS = "2,4,6,8,10,12,14,16"
CLI_CPUS = "18,20,22,24,26,28,30,32"
OBJECTS = 5000000
MIX = "read=70,deposit=15,transfer=10,index=5"
DEV = "sda"
# name, rate (None = closed loop), window s, checkpoint s (0 = never)
CELLS = [
    ("r2183233-w30-ckpt60", 2183233, 30, 60),    # ends before the first checkpoint
    ("r2183233-w60-ckpt60", 2183233, 60, 60),    # one checkpoint
    ("r2183233-w130-ckpt60", 2183233, 130, 60),  # two checkpoints
    ("r2183233-w130-nockpt", 2183233, 130, 0),   # diagnostic only
    ("r436646-w60-ckpt60", 436646, 60, 60),
    ("r436646-w60-nockpt", 436646, 60, 0),       # diagnostic only
    ("r218323-w60-ckpt60", 218323, 60, 60),
    ("closed-d512-w60-ckpt60", None, 60, 60),    # calibration semantics
]
SMOKE = [("smoke", 218323, 3, 60)]


def admin(port, line):
    with socket.create_connection(("127.0.0.1", port), timeout=1) as s:
        s.settimeout(1)
        s.sendall(line.encode() + b"\n")
        buf = b""
        while b"\n" not in buf:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        return buf.decode(errors="replace").strip()


def diskstats():
    with open("/proc/diskstats") as f:
        for l in f:
            p = l.split()
            if p[2] == DEV:
                return dict(w_ios=int(p[7]), w_sect=int(p[9]), w_ms=int(p[10]), inflight=int(p[11]),
                            io_ms=int(p[12]), wq_ms=int(p[13]), flush=int(p[18]) if len(p) > 18 else None,
                            flush_ms=int(p[19]) if len(p) > 19 else None)
    return {}


def sampler(port, path, stop, errs):
    os.sched_setaffinity(0, {0})
    with open(path, "w") as f:
        while not stop.is_set():
            t = time.time()
            rec = dict(t=round(t, 3), disk=diskstats())
            try:
                rec["res"] = admin(port, "RESOURCES")
            except OSError as e:
                errs.append(str(e))
                rec["err"] = str(e)
            f.write(json.dumps(rec) + "\n")
            f.flush()
            stop.wait(max(0.0, 0.1 - (time.time() - t)))


def wait_port(port):
    for _ in range(600):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit(f"port {port} never opened")


def run_cell(out, i, cell):
    name, rate, window, ckpt = cell
    port, aport = 23000 + 40 * i, 24000 + 40 * i
    data, ids = f"{DATA}/{name}", f"{DATA}/{name}.ids"
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(DATA, exist_ok=True)
    srv = [f"{BUILD}/snc_server", "--shards", "8", "--port", str(port), "--objects", str(OBJECTS * 2),
           "--cpus", SRV_CPUS, "--data-dir", data, "--admin-port", str(aport), "--report-ms", "1000"]
    if ckpt:
        srv += ["--checkpoint-s", str(ckpt)]
    bind_s = ["numactl", f"--physcpubind={SRV_CPUS}", "--membind=0"]
    cli = ["numactl", f"--physcpubind={CLI_CPUS}", "--membind=0", f"{BUILD}/net_bench", "--port", str(port),
           "--threads", "8", "--objects", str(OBJECTS), "--ids", ids, "--mix", MIX, "--route", "owner",
           "--cpus", CLI_CPUS]
    if rate:
        measure = cli + ["--rate", str(rate), "--depth", "1024", "--warmup", "10", "--seconds", str(window),
                         "--drain-s", "10"]
    else:
        measure = cli + ["--depth", "512", "--seconds", str(window)]
    base = f"{out}/{name}"
    slog = open(base + ".server.log", "w")
    t_start = time.time()
    p = subprocess.Popen(bind_s + srv, stdout=slog, stderr=subprocess.STDOUT)
    stop, errs, aux = threading.Event(), [], []
    th = None
    times = dict(server_start=t_start)
    try:
        wait_port(port)
        wait_port(aport)
        th = threading.Thread(target=sampler, args=(aport, base + ".samples.jsonl", stop, errs))
        th.start()
        for cmd, ext in ((["taskset", "-c", "36", "iostat", "-x", "-t", "1", DEV], "iostat"),
                         (["taskset", "-c", "36", "pidstat", "-t", "-p", str(p.pid), "1"], "pidstat")):
            aux.append(subprocess.Popen(cmd, stdout=open(f"{base}.{ext}.log", "w"), stderr=subprocess.STDOUT,
                                        env=dict(os.environ, S_TIME_FORMAT="ISO")))
        load = subprocess.run(cli + ["--depth", "64", "--seconds", "0"], capture_output=True, text=True, timeout=600)
        times["load_done"] = time.time()
        if load.returncode:
            raise SystemExit(f"{name}: load failed\n{load.stdout}\n{load.stderr}")
        times["run_start"] = time.time()
        run = subprocess.run(measure, capture_output=True, text=True, timeout=window + 660)
        times["run_end"] = time.time()
        with open(base + ".client.log", "w") as f:
            f.write(" ".join(bind_s + srv) + "\n" + " ".join(measure) + "\n--- load\n" + load.stdout + load.stderr
                    + "--- run\n" + run.stdout + run.stderr)
        if run.returncode or p.poll() is not None:
            raise SystemExit(f"{name}: run exit {run.returncode}, server poll {p.poll()}")
    finally:
        stop.set()
        if th:
            th.join()
        for a in aux:
            a.terminate()
            a.wait()
        p.terminate()
        try:
            p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        slog.close()
        times["server_exit"] = time.time()
        times["sampler_errors"] = len(errs)
        json.dump(times, open(base + ".times.json", "w"), indent=1)
        shutil.rmtree(data, ignore_errors=True)
        if os.path.exists(ids):
            os.remove(ids)
    print(f"{name}: done, sampler errors {len(errs)}", flush=True)


def main():
    out = sys.argv[1]
    smoke = "--smoke" in sys.argv
    os.makedirs(out, exist_ok=True)
    gov = open("/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor").read().strip()
    numa = open("/proc/sys/kernel/numa_balancing").read().strip()
    print(f"governor {gov} numa_balancing {numa}", flush=True)
    if gov != "performance" or numa != "0":
        raise SystemExit("settings differ from the pilot's")
    for i, c in enumerate(SMOKE if smoke else CELLS):
        run_cell(out, i, c)
    print("ALL CELLS DONE", flush=True)


main()
