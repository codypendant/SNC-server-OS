"""AR-1 G3 counted campaign (prereg revision 3): python -m ar1.g3 <results dir>.

Runs as the non-root runner on the dedicated VM, from a clean checkout of the frozen commit. Phases, each with its
section 10 cap: Phase 0 (hard gate), SNC arm, plain step 1, plain step 2, performance, determinism divergence. A
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
from .batch import code_hash
from .harness import HERE, PY
from .matrix import points

REPO = HERE.parent.parent
SEEDS = int(os.environ.get("AR1_SEEDS", "20"))
TASKS = int(os.environ.get("AR1_PERF_TASKS", "1000"))
CHECKS = [(c, SEEDS) for c in ("OI-1a", "OI-1a-control", "OI-1a-v", "OI-1b", "OI-1c", "OI-1d", "C-7", "C-7b")]
CAP_H = {"phase0": 8, "snc": 16, "plain1": 16, "plain2": 16, "perf": 4}
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


def capped(cmd, hours, env=None):
    try:
        return subprocess.run(cmd, cwd=HERE, env={**os.environ, **(env or {})}, timeout=hours * 3600).returncode
    except subprocess.TimeoutExpired:
        return "cap"


def phase0(out):
    d = out / "phase0"
    end = time.time() + CAP_H["phase0"] * 3600
    for name, n in CHECKS:
        if (d / f"{name}.json").exists():
            continue
        if capped([PY, "-m", "ar1.phase0", name, str(n), str(d)], max(0.01, (end - time.time()) / 3600)) == "cap":
            return {"gate": "UNRESOLVED", "reason": f"Phase 0 cap hit during {name}"}
    if not (d / "OI-1e.json").exists():
        with open(d / "OI-1e.json", "w") as f:
            subprocess.run(
                [
                    PY,
                    "-m",
                    "check.oi1e",
                    str(Path(os.environ.get("AR1_RUNS", "/tmp/ar1-runs")) / "oi1e"),
                    str(SEEDS),
                    "1000" if SEEDS == 20 else "40",
                ],
                cwd=HERE,
                stdout=f,
                timeout=max(1, end - time.time()),
            )
    fails = []
    for name, n in CHECKS:
        rs = json.loads((d / f"{name}.json").read_text()) if (d / f"{name}.json").exists() else []
        if len(rs) < n or not all(r.get("pass") for r in rs):
            fails.append(name)
    e = json.loads((d / "OI-1e.json").read_text() or "{}")
    if e.get("overall") != "pass":
        return {"gate": "UNRESOLVED", "reason": "OI-1e instrument failure (section 6)", "fails": fails + ["OI-1e"]}
    return {"gate": "pass"} if not fails else {"gate": "REFINE (design)", "fails": fails}


def arm(out, name):
    files = [out / f"{name}.w{w}.jsonl" for w in range(WORKERS)]
    procs = [
        subprocess.Popen([PY, "-m", "ar1.batch", f"counted-{name}", str(f), str(w), str(WORKERS)], cwd=HERE)
        for w, f in enumerate(files)
    ]
    end = time.time() + CAP_H[name] * 3600
    capped_out = False
    for p in procs:
        try:
            p.wait(timeout=max(1, end - time.time()))
        except subprocess.TimeoutExpired:
            p.kill()
            capped_out = True
    rows = [json.loads(l) for f in files if f.exists() for l in f.read_text().splitlines()]
    return summarize(name, rows, capped_out)


def summarize(name, rows, capped_out):
    ids = {p["id"] for p in points()}
    checked = [r for r in rows if "check" in r]
    verdicts = {}
    for r in rows:
        k = (
            r["check"]["verdict"]
            if "check" in r
            else ("missing_point" if "missing_point" in r else "unresolved" if "unresolved" in r else "harness_error")
        )
        verdicts[k] = verdicts.get(k, 0) + 1
    exercised, violations = {}, {}
    for r in checked:
        for c in r["check"]["exercised"]:
            exercised[c] = exercised.get(c, 0) + 1
        for v in r["check"]["violations"]:
            violations[v["clause"]] = violations.get(v["clause"], 0) + 1
    clauses = set(checked[0]["check"]["clauses"]) if checked else set()
    unresolved = [
        {
            "point": r["point"],
            "seed": r["seed"],
            "why": r.get("unresolved")
            or r.get("harness_error")
            or r["check"].get("unresolved")
            or r["check"].get("invalid"),
        }
        for r in rows
        if "unresolved" in r
        or "harness_error" in r
        or ("check" in r and (r["check"]["verdict"] not in ("pass", "violation")))
    ]
    return {
        "arm": name,
        "runs": len(rows),
        "expected": len(ids) * SEEDS,
        "capped": capped_out,
        "verdicts": verdicts,
        "points_reached": len({r["point"] for r in checked}),
        "violations_by_clause": violations,
        "exercised_runs_per_clause": dict(sorted(exercised.items())),
        "unexercised": sorted(clauses - set(exercised)),
        "missing_points": sorted({r["point"] for r in rows if "missing_point" in r}),
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


def perf(out):
    f = out / "perf.jsonl"
    rc = capped([PY, "-m", "ar1.perf", "snc,plain2", str(TASKS), str(f)], CAP_H["perf"])
    rows = [json.loads(l) for l in f.read_text().splitlines()] if f.exists() else []

    def q(xs, p):
        xs = sorted(xs)
        return xs[min(len(xs) - 1, int(len(xs) * p))] if xs else None

    by = {}
    for a in ("snc", "plain2"):
        ok = [r for r in rows if r["arm"] == a and r["rc"] == 0 and r["net_ns"] is not None]
        by[a] = {
            "tasks": len(ok),
            "failed": sum(r["arm"] == a for r in rows) - len(ok),
            "net_ms_p50": q([r["net_ns"] / 1e6 for r in ok], 0.5),
            "net_ms_p99": q([r["net_ns"] / 1e6 for r in ok], 0.99),
            "commits_per_task": q([r["commits"] for r in ok], 0.5),
            "wal_bytes_per_task": q([r["wal_bytes"] for r in ok], 0.5),
            "blob_bytes_per_task": q([r["blob_bytes"] for r in ok], 0.5),
        }
    s, p2 = by["snc"], by["plain2"]
    res = {"capped": rc == "cap", "arms": by}
    if s["tasks"] and p2["tasks"] and s["commits_per_task"]:
        res["B_ms_p50"] = (s["net_ms_p50"] - p2["net_ms_p50"]) / s["commits_per_task"]
        res["B_ms_p99"] = (s["net_ms_p99"] - p2["net_ms_p99"]) / s["commits_per_task"]
        res["B_holds"] = res["B_ms_p50"] <= 10 and res["B_ms_p99"] <= 50
        res["B_factor"] = max(res["B_ms_p50"] / 10, res["B_ms_p99"] / 50)
    return res


def diverge(out):
    res = {}
    for a in ("snc", "plain1", "plain2"):
        f = out / f"diverge-{a}.json"
        if not f.exists():
            subprocess.run([PY, "-m", "ar1.diverge", a, "1", str(f)], cwd=HERE, timeout=900)
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
    arms = [s[a] for a in ("snc", "plain1", "plain2") if a in s]
    if (
        any(a["capped"] or a["unresolved"] or a["runs"] < a["expected"] for a in arms)
        or s["snc"]["unexercised"]
        or s["snc"].get("missing_points")
        or s.get("perf", {}).get("capped")
    ):
        return "UNRESOLVED", "cap, unresolved run, missing SNC point or unexercised clause"
    b = s.get("perf", {})
    if s["snc"]["violations_by_clause"]:
        return "STOP (unless package 3.5 can fix it)", s["snc"]["violations_by_clause"]
    if "B_holds" not in b:
        return "UNRESOLVED", "no B measurement"
    if b["B_factor"] > 2:
        return "STOP", "B fails by more than a factor of 2"
    if not b["B_holds"]:
        return "REFINE", "B fails by at most a factor of 2"
    p2 = s["plain2"]
    if p2["verdicts"].get("pass") == p2["runs"] and s["lines"]["step1_to_step2_lines"] <= 300:
        return "NARROW", "plain DBOS step 2 meets items 1-7 in L lines or fewer"
    return "GO", "Phase 0 passes, items 1-7 pass on SNC, B holds, plain DBOS needs more than L lines"


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    fz = freeze()
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
    if "phase0" not in s:
        s["phase0"] = phase0(out)
        save()
    if s["phase0"]["gate"] != "pass":
        s["outcome"] = outcome(s)
        save()
        print(json.dumps(s["outcome"]))
        return
    for a in ("snc", "plain1", "plain2"):
        if a not in s:
            s[a] = arm(out, a)
            save()
        if s[a]["unresolved"] or s[a]["capped"]:
            s["outcome"] = outcome(s)
            save()
            print(json.dumps(s["outcome"]))
            return
    if "perf" not in s:
        s["perf"] = perf(out)
        save()
    if "diverge" not in s:
        s["diverge"] = diverge(out)
        save()
    s["outcome"] = outcome(s)
    save()
    print(json.dumps(s["outcome"]))


if __name__ == "__main__":
    main()
