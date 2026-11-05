#!/usr/bin/env python3
"""B2: a standby is promotion-eligible only once its current-term primary
has proven it in sync (docs/SYNC_READINESS.md; work order
docs/handoff/PR47_PR48_Blocker_Fix_Handoff.txt, B2 tests 1 to 10).

Real processes: the sync pair of tests/test_health.py (two snc_server nodes,
node 0 restarting with --rejoin, three replicated snc_witness on HEALTH,
--failover-ms 1000), and net_bench --session --verify sending deposits
through both nodes (exact balances at the end: every acknowledged write
applied once). Most scenarios start the same way: the pair is READY at
term 0, node 0 is SIGKILLed, node 1 is promoted to term 1, which the
witnesses record NOT READY; node 0 restarts with --rejoin, is seeded by
node 1 and catches up.

  s1_seed_start     node 1 killed the moment node 0's seed starts: no
                    promotion (test 1).
  s2_mid_catchup    node 1 killed halfway through node 0's catch-up (from
                    WAL files, 8 MiB/s, the client stopped so the backlog is
                    fixed): no promotion (test 2).
  s3_synced_fail    node 1 SYNCED, then killed: node 0 promoted, exact
                    (test 3).
  s4_restart_reseed the SYNCED standby restarted on its own data: READY
                    is kept or re-proven; then reseeded from an empty
                    directory: NOT READY recorded before the seed is served,
                    READY only once proven again; then a failover: exact
                    (test 4).
  s5_stale_synced   SYNCED of term 0 sent to every witness at term 1: refused,
                    nothing promoted when node 1 then dies (test 5).
  s6_writes_catchup writes at full rate all through node 0's catch-up: never
                    READY while node 0 is behind (sampled: node 1's
                    unacknowledged WAL past the client's window, or a disk
                    reader); READY while the writes continue; then a
                    failover is possible and every write is exact (test 6).
  s7_fail_at_synced node 1 killed the moment it says SYNCED: node 0
                    promoted, exact (test 7).
  s10_majority_loss two of three witnesses killed while node 0 catches up,
                    then node 1 killed: no promotion (test 10).
  s10b_one_witness_lost  the same with one witness killed: a majority could
                    promote, NOT READY stops it.

Test 8 (two failovers with a reseed, exact) is test_baseline_retry.py
q5_kill_kill / q5_kill_stop / q5_catchup_race(_late); test 9 (migration,
rejoin, failover: D1/D7) is test_fail_closed.py f19.

Every scenario prints one JSON line (and writes it to --json DIR).

Usage: test_sync_readiness.py BUILD_DIR [SCENARIO ...] [--json DIR]
"""
import json
import os
import re
import shutil
import signal
import subprocess
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

SESSION = 1000
WINDOW = 2 * 64 * 2  # the client's requests in flight, as records per shard pair
# Catch-up from WAL files: little kept in memory, and slowly, so that the
# windows these tests aim at last seconds.
# 8 MiB/s so the backlog drains in time. (Below about 2 MiB/s catch-up used
# to livelock against the ack timeout: docs/BASELINE_READINESS.md F5, fixed.)
THROTTLED = ("--replica-retain-mib", "1", "--catchup-mib-s", "8")
NO_PROMOTION_S = 8  # well past --failover-ms (1000) and the polls


class Bench:
    """net_bench with a session through both nodes, deposits only."""

    def __init__(self, pr, seconds, threads=2, depth=64, objects=2000):
        # The backlog scenarios run the client only until node 0 rejoins,
        # so its catch-up (throttled) has a fixed backlog to work through.
        self.out = f"{pr.work}/bench.out"
        args = [f"{BUILD}/net_bench", "--port", str(pr.to0_cli), "--endpoint", f"127.0.0.1:{pr.to0_cli}",
                "--endpoint", f"127.0.0.1:{pr.cli[1]}", "--session", str(SESSION), "--threads", str(threads),
                "--depth", str(depth), "--objects", str(objects), "--reads", "0", "--seconds", str(seconds),
                "--route", "owner", "--verify", "--retry-claim-ttl-s", "3600"]
        self.f = open(self.out, "w")
        self.p = subprocess.Popen(args, stdout=self.f, stderr=subprocess.STDOUT)
        pr.bench = self

    def running(self):
        return self.p.poll() is None

    def wait(self, timeout):
        try:
            rc = self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = self.p.wait()
        self.f.close()
        text = open(self.out).read()
        r = {"rc": rc}
        m = re.search(r"verify: (\d+) accounts, (\d+) deposits, (\d+) mismatches", text)
        if m:
            r.update(zip(("accounts", "deposits", "mismatches"), map(int, m.groups())))
        m = re.search(r"failovers (\d+)", text)
        if m:
            r["failovers"] = int(m[1])
        r["tail"] = text.strip().splitlines()[-4:]
        return r

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


def exact(b):
    return b.get("rc") == 0 and b.get("mismatches") == 0 and b.get("accounts", 0) > 0 and b.get("deposits", 0) > 0


def resources(pr, k):
    r, _ = th.ask(pr.adm[k], "RESOURCES", 3)
    f = th.fields(r)
    return {k: int(v) for k, v in f.items() if v.lstrip("-").isdigit()} if f else None


def role(pr, k):
    r, _ = th.ask(pr.adm[k], "HEALTH", 0.5)
    return th.fields(r).get("role")


def wait_state(pr, primary, timeout, degraded=None):
    """The chosen state once node `primary` is primary there (and answers as
    one), with `degraded` if given; None on timeout."""
    t_end = time.time() + timeout
    while time.time() < t_end:
        st = pr.state()
        if st and st[2] == primary and (degraded is None or st[3] == degraded):
            if role(pr, primary) in ("p", "n"):
                return st
        time.sleep(0.05)
    return None


def no_promotion(pr, k, seconds):
    """Watches for `seconds`: node k never primary, the chosen state never
    names it. Returns (held, states seen)."""
    seen, t_end = [], time.time() + seconds
    while time.time() < t_end:
        st = pr.state()
        if st and st not in seen:
            seen.append(st)
        if (st and st[2] == k) or role(pr, k) in ("p", "n"):
            return False, seen
        time.sleep(0.1)
    return True, seen


def first_failover(pr, res, rejoin_delay=0.0):
    """READY at term 0; node 0 killed; node 1 promoted to term 1 NOT READY;
    after rejoin_delay node 0 restarts with --rejoin (not waited for)."""
    st = pr.wait_ready(30, term=0)
    assert st, f"never READY at term 0: {pr.state()}"
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[0].p.wait(timeout=30)
    st = wait_state(pr, 1, 20)
    assert st, f"node 1 not promoted: {pr.state()}"
    res["promoted_state"] = st
    # A promotion records NOT READY for the new term.
    res["promotion_not_ready"] = st[1] == 1 and st[3] is True
    time.sleep(rejoin_delay)
    pr.old_nodes.append(pr.nodes[0])
    pr.nodes[0] = th.Proc([f"{BUILD}/snc_server"] + pr.args[0])


def rejoined(pr):
    assert pr.nodes[0].wait_for(r"rejoin: starting as its standby", 30), pr.nodes[0].text()
    assert pr.nodes[0].wait_for(r"standby, listening", 60), pr.nodes[0].text()


def keep_logs(name, pr):
    if not JSON_DIR:
        return
    os.makedirs(JSON_DIR, exist_ok=True)
    with open(f"{JSON_DIR}/{name}_logs.txt", "w") as f:
        for i, p in enumerate(pr.nodes):
            if p:
                f.write(f"==== node {i} (last start) ====\n" + "\n".join(p.lines) + "\n")
        for i, p in enumerate(pr.old_nodes):
            f.write(f"==== earlier node process {i} ====\n" + "\n".join(p.lines) + "\n")
        for i, w in enumerate(pr.w):
            f.write(f"==== witness {i} ====\n" + "\n".join(w.lines) + "\n")
        if pr.bench:
            f.write("==== client ====\n" + open(pr.bench.out).read())


def run(name, body, **pair_args):
    pr = th.Pair(**pair_args)
    pr.old_nodes, pr.bench = [], None
    res = None
    try:
        res = body(pr)
        return res
    finally:
        if pr.bench:
            pr.bench.stop()
        if res is None or not res.get("pass"):
            keep_logs(name, pr)
        pr.stop()


# ---- 1, 2, 10: no promotion of a standby that is not proven ------------------------------

def s1_seed_start(pr):
    res = {}
    # 200,000 accounts, so node 1's checkpoints (every second) make a seed
    # of tens of MB: still in flight when node 1 dies.
    b = Bench(pr, 60, objects=200000)
    time.sleep(8)
    first_failover(pr, res)
    # The seed starts when node 0 says it rejoins (it then connects to node
    # 1's seed port and copies the checkpoint).
    assert pr.nodes[0].wait_for(r"rejoin: starting as its standby", 30), pr.nodes[0].text()
    pr.nodes[1].signal(signal.SIGKILL)
    # Was the seed still in flight? Node 1 logs a seed once sent, node 0
    # once written and synced.
    time.sleep(0.2)  # let both logs drain up to the kill
    res["node0_lines_at_kill"] = pr.nodes[0].lines[-3:]
    res["seed_sent"] = any("seed of checkpoint" in l and "sent" in l for l in pr.nodes[1].lines)
    res["seeded"] = any(l.startswith("snc: seeded from") for l in pr.nodes[0].lines)
    held, seen = no_promotion(pr, 0, NO_PROMOTION_S)
    res["no_promotion"], res["states"] = held, seen
    res["witness_still_not_ready"] = bool(seen) and seen[-1][1] == 1 and seen[-1][3] is True
    b.stop()
    res["pass"] = (res["promotion_not_ready"] and held and res["witness_still_not_ready"] and
                   not res["seed_sent"] and not res["seeded"])
    return res


def catching_up(pr, res, frac):
    """Waits until node 0, rejoined, has caught up `frac` of the backlog it
    started with (node 1's unacknowledged WAL), from WAL files."""
    rejoined(pr)
    peak, series, t_end = 0, [], time.time() + 60
    while time.time() < t_end:
        r = resources(pr, 1)
        if r:
            u = r.get("wal_unacked", 0)
            series.append((u, r.get("disk_readers", 0)))
            peak = max(peak, u)
            if peak > 20 * WINDOW and u <= peak * (1 - frac) and r.get("disk_readers", 0) > 0:
                res["backlog_peak"], res["backlog_at_kill"] = peak, u
                return True
        time.sleep(0.1)
    res["backlog_series"] = series[-20:]
    return False


def s2_mid_catchup(pr):
    res = {}
    b = Bench(pr, 9)
    time.sleep(2)
    first_failover(pr, res, rejoin_delay=4.0)  # node 1 alone: a backlog
    res["halfway"] = catching_up(pr, res, 0.5)
    assert res["halfway"], res
    pr.nodes[1].signal(signal.SIGKILL)
    held, seen = no_promotion(pr, 0, NO_PROMOTION_S)
    res["no_promotion"], res["states"] = held, seen
    res["node0_role"] = role(pr, 0)
    b.stop()
    res["pass"] = res["promotion_not_ready"] and held and res["node0_role"] == "s"
    return res


def s10_majority_loss(pr, n_killed=2):
    """n_killed witnesses lost: 2 (a majority: nothing can be chosen) or 1
    (a majority remains, so only NOT READY stops the promotion)."""
    res = {"witnesses_killed": n_killed}
    b = Bench(pr, 9)
    time.sleep(2)
    first_failover(pr, res, rejoin_delay=4.0)
    res["halfway"] = catching_up(pr, res, 0.2)
    assert res["halfway"], res
    for w in pr.w[:n_killed]:
        w.signal(signal.SIGKILL)
    pr.nodes[1].signal(signal.SIGKILL)
    held, seen = no_promotion(pr, 0, NO_PROMOTION_S)
    res["no_promotion"] = held
    res["node0_role"] = role(pr, 0)
    b.stop()
    res["pass"] = res["promotion_not_ready"] and held and res["node0_role"] == "s"
    return res


# ---- 5: a stale SYNCED --------------------------------------------------------------------

def s5_stale_synced(pr):
    res = {}
    b = Bench(pr, 9)
    time.sleep(2)
    first_failover(pr, res, rejoin_delay=4.0)
    res["halfway"] = catching_up(pr, res, 0.2)
    assert res["halfway"], res
    # The old term's SYNCED (what node 0 said at term 0), replayed.
    res["stale_replies"] = [th.ask(p, f"SYNCED {th.UUID} 0", 3)[0] for p in pr.wit]
    res["refused"] = all(r and r.startswith("NO term 0 was replaced by 1") for r in res["stale_replies"])
    res["state_after"] = pr.state()
    pr.nodes[1].signal(signal.SIGKILL)
    held, seen = no_promotion(pr, 0, NO_PROMOTION_S)
    res["no_promotion"] = held
    res["node0_role"] = role(pr, 0)
    b.stop()
    res["pass"] = (res["promotion_not_ready"] and res["refused"] and res["state_after"][3] is True and held and
                   res["node0_role"] == "s")
    return res


# ---- 3, 7: SYNCED, then the primary fails: exact ------------------------------------------

def synced_then_fail(pr, at_once):
    res = {}
    b = Bench(pr, 40)
    time.sleep(2)
    first_failover(pr, res)
    rejoined(pr)
    t = time.time()
    if at_once:
        # The moment node 1 has its SYNCED answered.
        assert pr.nodes[1].wait_for(r"witness: standby back in sync", 60), pr.nodes[1].text()
        pr.nodes[1].signal(signal.SIGKILL)
        res["synced_s"] = round(time.time() - t, 2)
        res["state_at_kill"] = pr.state()
    else:
        st = pr.wait_ready(60, term=1)
        assert st, f"never READY at term 1: {pr.state()}"
        res["synced_s"] = round(time.time() - t, 2)
        time.sleep(2)
        res["state_at_kill"] = pr.state()
        pr.nodes[1].signal(signal.SIGKILL)
    assert b.running(), "the client finished before the second failure"
    t = time.time()
    st = wait_state(pr, 0, 20)
    res["second_promoted_s"] = round(time.time() - t, 2) if st else None
    res["term_2"] = st[1] if st else None
    res["bench"] = r = b.wait(180)
    res["pass"] = res["promotion_not_ready"] and bool(st) and st[1] == 2 and exact(r)
    return res


# ---- 4: restart, then reseed, of a SYNCED standby -----------------------------------------

def s4_restart_reseed(pr):
    res = {}
    b = Bench(pr, 60)
    time.sleep(2)
    assert pr.wait_ready(30, term=0)
    standby = [f"{BUILD}/snc_server", "--standby", str(pr.b + 20)] + pr.args[1]
    # a. Restarted on its own data (inside --replica-timeout-ms, which this
    #    scenario sets to 5 s): it still holds every acknowledged write.
    pr.nodes[1].signal(signal.SIGKILL)
    pr.nodes[1].p.wait(timeout=30)
    pr.old_nodes.append(pr.nodes[1])
    pr.nodes[1] = th.Proc(standby)
    assert th.wait_port(pr.adm[1], proc=pr.nodes[1]), pr.nodes[1].text()
    states, t_end = [], time.time() + 30
    while time.time() < t_end:
        st = pr.state()
        if st and st not in states:
            states.append(st)
        r = resources(pr, 0)
        if st and not st[3] and r and r.get("wal_unacked", 1 << 60) < WINDOW and r.get("disk_readers", 1) == 0:
            break
        time.sleep(0.1)
    res["restart_states"] = states
    res["restart_ready"] = bool(states) and not states[-1][3] and states[-1][1] == 0
    res["restart_regressed"] = any("below the acknowledged" in l for l in pr.nodes[0].lines)
    # b. Reseeded: its directory replaced by a seed from node 0.
    pr.nodes[1].signal(signal.SIGKILL)
    pr.nodes[1].p.wait(timeout=30)
    pr.old_nodes.append(pr.nodes[1])
    shutil.rmtree(f"{pr.work}/n1")
    n_lines = len(pr.nodes[0].lines)
    pr.nodes[1] = th.Proc(standby + ["--seed-from", f"127.0.0.1:{pr.b + 70}"])
    states, t_end, seeded_at = [], time.time() + 60, None
    while time.time() < t_end:
        st = pr.state()
        if st and st not in states:
            states.append(st)
        if seeded_at is None and any("seeded from" in l for l in pr.nodes[1].lines):
            seeded_at = len(states)
        if seeded_at is not None and st and not st[3] and states.index(st) >= seeded_at:
            break
        time.sleep(0.05)
    res["reseed_states"] = states
    later = pr.nodes[0].lines[n_lines:]
    gate = [i for i, l in enumerate(later) if "recorded NOT READY (a seed is asked for)" in l]
    sent = [i for i, l in enumerate(later) if "seed of checkpoint" in l and "sent" in l]
    res["seed_gate"] = [later[i] for i in gate[:1]]
    # In node 0's own log: NOT READY recorded before the seed was sent.
    res["not_ready_before_seed"] = bool(gate) and bool(sent) and gate[0] < sent[0]
    res["not_ready_seen"] = any(s[3] for s in states)
    res["ready_again"] = bool(states) and not states[-1][3] and states[-1][1] == 0
    # c. The failover now: exact.
    pr.nodes[0].signal(signal.SIGKILL)
    st = wait_state(pr, 1, 20)
    res["promoted"] = st
    res["bench"] = r = b.wait(180)
    res["pass"] = (res["restart_ready"] and res["not_ready_before_seed"] and res["not_ready_seen"] and
                   res["ready_again"] and bool(st) and exact(r))
    return res


# ---- 6: writes during catch-up cannot make SYNCED early ------------------------------------

def s6_writes_catchup(pr):
    res = {}
    # Writes all along, at full rate; node 1 runs alone 4 s first, so
    # node 0 has a backlog to catch up while they continue.
    b = Bench(pr, 40)
    time.sleep(2)
    first_failover(pr, res, rejoin_delay=4.0)
    rejoined(pr)
    early, samples, ready_at, t_end = [], 0, None, time.time() + 50
    while time.time() < t_end:
        st = pr.state()
        r = resources(pr, 1)
        if not r or not st:
            time.sleep(0.05)
            continue
        samples += 1
        if samples % 10 == 0:
            res.setdefault("series", []).append((r.get("wal_unacked"), r.get("disk_readers"), r.get("degraded"),
                                                 r.get("disk_started"), st[3]))
        behind = r.get("wal_unacked", 0) > WINDOW or r.get("disk_readers", 0) > 0
        if st[1] == 1 and not st[3]:
            ready_at = {"wal_unacked": r.get("wal_unacked"), "disk_readers": r.get("disk_readers"),
                        "client_running": b.running()}
            # The resources are read after the state: node 0 still behind
            # once READY is on record would be an early SYNCED.
            if behind:
                early.append(ready_at)
            break
        time.sleep(0.05)
    res["samples"], res["ready_at"], res["early_ready"] = samples, ready_at, early
    res["synced_lines"] = [l for l in pr.nodes[1].lines if "witness:" in l]
    res["bench"] = r = b.wait(120)
    # READY at term 1 makes node 0 promotable.
    pr.nodes[1].signal(signal.SIGKILL)
    st = wait_state(pr, 0, 20)
    res["promoted_after"] = bool(st)
    res["pass"] = (res["promotion_not_ready"] and ready_at is not None and ready_at["client_running"] and
                   not early and exact(r) and bool(st))
    return res


SCEN = {
    # Checkpoints every second, so the seed is a large checkpoint (not the
    # empty first one) and still in flight when node 1 dies.
    # Under TSan, node 1's two shards share one CPU with a checkpoint that
    # never stops (1.8 s each, every second): STATUS, which needs every
    # shard to run a task within the witness's poll, answers in 60-100 ms
    # and the 100 ms eligibility check sometimes misses. A sanitizer-only
    # allowance, as RSS_PER_RETAINED is; release keeps --poll-ms 100.
    "s1_seed_start": lambda: run("s1_seed_start", s1_seed_start, objects="1000000", extra=("--checkpoint-s", "1"),
                                 poll_ms=300 if th.sanitizer(BUILD) == "thread" else th.POLL_MS),
    "s2_mid_catchup": lambda: run("s2_mid_catchup", s2_mid_catchup, extra=THROTTLED),
    "s3_synced_fail": lambda: run("s3_synced_fail", lambda pr: synced_then_fail(pr, False)),
    "s4_restart_reseed": lambda: run("s4_restart_reseed", s4_restart_reseed, extra=("--replica-timeout-ms", "5000")),
    "s5_stale_synced": lambda: run("s5_stale_synced", s5_stale_synced, extra=THROTTLED),
    "s6_writes_catchup": lambda: run("s6_writes_catchup", s6_writes_catchup),
    "s7_fail_at_synced": lambda: run("s7_fail_at_synced", lambda pr: synced_then_fail(pr, True)),
    "s10_majority_loss": lambda: run("s10_majority_loss", s10_majority_loss, extra=THROTTLED),
    "s10b_one_witness_lost": lambda: run("s10b_one_witness_lost", lambda pr: s10_majority_loss(pr, 1),
                                         extra=THROTTLED),
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
        print(("PASS " if r.get("pass") else "FAIL ") + name + " " + json.dumps(r, default=str)[:3000], flush=True)
        if JSON_DIR:
            os.makedirs(JSON_DIR, exist_ok=True)
            with open(f"{JSON_DIR}/{name}.json", "w") as f:
                json.dump(r, f, indent=1, default=str)
        if not r.get("pass"):
            failed.append(name)
    print("test_sync_readiness:", "FAILED " + str(failed) if failed else "OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
