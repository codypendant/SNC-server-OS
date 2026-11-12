#!/usr/bin/env python3
"""CP-1A Profile-A objective-rate duration validation
(docs/handoff/PR111_CP1A_Result_Final_Review.md, NEXT AUTHORIZED TASK; PREREG.md here).

A pilot-method validation, not REFINE round 2: 9 open-loop Profile-A runs at
exactly the CP-1A objective rate, 654,569 ops/s, 60/120/240 s x 3 reps, with
the normal 10 s warm-up and 10 s drain and a fresh server and data dir per rep.

bench/pilot.py (frozen at runner commit 4250b32) is imported unchanged:
- its real-mode preflight (Pilot.environment: the CP-1A freeze gate, clean
  tree, rebuild, RelWithDebInfo, governors, NUMA balancing, manifest e,
  scratch/data/out isolation);
- its run path (Pilot.one, as the pilot's own duration check calls it), the
  plan's Profile-A server command and checkpoint configuration, and the
  profile guard;
- its sustained() rule.
On top of the preflight it refuses unless the rebuilt binaries are the
CP-1A binaries and the rate and D reproduce the CP-1A record.

  aduration.py run OUT --scratch DIR --manifest M [--build build-s6]
  aduration.py judge OUT        # recompute the verdict from OUT/pilot.json
"""
import argparse, json, os, random, re, shutil, statistics, sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.realpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "bench"))
import pilot  # noqa: E402

CP1A = os.path.join(REPO, "bench", "results", "cp1a-r640-20261111", "pilot.json")
PROFILE, PHASE = "A", "aduration"
RATE = 654569                          # CP-1A A objective rate (PR #111 review)
WINDOWS = (60, 120, 240)
REPS = 3
CV_P99_FIXED_A = 0.019286005545956553  # CP-1A results.A.fixed.cv_p99, as recorded
D = 3 * Fraction(CV_P99_FIXED_A)       # exact: 3 x the recorded float; = 0.0578580166...
DATA_ROOT = "/srv/snc-data/aduration"


def order(plan):
    """The run order, fixed before data: the pilot's seeded shuffle of (window, rep)."""
    cells = [(w, i) for w in WINDOWS for i in range(REPS)]
    random.Random(plan["order_seed"]).shuffle(cells)
    return cells


def label(w, i):
    return f"{w}s-r{i}"


def cp1a_record(path=CP1A):
    """The CP-1A values this validation depends on, checked against the constants."""
    d = json.load(open(path))
    a = d["results"]["A"]
    assert a["sweep"]["objective_rate"] == RATE == a["fixed"]["rate"], "objective rate is not the CP-1A one"
    assert a["fixed"]["cv_p99"] == CV_P99_FIXED_A, "CV_p99 is not the CP-1A one"
    e = d["environment"]
    return dict(snc_server_sha256=e["snc_server_sha256"], net_bench_sha256=e["net_bench_sha256"],
                plan_sha256=e["plan_sha256"], manifest_sha256=e["manifest_sha256"], git_head=e["git_head"])


def checkpoints(server_log):
    """Periodic checkpoints in one server log (report only; not the recovery one)."""
    try:
        return len(re.findall(r"^checkpoint: \d+ objects in", open(server_log).read(), re.M))
    except OSError:
        return None


def judge(runs, plan):
    """The preregistered verdict from the 9 runs (PREREG.md, 'Decision').

    PASS: all 9 reps sustained (pilot.sustained) and the 120 s and 240 s
    median total p99 are each <= the 60 s median total p99 x (1 + D).
    FAIL: any rep unsustained or failed, or either bound exceeded.
    INCOMPLETE: fewer than 3 reps in some cell (an abort); not a PASS."""
    cells = {}
    for w in WINDOWS:
        reps = sorted((r for r in runs if r.get("profile") == PROFILE and r.get("phase") == PHASE
                       and r.get("window_s") == w), key=lambda r: r["label"])
        rows = []
        for r in reps:
            ok, why = pilot.sustained(r, plan)
            rows.append(dict(label=r["label"], sustained=ok, reasons=why, p99_ns=r.get("p99_ns"),
                             p999_ns=r.get("p999_ns"), max_ns=r.get("max_ns"), achieved=r.get("achieved"),
                             overloaded=r.get("overloaded"), index_refused=r.get("index_refused"),
                             checkpoints=r.get("checkpoints")))
        good = [x for x in rows if x["p99_ns"] is not None]
        cells[w] = dict(reps=len(rows), sustained_reps=sum(x["sustained"] for x in rows), rows=rows,
                        median_p99_ns=statistics.median(x["p99_ns"] for x in good) if len(good) == REPS else None,
                        median_p999_ns=statistics.median(x["p999_ns"] for x in good) if len(good) == REPS else None,
                        median_max_ns=statistics.median(x["max_ns"] for x in good) if len(good) == REPS else None)
    v = dict(rate=RATE, D=str(D), D_float=float(D), cells={str(w): c for w, c in cells.items()})
    if any(c["reps"] != REPS for c in cells.values()):
        v.update(verdict="INCOMPLETE", why=["a cell has fewer than 3 reps (abort): stop and review"])
        return v
    why = [f"{w} s {x['label']}: unsustained ({'; '.join(x['reasons'])})"
           for w, c in cells.items() for x in c["rows"] if not x["sustained"]]
    m60 = cells[60]["median_p99_ns"]
    bound = None
    if m60 is not None:
        bound = Fraction(m60) * (1 + D)
        v["bound_p99_ns"] = float(bound)
        for w in (120, 240):
            m = cells[w]["median_p99_ns"]
            within = m is not None and Fraction(m) <= bound
            cells[w]["within_bound"] = within
            if not within:
                why.append(f"{w} s median p99 {m} > 60 s median {m60} x (1 + D) = {float(bound):.1f}")
    else:
        why.append("no 60 s median p99")
    v.update(verdict="FAIL" if why else "PASS", why=why)
    return v


def run(a):
    plan = json.load(open(a.plan))
    ref = cp1a_record()
    a.quick, a.allocation, a.data_root, a.scratch_given = False, plan["baseline_allocation"], DATA_ROOT, True
    os.makedirs(os.path.join(a.out, "raw"), exist_ok=True)
    p = pilot.Pilot(a, plan)               # the CP-1A real-mode preflight, unchanged
    bad = [k for k in ("snc_server_sha256", "net_bench_sha256", "plan_sha256", "manifest_sha256")
           if p.env[k] != ref[k]]
    if bad:
        raise SystemExit("refusing: not the CP-1A identity: " + ", ".join(f"{k} {p.env[k]} != {ref[k]}" for k in bad))
    p.env.update(validation="cp1a-a-duration", cp1a_identity=ref, order=[label(w, i) for w, i in order(plan)],
                 rate=RATE, D=str(D))
    p.write()
    try:
        res = execute(p, plan)
    finally:
        shutil.rmtree(DATA_ROOT, ignore_errors=True)
        try:
            os.rmdir(a.scratch)
        except OSError:
            pass
    print(f"VERDICT {res['verdict']}: " + ("; ".join(res["why"]) or "all 9 sustained, both bounds hold"))
    return 0


def execute(p, plan):
    """The 9 runs in the preregistered order through pilot's own run path,
    then the verdict. An abort stops the session: no rerun, no replacement."""
    res = p.results.setdefault("A_duration_validation", {})
    try:
        for w, i in order(plan):
            r = p.one(PROFILE, PHASE, label(w, i), rate=RATE, depth=plan["rate_depth"],
                      window=(plan["warmup_s"], w))
            r["checkpoints"] = checkpoints(os.path.join(p.a.out, "raw", f"{PROFILE}-{PHASE}-{label(w, i)}.server.log"))
            p.write()
    except pilot.Abort as e:
        res["aborted"] = str(e)
        print(f"ABORTED: {e}", file=sys.stderr)
    res.update(judge(p.runs, plan))
    p.write()
    return res


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
    print(json.dumps({k: v[k] for k in ("verdict", "why", "D", "rate") if k in v}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
