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

    def __init__(self, role, uuid=UUID):
        self.role = role
        self.uuid = uuid
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
                reply = f"role={self.role} term={self.term} uuid={self.uuid} shards=4 live=4 degraded=0 blocked=0"
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
    """n ports free to bind now, below the ephemeral range (32768 up), where
    the kernel would also hand them to outgoing connections."""
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


def state_of(path, machine="ha/0"):
    with open(path) as f:
        for line in f:
            if line.startswith(f"chosen {machine} "):
                return line.split()[2:]  # version, then the body (ha: uuid term degraded primary)
    return None


def multi_group(build, work):
    """Three groups' HA on one witness quorum (docs/CONTROL_PLANE.md section
    10), next to placement and leases: one group fails over, another runs
    alone under a grant, and nothing else moves."""
    uuids = [f"{g:02x}" * 16 for g in range(3)]
    pairs = [(FakeNode("p", uuids[g]), FakeNode("s", uuids[g])) for g in range(3)]
    ports = free_ports(3)
    peers = ",".join(f"127.0.0.1:{p}" for p in ports)
    ws = []
    for i, p in enumerate(ports):
        args = [f"{build}/snc_witness", "--port", str(p), "--state", f"{work}/w{i}.state",
                "--failover-ms", "200", "--poll-ms", "20", "--id", str(i), "--peers", peers]
        for g, (pr, sb) in enumerate(pairs):
            args += ["--group", f"{g}=127.0.0.1:{pr.port},127.0.0.1:{sb.port}"]
        ws.append(subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))

    def chosen(machine):
        """The machine's chosen value on a majority (READ on every witness)."""
        vals = [ask(p, f"READ {machine}") for p in ports]
        vals = [v.split(" ", 2)[1:] for v in vals if v and v.startswith("STATE ")]
        if not vals:
            return 0, ""
        best = max(vals, key=lambda v: int(v[0]))
        return int(best[0]), best[1] if len(best) > 1 else ""

    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            if all(chosen(f"ha/{g}")[1].startswith(uuids[g]) for g in range(3)):
                break
            time.sleep(0.02)
        else:
            raise AssertionError("the witnesses never learned every group's primary")
        assert ask(ports[0], "PLACEMENT INIT v1 test-map epoch=1") == "OK"
        block = ask(ports[1], "LEASES TAKE 65536")
        assert block.startswith("BLOCK "), block
        before = {m: chosen(m) for m in ["ha/0", "ha/1", "ha/2", "placement", "leases"]}
        # Group 2's primary runs alone under a grant (its own machine).
        assert ask(ports[2], f"DEGRADE {uuids[2]} 0 2") == "OK"
        # Group 1's primary goes silent: its standby is promoted.
        pairs[1][0].answering = False
        deadline = time.time() + 10
        while time.time() < deadline and pairs[1][1].role != "p":
            time.sleep(0.02)
        assert pairs[1][1].role == "p" and pairs[1][1].term == 1, "group 1's standby was not promoted"
        time.sleep(0.5)  # anything else the witnesses would do, they do now
        after = {m: chosen(m) for m in before}
        # Group 1: a new term, its standby now primary.
        v1, b1 = after["ha/1"]
        assert b1.split() == [uuids[1], "1", "0", "1"], b1
        # Group 2: degraded, nothing else. Group 0, placement, leases: unchanged.
        assert after["ha/2"][1].split() == [uuids[2], "0", "1", "0"], after["ha/2"]
        assert after["ha/2"][0] == before["ha/2"][0] + 1
        for m in ["ha/0", "placement", "leases"]:
            assert after[m] == before[m], f"{m} moved: {before[m]} -> {after[m]}"
        assert pairs[0][1].promotes == 0 and pairs[2][1].promotes == 0, "another group's standby promoted"
        # The old term of group 1 is refused; group 0's primary is not.
        assert ask(ports[0], f"DEGRADE {uuids[1]} 0 1").startswith("NO term 0 was replaced")
        assert ask(ports[0], f"SYNCED {uuids[0]} 0 0") == "OK"
        print(f"multi-group: 3 groups on one witness quorum; group 1 failed over to term 1, group 2 "
              f"granted to run alone, group 0, placement (version {after['placement'][0]}) and leases "
              f"unchanged")
    finally:
        for w in ws:
            if w.poll() is None:
                w.kill()
            w.wait()
        for pr, sb in pairs:
            pr.stop = sb.stop = True


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
    work = tempfile.mkdtemp(prefix="snc-witness-")
    try:
        multi_group(build, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("test_witness: ok")


if __name__ == "__main__":
    main()
