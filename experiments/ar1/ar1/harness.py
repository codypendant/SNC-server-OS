"""Runs one matrix point: python -m ar1.harness <point-id> <seed> [mutation] [arm]. Prints the run's check summary."""

import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from check.oracle import Oracle

from . import jlog, matrix, pg

PY = sys.executable
HERE = Path(__file__).resolve().parent.parent
RUNS = Path(os.environ.get("AR1_RUNS", "/tmp/ar1-runs"))
HOLD_LIMIT = 10.0
RUN_CAP = 300.0
ARMS = {"snc": "ar1", "plain1": "plain1", "plain2": "plain2"}

TN_HOLD = 2.0 + 2.0 + 10.0 + 5.0


class Unresolved(Exception):
    pass


class Run:
    def __init__(self, point, seed, mutation="", arm="snc"):
        self.p = next(x for x in matrix.points() if x["id"] == point)
        self.seed, self.mutation, self.arm = seed, mutation, arm
        self.pkg = ARMS[arm]
        self.probes = json.loads((HERE / self.pkg / "probes.json").read_text())
        self.R = RUNS / f"{point.replace('/', '_').replace(':', '-')}.{seed}.{mutation or 'none'}.{arm}"
        shutil.rmtree(self.R, ignore_errors=True)
        (self.R / "ctl").mkdir(parents=True)
        (self.R / "svc").mkdir()
        self.pair = pg.Pair(self.R / "pg")
        self.procs = {}
        self.deadline = time.time() + RUN_CAP
        self.mpos = 0
        self.release_order = matrix.release(point, seed)
        self.t0 = time.time()
        self.threads = []

    # ---- plumbing ----

    def env(self, executor, points=()):
        return {
            **os.environ,
            "AR1_RUN": str(self.R),
            "AR1_EXECUTOR": executor,
            "AR1_SEED": str(self.seed),
            "AR1_POINTS": ",".join(points),
            "AR1_MUTATION": self.mutation,
            "PYTHONPATH": str(HERE),
            "DBOS__APPVERSION": f"ar1-{self.arm}-1",
        }

    def spawn(self, executor, mode, epoch, points=()):
        log = open(self.R / f"app-{executor}.log", "a")
        p = subprocess.Popen(
            [PY, "-m", f"{self.pkg}.app", mode, str(epoch)],
            env=self.env(executor, points),
            stdout=log,
            stderr=subprocess.STDOUT,
            cwd=HERE,
        )
        self.procs[executor] = p
        return p

    def ctl(self, name):
        (self.R / "ctl" / name).write_text("1")

    def mark(self, event, point=None, **data):
        jlog.append(self.R / "markers.jsonl", {"event": event, "point": point, "data": data})

    def wait_marker(self, pred, limit=None):
        end = time.time() + (limit or (self.deadline - time.time()))
        while time.time() < end:
            ms = jlog.read(self.R / "markers.jsonl")
            for m in ms[self.mpos :]:
                self.mpos += 1
                if pred(m):
                    return m
            if self.procs and not self.working():
                ms = jlog.read(self.R / "markers.jsonl")
                if len(ms) == self.mpos:
                    return None
            time.sleep(0.002)
        return None

    def working(self):
        idle = not (self.R / "ctl" / "go-takeover").exists()
        return [n for n, p in self.procs.items() if p.poll() is None and not (n == "e2" and idle)]

    def reached(self, point, limit=None):
        m = self.wait_marker(lambda m: m["event"] == "reached" and m["point"] == point, limit)
        if not m:
            raise Unresolved(f"point {point} not reached")
        return m

    def pid_of(self, point):
        return int((self.R / "ctl" / f"fired-{point}").read_text())

    def executor_of(self, pid):
        return next(e for e, p in self.procs.items() if p.pid == pid)

    def kill(self, executor):
        os.kill(self.procs[executor].pid, signal.SIGKILL)
        self.procs[executor].wait()
        self.mark("killed", None, executor=executor)

    def answer(self, call, arg):
        r = subprocess.run(
            [PY, "-m", f"{self.pkg}.iface", str(self.pair.primary.port), call, arg],
            env=self.env("iface"),
            capture_output=True,
            text=True,
            cwd=HERE,
            timeout=120,
        )
        ans = json.loads(r.stdout) if r.returncode == 0 and r.stdout.strip() else {"error": r.stderr[-2000:]}
        jlog.append(self.R / "answers.jsonl", {"call": call, "arg": arg, "answer": ans})
        return ans

    def first_answer(self, standby_stopped=False, started=None):
        """Item 1 observation (prereg 7.1): answer, then a cut. At a3 and E the standby stays stopped until the
        answer or 5 s after recovery starts."""
        self.mark("answer-requested")
        if standby_stopped:
            proc = subprocess.Popen(
                [PY, "-m", f"{self.pkg}.iface", str(self.pair.primary.port), "state", "A"],
                env=self.env("iface"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                cwd=HERE,
            )
            end = (started or time.time()) + 5
            while proc.poll() is None and time.time() < end:
                time.sleep(0.005)
            if proc.poll() is not None:
                self.mark("answer-returned")
            self.oracle.pre_resume_cut()
            self.pair.resume_standby()
            self.mark("standby-resumed")
            out, err = proc.communicate(timeout=120)
            ans = json.loads(out) if proc.returncode == 0 and out.strip() else {"error": err[-2000:]}
            jlog.append(self.R / "answers.jsonl", {"call": "state", "arg": "A", "answer": ans})
        else:
            self.answer("state", "A")
        self.oracle.cut("item1")

    def wait_sync_rep(self, prefix):
        end = time.time() + 10
        while time.time() < end:
            with pg.connect(self.pair.primary.port, pg.ADMIN, "postgres") as c:
                n = c.execute(
                    "SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE %s"
                    " AND application_name NOT LIKE '%%-sys' AND wait_event='SyncRep'",
                    (prefix + "%",),
                ).fetchone()[0]
            if n:
                return
            time.sleep(0.005)
        raise Unresolved("COMMIT never blocked on the stopped standby")

    def wait_lock(self, prefix):
        end = time.time() + 10
        while time.time() < end:
            with pg.connect(self.pair.primary.port, pg.ADMIN, "postgres") as c:
                n = c.execute(
                    "SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE %s"
                    " AND wait_event_type='Lock'",
                    (prefix + "%",),
                ).fetchone()[0]
            if n:
                return
            time.sleep(0.005)
        raise Unresolved("takeover never waited on the agent row lock")

    def cancel(self):
        subprocess.run(
            [PY, "-c", f"from {self.pkg} import rt; rt.cancel()"],
            env=self.env("cancel"),
            check=True,
            cwd=HERE,
            capture_output=True,
            timeout=60,
        )
        self.mark("cancelled")

    def wait_done(self):
        for e, p in list(self.procs.items()):
            try:
                p.wait(timeout=max(1, self.deadline - time.time()))
            except subprocess.TimeoutExpired:
                raise Unresolved(f"run cap: {e} still running")

    # ---- schedules ----

    def setup(self):
        self.pair.create()
        run = {
            "point": self.p["id"],
            "seed": self.seed,
            "arm": self.arm,
            "mutation": self.mutation,
            "scenario": self.p["scenario"],
            "svc_port": self.pair.primary.port + 10,
            "stub_latency_ms": 20,
            "app_name_prefix": "ar1-",
            "policy_version": json.loads((HERE / "policy.json").read_text())["version"],
            "release": self.release_order,
            "pg": {
                "bin": str(pg.BIN),
                "primary_port": self.pair.primary.port,
                "standby_port": self.pair.standby.port,
                "primary_data": str(self.pair.primary.data),
                "standby_data": str(self.pair.standby.data),
            },
        }
        (self.R / "run.json").write_text(json.dumps(run, indent=1))
        subprocess.run(
            [
                PY,
                "-c",
                f"import os; from pathlib import Path; from {self.pkg} import rt;"
                f" rt.setup_db(Path('{self.R}') / 'blobs')",
            ],
            env=self.env("setup"),
            check=True,
            cwd=HERE,
            capture_output=True,
        )
        self.svc = subprocess.Popen(
            [PY, "-m", "ar1.services", str(self.R)],
            env=self.env("svc"),
            cwd=HERE,
            stdout=open(self.R / "svc.log", "w"),
            stderr=subprocess.STDOUT,
        )
        if not self.wait_until(lambda: (self.R / "ctl" / "svc-ready").exists(), 30):
            raise Unresolved("receivers did not start")
        self.oracle = Oracle(self.R, self.pair.primary.port, self.pair.standby.port, "ar1-")

    def hold(self, at):
        if at.startswith("b2:"):
            self.ctl(f"hold-{at[3:]}")

    def kill_and_recover(self, at, executor="e1", epoch=1, standby_stopped=False, second=None):
        ex = self.executor_of(self.pid_of(at)) if not at.startswith("b2:") else executor
        self.oracle.kill_cut(at, lambda: self.kill(ex), standby_stopped=standby_stopped)
        if at.startswith("b2:"):
            self.release_after_next_request(at)
        started = time.time()
        self.spawn(ex, "recover", epoch if ex == "e1" else 2, [second] if second else [])
        if second:
            self.reached(second)
            self.kill_and_recover(second, ex, epoch)
            return
        self.first_answer(standby_stopped, started)

    def release(self, at, who):
        self.ctl(f"release-{at}")
        self.mark("released", at, who=who)

    def release_after_next_request(self, at):
        """b2 kill: the request stays in flight until the recovered process contacts that receiver, or 10 s."""
        log = self.R / "svc" / f"{'stub' if at == 'b2:model' else at[3:]}.jsonl"
        n = len(jlog.read(log))

        def go():
            end = time.time() + HOLD_LIMIT
            while time.time() < end and len(jlog.read(log)) == n:
                time.sleep(0.01)
            self.release(at, "old")

        t = threading.Thread(target=go, daemon=True)
        t.start()
        self.threads.append(t)

    def run_A_B(self):
        at = self.p["at"]
        self.hold(at)
        a3 = at.startswith("a3:")
        self.spawn("e1", "start", 1, [at])
        self.reached(at)
        if a3:
            self.pair.stop_standby()
            self.mark("standby-stopped")
            self.ctl(f"release-{at}")
            self.wait_sync_rep("ar1-e1")
        self.kill_and_recover(at, standby_stopped=a3)

    def run_E(self):
        at = self.p["at"]
        self.spawn("e1", "start", 1, [at])
        self.reached(at)
        self.pair.stop_standby()
        self.mark("standby-stopped")
        self.ctl(f"release-{at}")
        self.wait_sync_rep("ar1-e1")
        mode = self.p["mode"]

        def restart():
            self.pair.primary.stop(mode)
            self.pair.primary.start()
            self.mark("restarted", None, mode=mode)

        self.oracle.kill_cut(at, restart, standby_stopped=True)
        self.first_answer(True, time.time())

    def takeover_start(self, points):
        self.spawn("e2", "takeover", 2, points)
        while not (self.R / "ctl" / "ready-e2").exists():
            if self.procs["e2"].poll() is not None:
                raise Unresolved("takeover process died")
            time.sleep(0.005)

    def go_takeover(self):
        self.oracle.takeover_cut("takeover")
        self.ctl("go-takeover")

    def run_C(self):
        kind, at = self.p["kind"], self.p["at"]
        self.hold(at)
        held_new = kind in ("C-p", "C-s") or (kind == "C-b2" and self.release_order != "before")
        self.spawn("e1", "start", 1, [at, "stale-decision"] if kind == "C-s" else [at])
        self.takeover_start(["post-takeover"] if held_new else [])
        self.reached(at)
        if kind == "C-a1":
            self.go_takeover()
            self.wait_lock("ar1-e2")
            self.mark("takeover-waiting")
            self.release(at, "old")
            self.wait_takeover_visible()
            self.mark("takeover-committed")
        elif kind == "C-b2":
            if not held_new:
                self.receiver_release(at)
                self.mark("released", at, who="old")
                self.go_takeover()
                self.wait_takeover_visible()
                self.mark("takeover-committed")
            else:
                self.c_b2_after(at)
        else:
            self.go_takeover()
            self.reached("post-takeover", HOLD_LIMIT)
            self.mark("takeover-committed")
            self.release(at, "old")
            if kind == "C-s":
                m = self.wait_marker(
                    lambda m: m["event"] in ("stale-decision-rejected", "stale-decision-committed"), HOLD_LIMIT
                )
                if not m:
                    raise Unresolved("stale-decision not reached within the hold limit")
            else:
                try:
                    self.procs["e1"].wait(timeout=HOLD_LIMIT)
                except subprocess.TimeoutExpired:
                    raise Unresolved("old activation did not stop within the hold limit")
            self.release("post-takeover", "new")
        self.first_answer()

    def c_b2_after(self, at):
        """The old activation is paused while its reply is in flight, so no client timeout decides the order."""
        svc = at[3:]
        outcome = svc in ("model", "T-I") or self.release_order == "after-outcome"
        old = self.procs["e1"]
        os.kill(old.pid, signal.SIGSTOP)
        self.mark("old-paused")
        if outcome:
            self.receiver_release(at)
        self.go_takeover()
        self.reached("post-takeover", HOLD_LIMIT)
        self.mark("takeover-committed")
        if not outcome:
            self.release("post-takeover", "new")
            if svc == "T-F":
                ok = self.wait_until(
                    lambda: any(
                        e.get("kind") == "request" and e.get("epoch") == 2
                        for e in jlog.read(self.R / "svc" / "T-F.jsonl")
                    ),
                    HOLD_LIMIT,
                )
            else:
                ok = self.wait_until(self.new_tn_dispatch, TN_HOLD)
            if not ok:
                raise Unresolved(f"new activation never reached {svc} before the old reply")
            self.receiver_release(at)
        os.kill(old.pid, signal.SIGCONT)
        self.mark("released", at, who="old")
        try:
            old.wait(timeout=HOLD_LIMIT)
        except subprocess.TimeoutExpired:
            raise Unresolved("old activation still running after its reply")
        if outcome:
            self.release("post-takeover", "new")

    def receiver_release(self, at):
        log = self.R / "svc" / f"{'stub' if at == 'b2:model' else at[3:]}.jsonl"
        n = sum(e.get("kind") == "delivered" for e in jlog.read(log))
        self.ctl(f"release-{at}")
        self.mark("receiver-released", at)
        if not self.wait_until(lambda: sum(e.get("kind") == "delivered" for e in jlog.read(log)) > n, HOLD_LIMIT):
            raise Unresolved(f"held reply at {at} never delivered")

    def wait_until(self, pred, limit):
        end = time.time() + limit
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.01)
        return False

    def new_tn_dispatch(self):
        with self.pair.primary.conn() as c:
            return bool(c.execute(self.probes["new_tn_dispatch"]).fetchone())

    def cancel_syncrep(self, stop):
        """Prereg C-2: cancelling a SyncRep wait leaves the commit local and returns success to the client."""
        done = set()
        with pg.connect(self.pair.primary.port, pg.ADMIN, "postgres") as c:
            while not stop.is_set():
                for pid, sc in c.execute(
                    "SELECT pid, state_change::text FROM pg_stat_activity"
                    " WHERE (application_name LIKE 'ar1-e1%%' OR datname = 'ar1_sys_e1') AND wait_event='SyncRep'"
                ).fetchall():
                    if (pid, sc) not in done:
                        done.add((pid, sc))
                        c.execute("SELECT pg_cancel_backend(%s)", (pid,))
                time.sleep(0.005)
        self.mark("syncrep-cancel-off", None, cancelled=len(done))

    def unestablished(self, at, setup, resume_when):
        self.spawn("e1", "start", 1, [at])
        self.reached(at)
        self.pair.stop_standby()
        self.mark("standby-stopped")
        setup()
        stop = threading.Event()
        t = threading.Thread(target=self.cancel_syncrep, args=(stop,), daemon=True)
        t.start()
        self.mark("syncrep-cancel-on")
        self.ctl(f"release-{at}")
        self.wait_until(resume_when, 3.0)
        stop.set()
        t.join()
        self.oracle.pre_resume_cut()
        self.pair.resume_standby()
        self.mark("standby-resumed")

    def run_Ac(self):
        svc = {"I-disp": "stub", "TI-disp": "T-I", "TF-disp": "T-F", "TN-disp": "T-N"}[self.p["at"][3:]]
        log = self.R / "svc" / f"{svc}.jsonl"
        self.unestablished(self.p["at"], lambda: None, lambda: any(e.get("kind") == "request" for e in jlog.read(log)))
        self.wait_done()
        self.first_answer()

    def run_P(self):
        def inject():
            with pg.connect(self.pair.primary.port, pg.RUNTIME, pg.DB, app="ar1-inject") as c:
                with c.transaction():
                    c.execute("SET LOCAL synchronous_commit TO local")
                    c.execute(self.probes["inject_memory"])
                    x = c.execute(self.probes["inject_token"]).fetchone()[0]
            self.mark("injected", None, xid=x)

        def p_dec():
            with self.pair.primary.conn() as c:
                return bool(c.execute(self.probes["policy_committed"]).fetchone())

        self.unestablished("pre-policy", inject, p_dec)
        self.wait_done()
        self.first_answer()

    def run_D(self):
        at = self.p["at"]
        self.hold(at)
        self.spawn("e1", "start", 1, [at])
        self.reached(at)
        self.cancel()
        self.release(at, "old")

    def run_R_two(self):
        at = self.p["at"]
        self.hold(at)
        self.spawn("e1", "start", 1, [at])
        self.reached(at)
        self.kill_and_recover(at, second=self.p["second"])

    def run_R_takeover(self):
        at = self.p["at"]
        self.hold(at)
        self.spawn("e1", "start", 1, [at])
        self.takeover_start(["a1:TAKEOVER"])
        self.reached(at)
        self.go_takeover()
        self.reached("a1:TAKEOVER")
        self.kill_and_recover("a1:TAKEOVER", "e2", 2)
        self.wait_takeover_visible()
        self.release(at, "old")

    def wait_takeover_visible(self):
        end = time.time() + HOLD_LIMIT
        while time.time() < end:
            with self.pair.primary.conn() as c:
                if c.execute(self.probes["takeover_committed"]).fetchone():
                    return
            time.sleep(0.01)
        raise Unresolved("takeover not committed after recovery")

    def execute(self):
        result = {"point": self.p["id"], "seed": self.seed, "mutation": self.mutation, "arm": self.arm}
        try:
            self.setup()
            {
                "A": self.run_A_B,
                "B": self.run_A_B,
                "E": self.run_E,
                "C-a1": self.run_C,
                "C-b2": self.run_C,
                "C-p": self.run_C,
                "C-s": self.run_C,
                "D": self.run_D,
                "R-two": self.run_R_two,
                "R-takeover": self.run_R_takeover,
                "Ac": self.run_Ac,
                "P": self.run_P,
            }[self.p["kind"]]()
            self.wait_done()
            for t in self.threads:
                t.join()
            self.oracle.final_cut()
            eff = self.answer("effects", "A")
            for e in eff.get("effects", []):
                self.answer("reconstruct", e)
            self.answer("state", "A")
            if self.p["kind"] == "D":
                self.oracle.cut("after-state")
            r = subprocess.run(
                [PY, "-m", "check.check", str(self.R)],
                env=self.env("check"),
                cwd=HERE,
                capture_output=True,
                text=True,
                timeout=300,
            )
            result["check"] = (
                json.loads((self.R / "check.json").read_text())
                if (self.R / "check.json").exists()
                else {"error": r.stderr[-3000:]}
            )
            if time.time() - self.t0 > RUN_CAP:
                result["unresolved"] = f"run cap: {time.time() - self.t0:.0f} s from setup to check"
        except Unresolved as e:
            if self.arm != "snc" and str(e).endswith("not reached"):
                result["missing_point"] = str(e).split()[1]
            else:
                result["unresolved"] = str(e)
        except Exception as e:
            result["harness_error"] = repr(e)
        finally:
            result["results"] = {p.name: p.read_text() for p in (self.R / "ctl").glob("result-*")}
            result["seconds"] = round(time.time() - self.t0, 1)
            self.teardown()
        return result

    def teardown(self):
        for p in list(self.procs.values()) + [getattr(self, "svc", None)]:
            if p and p.poll() is None:
                p.kill()
                p.wait()
        try:
            self.oracle.close()
        except Exception:
            pass
        self.pair.destroy()
        if not os.environ.get("AR1_KEEP"):
            shutil.rmtree(self.R, ignore_errors=True)


def main():
    point, seed = sys.argv[1], int(sys.argv[2])
    mutation = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != "none" else ""
    arm = sys.argv[4] if len(sys.argv) > 4 else "snc"
    print(json.dumps(Run(point, seed, mutation, arm).execute(), default=str))


if __name__ == "__main__":
    main()
