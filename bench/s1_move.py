#!/usr/bin/env python3
"""The S1 evidence run (docs/MIGRATION_STAGING.md 18): one slot move of N
accounts between two groups on this machine, with the final stage's delta
made by writers during the copy ahead, measured.

Real processes: tests/test_s1.py's cluster (three witnesses, two groups of
a primary and an async standby; snc_ctl drives the move). SNC_TRACE=1 on
every node. During the move:

  - HEALTH and STATUS at B (the destination's primary), every 5 ms, each
    on its own thread (HEALTH's stall_ms: the longest a shard has not
    progressed, as sampled);
  - a foreground probe: a durable deposit into one of 16 accounts B serves
    (other slots, both shards), one at a time, every 2 ms;
  - a client probe reading an account of S from A, and from B once A says
    E_MOVED, every 2 ms: the client-visible E_MIGRATING interval.

From the node traces: the final stage's span (stage_begin/stage_end
final=1 at B), every unit (stage_unit), the seal at A, slot_active at B,
and B's shard iterations of 20 ms or more (shard_long_iter). From the
chain's report line: units, unit p50/p95/p99/max, puts, window high-water.
WAL bytes of the final stage: the chain's `bytes` (record headers and payloads it
appended; the async standby receives the same records). B's VmHWM.

  s1_move.py BUILD [--objects N] [--email shared|unique] [--units on|off]
             [--unit-us N] [--unit-bytes N] [--window-bytes N] [--writers K]
             [--chunk N] [--changed N] [--json OUT]

Prints one JSON object (and writes it to --json).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests"))

ap = argparse.ArgumentParser()
ap.add_argument("build")
ap.add_argument("--objects", type=int, default=100000)
ap.add_argument("--email", choices=["shared", "unique"], default="shared")
ap.add_argument("--units", choices=["on", "off"], default="on")
ap.add_argument("--unit-us", type=int, default=1000)
ap.add_argument("--unit-bytes", type=int, default=1 << 20)
ap.add_argument("--window-bytes", type=int, default=8 << 20)
ap.add_argument("--writers", type=int, default=4)
ap.add_argument("--changed", type=int, default=18000,
                help="the writers deposit into the first N accounts only (the baseline final changed 17,874)")
ap.add_argument("--chunk", type=int, default=256)
ap.add_argument("--json")
ap.add_argument("--keep", action="store_true", help="keep the run's directory (logs and traces)")
args = ap.parse_args()
sys.argv = [sys.argv[0], args.build]  # test_fail_closed reads BUILD from argv
os.environ["SNC_TRACE"] = "1"

import test_fail_closed as fc  # noqa: E402
import test_s1  # noqa: E402
from test_fail_closed import OK, E_MIGRATING, E_MOVED, balance, create_here, deposit, owner_primary  # noqa: E402


def pct(v, q):
    v = sorted(v)
    return round(v[min(len(v) - 1, int(q * len(v)))], 3) if v else None


def stats(v):
    return {"n": len(v), "p50": pct(v, .5), "p95": pct(v, .95), "p99": pct(v, .99), "max": pct(v, 1)}


def traces(path, event):
    """(ns, {k: v}) of every trace line of `event` in a node log."""
    out = []
    for line in open(path, errors="replace"):
        if not line.startswith("T "):
            continue
        f = line.split()
        if len(f) < 4 or f[3] != event:
            continue
        kv = dict(x.split("=", 1) for x in f[4:] if "=" in x)
        out.append((int(f[1]), kv))
    return out


def main():
    extra = ["--objects", str(args.objects * 3 + 100000), "--move-units", args.units,
             "--move-unit-us", str(args.unit_us), "--move-unit-bytes", str(args.unit_bytes),
             "--move-window-bytes", str(args.window_bytes)]
    work = tempfile.mkdtemp(prefix="snc-s1move-")
    cls = type("Bench", (test_s1.S1Cluster,), {"extra_all": extra})
    c = cls(work)
    res = {"args": vars(args), "work": work}
    try:
        ids_file = f"{work}/ids"
        r = subprocess.run([f"{args.build}/snc_load", "create", "--connect", f"127.0.0.1:{c.client[1]}",
                            "--accounts", "1", "--in-slot", str(args.objects - 1), "--email", args.email,
                            "--ids", ids_file], capture_output=True, text=True, timeout=1800)
        assert r.returncode == 0, r.stdout + r.stderr
        S = int(r.stdout.rsplit(" in slot ", 1)[1])
        ids = open(ids_file).read().split()
        a = owner_primary(c, S)
        b = 3 if a == 1 else 1
        qb = c.peer(b)
        res.update(slot=S, slot_objects=len(ids), source=a, dest=b)
        fg = [create_here(c.client[b]) for _ in range(16)]
        done = threading.Event()
        lat = {"HEALTH": [], "STATUS": [], "fg": [], "stall_ms": []}

        def sampler(verb):
            # One thread per verb: a slow STATUS must not delay HEALTH's samples.
            while not done.is_set():
                t = time.monotonic_ns()
                rep = c.ask(b, verb, 10)
                if rep:
                    lat[verb].append((t, (time.monotonic_ns() - t) / 1e6))
                    if verb == "HEALTH" and " stall_ms=" in rep:
                        lat["stall_ms"].append((t, int(rep.split(" stall_ms=")[1].split()[0])))
                done.wait(0.005)

        def foreground():
            k = 0
            while not done.is_set():
                t = time.monotonic_ns()
                st = deposit(c.client[b], fg[k % len(fg)], 1)
                if st == OK:
                    lat["fg"].append((t, (time.monotonic_ns() - t) / 1e6))
                k += 1
                done.wait(0.002)

        visible = []  # (ns, status) of reads of S

        def client_probe():
            target = ids[0]
            while not done.is_set():
                t = time.monotonic_ns()
                st, _ = balance(c.client[a], target)
                if st == E_MOVED:
                    st, _ = balance(c.client[b], target)
                visible.append((t, st))
                done.wait(0.002)

        stop_writers = threading.Event()
        changed = [0] * args.writers

        def writer(w):
            port = c.client[a]
            k = w
            while not stop_writers.is_set():
                st = deposit(port, ids[k % min(len(ids), args.changed)], 1)
                if st == OK:
                    changed[w] += 1
                elif st == E_MIGRATING:
                    return
                k += args.writers

        threads = [threading.Thread(target=sampler, args=(v,), daemon=True) for v in ("HEALTH", "STATUS")]
        threads += [threading.Thread(target=f, daemon=True) for f in (foreground, client_probe)]
        threads += [threading.Thread(target=writer, args=(w,), daemon=True) for w in range(args.writers)]
        for t in threads:
            t.start()
        time.sleep(0.5)
        t_move = time.monotonic_ns()
        p, out = test_s1.ctl_bg(c, "move", "--chunk", args.chunk, S, c.group_of(b))
        while p.poll() is None:
            time.sleep(0.02)
        t_end = time.monotonic_ns()
        stop_writers.set()
        time.sleep(0.3)
        done.set()
        for t in threads:
            t.join(timeout=10)
        r_out, r_err = test_s1.ctl_result(p, out)
        res["move"] = r_out
        res["move_s"] = round((t_end - t_move) / 1e9, 3)
        res["writer_deposits_ok"] = sum(changed)
        blog, alog = f"{work}/n{b}.log", f"{work}/n{a}.log"
        begin = [x for x in traces(blog, "stage_begin") if x[1].get("final") == "1"]
        end = [x for x in traces(blog, "stage_end") if x[1].get("final") == "1"]
        seal = [x for x in traces(alog, "seal") if x[1].get("slot") == str(S)]
        active = [x for x in traces(blog, "slot_active") if x[1].get("slot") == str(S)]
        if begin and end:
            fb, fe = begin[-1][0], end[-1][0]
            res["final_stage_ms"] = round((fe - fb) / 1e6, 1)
            res["final_stage_end"] = end[-1][1]
        else:
            fb, fe = t_move, t_end
        if seal and active:
            res["e_migrating_trace_ms"] = round((active[-1][0] - seal[-1][0]) / 1e6, 1)
        # Client-visible: first refusal to the first OK after it.
        bad = [t for t, st in visible if st != OK]
        if bad:
            first = bad[0]
            back = [t for t, st in visible if st == OK and t > first]
            res["e_migrating_client_ms"] = round(((back[0] if back else t_end) - first) / 1e6, 1)
            res["client_refusals"] = {fc.STATUS.get(s, s): sum(1 for _, x in visible if x == s)
                                      for s in {x for _, x in visible if x != OK}}
        units = [int(kv["unit_us"]) / 1000 for _, kv in traces(blog, "stage_unit") if kv.get("final") == "1"]
        res["stage_units_trace"] = stats(units)
        chain = test_s1.chains(c, b, S)
        res["chain"] = chain[-1] if chain else None
        long_iter = [float(kv["total_ms"]) for t, kv in traces(blog, "shard_long_iter")]
        long_final = [float(kv["total_ms"]) for t, kv in traces(blog, "shard_long_iter") if fb <= t <= fe + 50e6]
        res["dest_long_iters_ge20ms"] = {"whole_run": stats(long_iter), "final_stage": stats(long_final)}
        res["dest_long_iters_final_detail"] = [kv for t, kv in traces(blog, "shard_long_iter") if fb <= t <= fe + 50e6]
        # What else B's shards ran in the final stage's span: traced events by name.
        names = {}
        for line in open(blog, errors="replace"):
            f = line.split()
            if line.startswith("T ") and len(f) > 3 and fb <= int(f[1]) <= fe:
                names[f[3]] = names.get(f[3], 0) + 1
        res["dest_trace_events_final"] = names
        for k in ("HEALTH", "STATUS", "fg"):
            res[k] = {"final_stage": stats([v for t, v in lat[k] if fb <= t <= fe]),
                      "whole_move": stats([v for t, v in lat[k] if t_move <= t <= t_end])}
        res["health_stall_ms_max"] = {
            "final_stage": max([v for t, v in lat["stall_ms"] if fb <= t <= fe] or [0]),
            "whole_move": max([v for t, v in lat["stall_ms"] if t_move <= t <= t_end] or [0])}
        try:
            pid = c.server[b].pid
            for line in open(f"/proc/{pid}/status"):
                if line.startswith(("VmHWM", "VmRSS")):
                    res[line.split(":")[0]] = line.split(":")[1].strip()
        except OSError:
            pass
        res["driver_log"] = [ln for ln in r_err.splitlines() if "final copy" in ln or "seal:" in ln
                             or "copied" in ln or "staged" in ln or "commit" in ln]
    finally:
        c.stop()
    print(json.dumps(res, indent=1))
    if args.json:
        json.dump(res, open(args.json, "w"), indent=1)
    if not args.keep:
        subprocess.run(["rm", "-rf", work])


if __name__ == "__main__":
    main()
