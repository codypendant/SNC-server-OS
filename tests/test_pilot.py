#!/usr/bin/env python3
"""Arithmetic of bench/pilot.py: CV, the knee rule, per-op metrics, parsing."""
import os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench"))
import pilot


def reps(t, l, errors=0):
    return [dict(total_ops_s=x, p99_ns=l, errors=errors) for x in t]


def test_cv():
    assert pilot.cv([1]) is None
    assert pilot.cv([0, 0]) is None
    assert abs(pilot.cv([90, 100, 110]) - 0.1) < 1e-12


def test_knee_rule():
    r = pilot.knee({1: reps([100] * 3, 10), 4: reps([700] * 3, 20), 16: reps([950] * 3, 30), 64: reps([1000] * 3, 90)})
    assert r["peak"] == 1000
    assert r["knee_depth"] == 16      # first at or above 900
    assert r["objective_depth"] == 4  # first at or above 700
    assert r["objective_p99_ns"] == 20


def test_unstable_depths_are_ignored():
    r = pilot.knee({4: reps([700] * 3, 20), 16: reps([5000] * 3, 30, errors=1), 64: reps([100, 2000, 4000], 40),
                    128: reps([1000] * 3, 50)})
    assert r["peak"] == 1000 and r["knee_depth"] == 128 and r["objective_depth"] == 4


def test_same_data_same_answer():
    d = {i: reps([i * 10] * 3, i) for i in (1, 4, 16)}
    assert pilot.knee(d) == pilot.knee(dict(reversed(list(d.items()))))


def test_nothing_stable():
    assert pilot.knee({1: reps([1, 2], 1, errors=1)})["peak"] is None


def test_per_op():
    a, b = dict(cpu_s=1, write_bytes=100, lo_bytes=0), dict(cpu_s=3, write_bytes=300, lo_bytes=400)
    r = pilot.per_op(a, b, 100)
    assert r == dict(cpu_us_per_op=20000, write_bytes_per_op=2, loopback_bytes_per_op=4)
    assert pilot.per_op(a, b, 0)["cpu_us_per_op"] is None


def test_parse_keeps_index_apart():
    out = """run: reads=70% seconds=10.00
  ops/s 1000  errors 2  overloaded 3  stale 0
  latency ns  p50 1  p95 2  p99 3  p99.9 4
  sent to non-owner 0.0%  forwarded by server 0.0%
  index ops/s 50  ok 500  empty 1  refused 0  errors 5
  index latency ns  p50 10  p95 20  p99 30  p99.9 40
"""
    r = pilot.parse_client(out)
    assert r["total_ops_s"] == 1050 and r["errors"] == 7
    assert r["p99_ns"] == 3 and r["index_p99_ns"] == 30 and r["run_s"] == 10


if __name__ == "__main__":
    for k, v in list(globals().items()):
        if k.startswith("test_"):
            v()
    print("ok")
