#!/usr/bin/env python3
"""A running checkpoint must not hold failover (docs/CHECKPOINT_FAILOVER.md).

Real processes: the sync pair of tests/test_health.py (two snc_server nodes
with --checkpoint-s 1, three replicated snc_witness) and a create load
through node 0. The checkpoint is held deterministically with DEBUG
CKPT-HOLD, so nothing depends on how long a checkpoint happens to take.

  k1_commit  Node 0 (primary) holds its checkpoint just before the commit:
             every shard's part is written and synced, so the shards are
             free and only the checkpoint's caller waits. Its standby is
             then lost (SIGSTOP). The primary must be granted by the
             witnesses to run alone, and acknowledge writes, while the
             checkpoint is still held. The standby then comes back and is
             proven in sync (READY), still during the held checkpoint.
             Released: the checkpoint commits; nothing acknowledged is
             missing.
  k2_sync    The same with the checkpoint held inside the shards (the final
             fdatasync completions of its parts held), so the shards are in
             their checkpoint while the standby is lost.
  k3_promote The standby's own checkpoint held at the commit; the primary
             killed. Promotion runs wholly before or wholly after a
             checkpoint (Engine::promote, section 10 of the durable control
             rules), so it waits for the held checkpoint: no promotion while
             held, then promotion once released, and nothing acknowledged
             is missing. This records the remaining bound: a promotion
             waits for a running standby checkpoint.

Before the fix, k1 and k2 fail: the server's main loop runs the checkpoint
and the witness exchange in turn, so no grant is asked for while the
checkpoint is held.

Every scenario prints one JSON line.

Usage: test_ckpt_failover.py BUILD_DIR [SCENARIO ...]
"""
import json
import os
import signal
import sys
import time

ARGS = list(sys.argv[1:])
BUILD = ARGS[0] if ARGS else "build"
SCENARIOS = ARGS[1:]
sys.argv = [sys.argv[0], BUILD]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_health as th  # noqa: E402

GRANTED = "witness: granted, continuing without the standby"
# A grant needs the standby timeout (--replica-timeout-ms 500) and one
# witness round; the other tests allow 20 s for it under sanitizers.
GRANT_BOUND_S = 20


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


def hold(port, what, on):
    r, _ = th.ask(port, f"DEBUG CKPT-HOLD {what} {1 if on else 0}", 3)
    assert r and r.startswith("OK"), f"DEBUG CKPT-HOLD refused: {r}"


def at_commit(port):
    r, _ = th.ask(port, "DEBUG CKPT-HOLD ?", 3)
    return bool(r) and "at_commit=1" in r


def health(port):
    r, _ = th.ask(port, "HEALTH", 1.0)
    return th.fields(r)


def ckpt_held_in_shards(port, ms):
    """A checkpoint has been in its shard phase for at least `ms`."""
    f = health(port)
    return f.get("ckpt") == "shards" and int(f.get("ckpt_age_ms", "0")) >= ms


def standby_lost_during_checkpoint(name, what):
    pr = th.Pair(extra=("--checkpoint-s", "1"))
    res = {"name": name}
    try:
        n0, n1 = pr.nodes
        pr.load = th.Load(pr.to0_cli, pr.work)
        assert wait_until(lambda: len(pr.load.acked) >= 3, 30), "no load before the fault"
        # 1. A checkpoint held on the primary.
        hold(pr.adm[0], what, True)
        if what == "commit":
            assert wait_until(lambda: at_commit(pr.adm[0]), 30), n0.text()
        else:
            assert wait_until(lambda: ckpt_held_in_shards(pr.adm[0], 1000), 30), (health(pr.adm[0]), n0.text())
        ckpts_held = count(n0, "checkpoint: ")
        # 2. The standby lost while it is held.
        grants0 = count(n0, GRANTED)
        t0 = time.time()
        n1.signal(signal.SIGSTOP)
        granted = wait_until(lambda: count(n0, GRANTED) > grants0, GRANT_BOUND_S)
        res["grant_s"] = round(time.time() - t0, 2) if granted else None
        assert granted, f"no grant within {GRANT_BOUND_S} s while a checkpoint is held\n" + n0.text()
        # 3. Writes acknowledged alone, the checkpoint still held.
        acked0 = len(pr.load.acked)
        assert wait_until(lambda: len(pr.load.acked) >= acked0 + 3, GRANT_BOUND_S), (
            f"no writes acknowledged after the grant ({acked0} batches)\n" + n0.text())
        res["acked_while_held"] = len(pr.load.acked) - acked0
        res["held_throughout"] = count(n0, "checkpoint: ") == ckpts_held and (
            at_commit(pr.adm[0]) if what == "commit" else health(pr.adm[0]).get("ckpt") == "shards")
        assert res["held_throughout"], "the checkpoint was not held throughout"
        st = pr.state()
        res["state_while_held"] = st
        assert st and st[2] == 0 and st[3], f"the witnesses should hold node 0 degraded: {st}"
        # 4. The standby comes back and is proven in sync, the checkpoint
        #    still held.
        n1.signal(signal.SIGCONT)
        assert pr.wait_ready(60, term=0), f"never READY again: {pr.state()}\n" + n0.text()
        res["ready_while_held"] = count(n0, "checkpoint: ") == ckpts_held
        assert res["ready_while_held"], "the checkpoint ended before READY"
        # 5. Released: the checkpoint commits.
        hold(pr.adm[0], what, False)
        assert wait_until(lambda: count(n0, "checkpoint: ") > ckpts_held, 30), n0.text()
        pr.load.stop()
        missing, total = pr.load.missing_on(pr.cli[0])
        res["missing"], res["total"] = missing, total
        assert missing == 0, res
        print(json.dumps(res), flush=True)
    except BaseException:
        for i, p in enumerate(pr.nodes + pr.w):
            if p:
                th.log(f"-- proc {i}\n" + p.text()[-3000:])
        raise
    finally:
        for p in pr.adm:
            th.ask(p, f"DEBUG CKPT-HOLD {what} 0", 1)
        pr.stop()


def k1_commit():
    standby_lost_during_checkpoint("k1_commit", "commit")


def k2_sync():
    standby_lost_during_checkpoint("k2_sync", "sync")


def k3_promote():
    pr = th.Pair(extra=("--checkpoint-s", "1"))
    res = {"name": "k3_promote"}
    try:
        n0, n1 = pr.nodes
        pr.load = th.Load(pr.to0_cli, pr.work)
        assert wait_until(lambda: len(pr.load.acked) >= 3, 30), "no load before the fault"
        hold(pr.adm[1], "commit", True)
        assert wait_until(lambda: at_commit(pr.adm[1]), 30), n1.text()
        pr.load.stop()
        n0.signal(signal.SIGKILL)
        # The witnesses choose node 1; its promotion waits for the held checkpoint.
        st = wait_until(lambda: (lambda x: x if x and x[2] == 1 else None)(pr.state()), 30)
        assert st, f"the witnesses never chose node 1: {pr.state()}"
        res["state_while_held"] = st
        time.sleep(2)  # the promotion asked for and waiting
        r, _ = th.ask(pr.adm[1], "HEALTH", 1.0)
        res["role_while_held"] = th.fields(r).get("role")
        assert res["role_while_held"] == "s", f"promoted during its own checkpoint: {r}"
        t0 = time.time()
        hold(pr.adm[1], "commit", False)
        assert pr.wait_promoted(30), f"not promoted after the release: {pr.state()}\n" + n1.text()
        res["promote_after_release_s"] = round(time.time() - t0, 2)
        missing, total = pr.load.missing_on(pr.cli[1])
        res["missing"], res["total"] = missing, total
        assert missing == 0, res
        print(json.dumps(res), flush=True)
    except BaseException:
        for i, p in enumerate(pr.nodes + pr.w):
            if p:
                th.log(f"-- proc {i}\n" + p.text()[-3000:])
        raise
    finally:
        for p in pr.adm:
            th.ask(p, "DEBUG CKPT-HOLD commit 0", 1)
        pr.stop()


ALL = {"k1_commit": k1_commit, "k2_sync": k2_sync, "k3_promote": k3_promote}


def main():
    for name in SCENARIOS or list(ALL):
        th.log(f"== {name}")
        ALL[name]()
    th.log("test_ckpt_failover: OK")


if __name__ == "__main__":
    main()
