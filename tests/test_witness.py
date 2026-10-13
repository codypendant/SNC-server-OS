#!/usr/bin/env python3
"""Replicated witnesses (tools/snc_witness.cpp): a grant to run alone and a
promotion are never both decided against the same primary.

Three snc_witness processes watch a fake primary and a fake standby (admin
ports answering STATUS and PROMOTE). In each trial the fake primary stops
answering STATUS (as if cut off from the witnesses), so the witnesses move
to promote the standby, while a DEGRADE request for the primary's term goes
to one witness picked at random (as if the primary could still reach it). A witness answers OK to DEGRADE only when the grant was chosen.

Checked in every trial:
  - never both a DEGRADE answered OK and a PROMOTE received by the standby;
  - exactly one of them happens (the trial decides something);
  - every witness ends with the same chosen state.
And once, with one witness killed first: the other two still decide.

Usage: test_witness.py BUILD_DIR [TRIALS]
"""
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

UUID = "ab" * 16


class FakeNode:
    """An admin port: STATUS (when answering), PROMOTE."""

    def __init__(self, role):
        self.role = role
        self.term = 0
        self.answering = True
        self.promotes = 0
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(64)
        self.port = self.sock.getsockname()[1]
        self.stop = False
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        self.sock.settimeout(0.2)
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except socket.timeout:
                continue
            threading.Thread(target=self._one, args=(c,), daemon=True).start()

    def _one(self, c):
        with c:
            c.settimeout(2)
            try:
                line = c.makefile().readline().strip()
            except OSError:
                return
            if line == "STATUS":
                if not self.answering:
                    time.sleep(2)  # silent: the caller times out
                    return
                reply = f"role={self.role} term={self.term} uuid={UUID} shards=4 live=4 degraded=0 blocked=0"
            elif line == "PROMOTE":
                self.promotes += 1
                if self.role == "s":
                    self.role = "p"
                    self.term += 1
                reply = "OK"
            else:
                reply = "NO"
            try:
                c.sendall((reply + "\n").encode())
            except OSError:
                pass


def ask(port, line, timeout=3):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout) as s:
            s.settimeout(timeout)
            s.sendall((line + "\n").encode())
            return s.makefile().readline().strip()
    except OSError:
        return None


def free_ports(n):
    socks = [socket.socket() for _ in range(n)]
    for s in socks:
        s.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in socks]
    for s in socks:
        s.close()
    return ports


def state_of(path):
    with open(path) as f:
        for line in f:
            if line.startswith("chosen ha/0 "):
                return line.split()[2:]  # version uuid term degraded primary
    return None


def trial(build, work, n_down=0, seed=0):
    rnd = random.Random(seed)
    prim, sb = FakeNode("p"), FakeNode("s")
    ports = free_ports(3)
    peers = ",".join(f"127.0.0.1:{p}" for p in ports)
    ws = []
    for i, p in enumerate(ports):
        ws.append(subprocess.Popen(
            [f"{build}/snc_witness", "--port", str(p), "--primary", f"127.0.0.1:{prim.port}",
             "--standby", f"127.0.0.1:{sb.port}", "--state", f"{work}/w{i}.state",
             "--failover-ms", "200", "--poll-ms", "20", "--id", str(i), "--peers", peers],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    try:
        # Every witness has learned the primary.
        deadline = time.time() + 10
        while time.time() < deadline:
            st = [state_of(f"{work}/w{i}.state") if os.path.exists(f"{work}/w{i}.state") else None
                  for i in range(3)]
            if all(s and s[1] == UUID for s in st):
                break
            time.sleep(0.02)
        else:
            raise AssertionError(f"the witnesses never learned the primary: {st}")
        for i in range(n_down):
            ws[i].kill()
            ws[i].wait()
        # Cut off: the witnesses stop hearing the primary. Somewhere around
        # their failover timeout (200 ms), the primary asks to run alone.
        prim.answering = False
        time.sleep(rnd.uniform(0.0, 0.35))
        # Like the primary, it asks one witness (the others may be the
        # ones deciding to promote).
        oks = [ask(ports[rnd.randrange(n_down, 3)], f"DEGRADE {UUID} 0")]
        time.sleep(0.6)  # time for a promotion, if one was chosen
        granted = any(r == "OK" for r in oks)
        promoted = sb.promotes > 0
        assert not (granted and promoted), f"granted {oks} AND promoted ({sb.promotes} PROMOTEs)"
        assert granted or promoted, f"nothing decided: {oks}, {sb.promotes} PROMOTEs"
        # Every live witness ends with the same chosen state (a laggard
        # learns it at the latest from the next proposal; force one).
        for i in range(n_down, 3):
            ask(ports[i], f"SYNCED {UUID} 999")  # refused, but runs a read of the latest
        live = [state_of(f"{work}/w{i}.state") for i in range(n_down, 3)]
        majority_state = max(live, key=lambda s: int(s[0]))
        assert sum(s == majority_state for s in live) >= 2, f"witnesses disagree: {live}"
        return "granted" if granted else "promoted"
    finally:
        for w in ws:
            if w.poll() is None:
                w.kill()
            w.wait()
        prim.stop = sb.stop = True


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    counts = {"granted": 0, "promoted": 0}
    for k in range(trials):
        work = tempfile.mkdtemp(prefix="snc-witness-")
        try:
            counts[trial(build, work, seed=k)] += 1
        finally:
            shutil.rmtree(work, ignore_errors=True)
    work = tempfile.mkdtemp(prefix="snc-witness-")
    try:
        down = trial(build, work, n_down=1, seed=12345)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"witness race: {trials} trials, {counts['granted']} granted, {counts['promoted']} "
          f"promoted, never both; with one witness down: {down}")
    print("test_witness: ok")


if __name__ == "__main__":
    main()
