import json
import os
import signal
import threading
import time
from pathlib import Path

import psycopg

RETRY_S = 10.0
FREEZE_MAX_S = 1.0
DRAIN_S = 10.0


class ReadFailed(Exception):
    pass


def now():
    return time.monotonic_ns()


def lsn_int(s):
    if s is None:
        return None
    hi, lo = str(s).split("/")
    return (int(hi, 16) << 32) | int(lo, 16)


def _proc_state(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (FileNotFoundError, ProcessLookupError, IndexError):
        return None


class Oracle:
    def __init__(self, run_dir, primary_port, standby_port, app_prefix, user="ar1", db="ar1"):
        self.path = Path(run_dir) / "cuts.jsonl"
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.ports = {"primary": primary_port, "standby": standby_port}
        self.user, self.db, self.app_prefix = user, db, app_prefix
        self.conns = {k: self._connect(k) for k in self.ports}
        self.lock = threading.Lock()
        self.frozen = None
        self.overdue = False
        self.invalid = []
        self.last_standby = None

    def _connect(self, which):
        return psycopg.connect(
            host="127.0.0.1",
            port=self.ports[which],
            user=self.user,
            dbname=self.db,
            autocommit=True,
            application_name="oracle-ar1",
            connect_timeout=2,
        )

    def q(self, which, sql, args=None):
        end = time.monotonic() + RETRY_S
        err = None
        while time.monotonic() < end:
            try:
                if self.conns.get(which) is None or self.conns[which].closed:
                    self.conns[which] = self._connect(which)
                return self.conns[which].execute(sql, args).fetchall()
            except psycopg.Error as e:
                err = e
                try:
                    self.conns[which].close()
                except Exception:
                    pass
                self.conns[which] = None
                time.sleep(0.05)
        raise ReadFailed(f"{which}: {err}")

    def write(self, rec):
        rec.setdefault("t", now())
        line = json.dumps(rec, sort_keys=True) + "\n"
        with self.lock:
            fd = os.open(self.path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
            try:
                os.write(fd, line.encode())
                os.fsync(fd)
            finally:
                os.close(fd)
        return rec

    def mark_invalid(self, label, reason):
        self.invalid.append(reason)
        self.write({"kind": "invalid", "label": label, "reason": reason})

    # freeze / thaw

    def freeze(self, label):
        pid = self.q("standby", "SELECT pid FROM pg_stat_wal_receiver")
        pid = pid[0][0] if pid and pid[0][0] else None
        if pid is None:
            self.frozen = {"pid": None, "t0": now(), "label": label, "ours": False}
            return self.frozen
        ours = _proc_state(pid) != "T"
        t0 = now()
        os.kill(pid, signal.SIGSTOP)
        # kill() only queues the signal; the walreceiver may run until it reaches user mode, so wait for state T.
        while _proc_state(pid) not in ("T", None):
            time.sleep(0.0002)
        self.overdue = False
        timer = threading.Timer(FREEZE_MAX_S, self._overdue, (pid, label)) if ours else None
        self.frozen = {"pid": pid, "t0": t0, "label": label, "ours": ours, "timer": timer}
        if timer:
            timer.daemon = True
            timer.start()
        return self.frozen

    def _overdue(self, pid, label):
        self.overdue = True
        try:
            os.kill(pid, signal.SIGCONT)
        except ProcessLookupError:
            pass
        self.mark_invalid(label, "freeze over 1 s")

    def thaw(self):
        f, self.frozen = self.frozen, None
        if not f or f["pid"] is None or not f["ours"]:
            return {"freeze_ms": None, "pid": f and f["pid"]}
        f["timer"].cancel()
        try:
            os.kill(f["pid"], signal.SIGCONT)
        except ProcessLookupError:
            pass
        ms = (now() - f["t0"]) / 1e6
        return {"freeze_ms": ms, "pid": f["pid"], "overdue": self.overdue}

    # reads

    def standby_read(self):
        r = self.q("standby", "SELECT pid, flushed_lsn::text, received_tli FROM pg_stat_wal_receiver")
        if r and r[0][0]:
            return {
                "source": "walreceiver",
                "walreceiver_pid": r[0][0],
                "flushed_lsn": r[0][1],
                "received_tli": r[0][2],
            }
        r = self.q(
            "standby",
            "SELECT (SELECT pid FROM pg_stat_wal_receiver), pg_last_wal_receive_lsn()::text,"
            " (SELECT pid FROM pg_stat_wal_receiver)",
        )[0]
        if r[0] or r[2]:
            return self.standby_read()
        return {"source": "receive_lsn", "walreceiver_pid": None, "flushed_lsn": r[1], "received_tli": None}

    def primary_read(self):
        r = self.q(
            "primary",
            "SELECT flush_lsn::text, sync_state, pid FROM pg_stat_replication WHERE application_name = 's1'",
        )
        rep = (
            {"flush_lsn": r[0][0], "sync_state": r[0][1], "walsender_pid": r[0][2]}
            if r
            else {"flush_lsn": None, "sync_state": None, "walsender_pid": None}
        )
        ins, tli = self.q(
            "primary",
            "SELECT pg_current_wal_insert_lsn()::text,"
            " ('x'||substr(pg_walfile_name(pg_current_wal_insert_lsn()),1,8))::bit(32)::int",
        )[0]
        return rep, ins, tli

    def _cut(self, kind, label, boundary=False):
        rec = {"kind": kind, "label": label, "unresolved": [], "invalid": []}
        try:
            rec["standby"] = self.standby_read()
            rep, ins, tli = self.primary_read()
            rec["primary"] = rep
            rec["primary_tli"] = tli
            rec["insert_lsn"] = ins
            rec["t_cut"] = now()
            pf, sf = lsn_int(rep["flush_lsn"]), lsn_int(rec["standby"]["flushed_lsn"])
            if pf is not None and sf is not None and pf > sf:
                rec["unresolved"].append("primary flush_lsn above standby flushed_lsn")
            if boundary:
                rec["kill_boundary"] = ins
            self.last_standby = rec["standby"]
        except ReadFailed as e:
            rec["invalid"].append(f"read failed: {e}")
        return rec

    def _frozen_cut(self, kind, label, boundary=False):
        try:
            self.freeze(label)
        except ReadFailed as e:
            rec = {"kind": kind, "label": label, "unresolved": [], "invalid": [f"read failed: {e}"]}
            self.invalid.append(rec["invalid"][0])
            return self.write(rec)
        try:
            rec = self._cut(kind, label, boundary)
        finally:
            th = self.thaw()
        rec.update(th)
        if th.get("overdue"):
            rec["invalid"].append("freeze over 1 s")
        self.invalid += rec["invalid"]
        return self.write(rec)

    # harness API

    def kill_cut(self, label, kill, standby_stopped=False):
        try:
            if not standby_stopped:
                self.freeze(label)
            rec = self._cut("kill", label)
        except ReadFailed as e:
            rec = {"kind": "kill", "label": label, "unresolved": [], "invalid": [f"read failed: {e}"]}
        self.invalid += rec["invalid"]
        try:
            if rec["invalid"] and self.frozen:
                self.thaw()
            self.write(rec)
            kill()
        finally:
            if self.frozen:
                th = self.thaw()
                self.write({"kind": "thaw", "label": label, **th})
        return self.write(self._kill_boundary(label))

    def _kill_boundary(self, label):
        rec = {"kind": "kill_boundary", "label": label, "invalid": []}
        end = time.monotonic() + DRAIN_S
        try:
            while True:
                n = self.q(
                    "primary",
                    "SELECT count(*) FROM pg_stat_activity WHERE pid <> pg_backend_pid()"
                    " AND starts_with(application_name, %s) AND state = 'active' AND wait_event IS DISTINCT FROM 'SyncRep'",
                    (self.app_prefix,),
                )[0][0]
                if n == 0:
                    break
                if time.monotonic() > end:
                    rec["invalid"].append("app backends did not exit")
                    break
                time.sleep(0.01)
            rec["kill_boundary"] = self.q("primary", "SELECT pg_current_wal_insert_lsn()::text")[0][0]
        except ReadFailed as e:
            rec["invalid"].append(f"read failed: {e}")
        self.invalid += rec["invalid"]
        return rec

    def pre_resume_cut(self, label="pre-resume"):
        prev = (self.last_standby or {}).get("walreceiver_pid")
        rec = self._cut("pre-resume", label)
        pid = (rec.get("standby") or {}).get("walreceiver_pid")
        if not rec["invalid"]:
            if pid is None or (prev is not None and pid != prev):
                rec["invalid"].append(f"walreceiver pid {pid} is not the stopped one {prev}")
            elif _proc_state(pid) != "T":
                rec["invalid"].append(f"walreceiver {pid} not stopped")
        self.invalid += rec["invalid"]
        return self.write(rec)

    def takeover_cut(self, label):
        return self._frozen_cut("takeover", label, boundary=True)

    def cut(self, label):
        return self._frozen_cut("cut", label)

    def final_cut(self, timeout=30):
        label = "final"
        try:
            target = lsn_int(self.q("primary", "SELECT pg_current_wal_flush_lsn()::text")[0][0])
            end = time.monotonic() + timeout
            while True:
                s = lsn_int(self.standby_read()["flushed_lsn"])
                if s is not None and s >= target:
                    break
                if time.monotonic() > end:
                    self.write({"kind": "unresolved", "label": label, "reason": "catch-up over 30 s"})
                    break
                time.sleep(0.02)
        except ReadFailed as e:
            self.mark_invalid(label, f"read failed: {e}")
        return self._frozen_cut("final", label)

    def close(self):
        for c in self.conns.values():
            try:
                c and c.close()
            except Exception:
                pass
