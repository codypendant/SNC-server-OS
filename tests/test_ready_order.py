#!/usr/bin/env python3
"""Causal order of NOT READY and READY (docs/SYNC_READINESS.md 7; work
order docs/handoff/PR49_Review_Handoff.txt).

Real processes: the sync pair of tests/test_health.py (two snc_server
nodes, three replicated snc_witness with --debug 1), and net_bench
--session --verify depositing through both nodes.

  c1_delayed_ready  The primary proves its standby in sync and asks for
                    READY (SYNCED before the fix); every witness holds that
                    request (DEBUG HOLD: the deterministic form of a request
                    queued behind other proposals). The standby is then lost
                    (SIGSTOP), the primary is granted to run alone and
                    acknowledges writes on local durability only. The held
                    requests are released, then the primary is killed and
                    the standby resumed.
                    Before the fix (SNC_CHAR=1): the released SYNCED restores
                    READY over the later grant, the witnesses promote the
                    standby, and acknowledged writes are lost (the client's
                    verify finds mismatches). After the fix: READY is refused,
                    the state stays NOT READY, nobody is promoted (V1).

Every scenario prints one JSON line (and writes it to --json DIR).

Usage: test_ready_order.py BUILD_DIR [SCENARIO ...] [--json DIR]
"""
import json
import os
import signal
import sys
import time

ARGS = list(sys.argv[1:])
JSON_DIR = None
if "--json" in ARGS:
    i = ARGS.index("--json")
    JSON_DIR = ARGS[i + 1]
    del ARGS[i:i + 2]
BUILD = ARGS[0] if ARGS else "build"
SCENARIOS = ARGS[1:]
sys.argv = [sys.argv[0], BUILD]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_health as th  # noqa: E402
import test_sync_readiness as sr  # noqa: E402

CHAR = os.environ.get("SNC_CHAR") == "1"  # characterize the pre-fix behavior
DEBUG = ("--debug", "1")


def debug_all(pr, what):
    return [th.ask(p, f"DEBUG {what}", 3)[0] for p in pr.wit]


def held_total(pr):
    n = 0
    for r in debug_all(pr, "HELD"):
        if r and r.startswith("HELD "):
            n += int(r.split()[2])
    return n


def count(proc, pat):
    return sum(1 for l in proc.lines if pat in l)


def wait_until(cond, timeout, step=0.05):
    t_end = time.time() + timeout
    while time.time() < t_end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return None


GRANTED = "witness: granted, continuing without the standby"


def c1_delayed_ready(pr):
    res = {"char": CHAR}
    st = pr.wait_ready(30, term=0)
    assert st, f"never READY at term 0: {pr.state()}"
    b = sr.Bench(pr, 60)
    time.sleep(2)
    n0, n1 = pr.nodes
    # 1. The standby lost: the primary is granted to run alone (grant 1).
    n1.signal(signal.SIGSTOP)
    assert wait_until(lambda: count(n0, GRANTED) >= 1, 20), n0.text()
    res["state_after_grant_1"] = pr.state()
    # 2. Every witness holds readiness requests; the standby comes back and
    #    catches up; the primary proves it in sync and asks: held.
    assert all(r == "OK" for r in debug_all(pr, "HOLD")), "witnesses without --debug"
    n1.signal(signal.SIGCONT)
    assert wait_until(lambda: held_total(pr) >= 1, 30), (pr.state(), n0.lines[-10:])
    res["held_before_loss"] = held_total(pr)
    # 3. The standby lost again while that request is held: grant 2, and
    #    writes acknowledged on the primary's durability alone.
    grants = count(n0, GRANTED)
    n1.signal(signal.SIGSTOP)
    assert wait_until(lambda: count(n0, GRANTED) > grants, 30), n0.lines[-20:]
    res["state_after_grant_2"] = pr.state()
    time.sleep(2)  # deposits acknowledged alone
    assert b.running(), "the client finished early"
    # 4. The held requests are released.
    res["released"] = debug_all(pr, "RELEASE")
    time.sleep(1.5)
    res["state_after_release"] = st = pr.state()
    res["ready_restored"] = bool(st) and st[1] == 0 and st[3] is False
    res["witness_lines"] = [l for w in pr.w for l in w.lines if "has its standby back in sync" in l][-6:]
    res["primary_lines"] = [l for l in n0.lines if l.startswith("witness:")][-8:]
    # 5. The primary dies; the standby resumes (it lacks grant 2's writes).
    n0.signal(signal.SIGKILL)
    n1.signal(signal.SIGCONT)
    promoted = sr.wait_state(pr, 1, sr.NO_PROMOTION_S)
    res["promoted"] = promoted
    if promoted:
        res["bench"] = r = b.wait(120)
        res["acked_writes_lost"] = r.get("rc") != 0 or r.get("mismatches", 0) > 0
    else:
        res["final_state"] = pr.state()
        b.stop()
    if CHAR:
        # The pre-fix hole: READY over a later grant, an unsafe promotion,
        # acknowledged writes lost.
        res["pass"] = res["ready_restored"] and bool(promoted) and res.get("acked_writes_lost", False)
    else:
        fin = res.get("final_state")
        res["pass"] = (not res["ready_restored"] and not promoted and bool(fin) and fin[1] == 0 and
                       fin[3] is True)
    return res


SCEN = {
    "c1_delayed_ready": lambda: sr.run("c1_delayed_ready", c1_delayed_ready, witness_extra=DEBUG),
}


def main():
    names = SCENARIOS or list(SCEN)
    failed = []
    for name in names:
        t = time.time()
        try:
            r = SCEN[name]()
        except Exception as e:  # noqa: BLE001
            r = {"pass": False, "error": f"{type(e).__name__}: {e}"}
        r["scenario"], r["seconds"] = name, round(time.time() - t, 1)
        print(("PASS " if r.get("pass") else "FAIL ") + name + " " + json.dumps(r, default=str)[:4000], flush=True)
        if JSON_DIR:
            os.makedirs(JSON_DIR, exist_ok=True)
            with open(f"{JSON_DIR}/{name}.json", "w") as f:
                json.dump(r, f, indent=1, default=str)
        if not r.get("pass"):
            failed.append(name)
    print("test_ready_order:", "FAILED " + str(failed) if failed else "OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
