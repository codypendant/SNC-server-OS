#!/usr/bin/env python3
"""Pre-baseline tests T-Q1 and T-Q5 (docs/EXPERIMENTAL_BASELINE.md section 5)
for Profile B (SYNC HA): real processes, the sync pair of
tests/test_health.py (two snc_server nodes, each the other's primary or
standby, node 0 restarting with --rejoin; three replicated snc_witness on
HEALTH, --failover-ms 1000, --poll-ms 100), and the shipped client
(net::Client through net_bench --session with both nodes as endpoints,
request timeout 5000 ms, failover deadline 30000 ms) sending deposits all
along. net_bench --verify reads every account at the end: its balance must
be its start plus the deposits sent, exactly (one application per request).

T-Q1: the age of the oldest resent request when it is resent (net::Client
max_resend_age_ms: time since it was first sent), for each path that makes
the client resend. A resend is answered from its session only while the
session lives (TTL 3600 s, expired only at a checkpoint, timed from the
session's last applied request), so the contract needs the age far below
the TTL on every campaign path.

  q1_kill      SIGKILL the primary: promotion, resend to the new primary.
  q1_stop      SIGSTOP the primary (hung): request timeout, promotion.
  q1_partition every path to and from the primary cut (proxies): request
               timeout, promotion; after the heal the old primary is fenced.
  q1_flap      the primary's paths cut for 0.5 s then healed, five times
               (no promotion): the client reconnects to the same primary
               and resends each time.
  q1_stall     shard 0 of the primary stalled 7 s (DEBUG STALL; witnesses on
               HEALTH do not promote): request timeouts, resends to the same
               primary.
  q1_blocked   the standby and every witness SIGSTOPped, so the sync primary
               is blocked without a DEGRADE grant for 40 s (a control-plane
               outage): the client times out and resends every cycle; the
               age grows with the outage. Reported, not bounded by
               failover_ms + request_timeout_ms.
  q1_failed    both nodes SIGKILLed: no primary answers; the client gives up
               after its failover deadline (the application gets an error;
               nothing is resent later).

T-Q5: two consecutive failovers during a resend storm.

  q5_kill_kill node 0 (primary) SIGKILLed, node 1 promoted, the client
               resends there; node 0 restarts with --rejoin, is seeded and
               catches up (node 1's unacknowledged WAL down to the client window), then node 1 is
               SIGKILLed with the client's full window in flight: node 0
               promoted, the client resends again. Exact balances.
  q5_kill_stop the same, the second failure a SIGSTOP (request timeout path);
               after SIGCONT the old primary is fenced.
  q5_catchup_race  as q5_kill_kill, but node 1 is SIGKILLed as soon as node
               0 is its standby again, while node 0 still catches up. The
               contract: node 0 is not promoted while it lacks acknowledged
               writes, or every acknowledged write is there.
  q5_catchup_race_late  the same with node 0 restarted 2 s after the
               promotion, past --replica-timeout-ms (500): node 1 first
               runs alone under a DEGRADE grant.

F2 / R11: every client claims session deduplication against the server's
TTL (net_bench --retry-claim-ttl-s 3600: the oldest resend must be at most
a tenth of it, or the run is INVALID, exit 3).

  r11_guard    q1_kill claiming a TTL of 1 s: the guard refuses the run.

Every scenario prints one JSON line (and writes it to --json DIR).

Usage: test_baseline_retry.py BUILD_DIR [SCENARIO ...] [--json DIR]
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

TTL_S = 3600
# What the failover paths without a control-plane outage are checked
# against: failover_ms (30000) + request_timeout_ms (5000). It is an expected
# value for those paths, not the R11 contract: that is "every in-doubt resend
# before session expiry", checked by net_bench --retry-claim-ttl-s (r11_guard).
BOUND_MS = 35000
SESSION = 1000


class Bench:
    """net_bench with a session through both nodes, deposits only."""

    def __init__(self, pr, seconds, first_port=None, threads=2, depth=32, objects=2000, claim_ttl_s=TTL_S):
        self.out = f"{pr.work}/bench.out"
        port = first_port or pr.to0_cli
        args = [f"{BUILD}/net_bench", "--port", str(port), "--endpoint", f"127.0.0.1:{pr.to0_cli}",
                "--endpoint", f"127.0.0.1:{pr.cli[1]}", "--session", str(SESSION), "--threads", str(threads),
                "--depth", str(depth), "--objects", str(objects), "--reads", "0", "--seconds", str(seconds),
                "--route", "owner", "--verify", "--retry-claim-ttl-s", str(claim_ttl_s)]
        self.f = open(self.out, "w")
        self.p = subprocess.Popen(args, stdout=self.f, stderr=subprocess.STDOUT)
        self.t0 = time.time()
        pr.bench = self

    def running(self):
        """True once the run phase started (creates done): give it time."""
        return self.p.poll() is None

    def wait(self, timeout):
        try:
            rc = self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = self.p.wait()
        self.f.close()
        text = open(self.out).read()
        r = {"rc": rc, "exited_after_s": round(time.time() - self.t0, 2)}
        m = re.search(r"failovers (\d+)\s+replayed (\d+)\s+timeouts (\d+)\s+resends (\d+)\s+max_resend_age_ms (\d+)", text)
        if m:
            r.update(zip(("failovers", "replayed", "timeouts", "resends", "max_resend_age_ms"), map(int, m.groups())))
        m = re.search(r"verify: (\d+) accounts, (\d+) deposits, (\d+) mismatches", text)
        if m:
            r.update(zip(("accounts", "deposits", "mismatches"), map(int, m.groups())))
        m = re.search(r"ops/s (\d+)\s+errors (\d+)", text)
        if m:
            r["ops_s"], r["errors"] = int(m[1]), int(m[2])
        m = re.search(r"retry claim: max_resend_age_ms \d+, limit (\d+) .*: (\w+)", text)
        if m:
            r["retry_claim_limit_ms"], r["retry_claim"] = int(m[1]), m[2]
        r["tail"] = text.strip().splitlines()[-6:]
        return r

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


def wait_state(pr, primary, timeout, degraded=None):
    t_end = time.time() + timeout
    while time.time() < t_end:
        st = pr.state()
        if st and st[2] == primary and (degraded is None or st[3] == degraded):
            r, _ = th.ask(pr.adm[primary], "HEALTH", 0.5)
            if th.fields(r).get("role") in ("p", "n"):
                return st
        time.sleep(0.05)
    return None


def exact(b):
    return b.get("rc") == 0 and b.get("mismatches") == 0 and b.get("accounts", 0) > 0 and b.get("deposits", 0) > 0


def keep_logs(name, pr, bench):
    """Every process's output (and the client's), as evidence."""
    if not JSON_DIR:
        return
    os.makedirs(JSON_DIR, exist_ok=True)
    with open(f"{JSON_DIR}/{name}_logs.txt", "w") as f:
        for i, p in enumerate(pr.nodes):
            if p:
                f.write(f"==== node {i} (last start) ====\n" + "\n".join(p.lines) + "\n")
        for i, w in enumerate(pr.w):
            f.write(f"==== witness {i} ====\n" + "\n".join(w.lines) + "\n")
        if bench:
            f.write("==== net_bench ====\n" + open(bench.out).read()[-20000:])
        for i, p in enumerate(getattr(pr, "old_nodes", [])):
            f.write(f"==== earlier node process {i} ====\n" + "\n".join(p.lines) + "\n")


def run(name, body, **pair_args):
    pr = th.Pair(**pair_args)
    pr.old_nodes = []
    pr.bench = None
    res = None
    try:
        res, _ = body(pr)
        return res
    finally:
        bench = pr.bench
        if bench:
            bench.stop()
        if res is None or not res.get("pass"):
            keep_logs(name, pr, bench)
        pr.stop()


def started(bench, settle=3.0):
    time.sleep(settle)
    assert bench.running(), open(bench.out).read()


# ---- T-Q1 --------------------------------------------------------------------------------

def q1_kill(pr):
    b = Bench(pr, 15)
    started(b)
    t = time.time()
    pr.nodes[0].signal(signal.SIGKILL)
    st = wait_state(pr, 1, 15)
    res = {"promoted_s": round(time.time() - t, 2) if st else None}
    res["bench"] = r = b.wait(120)
    res["pass"] = bool(st) and exact(r) and r.get("resends", 0) > 0 and r["max_resend_age_ms"] < BOUND_MS
    return res, b


def q1_stop(pr):
    b = Bench(pr, 20)
    started(b)
    t = time.time()
    pr.nodes[0].signal(signal.SIGSTOP)
    st = wait_state(pr, 1, 15)
    res = {"promoted_s": round(time.time() - t, 2) if st else None}
    res["bench"] = r = b.wait(120)
    pr.nodes[0].signal(signal.SIGCONT)
    res["old_primary_role"] = old_role(pr, 0)
    res["pass"] = (bool(st) and exact(r) and r.get("timeouts", 0) > 0 and r["max_resend_age_ms"] < BOUND_MS
                   and res["old_primary_role"] in ("f", None))
    return res, b


def old_role(pr, k, timeout=15):
    """The old primary's HEALTH role once it has seen the newer term (f), or
    None if it no longer answers."""
    t_end = time.time() + timeout
    role = None
    while time.time() < t_end:
        r, _ = th.ask(pr.to0_adm if k == 0 else pr.adm[k], "HEALTH", 0.5)
        role = th.fields(r).get("role") if r else None
        if role in ("f", None):
            return role
        time.sleep(0.1)
    return role


def q1_partition(pr):
    b = Bench(pr, 25)
    started(b)
    t = time.time()
    for x in pr.proxies:
        x.cut()
    st = wait_state(pr, 1, 15)
    res = {"promoted_s": round(time.time() - t, 2) if st else None}
    res["bench"] = r = b.wait(120)
    for x in pr.proxies:
        x.heal()
    res["old_primary_role"] = old_role(pr, 0, 30)
    res["pass"] = (bool(st) and exact(r) and r.get("resends", 0) > 0 and r["max_resend_age_ms"] < BOUND_MS
                   and res["old_primary_role"] in ("f", None))
    return res, b


def q1_flap(pr):
    b = Bench(pr, 20)
    started(b)
    for _ in range(5):
        for x in pr.proxies:
            x.cut()
        time.sleep(0.5)
        for x in pr.proxies:
            x.heal()
        time.sleep(2.0)
    st = pr.state()
    res = {"primary_after": st[2] if st else None, "term_after": st[1] if st else None}
    res["bench"] = r = b.wait(120)
    res["pass"] = (exact(r) and r.get("failovers", 0) >= 2 and r.get("resends", 0) > 0
                   and r["max_resend_age_ms"] < BOUND_MS and res["term_after"] == 0)
    return res, b


def q1_stall(pr):
    b = Bench(pr, 20)
    started(b)
    r0, _ = th.ask(pr.adm[0], "DEBUG STALL 0 7000")
    assert r0 == "OK", r0
    time.sleep(8)
    st = pr.state()
    res = {"primary_after": st[2] if st else None}
    res["bench"] = r = b.wait(120)
    res["pass"] = (exact(r) and r.get("timeouts", 0) > 0 and r["max_resend_age_ms"] < BOUND_MS
                   and res["primary_after"] == 0)
    return res, b


BLOCK_S = 40


def q1_blocked(pr):
    b = Bench(pr, BLOCK_S + 25)
    started(b)
    for w in pr.w:
        w.signal(signal.SIGSTOP)
    pr.nodes[1].signal(signal.SIGSTOP)
    t = time.time()
    time.sleep(BLOCK_S)
    for w in pr.w:
        w.signal(signal.SIGCONT)
    granted = pr.nodes[0].wait_for(r"witness: granted", 15)
    res = {"blocked_s": round(time.time() - t, 2), "granted": granted is not None}
    pr.nodes[1].signal(signal.SIGCONT)
    res["bench"] = r = b.wait(180)
    age = r.get("max_resend_age_ms", 0)
    res["age_over_blocked"] = round(age / 1000 - BLOCK_S, 2)
    # The finding: the age follows the outage, past failover_ms +
    # request_timeout_ms; the contract (TTL) holds while outages stay far
    # below it. Asserted: exact, and the age within the outage plus one
    # reconnect cycle.
    res["pass"] = exact(r) and res["granted"] and age < (BLOCK_S + 20) * 1000 and age < TTL_S * 1000
    return res, b


def q1_failed(pr):
    # The run ends before the deadline; the process exits once every
    # thread's failover has given up.
    b = Bench(pr, 6)
    started(b)
    t = time.time()
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[1].signal(signal.SIGKILL)
    r = b.wait(120)
    res = {"bench": r, "gave_up_after_s": round(time.time() - t, 2)}
    res["pass"] = r["rc"] != 0 and any("no primary answered" in l for l in r["tail"]) and \
        res["gave_up_after_s"] < BOUND_MS / 1000 + 5
    return res, b


# ---- T-Q5 --------------------------------------------------------------------------------

def unacked(pr, k):
    """RESOURCES wal_unacked at node k: durable records its standby has not
    acknowledged, summed over shards (None if no answer)."""
    r, _ = th.ask(pr.adm[k], "RESOURCES", 3)
    v = th.fields(r).get("wal_unacked")
    return int(v) if v is not None else None


def two_failovers(pr, second, caught_up=True, rejoin_delay=0.0):
    """second: kill or stop. caught_up: the second failure waits until node
    0 has caught up (node 1's unacknowledged WAL down to the client's
    window); otherwise it comes as soon as node 0 is a standby again, while
    it still catches up (the window the witnesses' eligibility must cover).
    rejoin_delay: seconds between the promotion and node 0's restart (past
    --replica-timeout-ms, node 1 first runs alone under a DEGRADE grant)."""
    b = Bench(pr, 45, threads=2, depth=64)
    started(b)
    res = {"second_failure_waits_for_catch_up": caught_up}
    t = time.time()
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[0].p.wait(timeout=30)
    st = wait_state(pr, 1, 15)
    assert st, "node 1 not promoted"
    res["first_promoted_s"] = round(time.time() - t, 2)
    res["term_1"] = st[1]
    time.sleep(rejoin_delay)
    res["rejoin_delay_s"] = rejoin_delay
    res["witness_state_at_rejoin"] = pr.state()
    # Node 0 restarts with its own command line: --rejoin finds node 1 as
    # primary, moves its directory aside, starts as its standby, is seeded.
    t = time.time()
    pr.old_nodes.append(pr.nodes[0])
    pr.nodes[0] = th.Proc([f"{BUILD}/snc_server"] + pr.args[0])
    assert pr.nodes[0].wait_for(r"rejoin: starting as its standby", 30), pr.nodes[0].text()
    assert pr.nodes[0].wait_for(r"standby, listening", 30), pr.nodes[0].text()
    res["rejoined_standby_s"] = round(time.time() - t, 2)
    res["witness_state_at_second_failure"] = None
    if caught_up:
        ok_n, t_end, series = 0, time.time() + 120, []
        while ok_n < 3 and time.time() < t_end:
            u = unacked(pr, 1)
            series.append(u)
            ok_n = ok_n + 1 if u is not None and u < 2 * 64 * 2 else 0
            time.sleep(0.2)
        assert ok_n >= 3, ("node 0 never caught up", series[-10:])
        res["caught_up_s"] = round(time.time() - t, 2)
        # Caught up is not yet READY (docs/SYNC_READINESS.md): node 1 must
        # still prove it with SYNCED. A second failure before that is the
        # catching-up case, which q5_catchup_race covers.
        assert pr.wait_ready(30, term=1), ("never READY at term 1 after catch-up", pr.state())
        res["ready_s"] = round(time.time() - t, 2)
    res["node1_unacked_at_second_failure"] = unacked(pr, 1)
    res["witness_state_at_second_failure"] = pr.state()
    assert b.running(), "the client finished before the second failover"
    t = time.time()
    if second == "kill":
        pr.nodes[1].signal(signal.SIGKILL)
    else:
        pr.nodes[1].signal(signal.SIGSTOP)
    st = wait_state(pr, 0, 20)
    res["second_promoted_s"] = round(time.time() - t, 2) if st else None
    res["term_2"] = st[1] if st else None
    res["bench"] = r = b.wait(180)
    if caught_up:
        ok = bool(st) and exact(r) and r.get("max_resend_age_ms", BOUND_MS) < BOUND_MS
        # Each of the 2 threads followed both failovers.
        ok = ok and r.get("failovers", 0) >= 4 and r.get("resends", 0) > 0
    else:
        # The contract: either node 0 is not promoted while it lacks
        # acknowledged writes (the group waits), or it is and every
        # acknowledged write is there. Never acknowledged writes missing.
        # B2 fixed (docs/SYNC_READINESS.md): node 0 is not promoted while
        # it catches up; the witnesses keep NOT READY for term 1.
        ws = res["witness_state_at_second_failure"]
        ok = (not st) and bool(ws) and ws[1] == 1 and ws[3] is True
        res["no_promotion_while_catching_up"] = ok
    if second == "stop":
        pr.nodes[1].signal(signal.SIGCONT)
        r1, _ = th.ask(pr.adm[1], "HEALTH", 1)
        t_end = time.time() + 15
        while th.fields(r1).get("role") not in ("f",) and time.time() < t_end:
            time.sleep(0.1)
            r1, _ = th.ask(pr.adm[1], "HEALTH", 1)
        res["old_primary_role"] = th.fields(r1).get("role")
        ok = ok and res["old_primary_role"] == "f"
    res["pass"] = ok
    return res, b


# ---- F2 / R11: the run-validity guard ----------------------------------------------------

def r11_guard(pr):
    """net_bench --retry-claim-ttl-s refuses a run whose oldest resend is
    not far below the claimed TTL: q1_kill with a claimed TTL of 1 s (limit
    100 ms) is INVALID (exit 3) though every balance is exact."""
    b = Bench(pr, 15, claim_ttl_s=1)
    started(b)
    pr.nodes[0].signal(signal.SIGKILL)
    st = wait_state(pr, 1, 15)
    res = {"promoted": bool(st)}
    res["bench"] = r = b.wait(120)
    res["pass"] = (bool(st) and r.get("rc") == 3 and r.get("retry_claim") == "INVALID" and
                   r.get("mismatches") == 0 and r.get("max_resend_age_ms", 0) > 100)
    return res, b


# ---- B3: catch-up from disk over a link that keeps dropping -----------------------------

def b3_stalled_catchup(pr):
    """docs/BASELINE_READINESS.md B3. Node 1, promoted, runs alone for 8 s
    with 16 MiB of in-memory retention per shard, so the rejoined node 0
    catches up from WAL files. Node 0 is then SIGSTOPped 0.7 s of every
    1 s for 20 s: it keeps missing the 500 ms ack timeout, so node 1 cancels
    its catch-up reader and starts another, again and again. The contract:
    a cancelled reader stops producing, so readers and read-ahead stay
    bounded (16 MiB per live reader) and so does RSS; once the stalls end
    the catch-up completes, no reader is left, and a failover to node 0
    then keeps every acknowledged write."""
    b = Bench(pr, 75, threads=2, depth=64)
    started(b)
    res = {}
    pr.nodes[0].signal(signal.SIGKILL)
    pr.nodes[0].p.wait(timeout=30)
    assert wait_state(pr, 1, 15), "node 1 not promoted"
    time.sleep(8)
    pr.old_nodes.append(pr.nodes[0])
    pr.nodes[0] = th.Proc([f"{BUILD}/snc_server"] + pr.args[0])
    assert pr.nodes[0].wait_for(r"standby, listening", 30), pr.nodes[0].text()
    series = []

    def sample():
        r, _ = th.ask(pr.adm[1], "RESOURCES", 3)
        f = th.fields(r)
        if f:
            series.append({k: int(f[k]) for k in ("rss_kb", "repl_retained", "wal_unacked", "disk_readers",
                                                  "disk_queued", "disk_queued_hwm", "disk_started",
                                                  "disk_stopped_on_cancel")})
        return f

    t_end = time.time() + 20
    while time.time() < t_end:
        pr.nodes[0].signal(signal.SIGSTOP)
        time.sleep(0.7)
        sample()
        pr.nodes[0].signal(signal.SIGCONT)
        time.sleep(0.3)
        sample()
    ok_n, t_end = 0, time.time() + 90
    while ok_n < 3 and time.time() < t_end:
        f = sample()
        u = int(f["wal_unacked"]) if f else None
        ok_n = ok_n + 1 if u is not None and u < 2 * 64 * 2 and int(f["disk_readers"]) == 0 else 0
        time.sleep(0.2)
    res["caught_up"] = ok_n >= 3
    last = series[-1] if series else {}
    res["peak_rss_mib"] = max(s["rss_kb"] for s in series) // 1024
    res["max_disk_readers"] = max(s["disk_readers"] for s in series)
    res["disk_queued_hwm_mib"] = round(last.get("disk_queued_hwm", 0) / (1 << 20), 1)
    res["disk_started"] = last.get("disk_started")
    res["disk_stopped_on_cancel"] = last.get("disk_stopped_on_cancel")
    res["at_end"] = last
    res["stopped_acking"] = sum("stopped acking" in l for l in pr.nodes[1].lines)
    # Caught up is not yet READY (docs/SYNC_READINESS.md 7): node 1 must
    # still prove it, and until then the witnesses rightly refuse to promote
    # node 0. The failover this scenario checks follows READY, as in q5.
    res["witness_state_when_caught_up"] = pr.state()
    t = time.time()
    res["ready"] = bool(pr.wait_ready(30, term=res["witness_state_when_caught_up"][1]
                                      if res["witness_state_when_caught_up"] else None))
    res["ready_after_caught_up_s"] = round(time.time() - t, 2)
    pr.nodes[1].signal(signal.SIGKILL)
    st = wait_state(pr, 0, 20)
    res["second_promoted"] = bool(st)
    res["bench"] = r = b.wait(180)
    shards = 2
    res["checks"] = {
        "cancel_path_exercised": res["stopped_acking"] >= 5 and (res["disk_stopped_on_cancel"] or 0) >= 2,
        # Each shard: its current reader, plus at most one that is stopping.
        "readers_bounded": res["max_disk_readers"] <= 2 * shards,
        "read_ahead_bounded": last.get("disk_queued_hwm", 1 << 62) <= 2 * shards * (17 << 20),
        # Release measured 283 MiB without stalls and 2,059 MiB with them
        # before the fix (289 MiB after); sanitizer builds multiply RSS.
        "rss_bounded": res["peak_rss_mib"] < (512 if th.sanitizer(BUILD) is None else 4096),
        "caught_up_no_reader_left": res["caught_up"] and last.get("disk_readers") == 0 and
        last.get("disk_queued") == 0,
        "ready_before_failover": res["ready"],
        "exact_after_failover": res["second_promoted"] and exact(r),
    }
    res["series"] = series[::4]
    res["pass"] = all(res["checks"].values())
    return res, b


SCEN = {
    "b3_stalled_catchup": lambda: run("b3_stalled_catchup", b3_stalled_catchup,
                                      extra=("--replica-retain-mib", "16")),
    "q1_kill": lambda: run("q1_kill", q1_kill),
    "r11_guard": lambda: run("r11_guard", r11_guard),
    "q1_stop": lambda: run("q1_stop", q1_stop),
    "q1_partition": lambda: run("q1_partition", q1_partition, proxied=True),
    "q1_flap": lambda: run("q1_flap", q1_flap, proxied=True),
    "q1_stall": lambda: run("q1_stall", q1_stall),
    "q1_blocked": lambda: run("q1_blocked", q1_blocked),
    "q1_failed": lambda: run("q1_failed", q1_failed),
    "q5_kill_kill": lambda: run("q5_kill_kill", lambda pr: two_failovers(pr, "kill")),
    "q5_kill_stop": lambda: run("q5_kill_stop", lambda pr: two_failovers(pr, "stop")),
    "q5_catchup_race": lambda: run("q5_catchup_race", lambda pr: two_failovers(pr, "kill", caught_up=False)),
    "q5_catchup_race_late": lambda: run("q5_catchup_race_late",
                                        lambda pr: two_failovers(pr, "kill", caught_up=False, rejoin_delay=2.0)),
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
        print(("PASS " if r.get("pass") else "FAIL ") + name + " " + json.dumps(r)[:3000], flush=True)
        if JSON_DIR:
            os.makedirs(JSON_DIR, exist_ok=True)
            with open(f"{JSON_DIR}/{name}.json", "w") as f:
                json.dump(r, f, indent=1)
        if not r.get("pass"):
            failed.append(name)
    print("test_baseline_retry:", "FAILED " + str(failed) if failed else "OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
