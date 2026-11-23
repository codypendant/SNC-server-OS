"""Performance runs (prereg 8, latency budget B): python -m ar1.perf <arm,arm> <tasks per arm> <out.jsonl>.

Tasks alternate between the arms (A B A B), jitter off, stub at 500 ms, no fault points. One primary and standby
pair serves all tasks; each task gets fresh databases and its own run directory.
"""

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from . import jlog, pg
from .harness import ARMS, HERE, PY, RUNS

HOOK = Path(__file__).resolve().parent / "perfhook"
STUB_MS = 500


def reset_dbs(port):
    with pg.connect(port, pg.ADMIN, "postgres") as c:
        for db in ("ar1", "ar1_sys_e1", "ar1_sys_e2"):
            c.execute(f"DROP DATABASE IF EXISTS {db} WITH (FORCE)")
            c.execute(f"CREATE DATABASE {db} OWNER {pg.RUNTIME}")


def lsn_int(s):
    hi, lo = s.split("/")
    return (int(hi, 16) << 32) + int(lo, 16)


def window(pair, start, end):
    """Commit records and WAL bytes between the task's start and end insert LSNs."""
    r = subprocess.run(
        [str(pg.BIN / "pg_waldump"), "-p", str(pair.primary.wal_dir()), "-s", start, "-e", end, "--rmgr=Transaction"],
        capture_output=True,
        text=True,
    )
    return r.stdout.count("desc: COMMIT"), lsn_int(end) - lsn_int(start)


def task(pair, arm, i):
    R = RUNS / f"perf.{arm}.{i}"
    shutil.rmtree(R, ignore_errors=True)
    (R / "ctl").mkdir(parents=True)
    (R / "svc").mkdir()
    reset_dbs(pair.primary.port)
    run = {
        "point": "perf",
        "seed": i,
        "arm": arm,
        "mutation": "",
        "scenario": "S-M",
        "svc_port": pair.primary.port + 10,
        "stub_latency_ms": STUB_MS,
        "app_name_prefix": "ar1-",
        "policy_version": json.loads((HERE / "policy.json").read_text())["version"],
        "release": "after",
        "pg": {
            "bin": str(pg.BIN),
            "primary_port": pair.primary.port,
            "standby_port": pair.standby.port,
            "primary_data": str(pair.primary.data),
            "standby_data": str(pair.standby.data),
        },
    }
    (R / "run.json").write_text(json.dumps(run))
    env = {
        **os.environ,
        "AR1_RUN": str(R),
        "AR1_SEED": str(i),
        "AR1_POINTS": "",
        "AR1_MUTATION": "",
        "PYTHONPATH": str(HERE),
        "DBOS__APPVERSION": f"ar1-{arm}-1",
    }
    pkg = ARMS[arm]
    subprocess.run(
        [PY, "-c", f"from pathlib import Path; from {pkg} import rt; rt.setup_db(Path('{R}') / 'blobs')"],
        env={**env, "AR1_EXECUTOR": "setup"},
        check=True,
        cwd=HERE,
        capture_output=True,
    )
    svc = subprocess.Popen(
        [PY, "-m", "ar1.services", str(R)],
        env={**env, "AR1_EXECUTOR": "svc"},
        cwd=HERE,
        stdout=open(R / "svc.log", "w"),
        stderr=subprocess.STDOUT,
    )
    try:
        end = time.time() + 30
        while not (R / "ctl" / "svc-ready").exists():
            if time.time() > end:
                raise RuntimeError("receivers did not start")
            time.sleep(0.01)
        app = subprocess.run(
            [PY, "-m", f"{pkg}.app", "start", "1"],
            env={**env, "AR1_EXECUTOR": "e1", "PYTHONPATH": f"{HOOK}:{HERE}"},
            cwd=HERE,
            capture_output=True,
            text=True,
            timeout=300,
        )
    finally:
        svc.kill()
        svc.wait()
    result = (R / "ctl" / "result-e1").read_text() if (R / "ctl" / "result-e1").exists() else None
    ev = jlog.read(R / "perf-client.jsonl")
    t0 = next((e["t"] for e in ev if e["kind"] == "task-start"), None)
    t1 = next((e["t"] for e in ev if e["kind"] == "task-end"), None)
    http = sum(e["ns"] for e in ev if e["kind"] == "http")
    s0 = next((e["lsn"] for e in ev if e["kind"] == "task-start"), None)
    s1 = next((e["lsn"] for e in ev if e["kind"] == "task-end"), None)
    commits, wal_bytes = window(pair, s0, s1) if s0 and s1 else (None, None)
    out = {
        "arm": arm,
        "i": i,
        "result": result,
        "rc": app.returncode,
        "task_ns": t1 - t0 if t0 and t1 else None,
        "http_ns": http,
        "http_requests": sum(e["kind"] == "http" for e in ev),
        "net_ns": t1 - t0 - http if t0 and t1 else None,
        "commits": commits,
        "wal_bytes": wal_bytes,
        "blob_bytes": sum(f.stat().st_size for f in (R / "blobs").iterdir()) if (R / "blobs").exists() else 0,
    }
    shutil.rmtree(R, ignore_errors=True)
    return out


def main():
    arms, n, out = sys.argv[1].split(","), int(sys.argv[2]), Path(sys.argv[3])
    done = {(r["arm"], r["i"]) for r in map(json.loads, out.read_text().splitlines())} if out.exists() else set()
    pair = pg.Pair(RUNS / "perf-pg")
    pair.create()
    try:
        for i in range(n):
            for arm in arms:
                if (arm, i) in done:
                    continue
                with open(out, "a") as f:
                    f.write(json.dumps(task(pair, arm, i)) + "\n")
    finally:
        pair.destroy()


if __name__ == "__main__":
    main()
