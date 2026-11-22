import os
import shutil
import signal
import subprocess
import time
from pathlib import Path

import psycopg

BIN = Path(os.environ.get("AR1_PGBIN", "/opt/ar1/bin"))
ADMIN = "ar1admin"
RUNTIME = "ar1"
NOMON = "ar1nomon"
DB = "ar1"

PRIMARY_CONF = """
listen_addresses = '127.0.0.1'
port = {port}
unix_socket_directories = '{sock}'
wal_level = replica
synchronous_commit = on
synchronous_standby_names = 'FIRST 1 (s1)'
fsync = on
full_page_writes = on
wal_keep_size = '8GB'
max_wal_senders = 8
wal_sender_timeout = '60s'
shared_buffers = '128MB'
max_connections = 200
hot_standby = on
log_min_messages = warning
log_line_prefix = '%m [%p] '
logging_collector = off
"""


def _run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, **kw)


def connect(port, user=RUNTIME, db=DB, autocommit=True, app="ar1"):
    return psycopg.connect(
        host="127.0.0.1",
        port=port,
        user=user,
        dbname=db,
        autocommit=autocommit,
        application_name=app,
        connect_timeout=5,
    )


class Node:
    def __init__(self, root, name, port):
        self.name = name
        self.port = port
        self.data = Path(root) / name
        self.sock = Path(root) / "sock"
        self.log = Path(root) / f"{name}.log"

    def pg_ctl(self, *args, check=True):
        return subprocess.run(
            [str(BIN / "pg_ctl"), "-D", str(self.data), "-l", str(self.log), *args],
            check=check,
            capture_output=True,
            text=True,
        )

    def start(self):
        self.pg_ctl("-w", "-t", "60", "start")

    def stop(self, mode="fast"):
        self.pg_ctl("-w", "-t", "120", "-m", mode, "stop", check=False)

    def restart(self, mode):
        self.pg_ctl("-w", "-t", "120", "-m", mode, "restart")

    def postmaster_pid(self):
        return int((self.data / "postmaster.pid").read_text().split()[0])

    def conn(self, user=RUNTIME, db=DB, app="ar1"):
        return connect(self.port, user, db, app=app)

    def wal_dir(self):
        return self.data / "pg_wal"


class Pair:
    """A primary and one synchronous physical standby (s1) on 127.0.0.1."""

    def __init__(self, root, base_port=None):
        self.root = Path(root)
        base = base_port or int(os.environ.get("AR1_PORT", "54320"))
        self.primary = Node(self.root, "primary", base + 1)
        self.standby = Node(self.root, "standby", base + 2)
        self._stopped = False

    def create(self):
        if self.root.exists():
            shutil.rmtree(self.root)
        self.root.mkdir(parents=True)
        self.primary.sock.mkdir()
        p = self.primary
        _run(BIN / "initdb", "-D", p.data, "-U", ADMIN, "--data-checksums", "-A", "trust", "--no-sync")
        with open(p.data / "postgresql.conf", "a") as f:
            f.write(PRIMARY_CONF.format(port=p.port, sock=p.sock))
        with open(p.data / "pg_hba.conf", "a") as f:
            f.write("host replication all 127.0.0.1/32 trust\n")
        p.start()
        with connect(p.port, ADMIN, "postgres") as c:
            c.execute("SET synchronous_commit = local")
            c.execute(f"CREATE ROLE {RUNTIME} LOGIN")
            c.execute(f"GRANT pg_monitor TO {RUNTIME}")
            c.execute(f"CREATE ROLE {NOMON} LOGIN")
            c.execute(f"CREATE DATABASE {DB} OWNER {RUNTIME}")
            for ex in ("e1", "e2"):
                c.execute(f"CREATE DATABASE {DB}_sys_{ex} OWNER {RUNTIME}")
        s = self.standby
        _run(
            BIN / "pg_basebackup",
            "-h",
            "127.0.0.1",
            "-p",
            p.port,
            "-U",
            ADMIN,
            "-D",
            s.data,
            "-X",
            "stream",
            "-c",
            "fast",
            "-R",
            "-d",
            f"host=127.0.0.1 port={p.port} user={ADMIN} application_name=s1",
        )
        with open(s.data / "postgresql.conf", "a") as f:
            f.write(f"port = {s.port}\n")
        s.start()
        self.wait_sync()

    def wait_sync(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            try:
                with self.primary.conn() as c:
                    r = c.execute("SELECT sync_state FROM pg_stat_replication WHERE application_name='s1'").fetchone()
                if r and r[0] == "sync":
                    return
            except psycopg.OperationalError:
                pass
            time.sleep(0.1)
        raise RuntimeError("standby never reached sync")

    def walreceiver_pid(self, conn=None):
        own = conn is None
        conn = conn or self.standby.conn()
        try:
            r = conn.execute("SELECT pid FROM pg_stat_wal_receiver").fetchone()
            return r[0] if r else None
        finally:
            if own:
                conn.close()

    def _standby_pids(self):
        return [self.standby.postmaster_pid(), self.walreceiver_pid()]

    def stop_standby(self):
        """SIGSTOP of the standby's postmaster and walreceiver, so commits block inside COMMIT."""
        self._stop_pids = [p for p in self._standby_pids() if p]
        for pid in self._stop_pids:
            os.kill(pid, signal.SIGSTOP)
        self._stopped = True

    def resume_standby(self):
        for pid in getattr(self, "_stop_pids", []):
            try:
                os.kill(pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
        self._stopped = False

    def cut_off_standby(self):
        s = self.standby
        s.stop("fast")
        with open(s.data / "postgresql.auto.conf", "a") as f:
            f.write("primary_conninfo = ''\nrestore_command = ''\n")
        s.start()

    def promote_standby(self):
        with self.standby.conn(ADMIN, "postgres") as c:
            return c.execute("SELECT pg_promote(true, 60)").fetchone()[0]

    def destroy(self):
        if self._stopped:
            self.resume_standby()
        for n in (self.standby, self.primary):
            if (n.data / "postmaster.pid").exists():
                n.stop("immediate")
        shutil.rmtree(self.root, ignore_errors=True)


def waldump(bindir_wal, start=None, end=None, timeline=None, extra=()):
    args = [str(BIN / "pg_waldump"), "-p", str(bindir_wal), *extra]
    if timeline:
        args += ["-t", str(timeline)]
    if start:
        args += ["-s", start]
    if end:
        args += ["-e", end]
    return subprocess.run(args, capture_output=True, text=True).stdout
