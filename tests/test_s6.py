#!/usr/bin/env python3
"""Deterministic checks of bench/s6.py's rules (docs/S6_PREREG.md 6-7;
docs/handoff/PR99_S6_Pacing_CLI_Final_Review.md "Runner tests required").
No process is started and nothing touches the machine.

  R1  p99 at exactly 3/2 x the N median passes; one ns more fails
  R2  lateness: late_over_1ms x 1000 = scheduled passes; +1 fails
  R3  p99.9 at 10,000,000 ns passes; +1 fails
  R4  backlog: emits_peak 1023 on every shard passes; 1024, an absent
      shard, or a missing shard fails
  R5  capacity: median C = 95% of median C0 passes; one less fails; a
      rule-7 failure in any C or C0 rep fails the guard
  R6  no refusal: one EXEC or one INDEX refusal fails
  R7  correctness/drained: each of errors, stale, index errors, not drained,
      server not alive, non-zero exit, a log marker fails; closed loop
      needs no drained line
  R8  checkpoint: 59.99 s passes, 60.00 s fails, none fails, degraded 1 or
      absent fails
  V1  N reference unusable -> VOID, not FAIL
  V2  checkpoint outside the window: one rep -> rerun once, then judged;
      a second such rep -> VOID; the window bounds are inclusive
  V3  machine/build drift at postflight -> VOID
  V4  one rerun at most: VOID again on the rerun -> VOID
  V5  a genuine FAIL is never rerun (exactly 15 executions)
  M1  medians of N p99 and of capacity
  T1  time above threshold from timestamps (irregular spacing, gaps, failed
      samples), not from sample counts
  O1  old-server samples: absent gauges reported as None, never 0
  C1  commands: one net_bench for every cell; C0 runs the pre-S1 server
      without pacing flags; N differs from P only by --checkpoint-s
  P1  parse_client on net_bench's real output (diag3 run, open loop)
  F1  a real run refuses while bench/s6_frozen.json is not frozen
  B1  checkpoint_begins from ckpt_seq
  K1  the measured net_bench exit status: 0 passes; 1, a signal, or none
      fails rule 7; a P rep with one is a FAIL, never rerun; a C or C0 rep
      with one fails the capacity guard (review 1, item 1)
  S1  sampler deadlines: no catch-up burst after an overrun; missed
      deadlines skipped; the timestamp gap still reported (item 2)
  W1  C and C0 must prove a checkpoint began inside the measured run:
      inside accepted, outside rerun once, a second is session VOID, no
      observable begin is not accepted (item 3)
  D1  storage identity: scheduler, write cache, FUA, filesystem UUID, ext4
      and every manifest-e mount option, on the resolved disk (item 4)
"""
import copy
import os
import sys
from fractions import Fraction

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "bench"))
import s6  # noqa: E402

OK_CLIENT = dict(total=436000, errors=0, overloaded=0, stale=0, index_refused=0, index_errors=0, p99=1_000_000,
                 p999=2_000_000, max=5_000_000, scheduled_in_window=26_198_760, late_over_1ms=0,
                 completed_in_window=26_198_000, end_backlog=0, drained=True)


def rep(cell="P", r=0, **client):
    c = dict(OK_CLIENT, **client)
    return dict(cell=cell, rep=r, void=None, client=c, client_rc=0, server_alive_at_end=True, server_rc=0, log_markers=[],
                final_emits_peak=[10] * 8, final_degraded=[0] * 8, checkpoints=[(625000, Fraction("2.50"))])


def closed(cell, r, total):
    x = rep(cell, r, total=total, drained=None, scheduled_in_window=None, late_over_1ms=None)
    if cell == "C0":
        x["final_degraded"] = [None] * 8   # the pre-S1 server has no such gauge
    return x


def session(p_mutate=None, n_p99=(900_000, 1_000_000, 1_100_000), c=(950, 950, 950), c0=(1000, 1000, 1000)):
    reps = [rep("N", i, p99=v) for i, v in enumerate(n_p99)]
    reps += [rep("P", i) for i in range(3)]
    reps += [rep("H", i) for i in range(3)]
    reps += [closed("C", i, v) for i, v in enumerate(c)]
    reps += [closed("C0", i, v) for i, v in enumerate(c0)]
    if p_mutate:
        p_mutate(reps[3])
    return reps


def rules(x, ref=1_000_000):
    return s6.p_rules(x, Fraction(ref))[0]


def main():
    # R1
    assert rules(rep(p99=1_500_000))["3_p99"]
    assert not rules(rep(p99=1_500_001))["3_p99"]
    assert s6.verdict(session(lambda x: x["client"].update(p99=1_500_000)), [])["verdict"] == "PASS"
    assert s6.verdict(session(lambda x: x["client"].update(p99=1_500_001)), [])["verdict"] == "FAIL"
    print("R1 p99 3/2 boundary")
    # R2
    assert rules(rep(scheduled_in_window=1_000_000, late_over_1ms=1000))["2_lateness"]
    assert not rules(rep(scheduled_in_window=1_000_000, late_over_1ms=1001))["2_lateness"]
    assert not rules(rep(late_over_1ms=None))["2_lateness"]
    print("R2 lateness 0.1% boundary")
    # R3
    assert rules(rep(p999=10_000_000))["4_p999"] and not rules(rep(p999=10_000_001))["4_p999"]
    print("R3 p99.9 10 ms boundary")
    # R4
    x = rep(); x["final_emits_peak"] = [1023] * 8; assert rules(x)["5_backlog"]
    x["final_emits_peak"] = [1023] * 7 + [1024]; assert not rules(x)["5_backlog"]
    x["final_emits_peak"] = [10] * 7 + [None]; assert not rules(x)["5_backlog"]
    x["final_emits_peak"] = [10] * 7; assert not rules(x)["5_backlog"]
    print("R4 backlog 1024 boundary")
    # R5
    assert s6.capacity_guard([closed("C", i, 950) for i in range(3)], [closed("C0", i, 1000) for i in range(3)])[0]
    assert not s6.capacity_guard([closed("C", i, 949) for i in range(3)], [closed("C0", i, 1000) for i in range(3)])[0]
    bad = [closed("C0", i, 1000) for i in range(3)]; bad[1]["client"]["errors"] = 1
    assert not s6.capacity_guard([closed("C", i, 2000) for i in range(3)], bad)[0]
    assert not s6.capacity_guard([closed("C", i, 2000) for i in range(2)], [closed("C0", i, 1) for i in range(3)])[0]
    assert s6.verdict(session(c=(949, 949, 949)), [])["verdict"] == "FAIL"
    print("R5 capacity 95% boundary and rule 7 in C/C0")
    # R6
    assert not rules(rep(overloaded=1))["1_no_refusal"] and not rules(rep(index_refused=1))["1_no_refusal"]
    assert rules(rep())["1_no_refusal"]
    print("R6 refusal rule")
    # R7
    for k, v in (("errors", 1), ("stale", 1), ("index_errors", 1), ("drained", False), ("errors", None)):
        assert not rules(rep(**{k: v}))["7_correctness"], k
    for k, v in (("server_alive_at_end", False), ("server_rc", -6), ("log_markers", ["terminate called"])):
        x = rep(); x[k] = v; assert not rules(x)["7_correctness"], k
    assert s6.correctness_ok(closed("C", 0, 1), open_loop=False)[0]
    assert not s6.correctness_ok(closed("C", 0, 1), open_loop=True)[0]   # open loop needs "drained yes"
    print("R7 correctness/drained rule")
    # R8
    x = rep(); x["checkpoints"] = [(1, Fraction("59.99"))]; assert rules(x)["6_checkpoint"]
    x["checkpoints"] = [(1, Fraction("60.00"))]; assert not rules(x)["6_checkpoint"]
    x["checkpoints"] = []; assert not rules(x)["6_checkpoint"]
    x = rep(); x["final_degraded"] = [0] * 7 + [1]; assert not rules(x)["6_checkpoint"]
    x["final_degraded"] = [None] * 8; assert not rules(x)["6_checkpoint"]
    assert [c for c in s6.checkpoints_completed("x\ncheckpoint: 625000 objects in 2.51s, 3 WAL segments deleted\n")] \
        == [(625000, Fraction("2.51"))]
    print("R8 checkpoint completion and degraded")

    # V1
    reps = session(); reps[1]["client"]["overloaded"] = 3
    v = s6.verdict(reps, [])
    assert v["verdict"] == "VOID" and any("N reference" in w for w in v["void"]), v
    reps = session(); reps[2]["server_rc"] = 134
    assert s6.verdict(reps, [])["verdict"] == "VOID"
    print("V1 unusable N reference -> VOID")

    # V2-V5 drive run_session with a fake executor.
    def fake(script):
        calls = []

        def execute(cell, r):
            n = sum(1 for c in calls if c == (cell, r))
            calls.append((cell, r))
            x = closed(cell, r, 1000) if cell in ("C", "C0") else rep(cell, r)
            x["void"] = script.get((cell, r, n))
            return x
        return execute, calls

    ex, calls = fake({("P", 1, 0): "window"})
    reps, void = s6.run_session(ex)
    assert void == [] and calls.count(("P", 1)) == 2 and calls[-1] == ("P", 1) and len(calls) == 16
    assert s6.verdict(reps, void)["verdict"] == "PASS"
    ex, calls = fake({("P", 0, 0): "window", ("H", 2, 0): "window"})
    reps, void = s6.run_session(ex)
    assert any("second checkpoint-outside-window" in w for w in void) and s6.verdict(reps, void)["verdict"] == "VOID"
    W = 10**9
    assert s6.began_in_window((5 * W, 6 * W), 5 * W, 65 * W) and s6.began_in_window((64 * W, 65 * W), 5 * W, 65 * W)
    assert not s6.began_in_window((4 * W, 6 * W), 5 * W, 65 * W) and not s6.began_in_window((64 * W, 66 * W), 5 * W, 65 * W)
    assert not s6.began_in_window(None, 0, W) and not s6.began_in_window((None, 1), 0, W)
    print("V2 checkpoint-outside-window rerun once, second -> VOID")
    ex, calls = fake({})
    reps, void = s6.run_session(ex, lambda: (False, ["machine governors"]))
    assert any("drift" in w for w in void) and s6.verdict(reps, void)["verdict"] == "VOID"
    print("V3 machine/build drift -> VOID")
    ex, calls = fake({("C0", 0, 0): "harness", ("C0", 0, 1): "harness"})
    reps, void = s6.run_session(ex)
    assert calls.count(("C0", 0)) == 2 and any("VOID again" in w for w in void)
    ex, calls = fake({("N", 2, 0): "window", ("N", 2, 1): "window"})
    reps, void = s6.run_session(ex)
    assert calls.count(("N", 2)) == 2 and void
    print("V4 one rerun at most")

    def failing(cell, r):
        x = closed(cell, r, 1000) if cell in ("C", "C0") else rep(cell, r)
        if cell == "P":
            x["client"]["overloaded"] = 5   # a genuine FAIL
        failing.n += 1
        return x
    failing.n = 0
    reps, void = s6.run_session(failing)
    assert failing.n == 15 and void == [] and s6.verdict(reps, void)["verdict"] == "FAIL"
    print("V5 genuine FAIL not rerun (15 executions)")

    # M1
    v = s6.verdict(session(n_p99=(3_000_000, 1_000_000, 2_000_000)), [])
    assert v["p99_reference_ns"] == "2000000", v
    assert s6.median([7, 1, 1000]) == 7 and s6.median([1, 2]) == Fraction(3, 2)
    assert s6.verdict(session(c=(10, 950, 2000), c0=(1, 1000, 10**9)), [])["capacity_ok"]
    print("M1 medians")

    # T1
    ms = 1_000_000
    ser = [(0, 5), (100 * ms, 5), (150 * ms, 1), (400 * ms, 9), (500 * ms, None), (600 * ms, 9), (700 * ms, 9)]
    above, covered, gap = s6.time_above(ser, lambda v: v > 4, max_gap_ns=200 * ms)
    # 0-100 above, 100-150 above, 150-400 is 250 ms > 200: gap, 400-500 above, 500-600 failed: gap, 600-700 above
    assert (above, covered, gap) == (350 * ms, 350 * ms, 350 * ms), (above, covered, gap)
    # the same samples twice as dense would double a count-based answer; time-based is unchanged
    dense = sorted(ser + [(t + 50 * ms, v) for t, v in ser[:2]])
    assert s6.time_above(dense, lambda v: v > 4, 200 * ms)[0] == 350 * ms
    print("T1 time above threshold from timestamps")

    # O1
    old = "shards=8 ckpt=idle ckpt_seq=1 " + " ".join(f"s{i}.emits_now={600 if i == 3 else 0} s{i}.emits_peak=700 "
                                                      f"s{i}.wal_flush_ewma_us=40" for i in range(8))
    samples = [dict(t_ns=i * 100 * ms, ok=True, gauges=s6.parse_kv(old)) for i in range(11)]
    cs = s6.controller_summary(samples)
    assert cs["catchup"] is None and cs["walk_budget_ns_range"] is None and cs["wal_above_high"] is None, cs
    assert cs["emits_peak"] == 700 and cs["wal_ewma_peak_us"] == 40
    assert cs["backlog_above_50pct"]["max_shard_ms"] == 1000.0 and cs["backlog_above_25pct"]["sum_ms"] == 1000.0
    assert s6.shard_values(s6.parse_kv(old), "ckpt_degraded_count") == [None] * 8
    samples[4] = dict(t_ns=400 * ms, ok=False, gauges=None)
    cs = s6.controller_summary(samples)
    # the interval that starts at the failed sample (400-500 ms) is a gap: 1000 - 100
    assert cs["failed_samples"] == 1 and cs["backlog_above_50pct"]["max_shard_ms"] == 900.0, cs
    assert cs["backlog_above_50pct"]["gap_ms_max"] == 100.0, cs
    print("O1 old-server gauges absent, never 0; failed samples are gaps")

    # C1
    fr = dict(server_build="build-s6", pres1_server="build-preS1/snc_server")
    cmd = {c: s6.commands(c, 0, 26040, 26070, "/dev/shm/snc-s6", fr) for c in s6.ORDER}
    nb = {c: cmd[c]["run"][cmd[c]["run"].index("--port") - 1] for c in cmd}
    assert len(set(nb.values())) == 1 and nb["C0"].endswith("build-s6/net_bench"), nb
    assert any(a.endswith("build-preS1/snc_server") for a in cmd["C0"]["server"])
    assert "--ckpt-walk-max-us" not in cmd["C0"]["server"] and "--checkpoint-s" in cmd["C0"]["server"]
    n_srv, p_srv = list(cmd["N"]["server"]), list(cmd["P"]["server"])
    i = p_srv.index("--checkpoint-s"); del p_srv[i:i + 2]
    for lst in (n_srv, p_srv):   # the per-cell data directory is the one other difference
        lst[lst.index("--data-dir") + 1] = "DATA"
    assert n_srv == p_srv and "--checkpoint-s" not in n_srv
    for flag, val in zip(s6.PACING_FLAGS[::2], s6.PACING_FLAGS[1::2]):
        assert cmd["P"]["server"][cmd["P"]["server"].index(flag) + 1] == val
    assert "--admin-port" in cmd["C0"]["server"] and "--rate" not in cmd["C"]["run"]
    assert cmd["P"]["run"][cmd["P"]["run"].index("--rate") + 1] == "436646"
    assert cmd["H"]["run"][cmd["H"]["run"].index("--rate") + 1] == "2183233"
    assert s6.ORDER == ["N", "P", "H", "C", "C0"] and s6.REPS == 3 and s6.SAMPLE_S == Fraction(1, 10)
    print("C1 commands")

    # P1 (net_bench's run section from diag3 B-w30-r0, abridged to its lines)
    out = """run: mix=read=70,deposit=15,transfer=10,index=5 seconds=30.00
  ops/s 2074059  errors 0  overloaded 0  stale 0
  index ops/s 109152  ok 3274639  empty 352  refused 0  errors 0
  total ops/s 2183211  (exec 62221772 + index 3274560 in 30.00s)  served_all 87329320
  total latency ns  n 65496990  p50 180224  p95 851968  p99 999424  p99.9 1245184  max 6395576
rate: offered 2183233 ops/s  warmup 10.00s  window 30.00s  drain limit 10.00s
  scheduled 87329320  scheduled_in_window 65496990  issued_by_end 87329303  completed_in_window 65496332  achieved 2183211 ops/s
  end_backlog 17 (0.0000% of scheduled_in_window)  in_flight_at_end 542
  issue lateness ns  p50 9472  p99 26624  p99.9 31744  max 170193  late_over_1ms 0
  drained yes  remaining_unissued 0  remaining_in_flight 0
"""
    c = s6.parse_client(out)
    assert c == dict(total=2183211, errors=0, overloaded=0, stale=0, index_refused=0, index_errors=0, p99=999424,
                     p999=1245184, max=6395576, scheduled_in_window=65496990, late_over_1ms=0,
                     completed_in_window=65496332, end_backlog=17, drained=True), c
    assert s6.parse_client("")["errors"] is None
    print("P1 parse net_bench")

    # F1
    import json
    fr = json.load(open(s6.FROZEN_PATH))
    probs = s6.frozen_problems(fr)
    if fr.get("runner_commit") is None:
        assert any("runner_commit" in p for p in probs) and any("go_record" in p for p in probs), probs
    f2 = copy.deepcopy(fr); f2["go_record"] = "docs/handoff/NO_SUCH_GO.md"
    assert any("does not exist" in p for p in s6.frozen_problems(f2))
    print("F1 refuses while not frozen / no GO")

    # B1
    g = lambda seq, ck="idle": dict(ckpt_seq=str(seq), ckpt=ck)   # noqa: E731
    smp = [dict(t_ns=t, ok=True, gauges=g(s)) for t, s in ((0, 1), (100, 1), (200, 2), (300, 2), (400, 3))]
    assert s6.checkpoint_begins(smp) == [(100, 200), (300, 400)]
    smp[1] = dict(t_ns=100, ok=False, gauges=None)
    assert s6.checkpoint_begins(smp)[0] == (0, 200)
    assert s6.checkpoint_begins([dict(t_ns=0, ok=True, gauges=g(1, "shards"))]) == [(None, 0)]
    assert s6.checkpoint_begins([dict(t_ns=0, ok=True, gauges=g(1))]) == []
    print("B1 checkpoint begins from ckpt_seq")

    # K1 (item 1)
    assert s6.correctness_ok(rep(), open_loop=True)[0]
    for rc in (1, -9, 134, None):
        x = rep(); x["client_rc"] = rc
        assert not rules(x)["7_correctness"], rc
        assert not s6.correctness_ok(closed("C", 0, 1000) | dict(client_rc=rc), open_loop=False)[0], rc
    x = rep(); del x["client_rc"]; assert not rules(x)["7_correctness"]
    assert s6.verdict(session(lambda x: x.update(client_rc=1)), [])["verdict"] == "FAIL"

    def rc_fail(cell, r):
        x = closed(cell, r, 1000) if cell in ("C", "C0") else rep(cell, r)
        if cell == "P" and r == 1:
            x["client_rc"] = 1
        rc_fail.n += 1
        return x
    rc_fail.n = 0
    reps, void = s6.run_session(rc_fail)
    assert rc_fail.n == 15 and void == [] and s6.verdict(reps, void)["verdict"] == "FAIL"
    for cell in ("C", "C0"):
        cs = [closed("C", i, 2000) for i in range(3)]; c0 = [closed("C0", i, 1000) for i in range(3)]
        (cs if cell == "C" else c0)[2]["client_rc"] = 1
        ok, why = s6.capacity_guard(cs, c0)
        assert not ok and any("rule 7" in w for w in why), (cell, why)
    print("K1 measured client exit status")

    # S1 (item 2)
    P = 100
    assert s6.next_deadline(0, 10, P) == (100, 0)            # normal progression
    assert s6.next_deadline(100, 150, P) == (200, 0)         # a 50 ms cycle: no skip
    assert s6.next_deadline(0, 99, P) == (100, 0)
    assert s6.next_deadline(0, 100, P) == (200, 1)           # deadline reached during the cycle: never 0 wait twice
    assert s6.next_deadline(0, 350, P) == (400, 3)           # a 250 ms overrun past 100: skip 100, 200, 300
    assert s6.next_deadline(0, 1100, P) == (1200, 11)        # a 1 s overrun: one future deadline, not ten owed

    def simulate(durations, period=P):
        """The sampler's loop on a fake clock: cycle i takes durations[i].
        Returns the start times and how many cycles began with no wait."""
        t, nxt, starts, immediate = 0, 0, [], 0
        for d in durations:
            if nxt <= t and starts:
                immediate += 1
            t = max(t, nxt)
            starts.append(t)
            t += d
            nxt, _ = s6.next_deadline(nxt, t, period)
        return starts, immediate
    starts, imm = simulate([5] * 10)
    assert starts == list(range(0, 1000, 100)) and imm == 0
    starts, imm = simulate([5, 1000, 5, 5, 5])
    assert starts == [0, 100, 1200, 1300, 1400] and imm == 0, starts   # no burst after the 1 s overrun
    starts, imm = simulate([5, 250, 5])
    assert starts == [0, 100, 400] and imm == 0, starts
    ms = 1_000_000
    smp = [dict(t_ns=t * ms, ok=True, gauges=s6.parse_kv("s0.emits_now=600")) for t in (0, 100, 1200, 1300)]
    cs = s6.controller_summary(smp, shards=1)
    assert cs["max_interval_ms"] == 1100.0 and cs["backlog_above_50pct"]["gap_ms_max"] == 1100.0, cs
    assert cs["backlog_above_50pct"]["max_shard_ms"] == 200.0, cs
    print("S1 sampler deadlines: no catch-up burst")

    # W1 (item 3)
    sec = 10**9
    def samples_with_begin(lo, hi):
        return [dict(t_ns=lo, ok=True, gauges=g(1)), dict(t_ns=hi, ok=True, gauges=g(2, "shards"))]
    T = dict(run_start=100 * sec, run_end=160 * sec)
    for cell in ("C", "C0"):
        win, b, inside = s6.checkpoint_window(cell, T, samples_with_begin(140 * sec, 140 * sec + 10**8))
        assert win == [100 * sec, 160 * sec] and inside, (cell, win, b)
        assert not s6.checkpoint_window(cell, T, samples_with_begin(90 * sec, 90 * sec + 10**8))[2]   # before the run
        assert not s6.checkpoint_window(cell, T, [dict(t_ns=120 * sec, ok=True, gauges=g(1))])[2]       # none seen
        assert not s6.checkpoint_window(cell, T, [])[2]                                                 # no samples
    assert s6.checkpoint_window("N", T, [])[2]    # no checkpoint cell: nothing to prove
    win, _, inside = s6.checkpoint_window("P", T, samples_with_begin(140 * sec, 141 * sec))
    assert win == [110 * sec, 170 * sec] and inside
    two = samples_with_begin(90 * sec, 91 * sec) + [dict(t_ns=140 * sec, ok=True, gauges=g(2)),
                                                    dict(t_ns=145 * sec, ok=True, gauges=g(3, "shards"))]
    assert s6.checkpoint_window("C", T, two)[2]   # a later checkpoint inside the run is enough
    gap = samples_with_begin(90 * sec, 91 * sec) + [dict(t_ns=145 * sec, ok=True, gauges=g(3, "shards"))]
    assert not s6.checkpoint_window("C", T, gap)[2]   # its lower bound (91 s) is before the run: not proven
    for cell in ("C", "C0"):
        ex, calls = fake({(cell, 0, 0): "window"})
        reps, void = s6.run_session(ex)
        assert void == [] and calls.count((cell, 0)) == 2 and s6.verdict(reps, void)["verdict"] == "PASS"
        assert len([x for x in reps if x["cell"] == cell and not x["void"]]) == 3
    ex, calls = fake({("C", 1, 0): "window", ("C0", 2, 0): "window"})
    reps, void = s6.run_session(ex)
    assert any("second checkpoint-outside-window" in w for w in void) and s6.verdict(reps, void)["verdict"] == "VOID"
    reps = session(); reps[9]["void"] = "window"   # a C rep without a proven checkpoint, and no rerun yet
    assert not s6.verdict(reps, [])["capacity_ok"]
    print("W1 C/C0 checkpoint inside the measured run")

    # D1 (item 4)
    good = {"/sys/block/sdx/device/model": "ARFX3840S5xnFTRI\n", "/sys/block/sdx/queue/scheduler": "none [mq-deadline] \n",
            "/sys/block/sdx/queue/write_cache": "write back\n", "/sys/block/sdx/queue/fua": "1\n"}
    mounts = "/dev/sdx1 /srv/snc-data ext4 rw,relatime,stripe=2 0 0\n"
    real = lambda p: {"/dev/sdx1": "/dev/sdx1"}.get(p, p)   # noqa: E731
    def chk(files=None, m=mounts, disk="sdx", uuid_dev="/dev/sdx1"):
        f = dict(good, **(files or {}))
        return s6.storage_checks(disk, m, lambda p: f.get(p, ""), real, uuid_dev)[0]
    assert all(chk().values()), chk()
    bad = [({"/sys/block/sdx/queue/scheduler": "[none] mq-deadline\n"}, mounts, "data_disk_scheduler"),
           ({"/sys/block/sdx/queue/scheduler": "none mq-deadline [kyber]\n"}, mounts, "data_disk_scheduler"),
           ({"/sys/block/sdx/queue/write_cache": "write through\n"}, mounts, "data_disk_write_cache"),
           ({"/sys/block/sdx/queue/fua": "0\n"}, mounts, "data_disk_fua"),
           ({"/sys/block/sdx/queue/fua": ""}, mounts, "data_disk_fua"),
           ({"/sys/block/sdx/device/model": "OTHER\n"}, mounts, "data_disk_model"),
           ({}, mounts.replace("stripe=2", "stripe=4"), "data_mount_options"),
           ({}, mounts.replace(",stripe=2", ""), "data_mount_options"),
           ({}, mounts.replace("relatime", "noatime"), "data_mount_options"),
           ({}, mounts.replace("rw,", "ro,"), "data_mount_options"),
           ({}, mounts.replace("ext4", "xfs"), "data_fs_ext4"),
           ({}, "", "data_mount_options")]
    for files, m, key in bad:
        c = chk(files, m)
        assert not c[key], (key, files, m)
    assert not chk(uuid_dev="/dev/sdy1")["data_fs_uuid"]
    assert all(chk(m=mounts.replace("stripe=2", "stripe=2,errors=remount-ro")).values())   # containment
    assert not any(v for k, v in chk(disk=None).items() if k.startswith("data_disk"))
    print("D1 storage identity")
    print("ok")


if __name__ == "__main__":
    main()
