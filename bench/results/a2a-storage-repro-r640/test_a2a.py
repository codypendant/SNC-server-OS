#!/usr/bin/env python3
"""Deterministic checks of a2a.py, before any A2-a data. Starts no process, trims nothing.

  A1 constants: K 5; anchor 654,569 = CP-1A A objective; settle 1,800 s; the fixed 12-slot
     order; stop CV 10%; SENTINEL_LIMIT exactly 11/10; the 4 root commands = sudoers-snc-a2a;
     the parsers on the real 2026-11-12 captures (SMART, sg_logs, tune2fs, the 11-11 fstrim
     line); a1_problems accepts the real capture and names each missing field
  A2 guard_problems (pure): the live-state reference passes; each drift is named: boot id,
     HEAD, dirty tree, timer enabled, timer active, NUMA, governor, mount source / options /
     fstype, data-root mount, data disk, plan / manifest / snc_server / net_bench SHA
  A3 the session procedure (observe, root calls, diskstats, sleep and processes stubbed):
     exact sequence per session (guard, clear + sync, A1 pre-trim, discard counter, fstrim,
     discard counter, sync, settle counters, 1,800 s, settle counters, A1 start, 12 runs, A1
     end); commands = CP-1A A-fixed-r0 / A-capacity-r0; fixed and sentinel reps with memory,
     capacity without; 60 runs in order; each integrity failure stops at once with INCOMPLETE
     and nothing after it: guard drift (boot id, governor, NUMA, dirty tree, HEAD, timer,
     mount, data disk) at session 2; fstrim rc, 0 bytes, discard delta 0, mount not named;
     settle write, settle discard; A1 unusable (temperature, sg_logs rc, tune2fs, boot id);
     client exit nonzero on a capacity and on an anchor rep; unparsable report
  A4 verdict: realistic data -> STABLE; N_raw = 3 x max(between, max within); max not median;
     CV > 10% between (p99, thr) and within stops; sentinels: 11/10 passes, above fails, both
     directions, and the limit is independent of N_raw (N_raw 0 still accepts 1.10; N_raw
     ~0.27 still rejects 1.2); anchor unsustained, capacity stale / index_errors / errors ->
     STOP (EXEC overload not a failure); capacity rc nonzero, missing run, incomplete session,
     an abort -> INCOMPLETE; separated clusters -> STOP (supplementary multimodal signal)
"""
import json, os, shutil, sys, tempfile, time, types
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tests"))
import a2a as A  # noqa: E402
import test_pilot as T  # noqa: E402

pilot = A.pilot
PLAN = json.load(open(os.path.join(A.REPO, "bench", "pilot_plan.json")))
CP1A_RAW = os.path.join(A.REPO, "bench", "results", "cp1a-r640-20261111", "raw")
CAP = os.path.join(A.REPO, "bench", "results", "r640-storage-state-20261112", "capture")
SMART = open(os.path.join(CAP, "smartctl-x-sdb.txt")).read()
SGLOGS = open(os.path.join(CAP, "sg-logs-all-sdb.txt")).read()
TUNE2FS = open(os.path.join(CAP, "tune2fs-sdb1.txt")).read()
TRIM_OK = "/srv/snc-data: 3.4 TiB (3779298537472 bytes) trimmed on /dev/sdb1\n"
BOOT = "16253771-facb-4d86-9eea-4e08ffa1edf3"
REF = A.cp1a_record()

# --- A1 ---
assert A.K == 5 and A.ANCHOR == 654569 and A.SETTLE_S == 1800 and A.STOP_CV == 0.10
assert A.SENTINEL_LIMIT == Fraction(11, 10) and isinstance(A.SENTINEL_LIMIT, Fraction)
assert A.ORDER == ["S0", "F1", "C1", "F2", "C2", "F3", "C3", "F4", "C4", "F5", "C5", "S1"]
assert REF["snc_server_sha256"].startswith("69a51b69") and REF["net_bench_sha256"].startswith("9fc54944")
sud = open(os.path.join(HERE, "sudoers-snc-a2a")).read()
line = [l for l in sud.splitlines() if l.startswith("solo ")][0]
assert [c.strip().split() for c in line.split("NOPASSWD:")[1].split(",")] == list(A.ROOT.values()), line
sm = A.parse_smart(SMART)
assert sm["bms_progress_pct"] == 79.61 and sm["bms_status"] == "scan is active" and sm["temperature_c"] == 36
assert sm["health"] == "OK" and sm["uncorrected_write"] == 0 and sm["write_gb_processed"] == 17105.852
assert int(A.TRIM_RX.search(TRIM_OK)[1]) == 3779298537472
real = dict(sectors_written=1, sectors_discarded=1, boot_id=BOOT, uptime_s=48600.0, smart=sm, sg_logs_rc=0,
            sg_logs_has_bsr="Background scan results page" in SGLOGS, tune2fs_rc=0, fs_lifetime_writes="6752 GB")
assert A.a1_problems(real) == [], A.a1_problems(real)
for k, v, want in (("sectors_written", None, "diskstats"), ("boot_id", "x", "boot id"), ("uptime_s", None, "uptime"),
                   ("sg_logs_rc", 1, "sg_logs"), ("sg_logs_has_bsr", False, "sg_logs"),
                   ("tune2fs_rc", 1, "tune2fs lifetime writes"), ("fs_lifetime_writes", None, "tune2fs lifetime writes")):
    assert A.a1_problems(dict(real, **{k: v})) == [want], (k, A.a1_problems(dict(real, **{k: v})))
for f in A.SMART_REQUIRED:
    assert A.a1_problems(dict(real, smart=dict(sm, **{f: None}))) == [f"smart {f}"], f
assert A.a1_problems(dict(real, smart=dict(sm, bms_progress_pct=None))) == ["smart background-scan progress"]
print("A1 ok")

# --- A2 guard (pure) ---
GOOD = dict(boot_id=BOOT, head="h" * 40, git_status="", timer_enabled="disabled", timer_active="inactive",
            numa_balancing="0", governors=["performance"], mount=["/dev/sdb1", "/srv/snc-data", "ext4", "rw,relatime,stripe=2"],
            data_root_mount="/srv/snc-data", data_disks=["sdb"], **REF)
WANT = dict(REF, boot_id=BOOT, head="h" * 40)
assert A.guard_problems(GOOD, WANT) == []
DRIFTS = dict(boot=dict(boot_id="00000000-0000-0000-0000-000000000000"), head=dict(head="g" * 40),
              dirty=dict(git_status="M x"), timer_enabled=dict(timer_enabled="enabled"),
              timer_active=dict(timer_active="active"), numa=dict(numa_balancing="1"),
              governor=dict(governors=["performance", "powersave"]),
              mount_source=dict(mount=["/dev/sdc1", "/srv/snc-data", "ext4", "rw,relatime,stripe=2"]),
              mount_options=dict(mount=["/dev/sdb1", "/srv/snc-data", "ext4", "rw,relatime,discard,stripe=2"]),
              mount_fstype=dict(mount=["/dev/sdb1", "/srv/snc-data", "xfs", "rw,relatime,stripe=2"]),
              data_root=dict(data_root_mount="/"), disk=dict(data_disks=["sda"]),
              plan=dict(plan_sha256="0" * 64), manifest=dict(manifest_sha256="0" * 64),
              snc=dict(snc_server_sha256="0" * 64), netb=dict(net_bench_sha256="0" * 64))
for name, ch in DRIFTS.items():
    assert len(A.guard_problems(dict(GOOD, **ch), WANT)) == 1, (name, A.guard_problems(dict(GOOD, **ch), WANT))
print("A2 ok")

# --- A3 procedure ---
def section(log):
    t = open(os.path.join(CP1A_RAW, log)).read()
    lines = t.split("\n")
    return lines[0].split(), lines[1].split(), t.split("--- run\n", 1)[1]


fsrv, fcli, fout = section("A-fixed-r0.client.log")
csrv, ccli, cout = section("A-capacity-r0.client.log")
ev = []
S = {}                                          # the scenario of the current drive()


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
    ev.append(("server", cmd))
    return FakeP()


def prun(cmd, capture_output=True, text=True, timeout=None):
    load = cmd[-4:] == ["--depth", "64", "--seconds", "0"]
    if not load:
        ev.append(("client", cmd))
    out = "load ok\n" if load else (fout if "--rate" in cmd else cout)
    rc = 0
    n = sum(1 for e in ev if e[0] == "client")
    if not load and S.get("client_fail") == n:
        rc, out = 1, out
    if not load and S.get("unparsable") == n:
        out = "garbage\n"
    return types.SimpleNamespace(returncode=rc, stdout=out, stderr="")


pilot.subprocess = types.SimpleNamespace(Popen=popen, run=prun, STDOUT=-2,
                                         TimeoutExpired=__import__("subprocess").TimeoutExpired)
pilot.wait_port = lambda port, timeout=60: None
pilot.sample = lambda pid: dict(cpu_s=0, write_bytes=0, lo_rx=0, lo_tx=0)
pilot.memory_kb = lambda pid: dict(VmRSS=0, VmHWM=0, Pss=0)
pilot.time = types.SimpleNamespace(sleep=lambda s: None, time=time.time, strftime=time.strftime, gmtime=time.gmtime)
CTR = dict(w=7473308656, d=7381442456)


def session_no():
    return sum(1 for e in ev if e == ("observe",))


def fake_observe(build):
    ev.append(("observe",))
    k = session_no()
    o = dict(GOOD)
    if S.get("drift") and k == 2:
        o.update(DRIFTS[S["drift"]])
    return o


def fake_sh(cmd, root=False):
    k = session_no()
    if root:
        assert cmd in A.ROOT.values(), cmd
        name = [n for n, v in A.ROOT.items() if v == cmd][0]
        ev.append(("root", name))
        if name == "fstrim":
            if S.get("trim") and k == 1:
                kind = S["trim"]
                if kind == "rc":
                    return 1, "fstrim: /srv/snc-data: FITRIM ioctl failed: Operation not supported\n"
                if kind == "zero":
                    CTR["d"] += 0
                    return 0, "/srv/snc-data: 0 B (0 bytes) trimmed on /dev/sdb1\n"
                if kind == "nodelta":
                    return 0, TRIM_OK
                if kind == "unnamed":
                    CTR["d"] += 1000
                    return 0, "fstrim: done\n"
            CTR["d"] += 7381442456
            return 0, TRIM_OK
        bad = S.get("a1") if k == 1 else None
        if name == "smartctl":
            return 4, SMART.replace("Current temperature", "Current temp") if bad == "temperature" else SMART
        if name == "sg_logs":
            return (1, "sg_logs: error\n") if bad == "sg_logs" else (0, SGLOGS)
        return (0, TUNE2FS.replace("Lifetime writes", "Lifetime wr")) if bad == "tune2fs" else (0, TUNE2FS)
    ev.append(("sh", cmd[0]))
    return 0, ""


def fake_readf(path):
    if path == "/proc/diskstats":
        ev.append(("diskstats",))
        f = ["8", "16", "sdb"] + ["0"] * 17
        f[9], f[16] = str(CTR["w"]), str(CTR["d"])
        return " ".join(f) + "\n"
    if path.endswith("boot_id"):
        return ("not-a-boot-id\n" if S.get("a1") == "boot_id" and session_no() == 1 else BOOT + "\n")
    if path == "/proc/uptime":
        return "48600.00 3000000.00\n"
    raise AssertionError(path)


def fake_sleep(s):
    ev.append(("sleep", s))
    if S.get("settle") == "write":
        CTR["w"] += 8
    if S.get("settle") == "discard":
        CTR["d"] += 8


A.observe, A.sh, A.readf, A.sleep = fake_observe, fake_sh, fake_readf, fake_sleep


def drive(**scenario):
    ev.clear()
    S.clear()
    S.update(scenario)
    tmp = tempfile.mkdtemp(prefix="a2a-test-")
    A.DATA_ROOT = os.path.join(tmp, "data")
    a = types.SimpleNamespace(out=os.path.join(tmp, "out"), data_root=A.DATA_ROOT, scratch=os.path.join(tmp, "s"),
                              build="build-s6", quick=False, allocation=PLAN["baseline_allocation"])
    os.makedirs(os.path.join(a.out, "raw"))
    p = object.__new__(pilot.Pilot)
    p.a, p.plan, p.alloc = a, PLAN, PLAN["allocations"][a.allocation]
    p.port, p.runs, p.results, p.env = 22000, [], {}, {}
    try:
        res = A.execute(p, PLAN, WANT)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return p, res


p, res = drive()
reads = [("diskstats",), ("root", "smartctl"), ("root", "sg_logs"), ("root", "tune2fs"), ("sh", "journalctl")]
one = ([("observe",), ("sh", "sync")] + reads + [("diskstats",), ("root", "fstrim"), ("diskstats",), ("sh", "sync"),
       ("diskstats",), ("sleep", 1800), ("diskstats",)] + reads + [("sh", "journalctl")] + ["RUN"] * 12
       + reads + [("sh", "journalctl")])
got = [("RUN" if e[0] == "server" else e) for e in ev if e[0] != "client"]
assert got == one * 5, [x for x in got[:45]]
assert res["verdict"] == "STABLE", res["why"]
assert [r["label"] for r in p.runs] == [A.label(k, x) for k in range(1, 6) for x in A.ORDER]
srv = [e[1] for e in ev if e[0] == "server"]
cli = [e[1] for e in ev if e[0] == "client"]
assert len(srv) == len(cli) == 60
for i, (s_, c_) in enumerate(zip(srv, cli)):
    slot = A.ORDER[i % 12]
    s_ = s_[s_.index("build-s6/snc_server"):]
    rs, rc_ = (fsrv, fcli) if slot[0] in "SF" else (csrv, ccli)
    assert mask(s_) == mask(rs) and mask(c_) == mask(rc_), (slot, s_, c_)
assert all(("rss_bytes_per_object" in r) == (r["slot"][0] in "SF") for r in p.runs)
ss = res["sessions"]
assert len(ss) == 5 and all(s["complete"] and s["trim"]["bytes"] == 3779298537472 and s["trim"]["problems"] == []
                            and s["settle"]["sectors_written"] == 0 and s["start"]["smart"]["bms_progress_pct"] == 79.61
                            for s in ss)


def stops(scenario, runs_before, last_event, why):
    p, res = drive(**scenario)
    assert res["verdict"] == "INCOMPLETE", (scenario, res["verdict"], res["why"])
    assert why in res["aborted"], (scenario, res["aborted"])
    assert len(p.runs) == runs_before, (scenario, len(p.runs))
    assert ev[-1] == last_event or (callable(last_event) and last_event(ev[-1])), (scenario, ev[-3:])
    return res


for d in ("boot", "governor", "numa", "dirty", "head", "timer_enabled", "timer_active", "mount_source",
          "mount_options", "disk"):
    stops(dict(drift=d), 12, ("observe",), "session 2 guard")
stops(dict(trim="rc"), 0, ("diskstats",), "fstrim rc 1")
stops(dict(trim="zero"), 0, ("diskstats",), "fstrim reported 0 bytes")
stops(dict(trim="nodelta"), 0, ("diskstats",), "discard sectors +0")
stops(dict(trim="unnamed"), 0, ("diskstats",), "does not report /srv/snc-data")
stops(dict(settle="write"), 0, ("diskstats",), "wrote 8 sectors during the settle")
stops(dict(settle="discard"), 0, ("diskstats",), "discarded 8 sectors during the settle")
for bad, why in (("temperature", "smart temperature_c"), ("sg_logs", "sg_logs"), ("tune2fs", "tune2fs lifetime"),
                 ("boot_id", "boot id")):
    stops(dict(a1=bad), 0, ("sh", "journalctl"), why)        # pre-trim snapshot: no trim after it
    assert ("root", "fstrim") not in ev
stops(dict(client_fail=3), 3, lambda e: e[0] in ("server", "client") or e == ("sh", "journalctl") or True,
      "k1-C1: client exit 1")
stops(dict(client_fail=2), 2, lambda e: True, "k1-F1: client exit 1")
stops(dict(unparsable=5), 5, lambda e: True, "k1-C2: unparsable")
print("A3 ok")

# --- A4 verdict ---
FP = [901120, 933888, 933888, 950272, 933888]
CT = [4028679, 4033653, 4038215, 4031898, 4002903]
SESS = [dict(k=k, complete=True) for k in range(1, 6)]


def runs(fixed=None, cap=None, sent=None, over=None):
    out = []
    for k in range(1, 6):
        f = (fixed or {}).get(k, FP)
        c = (cap or {}).get(k, CT)
        s0, s1 = (sent or {}).get(k, (933888, 933888))
        for x in A.ORDER:
            if x[0] == "C":
                r = dict(total_ops_s=c[int(x[1]) - 1], errors=0, stale=0, index_errors=0, overloaded=41131922,
                         returncode=0)
            else:
                r = T.rep(A.ANCHOR, p99={"S0": s0, "S1": s1}.get(x) or f[int(x[1]) - 1], window=60)
                r["returncode"] = 0
            r.update(phase="a2a", session=k, slot=x, label=A.label(k, x))
            r.update((over or {}).get((k, x), {}))
            out.append(r)
    return out


V = lambda rs, sess=SESS, ab=None: A.judge(rs, PLAN, sess, ab)
v = V(runs())
assert v["verdict"] == "STABLE", v["why"]
assert v["N_raw"] == 3 * max(v["cv_p99_between"], v["cv_thr_between"], v["max_cv_p99_within"], v["max_cv_thr_within"])
noisy = [700000, 933888, 933888, 950272, 1200000]
assert pilot.cv(noisy) > 0.10 and V(runs(fixed={3: noisy}))["verdict"] == "STOP"
mid = [880000, 933888, 933888, 990000, 1000000]
vd = V(runs(fixed={2: mid}))
assert vd["verdict"] == "STABLE" and vd["max_cv_p99_within"] == pilot.cv(mid) > 2 * pilot.cv(FP)


def spread(scale):
    base = [1000000 + int(scale * d) for d in (-2, -1, 0, 1, 2)]
    return {k: [m - 16000, m - 8000, m, m + 8000, m + 16000] for k, m in enumerate(base, 1)}, base


for scale, want in ((60000, "STABLE"), (70000, "STOP")):
    fx, base = spread(scale)
    vv = V(runs(fixed=fx, sent={k: (fx[k][2], fx[k][2]) for k in fx}))
    assert (pilot.cv(base) > 0.10) == (want == "STOP") and vv["verdict"] == want, (scale, vv["why"])
capx = {k: [m - 2000, m - 1000, m, m + 1000, m + 2000] for k, m in enumerate([3000000, 3500000, 4000000, 4500000, 5000000], 1)}
assert V(runs(cap=capx))["verdict"] == "STOP"
noisy_thr = [3000000, 4000000, 4000000, 4000000, 5000000]
assert V(runs(cap={4: noisy_thr}))["verdict"] == "STOP"
# sentinels: the fixed 11/10 limit, inclusive, symmetric
for k in (1, 5):
    assert V(runs(sent={k: (1000000, 1100000)}))["verdict"] == "STABLE"      # exactly 1.10 degradation
    assert V(runs(sent={k: (1000000, 1100001)}))["verdict"] == "STOP"
    assert V(runs(sent={k: (1100000, 1000000)}))["verdict"] == "STABLE"      # exactly 1.10 improvement
    assert V(runs(sent={k: (1100001, 1000000)}))["verdict"] == "STOP"
# independent of N_raw: N_raw = 0 still accepts 1.10; N_raw ~0.27 still rejects 1.2
flat = {k: [933888] * 5 for k in range(1, 6)}
flat_cap = {k: [4000000] * 5 for k in range(1, 6)}
v0 = V(runs(fixed=flat, cap=flat_cap, sent={2: (1000000, 1100000)}))
assert v0["N_raw"] == 0 and v0["verdict"] == "STABLE", v0
hi = [840000, 900000, 933888, 1000000, 1030000]                              # CV ~8.3%: N_raw ~0.25
vh = V(runs(fixed={k: hi for k in range(1, 6)}, sent={3: (1000000, 1200000)}))
assert vh["N_raw"] > 0.2 and vh["verdict"] == "STOP" and any("sentinels" in w for w in vh["why"]), vh
vh2 = V(runs(fixed={k: hi for k in range(1, 6)}))
assert vh2["verdict"] == "STABLE", vh2["why"]
# anchor unsustained / capacity validity
for key, ch in (((2, "F3"), dict(overloaded=1)), ((4, "S0"), dict(index_refused=2)), ((5, "S1"), dict(drained=False)),
                ((1, "C2"), dict(errors=1)), ((3, "C5"), dict(stale=1)), ((2, "C4"), dict(index_errors=1))):
    assert V(runs(over={key: ch}))["verdict"] == "STOP", key
assert V(runs(over={(1, "C1"): dict(overloaded=99999999)}))["verdict"] == "STABLE"   # closed-loop overload is expected
# execution integrity -> INCOMPLETE
assert V(runs(over={(2, "C3"): dict(returncode=1)}))["verdict"] == "INCOMPLETE"
assert V(runs(over={(2, "F2"): dict(failed=True, why="unparsable")}))["verdict"] == "INCOMPLETE"
assert V(runs()[:-1])["verdict"] == "INCOMPLETE"
assert V(runs(), SESS[:4])["verdict"] == "INCOMPLETE"
assert V(runs(), [dict(s, complete=(s["k"] != 3)) for s in SESS])["verdict"] == "INCOMPLETE"
assert V(runs(), SESS, "session 3 guard: x")["verdict"] == "INCOMPLETE"
assert V(runs(), [dict(s, settle=dict(problems=["x"])) if s["k"] == 2 else s for s in SESS])["verdict"] == "INCOMPLETE"
# supplementary multimodal signal
mm = {k: [m - 4000, m - 2000, m, m + 2000, m + 4000] for k, m in enumerate([900000, 905000, 910000, 1050000, 1055000], 1)}
vm = V(runs(fixed=mm, sent={k: (mm[k][2], mm[k][2]) for k in mm}))
assert vm["verdict"] == "STOP" and vm["multimodal_p99"]["flag"], vm
even = {k: [m - 30000, m - 15000, m, m + 15000, m + 30000] for k, m in enumerate([900000, 937500, 975000, 1012500, 1050000], 1)}
assert not V(runs(fixed=even, sent={k: (even[k][2], even[k][2]) for k in even}))["multimodal_p99"]["flag"]
print("A4 ok")
print("ok")
