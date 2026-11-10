#!/usr/bin/env python3
"""D.2 cross-node exchange accounting with real processes
(docs/CROSS_NODE_EXCHANGE_DESIGN.md 8.1: X6, X7, X7b, X9). The other X
tests run engines in one process (tests/test_xnode.cpp).

The cluster is tests/test_fail_closed.py's: two replication groups, each a
sync primary (nodes 1 and 3) and its standby (2 and 4), three snc_witness
processes holding placement v2. The client is the shipped ClusterClient
(snc_load run, transfers between accounts spread over both groups, so most
transfers cross nodes). At the end, once every transfer has settled (AUDIT
pending 0 on both primaries), snc_load check compares every account with
the acknowledged transfers, and every primary's xnode counts are 0 (remote
live, held for peers, END owed) with no underflow.

  x6   crash of o: node 1 (group 0's primary) is SIGKILLed under load and
       restarted from its directory (no promotion: admin mode). Node 3
       retires node 1's old links entry (a new incarnation) and purges its
       items; settles; money conserved.
  x7   failover of B: witness HA; node 3 (group 1's primary) is SIGKILLed;
       the witnesses promote node 4. Node 1 binds to node 4; its entry for
       node 3 quarantines (by its In side down). Settles. (Retirement of
       the old entry needs that member's next instance: restarting node 3
       as a standby creates none; retirement on restart is x6.)
  x7b  fenced peer alive: as x7, but node 3 is SIGSTOPped, not killed (cut
       off from everything), promoted past, then SIGCONTinued: an old
       primary alive next to the new one. Both entries coexist at node 1;
       no underflow; node 3 withdraws; settles.
  x9   mixed version: group 0 runs the v1 build (--old DIR, a build of the
       last D.1 commit), group 1 this one. Both sides refuse the links,
       with a message and a gauge on the new side; clients are served
       (intra-group transfers complete), cross-group ones stay pending;
       the new side's live count stays below X. Group 0 is then upgraded
       (its data directories have no links_generation: started with
       --links-generation-min 1, the floor no peer ever saw); all settle,
       nothing lost or doubled.

Every scenario prints one JSON line.

Usage: test_xnode_proc.py BUILD_DIR [SCENARIO ...] [--old OLD_BUILD_DIR]
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ARGS = list(sys.argv[1:])
OLD = None
if "--old" in ARGS:
    i = ARGS.index("--old")
    OLD = ARGS[i + 1]
    del ARGS[i:i + 2]
BUILD = ARGS[0] if ARGS else "build"
SCENARIOS = ARGS[1:]
sys.argv = [sys.argv[0], BUILD]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_fail_closed as fc  # noqa: E402
import test_baseline_move as tbm  # noqa: E402

START = 1000000


class XCluster(fc.Cluster):
    """Sync groups; `old`: nodes started with that build's binaries."""

    def __init__(self, work, ha=False, old=(), extra=None):
        self.old = set(old)
        self.extra = extra or {}
        super().__init__(work, groups=2, ha=ha, async_replica=False, v2=True)

    def start(self, k, standby_of=None, extra=(), bin_dir=None):
        more = list(self.extra.get(k, []))
        if self.how == "ha":
            more += []
        super().start(k, standby_of=standby_of, extra=list(extra) + more,
                      bin_dir=bin_dir or (OLD if k in self.old else None))


def gauges(c, k):
    r = c.ask(k, "GAUGES", 5)
    if not r or "=" not in r:
        return None
    return dict(t.split("=", 1) for t in r.split() if "=" in t)


def g_sum(g, name):
    n = int(g.get("shards", "0"))
    return sum(int(g.get(f"s{s}.{name}", "0")) for s in range(n))


def make_ids(c, accounts=64):
    ids = f"{c.work}/ids"
    rc, out = tbm.snc_load("create", "--connect", f"127.0.0.1:{c.client[1]}", "--accounts", accounts, "--ids", ids)
    assert rc == 0, out
    return ids


class Load:
    """snc_load run over every account (hot-pct 0: most transfers cross
    groups)."""

    def __init__(self, c, ids, seconds, threads=4, depth=4, port=None):
        self.prefix = f"{c.work}/load"
        self.p = subprocess.Popen([f"{BUILD}/snc_load", "run", "--connect", f"127.0.0.1:{port or c.client[1]}", "--ids",
                                   ids, "--seconds", str(seconds), "--threads", str(threads), "--depth", str(depth),
                                   "--hot-pct", "0", "--report-ms", "500", "--out", self.prefix],
                                  stdout=open(f"{c.work}/load.log", "a"), stderr=subprocess.STDOUT)

    def wait(self, timeout=300):
        try:
            rc = self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = self.p.wait()
        s = {}
        try:
            for line in open(self.prefix + ".summary"):
                k, _, v = line.strip().partition(" ")
                s[k] = v
        except OSError:
            s["error"] = "no summary"
        s["rc"] = rc
        return s

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


ZERO = ("xnode_remote_live", "xnode_held_for_peers", "xnode_end_owed")
NEVER = ("xnode_end_underflow", "xnode_held_underflow", "xnode_debt_increases", "xnode_uncounted")


def counts_zero(c, res, debt_zero=True):
    """Every serving primary: counts 0 (debt too unless a dead instance's
    quarantined debt is expected), never-counters 0, entries <= slots."""
    out = {}
    ok = True
    for k in tbm.primaries(c):
        g = gauges(c, k)
        if g is None:
            continue
        d = {n: g_sum(g, n) for n in ZERO + NEVER + ("xnode_debt", "xnode_entries", "xnode_entry_slots",
                                                      "xnode_retirements", "xnode_quarantines", "xnode_bindings",
                                                      "link_version_refusals", "intake_exchanges_live")}
        out[k] = d
        ok = ok and all(d[n] == 0 for n in ZERO + NEVER) and d["xnode_entries"] <= d["xnode_entry_slots"]
        ok = ok and (not debt_zero or d["xnode_debt"] == 0)
    res["xnode"] = out
    return ok


def wait_counts_zero(c, res, debt_zero=True, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if counts_zero(c, res, debt_zero):
            return True
        time.sleep(0.5)
    return False


def x6(work):
    c = XCluster(work)
    res = {}
    load = None
    try:
        ids = make_ids(c)
        g3 = gauges(c, 3)
        r0 = g_sum(g3, "xnode_retirements")
        load = Load(c, ids, 12)
        time.sleep(3)
        c.kill(1)
        time.sleep(0.5)
        c.start(1)
        fc.wait(lambda: c.role(1) in ("p", "n"), "node 1 back as primary", timeout=60)
        s = load.wait()
        res["load"] = {k: s.get(k) for k in ("rc", "acked", "unknown", "failovers", "resends", "error")}
        g3 = gauges(c, 3)
        res["node3_retirements"] = g_sum(g3, "xnode_retirements") - r0
        res["node3_retire_purged"] = g_sum(g3, "xnode_retire_purged")
        ok = tbm.settle_and_check(c, ids, res)
        ok = wait_counts_zero(c, res) and ok
        res["pass"] = ok and res["node3_retirements"] >= 1 and s["rc"] == 0
        return res
    finally:
        if load:
            load.stop()
        c.stop()


def x7(work, stop_not_kill=False):
    c = XCluster(work, ha=True)
    res = {}
    load = None
    try:
        ids = make_ids(c)
        load = Load(c, ids, 15)
        time.sleep(3)
        g1 = gauges(c, 1)
        q0 = g_sum(g1, "xnode_quarantines")
        if stop_not_kill:
            c.server[3].send_signal(signal.SIGSTOP)
        else:
            c.kill(3)
        fc.wait(lambda: c.role(4) in ("p", "n"), "the witnesses promoted node 4", timeout=60)
        if stop_not_kill:
            # Both entries at node 1 while node 3 is still frozen: node 4's
            # (bound) and node 3's (quarantined, its In side down).
            fc.wait(lambda: g_sum(gauges(c, 1), "xnode_quarantines") > q0, "node 3's entry quarantined", timeout=30)
            g1 = gauges(c, 1)
            res["entries_both"] = g_sum(g1, "xnode_entries")
            c.server[3].send_signal(signal.SIGCONT)
            time.sleep(3)  # alive next to the new primary: it learns the term and withdraws
            res["node3_role"] = c.role(3)
        else:
            fc.wait(lambda: g_sum(gauges(c, 1), "xnode_quarantines") > q0, "node 3's entry quarantined", timeout=30)
        s = load.wait()
        res["load"] = {k: s.get(k) for k in ("rc", "acked", "unknown", "failovers", "resends", "error")}
        if stop_not_kill:
            c.kill(3)  # the withdrawn old primary, shut down before the audit
        ok = tbm.settle_and_check(c, ids, res)
        # Node 3's entry at node 1 keeps its quarantined debt (bounded: U)
        # until member 0 of group 1 has a next instance.
        ok = wait_counts_zero(c, res, debt_zero=False) and ok
        res["pass"] = ok and s["rc"] == 0
        return res
    finally:
        if load:
            load.stop()
        c.stop()


def x9(work):
    assert OLD, "x9 needs --old OLD_BUILD_DIR (a build of the last D.1 commit)"
    c = XCluster(work, old=(1, 2))
    res = {}
    load = None
    try:
        ids = make_ids(c)
        load = Load(c, ids, 8)
        time.sleep(4)
        g3 = gauges(c, 3)
        res["new_side_version_refusals"] = g_sum(g3, "link_version_refusals")
        res["new_side_live"] = g_sum(g3, "intake_exchanges_live")
        log1 = open(f"{c.work}/n1.log").read()
        log3 = open(f"{c.work}/n3.log").read()
        res["old_side_message"] = next((l for l in log1.splitlines() if "node link" in l and ("version" in l or
                                        "magic" in l or "refus" in l or "bad" in l)), None)
        res["new_side_message"] = next((l for l in log3.splitlines() if "node link" in l and "wire v" in l), None)
        pend = {}
        for k in (1, 3):
            r = c.ask(k, "AUDIT", 30)
            pend[k] = int(r.split()[3]) if r and r.startswith("AUDIT") else None
        res["pending_mixed"] = pend
        s = load.wait()
        res["load_mixed"] = {k: s.get(k) for k in ("rc", "acked", "unknown", "error")}
        # Upgrade group 0: standby first, then its primary, on the same
        # directories, with the operator's floor for the missing file.
        for k in (2, 1):
            c.kill(k)
        c.old = set()
        c.extra = {1: ["--links-generation-min", "1"], 2: ["--links-generation-min", "1"]}
        c.start(2, standby_of=1)
        fc.wait(lambda: c.role(2) == "s", "standby 2 back")
        c.start(1)
        fc.wait(lambda: c.role(1) in ("p", "n"), "primary 1 upgraded", timeout=60)
        ok = tbm.settle_and_check(c, ids, res, timeout=180)
        ok = wait_counts_zero(c, res) and ok
        ok = ok and res["new_side_version_refusals"] > 0 and res["new_side_message"] is not None
        ok = ok and res["old_side_message"] is not None and int(s.get("acked", "0")) > 0
        ok = ok and any((v or 0) > 0 for v in pend.values())
        ok = ok and res["new_side_live"] < 2 * 1024 * 2  # X per shard, two shards
        res["pass"] = ok
        return res
    finally:
        if load:
            load.stop()
        c.stop()


SCEN = {"x6": x6, "x7": x7, "x7b": lambda w: x7(w, stop_not_kill=True), "x9": x9}


def main():
    names = SCENARIOS or list(SCEN)
    failed = []
    for n in names:
        work = tempfile.mkdtemp(prefix="snc-xproc-")
        t = time.time()
        try:
            res = SCEN[n](work)
        except Exception as e:  # noqa: BLE001
            res = {"pass": False, "error": repr(e)}
        res["scenario"] = n
        res["s"] = round(time.time() - t, 1)
        print(json.dumps(res), flush=True)
        if res.get("pass"):
            shutil.rmtree(work, ignore_errors=True)
        else:
            failed.append(n)
            print(f"  (kept {work})", flush=True)
    print("test_xnode_proc:", "FAILED " + " ".join(failed) if failed else "ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
