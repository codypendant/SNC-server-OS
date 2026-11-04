#!/usr/bin/env python3
"""S1 bounded destination staging (docs/MIGRATION_STAGING.md 3, the PR #41
work order's S1-1..S1-18), with real snc_server nodes: the harness of
tests/test_fail_closed.py (three witnesses holding the placement, two
groups of a primary and an async standby, snc_ctl moves). Every node runs
the chain with one object per unit (--move-unit-us 1 --move-unit-bytes 1),
so a final stage of a few dozen objects is hundreds of units and DEBUG
MOVE-STOP can stop or crash it at any phase and unit.

Each scenario: a slot S of A's (group of node `a`) gets 60 accounts; the
copy ahead reaches B; then 20 of them change at A (deposits) and 5 more
are created there, so the final stage puts 25 objects. The final stage is
then held, crashed or raced as the scenario says.

  normal       S1-1, S1-13, S1-14, S1-12: the chain held mid-put while a
               transfer from another slot credits S (a follow-up: held at
               the sealed source), a deposit to S is refused, B's client
               port refuses S, and B never serves S before activation;
               then STAGED exactly the source's count/digest; then the
               move completes and every balance is exact (credit once).
  off          the same with --move-units off (one task), for equivalence.
  crash:PHASE:UNIT  S1-4/5/6, the crash matrix: B's primary exits at that
               point (admit 0: before the first unit; check 0: the first
               unit; put 1: early; put 12: between units; scan 2: after
               every put; stage 0: object work done, STAGED not written;
               reply 0: STAGED durable, not answered); restarted (put 12
               restarts with --move-units off: rollback); never serves S
               before activation; the move converges, exact.
  dst_promote  S1-7: B's primary dies mid-chain; its standby is promoted;
               the volatile chain is gone; the move converges there.
  src_promote  S1-8: A's primary dies mid-chain after the seal; its standby
               (re-sealed at its barrier, PR #40) never serves S; the move
               commits through the gate; exact.
  driver_death S1-9: the driver is killed mid-chain; the chain finishes;
               a new driver completes the move; one STAGED.
  duplicate    S1-10: a second driver's final stage while the chain runs is
               refused E_OVERLOADED and resent; one chain, one STAGED.
  fence        S1-11: B's standby is promoted while B's chain is held; the
               chain stops at its next unit (E_FENCED or E_STANDBY), no
               STAGED at B; the move completes at the new primary.
  health       S1-15/16: a final of thousands of puts with the default
               budget (1 ms); HEALTH and STATUS at B sampled throughout.

S1-17 and S1-18 are the existing suites (test_fail_closed, test_migrate);
S1-2/S1-3 are the 100K measurements (bench/s1_move.py,
docs/MIGRATION_STAGING.md 18.4).

Usage: test_s1.py BUILD_DIR [SCENARIO ...]
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_fail_closed as fc  # noqa: E402
from test_fail_closed import (OK, E_MIGRATING, STATUS, Probe, balance, create_here, deposit,  # noqa: E402
                              owner_primary, request, transfer, wait)

FINE = ["--move-unit-us", "1", "--move-unit-bytes", "1"]
CRASHES = ["admit:0", "check:0", "put:1", "put:12", "scan:2", "stage:0", "reply:0"]


class S1Cluster(fc.Cluster):
    """Every node runs the chain with `extra_all` (fine units by default)."""

    extra_all = FINE

    def start(self, k, standby_of=None, extra=(), bin_dir=None):
        super().start(k, standby_of=standby_of, extra=list(self.extra_all) + list(extra), bin_dir=bin_dir)


def log_of(c, k):
    return open(f"{c.work}/n{k}.log").read()


def chains(c, k, slot):
    """The node's stage_chain lines for `slot`, as dicts of their fields."""
    out = []
    for line in log_of(c, k).splitlines():
        if f"stage_chain slot={slot} " in line:
            d = {}
            for kv in line.split():
                if "=" in kv:
                    a, b = kv.split("=", 1)
                    d.setdefault(a, b)
            out.append(d)
    return out


def held(c, k, timeout=20):
    wait(lambda: "debug: stage chain held" in log_of(c, k), f"node {k}'s chain held", timeout)


def ctl_bg(c, *args):
    """snc_ctl in the background: (Popen, output file)."""
    out = f"{c.work}/ctl-{time.time_ns()}"
    v2 = ["--placement-v2"] if c.v2 else []
    p = subprocess.Popen([f"{c.bin}/snc_ctl", "--control", c.control] + v2 + [str(a) for a in args],
                         stdout=open(out + ".out", "w"), stderr=open(out + ".err", "w"))
    c.procs.append(p)
    return p, out


def ctl_result(p, out, timeout=120):
    p.wait(timeout=timeout)
    return open(out + ".out").read().strip(), open(out + ".err").read()


class Setup:
    """S with n accounts at A. move() starts the driver (copy ahead one
    object per request) with a writer at A that, while the copy ahead runs,
    deposits into the first `changed` accounts round robin and creates
    `new` more in S, until the seal refuses it: everything it did after the
    copy's export is the final stage's delta (puts)."""

    def __init__(self, c, ev, n=60, changed=20, new=5):
        ids, S = c.create(1, 1, in_slot=n - 1)
        self.c, self.ev, self.S, self.ids = c, ev, S, ids
        self.a = owner_primary(c, S)
        self.b = 3 if self.a == 1 else 1
        self.qa, self.qb = c.peer(self.a), c.peer(self.b)
        self.gb = c.group_of(self.b)
        self.changed, self.new = changed, new
        self.expect = {i: 1000000 for i in ids}
        ev.update(slot=S, source=self.a, dest=self.b)

    def writer(self, deadline):
        port = self.c.client[self.a]
        made = 0
        k = 0
        while time.time() < deadline:
            if made < self.new:
                args = fc.struct.pack("<qQBB6s64s96s", 1000000, 1, 1, 1, b"", b"e", b"n")
                st, _, oid, _ = request(port, 2, fc.ACCOUNT, fc.oid_bytes(self.ids[0]), args)
                if st == OK:
                    hi, lo = fc.struct.unpack("<QQ", oid)
                    self.expect[f"{hi:016x}{lo:016x}"] = 1000000
                    made += 1
            if self.changed:
                i = self.ids[k % self.changed]
                k += 1
                st = deposit(port, i, 100)
                if st == OK:
                    self.expect[i] += 100
                elif st == E_MIGRATING:
                    break
            elif made >= self.new:
                break
        self.ev["writer"] = f"{k} deposits tried, {made} created"

    def move(self, stop_after="staged", chunk=1):
        th = threading.Thread(target=self.writer, args=(time.time() + 60,), daemon=True)
        th.start()
        time.sleep(0.05)
        p = ctl_bg(self.c, "move", "--chunk", chunk, *(["--stop-after", stop_after] if stop_after else []),
                   self.S, self.gb)
        self.writer_thread = th
        return p

    def written(self):
        self.writer_thread.join()
        self.ids = list(self.expect)

    def check_staged(self, c, k):
        """k holds STAGED for the move, exactly the sealed source's image."""
        self.written()
        d = c.info(k, self.S)
        assert d["phase"] == 3 and d["durable"] == 1 and d["serving"] == 0, d
        assert d["count"] == len(self.ids) and d["live_count"] == d["count"], d
        assert d["live_digest"] == d["digest"], d
        return d

    def finish(self, c, k, ev):
        self.written()
        out, err = c.ctl("move", self.S, self.gb)
        assert out == "DONE", out + err
        wait(lambda: c.info(k, self.S)["serving"] == 1, "destination serves S")
        bad = {}
        for i, cents in self.expect.items():
            st, bal = balance(c.client[k], i)
            if st != OK or bal != cents:
                bad[i] = (STATUS.get(st, st), bal, cents)
        assert not bad, bad
        ev["balances"] = f"{len(self.expect)} exact at node {k}"


def scenario_normal(c, ev):
    s = Setup(c, ev)
    probe = Probe(c, [(s.b, s.S), (s.qb, s.S)])
    assert c.ask(s.b, "DEBUG MOVE-STOP put 3 hold:3000") == "OK"
    p, out = s.move()
    held(c, s.b)
    # A transfer source in another slot A serves (S is sealed now: not minted into).
    y = create_here(c.client[s.a])
    # S1-14: follow-ups and ops during the chain.
    st = transfer(c.client[s.a], y, s.ids[0], 7)
    ev["transfer_into_S_during_chain"] = STATUS.get(st, st)
    assert st == OK, st  # the debit is y's; the credit to S waits
    s.expect[s.ids[0]] += 7
    st = deposit(c.client[s.a], s.ids[1], 1)
    ev["deposit_to_S_at_source"] = STATUS.get(st, st)
    assert st == E_MIGRATING, st
    st, _ = balance(c.client[s.b], s.ids[0])
    ev["read_S_at_destination"] = STATUS.get(st, st)
    assert st != OK, st
    d = c.info(s.b, s.S)
    ev["dest_mid_chain"] = {k: d[k] for k in ("phase", "serving", "live_count")}
    assert d["serving"] == 0 and d["phase"] != 3, d  # S1-12
    r, err = ctl_result(p, out)
    assert r == "STOPPED staged", r + err
    s.check_staged(c, s.b)
    ch = chains(c, s.b, s.S)
    ev["chain"] = {k: ch[-1][k] for k in ("status", "units", "put", "max", "window_hw")}
    assert ch[-1]["status"] == "OK" and int(ch[-1]["put"]) >= 4, ch[-1]
    served = probe.stop()
    ev["probe"] = f"{probe.samples} samples, served {len(served)}"
    assert probe.samples > 0 and not served, served
    s.finish(c, s.b, ev)
    # The credit landed once (finish checked the balance, which includes it).


def scenario_off(c, ev):
    s = Setup(c, ev)
    p, o = s.move()
    out, err = ctl_result(p, o)
    assert out == "STOPPED staged", out + err
    s.check_staged(c, s.b)
    ch = chains(c, s.b, s.S)
    assert ch[-1]["units"] == "1", ch[-1]
    ev["chain"] = {k: ch[-1][k] for k in ("status", "units", "put")}
    s.finish(c, s.b, ev)


def crash_at(c, ev, phase, unit):
    s = Setup(c, ev)
    probe = Probe(c, [(s.b, s.S), (s.qb, s.S)])
    assert c.ask(s.b, f"DEBUG MOVE-STOP {phase} {unit} crash") == "OK"
    p, out = s.move()
    rc = c.server[s.b].wait(timeout=60)
    assert rc == 9 and "debug: stage chain crash" in log_of(c, s.b), rc
    before = len(chains(c, s.b, s.S))
    rollback = phase == "put" and unit == 12
    c.start(s.b, extra=["--move-units", "off"] if rollback else [])
    wait(lambda: c.role(s.b) in ("p", "n"), "B restarted", timeout=30)
    d = c.info(s.b, s.S)
    ev["after_restart"] = {k: d[k] for k in ("phase", "serving", "live_count", "durable")}
    assert d["serving"] == 0, d
    if phase == "reply":
        assert d["phase"] == 3 and d["durable"] == 1, d  # STAGED was durable before the crash
    else:
        assert d["phase"] != 3, d
    r, err = ctl_result(p, out)
    assert r == "STOPPED staged", r + err
    s.check_staged(c, s.b)
    ch = chains(c, s.b, s.S)[before:]
    ev["retry_chains"] = [{k: x[k] for k in ("status", "units", "put")} for x in ch]
    if phase == "reply":
        assert not ch, ch  # the driver saw STAGED: no second final stage
    served = probe.stop()
    ev["probe"] = f"{probe.samples} samples, served {len(served)}"
    assert probe.samples > 0 and not served, served
    ev["rollback_restart_units_off"] = rollback
    s.finish(c, s.b, ev)


def scenario_dst_promote(c, ev):
    s = Setup(c, ev)
    probe = Probe(c, [(s.qb, s.S)])
    assert c.ask(s.b, "DEBUG MOVE-STOP put 5 hold:4000") == "OK"
    p, out = s.move()
    held(c, s.b)
    c.kill(s.b)
    c.failover(s.b, s.qb)
    d = c.info(s.qb, s.S)
    ev["promoted_before_retry"] = {k: d[k] for k in ("phase", "serving", "live_count")}
    assert d["serving"] == 0 and d["phase"] != 3, d
    r, err = ctl_result(p, out)
    assert r == "STOPPED staged", r + err
    s.check_staged(c, s.qb)
    served = probe.stop()
    ev["probe"] = f"{probe.samples} samples, served {len(served)}"
    assert probe.samples > 0 and not served, served
    s.finish(c, s.qb, ev)


def scenario_src_promote(c, ev):
    s = Setup(c, ev)
    probe = Probe(c, [(s.qa, s.S)])
    assert c.ask(s.b, "DEBUG MOVE-STOP put 5 hold:4000") == "OK"
    p, out = s.move()
    held(c, s.b)
    c.kill(s.a)
    c.failover(s.a, s.qa)
    d = c.info(s.qa, s.S)
    ev["source_promoted"] = {k: d[k] for k in ("phase", "serving", "move")}
    assert d["serving"] == 0, d
    r, err = ctl_result(p, out)
    assert r == "STOPPED staged", r + err
    s.check_staged(c, s.b)
    out2, err2 = c.ctl("move", s.S, s.gb)
    assert out2 == "DONE", out2 + err2
    served = probe.stop()
    ev["probe"] = f"{probe.samples} samples, served {len(served)}"
    assert probe.samples > 0 and not served, served
    s.finish(c, s.b, ev)


def scenario_driver_death(c, ev):
    s = Setup(c, ev)
    assert c.ask(s.b, "DEBUG MOVE-STOP put 5 hold:2000") == "OK"
    p, out = s.move()
    held(c, s.b)
    p.kill()
    p.wait()
    wait(lambda: c.info(s.b, s.S)["phase"] == 3, "the chain finished without its driver", timeout=30)
    s.check_staged(c, s.b)
    wait(lambda: chains(c, s.b, s.S), "the chain's report")
    ev["chains"] = [{k: x[k] for k in ("status", "units", "put")} for x in chains(c, s.b, s.S)]
    s.finish(c, s.b, ev)
    staged = log_of(c, s.b).count(f"slot {s.S} staged for move")
    assert staged == 1, staged


def scenario_duplicate(c, ev):
    s = Setup(c, ev)
    assert c.ask(s.b, "DEBUG MOVE-STOP put 5 hold:3000") == "OK"
    p1, o1 = s.move()
    held(c, s.b)
    p2, o2 = ctl_bg(c, "move", "--stop-after", "staged", s.S, s.gb)
    r1, e1 = ctl_result(p1, o1)
    r2, e2 = ctl_result(p2, o2)
    ev["drivers"] = [r1, r2]
    assert r1 in ("STOPPED staged", "DONE") and r2 in ("STOPPED staged", "DONE"), (r1 + e1, r2 + e2)
    ev["second_driver_saw"] = [ln.split("] ", 1)[1] for ln in e2.splitlines() if "final copy" in ln]
    assert "E_OVERLOADED" in e1 + e2, e1 + e2
    oks = [x for x in chains(c, s.b, s.S) if x["status"] == "OK"]
    assert len(oks) == 1, oks
    assert log_of(c, s.b).count(f"slot {s.S} staged for move") == 1
    s.finish(c, s.b, ev)


def scenario_fence(c, ev):
    s = Setup(c, ev)
    assert c.ask(s.b, "DEBUG MOVE-STOP put 5 hold:4000") == "OK"
    p, out = s.move()
    held(c, s.b)
    c.promote(s.qb)  # B is not told: it learns at its sender, or not at all
    r, err = ctl_result(p, out)
    assert r == "STOPPED staged", r + err
    ch = chains(c, s.b, s.S)
    ev["old_primary_chain"] = [{k: x[k] for k in ("status", "units", "put")} for x in ch]
    assert ch and ch[0]["status"] in ("E_FENCED", "E_STANDBY"), ch
    d = c.info(s.b, s.S) if c.server[s.b].poll() is None else None
    ev["old_primary_after"] = d and {k: d[k] for k in ("phase", "serving")}
    assert d is None or (d["phase"] != 3 and d["serving"] == 0), d
    s.check_staged(c, s.qb)
    c.kill(s.b)
    s.finish(c, s.qb, ev)


def scenario_health(c, ev):
    """S1-15/16 with the default budget: 10,000 accounts, those the writer
    changes during the copy ahead (thousands) put by the final."""
    # A sanitizer build runs the copy ahead (one request per `chunk`
    # objects, each answered with a walk of the map) 3-10x slower, past the
    # driver's 60 s deadline at 10,000: the same shape at a fifth the size.
    san = "fsanitize" in open(os.path.join(fc.BUILD, "CMakeCache.txt"), errors="replace").read()
    n = 2000 if san else 10000
    ev["sanitizer_build"] = san
    s = Setup(c, ev, n=n, changed=n, new=0)
    lat = {"HEALTH": [], "STATUS": []}
    stop = [False]

    def sample():
        while not stop[0]:
            for verb in lat:
                t = time.perf_counter()
                r = c.ask(s.b, verb, 5)
                if r:
                    lat[verb].append((time.perf_counter() - t) * 1000)
            time.sleep(0.005)

    th = threading.Thread(target=sample, daemon=True)
    th.start()
    p, o = s.move(chunk=4)
    out, err = ctl_result(p, o)
    stop[0] = True
    th.join()
    assert out == "STOPPED staged", out + err
    s.check_staged(c, s.b)
    ch = chains(c, s.b, s.S)[-1]
    ev["chain"] = {k: ch[k] for k in ("status", "units", "put", "p50", "p99", "max", "chain_ms")}

    def pct(v, q):
        v = sorted(v)
        return round(v[min(len(v) - 1, int(q * len(v)))], 2) if v else None

    for verb, v in lat.items():
        ev[verb] = {"n": len(v), "p50": pct(v, .5), "p95": pct(v, .95), "p99": pct(v, .99), "max": pct(v, 1)}
    assert int(ch["units"]) > 10 and int(ch["put"]) > n // 10, ch
    assert ev["HEALTH"]["max"] < 200, ev["HEALTH"]
    s.finish(c, s.b, ev)


def run(name):
    work = tempfile.mkdtemp(prefix="snc-s1-")
    cls = S1Cluster
    if name == "off":
        cls = type("Off", (S1Cluster,), {"extra_all": ["--move-units", "off"]})
    elif name == "health":
        cls = type("Default", (S1Cluster,), {"extra_all": []})
    c = cls(work)
    ev = {}
    t0 = time.time()
    try:
        if name.startswith("crash:"):
            _, phase, unit = name.split(":")
            crash_at(c, ev, phase, int(unit))
        else:
            globals()["scenario_" + name](c, ev)
        print(f"PASS {name} ({time.time() - t0:.1f} s): {ev}", flush=True)
        ok = True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL {name}: {type(e).__name__}: {e}\n  evidence: {ev}\n  work dir: {work}", flush=True)
        ok = False
    finally:
        c.stop()
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    return ok


def main():
    names = sys.argv[2:] or (["normal", "off"] + [f"crash:{x}" for x in CRASHES] +
                             ["dst_promote", "src_promote", "driver_death", "duplicate", "fence", "health"])
    failed = [n for n in names if not run(n)]
    print("test_s1:", "OK" if not failed else f"FAILED {failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
