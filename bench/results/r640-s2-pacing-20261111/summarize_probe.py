#!/usr/bin/env python3
"""Summarize probe/*.events.gz: per placement and mode, walking-step and visit-call durations (ns)."""
import gzip, glob, os, re, sys, collections
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probe")
S = collections.defaultdict(list); V = collections.defaultdict(list); ST = collections.defaultdict(list)
for f in sorted(glob.glob(os.path.join(d, "*.events.gz"))):
    pl, mode, p = re.match(r"(\w+)-(\w+)-p(\d)", os.path.basename(f)).groups()
    for line in gzip.open(f, "rt"):
        t = line.split()
        if not t: continue
        if t[0] == "S": S[(pl, mode)].append(int(t[1]))
        elif t[0] == "V": V[(pl, mode)].append(int(t[1]))
    ST[(pl, mode)].append(open(f.replace(".events.gz", ".stdout")).read())
def q(a, p): return a[min(len(a) - 1, int(p * len(a)))]
order = ["off", "1", "5", "10", "20", "25", "50", "100", "250"]
print("placement mode     steps    step_us p50/p90/p99/p99.9/max                 visit_calls visit_us p50/p99/max   ids/step(mean)")
for pl in ("L2", "L0"):
    for m in order:
        s = sorted(S[(pl, m)]); v = sorted(V[(pl, m)])
        if not s: continue
        n = 3 * 625000  # three checkpoints per process
        procs = len(ST[(pl, m)])
        print(f"{pl:9} {m:5} {len(s):9}  " + "/".join(f"{q(s,x)/1000:.1f}" for x in (.5,.9,.99,.999)) + f"/{s[-1]/1000:.1f}"
              + f"   {len(v):9}  " + "/".join(f"{q(v,x)/1000:.2f}" for x in (.5,.99)) + f"/{v[-1]/1000:.1f}"
              + f"   {procs*n/len(s):.1f}")
