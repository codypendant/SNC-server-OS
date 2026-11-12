#!/usr/bin/env python3
"""A2-a storage-state reproducibility validation (method validation, SNC only)
(docs/handoff/PR113_Storage_State_Final_Review.md; corrections of
docs/handoff/PR114_A2a_Prereg_Review1.md; PREREG.md here).

K = 5 physical sessions. Each session, fail closed:
  0. per-session environment guard (boot id, HEAD, clean tree, fstrim.timer, NUMA,
     governors, mount and data disk, plan/manifest/binary SHA-256); no rebuild;
  1. clear the data root and sync the filesystem;
  2. A1 pre-trim snapshot (validated);
  3. explicit trim `fstrim -v /srv/snc-data`, proven (rc 0, the mount named, bytes > 0,
     sdb discard sectors increased), then sync;
  4. 30 min idle settle, judged: sdb sectors written + 0 and discarded + 0;
  5. A1 start snapshot (validated);
  6. 12 Profile-A runs, fixed order S0 F1 C1 F2 C2 F3 C3 F4 C4 F5 C5 S1 (S/F: open loop
     at the historical anchor 654,569 ops/s; C: closed-loop capacity); a run with a
     nonzero client exit or an unparsable report stops at once;
  7. A1 end snapshot (validated).
Any execution-integrity failure in 0-7 aborts: INCOMPLETE, no replacement session.
Then the verdict: STABLE, STOP or INCOMPLETE.

bench/pilot.py (frozen at runner commit 4250b32) is imported unchanged: its
real-mode preflight (Pilot.environment, once), run path (Pilot.one) and
sustained() rule. Root steps only via `sudo -n` with the exact commands in ROOT
(sudoers-snc-a2a here).

  a2a.py run OUT --scratch DIR --manifest M [--build build-s6]
  a2a.py judge OUT
"""
import argparse, glob, json, os, re, shutil, statistics, subprocess, sys, time
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.realpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "bench"))
import pilot  # noqa: E402

CP1A = os.path.join(REPO, "bench", "results", "cp1a-r640-20261111", "pilot.json")
PROFILE, PHASE = "A", "a2a"
K = 5
ANCHOR = 654569                     # historical anchor rate only (PR #113 review B1)
SETTLE_S = 1800                     # A2-a: 30 min idle settle after the trim
ORDER = ["S0", "F1", "C1", "F2", "C2", "F3", "C3", "F4", "C4", "F5", "C5", "S1"]
FIXED = [x for x in ORDER if x[0] == "F"]
CAPACITY = [x for x in ORDER if x[0] == "C"]
STOP_CV = 0.10                      # B3: a CV strictly above 10% stops
SENTINEL_LIMIT = Fraction(11, 10)   # PR #114 review 1, item 1: fixed, independent of N_raw
MULTIMODAL_GAP = 2                  # supplementary automatic signal (item 7)
MOUNT, DATA_ROOT, DEV, PART = "/srv/snc-data", "/srv/snc-data/a2a", "sdb", "/dev/sdb1"
MOUNT_FSTYPE, MOUNT_OPTIONS = "ext4", "rw,relatime,stripe=2"   # the accepted configuration
ROOT = {
    "fstrim": ["/usr/sbin/fstrim", "-v", MOUNT],
    "smartctl": ["/usr/sbin/smartctl", "-x", "/dev/sdb"],
    "sg_logs": ["/usr/bin/sg_logs", "-a", "/dev/sdb"],
    "tune2fs": ["/usr/sbin/tune2fs", "-l", PART],
}
sleep = time.sleep                  # injectable for the test
now = time.time


def sh(cmd, root=False):
    """Run one command; root ones only via `sudo -n` and only from ROOT."""
    if root:
        assert cmd in ROOT.values(), cmd
        cmd = ["sudo", "-n"] + cmd
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def readf(path):
    return open(path).read()


def utc():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now()))


def label(k, x):
    return f"k{k}-{x}"


class Incomplete(pilot.Abort):
    """An execution-integrity failure: the validation stops, verdict INCOMPLETE."""


# --- observations ---

def diskstats(text=None):
    """(sectors written, sectors discarded) of DEV from /proc/diskstats."""
    text = text if text is not None else readf("/proc/diskstats")
    for line in text.splitlines():
        f = line.split()
        if len(f) > 17 and f[2] == DEV:
            return int(f[9]), int(f[16])
    raise Incomplete(f"{DEV} not in /proc/diskstats")


def parse_smart(text):
    """The A1 fields of `smartctl -x` on this SAS SSD (None where absent)."""
    def g(rx, cast=str):
        m = re.search(rx, text, re.M)
        return cast(m[1].replace(",", "")) if m else None
    return dict(health=g(r"SMART Health Status:\s*(\S+)"),
                temperature_c=g(r"Current temperature\s*=\s*(\d+)", int),
                bms_status=g(r"^\s*Status:\s*(.+?)\s*$"),
                bms_progress_pct=g(r"scan progress:\s*([\d.]+)%", float),
                bms_scans=g(r"Number of background scans performed:\s*([\d,]+)", int),
                endurance_used_pct=g(r"Percentage used endurance indicator:\s*(\d+)%", int),
                grown_defects=g(r"Elements in grown defect list:\s*(\d+)", int),
                non_medium_errors=g(r"Non-medium error count:\s*(\d+)", int),
                write_gb_processed=g(r"^write:(?:\s+\S+){5}\s+([\d.]+)", float),
                uncorrected_write=g(r"^write:(?:\s+\S+){6}\s+(\d+)", int),
                uncorrected_read=g(r"^read:(?:\s+\S+){6}\s+(\d+)", int))


SMART_REQUIRED = ("health", "temperature_c", "bms_status", "endurance_used_pct", "grown_defects",
                  "non_medium_errors", "uncorrected_write", "uncorrected_read")


def a1_problems(rec):
    """Why one A1 snapshot is not usable ([] = usable). smartctl's exit status is a
    bitmask that can be nonzero with full data on this SAS device, so its parsed fields
    are validated instead; sg_logs and tune2fs must exit 0 with their content."""
    p = []
    if rec.get("sectors_written") is None or rec.get("sectors_discarded") is None:
        p.append("diskstats")
    if not re.fullmatch(r"[0-9a-f-]{36}", rec.get("boot_id") or ""):
        p.append("boot id")
    if rec.get("uptime_s") is None:
        p.append("uptime")
    sm = rec.get("smart") or {}
    p += [f"smart {k}" for k in SMART_REQUIRED if sm.get(k) is None]
    if sm.get("bms_status") == "scan is active" and sm.get("bms_progress_pct") is None:
        p.append("smart background-scan progress")
    if rec.get("sg_logs_rc") != 0 or not rec.get("sg_logs_has_bsr"):
        p.append("sg_logs")
    if rec.get("tune2fs_rc") != 0 or not rec.get("fs_lifetime_writes"):
        p.append("tune2fs lifetime writes")
    return p


def snapshot(out, tag, since=None):
    """A1 (read only): diskstats, boot id, uptime, SMART/SCSI health, temperature,
    background scan, endurance, defects/errors, sg_logs, fs lifetime writes, last trim;
    and the host units started since `since` (report only). Raises Incomplete if unusable."""
    d = os.path.join(out, "state", tag)
    os.makedirs(d, exist_ok=True)
    rec = dict(tag=tag, utc=utc())
    try:
        ds = readf("/proc/diskstats")
        open(os.path.join(d, "diskstats.txt"), "w").write(ds)
        rec["sectors_written"], rec["sectors_discarded"] = diskstats(ds)
    except (OSError, pilot.Abort):
        pass
    for name, path in (("boot_id", "/proc/sys/kernel/random/boot_id"), ("uptime", "/proc/uptime")):
        try:
            txt = readf(path)
            open(os.path.join(d, name + ".txt"), "w").write(txt)
            rec[name] = txt.strip()
        except OSError:
            rec[name] = None
    try:
        rec["uptime_s"] = float((rec.get("uptime") or "").split()[0])
    except (IndexError, ValueError):
        rec["uptime_s"] = None
    for name in ("smartctl", "sg_logs", "tune2fs"):
        rc, txt = sh(ROOT[name], root=True)
        open(os.path.join(d, name + ".txt"), "w").write(txt + f"\n# rc={rc}\n")
        rec[name + "_rc"] = rc
        if name == "smartctl":
            rec["smart"] = parse_smart(txt)
        elif name == "sg_logs":
            rec["sg_logs_has_bsr"] = "Background scan results page" in txt
        else:
            m = re.search(r"Lifetime writes:\s*(.+)", txt)
            rec["fs_lifetime_writes"] = m[1].strip() if m else None
    for name, cmd in (("fstrim_journal", ["journalctl", "-u", "fstrim.service", "-n", "20", "--no-pager",
                                          "-o", "short-iso-precise"]),):
        rc, txt = sh(cmd)
        open(os.path.join(d, name + ".txt"), "w").write(txt)
    if since is not None:
        rc, txt = sh(["journalctl", "--since", f"@{int(since)}", "--no-pager", "-o", "short-iso", "_PID=1"])
        open(os.path.join(d, "units_started.txt"), "w").write(txt)
        rec["units_started"] = sorted(set(re.findall(r"Starting (\S+?\.service)", txt)))
    rec["problems"] = a1_problems(rec)
    json.dump(rec, open(os.path.join(d, "snapshot.json"), "w"), indent=1)
    if rec["problems"]:
        raise Incomplete(f"A1 snapshot {tag} unusable: {', '.join(rec['problems'])}")
    return rec


def observe(build):
    """The per-session environment, read directly (no rebuild, no pilot initialization)."""
    o = {}
    try:
        o["boot_id"] = readf("/proc/sys/kernel/random/boot_id").strip()
    except OSError:
        o["boot_id"] = None
    o["head"] = pilot.git_in(REPO, "rev-parse", "HEAD").stdout.strip()
    o["git_status"] = pilot.git_in(REPO, "status", "--porcelain").stdout.strip()
    o["timer_enabled"] = sh(["systemctl", "is-enabled", "fstrim.timer"])[1].strip()
    o["timer_active"] = sh(["systemctl", "is-active", "fstrim.timer"])[1].strip()
    try:
        o["numa_balancing"] = readf("/proc/sys/kernel/numa_balancing").strip()
    except OSError:
        o["numa_balancing"] = None
    govs = [readf(f).strip() for f in sorted(glob.glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"))]
    o["governors"] = sorted(set(govs)) if govs else None
    o["mount"] = sh(["findmnt", "-no", "SOURCE,TARGET,FSTYPE,OPTIONS", MOUNT])[1].split()
    o["data_root_mount"] = sh(["findmnt", "-no", "TARGET", "-T", os.path.dirname(DATA_ROOT)])[1].strip()
    o["data_disks"] = sorted(pilot.disks_of(MOUNT) or [])
    for k, path in (("plan_sha256", os.path.join(REPO, "bench", "pilot_plan.json")),
                    ("manifest_sha256", os.path.join(REPO, "bench", "manifests", "r640-main-20261111.txt")),
                    ("snc_server_sha256", f"{build}/snc_server"), ("net_bench_sha256", f"{build}/net_bench")):
        o[k] = pilot.sha(path) if os.path.exists(path) else None
    return o


def guard_problems(o, want):
    """Item 2: every reason the environment drifted from the invocation's start ([] = ok)."""
    p = []
    for k in ("boot_id", "head"):
        if o.get(k) != want[k]:
            p.append(f"{k} {o.get(k)} != {want[k]}")
    if o.get("git_status"):
        p.append("git tree not clean")
    if o.get("timer_enabled") != "disabled" or o.get("timer_active") != "inactive":
        p.append(f"fstrim.timer {o.get('timer_enabled')}/{o.get('timer_active')}, not disabled/inactive")
    if o.get("numa_balancing") != "0":
        p.append(f"numa_balancing {o.get('numa_balancing')}")
    if o.get("governors") != ["performance"]:
        p.append(f"governors {o.get('governors')}")
    if o.get("mount") != [PART, MOUNT, MOUNT_FSTYPE, MOUNT_OPTIONS]:
        p.append(f"mount {o.get('mount')} != {[PART, MOUNT, MOUNT_FSTYPE, MOUNT_OPTIONS]}")
    if o.get("data_root_mount") != MOUNT:
        p.append(f"data root on {o.get('data_root_mount')!r}, not {MOUNT}")
    if o.get("data_disks") != [DEV]:
        p.append(f"data disks {o.get('data_disks')} != [{DEV!r}]")
    for k in ("plan_sha256", "manifest_sha256", "snc_server_sha256", "net_bench_sha256"):
        if o.get(k) != want[k]:
            p.append(f"{k} {o.get(k)} != frozen {want[k]}")
    return p


TRIM_RX = re.compile(r"^" + re.escape(MOUNT) + r": .*\((\d+) bytes\) trimmed", re.M)


def trim_problems(t):
    """Item 3: was the explicit trim proven? ([] = proven)."""
    p = []
    if t["rc"] != 0:
        p.append(f"fstrim rc {t['rc']}")
    if t["bytes"] is None:
        p.append(f"fstrim output does not report {MOUNT}")
    elif t["bytes"] <= 0:
        p.append("fstrim reported 0 bytes")
    if t["discard_sectors_delta"] <= 0:
        p.append(f"{DEV} discard sectors +{t['discard_sectors_delta']}")
    return p


def settle_problems(s):
    """Item 4: was the settle clean on the data disk? ([] = clean)."""
    p = []
    if s["sectors_written"] != 0:
        p.append(f"{DEV} wrote {s['sectors_written']} sectors during the settle")
    if s["sectors_discarded"] != 0:
        p.append(f"{DEV} discarded {s['sectors_discarded']} sectors during the settle")
    return p


def run_problems(r):
    """An execution-integrity failure of one run (any kind)."""
    p = []
    if r.get("returncode") != 0:
        p.append(f"client exit {r.get('returncode')}")
    if r.get("failed"):
        p.append(r.get("why") or "failed")
    return p


def capacity_problems(r):
    """Item 5: a capacity rep's validity; EXEC overload is expected in closed loop."""
    p = run_problems(r)
    for k in ("errors", "stale", "index_errors"):
        if r.get(k):
            p.append(f"{k} {r.get(k)}")
    return p


# --- preflight additions ---

def extra_preflight(p, ref):
    """Refusals on top of pilot's real-mode preflight (the CP-1A freeze gate etc.)."""
    bad = [f"{k} {p.env[k]} != CP-1A {ref[k]}" for k in
           ("snc_server_sha256", "net_bench_sha256", "plan_sha256", "manifest_sha256") if p.env[k] != ref[k]]
    for name, cmd in ROOT.items():
        if sh(["sudo", "-n", "-l"] + cmd)[0] != 0:
            bad.append(f"sudo -n cannot run {' '.join(cmd)} (install sudoers-snc-a2a)")
    if p.env["disks"].get("data") != [DEV]:
        bad.append(f"data root disks {p.env['disks'].get('data')} != [{DEV!r}]")
    return bad


def cp1a_record(path=CP1A):
    d = json.load(open(path))
    a = d["results"]["A"]
    assert a["sweep"]["objective_rate"] == ANCHOR == a["fixed"]["rate"], "anchor is not the CP-1A objective rate"
    e = d["environment"]
    return {k: e[k] for k in ("snc_server_sha256", "net_bench_sha256", "plan_sha256", "manifest_sha256")}


# --- the sessions ---

def session(p, plan, k, want, rec):
    """One A2-a session; fills `rec` as it goes; raises Incomplete on any integrity failure."""
    out, win = p.a.out, plan["profiles"][PROFILE]["window_s"]
    t0 = now()
    o = observe(p.a.build)
    rec["guard"] = dict(utc=utc(), observed=o, problems=guard_problems(o, want))
    if rec["guard"]["problems"]:
        raise Incomplete(f"session {k} guard: {'; '.join(rec['guard']['problems'])}")
    shutil.rmtree(DATA_ROOT, ignore_errors=True)
    sh(["sync", "-f", MOUNT])
    rec["pre_trim"] = snapshot(out, f"k{k}-0-pre-trim")
    w0, d0 = diskstats()
    rc, txt = sh(ROOT["fstrim"], root=True)
    w1, d1 = diskstats()
    m = TRIM_RX.search(txt)
    t = dict(utc=utc(), rc=rc, output=txt.strip(), bytes=int(m[1]) if m else None,
             discard_sectors_before=d0, discard_sectors_after=d1, discard_sectors_delta=d1 - d0,
             sectors_written_delta=w1 - w0)
    t["problems"] = trim_problems(t)
    rec["trim"] = t
    open(os.path.join(out, "state", f"k{k}-0-pre-trim", "fstrim-run.txt"), "w").write(json.dumps(t, indent=1))
    if t["problems"]:
        raise Incomplete(f"session {k} trim not proven: {'; '.join(t['problems'])}")
    sh(["sync", "-f", MOUNT])
    w0, d0 = diskstats()
    sleep(SETTLE_S)
    w1, d1 = diskstats()
    s = dict(seconds=SETTLE_S, sectors_written=w1 - w0, sectors_discarded=d1 - d0)
    s["problems"] = settle_problems(s)
    rec["settle"] = s
    if s["problems"]:
        raise Incomplete(f"session {k} settle not clean: {'; '.join(s['problems'])}")
    rec["start"] = snapshot(out, f"k{k}-1-start", since=t0)
    for x in ORDER:
        if x[0] in "SF":
            r = p.one(PROFILE, PHASE, label(k, x), rate=ANCHOR, depth=plan["rate_depth"],
                      window=(plan["warmup_s"], win), memory=True)
        else:
            r = p.one(PROFILE, PHASE, label(k, x), depth=plan["calibration_depth"], window=win)
        r.update(session=k, slot=x)
        p.write()
        bad = run_problems(r)
        if bad:
            raise Incomplete(f"{label(k, x)}: {'; '.join(bad)}")
    rec["end"] = snapshot(out, f"k{k}-2-end", since=t0)
    rec["complete"] = True


def execute(p, plan, want):
    res = p.results.setdefault("A2a_validation", dict(sessions=[]))
    try:
        for k in range(1, K + 1):
            rec = dict(k=k, complete=False)
            res["sessions"].append(rec)
            session(p, plan, k, want, rec)
            p.write()
    except pilot.Abort as e:            # Incomplete, or pilot's own abort (server exit, failed load)
        res["aborted"] = str(e)
        print(f"ABORTED: {e}", file=sys.stderr)
    res.update(judge(p.runs, plan, res["sessions"], res.get("aborted")))
    p.write()
    return res


# --- the verdict (PREREG.md "Decision") ---

def judge(runs, plan, sessions, aborted=None):
    """INCOMPLETE: an abort, a session not complete, a missing run, or a run that failed to
    execute. STOP: a completed experiment that breaks a statistical/stationarity rule.
    STABLE: otherwise."""
    by = {(r.get("session"), r.get("slot")): r for r in runs if r.get("phase") == PHASE}
    v = dict(anchor_rate=ANCHOR, session_stats={}, why=[])
    inc = []
    if aborted:
        inc.append(f"aborted: {aborted}")
    if len(sessions) != K or not all(s.get("complete") for s in sessions):
        inc.append(f"{sum(bool(s.get('complete')) for s in sessions)} of {K} sessions complete")
    for s in sessions:
        for part in ("guard", "trim", "settle", "pre_trim", "start", "end"):
            if s.get(part, {}).get("problems"):
                inc.append(f"session {s['k']} {part}: {'; '.join(s[part]['problems'])}")
    missing = [label(k, x) for k in range(1, K + 1) for x in ORDER if (k, x) not in by]
    if missing:
        inc.append(f"missing runs: {', '.join(missing[:6])}" + (" ..." if len(missing) > 6 else ""))
    for (k, x), r in sorted(by.items()):
        if run_problems(r):
            inc.append(f"{label(k, x)}: {'; '.join(run_problems(r))}")
    if inc:
        v.update(verdict="INCOMPLETE", why=inc + ["stop; no replacement session"])
        return v
    why = v["why"]
    for k in range(1, K + 1):
        for x in FIXED + ["S0", "S1"]:
            ok, rs = pilot.sustained(by[k, x], plan)
            if not ok:
                why.append(f"{label(k, x)}: anchor rep unsustained ({'; '.join(rs)})")
        for x in CAPACITY:
            if capacity_problems(by[k, x]):
                why.append(f"{label(k, x)}: capacity rep invalid ({'; '.join(capacity_problems(by[k, x]))})")
    if why:                                   # no CV from an unusable session
        v.update(verdict="STOP")
        return v
    s = v["session_stats"]
    for k in range(1, K + 1):
        p99 = [by[k, x]["p99_ns"] for x in FIXED]
        thr = [by[k, x]["total_ops_s"] for x in CAPACITY]
        s[k] = dict(median_p99_ns=statistics.median(p99), cv_p99=pilot.cv(p99), range_p99_ns=max(p99) - min(p99),
                    median_thr=statistics.median(thr), cv_thr=pilot.cv(thr), range_thr=max(thr) - min(thr),
                    sentinel_p99_ns=(by[k, "S0"]["p99_ns"], by[k, "S1"]["p99_ns"]))
    med_p99 = [s[k]["median_p99_ns"] for k in s]
    med_thr = [s[k]["median_thr"] for k in s]
    cvs = dict(cv_p99_between=pilot.cv(med_p99), cv_thr_between=pilot.cv(med_thr),
               max_cv_p99_within=max(s[k]["cv_p99"] for k in s), max_cv_thr_within=max(s[k]["cv_thr"] for k in s))
    v.update(cvs)
    v["N_raw"] = 3 * max(cvs.values())        # a result (B2); it judges nothing here
    for name, c in cvs.items():
        if c > STOP_CV:
            why.append(f"{name} {c:.4f} > {STOP_CV}")
    for name in ("cv_p99", "cv_thr"):
        for k in s:
            if s[k][name] > STOP_CV:
                why.append(f"session {k} {name} {s[k][name]:.4f} > {STOP_CV}")
    # Multimodality, supplementary automatic signal (B3; review 1 item 7): one adjacent gap of the
    # sorted session medians above MULTIMODAL_GAP x the widest within-session range of that metric.
    for key, med, rng in (("p99", med_p99, "range_p99_ns"), ("thr", med_thr, "range_thr")):
        srt = sorted(med)
        gap = max(b - a for a, b in zip(srt, srt[1:]))
        widest = max(s[k][rng] for k in s)
        flag = gap > MULTIMODAL_GAP * widest
        v[f"multimodal_{key}"] = dict(max_gap=gap, widest_within_range=widest, flag=flag)
        if flag:
            why.append(f"multimodal {key}: session-median gap {gap} > {MULTIMODAL_GAP} x widest within-session "
                       f"range {widest}")
    for k in s:                               # B4 with the fixed limit (review 1 item 1)
        a, b = s[k]["sentinel_p99_ns"]
        ratio = max(Fraction(b, a), Fraction(a, b))
        s[k]["sentinel_ratio"] = float(ratio)
        s[k]["sentinel_ok"] = ratio <= SENTINEL_LIMIT
        if not s[k]["sentinel_ok"]:
            why.append(f"session {k} sentinels {a} -> {b}: ratio {float(ratio):.4f} > {float(SENTINEL_LIMIT)}")
    v["anchor_median_of_session_medians_p99_ns"] = statistics.median(med_p99)   # B1: historical anchor only
    v["session_stats"] = {str(k): x for k, x in s.items()}
    v["verdict"] = "STOP" if why else "STABLE"
    return v


def run(a):
    plan = json.load(open(a.plan))
    ref = cp1a_record()
    a.quick, a.allocation, a.data_root, a.scratch_given = False, plan["baseline_allocation"], DATA_ROOT, True
    os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
    p = pilot.Pilot(a, plan)                  # CP-1A real-mode preflight, once (it rebuilds; no rebuild after)
    bad = extra_preflight(p, ref)
    o = observe(a.build)
    want = dict(ref, boot_id=o["boot_id"], head=o["head"])
    bad += guard_problems(o, want)
    if bad:
        raise SystemExit("refusing: " + "; ".join(bad))
    p.env.update(validation="a2a-storage-repro", cp1a_identity=ref, guard_reference=want, K=K, order=ORDER,
                 anchor_rate=ANCHOR, settle_s=SETTLE_S, sentinel_limit=str(SENTINEL_LIMIT), root_commands=ROOT)
    p.write()
    try:
        res = execute(p, plan, want)
    finally:
        shutil.rmtree(DATA_ROOT, ignore_errors=True)
        try:
            os.rmdir(a.scratch)
        except OSError:
            pass
    print(f"VERDICT {res['verdict']}: " + ("; ".join(res["why"]) or "A2-a reproducible under the preregistered rules"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("out")
    r.add_argument("--scratch", required=True)
    r.add_argument("--manifest", required=True)
    r.add_argument("--build", default="build-s6")
    r.add_argument("--plan", default="bench/pilot_plan.json")
    j = sub.add_parser("judge")
    j.add_argument("out")
    a = ap.parse_args()
    if a.cmd == "run":
        return run(a)
    d = json.load(open(os.path.join(a.out, "pilot.json")))
    res = d["results"]["A2a_validation"]
    v = judge(d["runs"], d["environment"]["plan_values"], res["sessions"], res.get("aborted"))
    print(json.dumps({k: v[k] for k in ("verdict", "why", "N_raw") if k in v}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
