import json
import sys
from collections import Counter, defaultdict

from check.check import MUTATION_CLAUSES


def score(rows):
    out = defaultdict(
        lambda: {
            "runs": 0,
            "exercised": 0,
            "detected": 0,
            "flagged": Counter(),
            "not_scored": Counter(),
            "by_clause": Counter(),
        }
    )
    for r in rows:
        m = r.get("mutation") or "none"
        s = out[m]
        s["runs"] += 1
        c = r.get("check") or {}
        if r.get("unresolved") or "clauses" not in c:
            s["not_scored"][
                r.get("harness_error") and "harness_error" or r.get("unresolved") and "unresolved" or "no check"
            ] += 1
            continue
        if c.get("verdict") in ("unresolved", "invalid"):
            s["not_scored"][c["verdict"]] += 1
            continue
        bad = [k for k, v in c["clauses"].items() if v in ("violation", "unmet")]
        s["flagged"].update(bad)
        want = MUTATION_CLAUSES.get(m, ())
        s["exercised"] += any(k in (c.get("exercised") or []) for k in want)
        s["detected"] += bool(set(want) & set(bad)) if want else bool(bad)
        for k in want:
            s["by_clause"][k] += k in bad
    return {
        m: {
            **s,
            "flagged": dict(s["flagged"]),
            "not_scored": dict(s["not_scored"]),
            "by_clause": {k: s["by_clause"][k] for k in MUTATION_CLAUSES.get(m, ())},
            "clauses": MUTATION_CLAUSES.get(m),
        }
        for m, s in sorted(out.items())
    }


def main(path):
    rows = [json.loads(x) for x in open(path).read().splitlines() if x.strip()]
    print(json.dumps(score(rows), indent=1, sort_keys=True))


if __name__ == "__main__":
    main(sys.argv[1])
