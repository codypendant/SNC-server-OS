#!/usr/bin/env python3
"""A2-a storage-state reproducibility validation (method validation, SNC only)
(docs/handoff/PR113_Storage_State_Final_Review.md; PREREG.md here).

K = 5 physical sessions. Each session:
  1. explicit, logged trim of /srv/snc-data (`fstrim -v`);
  2. 30 min idle settle;
  3. A1 start snapshot (read only);
  4. 12 Profile-A runs in a fixed order: start sentinel, 5 fixed open-loop reps at the
     historical anchor 654,569 ops/s interleaved with 5 closed-loop capacity reps, end sentinel;
  5. A1 end snapshot.
Then the preregistered verdict: STABLE, STOP or INCOMPLETE (B1-B4 of the review).

bench/pilot.py (frozen at runner commit 4250b32) is imported unchanged: its
real-mode preflight (Pilot.environment), run path (Pilot.one) and sustained()
rule. Root steps go only through `sudo -n` with the exact commands in ROOT
(the sudoers file `sudoers-snc-a2a` here).

  a2a.py run OUT --scratch DIR --manifest M [--build build-s6]
  a2a.py judge OUT
"""
import argparse, json, os, re, shutil, statistics, subprocess, sys, time
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.realpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "bench"))
import pilot  # noqa: E402

CP1A = os.path.join(REPO, "bench", "results", "cp1a-r640-20261111", "pilot.json")
PROFILE, PHASE = "A", "a2a"
K = 5
ANCHOR = 654569                     # historical anchor rate only (review B1): not the campaign objective
SETTLE_S = 1800                     # A2-a: 30 min idle settle after the trim
ORDER = ["S0", "F1", "C1", "F2", "C2", "F3", "C3", "F4", "C4", "F5", "C5", "S1"]
FIXED = [x for x in ORDER if x[0] == "F"]
CAPACITY = [x for x in ORDER if x[0] == "C"]
STOP_CV = 0.10                      # B3: a CV strictly above 10% stops
MULTIMODAL_GAP = 2                  # B3 operationalized: see judge()
MOUNT, DATA_ROOT, DEV, PART = "/srv/snc-data", "/srv/snc-data/a2a", "sdb", "/dev/sdb1"
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


def label(k, x):
    return f"k{k}-{x}"


# --- A1 snapshots (read only) ---

def diskstats(text=None):
    """(sectors written, sectors discarded) of DEV from /proc/diskstats."""
    text = text if text is not None else open("/proc/diskstats").read()
    for line in text.splitlines():
        f = line.split()
        if len(f) > 17 and f[2] == DEV:
            return int(f[9]), int(f[16])
    raise RuntimeError(f"{DEV} not in /proc/diskstats")


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


def snapshot(out, tag, since=None):
    """A1: diskstats, SMART/SCSI health, temperature, background scan, last trim,
    fs lifetime writes, uptime, boot id; and the host units started since `since`
    (report only). Files under out/state/<tag>/; parsed fields returned."""
    d = os.path.join(out, "state", tag)
    os.makedirs(d, exist_ok=True)
    rec = dict(tag=tag, utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now())))
    ds = open("/proc/diskstats").read()
    open(os.path.join(d, "diskstats.txt"), "w").write(ds)
    rec["sectors_written"], rec["sectors_discarded"] = diskstats(ds)
    for name in ("smartctl", "sg_logs", "tune2fs"):
        rc, txt = sh(ROOT[name], root=True)
        open(os.path.join(d, name + ".txt"), "w").write(txt + f"\n# rc={rc}\n")
        rec[name + "_rc"] = rc
        if name == "smartctl":
            rec["smart"] = parse_smart(txt)
        if name == "tune2fs":
            m = re.search(r"Lifetime writes:\s*(.+)", txt)
            rec["fs_lifetime_writes"] = m[1].strip() if m else None
    for name, cmd in (("uptime", ["cat", "/proc/uptime"]),
                      ("boot_id", ["cat", "/proc/sys/kernel/random/boot_id"]),
                      ("fstrim_journal", ["journalctl", "-u", "fstrim.service", "-n", "20", "--no-pager",
                                          "-o", "short-iso-precise"]),
                      ("fstrim_timer", ["systemctl", "is-enabled", "fstrim.timer"])):
        rc, txt = sh(cmd)
        open(os.path.join(d, name + ".txt"), "w").write(txt)
        rec[name] = txt.strip().splitlines()[-1] if txt.strip() else ""
    if since is not None:
        rc, txt = sh(["journalctl", "--since", f"@{int(since)}", "--no-pager", "-o", "short-iso", "_PID=1"])
        open(os.path.join(d, "units_started.txt"), "w").write(txt)
        rec["units_started"] = sorted(set(re.findall(r"Starting (\S+?\.service)", txt)))
    return rec


# --- preflight additions ---

def extra_preflight(p, ref):
    """Refusals on top of pilot's real-mode preflight (the CP-1A freeze gate etc.)."""
    bad = [f"{k} {p.env[k]} != CP-1A {ref[k]}" for k in
           ("snc_server_sha256", "net_bench_sha256", "plan_sha256", "manifest_sha256") if p.env[k] != ref[k]]
    rc, en = sh(["systemctl", "is-enabled", "fstrim.timer"])
    rc2, act = sh(["systemctl", "is-active", "fstrim.timer"])
    if en.strip() != "disabled" or act.strip() != "inactive":
        bad.append(f"fstrim.timer is {en.strip()}/{act.strip()}, not disabled/inactive (A0)")
    for name, cmd in ROOT.items():
        if sh(["sudo", "-n", "-l"] + cmd)[0] != 0:
            bad.append(f"sudo -n cannot run {' '.join(cmd)} (install sudoers-snc-a2a)")
    rc, tgt = sh(["findmnt", "-no", "TARGET", "-T", DATA_ROOT])
    if tgt.strip() != MOUNT:
        bad.append(f"{DATA_ROOT} is on {tgt.strip()!r}, not {MOUNT}")
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

def session(p, plan, k):
    """One A2-a session: trim, settle, A1 start, 12 runs, A1 end. Returns its record."""
    out, win = p.a.out, plan["profiles"][PROFILE]["window_s"]
    rec = dict(k=k)
    shutil.rmtree(DATA_ROOT, ignore_errors=True)       # nothing of ours on the fs at trim time
    t0 = now()
    rec["pre_trim"] = snapshot(out, f"k{k}-0-pre-trim")
    rc, txt = sh(ROOT["fstrim"], root=True)
    open(os.path.join(out, "state", f"k{k}-0-pre-trim", "fstrim-run.txt"), "w").write(txt + f"\n# rc={rc}\n")
    rec["trim"] = dict(utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now())), rc=rc, output=txt.strip())
    if rc != 0:
        raise pilot.Abort(f"session {k}: fstrim failed (rc {rc}): {txt.strip()}")
    w0, d0 = diskstats()
    sleep(SETTLE_S)
    w1, d1 = diskstats()
    rec["settle"] = dict(seconds=SETTLE_S, sectors_written=w1 - w0, sectors_discarded=d1 - d0)
    rec["start"] = snapshot(out, f"k{k}-1-start", since=t0)
    for x in ORDER:
        if x[0] in "SF":
            r = p.one(PROFILE, PHASE, label(k, x), rate=ANCHOR, depth=plan["rate_depth"],
                      window=(plan["warmup_s"], win), memory=True)
        else:
            r = p.one(PROFILE, PHASE, label(k, x), depth=plan["calibration_depth"], window=win)
        r.update(session=k, slot=x)
        p.write()
    rec["end"] = snapshot(out, f"k{k}-2-end", since=t0)
    return rec


def execute(p, plan):
    res = p.results.setdefault("A2a_validation", dict(sessions=[]))
    try:
        for k in range(1, K + 1):
            res["sessions"].append(session(p, plan, k))
            p.write()
    except pilot.Abort as e:
        res["aborted"] = str(e)
        print(f"ABORTED: {e}", file=sys.stderr)
    res.update(judge(p.runs, plan))
    p.write()
    return res


# --- the verdict (PREREG.md "Decision") ---

def judge(runs, plan):
    """STABLE: complete, every anchor rep (fixed and sentinel) sustained, every capacity rep
    error-free, no CV above 10% (between or any within), no multimodal flag, and every
    session's sentinels within 1 + N_raw. STOP: anything else. INCOMPLETE: a missing run."""
    by = {(r.get("session"), r.get("slot")): r for r in runs if r.get("phase") == PHASE}
    v = dict(anchor_rate=ANCHOR, session_stats={}, why=[])
    if any((k, x) not in by for k in range(1, K + 1) for x in ORDER):
        missing = [label(k, x) for k in range(1, K + 1) for x in ORDER if (k, x) not in by]
        v.update(verdict="INCOMPLETE", why=[f"missing runs: {', '.join(missing[:6])}"
                                            + (" ..." if len(missing) > 6 else "") + "; stop and review"])
        return v
    why = v["why"]
    for k in range(1, K + 1):
        for x in FIXED + ["S0", "S1"]:
            ok, rs = pilot.sustained(by[k, x], plan)
            if not ok:
                why.append(f"{label(k, x)}: anchor rep unsustained ({'; '.join(rs)})")
        for x in CAPACITY:
            r = by[k, x]
            if r.get("failed") or r.get("errors"):
                why.append(f"{label(k, x)}: capacity rep failed or had errors")
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
    v["N_raw"] = 3 * max(cvs.values())
    for name, c in cvs.items():
        if c > STOP_CV:
            why.append(f"{name} {c:.4f} > {STOP_CV}")
    for name, k_ in (("cv_p99", "p99"), ("cv_thr", "thr")):
        for k in s:
            if s[k][name] > STOP_CV:
                why.append(f"session {k} {name} {s[k][name]:.4f} > {STOP_CV}")
    # Multimodality, operationalized (B3): the sorted session medians split into separated groups
    # when one adjacent gap exceeds MULTIMODAL_GAP x the widest within-session range of that metric.
    for key, med in (("p99", med_p99), ("thr", med_thr)):
        srt = sorted(med)
        gap = max(b - a for a, b in zip(srt, srt[1:]))
        widest = max(s[k][f"range_{'p99_ns' if key == 'p99' else 'thr'}"] for k in s)
        v[f"multimodal_{key}"] = dict(max_gap=gap, widest_within_range=widest, flag=gap > MULTIMODAL_GAP * widest)
        if gap > MULTIMODAL_GAP * widest:
            why.append(f"multimodal {key}: session-median gap {gap} > {MULTIMODAL_GAP} x widest within-session "
                       f"range {widest}")
    n = Fraction(v["N_raw"])
    for k in s:
        a, b = s[k]["sentinel_p99_ns"]
        ratio = max(Fraction(b, a), Fraction(a, b))
        s[k]["sentinel_ratio"] = float(ratio)
        s[k]["sentinel_ok"] = ratio <= 1 + n
        if not s[k]["sentinel_ok"]:
            why.append(f"session {k} sentinels {a} -> {b}: ratio {float(ratio):.4f} > 1 + N_raw {1 + float(n):.4f}")
    v["anchor_median_of_session_medians_p99_ns"] = statistics.median(med_p99)   # B1: historical anchor only
    v["session_stats"] = {str(k): x for k, x in s.items()}
    v["verdict"] = "STOP" if why else "STABLE"
    return v


def run(a):
    plan = json.load(open(a.plan))
    ref = cp1a_record()
    a.quick, a.allocation, a.data_root, a.scratch_given = False, plan["baseline_allocation"], DATA_ROOT, True
    os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
    p = pilot.Pilot(a, plan)                  # CP-1A real-mode preflight, unchanged
    bad = extra_preflight(p, ref)
    if bad:
        raise SystemExit("refusing: " + "; ".join(bad))
    p.env.update(validation="a2a-storage-repro", cp1a_identity=ref, K=K, order=ORDER, anchor_rate=ANCHOR,
                 settle_s=SETTLE_S, root_commands=ROOT)
    p.write()
    try:
        res = execute(p, plan)
    finally:
        shutil.rmtree(DATA_ROOT, ignore_errors=True)
        try:
            os.rmdir(a.scratch)
        except OSError:
            pass
    print(f"VERDICT {res['verdict']}: " + ("; ".join(res["why"]) or "A2-a reproducible under B1-B4"))
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
    v = judge(d["runs"], d["environment"]["plan_values"])
    print(json.dumps({k: v[k] for k in ("verdict", "why", "N_raw") if k in v}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
