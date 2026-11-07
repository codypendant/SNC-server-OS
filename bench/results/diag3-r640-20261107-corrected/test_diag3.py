#!/usr/bin/env python3
"""Deterministic checks of diag3.py (no load, no processes): B and T use
identical scratch isolation and differ only by telemetry; isolation fails
closed; the revised G0 and G2 at their exact boundaries; the other gates
unchanged; the client-report parser.

  python3 bench/results/diag3-r640-20261107-corrected/test_diag3.py
"""
import os, sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diag3  # noqa: E402

SCR = "/dev/shm/snc-diag3"
RAW = "/out/raw"
TELEMETRY = {"--diag-gauges": 1, "--diag-cpu": 1, "--diag-buckets": 1}


def strip(cmd, arm):
    out, skip = [], 0
    for x in cmd:
        if skip:
            skip -= 1
            continue
        if x in TELEMETRY:
            skip = TELEMETRY[x]
            continue
        out.append(x.replace(diag3.BUILDS[arm], "BUILD").replace(f"{arm}-", "ARM-"))
    return out


def test_b_and_t_differ_only_by_telemetry():
    cells = diag3.PROOF + diag3.CAUSAL
    for kind, rate, win, ckpt in cells:
        b = diag3.commands("B", kind, 1, rate, win, ckpt, 26040, SCR, RAW)
        t = diag3.commands("T", kind, 1, rate, win, ckpt, 26040, SCR, RAW)
        for k in ("server", "load", "run"):
            assert strip(b[k], "B") == strip(t[k], "T"), (kind, k, b[k], t[k])
        # Both arms: ids under the one scratch, data under the data dir, never the reverse.
        for c in (b, t):
            assert os.path.dirname(c["ids"]) == SCR and c["ids"].endswith(".ids"), c["ids"]
            assert c["data"].startswith(diag3.DATA + "/") and not c["ids"].startswith(diag3.DATA), c
            assert c["run"][c["run"].index("--ids") + 1] == c["ids"] == c["load"][c["load"].index("--ids") + 1]
        assert "--diag-gauges" not in b["server"] and "--diag-buckets" not in b["run"]
        assert "--diag-gauges" in t["server"] and "--diag-buckets" in t["run"]


def test_cells_are_the_approved_ones():
    assert diag3.PROOF == [("closed", None, 60, 60), ("w30", 2183233, 30, 60)]
    assert diag3.CAUSAL == [("r436646-ckpt", 436646, 60, 60), ("r436646-nockpt", 436646, 60, 0),
                            ("r2183233-w60", 2183233, 60, 60)]
    c = diag3.commands("T", "r436646-nockpt", 0, 436646, 60, 0, 1, SCR, RAW)
    assert "--checkpoint-s" not in c["server"]  # the server's default: never
    c = diag3.commands("B", "closed", 0, None, 60, 60, 1, SCR, RAW)
    assert "--rate" not in c["run"] and c["run"][c["run"].index("--depth") + 1] == "512"
    c = diag3.commands("B", "w30", 0, 2183233, 30, 60, 1, SCR, RAW)
    r = c["run"]
    assert r[r.index("--rate") + 1] == "2183233" and r[r.index("--depth") + 1] == "1024"
    assert r[r.index("--warmup") + 1] == "10" and r[r.index("--seconds") + 1] == "30" and r[r.index("--drain-s") + 1] == "10"
    assert c["server"][:3] == ["numactl", "--physcpubind=2,4,6,8,10,12,14,16", "--membind=0"]
    assert r[:3] == ["numactl", "--physcpubind=18,20,22,24,26,28,30,32", "--membind=0"]


def test_isolation_fails_closed():
    def fake(table):
        return lambda p: table[p]
    out = "/repo/out"
    ok = {diag3.DATA: frozenset({"sda"}), SCR: frozenset(), out: frozenset({"sdc"})}
    assert diag3.isolation(SCR, out, fake(ok))[0] == []
    # The contaminated layout: ids next to the data.
    bad = dict(ok, **{SCR: frozenset({"sda"})})
    assert diag3.isolation(SCR, out, fake(bad))[0] == ["the scratch directory shares disk sda with the data directory"]
    bad = dict(ok, **{out: frozenset({"sda", "sdc"})})
    assert diag3.isolation(SCR, out, fake(bad))[0] == ["the out directory shares disk sda with the data directory"]
    for role in (SCR, out, diag3.DATA):
        p = diag3.isolation(SCR, out, fake(dict(ok, **{role: None})))[0]
        assert len(p) == 1 and p[0].startswith("cannot tell"), (role, p)
    # tmpfs whose swap is on the data disk.
    p = diag3.isolation(SCR, out, fake(dict(ok, **{SCR: frozenset({"sda"})})))[0]
    assert p and "scratch" in p[0]
    # The observer needs one data disk.
    p = diag3.isolation(SCR, out, fake(dict(ok, **{diag3.DATA: frozenset({"sda", "sdb"})})))[0]
    assert p == ["the data directory is on ['sda', 'sdb'], not exactly one disk"], p


def rep(arm, kind, total=4366466, p99=1048576, p999=2000000, over=0, idx=0, sched=None):
    r = dict(arm=arm, kind=kind, total=total, p99=p99, p999=p999, over=over, idx=idx)
    if sched:
        r["sched"] = sched
    return r


S = 65_496_990  # scheduled_in_window at 2,183,233 x 30 s


def proof(b_closed=(4366466,) * 3, t_closed=(4366466,) * 3, b_ref=(0, 0, 0), t_ref=(0, 0, 0), b_p99=(1048576,) * 3):
    res = []
    for i in range(3):
        res += [rep("B", "closed", total=b_closed[i]), rep("B", "w30", total=2183200, p99=b_p99[i], over=b_ref[i], sched=S),
                rep("T", "closed", total=t_closed[i]), rep("T", "w30", total=2183200, over=t_ref[i], sched=S)]
    return res


def test_all_pass_and_incomplete_fails():
    G, _ = diag3.gates(proof(), True)
    assert all(G.values()), G
    G, d = diag3.gates(proof()[:-1], True)  # one T w30 rep missing
    assert not any(G.values()) and d == {"incomplete": True}


def test_g0_revised():
    # No zero-refusal clause any more: B refusals do not fail G0.
    G, _ = diag3.gates(proof(b_ref=(0, 256, 1016), t_ref=(0, 256, 1016)), True)
    assert G["G0"] and G["G2"], G
    # A: within 5% of 4,366,466, exact. 1.05 x = 4,584,789.3; 0.95 x = 4,148,142.7.
    for med, ok in ((4584789, True), (4584790, False), (4148143, True), (4148142, False)):
        G, _ = diag3.gates(proof(b_closed=(1, med, 9_999_999), t_closed=(med,) * 3), True)
        assert G["G0"] is ok, (med, G)
    # B: median p99 <= 1.5 x 1,048,576 = 1,572,864.
    for med, ok in ((1572864, True), (1572865, False)):
        G, _ = diag3.gates(proof(b_p99=(0, med, 10**9)), True)
        assert G["G0"] is ok, (med, G)
    # C: identity.
    G, d = diag3.gates(proof(), False)
    assert not G["G0"] and d["G0_identity"] is False and G["G1"] and G["G2"] and G["G3"] and G["G4"]


def test_g2_revised_boundaries():
    # B fractions 0, 256/S, 1016/S: median 256/S, max 1016/S.
    # Median rule: median(T) <= 256/S + 1/10000. 1/10000 of S is 6549.699 refusals.
    lim = 256 + Fraction(S, 10000)  # 6805.699
    base = (0, 256, 1016)
    G, _ = diag3.gates(proof(b_ref=base, t_ref=(0, 6805, 6805)), True)
    assert G["G2"], G
    G, d = diag3.gates(proof(b_ref=base, t_ref=(0, 6806, 6806)), True)
    assert not G["G2"] and not d["G2_median"] and d["G2_per_rep"], d
    assert 6805 < lim < 6806
    # Per-rep rule: every T <= 1016/S + 2/10000; 2/10000 of S is 13099.398.
    G, d = diag3.gates(proof(b_ref=base, t_ref=(0, 0, 1016 + 13099)), True)
    assert G["G2"], d
    G, d = diag3.gates(proof(b_ref=base, t_ref=(0, 0, 1016 + 13100)), True)
    assert not G["G2"] and d["G2_median"] and not d["G2_per_rep"], d
    # INDEX refusals count too.
    r = proof(b_ref=base, t_ref=(0, 0, 0))
    r[-1]["idx"] = 1016 + 13100
    assert not diag3.gates(r, True)[0]["G2"]


def test_refusal_fraction_is_exact():
    assert diag3.refusal_fraction(dict(over=3, idx=4, sched=70)) == Fraction(1, 10)


def test_unchanged_gates():
    # G1: T closed >= 0.97 x B closed.
    G, _ = diag3.gates(proof(t_closed=(4235472,) * 3), True)  # 0.97 x 4,366,466 = 4,235,472.02
    assert not G["G1"]
    G, _ = diag3.gates(proof(t_closed=(4235473,) * 3), True)
    assert G["G1"]
    # G4: T w30 total >= 0.995 x B w30.
    r = proof()
    for x in r:
        if x["arm"] == "T" and x["kind"] == "w30":
            x["total"] = 2172283  # 0.995 x 2,183,200 = 2,172,284
    assert not diag3.gates(r, True)[0]["G4"]
    # G3: T w30 p99 <= max(1.25 x B median, B max).
    r = proof()
    for x in r:
        if x["arm"] == "T" and x["kind"] == "w30":
            x["p99"] = 1310721  # 1.25 x 1,048,576 = 1,310,720
    assert not diag3.gates(r, True)[0]["G3"]


CLIENT = """--- run
net_bench shards=8 threads=8 depth=1024/thread route=owner misroute=0%
run: mix=read=70,deposit=15,transfer=10,index=5 seconds=30.00
  ops/s 2074055  errors 0  overloaded 17  stale 0
  index ops/s 109153  ok 3274639  empty 325  refused 5  errors 0
  total ops/s 2183207  (exec 62221642 + index 3274579 in 30.00s)  served_all 87329320
  total latency ns  n 65496990  p50 180224  p95 851968  p99 999424  p99.9 1245184  max 5931411
rate: offered 2183233 ops/s  warmup 10.00s  window 30.00s  drain limit 10.00s
  scheduled 87329320  scheduled_in_window 65496990  issued_by_end 87329296  completed_in_window 65496221  achieved 2183207 ops/s
  issue lateness ns  p50 9472  p99 27136  p99.9 33792  max 131415  late_over_1ms 0
"""


def test_parse():
    r = diag3.parse(CLIENT)
    assert r == dict(total=2183207, over=17, idx=5, p99=999424, p999=1245184, maxl=5931411, sched=65496990,
                     late_over_1ms=0, achieved=2183207), r
    closed = CLIENT[:CLIENT.index("rate: offered")]
    assert "sched" not in diag3.parse(closed)


def test_manifest_values():
    v = diag3.manifest_values(open(diag3.MANIFEST).read())
    assert v == dict(kernel="6.8.0-149-generic", cpus=72, model="Intel(R) Xeon(R) Gold 6150 CPU @ 2.70GHz",
                     node0=",".join(map(str, range(0, 72, 2))), mem_kb=329771792), v
    assert diag3.cpulist("0-3,8,10-11") == [0, 1, 2, 3, 8, 10, 11]


if __name__ == "__main__":
    for k, f in list(globals().items()):
        if k.startswith("test_"):
            f()
            print("ok", k)
    print("test_diag3 ok")
