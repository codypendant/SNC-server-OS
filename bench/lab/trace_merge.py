#!/usr/bin/env python3
"""Merges the SNC_TRACE lines of one lab run into one timeline
(docs/TRACING.md).

  trace_merge.py RUN_DIR [--from S] [--to S] [--grep RE] [--no-cpu] [--cpu-min PCT]

Reads every *.log and *.trace file in RUN_DIR, keeps the lines that start
with "T <ns> <pid> ", tags each with its file (n1, w0, driver-move, cpu-a,
...) and prints them in time order, as seconds since the first line:

  12.345678  n1     admin_status_slow total_ms=812.0 ...

All the files must come from one host (CLOCK_MONOTONIC is per host).
"""
import argparse
import glob
import os
import re
import signal


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)  # quiet under | head
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--from", dest="t_from", type=float, default=None)
    ap.add_argument("--to", dest="t_to", type=float, default=None)
    ap.add_argument("--grep", default=None)
    ap.add_argument("--no-cpu", action="store_true")
    ap.add_argument("--cpu-min", type=float, default=50, help="cpu_thread lines below this percent are dropped")
    a = ap.parse_args()
    rows = []
    traces = glob.glob(os.path.join(a.run_dir, "*.trace"))
    have = {os.path.basename(p).rsplit(".", 1)[0] for p in traces}
    # A NAME.trace holds NAME.log's trace lines (the lab extracts them, as
    # .log files are not committed): read one or the other.
    logs = [p for p in glob.glob(os.path.join(a.run_dir, "*.log")) if os.path.basename(p)[:-4] not in have]
    for path in sorted(logs + traces):
        src = os.path.basename(path).rsplit(".", 1)[0]
        with open(path, errors="replace") as f:
            for line in f:
                if not line.startswith("T "):
                    continue
                parts = line.rstrip("\n").split(" ", 3)
                if len(parts) < 4:
                    continue
                try:
                    ns = int(parts[1])
                except ValueError:
                    continue
                rows.append((ns, src, parts[2], parts[3]))
    if not rows:
        print("no trace lines")
        return
    rows.sort()
    t0 = rows[0][0]
    rx = re.compile(a.grep) if a.grep else None
    for ns, src, pid, msg in rows:
        t = (ns - t0) / 1e9
        if a.t_from is not None and t < a.t_from:
            continue
        if a.t_to is not None and t > a.t_to:
            continue
        if msg.startswith("cpu_"):
            if a.no_cpu:
                continue
            m = re.search(r"pct=(\d+)", msg)
            if msg.startswith("cpu_thread") and m and float(m[1]) < a.cpu_min and "state=R" not in msg:
                continue
        if rx and not rx.search(src + " " + msg):
            continue
        print(f"{t:11.6f}  {src:<12} {pid:>7}  {msg}")


if __name__ == "__main__":
    main()
