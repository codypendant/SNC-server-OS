import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

from .pg import BIN
from .run import HERE, MATRIX, REPO, RUN_CAP, RUNS, kill_strays, matrix

PY = sys.executable
SET_CAP = 3 * 3600
RUN_TIMEOUT = RUN_CAP + 300
BUILD_T = 1800
LOG_CAP = 4 << 20
KEEP = {"run.json", "markers.jsonl", "svc", "answers.jsonl", "check.json", "logs"}
FROZEN = ("experiments/tracka", str(MATRIX), "bench/results/dc1-tracka/CRITERIA.md")


def sh(*a):
    return subprocess.run(a, capture_output=True, text=True, cwd=REPO).stdout.strip()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tree_hash(d):
    h = hashlib.sha256()
    for f in sorted(
        set(
            sh(
                "git", "ls-files", "-co", "--exclude-standard", "--", str(d)
            ).splitlines()
        )
    ):
        if (REPO / f).is_file():
            h.update(f.encode() + b"\0" + bytes.fromhex(sha(REPO / f)))
    return h.hexdigest()


def manifest(arm):
    head, want = sh("git", "rev-parse", "HEAD"), os.environ.get("TRACKA_FREEZE_EXPECT")
    dirty = sh("git", "status", "--porcelain", "--", *FROZEN).splitlines()
    if want and (dirty or not head.startswith(want)):
        sys.exit(f"freeze check failed: HEAD {head}, expected {want}, dirty {dirty}")
    return {
        "arm": arm,
        "commit": head,
        "freeze_expect": want,
        "dirty": dirty,
        "arm_tree_sha256": tree_hash(HERE / "arms" / arm),
        "arm_git_tree": sh("git", "rev-parse", f"HEAD:experiments/tracka/arms/{arm}"),
        "harness_tree_sha256": tree_hash(HERE / "tracka"),
        "check_tree_sha256": tree_hash(HERE / "check"),
        "files_sha256": {
            str(p.relative_to(REPO)): sha(p)
            for p in (
                MATRIX,
                HERE / "INTERFACES.md",
                HERE / "policy.json",
                REPO / FROZEN[2],
            )
        },
        "arm_json": json.loads((HERE / "arms" / arm / "arm.json").read_text()),
        "caps_s": {"run": RUN_CAP, "run_set": SET_CAP},
        "versions": {
            "python": sys.version,
            "postgres": subprocess.run(
                [BIN / "postgres", "--version"], capture_output=True, text=True
            ).stdout.strip(),
            "kernel": platform.release(),
            "host": platform.node(),
            "pip": subprocess.run(
                [PY, "-m", "pip", "freeze"], capture_output=True, text=True
            ).stdout.splitlines(),
        },
    }


def rows(path):
    if not path.exists():
        return []
    text = path.read_text()
    if not text.endswith("\n"):
        text = text[: text.rfind("\n") + 1]
        path.write_text(text)
    return [json.loads(x) for x in text.splitlines() if x.strip()]


def append(path, row):
    with open(path, "a") as f:
        f.write(json.dumps(row) + "\n")
        f.flush()
        os.fsync(f.fileno())


def prune(R):
    for p in R.iterdir():
        if p.name not in KEEP:
            shutil.rmtree(p) if p.is_dir() and not p.is_symlink() else p.unlink()
    logs = (
        [f for f in (R / "logs").iterdir() if f.is_file()]
        if (R / "logs").exists()
        else []
    )
    cap = max(64 << 10, LOG_CAP // max(1, len(logs)))
    for f in logs:
        n = f.stat().st_size
        if n > cap:
            with open(f, "rb") as x:
                x.seek(n - cap)
                tail = x.read()
            f.write_bytes(f"[tracka: first {n - cap} bytes cut]\n".encode() + tail)


def build(arm, out):
    d = HERE / "arms" / arm
    argv = json.loads((d / "arm.json").read_text()).get("build")
    if not argv:
        return
    with open(out.with_name(out.name + ".build.log"), "ab") as log:
        try:
            rc = subprocess.run(
                [a.replace("{arm}", str(d)) for a in argv],
                cwd=d,
                stdout=log,
                stderr=log,
                timeout=BUILD_T,
            ).returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    if rc != 0:
        sys.exit(f"build failed: {rc}")


def one(arm, row, left):
    R = RUNS / f"{arm}-{row['point']}-{row['seed']}"
    t = time.time()
    p = subprocess.Popen(
        [PY, "-m", "tracka.run", arm, row["point"], row["seed"]],
        cwd=HERE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        out, err = p.communicate(timeout=min(RUN_TIMEOUT, left))
        harness = json.loads(out.strip().splitlines()[-1])
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        out, err = p.communicate()
        why = (
            "run-set cap" if left < RUN_TIMEOUT else f"run.py exceeded {RUN_TIMEOUT} s"
        )
        harness = {"status": "cap", "why": why}
    except (IndexError, ValueError):
        harness = {"status": "error", "why": err[-2000:]}
    kill_strays(lambda v: v == str(R), f"-D {R}/")
    check = None
    if (HERE / "check" / "check.py").exists() and (R / "run.json").exists():
        c = subprocess.run(
            [PY, "-m", "check.check", str(R)],
            cwd=HERE,
            capture_output=True,
            text=True,
            timeout=600,
        )
        try:
            check = json.loads(c.stdout)
        except ValueError:
            check = {"error": c.stderr[-2000:], "exit": c.returncode}
    if R.exists():
        prune(R)
    verdict = {"error": "invalid", "cap": "unresolved"}.get(harness.get("status"))
    verdict = (
        verdict
        or (check or {}).get("verdict")
        or ("check_error" if check else "unchecked")
    )
    keys = ("point", "kind", "scenario", "boundary", "varies")
    return {
        "arm": arm,
        **{k: row[k] for k in keys},
        "seed": int(row["seed"]),
        "verdict": verdict,
        "harness": harness,
        "check": check,
        "R": str(R),
        "seconds": round(time.time() - t, 1),
    }


def main():
    arm, out, points = sys.argv[1], Path(sys.argv[2]), set(sys.argv[3:])
    man, mf = manifest(arm), out.with_name(out.name + ".manifest.json")
    if mf.exists():
        old = json.loads(mf.read_text())
        started = old.pop("started")
        if old != man:
            sys.exit(f"{out} belongs to a different freeze, arm or harness ({mf})")
    else:
        started = time.time()
        mf.write_text(json.dumps({**man, "started": started}, indent=1))
    done = {}
    for r in rows(out):
        done.setdefault((r["point"], r["seed"]), []).append(r["verdict"])
    prefix = f"{RUNS}/{arm}-"
    kill_strays(lambda v: v.startswith(prefix), f"-D {prefix}")
    build(arm, out)
    for row in matrix():
        if points and row["point"] not in points:
            continue
        vs = done.setdefault((row["point"], int(row["seed"])), [])
        while len(vs) < 2 and all(v == "invalid" for v in vs):
            R = RUNS / f"{arm}-{row['point']}-{row['seed']}"
            if vs and R.exists():
                old = R.with_name(f"{R.name}.invalid{len(vs)}")
                shutil.rmtree(old, ignore_errors=True)
                R.rename(old)
            left = started + SET_CAP - time.time()
            if left > 0:
                res = one(arm, row, left)
            else:
                res = {
                    "arm": arm,
                    "point": row["point"],
                    "seed": int(row["seed"]),
                    "verdict": "unresolved",
                }
                res["harness"] = {"status": "cap", "why": "run-set cap"}
            append(out, res)
            vs.append(res["verdict"])
            print(
                json.dumps(
                    {k: res.get(k) for k in ("point", "seed", "verdict", "seconds")}
                ),
                flush=True,
            )


if __name__ == "__main__":
    main()
