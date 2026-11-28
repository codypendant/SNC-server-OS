import json
import os
import shutil
import subprocess
import sys

import pytest

from tracka import batch, jlog
from tracka.run import HERE

RUNS = "/tmp/tracka-smoke"
ENV = {**os.environ, "TRACKA_RUNS": RUNS}
ROWS = [
    ("none", "0", "S-M"),
    ("none", "0", "S-AP"),
    ("A02", "0"),
    ("A07", "1"),
    ("A09", "1"),
    ("A10", "0"),
    ("A11", "1"),
    ("A13", "1"),
    ("A15", "0"),
]


@pytest.fixture(scope="module", autouse=True)
def clean():
    shutil.rmtree(RUNS, ignore_errors=True)
    yield
    shutil.rmtree(RUNS, ignore_errors=True)


QUIESCE_READING = {"answers less than 10 s after the end-state detection"}


def ok(c):
    fails = [x["id"] for x in c["clauses"] if x["status"] in ("fail", "unresolved")]
    return not fails and (
        c["verdict"] == "pass" or set(c.get("invalid", [])) <= QUIESCE_READING
    )


def check(R):
    out = subprocess.run(
        [sys.executable, "-m", "check.check", R],
        cwd=HERE,
        capture_output=True,
        text=True,
    )
    return json.loads(out.stdout)


def strays():
    out = subprocess.run(
        [
            "grep",
            "-ls",
            f"TRACKA_RUN={RUNS}",
            *[f"/proc/{p}/environ" for p in os.listdir("/proc") if p.isdigit()],
        ],
        capture_output=True,
        text=True,
    )
    return out.stdout.split()


@pytest.mark.parametrize("row", ROWS, ids=["-".join(r) for r in ROWS])
def test_ref_row(row):
    out = subprocess.run(
        [sys.executable, "-m", "tracka.run", "ref", *row],
        cwd=HERE,
        env=ENV,
        capture_output=True,
        text=True,
        timeout=400,
    )
    res = json.loads(out.stdout)
    assert res["status"] == "ended", res
    events = [m["event"] for m in jlog.read(f"{res['R']}/markers.jsonl")]
    assert events[0] == "started" and events[-3:] == [
        "end-state",
        "quiesced",
        "run-end",
    ]
    if row[0] != "none":
        assert "reached" in events
    assert ok(check(res["R"]))
    assert not strays()
    assert not os.path.exists(f"{res['R']}/pg/data")


def test_batch_rows_and_resume(tmp_path):
    out = tmp_path / "r.jsonl"
    cmd = [sys.executable, "-m", "tracka.batch", "ref", str(out), "A01"]
    subprocess.run(cmd, cwd=HERE, env=ENV, check=True, capture_output=True, timeout=900)
    rows = [json.loads(x) for x in out.read_text().splitlines()]
    per = {}
    for r in rows:
        assert ok(r["check"]), r["check"]
        per.setdefault(r["seed"], []).append(r["verdict"])
    assert sorted(per) == [0, 1]
    assert all(v == ["pass"] or v == ["invalid", "invalid"] for v in per.values())
    assert json.loads((tmp_path / "r.jsonl.manifest.json").read_text())["arm"] == "ref"
    assert "nothing to build" in (tmp_path / "r.jsonl.build.log").read_text()
    assert sorted(os.listdir(rows[0]["R"])) == sorted(batch.KEEP)
    with open(out, "a") as f:
        f.write('{"point": "A01", "se')
    subprocess.run(cmd, cwd=HERE, env=ENV, check=True, capture_output=True, timeout=60)
    assert out.read_text().count("\n") == len(rows) and out.read_text().endswith("\n")
