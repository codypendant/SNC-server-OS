"""python -m ar1.batch bringup|bringup-<arm>|adequacy|counted-<arm> <out.jsonl> [worker] [workers].

Counted kinds need AR1_FREEZE, the frozen SHA set (section 5.5); their seeds are sha256(freeze, point, i)."""

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

from .matrix import points, release, seed

PY = sys.executable
HERE = Path(__file__).resolve().parent.parent

ADEQUACY = {
    "M1": [
        "A/a3:I-disp",
        "A/a3:TI-disp",
        "A/a3:TF-disp",
        "A/a3:TN-disp",
        "Ac/I-disp",
        "Ac/TI-disp",
        "Ac/TF-disp",
        "Ac/TN-disp",
    ],
    "M2": ["C-s/I-out", "C-s/TI-out", "C-s/TF-out", "C-s/TN-out"],
    "M3": ["B/b2:model", "B/b3:model"],
    "M4": ["B/b2:T-N", "B/b3:T-N"],
    "M5": ["P/pre-policy"],
    "M6": ["C-p/I-prep", "C-p/TI-prep", "C-p/TF-prep", "C-p/TN-prep"],
    "M7": ["B/b2:T-I"],
    "M8": ["C-b2/model", "C-b2/T-I", "C-b2/T-F", "C-b2/T-N"],
    "M9": ["B/b2:T-I"],
    "M10": ["C-s/I-out", "C-s/TN-out"],
    "M11": [
        "C-a1/I-disp",
        "C-a1/TI-disp",
        "C-a1/TF-disp",
        "C-a1/TN-disp",
        "C-b2/model",
        "C-b2/T-I",
        "C-b2/T-F",
        "C-b2/T-N",
    ]
    + [f"D/{b}:{t}" for t in ("T-I", "T-F", "T-N") for b in ("b1", "b2", "b3")],
    "M12": ["B/b2:model", "B/b3:model"],
    "M13": ["C-b2/model", "C-b2/T-I", "C-b2/T-F", "C-b2/T-N"],
    "M14": ["C-b2/T-N"],
}
OUTCOME = {"C-b2/model": "after", "C-b2/T-I": "after", "C-b2/T-F": "after-outcome", "C-b2/T-N": "after-outcome"}
WANT = {
    "M8": lambda p, s: release(p, s) != "before",
    "M11": lambda p, s: release(p, s) != "before",
    "M13": lambda p, s: release(p, s) == OUTCOME[p],
    "M14": lambda p, s: release(p, s) == "after",
}
BRINGUP = {
    "C-b2/model": [lambda p, s: release(p, s) == "after", lambda p, s: release(p, s) == "before"],
    "C-b2/T-I": [lambda p, s: release(p, s) == "after", lambda p, s: release(p, s) == "before"],
    "C-b2/T-F": [lambda p, s: release(p, s) == "after", lambda p, s: release(p, s) == "after-outcome"],
    "C-b2/T-N": [lambda p, s: release(p, s) == "after" and s % 2 == 0, lambda p, s: release(p, s) == "after-outcome"],
}


def seeds(point_id, n, prefix, want=None):
    wants = want if isinstance(want, list) else [want] * n
    out, i = [], 0
    for w in wants:
        while True:
            s = seed(point_id, i, prefix)
            i += 1
            if w is None or w(point_id, s):
                out.append(s)
                break
    return out


def code_hash():
    h = hashlib.sha256()
    for f in sorted(HERE.glob("*/*.py")) + [HERE / "policy.json"]:
        h.update(f.read_bytes())
    return h.hexdigest()[:16]


def jobs(kind):
    if kind.startswith("counted-"):
        freeze = os.environ["AR1_FREEZE"]
        return [
            (p["id"], seed(p["id"], i, f"counted:{freeze}"), "none", kind[8:])
            for p in points()
            for i in range(int(os.environ.get("AR1_SEEDS", "20")))
        ]
    if kind.startswith("bringup"):
        arms = [kind[8:]] if kind.startswith("bringup-") else ["snc", "plain1"]
        return [
            (p["id"], s, "none", arm)
            for arm in arms
            for p in points()
            for s in seeds(p["id"], 2, "bringup", BRINGUP.get(p["id"]))
        ]
    return [
        (pt, s, m, "snc")
        for m, pts in ADEQUACY.items()
        for pt in pts
        for s in seeds(
            pt, 5, f"adequacy:{os.environ.get('AR1_FREEZE', '')}{m}", WANT.get(m) if pt.startswith("C-b2/") else None
        )
    ]


def is_invalid(r):
    return (r.get("check") or {}).get("verdict") == "invalid"


def main():
    kind, out = sys.argv[1], Path(sys.argv[2])
    worker, workers = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (0, 1)
    done, invalid = set(), {}
    if out.exists():
        for r in map(json.loads, out.read_text().splitlines()):
            k = (r["point"], r["seed"], r["mutation"] or "none", r["arm"])
            if is_invalid(r):
                invalid[r["point"]] = invalid.get(r["point"], 0) + 1
                if invalid[r["point"]] == 1:
                    continue
            done.add(k)
    env = {
        **os.environ,
        "AR1_PORT": str(int(os.environ.get("AR1_PORT_BASE", "24320")) + 100 * worker),
        "AR1_RUNS": f"{os.environ.get('AR1_RUNS', '/tmp/ar1-runs')}/w{worker}",
    }
    code = code_hash()
    subprocess.run(["pkill", "-9", "-f", "--", f"-D {env['AR1_RUNS']}"], capture_output=True)
    for i, job in enumerate(jobs(kind)):
        if i % workers != worker or job in done:
            continue
        while True:
            res = run(job, env, code)
            with open(out, "a") as f:
                f.write(json.dumps(res, default=str) + "\n")
            if not is_invalid(res):
                break
            invalid[job[0]] = invalid.get(job[0], 0) + 1
            if invalid[job[0]] > 1:
                break


def run(job, env, code):
    p = subprocess.Popen(
        [PY, "-m", "ar1.harness", job[0], str(job[1]), job[2], job[3]],
        cwd=HERE,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        so, err = p.communicate(timeout=420)
    except subprocess.TimeoutExpired:
        so, err = "", "batch timeout"
    subprocess.run(["pkill", "-9", "-s", str(p.pid)], capture_output=True)
    try:
        res = json.loads(so.strip().splitlines()[-1])
    except (IndexError, ValueError):
        res = {"point": job[0], "seed": job[1], "arm": job[3], "harness_error": err[-2000:]}
    res["mutation"] = res.get("mutation") or job[2]
    res["code"] = code
    subprocess.run(["pkill", "-9", "-f", "--", f"-D {env['AR1_RUNS']}"], capture_output=True)
    return res


if __name__ == "__main__":
    main()
