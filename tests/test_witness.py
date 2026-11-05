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
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from witness_state import first_start  # noqa: E402

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


def make_ready(port, uuid, term=0, group=None):
    """What a primary does once its standby is proven (docs/SYNC_READINESS.md
    7): NOT READY at a fresh version N, then READY after=N."""
    g = "" if group is None else f" {group}"
    r = ask(port, f"NOTREADY {uuid} {term}{g}")
    assert r and r.startswith("OK v="), r
    n = int(r[5:])
    r = ask(port, f"READY {uuid} {term}{g} after={n}")
    assert r == f"OK v={n + 1}", (n, r)
    return n + 1


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
        ws.append(subprocess.Popen(first_start(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))

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
        # Learned primaries are NOT READY; each says READY (its standby proven).
        for g in range(3):
            make_ready(ports[g], uuids[g], 0, g)
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
        # Group 1: a new term, its standby now primary, NOT READY until it
        # has a standby of its own in sync.
        v1, b1 = after["ha/1"]
        assert b1.split() == [uuids[1], "1", "1", "1"], b1
        # A stale SYNCED of the replaced term changes nothing.
        assert ask(ports[1], f"READY {uuids[1]} 0 1 after=1").startswith("NO term 0 was replaced")
        assert ask(ports[1], f"SYNCED {uuids[1]} 0 1").startswith("NO term 0 was replaced")
        # Group 2: degraded, nothing else. Group 0, placement, leases: unchanged.
        assert after["ha/2"][1].split() == [uuids[2], "0", "1", "0"], after["ha/2"]
        assert after["ha/2"][0] == before["ha/2"][0] + 1
        for m in ["ha/0", "placement", "leases"]:
            assert after[m] == before[m], f"{m} moved: {before[m]} -> {after[m]}"
        assert pairs[0][1].promotes == 0 and pairs[2][1].promotes == 0, "another group's standby promoted"
        # The old term of group 1 is refused; group 0's primary is not.
        assert ask(ports[0], f"DEGRADE {uuids[1]} 0 1").startswith("NO term 0 was replaced")
        make_ready(ports[0], uuids[0], 0, 0)
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
             "--failover-ms", "200", "--poll-ms", "20", "--id", str(i), "--peers", peers, "--new-state", "1"],
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
        # A learned primary is NOT READY (docs/SYNC_READINESS.md); a real
        # one says READY once its standby is proven. Do that here, so the
        # race below is between a grant and a promotion.
        make_ready(ports[0], UUID)
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


def ready_order(build, work):
    """The causal order of NOT READY and READY (docs/SYNC_READINESS.md 7),
    on the witnesses alone: three snc_witness --debug 1 over a fake pair.
    V1, V3 to V6 and the new-witness half of V10 (the mixed binaries are
    tests/test_ready_order.py m1 to m3)."""
    prim, sb = FakeNode("p"), FakeNode("s")
    ports = free_ports(3)
    peers = ",".join(f"127.0.0.1:{p}" for p in ports)
    ws = []
    for i, p in enumerate(ports):
        ws.append(subprocess.Popen(
            [f"{build}/snc_witness", "--port", str(p), "--primary", f"127.0.0.1:{prim.port}",
             "--standby", f"127.0.0.1:{sb.port}", "--state", f"{work}/w{i}.state",
             "--failover-ms", "300", "--poll-ms", "20", "--id", str(i), "--peers", peers, "--debug", "1",
             "--new-state", "1"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    res = {}

    def chosen():
        vals = [ask(p, "READ ha/0") for p in ports]
        vals = [v.split() for v in vals if v and v.startswith("STATE ")]
        if not vals:
            return 0, []
        best = max(vals, key=lambda v: int(v[1]))
        return int(best[1]), best[2:]  # version, [uuid, term, degraded, primary]

    def nr(port=None):
        r = ask(port or ports[0], f"NOTREADY {UUID} 0")
        assert r and r.startswith("OK v="), r
        return int(r[5:])

    try:
        deadline = time.time() + 10
        while time.time() < deadline and chosen()[1][:1] != [UUID]:
            time.sleep(0.02)
        v0, body = chosen()
        assert body == [UUID, "0", "1", "0"], body  # learned: NOT READY
        # NOT READY on NOT READY still chooses a fresh version (7.2), and a
        # retry after a lost reply gets another (V3).
        n1 = nr()
        n2 = nr(ports[1])
        res["fresh_versions"] = (v0, n1, n2)
        assert n1 == v0 + 1 and n2 == n1 + 1, res
        # V1, undelayed: a READY whose base was overtaken is refused.
        r = ask(ports[2], f"READY {UUID} 0 after={n1}")
        assert r == f"NO not ready since v={n2}", r
        assert chosen() == (n2, [UUID, "0", "1", "0"]), chosen()
        # V1, delayed: READY after=N held in every witness's queue; a NOT
        # READY chosen meanwhile; released: refused, still NOT READY.
        assert all(ask(p, "DEBUG HOLD") == "OK" for p in ports)
        held = {}
        t = threading.Thread(target=lambda: held.update(r=ask(ports[0], f"READY {UUID} 0 after={n2}", 10)))
        t.start()
        deadline = time.time() + 5
        while time.time() < deadline and ask(ports[0], "DEBUG HELD") != "HELD 1 1":
            time.sleep(0.01)
        n3 = nr(ports[1])
        assert all(ask(p, "DEBUG RELEASE").startswith("OK") for p in ports)
        t.join()
        res["delayed_ready"] = held.get("r")
        assert held.get("r") == f"NO not ready since v={n3}", held
        assert chosen() == (n3, [UUID, "0", "1", "0"]), chosen()
        # V2 and V4: READY grounded on the latest NOT READY is chosen; a
        # retry (a lost reply) answers OK at the same version, nothing new.
        r = ask(ports[2], f"READY {UUID} 0 after={n3}")
        assert r == f"OK v={n3 + 1}", r
        r = ask(ports[0], f"READY {UUID} 0 after={n3}")
        assert r == f"OK v={n3 + 1}", r
        assert chosen() == (n3 + 1, [UUID, "0", "0", "0"]), chosen()
        # V5: unrelated machines between NOT READY and READY.
        n4 = nr()
        assert ask(ports[1], "PLACEMENT INIT v1 test-map epoch=1") == "OK"
        assert ask(ports[2], "LEASES TAKE 4096").startswith("BLOCK ")
        r = ask(ports[0], f"READY {UUID} 0 after={n4}")
        assert r == f"OK v={n4 + 1}", r
        # Lost COMMITs (independent review of the fix): a value chosen
        # through w0 whose COMMIT nobody gets, w0 then unreachable, so w1
        # and w2 have it accepted but not learned.
        # a. A READY whose base the reader has not learned: not a false
        #    no-op on the older READY; the read finishes the instance first.
        make_ready(ports[0], UUID)
        assert ask(ports[0], "DEBUG SKIP-COMMIT 1") == "OK"
        na = nr(ports[0])
        ws[0].send_signal(signal.SIGSTOP)
        r = ask(ports[1], f"READY {UUID} 0 after={na}", 10)
        res["stale_read_ready"] = (na, r)
        ws[0].send_signal(signal.SIGCONT)
        assert r == f"OK v={na + 1}", res
        # b. A NOT READY whose proposer adopts that unlearned value in phase
        #    1: it must not answer with a version the primary already holds.
        assert ask(ports[0], "DEBUG SKIP-COMMIT 1") == "OK"
        na = nr(ports[0])
        ws[0].send_signal(signal.SIGSTOP)
        r = ask(ports[1], f"NOTREADY {UUID} 0", 10)
        res["adopted_not_ready"] = (na, r)
        ws[0].send_signal(signal.SIGCONT)
        assert r and r.startswith("OK v=") and int(r[5:]) > na, res
        # V10, new witnesses: an old primary's SYNCED is refused and moves
        # nothing; its DEGRADE still grants (plain OK, a fresh version).
        n5 = nr()
        r = ask(ports[1], f"SYNCED {UUID} 0")
        res["old_synced"] = r
        assert r.startswith("NO SYNCED has no causal base"), r
        assert chosen() == (n5, [UUID, "0", "1", "0"]), chosen()
        assert ask(ports[2], f"DEGRADE {UUID} 0") == "OK"
        assert chosen() == (n5 + 1, [UUID, "0", "1", "0"]), chosen()
        # Malformed: READY without its base.
        assert ask(ports[0], f"READY {UUID} 0") == "NO bad request"
        # V6: READY, then the primary goes silent: the standby is promoted
        # (term 1, NOT READY). The old term's NOT READY and READY are refused.
        make_ready(ports[0], UUID)
        prim.answering = False
        deadline = time.time() + 10
        while time.time() < deadline and sb.role != "p":
            time.sleep(0.02)
        assert sb.role == "p" and sb.term == 1, "the standby was not promoted"
        vp, body = chosen()
        assert body == [UUID, "1", "1", "1"], body
        for verb in ("NOTREADY", "SYNCED", "READY"):
            r = ask(ports[1], f"{verb} {UUID} 0" + (f" after={vp}" if verb == "READY" else ""))
            assert r.startswith("NO term 0 was replaced by 1"), (verb, r)
        assert chosen() == (vp, body), chosen()
        # The new primary has no base of its own: its first READY needs a
        # NOT READY of term 1 (V7, as the restarted or promoted primary does).
        r = ask(ports[1], f"READY {UUID} 1 after={vp - 1}")
        assert r == f"NO not ready since v={vp}", r
        n6 = int(ask(ports[1], f"NOTREADY {UUID} 1")[5:])
        assert ask(ports[2], f"READY {UUID} 1 after={n6}") == f"OK v={n6 + 1}"
        print(f"ready order: fresh NOT READY versions {res['fresh_versions']}; lost COMMIT: READY "
              f"{res['stale_read_ready']}, NOT READY {res['adopted_not_ready']}; delayed READY "
              f"refused ({res['delayed_ready']}); idempotent retry; unrelated machines ignored; old "
              f"SYNCED refused; old-term requests refused after promotion")
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
    work = tempfile.mkdtemp(prefix="snc-witness-")
    try:
        ready_order(build, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("test_witness: ok")


if __name__ == "__main__":
    main()
