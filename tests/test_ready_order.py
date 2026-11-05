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

  c2_restart_held   V7: a READY held; the primary process restarts (new
                    grant); the old process's READY, released, is refused;
                    the new one's READY names its own NOT READY; failover
                    exact.
  c3_two_failovers  V8: after the first failover, the new primary's READY
                    held while the rejoined standby is lost (grant);
                    released: refused; then proven, READY, second failover
                    exact.
  m1, m2, m3        V10 (only with SNC_OLD_BUILD=<dir of a build from before
                    the fix>): new nodes with old witnesses, old nodes with
                    new witnesses, new nodes with one old witness of three.

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
    res["witness_lines"] = [l for w in pr.w for l in w.lines if " SYNCED:" in l or " READY:" in l][-6:]
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


def grant_alone(pr, k, standby):
    """Stops `standby` (SIGSTOP) and waits for node k, under client load, to
    be granted to run alone again; returns the grant count before."""
    g = count(pr.nodes[k], GRANTED)
    pr.nodes[standby].signal(signal.SIGSTOP)
    assert wait_until(lambda: count(pr.nodes[k], GRANTED) > g, 30), pr.nodes[k].lines[-20:]
    return g


def refused_after_release(pr, res, term):
    """Releases the held READY requests: each must be refused; the state
    must stay NOT READY at `term`."""
    n = len([l for w in pr.w for l in w.lines if "NO not ready since" in l])
    res["released"] = debug_all(pr, "RELEASE")
    time.sleep(1.5)
    res["state_after_release"] = st = pr.state()
    res["refusals"] = len([l for w in pr.w for l in w.lines if "NO not ready since" in l]) - n
    return bool(st) and st[1] == term and st[3] is True and res["refusals"] >= 1


def c2_restart_held(pr):
    """V7: a READY held in the witnesses; the primary process dies and
    restarts (the standby frozen, so it starts as primary and runs alone
    under a new grant); the old process's READY, released, is refused.
    The standby resumes, the new process proves it and says READY after its
    own NOT READY; then a failover is exact."""
    res = {}
    assert pr.wait_ready(30, term=0)
    b = sr.Bench(pr, 60)
    time.sleep(2)
    grant_alone(pr, 0, 1)
    assert all(r == "OK" for r in debug_all(pr, "HOLD"))
    pr.nodes[1].signal(signal.SIGCONT)
    assert wait_until(lambda: held_total(pr) >= 1, 30), pr.nodes[0].lines[-10:]
    old_base = [l for l in pr.nodes[0].lines if "recorded NOT READY" in l or "granted" in l]
    pr.nodes[1].signal(signal.SIGSTOP)
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[0].p.wait(timeout=30)
    pr.old_nodes.append(pr.nodes[0])
    pr.nodes[0] = th.Proc([f"{BUILD}/snc_server"] + pr.args[0])
    started = wait_until(lambda: next((l for l in pr.nodes[0].lines if "recorded NOT READY (this primary started)" in l),
                                      None), 30)
    assert started, pr.nodes[0].text()
    assert wait_until(lambda: count(pr.nodes[0], GRANTED) >= 1, 30), pr.nodes[0].lines[-20:]
    res["old_process_lines"] = old_base[-3:]
    res["new_process_start"] = started
    res["refused"] = refused_after_release(pr, res, 0)
    pr.nodes[1].signal(signal.SIGCONT)
    st = pr.wait_ready(60, term=0)
    res["ready_again"] = st
    ready_line = next((l for l in pr.nodes[0].lines if "READY after v=" in l and "refused" not in l), "")
    base = int(started.split("at v=")[1])
    after = int(ready_line.split("after v=")[1].rstrip(")")) if ready_line else 0
    res["new_ready_line"] = ready_line
    res["grounded"] = after >= base  # the new process's READY names its own NOT READY
    time.sleep(1)
    pr.nodes[0].signal(signal.SIGKILL)
    res["promoted"] = sr.wait_state(pr, 1, 20)
    res["bench"] = r = b.wait(180)
    res["pass"] = (res["refused"] and bool(st) and res["grounded"] and bool(res["promoted"]) and sr.exact(r))
    return res


def c3_two_failovers(pr):
    """V8: the first failover (node 1 promoted, term 1); node 0 rejoins; the
    new primary's first READY held; node 0 lost again (a grant, writes
    alone); released: refused, NOT READY at term 1. Node 0 resumes, is
    proven, READY; node 1 dies; node 0 promoted (term 2): exact."""
    res = {}
    b = sr.Bench(pr, 60)
    time.sleep(2)
    assert all(r == "OK" for r in debug_all(pr, "HOLD"))  # only READY waits; the term-0 one is on record
    sr.first_failover(pr, res)
    sr.rejoined(pr)
    assert wait_until(lambda: held_total(pr) >= 1, 60), pr.nodes[1].lines[-10:]
    grant_alone(pr, 1, 0)
    time.sleep(1)  # deposits acknowledged alone
    res["refused"] = refused_after_release(pr, res, 1)
    pr.nodes[0].signal(signal.SIGCONT)
    st = pr.wait_ready(60, term=1)
    res["ready_term_1"] = st
    time.sleep(1)
    assert b.running(), "the client finished before the second failure"
    pr.nodes[1].signal(signal.SIGKILL)
    st2 = sr.wait_state(pr, 0, 20)
    res["second_promoted"] = st2
    res["bench"] = r = b.wait(180)
    res["pass"] = (res["promotion_not_ready"] and res["refused"] and bool(st) and bool(st2) and st2[1] == 2 and
                   sr.exact(r))
    return res


# ---- V10: mixed old and new binaries (OLD: a build from before the fix) ----

OLD = os.environ.get("SNC_OLD_BUILD")


def m1_new_primary_old_witnesses(pr):
    """New nodes, every witness old: NOTREADY and READY unknown there. Never
    READY; a lost standby gets no grant (durable writes wait); the
    primary's death promotes nobody."""
    res = {}
    time.sleep(3)
    res["state"] = pr.state()
    res["too_old_logged"] = any("does not know NOTREADY" in l for l in pr.nodes[0].lines)
    b = sr.Bench(pr, 15)
    time.sleep(1)
    pr.nodes[1].signal(signal.SIGSTOP)
    time.sleep(3)
    res["granted"] = count(pr.nodes[0], GRANTED)
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[1].signal(signal.SIGCONT)
    held, seen = sr.no_promotion(pr, 1, sr.NO_PROMOTION_S)
    res["no_promotion"], res["states"] = held, seen
    b.stop()
    res["pass"] = (bool(res["state"]) and res["state"][3] is True and res["too_old_logged"] and
                   res["granted"] == 0 and held)
    return res


def m2_old_primary_new_witnesses(pr):
    """Old nodes, new witnesses: the old primary's SYNCED is refused (never
    READY), its DEGRADE still grants; the primary's death promotes nobody."""
    res = {}
    b = sr.Bench(pr, 15)
    time.sleep(3)
    res["state"] = pr.state()
    res["synced_refused"] = any("NO SYNCED has no causal base" in l for w in pr.w for l in w.lines)
    pr.nodes[1].signal(signal.SIGSTOP)
    res["granted"] = bool(wait_until(lambda: count(pr.nodes[0], GRANTED) >= 1, 20))
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[1].signal(signal.SIGCONT)
    held, seen = sr.no_promotion(pr, 1, sr.NO_PROMOTION_S)
    res["no_promotion"], res["states"] = held, seen
    b.stop()
    res["pass"] = (bool(res["state"]) and res["state"][3] is True and res["synced_refused"] and res["granted"] and
                   held)
    return res


def m3_new_primary_mixed_witnesses(pr):
    """New nodes, one old witness of three: the primary skips it, READY is
    chosen through the new ones, and the forced window of c1 stays closed
    (the hold is on the two new witnesses; the old one never proposes READY
    for a new primary)."""
    res = {"too_old_logged": any("does not know" in l for l in pr.nodes[0].lines)}
    r = c1_body(pr, res, new_only=True)
    res["pass"] = r and res["too_old_logged"]
    return res


def c1_body(pr, res, new_only=False):
    """c1 without its own setup: True if the window stayed closed."""
    ports = pr.wit[1:] if new_only else pr.wit
    st = pr.wait_ready(30, term=0)
    assert st, f"never READY at term 0: {pr.state()}"
    b = sr.Bench(pr, 60)
    time.sleep(2)
    grant_alone(pr, 0, 1)
    assert all(th.ask(p, "DEBUG HOLD", 3)[0] == "OK" for p in ports)
    pr.nodes[1].signal(signal.SIGCONT)

    def held():
        return sum(int(r.split()[2]) for r in (th.ask(p, "DEBUG HELD", 3)[0] for p in ports)
                   if r and r.startswith("HELD "))
    assert wait_until(lambda: held() >= 1, 30), pr.nodes[0].lines[-10:]
    grant_alone(pr, 0, 1)
    time.sleep(2)
    res["released"] = [th.ask(p, "DEBUG RELEASE", 3)[0] for p in ports]
    time.sleep(1.5)
    res["state_after_release"] = st = pr.state()
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[1].signal(signal.SIGCONT)
    held_ok, seen = sr.no_promotion(pr, 1, sr.NO_PROMOTION_S)
    res["no_promotion"] = held_ok
    b.stop()
    return bool(st) and st[3] is True and held_ok


def old_bins(n_old_witnesses):
    return [f"{OLD}/snc_witness"] * n_old_witnesses + [f"{BUILD}/snc_witness"] * (3 - n_old_witnesses)


SCEN = {
    "c1_delayed_ready": lambda: sr.run("c1_delayed_ready", c1_delayed_ready, witness_extra=DEBUG),
    "c2_restart_held": lambda: sr.run("c2_restart_held", c2_restart_held, witness_extra=DEBUG),
    "c3_two_failovers": lambda: sr.run("c3_two_failovers", c3_two_failovers, witness_extra=DEBUG),
}
MIXED = {
    "m1_new_primary_old_witnesses": lambda: sr.run("m1_new_primary_old_witnesses", m1_new_primary_old_witnesses,
                                                   witness_bins=old_bins(3), ready=False),
    "m2_old_primary_new_witnesses": lambda: sr.run("m2_old_primary_new_witnesses", m2_old_primary_new_witnesses,
                                                   node_bin=f"{OLD}/snc_server", ready=False),
    "m3_new_primary_mixed_witnesses": lambda: sr.run("m3_new_primary_mixed_witnesses",
                                                     m3_new_primary_mixed_witnesses, witness_extra=DEBUG,
                                                     witness_bins=old_bins(1)),
}
if OLD:
    SCEN.update(MIXED)


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
