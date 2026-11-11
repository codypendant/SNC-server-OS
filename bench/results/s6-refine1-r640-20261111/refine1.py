#!/usr/bin/env python3
"""S6 REFINE round 1 (of 2): the W=1 configuration rescue
(docs/handoff/PR105_S6_Write_Window_Review.md; PREREG.md here).

It reruns only the affected current-server checkpoint cells, under the
unchanged S6 criteria:
- P1 = S6 P + `--ckpt-window 1` (primary, judged);
- H1 = S6 H + `--ckpt-window 1` (reported only);
- C1 = S6 C + `--ckpt-window 1` (capacity numerator).
3 reps each, rep-major P1, H1, C1.

The frozen S6 N reps (the p99 reference) and C0 reps (the capacity
denominator) are reused from bench/results/s6-r640-20261111, never rerun.
bench/s6.py (frozen at 63440b1) is imported unchanged. Its preflight,
postflight, driver, sampler, window proof, VOID/rerun policy and rule
functions are used as they are.

  refine1.py run OUT --scratch DIR
  refine1.py judge OUT              # recompute the verdict from OUT and the S6 references
"""
import json, os, sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "..", "..")
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "bench"))
import s6  # noqa: E402

S6_OUT = os.path.join(REPO, "bench", "results", "s6-r640-20261111")
RESCUE = {"P1": "P", "H1": "H", "C1": "C"}   # rescue cell -> the S6 cell it repeats
WINDOW = "1"
REF_P99_NS = 638976                           # frozen: median of S6 N p99 (PR #105 review)
C0_MEDIAN = 4016161                           # frozen: median of S6 C0 total ops/s


def setup():
    for cell, base in RESCUE.items():
        s6.CELLS[cell] = s6.CELLS[base]
    s6.ORDER = list(RESCUE)
    s6.DATA_ROOT = "/srv/snc-data/s6r1"
    base_commands = s6.commands

    def commands(cell, rep, port, admin, scratch, frozen):
        c = base_commands(cell, rep, port, admin, scratch, frozen)
        c["server"] = c["server"] + ["--ckpt-window", WINDOW]
        return c
    s6.commands = commands


def references(s6_out=S6_OUT):
    """The frozen S6 N and C0 reps, checked against the review's numbers."""
    reps = json.load(open(os.path.join(s6_out, "summary.json")))
    n = sorted((r for r in reps if r["cell"] == "N" and not r.get("void")), key=lambda r: r["rep"])
    c0 = sorted((r for r in reps if r["cell"] == "C0" and not r.get("void")), key=lambda r: r["rep"])
    assert len(n) == s6.REPS and len(c0) == s6.REPS, "S6 references incomplete"
    assert s6.n_usable(n), "S6 N reference unusable"
    assert s6.median(r["client"]["p99"] for r in n) == REF_P99_NS
    assert s6.median(r["client"]["total"] for r in c0) == C0_MEDIAN
    return n, c0


def judge(rescue_reps, void, n, c0):
    """REFINE #1 verdict:
    - PASS: all three P1 reps pass every original P rule (s6.p_rules against
      the frozen R), the capacity guard (s6.capacity_guard, C1 vs frozen
      C0) holds, and the session is not VOID;
    - FAIL: otherwise, unless VOID.
    H1 is reported only."""
    out = dict(void=list(void), p99_reference_ns=str(Fraction(REF_P99_NS)),
               p99_limit_ns=str(s6.P99_FACTOR * REF_P99_NS))
    by = lambda cell: sorted((x for x in rescue_reps if x["cell"] == cell and not x.get("void")),  # noqa: E731
                             key=lambda x: x["rep"])
    p1, c1 = by("P1"), by("C1")
    if not out["void"] and (len(p1) != s6.REPS or len(c1) != s6.REPS):
        out["void"].append("rescue cells incomplete")
    if out["void"]:
        out["verdict"] = "VOID"
        return out
    out["p1_reps"] = []
    for x in p1:
        rules, why = s6.p_rules(x, Fraction(REF_P99_NS))
        out["p1_reps"].append(dict(rep=x["rep"], rules=rules, failed=why))
    out["capacity_ok"], out["capacity_why"] = s6.capacity_guard(c1, c0)
    out["c1_median"] = str(s6.median(x["client"]["total"] for x in c1))
    out["c0_median"] = str(s6.median(x["client"]["total"] for x in c0))
    ok = all(all(r["rules"].values()) for r in out["p1_reps"]) and out["capacity_ok"]
    out["verdict"] = "PASS" if ok else "FAIL"
    return out


def run(out, scratch):
    setup()
    n, c0 = references()
    frozen = json.load(open(s6.FROZEN_PATH))
    if os.path.exists(out):
        raise SystemExit(f"{out} exists: REFINE #1 runs once")
    os.makedirs(os.path.join(out, "raw"))
    os.makedirs(scratch, exist_ok=True)
    os.makedirs(s6.DATA_ROOT, exist_ok=True)
    ok, rec = s6.preflight(out, scratch, frozen, real=False)   # identity, isolation, clean tree, binaries
    json.dump(rec, open(os.path.join(out, "preflight.json"), "w"), indent=1, default=str)
    if not ok:
        raise SystemExit("REFUSED before any run: " + "; ".join(rec["problems"]))
    d = s6.Driver(out, scratch, frozen, rec["machine_values"]["data_disk"])

    def post():
        mc, mv = s6.machine_identity(frozen, set(rec["disks"]["data"]))
        bc, bv = s6.build_identity(frozen)
        json.dump(dict(machine_checks=mc, machine_values=mv, build_checks=bc, build_values=bv),
                  open(os.path.join(out, "postflight.json"), "w"), indent=1, default=str)
        why = [f"machine {k}" for k, v in mc.items() if not v] + [f"build {k}" for k, v in bc.items() if not v]
        if mv != rec["machine_values"]:
            why.append("machine values changed")
        return not why, why

    reps, void = s6.run_session(d.execute, post)
    v = judge(reps, void, n, c0)
    json.dump(v, open(os.path.join(out, "verdict.json"), "w"), indent=1, default=str)
    print("REFINE1", v["verdict"], flush=True)
    return 0


def judge_saved(out):
    setup()
    n, c0 = references()
    reps = json.load(open(os.path.join(out, "summary.json")))
    for r in reps:
        r["checkpoints"] = [(o, Fraction(t)) for o, t in r.get("checkpoints", [])]
    void = json.load(open(os.path.join(out, "verdict.json")))["void"]
    return judge(reps, void, n, c0)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["run"] and len(a) == 4 and a[2] == "--scratch":
        sys.exit(run(a[1], a[3]))
    if a[:1] == ["judge"] and len(a) == 2:
        print(json.dumps(judge_saved(a[1]), indent=1, default=str))
        sys.exit(0)
    raise SystemExit(__doc__)
