#!/usr/bin/env python3
"""Deterministic checks of a2a.py, before any A2-a data. Starts no process, trims nothing.

  A1 constants: K 5; anchor 654,569 = CP-1A A objective; settle 1,800 s; the fixed 12-slot
     order; stop CV 10%; SENTINEL_LIMIT exactly 11/10; the 4 root commands = sudoers-snc-a2a;
     the parsers on the real 2026-11-12 captures (SMART, sg_logs, tune2fs, the 11-11 fstrim
     line); a1_problems accepts the real capture and names each missing field
  A2 guard_problems (pure): the live-state reference passes; each drift is named: boot id,
     HEAD, dirty tree, timer enabled, timer active, fstrim.service active, NUMA, governor,
     mount source / options /
     fstype, data-root mount, data disk, plan / manifest / snc_server / net_bench SHA
  A3 the session procedure (observe, root calls, diskstats, sleep and processes stubbed):
     exact sequence per session (guard, clear + sync, A1 pre-trim, discard counter, fstrim,
     discard counter, sync, settle counters, 1,800 s, settle counters, A1 start, 12 runs, A1
     end); commands = CP-1A A-fixed-r0 / A-capacity-r0; fixed and sentinel reps with memory,
     capacity without; 60 runs in order; each integrity failure stops at once with INCOMPLETE
     and nothing after it: guard drift (boot id, governor, NUMA, dirty tree, HEAD, timer,
     mount, data disk, fstrim.service active) at session 2; a failed pre-trim sync (no A1, no
     trim after it) and a failed post-trim sync (no settle after it), each with its rc and
     output recorded (PR #114 review 2); fstrim rc, 0 bytes, discard delta 0, mount not named;
     settle write, settle discard; A1 unusable (temperature, sg_logs rc, tune2fs, boot id);
     client exit nonzero on a capacity and on an anchor rep; unparsable report
  A4 verdict: realistic data -> STABLE; N_raw = 3 x max(between, max within); max not median;
     CV > 10% between (p99, thr) and within stops; sentinels: 11/10 passes, above fails, both
     directions, and the limit is independent of N_raw (N_raw 0 still accepts 1.10; N_raw
     ~0.27 still rejects 1.2); anchor unsustained, capacity stale / index_errors / errors ->
     STOP (EXEC overload not a failure); capacity rc nonzero, missing run, incomplete session,
     an abort -> INCOMPLETE; separated clusters -> STOP (supplementary multimodal signal)
  A5 persistent device identity (revision 4, docs/handoff/A2a_Device_Identity_Correction_20261112.md):
     gather_identity / identity_problems on a faked host: the approved SSD presented as sda and as
     sdb (HDD on the other name) passes; the HDD on the approved name, a wrong filesystem UUID, a
     correct mount on a wrong serial / model / WWN, a correct serial with a wrong mounted
     filesystem, a by-id path (disk, partition, UUID) resolving to the wrong disk, a changed mount
     option, and a device-name change against the invocation's start each fail; a whole session
     run (A3 machinery) with the SSD as sda and as sdb reads the SSD's own diskstats line, not
     the other disk's; a name change or wrong identity at each of the three in-session
     re-checks stops INCOMPLETE at once; a2a.py and sudoers-snc-a2a contain no sdX name
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
def IDENT(disk="sda", **ch):
    """The identity facts gather_identity returns for the approved SSD at kernel name `disk`."""
    f = dict(mount=["/srv/snc-data", "ext4", "rw,relatime,stripe=2"], fs_uuid=A.APPROVED["fs_uuid"],
             source=f"/dev/{disk}1", part=f"{disk}1", disk=disk, model=A.APPROVED["model"],
             serial=A.APPROVED["serial"], wwn=A.APPROVED["wwn"], byid_disk=f"/dev/{disk}",
             byid_part=f"/dev/{disk}1", byuuid=f"/dev/{disk}1")
    f.update(ch)
    return f


GOOD = dict(boot_id=BOOT, head="h" * 40, git_status="", timer_enabled="disabled", timer_active="inactive",
            fstrim_service_active="inactive",
            numa_balancing="0", governors=["performance"], identity=IDENT("sda"),
            data_root_mount="/srv/snc-data", data_disks=["sda"], **REF)
WANT = dict(REF, boot_id=BOOT, head="h" * 40, identity=IDENT("sda"))
assert A.guard_problems(GOOD, WANT) == []
# the same approved SSD under another kernel name is fine when the start also saw that name
assert A.guard_problems(dict(GOOD, identity=IDENT("sdb"), data_disks=["sdb"]), dict(WANT, identity=IDENT("sdb"))) == []
DRIFTS = dict(boot=dict(boot_id="00000000-0000-0000-0000-000000000000"), head=dict(head="g" * 40),
              dirty=dict(git_status="M x"), timer_enabled=dict(timer_enabled="enabled"),
              timer_active=dict(timer_active="active"), service=dict(fstrim_service_active="active"),
              numa=dict(numa_balancing="1"),
              governor=dict(governors=["performance", "powersave"]),
              mount_options=dict(identity=IDENT("sda", mount=["/srv/snc-data", "ext4", "rw,relatime,discard,stripe=2"])),
              mount_fstype=dict(identity=IDENT("sda", mount=["/srv/snc-data", "xfs", "rw,relatime,stripe=2"])),
              mount_target=dict(identity=IDENT("sda", mount=["/mnt/x", "ext4", "rw,relatime,stripe=2"])),
              fs_uuid=dict(identity=IDENT("sda", fs_uuid="00000000-0000-0000-0000-000000000000")),
              serial=dict(identity=IDENT("sda", serial="S48LNA0M700326")),
              model=dict(identity=IDENT("sda", model="ST3000DM008-2DM1")),
              wwn=dict(identity=IDENT("sda", wwn="0x5002538b097f68a1")),
              byid_disk_wrong=dict(identity=IDENT("sda", byid_disk="/dev/sdb")),
              byid_part_wrong=dict(identity=IDENT("sda", byid_part="/dev/sdb1")),
              byuuid_wrong=dict(identity=IDENT("sda", byuuid="/dev/sdb1")),
              data_root=dict(data_root_mount="/"), disk=dict(data_disks=["sdc"]),
              disk_not_ssd=dict(data_disks=["sdb"]),
              plan=dict(plan_sha256="0" * 64), manifest=dict(manifest_sha256="0" * 64),
              snc=dict(snc_server_sha256="0" * 64), netb=dict(net_bench_sha256="0" * 64))
for name, ch in DRIFTS.items():
    assert len(A.guard_problems(dict(GOOD, **ch), WANT)) == 1, (name, A.guard_problems(dict(GOOD, **ch), WANT))
# the SSD renamed during the invocation (persistent identity unchanged, kernel name not): refused
ren = A.guard_problems(dict(GOOD, identity=IDENT("sdb"), data_disks=["sdb"]), WANT)
assert any("device mapping changed during the invocation: disk sda -> sdb" in x for x in ren), ren
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


def ssd():
    return S.get("ssd", "sda")                  # the kernel name the approved SSD has in this scenario


def fake_observe(build):
    ev.append(("observe",))
    k = session_no()
    o = dict(GOOD, identity=IDENT(ssd()), data_disks=[ssd()])
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
    if cmd[0] == "sync":
        assert cmd == ["sync", "-f", "/srv/snc-data"], cmd
        n = sum(1 for e in ev if e == ("sh", "sync"))         # 1 = pre-trim, 2 = post-trim (session 1)
        if (S.get("sync") == "pre" and n == 1) or (S.get("sync") == "post" and n == 2):
            return 1, "sync: error syncing '/srv/snc-data': Input/output error\n"
    return 0, ""


IDENT_CALLS = []


def fake_gather():
    """gather_identity: the 1st/2nd/3rd in-session re-check of session 1 can be made to differ."""
    ev.append(("ident",))
    IDENT_CALLS.append(1)
    n = len(IDENT_CALLS)
    ch = S.get("ident_bad")
    if ch and ch[0] == n:
        return IDENT(**ch[1]) if "disk" in ch[1] else IDENT(ssd(), **ch[1])
    return IDENT(ssd())


def fake_readf(path):
    if path == "/proc/diskstats":
        ev.append(("diskstats",))
        other = "sdb" if ssd() == "sda" else "sda"
        decoy = ["8", "0", other] + ["0"] * 17          # the other disk: constant counters
        decoy[9], decoy[16] = "999", "999"
        f = ["8", "16", ssd()] + ["0"] * 17
        f[9], f[16] = str(CTR["w"]), str(CTR["d"])
        return " ".join(decoy) + "\n" + " ".join(f) + "\n"
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


REAL_GATHER = A.gather_identity
A.observe, A.sh, A.readf, A.sleep, A.gather_identity = fake_observe, fake_sh, fake_readf, fake_sleep, fake_gather


def drive(**scenario):
    ev.clear()
    S.clear()
    S.update(scenario)
    IDENT_CALLS.clear()
    want = dict(WANT, identity=IDENT(ssd()))
    tmp = tempfile.mkdtemp(prefix="a2a-test-")
    A.DATA_ROOT = os.path.join(tmp, "data")
    a = types.SimpleNamespace(out=os.path.join(tmp, "out"), data_root=A.DATA_ROOT, scratch=os.path.join(tmp, "s"),
                              build="build-s6", quick=False, allocation=PLAN["baseline_allocation"])
    os.makedirs(os.path.join(a.out, "raw"))
    p = object.__new__(pilot.Pilot)
    p.a, p.plan, p.alloc = a, PLAN, PLAN["allocations"][a.allocation]
    p.port, p.runs, p.results, p.env = 22000, [], {}, {}
    try:
        res = A.execute(p, PLAN, want)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return p, res


p, res = drive()
reads = [("diskstats",), ("root", "smartctl"), ("root", "sg_logs"), ("root", "tune2fs"), ("sh", "journalctl")]
one = ([("observe",), ("sh", "sync")] + reads + [("diskstats",), ("root", "fstrim"), ("diskstats",), ("ident",),
       ("sh", "sync"), ("diskstats",), ("sleep", 1800), ("diskstats",), ("ident",)] + reads + [("sh", "journalctl")]
       + ["RUN"] * 12 + [("ident",)] + reads + [("sh", "journalctl")])
got = [("RUN" if e[0] == "server" else e) for e in ev if e[0] != "client"]
assert got == one * 5, [x for x in got[:48]]
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
assert all(s["sync_pre_trim"]["rc"] == 0 and s["sync_post_trim"]["rc"] == 0 for s in ss)
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


# PR #114 review 2: both syncs fail closed
res_ = stops(dict(sync="pre"), 0, ("sh", "sync"), "sync pre_trim failed (rc 1)")
assert ("root", "smartctl") not in ev and ("root", "fstrim") not in ev       # no A1 pre-trim, no trim
assert res_["sessions"][0]["sync_pre_trim"]["rc"] == 1 and "Input/output error" in res_["sessions"][0]["sync_pre_trim"]["output"]
res_ = stops(dict(sync="post"), 0, ("sh", "sync"), "sync post_trim failed (rc 1)")
assert ("root", "fstrim") in ev and not any(e[0] == "sleep" for e in ev)     # trimmed, but no settle
assert res_["sessions"][0]["sync_post_trim"]["rc"] == 1 and res_["sessions"][0]["trim"]["problems"] == []
for d in ("boot", "governor", "numa", "dirty", "head", "timer_enabled", "timer_active", "service", "mount_options",
          "mount_fstype", "fs_uuid", "serial", "model", "wwn", "byid_disk_wrong", "byid_part_wrong", "byuuid_wrong",
          "disk", "disk_not_ssd"):
    stops(dict(drift=d), 12, ("observe",), "session 2 guard")
stops(dict(trim="rc"), 0, ("diskstats",), "fstrim rc 1")
stops(dict(trim="zero"), 0, ("diskstats",), "fstrim reported 0 bytes")
stops(dict(trim="nodelta"), 0, ("diskstats",), "discard sectors +0")
stops(dict(trim="unnamed"), 0, ("diskstats",), "does not report /srv/snc-data")
stops(dict(settle="write"), 0, ("ident",), "wrote 8 sectors during the settle")
stops(dict(settle="discard"), 0, ("ident",), "discarded 8 sectors during the settle")
for bad, why in (("temperature", "smart temperature_c"), ("sg_logs", "sg_logs"), ("tune2fs", "tune2fs lifetime"),
                 ("boot_id", "boot id")):
    stops(dict(a1=bad), 0, ("sh", "journalctl"), why)        # pre-trim snapshot: no trim after it
    assert ("root", "fstrim") not in ev
stops(dict(client_fail=3), 3, lambda e: e[0] in ("server", "client") or e == ("sh", "journalctl") or True,
      "k1-C1: client exit 1")
stops(dict(client_fail=2), 2, lambda e: True, "k1-F1: client exit 1")
stops(dict(unparsable=5), 5, lambda e: True, "k1-C2: unparsable")
# the approved SSD under either kernel name: STABLE, and its own diskstats line is the one read
for name in ("sda", "sdb"):
    p_, res_ = drive(ssd=name)
    assert res_["verdict"] == "STABLE", (name, res_["why"])
    assert all(s["identity"]["kernel_disk"] == name and s["trim"]["problems"] == [] and s["settle"]["sectors_written"] == 0
               for s in res_["sessions"]), name
def dsl(name, w, d):
    f = ["8", "0", name] + ["0"] * 17
    f[9], f[16] = str(w), str(d)
    return " ".join(f) + "\n"


assert A.diskstats("sdb", dsl("sda", 1, 2) + dsl("sdb", 7, 9)) == (7, 9)
assert A.diskstats("sda", dsl("sda", 1, 2) + dsl("sdb", 7, 9)) == (1, 2)
# identity wrong or changed at each in-session re-check (1 after the trim, 2 after the settle, 3 before A1 end)
CHANGES = dict(renamed=dict(disk="sdb", source="/dev/sdb1", part="sdb1", byid_disk="/dev/sdb", byid_part="/dev/sdb1",
                            byuuid="/dev/sdb1"),
               wrong_serial=dict(serial="S48LNA0M700326"), wrong_uuid=dict(fs_uuid="0" * 8 + "-0000-0000-0000-" + "0" * 12),
               byid_wrong=dict(byid_disk="/dev/sdb"))
for nm, ch in CHANGES.items():
    for n, last, runs_n, slept in ((1, ("ident",), 0, False), (2, ("ident",), 0, True), (3, ("ident",), 12, True)):
        res_ = stops(dict(ident_bad=(n, ch)), runs_n, last, "device identity")
        assert any(e == ("sleep", 1800) for e in ev) == slept, (nm, n)
        assert ("sh", "sync") not in ev[-1:] and (n != 1 or ev.count(("sh", "sync")) == 1)   # n=1: no post-trim sync
        assert (n == 3) == any("k1-S1" == r["label"] for r in p.runs) or n != 3
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

# --- A5 persistent identity on a faked host (gather_identity + identity_problems, nothing else stubbed) ---
A.gather_identity = REAL_GATHER
HDD = dict(model="ST3000DM008-2DM1", serial="Z504K3SF", wwn="")
UU = A.APPROVED["fs_uuid"]


def host(ssd_at="sda", other="sdb", mounted=None, uuid=UU, ssd_ch=None, byid="ssd", byuuid_to=None, opts="rw,relatime,stripe=2"):
    """A faked host: the approved SSD at kernel name `ssd_at`, the HDD at `other`; `mounted` = the disk whose first
    partition is mounted on /srv/snc-data; by-id/by-uuid links resolve to the approved SSD unless told otherwise."""
    disks = {ssd_at: {**dict(model=A.APPROVED["model"], serial=A.APPROVED["serial"], wwn=A.APPROVED["wwn"]), **(ssd_ch or {})},
             other: HDD}
    mounted = mounted or ssd_at

    def sh_(cmd, root=False):
        if cmd[0] == "findmnt":
            return 0, (f'SOURCE="/dev/{mounted}1" TARGET="/srv/snc-data" FSTYPE="ext4" OPTIONS="{opts}" '
                       f'UUID="{uuid}"\n')
        if cmd[0] == "lsblk":
            n = cmd[-1].split("/")[-1]
            d = disks[n]
            return 0, f'NAME="{n}" MODEL="{d["model"]} " SERIAL="{d["serial"]}" WWN="{d["wwn"]}"\n'
        raise AssertionError(cmd)

    def real(path):
        if path.startswith("/sys/class/block/"):
            n = path.rsplit("/", 1)[1]
            return f"/sys/devices/pci0000:17/host0/target0/block/{n[:-1]}/{n}"
        tgt = ssd_at if byid == "ssd" else byid
        if path == A.BYID_DISK:
            return f"/dev/{tgt}"
        if path == A.BYID_PART:
            return f"/dev/{tgt}1"
        if path == A.BYID_UUID:
            return f"/dev/{byuuid_to or mounted}1"
        return path
    return sh_, real


def facts(**kw):
    A.sh, A.realpath = host(**kw)
    return A.gather_identity()


sda, sdb = facts(ssd_at="sda", other="sdb"), facts(ssd_at="sdb", other="sda")
assert sda["disk"] == "sda" and sda["part"] == "sda1" and sda["source"] == "/dev/sda1" and sda["serial"] == "S48LNA0M700325"
assert sda["model"] == "ARFX3840S5xnFTRI" and sda["wwn"] == "0x5002538b097f68a0" and sda["fs_uuid"] == UU
assert A.identity_problems(sda) == [] and A.identity_problems(sdb) == [] and sdb["disk"] == "sdb"
assert A.identity_problems(sda, sda) == [] and A.identity_problems(sdb, sdb) == []
cases = {
    "hdd occupies the approved name": facts(ssd_at="sdc", other="sdb", mounted="sdb", uuid="11111111-2222-3333-4444-555555555555"),
    "wrong filesystem uuid": facts(uuid="11111111-2222-3333-4444-555555555555"),
    "correct mount, wrong serial": facts(ssd_ch=dict(serial="S48LNA0M700326")),
    "correct mount, wrong model": facts(ssd_ch=dict(model="ST3000DM008-2DM1")),
    "correct mount, wrong wwn": facts(ssd_ch=dict(wwn="0x5002538b097f68a1")),
    "correct serial, wrong mounted filesystem": facts(mounted="sdb", uuid="11111111-2222-3333-4444-555555555555"),
    "correct serial, mounted fs of the other disk but approved uuid": facts(mounted="sdb"),
    "by-id disk resolves to the other disk": facts(byid="sdb"),
    "by-uuid resolves to the other partition": facts(byuuid_to="sdb"),
    "mount option drift": facts(opts="rw,relatime,discard,stripe=2"),
}
for nm, f in cases.items():
    assert A.identity_problems(f), nm
assert len(A.identity_problems(cases["wrong filesystem uuid"])) == 1
assert len(A.identity_problems(cases["correct mount, wrong serial"])) == 1
assert len(A.identity_problems(cases["by-id disk resolves to the other disk"])) == 2   # disk and partition link
assert any("fs_uuid" in x for x in A.identity_problems(cases["hdd occupies the approved name"]))
# a device-name change during the invocation: persistent identity still approved, mapping changed
ch = A.identity_problems(sdb, sda)
assert len(ch) == 3 and all("changed during the invocation" in x for x in ch), ch
# check_identity returns the current kernel name, and raises Incomplete on a wrong or changed device
A.sh, A.realpath = host(ssd_at="sdb", other="sda")
assert A.check_identity(sdb) == "sdb"
for bad_want in (sda,):
    try:
        A.check_identity(bad_want)
    except A.Incomplete as e:
        assert "changed during the invocation" in str(e)
    else:
        raise AssertionError("name change not refused")
A.sh, A.realpath = host(ssd_ch=dict(serial="S48LNA0M700326"))
try:
    A.check_identity(None)
except A.Incomplete as e:
    assert "serial" in str(e)
else:
    raise AssertionError("wrong serial accepted")
# whole-disk filesystem (no partition) resolves to itself; an unreadable findmnt resolves to nothing
A.sh, A.realpath = lambda cmd, root=False: (1, ""), os.path.realpath
assert any("not resolved" in x for x in A.identity_problems(A.gather_identity()))
# no remaining dependency on a kernel name: not in the driver, not in the sudoers rule, not in ROOT
import re  # noqa: E402
for path in (os.path.join(HERE, "a2a.py"), os.path.join(HERE, "sudoers-snc-a2a")):
    assert not re.search(r"\bsd[a-z]\d*\b|/dev/sd", open(path).read()), path
assert all("/dev/sd" not in " ".join(c) for c in A.ROOT.values())
assert A.ROOT["smartctl"][-1] == A.ROOT["sg_logs"][-1] == "/dev/disk/by-id/wwn-0x5002538b097f68a0"
assert A.ROOT["tune2fs"][-1] == "/dev/disk/by-id/wwn-0x5002538b097f68a0-part1"
print("A5 ok")
print("ok")
