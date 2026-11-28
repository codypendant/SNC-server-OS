import sys
from collections import defaultdict

from .common import RESULTS, load

ORDER = (
    "miss",
    "false-positive",
    "checker-crash",
    "harness-error",
    "unresolved",
    "no-credit",
    "flagged",
    "correct-pass",
)


def rank(st):
    return next((i for i, k in enumerate(ORDER) if st.startswith(k)), len(ORDER))


def exp(e):
    if isinstance(e, dict):
        return " ".join(f"{k}={v}" for k, v in e.items())
    return ", ".join(e) or "none (must pass)"


def got(r):
    s = r["verdict"]
    if r.get("fails"):
        s += " fail: " + ", ".join(r["fails"])
    if r.get("unresolved"):
        s += " unresolved: " + ", ".join(r["unresolved"])
    if r.get("invalid"):
        s += f" invalid: {r['invalid']}"
    return s


def table(head, rows):
    rows.sort(key=lambda r: (rank(r[-1]), r[0]))
    out = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    out += ["| " + " | ".join(str(c).replace("|", "/") for c in r) + " |" for r in rows]
    return out


def l1():
    data = load("l1") or {}
    by = defaultdict(list)
    for r in data.values():
        by[r["fault"]].append(r)
    rows = []
    for f, rs in by.items():
        worst = min((r["status"] for r in rs), key=rank)
        bad = [
            f"{r['base']}: {r['status']} ({got(r)})"
            for r in rs
            if r["status"] != worst or rank(worst) < 6
        ]
        ok = sum(r["status"] == worst for r in rs)
        sample = (
            got(rs[0])
            if not bad
            else "; ".join(bad[:2])
            + (f"; +{len(bad) - 2} more" if len(bad) > 2 else "")
        )
        rows.append((f, exp(rs[0]["expected"]), f"{ok}/{len(rs)} bases", sample, worst))
    return table(["L1 fault", "expected", "bases", "checker result", "status"], rows)


def l2(arm):
    data = load(f"l2-{arm}") or {}
    rows = [
        (
            r["fault"],
            f"{r['point']}/{r['scenario']}/{r['seed']}",
            exp(r["expected"]),
            got(r),
            r["evidence"],
            r["status"],
        )
        for r in data.values()
    ]
    return table(
        [
            f"L2 fault ({arm})",
            "run",
            "expected",
            "checker result",
            "proxy evidence",
            "status",
        ],
        rows,
    )


def l3(arm):
    data = load(f"l3-{arm}") or {}
    rows = [
        (
            r["mutant"],
            r["cell"],
            r["guarantee"] + " (new clause)",
            got(r),
            r["evidence"],
            ", ".join(r["control_fails"]) or "-",
            r["status"],
        )
        for r in data.values()
    ]
    return table(
        [
            f"L3 mutant ({arm})",
            "run",
            "expected",
            "checker result",
            "mutation evidence",
            "control fails",
            "status",
        ],
        rows,
    )


def controls(arm):
    data = load(f"controls-{arm}") or {}
    rows = [
        (k, got(r), r["run"], "pass" if r["verdict"] == "pass" else "step-1 finding")
        for k, r in data.items()
    ]
    return table(
        [f"control ({arm}, unmodified)", "checker result", "harness", "status"], rows
    )


def main():
    arms = sys.argv[1:] or ["dbos"]
    out = ["# Track A checker adequacy (development scale)", ""] + l1() + [""]
    for a in arms:
        out += l2(a) + [""] + controls(a) + [""] + l3(a) + [""]
    text = "\n".join(out) + "\n"
    (RESULTS / "report.md").write_text(text)
    print(text)


if __name__ == "__main__":
    main()
