#!/usr/bin/env python3
"""Deterministic checks of refine1.py, before any rescue data. Starts no process.

  A1 every P1/H1/C1 server command is the S6 P/H/C command + --ckpt-window 1
     (ports, admin port, data dir and ids masked); run commands identical;
     order P1, H1, C1; window proof applies to each (open for P1/H1, closed for C1)
  A2 the reused S6 references reproduce the review's frozen numbers:
     R = 638,976 ns (limit 958,464), C0 median 4,016,161; N usable
  A3 verdict: all good -> PASS; p99 958,464 passes, 958,465 fails; p99.9
     10,000,000 passes, +1 fails; any other P1 rule fails -> FAIL; capacity
     95% x 4,016,161 boundary (3,815,353 passes, 3,815,352 fails); a C1
     rule-7 failure fails the guard; H1 is never judged; VOID and
     incomplete cells -> VOID
"""
import json, os, sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tests"))
import refine1  # noqa: E402
import test_s6 as T  # noqa: E402  (fixtures rep()/closed())

s6 = refine1.s6
fr = json.load(open(s6.FROZEN_PATH))
orig = {c: s6.commands(c, 0, 26040, 26070, "/dev/shm/x", fr) for c in ("P", "H", "C")}
refine1.setup()
assert s6.ORDER == ["P1", "H1", "C1"] and s6.DATA_ROOT == "/srv/snc-data/s6r1"


def mask(l, keys):
    l = list(l)
    for k in keys:
        l[l.index(k) + 1] = k.upper()
    return l


for cell, base in refine1.RESCUE.items():
    c = s6.commands(cell, 0, 26040, 26070, "/dev/shm/x", fr)
    a = mask(orig[base]["server"], ("--port", "--admin-port", "--data-dir"))
    b = mask(c["server"], ("--port", "--admin-port", "--data-dir"))
    assert b == a + ["--ckpt-window", "1"], (cell, a, b)
    assert mask(c["run"], ("--port", "--ids")) == mask(orig[base]["run"], ("--port", "--ids")), cell
    assert s6.CELLS[cell] == s6.CELLS[base]
W = 10**9
times = dict(run_start=100 * W, run_end=170 * W)
smp = [dict(t_ns=140 * W, ok=True, gauges=dict(ckpt_seq="1", ckpt="idle")),
       dict(t_ns=141 * W, ok=True, gauges=dict(ckpt_seq="2", ckpt="shards"))]
assert s6.checkpoint_window("P1", times, smp)[0] == [110 * W, 170 * W]
assert s6.checkpoint_window("C1", times, smp)[0] == [100 * W, 170 * W] and s6.checkpoint_window("C1", times, smp)[2]
print("A1 commands = S6 + --ckpt-window 1; order; window proof")

n, c0 = refine1.references()
assert s6.P99_FACTOR * refine1.REF_P99_NS == 958464
print("A2 references: R 638,976 (limit 958,464), C0 median 4,016,161")


def session(p_mut=None, c_total=(3_900_000,) * 3, c_mut=None, h_mut=None):
    # test_s6's default p99 (1,000,000) is above this round's frozen limit (958,464): use a passing one
    reps = [T.rep("P1", i, p99=900_000) for i in range(3)] + [T.rep("H1", i) for i in range(3)]
    reps += [T.closed("C1", i, v) for i, v in enumerate(c_total)]
    if p_mut:
        p_mut(reps[1])
    if c_mut:
        c_mut(reps[7])
    if h_mut:
        h_mut(reps[4])
    return reps


J = lambda reps, void=(): refine1.judge(reps, list(void), n, c0)  # noqa: E731
assert J(session())["verdict"] == "PASS"
assert J(session(lambda x: x["client"].update(p99=958464)))["verdict"] == "PASS"
assert J(session(lambda x: x["client"].update(p99=958465)))["verdict"] == "FAIL"
assert J(session(lambda x: x["client"].update(p999=10_000_000)))["verdict"] == "PASS"
assert J(session(lambda x: x["client"].update(p999=10_000_001)))["verdict"] == "FAIL"
for mut in (lambda x: x["client"].update(overloaded=1), lambda x: x["client"].update(index_refused=1),
            lambda x: x["client"].update(late_over_1ms=x["client"]["scheduled_in_window"] // 1000 + 1),
            lambda x: x.update(final_emits_peak=[1024] * 8), lambda x: x.update(final_degraded=[1] * 8),
            lambda x: x.update(checkpoints=[(1, Fraction(60))]), lambda x: x.update(client_rc=1),
            lambda x: x["client"].update(drained=False), lambda x: x.update(server_rc=1)):
    assert J(session(mut))["verdict"] == "FAIL"
assert J(session(c_total=(3_815_353,) * 3))["verdict"] == "PASS"
assert J(session(c_total=(3_815_352,) * 3))["verdict"] == "FAIL"
assert J(session(c_total=(1, 3_815_353, 10**9)))["verdict"] == "PASS"    # the median, not the mean
assert J(session(c_mut=lambda x: x.update(client_rc=1)))["verdict"] == "FAIL"
assert J(session(h_mut=lambda x: x["client"].update(overloaded=10**6, p999=10**9)))["verdict"] == "PASS"
assert J(session(), ["second checkpoint-outside-window rep (P1 r1)"])["verdict"] == "VOID"
assert J(session()[:-1])["verdict"] == "VOID"
r = session(); r[0]["void"] = "window"                                    # a VOID P1 rep with no rerun
assert J(r)["verdict"] == "VOID"
print("A3 verdict boundaries, H1 not judged, VOID")
print("ok")
