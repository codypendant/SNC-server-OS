import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ.setdefault("AR1_PORT", "25320")

from check import oracle as om  # noqa: E402
from check.oi1e import CLIENT, classes, commit, pair, run_dir  # noqa: E402
from check.oracle import Oracle, _proc_state, lsn_int  # noqa: E402

BLOCKING_CLIENT = """
import sys, time, psycopg
c = psycopg.connect(host="127.0.0.1", port=int(sys.argv[1]), user="ar1", dbname="ar1", autocommit=True,
                    application_name=sys.argv[2])
x = c.execute("SELECT pg_current_xact_id()::text").fetchone()[0]
c.execute("BEGIN")
x = c.execute("INSERT INTO t(tok) VALUES ('blocked') RETURNING pg_current_xact_id()::text").fetchone()[0]
print(x, flush=True)
c.execute("COMMIT")
time.sleep(600)
"""


@unittest.skipIf(os.geteuid() == 0, "needs a non-root user (initdb)")
class Live(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="ar1-check-live-", dir="/tmp"))
        cls.p = pair(cls.root / "pair")

    @classmethod
    def tearDownClass(cls):
        cls.p.destroy()
        shutil.rmtree(cls.root, ignore_errors=True)

    def setUp(self):
        self.p.wait_sync()
        self.R = run_dir(self.root, f"r{time.monotonic_ns()}", self.p)
        self.o = Oracle(self.R, self.p.primary.port, self.p.standby.port, "live-app")

    def tearDown(self):
        self.o.close()

    def cuts(self):
        return [json.loads(x) for x in (self.R / "cuts.jsonl").read_text().splitlines()]

    def test_cut_kinds(self):
        pid = self.p.walreceiver_pid()
        x = commit(self.p.primary.port, "a")
        a = self.o.cut("obs")
        b = self.o.takeover_cut("tk")
        f = self.o.final_cut()
        for c in (a, b, f):
            self.assertEqual(c["invalid"], [])
            self.assertEqual(c["unresolved"], [])
            self.assertEqual(c["standby"]["source"], "walreceiver")
            self.assertLess(c["freeze_ms"], 1000)
            self.assertEqual(c["pid"], pid)
            self.assertEqual(c["standby"]["flushed_lsn"], c["primary"]["flush_lsn"])
        self.assertIn("kill_boundary", b)
        self.assertEqual(self.p.walreceiver_pid(), pid)
        self.assertEqual([c["kind"] for c in self.cuts()], ["cut", "takeover", "final"])
        self.assertEqual(classes(self.R, {"a": x}).classes["a"]["class"], "D")

    def test_pre_resume_cut(self):
        self.o.cut("before")
        self.p.stop_standby()
        try:
            x = commit(self.p.primary.port, "pr", local=True)
            c = self.o.pre_resume_cut()
        finally:
            self.p.resume_standby()
        self.assertEqual(c["invalid"], [])
        self.assertEqual(c["standby"]["source"], "walreceiver")
        self.assertLess(lsn_int(c["standby"]["flushed_lsn"]), lsn_int(c["insert_lsn"]))
        self.assertIsNotNone(x)

    def test_pre_resume_cut_running_walreceiver_invalid(self):
        c = self.o.pre_resume_cut()
        self.assertTrue(any("not stopped" in r for r in c["invalid"]))

    def test_freeze_stops_flush(self):
        f = self.o.freeze("f")
        try:
            r1 = self.o.standby_read()["flushed_lsn"]
            subprocess.run(
                [sys.executable, "-c", CLIENT, str(self.p.primary.port), "x"], timeout=0.5, capture_output=True
            )
        except subprocess.TimeoutExpired:
            pass
        finally:
            r2 = self.o.standby_read()["flushed_lsn"]
            self.o.thaw()
        self.assertEqual(r1, r2)
        self.assertIsNotNone(f["pid"])

    def test_a3_kill_cut_standby_stopped(self):
        p = self.p
        x_on = commit(p.primary.port, "on")
        p.stop_standby()
        cl = subprocess.Popen(
            [sys.executable, "-c", BLOCKING_CLIENT, str(p.primary.port), "live-app-1"],
            stdout=subprocess.PIPE,
            text=True,
        )
        x = cl.stdout.readline().strip()
        time.sleep(0.3)
        rec = self.o.kill_cut("k1", cl.kill, standby_stopped=True)
        cl.wait()
        cl.stdout.close()
        self.assertEqual(rec["invalid"], [])
        p.resume_standby()
        self.o.final_cut()
        ch = classes(self.R, {"on": x_on, "blocked": x})
        self.assertEqual(ch.classes["on"]["class"], "D")
        self.assertEqual(ch.classes["blocked"]["at_cuts"]["k1"], "L")
        self.assertEqual(ch.classes["blocked"]["class"], "R")
        self.assertEqual(ch.unresolved, [])

    def test_kill_cut_with_freeze(self):
        cl = subprocess.Popen(
            [sys.executable, "-c", CLIENT, str(self.p.primary.port), "live-app-2"], stdout=subprocess.PIPE, text=True
        )
        x = cl.stdout.readline().strip()
        rec = self.o.kill_cut("k1", cl.kill)
        cl.wait()
        cl.stdout.close()
        self.assertEqual(rec["invalid"], [])
        kinds = [c["kind"] for c in self.cuts()]
        self.assertEqual(kinds, ["kill", "thaw", "kill_boundary"])
        self.assertLess(self.cuts()[1]["freeze_ms"], 1000)
        self.o.final_cut()
        ch = classes(self.R, {"local": x})
        self.assertIn(ch.classes["local"]["class"], ("D", "R"))

    def test_freeze_over_limit_is_invalid(self):
        old = om.FREEZE_MAX_S
        om.FREEZE_MAX_S = 0.05
        try:
            self.o.kill_cut("k1", lambda: time.sleep(0.2))
        finally:
            om.FREEZE_MAX_S = old
        self.assertTrue(any(c["kind"] == "invalid" and "freeze" in c["reason"] for c in self.cuts()))
        self.assertTrue(self.o.invalid)
        self.assertNotEqual(_proc_state(self.p.walreceiver_pid()), "T")

    def test_reconnect_after_primary_restart(self):
        self.p.primary.restart("fast")
        self.p.wait_sync()
        c = self.o.cut("after-restart")
        self.assertEqual(c["invalid"], [])
        self.assertIsNotNone(lsn_int(c["insert_lsn"]))

    def test_read_failure_is_invalid(self):
        old = om.RETRY_S
        om.RETRY_S = 0.5
        self.p.primary.stop("fast")
        try:
            c = self.o.cut("down")
        finally:
            om.RETRY_S = old
            self.p.primary.start()
        self.assertTrue(any("read failed" in r for r in c["invalid"]))
        self.assertNotEqual(_proc_state(self.o.frozen and self.o.frozen["pid"] or c.get("pid") or 0), "T")

    def test_checker_end_to_end(self):
        with self.p.primary.conn() as c:
            c.execute("CREATE SCHEMA IF NOT EXISTS snc")
            c.execute(
                "CREATE TABLE IF NOT EXISTS snc.commit_token(token text primary key, xid xid8, epoch int,"
                " kind text, name text, agent text)"
            )
            toks = []
            for i, name in enumerate(("C-acc", "I-prep", "I-disp")):
                with c.transaction():
                    c.execute(
                        "INSERT INTO snc.commit_token SELECT %s, pg_current_xact_id(), 1, 'decision', %s, 'A'",
                        (f"e2e{self.R.name}{i}", name),
                    )
                toks.append(f"e2e{self.R.name}{i}")
            rows = c.execute("SELECT token, xid::text, epoch, kind, name FROM snc.commit_token").fetchall()
        commits = [dict(zip(("token", "xid", "epoch", "kind", "name"), r)) for r in rows]
        run = json.loads((self.R / "run.json").read_text())
        run.update(point="A/a4:I-disp", seed=0, mutation=None, scenario="S-M", app_name_prefix="live-app")
        (self.R / "run.json").write_text(json.dumps(run))
        (self.R / "markers.jsonl").write_text(
            json.dumps({"t": om.now(), "event": "reached", "point": "a4:I-disp"}) + "\n"
        )
        (self.R / "answers.jsonl").write_text(
            json.dumps(
                {
                    "t": om.now(),
                    "call": "state",
                    "arg": "A",
                    "answer": {"agent": {"id": "A", "epoch": 1}, "commits": commits},
                }
            )
            + "\n"
        )
        self.o.cut("obs")
        self.o.final_cut()
        r = subprocess.run([sys.executable, "-m", "check.check", str(self.R)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        res = json.loads((self.R / "check.json").read_text())
        for t in toks:
            self.assertEqual(res["classes"][t]["class"], "D")
        self.assertFalse(
            [u for u in res["unresolved"] if "pg_waldump" in u["reason"] or "checker error" in u["reason"]],
            res["unresolved"],
        )
        self.assertEqual(res["clauses"]["1.prefix"], "pass")


if __name__ == "__main__":
    unittest.main()
