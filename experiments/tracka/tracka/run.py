import csv
import fcntl
import json
import os
import random
import select
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import jlog
from .pg import PG

HERE = Path(__file__).resolve().parent.parent
REPO = HERE.parent.parent
MATRIX = REPO / "docs" / "DIRECTION_COMPARISON_TRACK_A_MATRIX.csv"
RUNS = Path(os.environ.get("TRACKA_RUNS", "/tmp/tracka-runs"))
FROZEN = "8a0f787304e6b9df2fdf3a2a93f24ab83f34db84"
RUN_CAP, QUIESCE, TAKEOVER_T, POLL, CMD_T = 300, 10, 60, 2, 60
MAX_WORKERS = 20
SVC = {
    "model": "stub",
    "T-I": "T-I",
    "T-F": "T-F",
    "T-N": "T-N",
    "approver": "approver",
}
END = ("done", "denied", "stopped_in_doubt")
PORTS = 22
LOCKS = Path("/tmp/tracka-ports")
HELD = []


class Cap(Exception):
    pass


def matrix():
    with open(MATRIX) as f:
        return list(csv.DictReader(f))


def free_ports():
    LOCKS.mkdir(exist_ok=True)
    os.chmod(LOCKS, 0o1777)
    blocks = list(range(10000, 32700 - PORTS, PORTS))
    random.shuffle(blocks)
    for base in blocks:
        f = open(LOCKS / str(base), "a")
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for p in range(base, base + PORTS):
                with socket.socket() as s:
                    s.bind(("127.0.0.1", p))
        except OSError:
            f.close()
            continue
        HELD.append(f)
        return base
    raise RuntimeError("no free port block")


def group_alive(pgid):
    for d in os.listdir("/proc"):
        try:
            f = open(f"/proc/{d}/stat").read().rsplit(")", 1)[1].split()
        except (OSError, IndexError):
            continue
        if int(f[2]) == pgid and f[0] != "Z":
            return True
    return False


def killgrp(p, sig=signal.SIGKILL):
    try:
        os.killpg(p.pid, sig)
    except ProcessLookupError:
        return
    if sig == signal.SIGKILL:
        p.wait()
        end = time.monotonic() + 5
        while group_alive(p.pid) and time.monotonic() < end:
            time.sleep(0.01)


def kill_strays(match, pattern):
    me = os.getpid()
    for d in os.listdir("/proc"):
        if not d.isdigit() or int(d) == me:
            continue
        try:
            env = open(f"/proc/{d}/environ", "rb").read().split(b"\0")
        except OSError:
            continue
        if any(e.startswith(b"TRACKA_RUN=") and match(e[11:].decode()) for e in env):
            try:
                os.kill(int(d), signal.SIGKILL)
            except ProcessLookupError:
                pass
    subprocess.run(["pkill", "-9", "-f", "--", pattern], capture_output=True)


class Run:
    def __init__(self, arm, point, seed, scenario="S-M"):
        self.t0 = time.monotonic()
        self.deadline = self.t0 + RUN_CAP
        self.dir = HERE / "arms" / arm
        self.cfg = json.loads((self.dir / "arm.json").read_text())
        if point == "none":
            self.row = {
                "point": "none",
                "kind": "none",
                "scenario": scenario,
                "boundary": "none",
                "varies": "none",
            }
            name = f"{arm}-none-{scenario}-{seed}"
        else:
            self.row = next(
                r for r in matrix() if r["point"] == point and int(r["seed"]) == seed
            )
            name = f"{arm}-{point}-{seed}"
        self.arm, self.seed, self.R = arm, seed, RUNS / name
        self.workers, self.engines, self.bg, self.threads = [], {}, {}, []
        self.reached, self.buf, self.status, self.arm_id = [], b"", None, 0
        self.poll_at, self.poller, self.killed = 0, None, set()
        self.posted, self.post_lock = set(), threading.Lock()
        self.svc = self.pg = None
        self.notes = []

    def mark(self, event, **data):
        jlog.append(self.R / "markers.jsonl", {"event": event, "data": data})

    def env(self, worker):
        return {**os.environ, **self.base_env, "TRACKA_WORKER": worker}

    def cmd(self, argv):
        return [a.replace("{arm}", str(self.dir)) for a in argv]

    def spawn(self, name, argv, worker, **kw):
        with open(self.R / "logs" / f"{name}.log", "ab") as log:
            return subprocess.Popen(
                self.cmd(argv),
                cwd=self.dir,
                env=self.env(worker),
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                **kw,
            )

    def oneshot(self, name, args=(), timeout=CMD_T):
        out = self.R / "ctl" / f"{name}-{threading.get_ident()}.out"
        with open(out, "wb") as f, open(self.R / "logs" / f"{name}.log", "ab") as log:
            p = subprocess.Popen(
                self.cmd(self.cfg[name]) + list(args),
                cwd=self.dir,
                env=self.env(name),
                stdout=f,
                stderr=log,
                start_new_session=True,
            )
            try:
                rc = p.wait(timeout=max(1, timeout))
            except subprocess.TimeoutExpired:
                rc = None
            killgrp(p)
        text = out.read_text(errors="replace")
        with open(self.R / "logs" / f"{name}.log", "a") as log:
            log.write(text)
        return rc, text

    def answer(self, call, arg):
        rc, text = self.oneshot(call, [arg])
        try:
            ans = json.loads(text)
        except ValueError:
            ans = None
        jlog.append(
            self.R / "answers.jsonl",
            {"call": call, "arg": arg, "answer": ans, "exit": rc},
        )

    def tick(self):
        now = time.monotonic()
        name, w = self.workers[-1] if self.workers else (None, None)
        if (
            w
            and w.poll()
            and name not in self.killed
            and len(self.workers) < MAX_WORKERS
        ):
            self.killed.add(name)
            self.mark("exited", worker=name, code=w.returncode)
            self.start_worker()
        for name, p in list(self.bg.items()):
            if p.poll() is not None:
                del self.bg[name]
                if p.returncode:
                    raise RuntimeError(f"{name} exited {p.returncode}")
        if self.poller:
            p, f = self.poller
            if p.poll() is not None:
                self.poller = None
                try:
                    self.status = json.loads(f.read_text())["task"]["status"]
                except (ValueError, KeyError, TypeError):
                    pass
            elif now - self.poll_at > 30:
                killgrp(p)
                self.poller = None
        if not self.poller and now >= self.poll_at + POLL:
            self.poll_at = now
            f = self.R / "ctl" / "state-poll.out"
            with (
                open(f, "wb") as out,
                open(self.R / "logs" / "state-poll.log", "ab") as log,
            ):
                p = subprocess.Popen(
                    self.cmd(self.cfg["state"]) + ["A"],
                    cwd=self.dir,
                    env=self.env("state"),
                    stdout=out,
                    stderr=log,
                    start_new_session=True,
                )
            self.poller = p, f

    def pump(self, timeout):
        if not select.select([self.rfd], [], [], timeout)[0]:
            return
        data = os.read(self.rfd, 65536)
        if not data:
            raise RuntimeError("services exited")
        *lines, self.buf = (self.buf + data).split(b"\n")
        for m in map(json.loads, lines):
            if m["event"] == "reached":
                self.reached.append(m)
            else:
                self.later(m["task"], m["action"])

    def wait(self, pred, limit=None, capped=True):
        end = time.monotonic() + limit if limit is not None else self.deadline
        if capped:
            end = min(end, self.deadline)
        while True:
            self.pump(0.01)
            r = pred()
            if r:
                return r
            if time.monotonic() >= end:
                if capped and end >= self.deadline:
                    raise Cap(f"run cap {RUN_CAP} s")
                return None
            self.tick()

    def ended(self):
        return self.status in END

    def start_worker(self):
        name = f"w{len(self.workers) + 1}"
        self.workers.append((name, self.spawn(name, self.cfg["worker"], name)))
        self.mark("started", worker=name)

    def kill(self):
        name, p = self.workers[-1]
        t = jlog.now()
        killgrp(p)
        self.killed.add(name)
        jlog.append(
            self.R / "markers.jsonl",
            {"t": t, "event": "killed", "data": {"worker": name}},
        )

    def arm_fault(self, svc, phase):
        self.arm_id += 1
        tmp = self.R / "ctl" / "arm.tmp"
        tmp.write_text(json.dumps({"id": self.arm_id, "svc": svc, "phase": phase}))
        tmp.rename(self.R / "ctl" / "arm")
        return self.arm_id

    def release(self, aid):
        (self.R / "ctl" / f"release-{aid}").write_text("1")

    def settle(self, aid):
        if not self.wait(lambda: (self.R / "ctl" / f"done-{aid}").exists(), 10):
            raise RuntimeError(f"held request {aid} never finished")

    def reach(self):
        self.wait(lambda: self.reached or self.ended())
        if self.reached:
            return self.reached.pop(0)
        self.notes.append("end state before the fault point")

    def deliver(self, task, action):
        with self.post_lock:
            if (task, action) in self.posted:
                return
            self.posted.add((task, action))
        decision = "deny" if self.seed % 2 else "approve"
        rc, _ = self.oneshot("deliver", [task, action, decision])
        rec = {"task": task, "action": action, "decision": decision, "exit": rc}
        jlog.append(self.R / "svc" / "approver.jsonl", {"kind": "posted", **rec})
        self.mark("posted", **rec)

    def later(self, task, action):
        def go():
            time.sleep(0.2)
            self.deliver(task, action)

        t = threading.Thread(target=go, daemon=True)
        t.start()
        self.threads.append(t)

    def start_engine(self, e):
        self.engines[e["name"]] = p = self.spawn(e["name"], e["start"], e["name"])
        while subprocess.run(
            self.cmd(e["ready"]),
            cwd=self.dir,
            env=self.env(e["name"]),
            capture_output=True,
        ).returncode:
            if p.poll() is not None:
                raise RuntimeError(f"engine {e['name']} exited {p.returncode}")
            if time.monotonic() > self.deadline:
                raise Cap(f"run cap {RUN_CAP} s")
            time.sleep(0.5)

    def restart_engine(self):
        name = self.cfg["restart_engine"]
        if name == "pg":
            self.pg.stop("fast")
            self.pg.start()
        else:
            p = self.engines[name]
            killgrp(p, signal.SIGTERM)
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
            killgrp(p)
            self.start_engine(next(e for e in self.cfg["engines"] if e["name"] == name))
        self.mark("engine-restarted", engine=name)

    def lines(self, svc):
        return jlog.read(self.R / "svc" / f"{svc}.jsonl")

    def new_request(self, svc, n, seen):
        new = self.lines(svc)[n:]
        i = next(
            (i for i, x in enumerate(new) if x["kind"] in ("request", "status")), None
        )
        if i is None:
            return None
        seen.setdefault("t", time.monotonic())
        done = (
            new[i]["kind"] == "status"
            or len(new) > i + 1
            or time.monotonic() - seen["t"] > 5
        )
        return done and "request"

    def fault(self):
        r = self.row
        kind, svc = r["kind"], SVC.get(r["boundary"], "T-I")
        phase = {
            "kill_after_reply": "after_reply",
            "kill_at_approval": "approval",
            "event_before_wait": "approval",
        }
        aid = (
            self.arm_fault(svc, phase.get(kind, "receipt")) if kind != "none" else None
        )
        self.start_worker()
        self.bg["submit"] = self.spawn("submit", self.cfg["submit"], "submit")
        if kind == "none" or not (m := self.reach()):
            return
        if kind in ("kill_at_receipt", "kill_during_recovery", "kill_at_approval"):
            self.kill()
            self.release(aid)
            self.settle(aid)
            if kind == "kill_at_approval":
                self.deliver(m["req"]["task"], m["req"]["action"])
            if kind == "kill_during_recovery":
                aid = self.arm_fault("any", "receipt")
                self.start_worker()
                if not self.reach():
                    return
                self.kill()
                self.release(aid)
                self.settle(aid)
            self.start_worker()
        elif kind == "kill_after_reply":
            time.sleep(int(r["varies"].split("=")[1]) / 1000)
            self.kill()
            self.start_worker()
        elif kind == "event_before_wait":
            self.deliver(m["req"]["task"], m["req"]["action"])
            self.release(aid)
            self.settle(aid)
            self.kill()
            self.start_worker()
        elif kind == "engine_restart_in_flight":
            self.restart_engine()
            self.release(aid)
        elif kind == "zombie_takeover":
            name, z = self.workers[-1]
            killgrp(z, signal.SIGSTOP)
            self.mark("stopped", worker=name)
            n, seen = len(self.lines(svc)), {}
            self.start_worker()
            why = "zombie_first"
            if r["varies"] == "new_first":
                why = self.wait(
                    lambda: self.new_request(svc, n, seen)
                    or self.ended()
                    and self.status,
                    TAKEOVER_T,
                )
                if not why:
                    why = "timeout"
                    self.notes.append(f"no takeover within {TAKEOVER_T} s")
            self.release(aid)
            killgrp(z, signal.SIGCONT)
            self.mark("resumed", worker=name, why=why)
        else:
            raise RuntimeError(f"unknown kind {kind}")

    def execute(self):
        status, why = "ended", None
        try:
            self.setup()
            self.fault()
            self.wait(self.ended)
            self.mark("end-state", status=self.status)
            for i in range(1, self.arm_id + 1):
                self.release(i)
            self.wait(lambda: False, QUIESCE, capped=False)
            self.mark("quiesced")
            self.answer("state", "A")
            self.answer("reconstruct", "T")
        except Cap as e:
            status, why = "cap", str(e)
        except Exception as e:
            status, why = "error", repr(e)
            self.mark("error", reason=why)
        finally:
            exited = {
                n: p.returncode
                for n, p in self.workers
                if p.poll() is not None and n not in self.killed
            }
            self.teardown()
            self.mark("run-end", status=status, why=why)
        return {
            "arm": self.arm,
            **{
                k: self.row[k]
                for k in ("point", "kind", "scenario", "boundary", "varies")
            },
            "seed": self.seed,
            "R": str(self.R),
            "status": status,
            "why": why,
            "notes": self.notes,
            "workers": len(self.workers),
            "exited": exited,
            "seconds": round(time.monotonic() - self.t0, 1),
        }

    def setup(self):
        shutil.rmtree(self.R, ignore_errors=True)
        for d in ("svc", "logs", "ctl"):
            (self.R / d).mkdir(parents=True)
        base = free_ports()
        ports = {"pg": base, "svc": base + 1, "arm": base + 2}
        self.pg = PG(
            self.R / "pg", ports["pg"], {**os.environ, "TRACKA_RUN": str(self.R)}
        )
        self.base_env = {
            "TRACKA_RUN": str(self.R),
            "TRACKA_SVC": f"http://127.0.0.1:{ports['svc']}",
            "TRACKA_PG": self.pg.url,
            "TRACKA_AGENT": "A",
            "TRACKA_TASK": "T",
            "TRACKA_SCENARIO": self.row["scenario"],
            "TRACKA_SEED": str(self.seed),
            "TRACKA_PORTS": str(ports["arm"]),
        }
        run = {
            **{
                k: self.row[k]
                for k in ("point", "scenario", "kind", "boundary", "varies")
            },
            "seed": self.seed,
            "seed_string": f"tracka:{FROZEN}:{self.row['point']}:{self.seed}",
            "arm": self.arm,
            "ports": ports,
            "unsupported": self.cfg.get("unsupported", []),
        }
        (self.R / "run.json").write_text(json.dumps(run, indent=1))
        self.pg.create()
        for e in self.cfg["engines"]:
            self.start_engine(e)
        rc, _ = self.oneshot("setup", timeout=self.deadline - time.monotonic())
        if rc != 0:
            raise RuntimeError(f"setup exited {rc}")
        self.rfd, w = os.pipe()
        with open(self.R / "logs" / "services.log", "ab") as log:
            self.svc = subprocess.Popen(
                [sys.executable, "-m", "tracka.services"],
                cwd=HERE,
                env={**os.environ, "TRACKA_RUN": str(self.R), "TRACKA_NOTIFY": str(w)},
                stdout=log,
                stderr=subprocess.STDOUT,
                pass_fds=(w,),
                start_new_session=True,
            )
        os.close(w)
        if not self.wait(
            lambda: (self.R / "ctl" / "svc-ready").exists()
            or self.svc.poll() is not None,
            10,
        ):
            raise RuntimeError("services did not start")
        if self.svc.poll() is not None:
            raise RuntimeError(f"services exited {self.svc.returncode}")

    def teardown(self):
        for p in (
            [p for _, p in self.workers]
            + list(self.bg.values())
            + [self.poller and self.poller[0], self.svc]
        ):
            if p:
                killgrp(p)
        for p in self.engines.values():
            killgrp(p)
        if self.pg:
            self.pg.destroy()
            if (self.pg.root / "postgres.log").exists():
                shutil.move(self.pg.root / "postgres.log", self.R / "logs" / "pg.log")
        kill_strays(lambda v: v == str(self.R), f"-D {self.R}/")
        for t in self.threads:
            t.join(5)


def main():
    arm, point, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
    print(json.dumps(Run(arm, point, seed, *sys.argv[4:5]).execute()), flush=True)


if __name__ == "__main__":
    main()
