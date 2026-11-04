#!/usr/bin/env python3
"""R08 characterization (docs/EVIDENCE.md section 2): can a witness promote
an async standby that lacks writes its primary acknowledged?

Real processes: a primary snc_server with --async-replica and --witness, its
standby, and one snc_witness (--failover-ms 500, --poll-ms 50).

  1. Accounts are created on the primary while the standby is live; then
     the standby catches up (control set A).
  2. The standby is stopped (SIGSTOP). The async primary keeps
     acknowledging: accounts are created on it (set B, every create
     answered OK; creates always wait for local durability).
  3. The primary is SIGKILLed and the standby resumed (SIGCONT).
  4. Whether the witness promotes the standby, and which of A and B the
     promoted node has.

The same history is run with a sync primary (no --async-replica) as the
control: there the stopped standby blocks the primary until the witness
grants it leave to run alone, and with the grant on record the witness
must not promote.

This test asserts the behavior observed on 2026-11-04 (a characterization,
not the intended contract). When the contract for async pairs is decided,
change the assertions with it.

Usage: test_async_failover.py BUILD_DIR
"""
import os
import random
import struct
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
UUID = "a5" * 16


def free_ports(n):
    """n distinct ports free to bind now, below the ephemeral range."""
    ports = []
    rnd = random.Random(os.getpid() * 7919 + time.time_ns())
    while len(ports) < n:
        p = rnd.randrange(20000, 32000)
        if p in ports:
            continue
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            ports.append(p)
        except OSError:
            pass
        finally:
            s.close()
    return ports


def wait_port(port, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


class Proc:
    def __init__(self, args):
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.lines = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip())

    def wait_for(self, pattern, timeout):
        t_end = time.time() + timeout
        while time.time() < t_end:
            for l in list(self.lines):
                if re.search(pattern, l):
                    return l
            time.sleep(0.01)
        return None

    def text(self):
        return "\n".join(self.lines)

    def signal(self, sig):
        if self.p.poll() is None:
            self.p.send_signal(sig)

    def stop(self):
        self.signal(signal.SIGCONT)
        self.signal(signal.SIGKILL)
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass


def ask(port, line, timeout=2):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout) as s:
            s.settimeout(timeout)
            s.sendall((line + "\n").encode())
            return s.makefile().readline().strip()
    except OSError:
        return None


def bench_ids(port, ids, objects):
    """net_bench --ids: creates `objects` accounts and lists them in `ids`
    or, if `ids` exists, uses the accounts it lists (a missing one is an
    error). Returns (errors, output)."""
    out = subprocess.run(
        [f"{BUILD}/net_bench", "--port", str(port), "--threads", "1", "--objects", str(objects),
         "--seconds", "0.3", "--route", "owner", "--reads", "100", "--depth", "16", "--ids", ids],
        capture_output=True, text=True, timeout=120).stdout
    e = re.search(r"errors (\d+)", out)
    return (int(e[1]) if e else None), out


def count_missing(port, ids_path):
    """Reads every account listed in ids_path (16-byte ObjectIDs) once, on
    one connection to shard 0's port (which forwards to the owner), and
    returns (missing, total): how many answer anything but OK."""
    with open(ids_path, "rb") as f:
        raw = f.read()
    ids = [raw[i:i + 16] for i in range(0, len(raw), 16)]
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.settimeout(10)
    for n, oid in enumerate(ids):
        # ReqHeader: len, kind EXEC, flags, reserved, op GetBalance, reserved, req_id, target, expected_version
        s.sendall(struct.pack("<IBBHIIQ16sQ", 48, 1, 0, 0, 1006, 0, n + 1, oid, 0))
    buf, missing, got = b"", 0, 0
    while got < len(ids):
        chunk = s.recv(65536)
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
        while len(buf) >= 48:
            length, = struct.unpack_from("<I", buf, 0)
            if len(buf) < length:
                break
            status, = struct.unpack_from("<H", buf, 6)
            if status != 0:
                missing += 1
            got += 1
            buf = buf[length:]
    s.close()
    return missing, len(ids)


def run(async_replica):
    work = tempfile.mkdtemp(prefix="snc-async-")
    p_cli, s_cli, repl, p_adm, s_adm, wit = free_ports(6)
    common = ["--space-uuid", UUID, "--objects", "100000", "--shards", "2"]
    sb = Proc([f"{BUILD}/snc_server", "--port", str(s_cli), "--data-dir", f"{work}/s",
               "--admin-port", str(s_adm), "--standby", str(repl)] + common)
    procs = [sb]
    try:
        assert wait_port(s_adm), "standby never opened its admin port\n" + sb.text()
        prim = Proc([f"{BUILD}/snc_server", "--port", str(p_cli), "--data-dir", f"{work}/p",
                     "--admin-port", str(p_adm), "--replica-of", f"127.0.0.1:{repl}",
                     "--replica-timeout-ms", "500", "--witness", f"127.0.0.1:{wit}"] +
                    (["--async-replica"] if async_replica else []) + common)
        procs.append(prim)
        assert wait_port(p_adm), "primary never opened its admin port\n" + prim.text()
        w = Proc([f"{BUILD}/snc_witness", "--port", str(wit), "--primary", f"127.0.0.1:{p_adm}",
                  "--standby", f"127.0.0.1:{s_adm}", "--state", f"{work}/witness.state",
                  "--failover-ms", "500", "--poll-ms", "50"])
        procs.append(w)
        assert w.wait_for(r"primary up", 10), "witness never saw the primary\n" + w.text()

        # 1. Set A, with the standby live; give it time to catch up.
        ids_a, ids_b = f"{work}/ids.a", f"{work}/ids.b"
        err, out = bench_ids(p_cli, ids_a, 500)
        assert err == 0, out
        time.sleep(1.0)

        # 2. Set B, with the standby stopped.
        sb.signal(signal.SIGSTOP)
        time.sleep(0.2)
        if async_replica:
            err, out = bench_ids(p_cli, ids_b, 500)
            acked_b = err == 0 and os.path.exists(ids_b)
            assert acked_b, "the async primary did not acknowledge set B\n" + out
        else:
            # Sync: creates wait for the standby, then for the witness's
            # grant to run alone (past --replica-timeout-ms).
            err, out = bench_ids(p_cli, ids_b, 500)
            acked_b = err == 0 and os.path.exists(ids_b)
            assert acked_b, "the sync primary never continued under a grant\n" + out + prim.text()
            assert prim.wait_for(r"witness: granted", 5), "no grant\n" + prim.text()
        status_before = ask(s_adm, "STATUS", 0.3)  # stopped: no answer

        # 3. Kill the primary, resume the standby.
        prim.signal(signal.SIGKILL)
        prim.p.wait(timeout=30)
        sb.signal(signal.SIGCONT)

        # 4. Promoted?
        promoted = w.wait_for(r"failover:", 8) is not None
        st = ask(s_adm, "STATUS") or ""
        role = re.search(r"role=(\w)", st)
        role = role[1] if role else "?"
        res = dict(mode="async" if async_replica else "sync", acked_b=acked_b, promoted=promoted,
                   standby_role_after=role, status_while_stopped=status_before)
        if promoted:
            res["missing_a"], res["total_a"] = count_missing(s_cli, ids_a)
            res["missing_b"], res["total_b"] = count_missing(s_cli, ids_b)
        res["witness_log"] = [l for l in w.lines if "group" in l or "failover" in l or "DEGRADE" in l]
        return res
    finally:
        for p in procs:
            p.stop()
        shutil.rmtree(work, ignore_errors=True)


def main():
    a = run(async_replica=True)
    print(f"async: set B acknowledged with the standby stopped: {a['acked_b']}; "
          f"witness promoted the standby: {a['promoted']}; "
          f"on the promoted node, missing from set A: {a.get('missing_a')}, from set B: {a.get('missing_b')} of {a.get('total_b')}")
    for l in a["witness_log"]:
        print("  witness:", l)
    s = run(async_replica=False)
    print(f"sync (control): set B acknowledged under a grant: {s['acked_b']}; "
          f"witness promoted the standby: {s['promoted']} (standby role {s['standby_role_after']})")
    for l in s["witness_log"]:
        print("  witness:", l)

    # Observed 2026-11-04 (characterization): the async standby is promoted
    # and lacks acknowledged writes; the sync control is not promoted.
    assert a["promoted"], a
    assert a["missing_a"] == 0, a
    assert a["missing_b"] and a["missing_b"] > 0, a
    assert not s["promoted"], s
    print("test_async_failover: ok (async pair loses acknowledged writes on witness failover; sync does not)")


if __name__ == "__main__":
    main()
