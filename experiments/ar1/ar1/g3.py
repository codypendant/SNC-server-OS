"""AR-1 G3 counted campaign (prereg revision 3): python -m ar1.g3 <results dir>.

Runs as the non-root runner on the dedicated VM, from a clean checkout of the frozen commit. Phases, each with its
section 10 cap: Phase 0 (hard gate), checker adequacy on the frozen code, SNC arm, plain step 1, plain step 2, performance, determinism divergence. A
Phase 0 failure, or any UNRESOLVED run, stops the campaign (section 9). Resumable: finished rows are kept.
"""

import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

from . import pg
from check.score import score

from .batch import ADEQUACY, code_hash
from .harness import HERE, PY
from .matrix import points

REPO = HERE.parent.parent
SEEDS = int(os.environ.get("AR1_SEEDS", "20"))
TASKS = int(os.environ.get("AR1_PERF_TASKS", "1000"))
CHECKS = [(c, SEEDS) for c in ("OI-1a", "OI-1a-control", "OI-1a-v", "OI-1b", "OI-1c", "OI-1d", "C-7", "C-7b")]
CAP_H = {"phase0": 8, "adequacy": 6, "snc": 16, "plain1": 16, "plain2": 16, "perf": 4}
WORKERS = int(os.environ.get("AR1_WORKERS", "2"))


def sh(*a, **kw):
    return subprocess.run(a, capture_output=True, text=True, **kw).stdout.strip()


def freeze():
    if sh("git", "-C", str(REPO), "status", "--porcelain", "--", "experiments/ar1"):
        sys.exit("experiments/ar1 has uncommitted changes: counted runs need the frozen tree (prereg 5.5)")
    head = sh("git", "-C", str(REPO), "rev-parse", "HEAD")
    want = os.environ.get("AR1_FREEZE_EXPECT")
    if not want or not head.startswith(want):
        sys.exit(f"HEAD {head} is not the frozen commit {want}")
    trees = {
        name: sh("git", "-C", str(REPO), "rev-parse", f"HEAD:experiments/ar1/{d}")
        for name, d in (
            ("snc_rt", "ar1"),
            ("ar1_check", "check"),
            ("plain_dbos_step1", "plain1"),
            ("plain_dbos_step2", "plain2"),
        )
    }
    return {"commit": head, "trees": trees, "code_hash": code_hash()}


def manifest(out, fz):
    pip = sh(PY, "-m", "pip", "freeze")
    pgv = sh(str(Path(os.environ.get("AR1_PGBIN", "/opt/ar1/bin")) / "postgres"), "--version")
    runs = os.environ.get("AR1_RUNS", "/tmp/ar1-runs")
    text = "\n".join(
        [
            f"# AR-1 G3 manifest, {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}",
            f"host: {platform.node()}",
            f"kernel: {platform.release()}",
            f"freeze: {json.dumps(fz)}",
            f"workers: {WORKERS}, seeds: {SEEDS}, perf tasks: {TASKS}",
            "## cpu",
            sh("lscpu"),
            "## memory",
            sh("free", "-b"),
            "## runs filesystem",
            sh("df", "-T", runs),
            sh("findmnt", "-T", runs),
            "## postgres",
            pgv,
            "## postgresql.conf additions (ar1/pg.py)",
            pg.PRIMARY_CONF,
            "## python",
            sys.version,
            pip,
        ]
    )
    (out / "manifest.txt").write_text(text + "\n")


def capped(cmd, end, env=None):
    try:
        return subprocess.run(
            cmd, cwd=HERE, env={**os.environ, **(env or {})}, timeout=max(1, end - time.time())
        ).returncode
    except subprocess.TimeoutExpired:
        return "cap"


def phase0(out, end):
    d = out / "phase0"
    for name, n in CHECKS:
        if not (d / f"{name}.json").exists() and capped([PY, "-m", "ar1.phase0", name, str(n), str(d)], end) == "cap":
            return {"gate": "UNRESOLVED", "reason": f"Phase 0 cap hit during {name}"}
    e1 = d / "OI-1e.json"
    if not (e1.exists() and e1.read_text().strip()):
        work = Path(os.environ.get("AR1_RUNS", "/tmp/ar1-runs")) / "oi1e"
        try:
            with open(e1, "w") as f:
                subprocess.run(
                    [PY, "-m", "check.oi1e", str(work), str(SEEDS), "1000" if SEEDS == 20 else "40"],
                    cwd=HERE,
                    stdout=f,
                    timeout=max(1, end - time.time()),
                )
        except subprocess.TimeoutExpired:
            return {"gate": "UNRESOLVED", "reason": "Phase 0 cap hit during OI-1e"}
    fails, unexplained = [], []
    for name, n in CHECKS:
        rs = json.loads((d / f"{name}.json").read_text()) if (d / f"{name}.json").exists() else []
        if len(rs) < n or any(r.get("error") for r in rs):
            unexplained.append(name)
        elif not all(r.get("pass") for r in rs):
            fails.append(name)
    try:
        e = json.loads(e1.read_text())
    except ValueError:
        e = {}
    if e.get("overall") != "pass":
        unexplained.append("OI-1e")
    if unexplained:
        return {"gate": "UNRESOLVED", "reason": "crash, too few valid runs, or OI-1e", "unexplained": unexplained}
    return {"gate": "pass"} if not fails else {"gate": "REFINE (design)", "fails": fails}


def batch(out, kind, end):
    files = [out / f"{kind}.w{w}.jsonl" for w in range(WORKERS)]
    procs = [
        subprocess.Popen([PY, "-m", "ar1.batch", kind, str(f), str(w), str(WORKERS)], cwd=HERE)
        for w, f in enumerate(files)
    ]
    capped_out = False
    for p in procs:
        try:
            p.wait(timeout=max(1, end - time.time()))
        except subprocess.TimeoutExpired:
            p.kill()
            capped_out = True
    return [json.loads(l) for f in files if f.exists() for l in f.read_text().splitlines()], capped_out


def adequacy(out, end):
    rows, capped_out = batch(out, "adequacy", end)
    sc = score(rows)
    missed = {
        m: {k: sc.get(m, {}).get(k) for k in ("runs", "detected", "not_scored")}
        for m, pts in ADEQUACY.items()
        if sc.get(m, {}).get("runs") != 5 * len(pts) or sc[m]["detected"] != sc[m]["runs"]
    }
    return {"capped": capped_out, "runs": len(rows), "missed": missed, "score": sc}


def verdict(r):
    if "check" in r:
        return r["check"]["verdict"]
    return "missing_point" if "missing_point" in r else "unresolved" if "unresolved" in r else "harness_error"


def summarize(name, rows, capped_out):
    ids = {p["id"] for p in points()}
    invalid_at = {}
    for r in rows:
        if verdict(r) == "invalid":
            invalid_at[r["point"]] = invalid_at.get(r["point"], 0) + 1
    last = {}
    for r in rows:
        last[(r["point"], r["seed"])] = r
    rows = list(last.values())
    checked = [r for r in rows if "check" in r]
    verdicts, exercised, failing = {}, {}, {}
    for r in rows:
        verdicts[verdict(r)] = verdicts.get(verdict(r), 0) + 1
    for r in checked:
        for c in r["check"]["exercised"]:
            exercised[c] = exercised.get(c, 0) + 1
        for c, st in r["check"]["clauses"].items():
            if st in ("violation", "unmet"):
                failing.setdefault(c, {}).setdefault(st, 0)
                failing[c][st] += 1
    clauses = set(checked[0]["check"]["clauses"]) if checked else set()
    unresolved = [
        {
            "point": r["point"],
            "seed": r["seed"],
            "why": r.get("unresolved")
            or r.get("harness_error")
            or r.get("missing_point")
            or (r.get("check") or {}).get("unresolved")
            or (r.get("check") or {}).get("invalid"),
        }
        for r in rows
        if verdict(r) not in ("pass", "violation")
    ] + [{"point": p, "why": f"{n} invalid runs"} for p, n in invalid_at.items() if n > 1]
    per_point = {}
    for r in rows:
        per_point[r["point"]] = per_point.get(r["point"], 0) + 1
    return {
        "arm": name,
        "runs": len(rows),
        "expected": len(ids) * SEEDS,
        "short_points": sorted(p for p in ids if per_point.get(p, 0) < SEEDS),
        "capped": capped_out,
        "verdicts": verdicts,
        "points_reached": len({r["point"] for r in checked}),
        "failing_by_clause": failing,
        "exercised_runs_per_clause": dict(sorted(exercised.items())),
        "unexercised": sorted(clauses - set(exercised)),
        "invalid_repeated": invalid_at,
        "unresolved": unresolved,
        "max_seconds": max((float(r.get("seconds") or 0) for r in rows), default=0),
    }


def lines():
    """Section 8: logical non-blank, non-comment lines of the black-formatted diff from step 1 to step 2."""
    d = sh("git", "-C", str(REPO), "diff", "--no-index", "-U0", "experiments/ar1/plain1", "experiments/ar1/plain2")
    added = [l[1:] for l in d.splitlines() if l.startswith("+") and not l.startswith("+++")]
    count = sum(1 for l in added if l.strip() and not l.strip().startswith("#"))
    total = sum(
        1
        for f in (HERE / "plain2").glob("*.py")
        for l in f.read_text().splitlines()
        if l.strip() and not l.strip().startswith("#")
    )
    snc = sum(
        1
        for f in ("rt.py", "app.py", "iface.py")
        for l in (HERE / "ar1" / f).read_text().splitlines()
        if l.strip() and not l.strip().startswith("#")
    )
    return {"step1_to_step2_lines": count, "plain2_total_lines": total, "snc_rt_lines": snc}


def perf(out, end):
    f = out / "perf.jsonl"
    rc = capped([PY, "-m", "ar1.perf", "snc,plain2", str(TASKS), str(f)], end)
    rows = [json.loads(l) for l in f.read_text().splitlines()] if f.exists() else []

    def q(xs, p):
        xs = sorted(xs)
        return xs[min(len(xs) - 1, int(len(xs) * p))] if xs else None

    by = {}
    for a in ("snc", "plain2"):
        mine = [r for r in rows if r["arm"] == a]
        ok = [r for r in mine if r.get("rc") == 0 and r.get("net_ns") is not None and r.get("result")]
        by[a] = {
            "tasks": len(ok),
            "failed": [{k: r.get(k) for k in ("i", "rc", "error", "result")} for r in mine if r not in ok],
            "net_ms_p50": q([r["net_ns"] / 1e6 for r in ok], 0.5),
            "net_ms_p99": q([r["net_ns"] / 1e6 for r in ok], 0.99),
            "consequential_commits_per_task": q([r["consequential_commits"] for r in ok], 0.5),
            "commit_records_per_task": q([r["commits"] for r in ok], 0.5),
            "wal_bytes_per_task": q([r["wal_bytes"] for r in ok], 0.5),
            "blob_bytes_per_task": q([r["blob_bytes"] for r in ok], 0.5),
        }
    s, p2 = by["snc"], by["plain2"]
    res = {"capped": rc == "cap", "complete": s["tasks"] == TASKS and p2["tasks"] == TASKS, "arms": by}
    if res["complete"] and s["consequential_commits_per_task"]:
        n = s["consequential_commits_per_task"]
        res["B_ms_p50"] = (s["net_ms_p50"] - p2["net_ms_p50"]) / n
        res["B_ms_p99"] = (s["net_ms_p99"] - p2["net_ms_p99"]) / n
        res["B_holds"] = res["B_ms_p50"] <= 10 and res["B_ms_p99"] <= 50
        res["B_factor"] = max(res["B_ms_p50"] / 10, res["B_ms_p99"] / 50)
        m = s["commit_records_per_task"]
        res["B_per_commit_record_ms"] = [
            (s["net_ms_p50"] - p2["net_ms_p50"]) / m,
            (s["net_ms_p99"] - p2["net_ms_p99"]) / m,
        ]
    return res


def diverge(out):
    res = {}
    for a in ("snc", "plain1", "plain2"):
        f = out / f"diverge-{a}.json"
        if not f.exists():
            try:
                subprocess.run([PY, "-m", "ar1.diverge", a, "1", str(f)], cwd=HERE, timeout=900)
            except subprocess.TimeoutExpired:
                pass
        d = json.loads(f.read_text()) if f.exists() else {}
        res[a] = {
            "shadow_loaded": d.get("shadow_loaded"),
            "dbos_reported": d.get("dbos_reported"),
            "results": d.get("results"),
            "verdict": (d.get("check") or {}).get("verdict"),
            "violations": sorted({v["clause"] for v in (d.get("check") or {}).get("violations", [])}),
        }
    return res


def outcome(s):
    """Section 9, first that applies."""
    p0 = s["phase0"]
    if p0["gate"] == "UNRESOLVED":
        return "UNRESOLVED", p0
    if p0["gate"] != "pass":
        return "REFINE (design)", p0
    if s["adequacy"]["capped"] or s["adequacy"]["missed"]:
        return "UNRESOLVED", {"missed mutations": s["adequacy"]["missed"], "capped": s["adequacy"]["capped"]}
    arms = [s[a] for a in ("snc", "plain1", "plain2") if a in s]
    bad = [a["arm"] for a in arms if a["capped"] or a["unresolved"] or a["short_points"]]
    if bad or s["snc"]["unexercised"]:
        return "UNRESOLVED", {"arms": bad, "snc_unexercised": s["snc"]["unexercised"]}
    if s["snc"]["failing_by_clause"] or s["snc"]["verdicts"].get("violation"):
        return "STOP (unless package 3.5 can fix it)", s["snc"]["failing_by_clause"]
    b = s["perf"]
    if b["capped"] or "B_holds" not in b:
        return "UNRESOLVED", "performance capped or incomplete"
    if b["B_factor"] > 2:
        return "STOP", "B fails by more than a factor of 2"
    if not b["B_holds"]:
        return "REFINE", "B fails by at most a factor of 2"
    p2 = s["plain2"]
    if p2["verdicts"].get("pass") == p2["runs"] and s["lines"]["step1_to_step2_lines"] <= 300:
        return "NARROW", {
            "why": "plain DBOS step 2 meets items 1-7 in L lines or fewer",
            "unexercised": p2["unexercised"],
        }
    return "GO", "Phase 0 passes, items 1-7 pass on SNC, B holds, plain DBOS needs more than L lines"


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    fz = freeze()
    subprocess.run(["pkill", "-9", "-f", "--", f"-D {os.environ.get('AR1_RUNS', '/tmp/ar1-runs')}"])
    os.environ["AR1_FREEZE"] = fz["commit"]
    if not (out / "manifest.txt").exists():
        manifest(out, fz)
    sf = out / "summary.json"
    s = json.loads(sf.read_text()) if sf.exists() else {"freeze": fz}
    if s["freeze"] != fz:
        sys.exit("results directory belongs to a different freeze")

    def save():
        sf.write_text(json.dumps(s, indent=1))

    s.setdefault("lines", lines())

    def end(phase):
        t = s.setdefault("started", {}).setdefault(phase, time.time())
        save()
        return t + CAP_H[phase] * 3600

    def finish():
        s["outcome"] = outcome(s)
        save()
        print(json.dumps(s["outcome"]))

    if "phase0" not in s:
        s["phase0"] = phase0(out, end("phase0"))
        save()
    if s["phase0"]["gate"] != "pass":
        return finish()
    if "adequacy" not in s:
        s["adequacy"] = adequacy(out, end("adequacy"))
        save()
    if s["adequacy"]["capped"] or s["adequacy"]["missed"]:
        return finish()
    for a in ("snc", "plain1", "plain2"):
        if a not in s:
            s[a] = summarize(a, *batch(out, f"counted-{a}", end(a)))
            save()
        if s[a]["unresolved"] or s[a]["capped"] or s[a]["short_points"]:
            return finish()
    if "perf" not in s:
        s["perf"] = perf(out, end("perf"))
        save()
    if "diverge" not in s:
        s["diverge"] = diverge(out)
        save()
    finish()


if __name__ == "__main__":
    main()
