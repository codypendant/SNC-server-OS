"""Uncounted G2 runs: python -m ar1.batch bringup|adequacy <out.jsonl> [worker] [workers]."""

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
    if kind == "bringup":
        return [
            (p["id"], s, "none", arm)
            for arm in ("snc", "plain1")
            for p in points()
            for s in seeds(p["id"], 2, "bringup", BRINGUP.get(p["id"]))
        ]
    return [
        (pt, s, m, "snc")
        for m, pts in ADEQUACY.items()
        for pt in pts
        for s in seeds(pt, 5, f"adequacy:{m}", WANT.get(m) if pt.startswith("C-b2/") else None)
    ]


def main():
    kind, out = sys.argv[1], Path(sys.argv[2])
    worker, workers = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (0, 1)
    done = set()
    if out.exists():
        done = {
            (r["point"], r["seed"], r["mutation"] or "none", r["arm"])
            for r in map(json.loads, out.read_text().splitlines())
        }
    env = {
        **os.environ,
        "AR1_PORT": str(24320 + 100 * worker),
        "AR1_RUNS": f"{os.environ.get('AR1_RUNS', '/tmp/ar1-runs')}/w{worker}",
    }
    code = code_hash()
    for i, job in enumerate(jobs(kind)):
        if i % workers != worker or job in done:
            continue
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
        with open(out, "a") as f:
            f.write(json.dumps(res, default=str) + "\n")
        subprocess.run(["pkill", "-9", "-f", f"-D {env['AR1_RUNS']}"], capture_output=True)


if __name__ == "__main__":
    main()
