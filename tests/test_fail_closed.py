#!/usr/bin/env python3
"""Fail-closed migration serving (docs/MIGRATION_FAIL_CLOSED.md): real
snc_server nodes, three snc_witness processes holding the placement (no HA
machines: every promotion here is an admin PROMOTE unless a scenario says
otherwise), snc_ctl moves, and DEBUG REPL-PAUSE to choose exactly which
records a standby lacks.

Two replication groups, each a primary and an ASYNC standby (async or
degraded groups ack a seal or a stage once it is durable locally, which is
what makes the lossy promotion possible).

  F1  D1, destination: the copy ahead reaches B's standby; then B's
      replication is paused, A gains objects and new versions, the move
      commits and activates at B's primary; B's primary dies and its
      standby is promoted.
  F2  D7 path 1, source: A's replication is paused; the move seals, exports
      and stages, placement STAGED; A's primary dies and its standby is
      promoted; a write is sent to the promoted standby; the move commits.
  F3  F1 fixed, plus a create in S and a transfer's credit into S (held).
  F4  D1b (wrong-move STAGED), F5 D1c (older ACTIVE), F5b D1d (UNSEAL of
      an older SEALED): never served.
  F6  F2 fixed: re-sealed at the barrier; acked writes reach B.
  F7  the commit gate waits while a source member answers ROLE m (the
      barrier held open with --debug-barrier); f7b: a source primary that
      serves S (an older build's standby, promoted) makes the driver abort.
  F8/F9 completed move; B restarted, B failed over (sync), A restarted.
  F10 aborts at PREPARED, SEALED, STAGED.   F11 nine moves over 3 groups.
  F13 (f13:NAME) the same with witness promotion.   F15 promotion with the
      control plane down.   F17 (f17:NAME) the standby's directory
      restarted without --standby (fails while the control plane is down).
  F18 a primary's own directory restarted with the control plane down.
  role      DIR/role crash ordering (review item 13A).
  backfill  conservative backfill: refusals, unresolved slots, idempotence.
  stuck     a committed move whose destination is unproven: STUCK, the move
            record cleared so other moves proceed.
  prepared  an offline start of the source's standby at PREPARED seals S.
  f10_reseal  a re-seal at promotion, then an abort: the UNSEAL clears it.
  f7_prepared the gate while a standby promoted at PREPARED holds ROLE m.
  compat    (--old) old binaries, v1 placement, upgrade, backfill; an old
            snc_ctl cannot read v2, so cannot strip arrivals.
  size      placement v2 worst case through real witnesses.
(F12 and the proof-cache cases: tests/test_proof.cpp; v2 text rules:
tests/test_placement.cpp; F14: the probes in F1, F3, F6, F7, F13, F17.)

SNC_CHARACTERIZE=1 asserts the behavior of the code before the fail-closed
slice (the defects reproduced: implementation step 1 of the PR #39 review)
and prints the evidence. Without it, the fixed behavior is asserted.

Usage: test_fail_closed.py BUILD_DIR [--old OLD_BUILD_DIR] [SCENARIO ...]
"""
import os
import random
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
CHAR = os.environ.get("SNC_CHARACTERIZE") == "1"

OK, E_NOT_FOUND, E_STANDBY, E_MOVED, E_FENCED, E_MIGRATING = 0, 1, 11, 16, 17, 18
STATUS = {0: "OK", 1: "E_NOT_FOUND", 11: "E_STANDBY", 12: "E_STALE", 16: "E_MOVED", 17: "E_FENCED",
          18: "E_MIGRATING", 9: "E_OVERLOADED"}
ACCOUNT = 256
OP_DEPOSIT, OP_GET_BALANCE, OP_TRANSFER = 1003, 1006, 1008
# The build of the code before this slice, for the compatibility scenarios
# (compat, f7b): --old DIR, or SNC_OLD_BUILD.
OLD = os.environ.get("SNC_OLD_BUILD")
if "--old" in sys.argv:
    i = sys.argv.index("--old")
    OLD = sys.argv[i + 1]
    del sys.argv[i:i + 2]


def free_ports(n, span=1):
    bases, taken = [], set()
    rnd = random.Random(os.getpid() * 7919 + time.time_ns())
    while len(bases) < n:
        b = rnd.randrange(20000, 32000 - span)
        if any(b + k in taken for k in range(span)):
            continue
        socks = []
        try:
            for k in range(span):
                s = socket.socket()
                socks.append(s)
                s.bind(("127.0.0.1", b + k))
            bases.append(b)
            taken.update(b + k for k in range(span))
        except OSError:
            pass
        finally:
            for s in socks:
                s.close()
    return bases


def ask(port, line, timeout=3):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout) as s:
            s.settimeout(timeout)
            s.sendall((line + "\n").encode())
            return s.makefile().readline().strip()
    except OSError:
        return None


def wait(cond, what, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = cond()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError(f"timed out: {what}")


def oid_bytes(text):
    """ObjectID::to_string is hi then lo, 16 hex digits each; the wire
    carries the struct {u64 hi, u64 lo} little-endian."""
    h = text.replace("-", "")
    return struct.pack("<QQ", int(h[:16], 16), int(h[16:32], 16))


def request(port, kind, op, target=b"\0" * 16, args=b"", timeout=5):
    """One request to a node's client port (any shard forwards to the
    owner): (status, version, id bytes, body)."""
    with socket.create_connection(("127.0.0.1", port), timeout) as s:
        s.settimeout(timeout)
        s.sendall(struct.pack("<IBBHIIQ16sQ", 48 + len(args), kind, 0, 0, op, 0, 1, target, 0) + args)
        buf = b""
        while len(buf) < 48 or len(buf) < struct.unpack_from("<I", buf, 0)[0]:
            chunk = s.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            buf += chunk
        length, _, _, status = struct.unpack_from("<IBBH", buf, 0)
        version, = struct.unpack_from("<Q", buf, 40)
        return status, version, buf[24:40], buf[48:length]


def balance(port, oid):
    st, _, _, body = request(port, 1, OP_GET_BALANCE, oid_bytes(oid))
    return st, (struct.unpack_from("<q", body, 0)[0] if st == OK else None)


def deposit(port, oid, cents):
    st, _, _, _ = request(port, 1, OP_DEPOSIT, oid_bytes(oid), struct.pack("<qQ", cents, 1))
    return st


def transfer(port, src, dst, cents):
    st, _, _, _ = request(port, 1, OP_TRANSFER, oid_bytes(src), oid_bytes(dst) + struct.pack("<qQ", cents, 1))
    return st


class Probe:
    """Samples MOVE INFO serving for (node, slot) pairs on a thread; every
    sample where a node served is kept."""

    def __init__(self, c, pairs, period=0.01):
        import threading
        self.c, self.pairs, self.period = c, pairs, period
        self.served, self.samples, self.stop_ = [], 0, False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        while not self.stop_:
            for k, slot in self.pairs:
                r = self.c.ask(k, f"MOVE INFO {slot}", 1)
                if r and r.startswith("INFO"):
                    self.samples += 1
                    if int(r.split()[8]) == 1:
                        self.served.append((time.time(), k, slot))
            time.sleep(self.period)

    def stop(self):
        self.stop_ = True
        self.t.join()
        return self.served


def create_here(port):
    """One account minted by the node at `port` in a slot it serves (the
    CREATE request with no target); its ID as text."""
    args = struct.pack("<qQBB6s64s96s", 1000000, 1, 1, 1, b"", b"e", b"n")
    st, _, oid, _ = request(port, 2, ACCOUNT, b"\0" * 16, args)
    assert st == OK, st
    hi, lo = struct.unpack("<QQ", oid)
    return f"{hi:016x}{lo:016x}"


def create_near(port, oid):
    """create_near(oid): mints in oid's slot when the node serves it."""
    args = struct.pack("<qQBB6s64s96s", 0, 1, 1, 1, b"", b"e", b"n")
    st, _, new, _ = request(port, 2, ACCOUNT, oid_bytes(oid), args)
    return st


class Cluster:
    """Group g: node 2g+1 starts primary, node 2g+2 its async standby."""

    def __init__(self, work, groups=2, ha=False, async_replica=True, v2=None, how="admin", bin_dir=None):
        self.work, self.groups, self.procs = work, groups, []
        # How a standby takes over in failover(): admin PROMOTE, the
        # witnesses (ha), or offline: its directory restarted without
        # --standby (docs/MIGRATION_FAIL_CLOSED.md 8, F17).
        self.how = "ha" if ha else how
        self.bin = bin_dir or BUILD  # an older build for the compatibility scenarios
        self.witness = {}
        # The fixed code is tested with placement v2 (arrival records); the
        # characterization runs the old binaries' v1 path.
        self.v2 = (not CHAR) if v2 is None else v2
        p = free_ports(3 + 4 * 2 * groups, span=2)
        self.wports = p[:3]
        n = 2 * groups
        nodes = [p[3 + 4 * i: 3 + 4 * (i + 1)] for i in range(n)]
        self.control = ",".join(f"127.0.0.1:{w}" for w in self.wports)
        self.admin = {i + 1: nodes[i][0] for i in range(n)}
        self.link = {i + 1: nodes[i][1] for i in range(n)}
        self.client = {i + 1: nodes[i][2] for i in range(n)}
        self.repl = {i + 1: nodes[i][3] for i in range(n)}  # a node's listener while standby
        self.async_replica = async_replica
        self.server = {}
        groups_arg = ",".join(
            "2:" + "+".join(f"{k}/{self.link[k]}/{self.client[k]}" for k in (2 * g + 1, 2 * g + 2))
            for g in range(groups))
        node_list = ",".join(f"{i}@127.0.0.1:{self.admin[i]}" for i in range(1, n + 1))
        self.wargs = []
        for i, w in enumerate(self.wports):
            args = [f"{self.bin}/snc_witness", "--port", str(w), "--state", f"{work}/w{i}", "--id", str(i),
                    "--peers", self.control, "--failover-ms", "500", "--poll-ms", "50"]
            if ha:
                for g in range(groups):
                    args += ["--group", f"{g}=127.0.0.1:{self.admin[2 * g + 1]},127.0.0.1:{self.admin[2 * g + 2]}"]
            self.wargs.append(args)
            self.start_witness(i)
        time.sleep(0.3)
        r = subprocess.run([f"{self.bin}/snc_ctl", "--control", self.control] +
                           (["--placement-v2"] if self.v2 else []) + ["init", "--nodes", node_list,
                            "--groups", groups_arg], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        for g in range(groups):
            self.start(2 * g + 2, standby_of=2 * g + 1)
            wait(lambda: self.role(2 * g + 2) == "s", f"standby {2 * g + 2} up")
            self.start(2 * g + 1)
            wait(lambda: self.role(2 * g + 1) in ("p", "n"), f"primary {2 * g + 1} up")

    def start_witness(self, i):
        self.witness[i] = subprocess.Popen(self.wargs[i], stdout=open(f"{self.work}/w{i}.log", "a"),
                                           stderr=subprocess.STDOUT)
        self.procs.append(self.witness[i])

    def control_down(self):
        for i in self.witness:
            if self.witness[i].poll() is None:
                self.witness[i].kill()
                self.witness[i].wait()

    def control_up(self):
        for i in self.witness:
            self.start_witness(i)
        wait(lambda: self.placement().startswith("v"), "control plane back")

    def failover(self, dead, new):
        """Primary `dead` is gone (killed by the caller); its standby `new`
        takes over, as self.how says."""
        if self.how == "ha":
            wait(lambda: self.role(new) in ("p", "n"), f"witnesses promoted node {new}", timeout=30)
        elif self.how == "offline":
            self.kill(new)
            # F17: a former standby's directory restarted as a primary: with
            # the control plane down, its start barrier cannot run: it
            # must not start.
            self.control_down()
            self.start(new)
            rc = self.server[new].wait(timeout=60)
            assert rc != 0, "a former standby started with no control plane"
            log = open(f"{self.work}/n{new}.log").read()
            assert "start barrier" in log, log[-2000:]
            self.control_up()
            self.start(new)
            wait(lambda: self.role(new) in ("p", "n"), f"node {new} restarted as a primary", timeout=30)
        else:
            self.promote(new)

    def group_of(self, k):
        return (k - 1) // 2

    def peer(self, k):
        return k + 1 if k % 2 else k - 1

    def start(self, k, standby_of=None, extra=(), bin_dir=None):
        """Starts node k: a standby (listening for standby_of) or a primary
        shipping to its peer's listener."""
        args = [f"{bin_dir or self.bin}/snc_server", "--shards", "2", "--objects", "20000", "--data-dir", f"{self.work}/n{k}",
                "--control", self.control, "--node-id", str(k), "--admin-port", str(self.admin[k]),
                "--debug-admin"]
        if standby_of is not None:
            args += ["--standby", str(self.repl[k])]
        else:
            args += ["--replica-of", f"127.0.0.1:{self.repl[self.peer(k)]}", "--replica-timeout-ms", "300"]
            if self.async_replica:
                args += ["--async-replica"]
        args += list(extra)
        self.server[k] = subprocess.Popen(args, stdout=open(f"{self.work}/n{k}.log", "a"), stderr=subprocess.STDOUT)
        self.procs.append(self.server[k])

    def kill(self, k):
        if self.server[k].poll() is None:
            self.server[k].send_signal(signal.SIGKILL)
        self.server[k].wait()

    def ask(self, k, line, timeout=3):
        return ask(self.admin[k], line, timeout)

    def role(self, k):
        r = self.ask(k, "MOVE ROLE", 1)
        return r.split()[1] if r and r.startswith("ROLE") else None

    def info(self, k, slot):
        """MOVE INFO as a dict (tools/migrate.h report_text)."""
        r = self.ask(k, f"MOVE INFO {slot}")
        assert r and r.startswith("INFO"), r
        f = r.split()[1:]
        names = ["phase", "group", "move", "epoch", "count", "digest", "durable", "serving", "map_group",
                 "map_epoch", "live_count", "live_digest"]
        d = {n: (int(v, 16) if n == "move" else int(v)) for n, v in zip(names, f[:12])}
        for kv in f[12:]:
            if "=" in kv:
                a, b = kv.split("=", 1)
                d[a] = b
        return d

    def promote(self, k):
        r = self.ask(k, "PROMOTE")
        assert r == "OK", r
        wait(lambda: self.role(k) == "p" or self.role(k) == "n", f"node {k} promoted")

    def ctl(self, *args, timeout=120, bin_dir=None):
        # (An older snc_ctl has no such flag.)
        v2 = ["--placement-v2"] if self.v2 and bin_dir is None and args and args[0] in ("move", "placement") else []
        r = subprocess.run([f"{bin_dir or self.bin}/snc_ctl", "--control", self.control] + v2 +
                           [str(a) for a in args], capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip(), r.stderr

    def placement(self):
        out, _ = self.ctl("get")
        return out

    def create(self, k, n, near=None, in_slot=0):
        """n accounts through node k's client port, in near's slot if given;
        with in_slot, in_slot more in the first one's slot. Returns (IDs,
        the slot snc_load reports or None)."""
        ids = f"{self.work}/ids-{time.time_ns()}"
        cmd = [f"{self.bin}/snc_load", "create", "--connect", f"127.0.0.1:{self.client[k]}", "--accounts", str(n),
               "--ids", ids]
        if near:
            cmd += ["--near", near]
        if in_slot:
            cmd += ["--in-slot", str(in_slot)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, r.stdout + r.stderr
        slot = int(r.stdout.rsplit(" in slot ", 1)[1]) if " in slot " in r.stdout else None
        return open(ids).read().split(), slot

    def caught_up(self, k, slot, count):
        """Standby k holds `count` objects of slot."""
        return self.info(k, slot)["live_count"] == count

    def stop(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()


def statuses(port, ids):
    out = {}
    for i in ids:
        st, _ = balance(port, i)
        out[STATUS.get(st, st)] = out.get(STATUS.get(st, st), 0) + 1
    return out


def owner_primary(c, slot):
    """The primary (node) whose group the map gives slot."""
    g = c.info(1, slot)["map_group"]
    return 2 * g + 1


def scenario_f1(c, ev):
    """D1: the destination's standby, promoted, lacks STAGED and part of the copy."""
    ids, S = c.create(1, 1, in_slot=30)
    seed = ids[0]
    ids = [seed] + ids[1:]
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qb = c.peer(b)
    n0 = len(ids)
    ev.update(slot=S, source=a, dest=b, dest_standby=qb, accounts_before=n0)
    # 1. The copy ahead of the seal reaches B and its standby.
    out, err = c.ctl("move", "--stop-after", "copied", S, c.group_of(b))
    assert out == "STOPPED copied", out + err
    wait(lambda: c.caught_up(qb, S, n0), "B's standby holds the copy ahead")
    # 2. B ships nothing more. A gains objects and new versions.
    assert c.ask(b, "DEBUG REPL-PAUSE 1") == "OK"
    more, _ = c.create(a, 10, near=seed)
    for i in ids[:5]:
        assert deposit(c.client[a], i, 100) == OK
    n1 = n0 + len(more)
    # 3. The move finishes at B's primary (local acks only).
    out, err = c.ctl("move", "--stop-after", "activated", S, c.group_of(b))
    assert out == "STOPPED activated", out + err
    ib = c.info(b, S)
    assert ib["serving"] == 1 and ib["live_count"] == n1, ib
    # 4. B's primary dies; its standby is promoted (sampled throughout: F14).
    probe = None if CHAR else Probe(c, [(qb, S)])
    c.kill(b)
    c.failover(b, qb)
    wait(lambda: c.info(qb, S)["map_group"] == c.group_of(b), "promoted standby has the committed map")
    iq = c.info(qb, S)
    ev.update(accounts_committed=n1, promoted_info=iq)
    st_old = statuses(c.client[qb], ids)
    st_new = statuses(c.client[qb], more)
    ev.update(read_first_accounts=st_old, read_later_accounts=st_new)
    _, bal = balance(c.client[qb], ids[0])
    ev["balance_of_deposited_account"] = bal
    ev["create_in_slot"] = STATUS.get(create_near(c.client[qb], seed))
    if CHAR:
        # Before the fix: the promoted standby serves S from its partial,
        # stale copy (phase NONE, no slot state for S at all).
        assert iq["phase"] == 0 and iq["serving"] == 1 and iq["live_count"] == n0, iq
        assert st_new == {"E_NOT_FOUND": len(more)}, st_new
        assert st_old == {"OK": n0}, st_old
        assert bal == 1000000, bal  # the 100-cent deposit acked at A is missing
        assert ev["create_in_slot"] == "OK"
    else:
        assert iq["serving"] == 0 and iq.get("proof") == "missing", iq
        assert st_old == {"E_MIGRATING": n0} and st_new == {"E_MIGRATING": len(more)}, (st_old, st_new)
        assert ev["create_in_slot"] == "E_MIGRATING"
        served = probe.stop()
        ev["probe"] = f"{probe.samples} samples, served {len(served)}"
        assert probe.samples > 0 and not served, served
    return S, qb, ids, more


def scenario_f2(c, ev):
    """D7 path 1: the source's standby, promoted, lacks SEALED; the move commits."""
    ids, S = c.create(1, 1, in_slot=30)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    n0 = len(ids)
    ev.update(slot=S, source=a, source_standby=qa, dest=b, accounts=n0)
    wait(lambda: c.caught_up(qa, S, n0), "A's standby holds the slot")
    # 1. A ships nothing more; the move seals, exports and stages: placement STAGED.
    assert c.ask(a, "DEBUG REPL-PAUSE 1") == "OK"
    out, err = c.ctl("move", "--stop-after", "staged", S, c.group_of(b))
    assert out == "STOPPED staged", out + err
    assert c.info(a, S)["phase"] == 2  # sealed at A's primary
    # 2. A's primary dies; its standby (no SEALED) is promoted.
    c.kill(a)
    c.failover(a, qa)
    iq = c.info(qa, S)
    ev["promoted_info"] = iq
    w = deposit(c.client[qa], ids[0], 777)
    ev["write_at_promoted_source"] = STATUS.get(w, w)
    # 3. The move commits; B activates.
    out, err = c.ctl("move", "--stop-after", "committed", S, c.group_of(b))
    ev["commit"] = out
    r = c.ask(b, f"MOVE ACTIVATE {S}")
    ev["dest_activate"] = r.split()[0] if r else None
    two = c.info(qa, S)["serving"] == 1 and c.info(b, S)["serving"] == 1
    ev["two_serving_after_commit"] = two
    w2 = deposit(c.client[qa], ids[1], 555)
    ev["write_at_promoted_source_after_commit"] = STATUS.get(w2, w2)
    out, err = c.ctl("move", S, c.group_of(b))
    ev["finish"] = out
    _, bal_b = balance(c.client[b], ids[0])
    ev["balance_at_dest"] = bal_b
    if CHAR:
        assert iq["phase"] == 0 and iq["serving"] == 1, iq
        assert w == OK
        assert out == "DONE" and ev["commit"] == "STOPPED committed"
        assert bal_b == 1000000, bal_b  # the write acked by the promoted source is lost
    else:
        assert iq["serving"] == 0 and iq["phase"] == 2, iq  # re-sealed at the barrier
        assert w == E_MIGRATING
        assert not two
        assert out == "DONE" and bal_b == 1000000


def scenario_f4(c, ev):
    """D1b: an aborted move's STAGED at B; a later move of S to B commits
    while B's standby lacks its STAGED."""
    ids, S = c.create(1, 1, in_slot=20)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qb = c.peer(b)
    out, _ = c.ctl("move", "--stop-after", "staged", S, c.group_of(b))
    assert out == "STOPPED staged", out
    m1 = c.info(b, S)
    out, _ = c.ctl("move", "--abort", S)
    assert out == "ABORTED", out
    wait(lambda: c.info(qb, S)["phase"] == 3, "B's standby holds STAGED(M1)")
    more, _ = c.create(a, 10, near=ids[0])  # after M1's copy
    assert c.ask(b, "DEBUG REPL-PAUSE 1") == "OK"
    out, _ = c.ctl("move", "--stop-after", "activated", S, c.group_of(b))
    assert out == "STOPPED activated", out
    m3 = c.info(b, S)
    ev.update(slot=S, m1=hex(m1["move"]), m1_epoch=m1["epoch"], m3=hex(m3["move"]), m3_epoch=m3["epoch"])
    c.kill(b)
    c.failover(b, qb)
    wait(lambda: c.info(qb, S)["map_group"] == c.group_of(b), "promoted standby has the committed map")
    iq = c.info(qb, S)
    ev["promoted_info"] = iq
    ev["read_later_accounts"] = statuses(c.client[qb], more)
    if CHAR:
        # Activated M1's stale copy: ACTIVE with M1's ID, serving, lacking `more`.
        assert iq["phase"] == 1 and iq["move"] == m1["move"] and iq["serving"] == 1, iq
        assert ev["read_later_accounts"] == {"E_NOT_FOUND": len(more)}
    else:
        assert iq["phase"] == 3 and iq["move"] == m1["move"] and iq["serving"] == 0, iq
        assert iq.get("proof") == "missing", iq
        assert ev["read_later_accounts"] == {"E_MIGRATING": len(more)}


def scenario_f5(c, ev):
    """D1c: S arrives at B (M3), leaves (M5), returns (M7); B's standby's
    prefix ends at ACTIVE(M3)."""
    ids, S = c.create(1, 1, in_slot=20)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qb = c.peer(b)
    assert c.ctl("move", S, c.group_of(b))[0] == "DONE"
    wait(lambda: c.info(qb, S)["phase"] == 1, "B's standby holds ACTIVE(M3)")
    m3 = c.info(qb, S)["move"]
    assert c.ask(b, "DEBUG REPL-PAUSE 1") == "OK"
    assert c.ctl("move", S, c.group_of(a))[0] == "DONE"
    more, _ = c.create(a, 10, near=ids[0])  # while S is back at A
    assert c.ctl("move", "--stop-after", "activated", S, c.group_of(b))[0] == "STOPPED activated"
    c.kill(b)
    c.failover(b, qb)
    wait(lambda: c.info(qb, S)["map_group"] == c.group_of(b), "promoted standby has the committed map")
    iq = c.info(qb, S)
    ev.update(slot=S, m3=hex(m3), promoted_info=iq, read_later_accounts=statuses(c.client[qb], more))
    if CHAR:
        assert iq["phase"] == 1 and iq["move"] == m3 and iq["serving"] == 1, iq
        assert ev["read_later_accounts"] == {"E_NOT_FOUND": len(more)}
    else:
        assert iq["serving"] == 0 and iq.get("proof") == "missing", iq
        assert ev["read_later_accounts"] == {"E_MIGRATING": len(more)}


def scenario_f5b(c, ev):
    """D1d: S leaves A (M5) and returns (M7); A's standby's prefix ends at
    SEALED(M5); a later driver run UNSEALs it."""
    ids, S = c.create(1, 1, in_slot=20)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    assert c.ctl("move", S, c.group_of(b))[0] == "DONE"
    wait(lambda: c.info(qa, S)["phase"] == 2, "A's standby holds SEALED(M5)")
    m5 = c.info(qa, S)["move"]
    assert c.ask(a, "DEBUG REPL-PAUSE 1") == "OK"
    more, _ = c.create(b, 10, near=ids[0])  # while S is at B
    assert c.ctl("move", S, c.group_of(a))[0] == "DONE"  # M7 finished: no move in the placement
    c.kill(a)
    c.failover(a, qa)
    wait(lambda: c.info(qa, S)["map_group"] == c.group_of(a), "promoted standby has the committed map")
    before = c.info(qa, S)
    out, _ = c.ctl("move", S, c.group_of(a))  # nothing to move: settle_owner UNSEALs at the owner
    iq = c.info(qa, S)
    ev.update(slot=S, m5=hex(m5), before_unseal=before, driver=out, promoted_info=iq,
              read_later_accounts=statuses(c.client[qa], more))
    if CHAR:
        assert before["phase"] == 2 and before["serving"] == 0, before
        assert out == "DONE"
        assert iq["phase"] == 0 and iq["move"] == m5 and iq["serving"] == 1, iq  # unsealed, pre-M5 image
        assert ev["read_later_accounts"] == {"E_NOT_FOUND": len(more)}
    else:
        assert iq["serving"] == 0 and iq.get("proof") == "missing", iq
        assert ev["read_later_accounts"] == {"E_MIGRATING": len(more)}


def read_role(c, k):
    try:
        return open(f"{c.work}/n{k}/role").read()
    except FileNotFoundError:
        return None


def arrivals(c):
    """The placement's arrivals: {slot: (move, epoch)}."""
    out = {}
    for tok in c.placement().split():
        if tok.startswith("arrived="):
            for e in tok[8:].split(","):
                sl, mv, ep = e.split(":")
                out[int(sl)] = (int(mv, 16), int(ep))
    return out


def epoch_of(c):
    for tok in c.placement().split():
        if tok.startswith("epoch="):
            return int(tok[6:])
    return None


def scenario_f3(c, ev):
    """F3: D1 with the fix, plus the paths besides client ops: a create in
    S and the credit of a transfer into S (a follow-up) at the promoted
    standby. (Index lookups in an unproven slot: test_proof, case C.)"""
    S, qb, ids, more = scenario_f1(c, ev)
    if CHAR:
        return
    before = c.info(qb, S)
    args = struct.pack("<qQBB6s64s96s", 1000000, 1, 1, 1, b"", b"e", b"n")
    st, _, src, _ = request(c.client[qb], 2, ACCOUNT, b"\0" * 16, args)  # minted in a slot qb serves
    assert st == OK, st
    st, _, _, _ = request(c.client[qb], 1, OP_TRANSFER, src, oid_bytes(ids[0]) + struct.pack("<qQ", 100, 1))
    ev["transfer_into_unproven"] = STATUS.get(st, st)
    assert st == OK, ev  # the debit is the source's; the credit waits
    time.sleep(1.0)
    after = c.info(qb, S)
    ev["slot_unchanged"] = (before["live_count"], before["live_digest"]) == (after["live_count"], after["live_digest"])
    assert ev["slot_unchanged"], (before, after)
    status = c.ask(qb, "STATUS")
    ev["status_unproven"] = [t for t in status.split() if t.startswith("unproven=")]
    assert "unproven=1" in status.split(), status


def scenario_f6(c, ev):
    """F6: D7 with the fix (F2's fixed assertions) plus: a deposit acked by
    the source's primary before its replication paused is at B."""
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    assert deposit(c.client[a], ids[0], 4242) == OK
    wait(lambda: c.info(qa, S)["live_digest"] == c.info(a, S)["live_digest"], "A's standby caught up")
    assert c.ask(a, "DEBUG REPL-PAUSE 1") == "OK"
    out, _ = c.ctl("move", "--stop-after", "staged", S, c.group_of(b))
    assert out == "STOPPED staged", out
    probe = Probe(c, [(qa, S)])
    c.kill(a)
    c.failover(a, qa)
    iq = c.info(qa, S)
    w = deposit(c.client[qa], ids[1], 1)
    out, err = c.ctl("move", S, c.group_of(b))
    served = probe.stop()
    _, bal = balance(c.client[b], ids[0])
    ev.update(slot=S, promoted_info=iq, write=STATUS.get(w, w), finish=out, balance_at_b=bal,
              probe=f"{probe.samples} samples, served {len(served)}")
    assert iq["phase"] == 2 and iq["serving"] == 0, iq
    assert w == E_MIGRATING and out == "DONE", (w, out, err)
    assert bal == 1000000 + 4242, bal
    assert not served, served
    assert c.info(qa, S)["serving"] == 0


def scenario_f7(c, ev):
    """F7: the commit gate while a source member is still promoting (ROLE m,
    the barrier held open with --debug-barrier): the driver waits; once the
    member is a primary it has re-sealed S; the move commits; S is never
    served by two groups."""
    import threading
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    c.kill(qa)
    c.start(qa, standby_of=a, extra=["--debug-barrier", "hold:4000"])
    wait(lambda: c.role(qa) == "s", "A's standby back")
    wait(lambda: c.caught_up(qa, S, len(ids)), "A's standby caught up")
    assert c.ask(a, "DEBUG REPL-PAUSE 1") == "OK"
    out, _ = c.ctl("move", "--stop-after", "staged", S, c.group_of(b))
    assert out == "STOPPED staged", out
    probe = Probe(c, [(qa, S), (b, S)], period=0.005)
    c.kill(a)
    assert c.ask(qa, "PROMOTE") == "OK"
    wait(lambda: c.role(qa) == "m", "ROLE m while the barrier is held")
    res = {}
    t = threading.Thread(target=lambda: res.update(r=c.ctl("move", S, c.group_of(b))))
    t.start()
    roles = set()
    while t.is_alive():
        roles.add(c.role(qa))
        time.sleep(0.05)
    t.join()
    out, err = res["r"]
    served = probe.stop()
    both = [x for x in served if x[1] == qa]
    ev.update(slot=S, roles_seen=sorted(r for r in roles if r), driver=out,
              gate_waited="is promoting" in err, probe=f"{probe.samples} samples")
    assert out == "DONE", out + err
    assert ev["gate_waited"], err
    assert not both, both  # the promoted source never served S
    assert c.info(qa, S)["serving"] == 0 and c.info(b, S)["serving"] == 1


def scenario_f7b(c, ev):
    """F7, a reachable source that serves S at the gate (here: its standby
    runs the build before this slice, with no start barrier, and is
    promoted): the gate refuses, the driver aborts; B never activates."""
    assert OLD, "needs --old BUILD_DIR"
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    c.kill(qa)
    c.start(qa, standby_of=a, bin_dir=OLD)
    wait(lambda: c.role(qa) == "s", "A's (old) standby back")
    wait(lambda: c.caught_up(qa, S, len(ids)), "A's standby caught up")
    assert c.ask(a, "DEBUG REPL-PAUSE 1") == "OK"
    out, _ = c.ctl("move", "--stop-after", "staged", S, c.group_of(b))
    assert out == "STOPPED staged", out
    c.kill(a)
    c.promote(qa)
    assert c.info(qa, S)["serving"] == 1  # no barrier: serves S
    out, err = c.ctl("move", S, c.group_of(b))
    ib = c.info(b, S)
    ev.update(slot=S, driver=out, dest_info=ib, gate="serves the slot" in err)
    assert out.startswith("ABORTED"), out + err
    assert ev["gate"], err[-2000:]
    assert ib["serving"] == 0 and ib["phase"] == 3, ib  # inert
    assert c.info(qa, S)["map_group"] == c.group_of(a)


def scenario_f7b_cluster():
    return dict(v2=False)


def scenario_f8_f9(c, ev):
    """F8, F9 (sync replication): a completed move; then B's primary
    restarted, then a healthy failover of B, then A's primary restarted.
    S serves at B after each with every acked write; A never serves it;
    a never-moved slot serves throughout."""
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa, qb = c.peer(a), c.peer(b)
    other = [create_here(c.client[a])]  # never moved (Ma = 0)
    assert c.ctl("move", S, c.group_of(b))[0] == "DONE"
    for i in ids:
        assert deposit(c.client[b], i, 7) == OK
    probe = Probe(c, [(a, S), (qa, S)])
    c.kill(b)
    c.start(b)
    wait(lambda: c.role(b) in ("p", "n"), "B's primary restarted")
    wait(lambda: c.info(b, S)["serving"] == 1, "S served at B after its restart")
    assert statuses(c.client[b], ids) == {"OK": len(ids)}
    assert deposit(c.client[b], ids[0], 1) == OK
    wait(lambda: c.caught_up(qb, S, len(ids)) and c.info(qb, S)["live_digest"] == c.info(b, S)["live_digest"],
         "B's standby in sync")
    c.kill(b)
    c.promote(qb)
    wait(lambda: c.info(qb, S)["serving"] == 1, "S served at B's promoted standby")
    bals = [balance(c.client[qb], i)[1] for i in ids]
    c.kill(a)
    c.start(a)
    wait(lambda: c.role(a) in ("p", "n"), "A's primary restarted")
    ia = c.info(a, S)
    served = probe.stop()
    ev.update(slot=S, balances_ok=bals == [1000007 + (1 if j == 0 else 0) for j in range(len(ids))],
              a_after_restart=ia, probe=f"{probe.samples} samples, A served {len(served)}",
              never_moved=STATUS.get(balance(c.client[a], other[0])[0]))
    assert ev["balances_ok"], bals
    assert ia["serving"] == 0 and not served, (ia, served)
    assert ev["never_moved"] == "OK"


def scenario_f8_f9_cluster():
    return dict(async_replica=False)


def scenario_f10(c, ev):
    """F10: moves aborted at PREPARED, SEALED and STAGED: A serves S again
    (after its UNSEAL); B never activates; a later move completes."""
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    for stop in ("prepared", "sealed", "staged"):
        out, _ = c.ctl("move", "--stop-after", stop, S, c.group_of(b))
        assert out == f"STOPPED {stop}", out
        out, err = c.ctl("move", "--abort", S)
        assert out == "ABORTED", out + err
        ia, ib = c.info(a, S), c.info(b, S)
        ev[stop] = dict(a_serving=ia["serving"], a_flags=ia.get("flags"), b_phase=ib["phase"], b_serving=ib["serving"])
        assert ia["serving"] == 1 and ib["serving"] == 0 and ib["phase"] != 1, (stop, ia, ib)
        assert deposit(c.client[a], ids[0], 1) == OK
    assert c.ctl("move", S, c.group_of(b))[0] == "DONE"
    _, bal = balance(c.client[b], ids[0])
    ev["final_balance"] = bal
    assert bal == 1000003 and c.info(b, S)["serving"] == 1 and c.info(a, S)["serving"] == 0


def scenario_f11(c, ev):
    """F11: S moved A to B to C to A three times; after each move one
    group serves it, the placement holds one arrival for it (the latest
    move), and every acked deposit is there."""
    ids, S = c.create(1, 1, in_slot=10)
    g0 = c.info(1, S)["map_group"]
    order = [(g0 + k) % 3 for k in range(1, 10)]
    for n, g in enumerate(order, 1):
        out, err = c.ctl("move", S, g)
        assert out == "DONE", out + err
        owner = 2 * g + 1
        assert deposit(c.client[owner], ids[0], 1) == OK
        serving = [k for k in range(1, 7, 2) if c.info(k, S)["serving"] == 1]
        arr = arrivals(c)
        assert serving == [owner], serving
        assert list(arr) == [S] and arr[S][0] == c.info(owner, S)["move"], arr
    _, bal = balance(c.client[2 * order[-1] + 1], ids[0])
    ev.update(slot=S, moves=len(order), arrivals=len(arrivals(c)), balance=bal)
    assert bal == 1000000 + len(order)


def scenario_f11_cluster():
    return dict(groups=3)


def scenario_f15(c, ev):
    """F15: a promotion with the control plane down is refused (nothing
    changes; still a standby); with it back, the promotion runs."""
    ids, S = c.create(1, 1, in_slot=5)
    a = owner_primary(c, S)
    qa = c.peer(a)
    wait(lambda: c.caught_up(qa, S, len(ids)), "A's standby caught up")
    c.control_down()
    c.kill(a)
    assert c.ask(qa, "PROMOTE") == "OK"
    wait(lambda: "promote refused" in open(f"{c.work}/n{qa}.log").read(), "promotion refused", timeout=60)
    ev.update(role_after=c.role(qa), info=c.info(qa, S), role_file=read_role(c, qa))
    assert ev["role_after"] == "s" and ev["role_file"] == "standby\n"
    c.control_up()
    c.promote(qa)
    assert c.info(qa, S)["serving"] == 1 and read_role(c, qa) == "primary\n"


def scenario_f18(c, ev):
    """F18: a primary's own directory restarted with the control plane down
    starts on its stored map: proven slots (moved under v2, and never
    moved) serve."""
    ids, S = c.create(1, 1, in_slot=5)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    other = [create_here(c.client[b])]
    assert c.ctl("move", S, c.group_of(b))[0] == "DONE"
    c.control_down()
    c.kill(b)
    c.start(b)
    wait(lambda: c.role(b) in ("p", "n"), "B restarted with no control plane", timeout=60)
    ib = c.info(b, S)
    ev.update(slot=S, info=ib, moved=statuses(c.client[b], ids),
              never_moved=STATUS.get(balance(c.client[b], other[0])[0]))
    assert ib["serving"] == 1 and ib.get("proof") == "ok", ib
    assert ev["moved"] == {"OK": len(ids)} and ev["never_moved"] == "OK"


def scenario_role(c, ev):
    """A: DIR/role crash ordering. A standby says so on disk; a crash at
    the end of the start barrier (re-seal durable, role not yet primary)
    leaves "standby"; a former standby, or a role file malformed,
    truncated or missing after role files exist, starts only through the
    barrier: never with the control plane down."""
    ids, S = c.create(1, 1, in_slot=5)
    a = owner_primary(c, S)
    qa = c.peer(a)
    ev["roles"] = (read_role(c, a), read_role(c, qa))
    assert ev["roles"] == ("primary\n", "standby\n")
    c.kill(qa)
    c.start(qa, standby_of=a, extra=["--debug-barrier", "crash"])
    wait(lambda: c.role(qa) == "s", "standby back")
    wait(lambda: c.caught_up(qa, S, len(ids)), "standby caught up")
    c.kill(a)
    assert c.ask(qa, "PROMOTE") == "OK"
    rc = c.server[qa].wait(timeout=60)
    ev["crash_rc"] = rc
    assert rc == 9 and read_role(c, qa) == "standby\n", (rc, read_role(c, qa))

    def must_not_start_then_start(what):
        c.control_down()
        c.start(qa)
        rc = c.server[qa].wait(timeout=60)
        assert rc != 0, f"{what}: started with no control plane"
        c.control_up()
        c.start(qa)
        wait(lambda: c.role(qa) in ("p", "n"), f"{what}: started through the barrier", timeout=30)
        assert read_role(c, qa) == "primary\n"
        assert statuses(c.client[qa], ids) == {"OK": len(ids)}
        c.kill(qa)

    must_not_start_then_start("after the crash")
    for what, content in (("truncated", "prim"), ("malformed", "primary\nx"), ("empty", ""),
                          ("missing", None)):
        path = f"{c.work}/n{qa}/role"
        if content is None:
            os.unlink(path)
        else:
            open(path, "w").write(content)
        must_not_start_then_start(what)
    # A crash while the barrier is held open (ROLE m): still "standby".
    open(f"{c.work}/n{qa}/role", "w").write("standby\n")
    c.start(qa, extra=["--debug-barrier", "hold:3000"])
    wait(lambda: "start barrier held" in open(f"{c.work}/n{qa}.log").read(), "barrier held", timeout=60)
    c.kill(qa)
    assert read_role(c, qa) == "standby\n"
    c.start(qa)
    wait(lambda: c.role(qa) in ("p", "n"), "started after the held barrier", timeout=30)
    ev["final_role"] = read_role(c, qa)
    assert ev["final_role"] == "primary\n"


def scenario_backfill(c, ev):
    """F16 (new binaries, v1 placement) and F (ambiguous evidence): the
    backfill writes only unambiguous arrivals, refuses while a move is in
    progress or a group has no primary (writing nothing), reports what it
    cannot settle, and is idempotent."""
    ids, S1 = c.create(1, 1, in_slot=3)
    a = owner_primary(c, S1)
    b = 3 if a == 1 else 1
    near = {}
    for name in ("S2", "S3", "S4"):
        x, sl = c.create(a, 1, in_slot=3)
        near[name] = sl
    S2, S3, S4 = near["S2"], near["S3"], near["S4"]
    assert len({S1, S2, S3, S4}) == 4
    # S1: moved to B (clean). S2: aborted at STAGED (A unsealed, B STAGED).
    # S3: moved to B, then back to A aborted at STAGED (B unsealed).
    assert c.ctl("move", S1, c.group_of(b))[0] == "DONE"
    assert c.ctl("move", "--stop-after", "staged", S2, c.group_of(b))[0] == "STOPPED staged"
    assert c.ctl("move", "--abort", S2)[0] == "ABORTED"
    assert c.ctl("move", S3, c.group_of(b))[0] == "DONE"
    assert c.ctl("move", "--stop-after", "staged", S3, c.group_of(a))[0] == "STOPPED staged"
    assert c.ctl("move", "--abort", S3)[0] == "ABORTED"
    assert c.placement().startswith("v1 ")
    p0 = c.placement()
    c.v2 = True
    # A move in progress: refused, nothing written.
    assert c.ctl("move", "--stop-after", "prepared", S4, c.group_of(b))[0] == "STOPPED prepared"
    out, _ = c.ctl("placement", "backfill")
    ev["in_progress"] = out
    assert out.startswith("NO a move is in progress"), out
    assert c.ctl("move", "--abort", S4)[0] == "ABORTED"
    p1 = c.placement()
    # A group with no primary: refused, nothing written.
    c.kill(b)
    out, _ = c.ctl("placement", "backfill", timeout=180)
    ev["no_primary"] = out
    assert out.startswith("NO") or out.startswith("STUCK"), out
    assert c.placement() == p1
    c.start(b)
    wait(lambda: c.role(b) in ("p", "n"), "B back")
    out, _ = c.ctl("placement", "backfill")
    ev["backfill"] = out
    arr = arrivals(c)
    ev["arrived"] = {k: (hex(v[0]), v[1]) for k, v in arr.items()}
    assert out.startswith("BACKFILLED 1 "), out
    # S2 never arrived anywhere (its owner's record: NONE, unsealed); S3 was unsealed.
    assert f"{S2}:owner-not-active" in out and f"{S3}:owner-unsealed" in out, out
    assert list(arr) == [S1] and arr[S1][0] == c.info(b, S1)["move"], arr
    assert c.placement().startswith("v2 ")
    wait(lambda: c.info(b, S1).get("arrived", "0:0") != "0:0", "B adopted the backfilled placement")
    assert c.info(b, S1)["serving"] == 1
    e = epoch_of(c)
    out, _ = c.ctl("placement", "backfill")
    ev["again"] = out
    assert out.startswith("NOTHING; UNRESOLVED 2"), out
    assert epoch_of(c) == e  # idempotent: nothing written
    ev["v1_epoch_then_v2_epoch"] = (p0.split()[2], e)


def scenario_backfill_cluster():
    return dict(v2=False)


def scenario_compat(c, ev):
    """F16 and B with the build before this slice: a v1 cluster of old
    binaries moves S; every node restarts on the new build (pre-upgrade
    directories: no role file, superblock without the role flag: they
    start as primaries, even with the control plane down); S, moved before
    v2, still serves (documented: not protected until backfilled); the
    backfill records it; an old snc_ctl then cannot read the placement, so
    it cannot strip the arrival."""
    assert OLD, "needs --old BUILD_DIR"
    ids, S = c.create(1, 1, in_slot=5)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    assert c.ctl("move", S, c.group_of(b))[0] == "DONE"
    for k in sorted(c.server):
        c.kill(k)
    assert read_role(c, a) is None
    c.bin = BUILD
    c.control_down()
    for g in range(c.groups):
        c.start(2 * g + 1)
    for g in range(c.groups):
        wait(lambda: c.role(2 * g + 1) in ("p", "n"), f"pre-upgrade primary {2 * g + 1} up, control down", timeout=60)
    ev["roles"] = {k: read_role(c, k) for k in (1, 3)}
    assert ev["roles"] == {1: "primary\n", 3: "primary\n"}
    c.control_up()
    ib = c.info(b, S)
    ev["moved_before_v2"] = dict(serving=ib["serving"], proof=ib.get("proof"), arrived=ib.get("arrived"))
    assert ib["serving"] == 1 and ib.get("arrived") == "0:0"
    c.v2 = True
    out, _ = c.ctl("placement", "backfill")
    ev["backfill"] = out
    assert out.startswith("BACKFILLED 1 "), out
    after = c.placement()
    out_old, err_old = c.ctl("get", bin_dir=OLD)
    mv_old, mv_err = c.ctl("move", S, c.group_of(a), bin_dir=OLD, timeout=120)
    ev.update(old_get=(out_old[:40], err_old.strip()[:120]), old_move=(mv_old[:60], mv_err.strip()[-120:]))
    assert c.placement() == after  # the old writer changed nothing
    assert "arrived=" in after and after.startswith("v2 ")
    wait(lambda: c.info(b, S).get("arrived", "0:0") != "0:0", "B adopted the backfilled placement")
    assert c.info(b, S)["serving"] == 1


def scenario_compat_cluster():
    return dict(v2=False, bin_dir=OLD)


def scenario_stuck(c, ev):
    """A move committed whose destination standby, promoted, lacks STAGED:
    the driver reports STUCK (the slot stays unavailable: no repair yet)
    and clears the move record, so other moves are not blocked; the
    arrival stays on record and the source keeps its seal."""
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qb = c.peer(b)
    _, S2 = c.create(a, 1, in_slot=3)
    assert S2 != S
    assert c.ask(b, "DEBUG REPL-PAUSE 1") == "OK"
    out, _ = c.ctl("move", "--stop-after", "committed", S, c.group_of(b))
    assert out == "STOPPED committed", out
    c.kill(b)
    c.failover(b, qb)
    out, err = c.ctl("move", S, c.group_of(b))
    p = c.placement()
    ev.update(slot=S, driver=out, move_cleared=" move=" not in p, arrived=arrivals(c).get(S),
              source=c.info(a, S), dest=c.info(qb, S))
    assert out.startswith("STUCK the destination holds no evidence"), out + err
    assert ev["move_cleared"], p
    assert ev["source"]["phase"] == 2 and ev["source"]["serving"] == 0
    assert ev["dest"]["serving"] == 0 and ev["dest"].get("proof") == "missing"
    out2, err2 = c.ctl("move", S2, c.group_of(b))
    ev["other_move"] = out2
    assert out2 == "DONE", out2 + err2
    out3, _ = c.ctl("move", S, c.group_of(b))  # nothing a driver can do; still safe
    ev["again"] = out3
    assert c.info(a, S)["serving"] == 0 and c.info(qb, S)["serving"] == 0


def scenario_prepared(c, ev):
    """An offline start of the source's standby while the move is only
    PREPARED (the commit gate cannot see an offline start): it seals S
    before serving; the move then completes; S is never served by it."""
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    wait(lambda: c.caught_up(qa, S, len(ids)), "A's standby caught up")
    out, _ = c.ctl("move", "--stop-after", "prepared", S, c.group_of(b))
    assert out == "STOPPED prepared", out
    c.kill(a)
    c.how = "offline"
    c.failover(a, qa)
    iq = c.info(qa, S)
    out, err = c.ctl("move", S, c.group_of(b))
    ev.update(slot=S, restarted_info=iq, driver=out, final=statuses(c.client[b], ids))
    assert iq["phase"] == 2 and iq["serving"] == 0, iq
    assert out == "DONE", out + err
    assert ev["final"] == {"OK": len(ids)}


def scenario_f10_reseal(c, ev):
    """F10 with a re-seal: the source's standby lacks SEALED(M) and is
    promoted, so its barrier re-seals; the move is then aborted: the same
    UNSEAL clears the re-seal and S serves again at A."""
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    wait(lambda: c.caught_up(qa, S, len(ids)), "A's standby caught up")
    assert c.ask(a, "DEBUG REPL-PAUSE 1") == "OK"
    out, _ = c.ctl("move", "--stop-after", "sealed", S, c.group_of(b))
    assert out == "STOPPED sealed", out
    c.kill(a)
    c.failover(a, qa)
    iq = c.info(qa, S)
    log = open(f"{c.work}/n{qa}.log").read()
    out, err = c.ctl("move", "--abort", S)
    ia = c.info(qa, S)
    ev.update(slot=S, resealed="at the start barrier" in log, promoted=iq, abort=out, after=ia,
              read=statuses(c.client[qa], ids))
    assert iq["phase"] == 2 and iq["serving"] == 0 and ev["resealed"], iq
    assert out == "ABORTED", out + err
    assert ia["serving"] == 1 and ev["read"] == {"OK": len(ids)}, (ia, ev["read"])
    assert c.info(b, S)["serving"] == 0
    assert deposit(c.client[qa], ids[0], 1) == OK


def scenario_f7_prepared(c, ev):
    """F7, the PREPARED race: the source's standby is promoted in place
    while the move is PREPARED and its barrier is held open (ROLE m); the
    driver, run meanwhile, waits; the promoted node seals S at the barrier
    and the move completes; it never serves S."""
    import threading
    ids, S = c.create(1, 1, in_slot=10)
    a = owner_primary(c, S)
    b = 3 if a == 1 else 1
    qa = c.peer(a)
    c.kill(qa)
    c.start(qa, standby_of=a, extra=["--debug-barrier", "hold:4000"])
    wait(lambda: c.role(qa) == "s", "A's standby back")
    wait(lambda: c.caught_up(qa, S, len(ids)), "A's standby caught up")
    out, _ = c.ctl("move", "--stop-after", "prepared", S, c.group_of(b))
    assert out == "STOPPED prepared", out
    probe = Probe(c, [(qa, S)], period=0.005)
    c.kill(a)
    assert c.ask(qa, "PROMOTE") == "OK"
    wait(lambda: c.role(qa) == "m", "ROLE m while the barrier is held")
    res = {}
    t = threading.Thread(target=lambda: res.update(r=c.ctl("move", S, c.group_of(b))))
    t.start()
    roles = set()
    while t.is_alive():
        roles.add(c.role(qa))
        time.sleep(0.05)
    t.join()
    out, err = res["r"]
    served = probe.stop()
    ev.update(slot=S, roles_seen=sorted(r for r in roles if r), driver=out, probe=f"{probe.samples} samples",
              final=statuses(c.client[b], ids))
    assert "m" in roles and out == "DONE", out + err
    assert not served, served
    assert ev["final"] == {"OK": len(ids)}


def size_measure():
    """Placement v2 at its worst (every slot an arrival) through real
    witnesses: CAS and SYNC latency, and the witnesses' state on disk."""
    work = tempfile.mkdtemp(prefix="snc-fc-size-")
    ports = free_ports(3, span=1)
    control = ",".join(f"127.0.0.1:{w}" for w in ports)
    procs = []
    try:
        for i, w in enumerate(ports):
            procs.append(subprocess.Popen([f"{BUILD}/snc_witness", "--port", str(w), "--state", f"{work}/w{i}",
                                           "--id", str(i), "--peers", control],
                                          stdout=open(f"{work}/w{i}.log", "a"), stderr=subprocess.STDOUT))
        time.sleep(0.3)
        r = subprocess.run([f"{BUILD}/test_placement", "--cas", control], capture_output=True, text=True,
                           timeout=300)
        print(r.stdout, end="")
        assert r.returncode == 0, r.stdout + r.stderr
        for i in range(3):
            st = f"{work}/w{i}"
            total = (os.path.getsize(st) if os.path.isfile(st) else
                     sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(st) for f in fs))
            total += sum(os.path.getsize(f"{work}/{f}") for f in os.listdir(work) if f.startswith(f"w{i}.") and
                         not f.endswith(".log"))
            print(f"witness {i} state on disk: {total} bytes")
        r = subprocess.run([f"{BUILD}/test_placement", "--measure"], capture_output=True, text=True)
        print(r.stdout, end="")
    finally:
        for p in procs:
            p.kill()
            p.wait()
        shutil.rmtree(work, ignore_errors=True)


# Every scenario of the fixed code that needs no older build (ctest runs
# these); compat and f7b need --old.
ALL = ["f1", "f2", "f3", "f4", "f5", "f5b", "f6", "f7", "f8_f9", "f10", "f11", "f13:f3", "f13:f6", "f15",
       "f17:f1", "f17:f5", "f17:f6", "f18", "role", "backfill", "stuck", "prepared", "f10_reseal", "f7_prepared", "size"]


def main():
    """Names: a scenario (f1, f2, ...), or MODE:NAME to run one with the
    standby taking over another way: f13:NAME through the witnesses (HA),
    f17:NAME by restarting its directory without --standby. `size` runs
    the placement measurement."""
    names = sys.argv[2:] or (["f1", "f2", "f4", "f5", "f5b"] if CHAR else ALL)
    results = {}
    for name in names:
        if name == "size":
            size_measure()
            continue
        mode, _, base = name.rpartition(":")
        fn = globals()[f"scenario_{base}"]
        opts = globals().get(f"scenario_{base}_cluster", dict)()
        if mode == "f13":
            opts["ha"] = True
        elif mode == "f17":
            opts["how"] = "offline"
        work = tempfile.mkdtemp(prefix=f"snc-fc-{base}-")
        c = None
        try:
            c = Cluster(work, **opts)
            ev = {}
            fn(c, ev)
            results[name] = ev
            print(f"{name}: ok {ev}")
        except Exception:
            print(f"{name}: FAILED; logs in {work}")
            if c:
                for k in sorted(c.server):
                    print(f"--- node {k}")
                    os.system(f"tail -20 {work}/n{k}.log")
            raise
        finally:
            if c:
                c.stop()
            if not os.environ.get("SNC_KEEP"):
                shutil.rmtree(work, ignore_errors=True)
    print("test_fail_closed:", "characterized" if CHAR else "ok", " ".join(names))


if __name__ == "__main__":
    main()
