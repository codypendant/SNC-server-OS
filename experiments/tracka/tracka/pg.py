import os
import shutil
import subprocess
from pathlib import Path

BIN = Path(os.environ.get("TRACKA_PGBIN", "/opt/ar1/bin"))
USER = os.environ.get("TRACKA_PGUSER", "pgrun")
CONF = """
listen_addresses = '127.0.0.1'
port = {port}
unix_socket_directories = ''
fsync = on
synchronous_commit = on
full_page_writes = on
max_connections = 200
logging_collector = off
log_line_prefix = '%m [%p] '
"""


class PG:
    def __init__(self, root, port, env):
        self.root, self.port, self.env = Path(root), port, env
        self.data = self.root / "data"
        self.url = f"postgresql://tracka@127.0.0.1:{port}/tracka"

    def sh(self, *args, check=True):
        # initdb refuses root, as AR-1's runner was non-root (G3_VM_CARD's runuser)
        pre = ["runuser", "-u", USER, "--"] if os.geteuid() == 0 else []
        return subprocess.run(
            pre + [str(a) for a in args],
            cwd=self.root,
            env=self.env,
            check=check,
            capture_output=True,
            text=True,
            timeout=120,
        )

    def create(self):
        self.root.mkdir(parents=True)
        if os.geteuid() == 0:
            shutil.chown(self.root, USER)
        self.sh(
            BIN / "initdb", "-D", self.data, "-U", "tracka", "-A", "trust", "-E", "UTF8"
        )
        with open(self.data / "postgresql.conf", "a") as f:
            f.write(CONF.format(port=self.port))
        self.start()
        self.sh(
            BIN / "createdb",
            "-h",
            "127.0.0.1",
            "-p",
            self.port,
            "-U",
            "tracka",
            "tracka",
        )

    def start(self):
        self.sh(
            BIN / "pg_ctl",
            "-D",
            self.data,
            "-l",
            self.root / "postgres.log",
            "-w",
            "-t",
            "60",
            "start",
        )

    def stop(self, mode="fast"):
        self.sh(
            BIN / "pg_ctl",
            "-D",
            self.data,
            "-w",
            "-t",
            "60",
            "-m",
            mode,
            "stop",
            check=False,
        )

    def destroy(self):
        if (self.data / "postmaster.pid").exists():
            self.stop("immediate")
        shutil.rmtree(self.data, ignore_errors=True)
