#!/usr/bin/env python3
"""Deterministic checks of a2a.py, before any A2-a data. Starts no process, trims nothing.

  A1 constants: K 5; anchor 654,569 = the CP-1A A objective; settle 1,800 s; the fixed
     12-slot order S0 F1 C1 F2 C2 F3 C3 F4 C4 F5 C5 S1; stop CV 10%; the 4 root
     commands are exactly those of sudoers-snc-a2a; the SMART parser reads the real capture
  A2 session procedure (root calls, sleep and processes stubbed), per session in order:
     A1 pre-trim reads, fstrim -v /srv/snc-data, one 1,800 s settle, A1 start reads, the 12
     runs, A1 end reads; fixed and sentinel commands = CP-1A A-fixed-r0, capacity = CP-1A
     A-capacity-r0 (ports, data dir, ids masked), fixed and sentinel reps with the memory
     measurement, capacity reps without; 60 runs in the preregistered order;
     a failed trim aborts before any run of that session -> INCOMPLETE; extra_preflight
     refuses an enabled/active fstrim.timer, a missing sudo rule, a data root off sdb
  A3 verdict: realistic data -> STABLE; N_raw = 3 x max(between and max-within CVs); a
     within-session CV > 10% in one session stops though the median CV is low (max, not
     median; also pinned when every session is under 10%); between-session p99 or thr CV > 10% stops; symmetric sentinels with
     inclusive 1 + N_raw (exact edge passes, +1 fails, both directions); an unsustained
     fixed or sentinel rep, a capacity error -> STOP; a missing run -> INCOMPLETE; two
     separated clusters of session medians -> STOP (multimodal) at CV < 10%
"""
import json, os, re, shutil, sys, tempfile, time, types
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tests"))
import a2a as A  # noqa: E402
import test_pilot as T  # noqa: E402

pilot = A.pilot
PLAN = json.load(open(os.path.join(A.REPO, "bench", "pilot_plan.json")))
CP1A_RAW = os.path.join(A.REPO, "bench", "results", "cp1a-r640-20261111", "raw")
SMART = open(os.path.join(A.REPO, "bench", "results", "r640-storage-state-20261112", "capture",
                          "smartctl-x-sdb.txt")).read()

# --- A1 ---
assert A.K == 5 and A.ANCHOR == 654569 and A.SETTLE_S == 1800 and A.STOP_CV == 0.10
assert A.ORDER == ["S0", "F1", "C1", "F2", "C2", "F3", "C3", "F4", "C4", "F5", "C5", "S1"]
assert A.cp1a_record()["snc_server_sha256"].startswith("69a51b69")
sud = open(os.path.join(HERE, "sudoers-snc-a2a")).read()
line = [l for l in sud.splitlines() if l.startswith("solo ")][0]
assert [c.strip().split() for c in line.split("NOPASSWD:")[1].split(",")] == list(A.ROOT.values()), line
sm = A.parse_smart(SMART)
assert sm["bms_progress_pct"] == 79.61 and sm["bms_status"] == "scan is active" and sm["temperature_c"] == 36
assert sm["health"] == "OK" and sm["uncorrected_write"] == 0 and sm["write_gb_processed"] == 17105.852
print("A1 ok")


# --- A2 ---
def section(log):
    lines = open(os.path.join(CP1A_RAW, log)).read().split("\n")
    return lines[0].split(), lines[1].split(), open(os.path.join(CP1A_RAW, log)).read().split("--- run\n", 1)[1]


fsrv, fcli, fout = section("A-fixed-r0.client.log")
csrv, ccli, cout = section("A-capacity-r0.client.log")
events = []


def mask(cmd, keys=("--port", "--data-dir", "--ids")):
    c = list(cmd)
    for k in keys:
        if k in c:
            c[c.index(k) + 1] = k.upper()
    return c


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
    events.append(("server", cmd))
    return FakeP()


def prun(cmd, capture_output=True, text=True, timeout=None):
    if cmd[-4:] != ["--depth", "64", "--seconds", "0"]:
        events.append(("client", cmd))
    out = fout if "--rate" in cmd else (cout if "--depth" in cmd and cmd[-4:] != ["--depth", "64", "--seconds", "0"]
                                        else "load ok\n")
    return types.SimpleNamespace(returncode=0, stdout=out, stderr="")


pilot.subprocess = types.SimpleNamespace(Popen=popen, run=prun, STDOUT=-2,
                                         TimeoutExpired=__import__("subprocess").TimeoutExpired)
pilot.wait_port = lambda port, timeout=60: None
pilot.sample = lambda pid: dict(cpu_s=0, write_bytes=0, lo_rx=0, lo_tx=0)
pilot.memory_kb = lambda pid: dict(VmRSS=0, VmHWM=0, Pss=0)
pilot.time = types.SimpleNamespace(sleep=lambda s: None, time=time.time, strftime=time.strftime, gmtime=time.gmtime)
FAIL_TRIM = {"k": None}


def fake_sh(cmd, root=False):
    if root:
        assert cmd in A.ROOT.values(), cmd
        name = [k for k, v in A.ROOT.items() if v == cmd][0]
        events.append(("root", name))
        if name == "fstrim":
            k = sum(1 for e in events if e == ("root", "fstrim"))
            if FAIL_TRIM["k"] == k:
                return 1, "fstrim: /srv/snc-data: FITRIM ioctl failed"
            return 0, "/srv/snc-data: 3.4 TiB (3779298537472 bytes) trimmed"
        return 0, SMART if name == "smartctl" else "Lifetime writes:          6752 GB\n"
    return 0, "x\n"


A.sh = fake_sh
A.sleep = lambda s: events.append(("sleep", s))


def drive(fail_trim=None):
    events.clear()
    FAIL_TRIM["k"] = fail_trim
    tmp = tempfile.mkdtemp(prefix="a2a-test-")
    A.DATA_ROOT = os.path.join(tmp, "data")
    a = types.SimpleNamespace(out=os.path.join(tmp, "out"), data_root=A.DATA_ROOT, scratch=os.path.join(tmp, "s"),
                              build="build-s6", quick=False, allocation=PLAN["baseline_allocation"])
    os.makedirs(os.path.join(a.out, "raw"))
    p = object.__new__(pilot.Pilot)
    p.a, p.plan, p.alloc = a, PLAN, PLAN["allocations"][a.allocation]
    p.port, p.runs, p.results, p.env = 22000, [], {}, {}
    try:
        res = A.execute(p, PLAN)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return p, res


p, res = drive()
reads = [("root", "smartctl"), ("root", "sg_logs"), ("root", "tune2fs")]
one = reads + [("root", "fstrim"), ("sleep", 1800)] + reads + ["RUN"] * 12 + reads
got = [("RUN" if e[0] == "server" else e) for e in events if e[0] != "client"]
assert got == one * 5, got[:40]
assert [r["label"] for r in p.runs] == [A.label(k, x) for k in range(1, 6) for x in A.ORDER]
srv = [e[1] for e in events if e[0] == "server"]
cli = [e[1] for e in events if e[0] == "client"]
assert len(srv) == len(cli) == 60
for i, (s, c) in enumerate(zip(srv, cli)):
    slot = A.ORDER[i % 12]
    s = s[s.index("build-s6/snc_server"):]
    ref_s, ref_c = (fsrv, fcli) if slot[0] in "SF" else (csrv, ccli)
    assert mask(s) == mask(ref_s), (slot, s)
    assert mask(c) == mask(ref_c), (slot, c, ref_c)
assert len(res["sessions"]) == 5 and all(s["trim"]["rc"] == 0 for s in res["sessions"])
# fixed and sentinel reps take CP-1A's fixed-rep path with memory measurement; capacity reps do not
assert all(("rss_bytes_per_object" in r) == (r["slot"][0] in "SF") for r in p.runs)
assert all(s["start"]["smart"]["bms_progress_pct"] == 79.61 for s in res["sessions"])
p, res = drive(fail_trim=3)
assert res["verdict"] == "INCOMPLETE" and "fstrim failed" in res["aborted"], res
assert len(p.runs) == 24 and ("root", "fstrim") in events            # sessions 1, 2 only; 3 stopped at trim
assert events[-1] == ("root", "fstrim")                               # nothing after the failed trim
# extra_preflight
env = dict(snc_server_sha256=A.cp1a_record()["snc_server_sha256"], disks=dict(data=["sdb"]))
env.update({k: v for k, v in A.cp1a_record().items()})
fakep = types.SimpleNamespace(env=env)
for timer, sudo, tgt, disks, want in (("disabled\n", 0, "/srv/snc-data\n", ["sdb"], 0),
                                      ("enabled\n", 0, "/srv/snc-data\n", ["sdb"], 1),
                                      ("disabled\n", 1, "/srv/snc-data\n", ["sdb"], 4),
                                      ("disabled\n", 0, "/\n", ["sda"], 2)):
    def sh2(cmd, root=False, timer=timer, sudo=sudo, tgt=tgt):
        if cmd[:2] == ["systemctl", "is-enabled"]:
            return 0, timer
        if cmd[:2] == ["systemctl", "is-active"]:
            return 3, "inactive\n" if timer == "disabled\n" else "active\n"
        if cmd[:3] == ["sudo", "-n", "-l"]:
            return sudo, ""
        if cmd[0] == "findmnt":
            return 0, tgt
        raise AssertionError(cmd)
    A.sh = sh2
    fakep.env["disks"] = dict(data=disks)
    bad = A.extra_preflight(fakep, A.cp1a_record())
    assert len(bad) == want, (timer, sudo, tgt, bad)
A.sh = fake_sh
print("A2 ok")

# --- A3 ---
FP = [901120, 933888, 933888, 950272, 933888]
CT = [4028679, 4033653, 4038215, 4031898, 4002903]


def runs(fixed=None, cap=None, sent=None, over=None):
    """5 sessions; fixed[k] / cap[k] = 5 values; sent[k] = (S0, S1) p99; over[(k, slot)] = changes."""
    out = []
    for k in range(1, 6):
        f = (fixed or {}).get(k, FP)
        c = (cap or {}).get(k, CT)
        s0, s1 = (sent or {}).get(k, (933888, 933888))
        for x in A.ORDER:
            if x[0] == "C":
                r = dict(total_ops_s=c[int(x[1]) - 1], errors=0)
            else:
                r = T.rep(A.ANCHOR, p99={"S0": s0, "S1": s1}.get(x) or f[int(x[1]) - 1], window=60)
            r.update(phase="a2a", session=k, slot=x, label=A.label(k, x))
            r.update((over or {}).get((k, x), {}))
            out.append(r)
    return out


V = lambda rs: A.judge(rs, PLAN)
v = V(runs())
assert v["verdict"] == "STABLE", v["why"]
cv_p = pilot.cv(FP)
assert v["N_raw"] == 3 * max(v["cv_p99_between"], v["cv_thr_between"], v["max_cv_p99_within"], v["max_cv_thr_within"])
assert v["max_cv_p99_within"] == cv_p and v["cv_p99_between"] == 0
# max, not median, of the within-session CVs: one noisy session stops
noisy = [700000, 933888, 933888, 950272, 1200000]
assert pilot.cv(noisy) > 0.10
assert V(runs(fixed={3: noisy}))["verdict"] == "STOP"
# max over sessions: differing sessions all under 10%; the noisiest sets max_cv_p99_within and N_raw
mid = [880000, 933888, 933888, 990000, 1000000]                        # CV ~5.5%
vd = V(runs(fixed={2: mid}))
assert vd["verdict"] == "STABLE", vd["why"]
assert vd["max_cv_p99_within"] == pilot.cv(mid) > 2 * pilot.cv(FP)
assert vd["N_raw"] == 3 * max(pilot.cv(mid), vd["cv_p99_between"], vd["cv_thr_between"], vd["max_cv_thr_within"])
# between-session p99: medians spread so CV_between is just above / below 10% with tight sessions
def spread(scale):
    base = [1000000 + int(scale * d) for d in (-2, -1, 0, 1, 2)]
    return {k: [m - 16000, m - 8000, m, m + 8000, m + 16000] for k, m in enumerate(base, 1)}, base
for scale, want in ((60000, "STABLE"), (70000, "STOP")):
    fx, base = spread(scale)
    vv = V(runs(fixed=fx, sent={k: (fx[k][2], fx[k][2]) for k in fx}))
    cvb = pilot.cv(base)
    assert (cvb > 0.10) == (want == "STOP"), (scale, cvb)
    assert vv["verdict"] == want, (scale, cvb, vv["why"])
# between-session throughput CV > 10%
capx = {k: [m - 2000, m - 1000, m, m + 1000, m + 2000] for k, m in
        enumerate([3000000, 3500000, 4000000, 4500000, 5000000], 1)}
assert V(runs(cap=capx))["verdict"] == "STOP"
# symmetric sentinels, inclusive edge
n = Fraction(V(runs())["N_raw"])
edge = 1 + n
a0, b0 = edge.denominator, edge.numerator                              # b0 / a0 == 1 + N_raw exactly
for k in (1, 5):
    assert V(runs(sent={k: (a0, b0)}))["verdict"] == "STABLE"           # degradation at the edge
    assert V(runs(sent={k: (a0, b0 + 1)}))["verdict"] == "STOP"
    assert V(runs(sent={k: (b0, a0)}))["verdict"] == "STABLE"           # improvement at the edge
    assert V(runs(sent={k: (b0 + 1, a0)}))["verdict"] == "STOP"         # a large improvement also stops
# unsustained anchor reps, capacity errors
for key, ch in (((2, "F3"), dict(overloaded=1)), ((4, "S0"), dict(index_refused=2)), ((5, "S1"), dict(drained=False)),
                ((1, "C2"), dict(errors=1)), ((3, "C5"), dict(failed=True))):
    assert V(runs(over={key: ch}))["verdict"] == "STOP", key
# a missing run
assert V(runs()[:-1])["verdict"] == "INCOMPLETE"
# multimodal: two clusters of session medians, CV_between < 10%
mm = {k: [m - 4000, m - 2000, m, m + 2000, m + 4000] for k, m in
      enumerate([900000, 905000, 910000, 1050000, 1055000], 1)}
vm = V(runs(fixed=mm, sent={k: (mm[k][2], mm[k][2]) for k in mm}))
assert pilot.cv([900000, 905000, 910000, 1050000, 1055000]) < 0.10
assert vm["verdict"] == "STOP" and vm["multimodal_p99"]["flag"], vm
even = {k: [m - 30000, m - 15000, m, m + 15000, m + 30000] for k, m in
        enumerate([900000, 937500, 975000, 1012500, 1050000], 1)}
ve = V(runs(fixed=even, sent={k: (even[k][2], even[k][2]) for k in even}))
assert not ve["multimodal_p99"]["flag"], ve
print("A3 ok")
print("ok")
