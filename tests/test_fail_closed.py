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

SNC_CHARACTERIZE=1 asserts the behavior of the code before the fail-closed
slice (the defects reproduced: implementation step 1 of the PR #39 review)
and prints the evidence. Without it, the fixed behavior is asserted.

Usage: test_fail_closed.py BUILD_DIR [SCENARIO ...]
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
OP_DEPOSIT, OP_GET_BALANCE = 1003, 1006


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


def create_near(port, oid):
    """create_near(oid): mints in oid's slot when the node serves it."""
    args = struct.pack("<qQBB6s64s96s", 0, 1, 1, 1, b"", b"e", b"n")
    st, _, new, _ = request(port, 2, ACCOUNT, oid_bytes(oid), args)
    return st


class Cluster:
    """Group g: node 2g+1 starts primary, node 2g+2 its async standby."""

    def __init__(self, work, groups=2, ha=False, async_replica=True):
        self.work, self.groups, self.procs = work, groups, []
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
        for i, w in enumerate(self.wports):
            args = [f"{BUILD}/snc_witness", "--port", str(w), "--state", f"{work}/w{i}", "--id", str(i),
                    "--peers", self.control, "--failover-ms", "500", "--poll-ms", "50"]
            if ha:
                for g in range(groups):
                    args += ["--group", f"{g}=127.0.0.1:{self.admin[2 * g + 1]},127.0.0.1:{self.admin[2 * g + 2]}"]
            self.procs.append(subprocess.Popen(args, stdout=open(f"{work}/w{i}.log", "a"),
                                               stderr=subprocess.STDOUT))
        time.sleep(0.3)
        r = subprocess.run([f"{BUILD}/snc_ctl", "--control", self.control, "init", "--nodes", node_list,
                            "--groups", groups_arg], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        for g in range(groups):
            self.start(2 * g + 2, standby_of=2 * g + 1)
            wait(lambda: self.role(2 * g + 2) == "s", f"standby {2 * g + 2} up")
            self.start(2 * g + 1)
            wait(lambda: self.role(2 * g + 1) in ("p", "n"), f"primary {2 * g + 1} up")

    def group_of(self, k):
        return (k - 1) // 2

    def peer(self, k):
        return k + 1 if k % 2 else k - 1

    def start(self, k, standby_of=None, extra=()):
        """Starts node k: a standby (listening for standby_of) or a primary
        shipping to its peer's listener."""
        args = [f"{BUILD}/snc_server", "--shards", "2", "--objects", "20000", "--data-dir", f"{self.work}/n{k}",
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

    def ctl(self, *args, timeout=120):
        r = subprocess.run([f"{BUILD}/snc_ctl", "--control", self.control] + [str(a) for a in args],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip(), r.stderr

    def placement(self):
        out, _ = self.ctl("get")
        return out

    def create(self, k, n, near=None, in_slot=0):
        """n accounts through node k's client port, in near's slot if given;
        with in_slot, in_slot more in the first one's slot. Returns (IDs,
        the slot snc_load reports or None)."""
        ids = f"{self.work}/ids-{time.time_ns()}"
        cmd = [f"{BUILD}/snc_load", "create", "--connect", f"127.0.0.1:{self.client[k]}", "--accounts", str(n),
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
    # 4. B's primary dies; its standby is promoted.
    c.kill(b)
    c.promote(qb)
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
    c.promote(qa)
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
    c.promote(qb)
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
    c.promote(qb)
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
    c.promote(qa)
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


def main():
    names = sys.argv[2:] or ["f1", "f2"]
    results = {}
    for name in names:
        work = tempfile.mkdtemp(prefix=f"snc-fc-{name}-")
        c = None
        try:
            c = Cluster(work)
            ev = {}
            globals()[f"scenario_{name}"](c, ev)
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
