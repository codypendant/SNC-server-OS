import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ADQ = Path(__file__).resolve().parent
TRACKA = ADQ.parent
PY = "/opt/ar1/bin/python3"
RUNS = Path(os.environ.get("ADQ_RUNS", "/tmp/adq-runs"))
BASE = ADQ / "base"
BUILD = ADQ / "build"
RESULTS = ADQ / "results"
KEEP = ("run.json", "markers.jsonl", "answers.jsonl", "svc")
ARMS = {k: TRACKA / v for k, v in json.loads((ADQ / "arms.json").read_text()).items()}


def read(path):
    p = Path(path)
    return (
        [json.loads(x) for x in p.read_text().splitlines() if x.strip()]
        if p.exists()
        else []
    )


def write(path, recs):
    Path(path).write_text("".join(json.dumps(r, sort_keys=True) + "\n" for r in recs))


def check(R):
    p = subprocess.run(
        [PY, "-m", "check.check", str(R)], cwd=TRACKA, capture_output=True, text=True
    )
    try:
        out = json.loads(p.stdout)
    except ValueError:
        return {
            "verdict": "checker-crash",
            "fails": [],
            "unresolved": [],
            "err": p.stderr[-2000:],
        }
    by = lambda s: sorted(c["id"] for c in out["clauses"] if c["status"] == s)
    return {
        "verdict": out["verdict"],
        "guarantees": out["guarantees"],
        "fails": by("fail"),
        "unresolved": by("unresolved"),
        "invalid": out.get("invalid", []),
        "clauses": {c["id"]: c["status"] for c in out["clauses"]},
        "details": {
            c["id"]: c.get("detail", "")[:600]
            for c in out["clauses"]
            if c["status"] in ("fail", "unresolved")
        },
    }


def run(arm_dir, name, point, seed, scenario="S-M", env=None):
    p = subprocess.run(
        [PY, "-m", "adequacy.drive", str(arm_dir), name, point, str(seed), scenario],
        cwd=TRACKA,
        capture_output=True,
        text=True,
        env={**os.environ, "TRACKA_RUNS": str(RUNS), **(env or {})},
    )
    try:
        return json.loads(p.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"status": "driver-error", "why": p.stderr[-2000:], "R": None}


def trim(src, dst):
    shutil.rmtree(dst, ignore_errors=True)
    dst.mkdir(parents=True)
    for k in KEEP:
        s = Path(src) / k
        if s.is_dir():
            shutil.copytree(s, dst / k)
        elif s.exists():
            shutil.copy(s, dst / k)


def save(name, data):
    RESULTS.mkdir(exist_ok=True)
    tmp = RESULTS / f".{name}.json.tmp"
    tmp.write_text(json.dumps(data, indent=1) + "\n")
    tmp.rename(RESULTS / f"{name}.json")


def load(name):
    p = RESULTS / f"{name}.json"
    return json.loads(p.read_text()) if p.exists() else None


def pmap(fn, items, n=2):
    from concurrent.futures import ThreadPoolExecutor

    with ThreadPoolExecutor(n) as ex:
        yield from ex.map(fn, items)


def log(*a):
    print(*a, file=sys.stderr, flush=True)
