#!/usr/bin/env python3
"""Deterministic checks of aduration.py, before any validation data. Starts no process.

  A1 the constants reproduce the CP-1A record: rate 654,569 = A objective = fixed rate;
     CV_p99 0.019286005545956553; D = 3 x CV_p99 = 0.05785801663786966; the CP-1A
     binaries; windows 60/120/240 x 3 reps; the order is the preregistered list
  A2 the 9 runs go through pilot.Pilot.one (subprocess stubbed): each server
     command is the CP-1A A-fixed-r0 server command, and each client command the
     CP-1A A-fixed-r0 command with only --seconds changed (ports, data dir and ids
     masked); --warmup 10, --drain-s 10, --rate 654569; fresh data dir per run;
     run order as preregistered; checkpoints counted from the server log
  A3 verdict: all good -> PASS; the bound is 60 s median x (1 + D) with <=
     (933,888 -> 987,920 passes, 987,921 fails; and at an exactly integral bound the
     bound itself passes, +1 fails; for 120 s and for 240 s); the
     median (not mean, not max) of 3; one refusal, one error, one failed rep, an
     unsustained 60 s rep -> FAIL; a missing rep -> INCOMPLETE
"""
import json, os, shutil, sys, tempfile, types
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tests"))
import aduration as A  # noqa: E402
import test_pilot as T  # noqa: E402  (fixture rep())

pilot = A.pilot
REPO = A.REPO
PLAN = json.load(open(os.path.join(REPO, "bench", "pilot_plan.json")))
CP1A_RAW = os.path.join(REPO, "bench", "results", "cp1a-r640-20261111", "raw")

# --- A1 constants ---
ref = A.cp1a_record()
assert A.RATE == 654569
assert A.CV_P99_FIXED_A == 0.019286005545956553
assert A.D == 3 * Fraction(0.019286005545956553) and float(A.D) == 0.05785801663786966, float(A.D)
assert ref["snc_server_sha256"].startswith("69a51b69") and ref["net_bench_sha256"].startswith("9fc54944")
assert ref["plan_sha256"].startswith("568e9874") and ref["manifest_sha256"].startswith("768056bb")
assert A.WINDOWS == (60, 120, 240) and A.REPS == 3
ORDER = A.order(PLAN)
assert sorted(ORDER) == [(w, i) for w in (60, 120, 240) for i in range(3)]
assert ORDER == [(120, 1), (120, 0), (120, 2), (60, 2), (240, 2), (240, 0), (240, 1), (60, 1), (60, 0)], ORDER  # preregistered (PREREG.md)
assert PLAN["warmup_s"] == 10 and PLAN["drain_s"] == 10
print("A1 ok: order", [A.label(w, i) for w, i in ORDER])

# --- A2 the run path ---
log0 = open(os.path.join(CP1A_RAW, "A-fixed-r0.client.log")).read()
srv0, cli0 = log0.split("\n")[0].split(), log0.split("\n")[1].split()
run_out = log0.split("--- run\n", 1)[1]


def mask(cmd, keys):
    c = list(cmd)
    for k in keys:
        if k in c:
            c[c.index(k) + 1] = k.upper()
    return c


def without_seconds(cmd):
    c = list(cmd)
    i = c.index("--seconds")
    return c[:i] + c[i + 2:], c[i + 1]


calls = dict(popen=[], run=[])


class FakeP:
    pid = 1
    returncode = None

    def poll(self):
        return None

    def send_signal(self, s):
        pass

    def wait(self, timeout=None):
        return 0


def popen(cmd, stdout=None, stderr=None):
    calls["popen"].append(cmd)
    data = cmd[cmd.index("--data-dir") + 1]
    assert not os.path.exists(data), "data dir not fresh"
    stdout.write("checkpoint: 5000000 objects in 3.4s, 264 WAL segments deleted\n"
                 "recovery: checkpoint before serving in 0.01s\n")
    return FakeP()


def run(cmd, capture_output=True, text=True, timeout=None):
    calls["run"].append(cmd)
    out = run_out if "--rate" in cmd else "load: ok\n"
    return types.SimpleNamespace(returncode=0, stdout=out, stderr="")


pilot.subprocess = types.SimpleNamespace(Popen=popen, run=run, STDOUT=-2,
                                         TimeoutExpired=__import__("subprocess").TimeoutExpired)
pilot.wait_port = lambda port, timeout=60: None
pilot.sample = lambda pid: dict(cpu_s=0, write_bytes=0, lo_rx=0, lo_tx=0)
pilot.memory_kb = lambda pid: dict(VmRSS=0, VmHWM=0, Pss=0)

tmp = tempfile.mkdtemp(prefix="adur-test-")
try:
    a = types.SimpleNamespace(out=os.path.join(tmp, "out"), data_root=os.path.join(tmp, "data"),
                              scratch=os.path.join(tmp, "scratch"), build="build-s6", quick=False,
                              allocation=PLAN["baseline_allocation"])
    os.makedirs(os.path.join(a.out, "raw"))
    p = object.__new__(pilot.Pilot)
    p.a, p.plan, p.alloc = a, PLAN, PLAN["allocations"][a.allocation]
    p.port, p.runs, p.results, p.env = 22000, [], {}, {}
    res = A.execute(p, PLAN)
    assert len(calls["popen"]) == 9 and len(calls["run"]) == 18, (len(calls["popen"]), len(calls["run"]))
    S0 = mask(srv0, ("--port", "--data-dir"))
    C0, sec0 = without_seconds(mask(cli0, ("--port", "--ids")))
    assert sec0 == "60"
    for k, (w, i) in enumerate(ORDER):
        srv = calls["popen"][k]
        srv = srv[srv.index("build-s6/snc_server"):]  # after the numactl binding
        assert mask(srv, ("--port", "--data-dir")) == S0, (srv, S0)
        assert srv[srv.index("--data-dir") + 1].endswith(f"A-aduration-{A.label(w, i)}")
        assert srv0[srv0.index("--checkpoint-s"):] == srv[srv.index("--checkpoint-s"):]
        meas = calls["run"][2 * k + 1]
        c, sec = without_seconds(mask(meas, ("--port", "--ids")))
        assert c == C0 and sec == str(w), (c, C0, sec, w)
        assert meas[meas.index("--rate") + 1] == "654569" and meas[meas.index("--warmup") + 1] == "10"
        assert meas[meas.index("--drain-s") + 1] == "10"
        assert calls["run"][2 * k][-4:] == ["--depth", "64", "--seconds", "0"]  # the load
    labels = [r["label"] for r in p.runs]
    assert labels == [A.label(w, i) for w, i in ORDER], labels
    assert all(r["checkpoints"] == 1 for r in p.runs)
    assert all(r["phase"] == "aduration" and r["profile"] == "A" for r in p.runs)
    assert json.load(open(os.path.join(a.out, "pilot.json")))["results"]["A_duration_validation"]["verdict"]
    print("A2 ok:", res["verdict"], "on the stub")
finally:
    shutil.rmtree(tmp, ignore_errors=True)


# --- A3 verdict ---
def runs(p99=None, **over):
    """9 sustained reps; p99[w] = (p99 r0, r1, r2); over[(w, i)] = dict of changes."""
    p99 = p99 or {}
    out = []
    for w in A.WINDOWS:
        for i in range(3):
            r = T.rep(A.RATE, p99=p99.get(w, (933888,) * 3)[i], window=w)
            r.update(profile="A", phase="aduration", label=A.label(w, i), p999_ns=1, max_ns=1)
            r.update(over.get(f"{w}_{i}", {}))
            out.append(r)
    return out


V = lambda rs: A.judge(rs, PLAN)
assert V(runs())["verdict"] == "PASS"
for w in (120, 240):
    ok = {60: (901120, 933888, 950272), w: (100, 987920, 2_000_000)}
    assert V(runs(ok))["verdict"] == "PASS", V(runs(ok))
    bad = {60: (901120, 933888, 950272), w: (100, 987921, 2_000_000)}
    assert V(runs(bad))["verdict"] == "FAIL"
# the median, not the mean and not the max, of 60 s
assert V(runs({60: (933888, 933888, 10**9), 120: (987921,) * 3}))["verdict"] == "FAIL"   # mean/max 60 s would pass
assert V(runs({60: (933888,) * 3, 120: (987920, 987920, 10**9)}))["verdict"] == "PASS"   # mean/max 120 s would fail
assert V(runs({60: (933888,) * 3, 240: (1, 987921, 987921)}))["verdict"] == "FAIL"     # min 240 s would pass
for k, change in (("120_1", dict(overloaded=33)), ("240_2", dict(index_refused=1)), ("60_0", dict(errors=1)),
                  ("60_2", dict(drained=False)), ("120_0", dict(failed=True, why="x")),
                  ("240_0", dict(completed_in_window=A.RATE * 240 * 99 // 100 - 1))):
    v = V(runs(**{k: change}))
    assert v["verdict"] == "FAIL", (k, v)
assert V(runs()[:-1])["verdict"] == "INCOMPLETE"
# the bound is inclusive: with the 60 s median = D's denominator the bound is an exact integer
q = A.D.denominator
edge = q + A.D.numerator
assert Fraction(q) * (1 + A.D) == edge
for w in (120, 240):
    assert V(runs({60: (q,) * 3, w: (edge,) * 3}))["verdict"] == "PASS"
    assert V(runs({60: (q,) * 3, w: (edge + 1,) * 3}))["verdict"] == "FAIL"
assert abs(V(runs())["bound_p99_ns"] - 933888 * (1 + 0.05785801663786966)) < 1e-6
print("A3 ok")
print("ok")
