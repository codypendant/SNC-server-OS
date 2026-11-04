#!/usr/bin/env python3
"""Samples per-thread CPU use of the lab's processes into trace lines, for
one merged timeline with SNC_TRACE output (docs/TRACING.md).

  cpu_sample.py OUT [--interval-ms 100] [--match snc_]

Every interval, for each thread of each process whose name starts with
--match, it writes the CPU it used since the last sample (percent of one
CPU), its state (R running/runnable, S sleeping, D disk wait) and the CPU
it last ran on, but only threads that used at least 5% or are runnable.
It also writes the host's run queue (procs_running) and CPU pressure.
Timestamps are CLOCK_MONOTONIC ns, like SNC_TRACE's. Stops on SIGTERM.

  T <ns> <pid> cpu_thread tid=<tid> comm=<name> pct=<p> state=<s> cpu=<n>
  T <ns> 0 cpu_host procs_running=<n> psi_some_total_us=<n>
"""
import os
import signal
import sys
import time

TICK = os.sysconf("SC_CLK_TCK")


def stat_fields(path):
    with open(path) as f:
        s = f.read()
    # comm is in parentheses and may hold spaces
    r = s.rindex(")")
    comm = s[s.index("(") + 1:r]
    rest = s[r + 2:].split()
    # rest[0] is field 3 (state); utime is field 14, stime 15, processor 39
    return comm, rest[0], int(rest[11]) + int(rest[12]), int(rest[36])


def main():
    out = open(sys.argv[1], "w", buffering=1)
    interval = 0.1
    match = "snc_"
    args = sys.argv[2:]
    for i, a in enumerate(args):
        if a == "--interval-ms":
            interval = int(args[i + 1]) / 1000
        if a == "--match":
            match = args[i + 1]
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    last = {}
    t_last = time.monotonic_ns()
    while not stop:
        time.sleep(interval)
        now = time.monotonic_ns()
        dt = (now - t_last) / 1e9
        t_last = now
        seen = {}
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/comm") as f:
                    pcomm = f.read().strip()
                if not pcomm.startswith(match):
                    continue
                for tid in os.listdir(f"/proc/{pid}/task"):
                    comm, state, ticks, cpu = stat_fields(f"/proc/{pid}/task/{tid}/stat")
                    key = (pid, tid)
                    seen[key] = ticks
                    used = ticks - last.get(key, ticks)
                    pct = 100.0 * used / TICK / dt if dt > 0 else 0
                    if pct >= 5 or state == "R":
                        out.write(f"T {now} {pid} cpu_thread tid={tid} comm={pcomm} pct={pct:.0f} state={state} cpu={cpu}\n")
            except (FileNotFoundError, ProcessLookupError, ValueError, IndexError):
                continue
        last = seen
        running = 0
        with open("/proc/stat") as f:
            for line in f:
                if line.startswith("procs_running"):
                    running = int(line.split()[1])
        psi = ""
        try:
            with open("/proc/pressure/cpu") as f:
                psi = f.readline().split("total=")[1].strip()
        except (FileNotFoundError, IndexError):
            pass
        out.write(f"T {now} 0 cpu_host procs_running={running} psi_some_total_us={psi}\n")


if __name__ == "__main__":
    main()
