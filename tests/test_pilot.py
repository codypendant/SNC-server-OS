#!/usr/bin/env python3
"""The exact rules of bench/pilot.py (docs/PILOT_PLAN.md): the rate grid,
the sustained test, the stable peak, the objective and the knee; per-op
metrics; parsing net_bench's report. Pure arithmetic: no processes."""
import json, os, shutil, sys, tempfile
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bench"))
import pilot

PLAN = json.load(open(os.path.join(HERE, "..", "bench", "pilot_plan.json")))


def rep(rate, p99=100, window=10, **kw):
    """A sustained open-loop rep at `rate`, unless kw changes it."""
    r = dict(offered=rate, window_s=window, scheduled=rate * (window + 1), scheduled_in_window=rate * window,
             completed_in_window=rate * window,
             achieved=rate, end_backlog=0, drained=True, remaining_unissued=0, remaining_in_flight=0,
             errors=0, index_errors=0, stale=0, overloaded=0, index_refused=0, p99_ns=p99)
    r.update(kw)
    return r


def test_cv():
    assert pilot.cv([1]) is None
    assert pilot.cv([0, 0]) is None
    assert abs(pilot.cv([90, 100, 110]) - 0.1) < 1e-12


def test_grid():
    assert pilot.grid(1000, Fraction(1, 20), 22) == [50 * k for k in range(1, 23)]
    # floor, exact: 5% of 999 is 49.95
    assert pilot.grid(999, Fraction(1, 20), 3) == [49, 99, 149]
    assert pilot.grid(1_234_567, Fraction(1, 20), 22)[-1] == 1_358_023  # 110%, floored


def test_sustained_boundaries():
    ok, why = pilot.sustained(rep(1000), PLAN)
    assert ok and why == []
    # completed: >= 99% of offered x window, exactly at the boundary
    assert pilot.sustained(rep(1000, completed_in_window=9900), PLAN)[0]
    assert not pilot.sustained(rep(1000, completed_in_window=9899), PLAN)[0]
    # end backlog: <= 1% of the ops scheduled in the measured window (10000
    # here), not of warm-up + window (11000), which would allow 110 (C1)
    assert pilot.sustained(rep(1000, end_backlog=100), PLAN)[0]
    assert not pilot.sustained(rep(1000, end_backlog=101), PLAN)[0]
    for k in ("errors", "index_errors", "stale", "overloaded", "index_refused"):
        assert not pilot.sustained(rep(1000, **{k: 1}), PLAN)[0], k
    ok, why = pilot.sustained(rep(1000, drained=False, remaining_unissued=5), PLAN)
    assert not ok and "not drained" in why[0]
    assert not pilot.sustained(dict(failed=True, why="x"), PLAN)[0]


def points(spec):
    """{rate: (p99, sustained?)} -> three reps per rate."""
    return {r: [rep(r, p99=l) if ok else rep(r, p99=l, overloaded=1)] * 3 for r, (l, ok) in spec.items()}


def test_peak_objective_knee():
    # Rates 100..1000; sustained through 800; 900 fails; 1000 passes again.
    spec = {r: (100, True) for r in range(100, 1001, 100)}
    spec[400] = (150, True)
    spec[500] = (250, True)  # > 2 x 100: the knee
    spec[900] = (900, False)
    res = pilot.sweep_result(points(spec), PLAN)
    assert res["stable_peak"] == 800  # 1000 passing does not count: 900 below it failed
    # f = 7/10: 0.7 x 800 = 560, the largest grid rate <= 560 is 500
    assert res["objective_rate"] == 500 and res["objective_p99_ns"] == 250
    assert res["knee_rate"] == 500
    assert [p["sustained"] for p in res["points"]] == [True] * 8 + [False, True]


def test_objective_exact_fraction():
    # S = 1000, f x S = 700 exactly: 700 is a grid rate and is chosen.
    res = pilot.sweep_result(points({r: (100, True) for r in range(100, 1001, 100)}), PLAN)
    assert res["stable_peak"] == 1000 and res["objective_rate"] == 700
    assert res["knee_rate"] is None  # p99 never doubled


def test_one_bad_rep_fails_the_point():
    p = points({100: (10, True), 200: (10, True), 300: (10, True)})
    p[200][1] = rep(200, p99=10, completed_in_window=1)
    res = pilot.sweep_result(p, PLAN)
    assert res["stable_peak"] == 100
    p[200] = p[200][:2]  # a missing rep also fails it
    p[200][1] = rep(200, p99=10)
    assert pilot.sweep_result(p, PLAN)["stable_peak"] == 100


def suspect(rate):
    """Short of its rate, nothing refused, ops issued late: client-limited."""
    return [rep(rate, completed_in_window=1, issue_late_p99_ns=5_000_000)] * 3


def server_limited(rate):
    return [rep(rate, overloaded=1, issue_late_p99_ns=5_000_000)] * 3


def test_client_limit_at_boundary_invalidates():
    # C3: the lowest failing point is client-limited: the sweep is invalid,
    # not an SNC stable peak at 200.
    p = points({100: (10, True), 200: (10, True)})
    p[300] = suspect(300)
    p[400] = server_limited(400)
    res = pilot.sweep_result(p, PLAN)
    rows = {x["rate"]: x for x in res["points"]}
    assert rows[300]["client_limited_suspect"] and not rows[400]["client_limited_suspect"]
    assert res["invalid"] and "300" in res["invalid"]
    assert res["stable_peak"] is None and res["objective_rate"] is None and res["knee_rate"] is None
    # At R_1 too.
    res = pilot.sweep_result({100: suspect(100), 200: [rep(200)] * 3}, PLAN)
    assert res["invalid"] and res["stable_peak"] is None


def test_client_limit_above_boundary_is_diagnostic():
    # The boundary (300) is server-limited; a suspect point above it keeps
    # its flag and changes nothing.
    p = points({r: (10, True) for r in range(100, 1001, 100)})
    p[800] = server_limited(800)
    p[900] = suspect(900)
    res = pilot.sweep_result(p, PLAN)
    rows = {x["rate"]: x for x in res["points"]}
    assert res["invalid"] is None and res["stable_peak"] == 700 and rows[900]["client_limited_suspect"]
    assert res["objective_rate"] == 400  # largest grid rate <= 0.7 x 700 = 490
    # Errors are never client-limited, late issue or not.
    p[800] = [rep(800, errors=1, completed_in_window=1, issue_late_p99_ns=5_000_000)] * 3
    assert pilot.sweep_result(p, PLAN)["invalid"] is None
    # Lateness at 1 ms exactly does not trigger; above it does.
    p[800] = [rep(800, completed_in_window=1, issue_late_p99_ns=1_000_000)] * 3
    assert pilot.sweep_result(p, PLAN)["invalid"] is None
    p[800] = [rep(800, completed_in_window=1, issue_late_p99_ns=1_000_001)] * 3
    assert pilot.sweep_result(p, PLAN)["invalid"]


def test_no_stable_peak():
    res = pilot.sweep_result(points({100: (10, False), 200: (10, True)}), PLAN)
    assert res["stable_peak"] is None and res["objective_rate"] is None


def test_median_is_a_measured_value():
    p = points({100: (10, True)})
    p[100] = [rep(100, p99=x) for x in (30, 10, 20)]
    assert pilot.sweep_result(p, PLAN)["points"][0]["median_p99_ns"] == 20


def test_same_data_same_answer():
    spec = {r: (r // 10, True) for r in range(100, 1001, 100)}
    a = pilot.sweep_result(points(spec), PLAN)
    b = pilot.sweep_result(dict(reversed(list(points(spec).items()))), PLAN)
    assert a == b


def test_per_op_keeps_loopback_apart():
    a = dict(cpu_s=1, write_bytes=100, lo_rx=0, lo_tx=0)
    b = dict(cpu_s=3, write_bytes=300, lo_rx=400, lo_tx=600)
    r = pilot.per_op(a, b, 100)
    assert r == dict(cpu_us_per_op=20000, write_bytes_per_op=2, loopback_rx_bytes_per_op=4,
                     loopback_tx_bytes_per_op=6)
    assert pilot.per_op(a, b, 0)["cpu_us_per_op"] is None


REPORT = """net_bench shards=2 threads=2 depth=1024/thread route=owner misroute=0%
load: 20000 objects in 0.00s (1 creates/s)
run: reads=50% seconds=3.00
  ops/s 18992  errors 0  overloaded 0  stale 0
  latency ns  p50 16896  p95 60416  p99 1507328  p99.9 4980736
  sent to non-owner 0.0%  forwarded by server 0.0%
  index ops/s 1008  ok 3024  empty 0  refused 0  errors 0
  index latency ns  p50 36864  p95 151552  p99 2883584  p99.9 4849664
  mix of total ops (--mix): read 70.00%  deposit 15.00%  transfer 10.00%  index 5.00%
  total ops/s 20000  (exec 56976 + index 3024 in 3.00s)  served_all 80000
  total latency ns  n 60000  p50 22016  p95 67584  p99 1605632  p99.9 4980736  max 9783481
  read latency ns  n 41917  p50 16896  p95 58368  p99 1507328  p99.9 5111808  max 9783481
  deposit latency ns  n 9062  p50 17408  p95 64512  p99 1703936  p99.9 4587520  max 8285059
  transfer latency ns  n 5997  p50 16384  p95 67584  p99 1409024  p99.9 4587520  max 8584476
  index latency ns  n 3024  p50 36864  p95 151552  p99 2883584  p99.9 4849664  max 8686791
rate: offered 20000 ops/s  warmup 1.00s  window 3.00s  drain limit 10.00s
  scheduled 80000  scheduled_in_window 60000  issued_by_end 79990  completed_in_window 59999  achieved 20000 ops/s
  end_backlog 10 (0.0167% of scheduled_in_window)  in_flight_at_end 3
  issue lateness ns  p50 96  p99 167936  p99.9 3473408  max 5461402  late_over_1ms 382
  drained yes  remaining_unissued 0  remaining_in_flight 0
"""


def test_parse_report():
    r = pilot.parse_client(REPORT)
    assert r["total_ops_s"] == 20000 and r["exec_ops"] == 56976 and r["index_ok"] == 3024
    assert r["served_all"] == 80000
    assert r["p99_ns"] == 1605632 and r["n"] == 60000  # total, over every kind
    assert r["index_p99_ns"] == 2883584 and r["transfer_n"] == 5997
    assert r["offered"] == 20000 and r["scheduled"] == 80000 and r["completed_in_window"] == 59999
    assert r["scheduled_in_window"] == 60000
    assert r["end_backlog"] == 10 and r["in_flight_at_end"] == 3 and r["drained"]
    assert r["issue_late_max_ns"] == 5461402 and r["late_over_1ms"] == 382
    r["window_s"] = 3
    assert pilot.sustained(r, PLAN)[0]


def test_parse_closed_loop():
    out = REPORT[:REPORT.index("rate: offered")]
    r = pilot.parse_client(out)
    assert "offered" not in r and r["total_ops_s"] == 20000


def test_plan_is_complete():
    for k in ("objects", "mix", "allocations", "baseline_allocation", "calibration_depth", "rate_depth", "warmup_s",
              "drain_s", "grid_step", "grid_points", "reps", "min_completed_fraction", "max_end_backlog_fraction",
              "objective_fraction", "knee_p99_multiple", "order_seed"):
        assert k in PLAN, k
    assert sum(int(x.split("=")[1]) for x in PLAN["mix"].split(",")) == 100
    lo = PLAN["allocations"]["PILOT-L"]
    assert not set(lo["server_cpus"]) & set(lo["client_cpus"])  # disjoint
    assert all(c % 2 == 0 for c in lo["server_cpus"] + lo["client_cpus"])  # node 0
    assert all(c < 36 for c in lo["server_cpus"] + lo["client_cpus"])  # no SMT sibling
    assert not set(lo["housekeeping_cpus"]) & set(lo["server_cpus"] + lo["client_cpus"])
    assert len(lo["server_cpus"]) >= max(PLAN["shard_counts"] + [PLAN["shards"]])
    assert len(lo["client_cpus"]) >= PLAN["client_threads"]
    x = PLAN["allocations"]["PILOT-X"]
    assert all(c % 2 == 1 for c in x["client_cpus"]) and x["client_node"] == 1


# Benchmark scratch isolation (PR62 review, step 1): a fake sysfs.
MOUNTINFO = """\
25 24 0:27 / /dev/shm rw,relatime - tmpfs tmpfs rw
27 1 8:33 / / rw,relatime - ext4 /dev/sdc1 rw
30 27 8:1 / /srv/snc-data rw,relatime - ext4 /dev/sda1 rw
31 27 253:0 / /srv/lvm rw,relatime - ext4 /dev/mapper/vg-lv rw
32 27 0:45 / /srv/btrfs rw,relatime shared:9 - btrfs /dev/sdb2 rw
33 27 7:0 / /srv/loop rw,relatime - ext4 /dev/loop0 rw
"""


def fake_sysfs():
    root = tempfile.mkdtemp(prefix="snc-sysfs-")
    dev = os.path.join(root, "devices")

    def disk(path, part=False, slaves=()):
        os.makedirs(os.path.join(dev, path), exist_ok=True)
        if part:
            open(os.path.join(dev, path, "partition"), "w").close()
        if slaves:
            os.makedirs(os.path.join(dev, path, "slaves"))
            for name, target in slaves:
                os.symlink(os.path.join(dev, target), os.path.join(dev, path, "slaves", name))

    disk("pci/sda"); disk("pci/sda/sda1", part=True)
    disk("pci/sdb"); disk("pci/sdb/sdb2", part=True)
    disk("pci/sdc"); disk("pci/sdc/sdc1", part=True)
    disk("virtual/dm-0", slaves=[("sda2", "pci/sda/sda2"), ("sdb2", "pci/sdb/sdb2")])
    disk("pci/sda/sda2", part=True)
    disk("virtual/loop0")
    os.makedirs(os.path.join(root, "dev", "block"))
    for mm, target in (("8:1", "pci/sda/sda1"), ("8:33", "pci/sdc/sdc1"), ("253:0", "virtual/dm-0"),
                       ("7:0", "virtual/loop0")):
        os.symlink(os.path.join(dev, target), os.path.join(root, "dev", "block", mm))
    return root


def test_physical_disks():
    sysfs = fake_sysfs()
    try:
        pd = lambda dev, swaps=(): pilot.physical_disks(dev, MOUNTINFO, sysfs, swaps)
        assert pd((8, 1)) == {"sda"}  # a partition maps to its disk
        assert pd((8, 33)) == {"sdc"}
        assert pd((253, 0)) == {"sda", "sdb"}  # device mapper maps to every member's disk
        assert pd((0, 27)) == frozenset()  # tmpfs with no swap: no disk
        assert pd((0, 27), swaps={"sda"}) == {"sda"}  # tmpfs can page out to swap
        assert pd((0, 45)) is None  # btrfs: anonymous device, unknown disk: refuse
        assert pd((7, 0)) is None  # loop: backing file unknown: refuse
        assert pd((9, 9)) is None  # not in sysfs: refuse
    finally:
        shutil.rmtree(sysfs)


def test_isolation_problems():
    ok = pilot.isolation_problems(frozenset({"sda"}), {"scratch": frozenset(), "out": frozenset({"sdc"})})
    assert ok == [], ok
    # The pilot's layout before the fix: ids file next to the data dir.
    bad = pilot.isolation_problems(frozenset({"sda"}), {"scratch": frozenset({"sda"}), "out": frozenset({"sdc"})})
    assert bad == ["the scratch directory shares disk sda with the data directory"], bad
    bad = pilot.isolation_problems(frozenset({"sda"}), {"scratch": frozenset({"sda", "sdb"}), "out": None})
    assert len(bad) == 2 and "cannot tell" in bad[0] and "shares disk sda" in bad[1], bad
    assert pilot.isolation_problems(None, {"scratch": frozenset()}) == \
        ["cannot tell which disk holds the data directory"]


def test_disks_of_this_host():
    # The real resolver on whatever host runs the tests: /dev/shm is memory.
    d = pilot.disks_of("/dev/shm")
    assert d is None or all(isinstance(x, str) for x in d)


if __name__ == "__main__":
    for k, v in list(globals().items()):
        if k.startswith("test_"):
            v()
    print("ok")
