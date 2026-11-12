#!/usr/bin/env python3
"""A2-a session-2 postmortem, item 4: which journal events fall in each session's settle / load window.
Reads raw/journal-window.txt (journalctl -b 0 -o short-iso, 16:00-20:05 UTC, drm_fb_helper kernel noise removed)
and ../a2a-storage-repro-r640/out/pilot.json. Excluded as the harness's own or the sampler: sudo (the A2-a root
steps), their CRON session lines, debian-sa1 / sysstat-collect (every 10 min). Writes events.txt. python3 -I -B events.py"""
import re, json, os, datetime as dt
H = os.path.dirname(os.path.abspath(__file__))
pj = json.load(open(os.path.join(H, "..", "a2a-storage-repro-r640", "out", "pilot.json")))
S = {s["k"]: s for s in pj["results"]["A2a_validation"]["sessions"]}
u = lambda x: dt.datetime.strptime(x, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
ev = []
for l in open(os.path.join(H, "raw", "journal-window.txt")):
    m = re.match(r"(\S+) solo4 (\S+?)(?:\[\d+\])?: (.*)", l)
    if not m:
        continue
    t, who, msg = dt.datetime.fromisoformat(m.group(1)), m.group(2), m.group(3)
    if who == "sudo" or (who == "CRON" and "session" in msg) or "debian-sa1" in msg or "sysstat" in msg:
        continue
    ev.append((t, who, msg[:110]))
out = [f"{len(ev)} events other than harness sudo / sysstat in 16:00-20:05 UTC"]
for k in range(1, 6):
    s = S[k]
    a, b, c = u(s["trim"]["utc"]), u(s["start"]["utc"]), u(s["end"]["utc"])
    for name, lo, hi in (("settle", a, b), ("LOAD", b, c)):
        out.append(f"k{k} {name} {lo:%H:%M:%S}-{hi:%H:%M:%S}")
        for e in ev:
            if lo <= e[0] <= hi:
                out.append(f"    {e[0]:%H:%M:%S} {e[1]}: {e[2]}")
open(os.path.join(H, "events.txt"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
