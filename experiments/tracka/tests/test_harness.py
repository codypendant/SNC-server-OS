import json
import os
import signal
import socket
import subprocess
import sys
import shutil
import tempfile
import time
from pathlib import Path

from tracka import batch
from tracka.pg import PG
from tracka.run import (
    HELD,
    HERE,
    PORTS,
    free_ports,
    group_alive,
    kill_strays,
    killgrp,
    matrix,
)

SLEEPER = (
    "import os, sys, time; os.fork() if sys.argv[1] == 'fork' else None; time.sleep(60)"
)


def alive(pid):
    try:
        return open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()[0] != "Z"
    except FileNotFoundError:
        return False


def test_matrix_frozen_counts():
    rows = matrix()
    assert len(rows) == 114 and len({r["point"] for r in rows}) == 17
    assert sum(r["scenario"] == "S-M" for r in rows) == 94


def test_rows_cuts_partial_last_line(tmp_path):
    p = tmp_path / "r.jsonl"
    p.write_text('{"a": 1}\n{"a": 2}\n{"a": ')
    assert batch.rows(p) == [{"a": 1}, {"a": 2}]
    assert p.read_text() == '{"a": 1}\n{"a": 2}\n'
    batch.append(p, {"a": 3})
    assert [r["a"] for r in batch.rows(p)] == [1, 2, 3]


def test_free_ports_reserved_across_processes():
    code = "import time; from tracka.run import free_ports; print(free_ports(), flush=True); time.sleep(3)"
    ps = [
        subprocess.Popen(
            [sys.executable, "-c", code], cwd=HERE, stdout=subprocess.PIPE, text=True
        )
        for _ in range(8)
    ]
    bases = [int(p.stdout.readline()) for p in ps] + [free_ports() for _ in range(20)]
    for p in ps:
        p.kill()
        p.wait()
    assert len(set(bases)) == len(bases) and all(
        b % PORTS == 10000 % PORTS for b in bases
    )


def test_free_ports_skips_bound_port():
    b = free_ports()
    HELD.pop().close()
    with socket.socket() as s:
        s.bind(("127.0.0.1", b + 5))
        assert b not in [free_ports() for _ in range(300)]


def test_killgrp_kills_children():
    p = subprocess.Popen(
        [sys.executable, "-c", SLEEPER, "fork"], start_new_session=True
    )
    time.sleep(0.3)
    assert group_alive(p.pid)
    killgrp(p)
    assert not group_alive(p.pid)


def test_kill_strays_by_env_catches_escaped_daemon(tmp_path):
    R = str(tmp_path / "ref-A01-0")
    code = "import os, time; os.setsid(); time.sleep(60)"
    p = subprocess.Popen(
        [sys.executable, "-c", code], env={**os.environ, "TRACKA_RUN": R}
    )
    q = subprocess.Popen(
        [sys.executable, "-c", code], env={**os.environ, "TRACKA_RUN": R + "0"}
    )
    time.sleep(0.3)
    kill_strays(lambda v: v == R, f"-D {R}/")
    p.wait(5)
    assert p.returncode == -signal.SIGKILL and q.poll() is None
    q.kill()
    q.wait()


def test_pg_settings_restart():
    root = tempfile.mkdtemp(prefix="tracka-pgtest-")
    os.chmod(root, 0o755)
    pg = PG(Path(root) / "pg", free_ports(), dict(os.environ))
    pg.create()
    try:
        sql = "show fsync; show synchronous_commit; select current_database()"
        q = ["/opt/ar1/bin/psql", pg.url, "-tAc", sql]
        assert subprocess.run(q, capture_output=True, text=True).stdout.split() == [
            "on",
            "on",
            "tracka",
        ]
        pg.stop("fast")
        assert subprocess.run(q, capture_output=True).returncode != 0
        pg.start()
        assert subprocess.run(q, capture_output=True).returncode == 0
    finally:
        pg.destroy()
    assert not (Path(root) / "pg" / "data").exists()
    shutil.rmtree(root)


def test_manifest_hashes_arm_tree():
    m = batch.manifest("ref")
    assert len(m["arm_tree_sha256"]) == 64 and m["commit"]
    assert json.loads(json.dumps(m)) == m


def test_prune_keeps_evidence_and_caps_logs(tmp_path):
    R = tmp_path / "R"
    for d in ("svc", "logs", "ctl", "engine/data"):
        (R / d).mkdir(parents=True)
    for f in (
        "run.json",
        "markers.jsonl",
        "answers.jsonl",
        "check.json",
        "svc/T-I.jsonl",
        "ctl/x",
        "engine/data/y",
    ):
        (R / f).write_text("x\n")
    (R / "logs" / "w1.log").write_bytes(b"a" * (9 << 20) + b"END\n")
    (R / "logs" / "w2.log").write_text("small\n")
    batch.prune(R)
    assert sorted(p.name for p in R.iterdir()) == sorted(batch.KEEP)
    assert (R / "svc" / "T-I.jsonl").read_text() == "x\n"
    big = (R / "logs" / "w1.log").read_bytes()
    assert big.startswith(b"[tracka: first") and big.endswith(b"END\n")
    assert (
        sum(f.stat().st_size for f in R.rglob("*") if f.is_file())
        <= batch.LOG_CAP + 4096
    )
    assert (R / "logs" / "w2.log").read_text() == "small\n"
