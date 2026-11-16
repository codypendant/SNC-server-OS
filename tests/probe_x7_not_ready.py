#!/usr/bin/env python3
# Usage (from the repository root, after building into build/): STALL=1.0 AFTER=0 python3 tests/probe_x7_not_ready.py
# Not a ctest entry: a probe recorded with docs/handoff/PR137_D4_Startup_Errors_Review_Approval.md.
# Mechanism probe for test_xnode_proc x7: a standby stalled briefly under load
# (as on a slow host) makes its primary DEGRADE (NOT READY on record); a kill
# of the primary then is not followed by a promotion.
import os, sys, time, signal, shutil
sys.argv = [sys.argv[0], "build"]
sys.path.insert(0, "tests")
import test_xnode_proc as t
import test_fail_closed as fc
STALL = float(os.environ.get("STALL", "1.0"))
work = __import__("tempfile").mkdtemp(prefix="snc-x7mech-")
c = t.XCluster(work, ha=True)
load = None
try:
    ids = t.make_ids(c)
    for g in (0, 1):
        fc.wait(lambda: (fc.ha_state(c, g) or (0, 0, True))[2] is False, f"group {g} READY", timeout=60)
    print("before load:", fc.ha_state(c, 1))
    load = t.Load(c, ids, 15)
    time.sleep(1)
    if STALL > 0:
        c.server[4].send_signal(signal.SIGSTOP); time.sleep(STALL); c.server[4].send_signal(signal.SIGCONT)
    time.sleep(float(os.environ.get("AFTER", "2")))
    print("at kill:", fc.ha_state(c, 1))
    c.kill(3)
    t0 = time.time(); promoted = False
    while time.time() - t0 < 20:
        if c.role(4) in ("p", "n"): promoted = True; break
        time.sleep(0.2)
    print("promoted:", promoted, "after", round(time.time() - t0, 1), "s; state", fc.ha_state(c, 1))
    for line in open(f"{work}/n3.log"):
        if line.startswith("witness:"): print("  n3", line.rstrip())
    for i in range(3):
        n = open(f"{work}/w{i}.log").read().count("not ready")
        print(f"  w{i} 'not ready' mentions: {n}")
finally:
    if load: load.stop()
    c.stop()
    shutil.rmtree(work, ignore_errors=True)
