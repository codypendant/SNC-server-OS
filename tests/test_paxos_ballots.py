#!/usr/bin/env python3
"""F8 characterization: Paxos ballots after a witness restart
(docs/PAXOS_BALLOTS.md, work order docs/handoff/PR50_Review_Handoff.txt).

Three real snc_witness processes (--debug 1, no pairs: test machines
"debug/..." only). Scenarios:

  c3   ordinary restart, clock unchanged: ballots after it are above
       every ballot used before.
  c1   restart with the clock stepped back below a ballot B used in an
       instance (accepted, COMMIT dropped, so it is still open at the
       acceptors): does the witness generate B (or lower) again?
  c2a  X chosen at B by the two other acceptors, the proposer crashed
       before learning it; it restarts rolled back and proposes Y with B
       exactly (counter set to B's), in the same instance: two values?
  c2l  as c1, the proposer having learned X: B goes to the next instance.
  c2b  B promised by a majority, X accepted by the proposer's own acceptor
       only (the ACCEPT to the other lost), then the restarted witness
       proposes Y with B: two values?
  c2x  (not reachable by the proposer code) an ACCEPT at B with another
       value, sent without phase 1: shows why phase 1's strict promise is
       what makes B single-valued.
  c4a  an acceptor's state directory restored to before it accepted X
       (chosen by a majority including it): two values?
  c4b  the same with the state file deleted (a replaced disk).

Usage: test_paxos_ballots.py BUILD [scenario...] [--json DIR]
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ARGS = list(sys.argv[1:])
JSON_DIR = None
if "--json" in ARGS:
    i = ARGS.index("--json")
    JSON_DIR = ARGS[i + 1]
    del ARGS[i:i + 2]
BUILD = ARGS[0] if ARGS else "build"
ONLY = ARGS[1:]


def free_ports(n):
    socks, ports = [], []
    for _ in range(n):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        socks.append(s)
        ports.append(s.getsockname()[1])
    for s in socks:
        s.close()
    return ports


def ask(port, line, timeout=5):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.sendall((line + "\n").encode())
            s.settimeout(timeout)
            d = b""
            while not d.endswith(b"\n"):
                c = s.recv(4096)
                if not c:
                    break
                d += c
            return d.decode().strip()
    except OSError as e:
        return f"ERR {e}"


def ballot(counter, ident):
    return (counter << 6) | ident


class Witnesses:
    def __init__(self):
        self.work = tempfile.mkdtemp(prefix="snc-f8-")
        self.ports = free_ports(3)
        self.peers = ",".join(f"127.0.0.1:{p}" for p in self.ports)
        self.p = [None] * 3
        for i in range(3):
            self.start(i)

    def state(self, i):
        return f"{self.work}/w{i}.state"

    def start(self, i, offset_ms=0):
        args = [f"{BUILD}/snc_witness", "--port", str(self.ports[i]), "--state", self.state(i), "--id", str(i),
                "--peers", self.peers, "--debug", "1"]
        if offset_ms:
            args += ["--debug-clock-offset-ms", str(offset_ms)]
        self.p[i] = subprocess.Popen(args, stdout=open(f"{self.work}/w{i}.log", "a"), stderr=subprocess.STDOUT)
        t = time.time() + 10
        while time.time() < t:
            if ask(self.ports[i], "READ debug/ping", 0.5).startswith("STATE"):
                return
            time.sleep(0.05)
        raise AssertionError(f"witness {i} did not start")

    def crash(self, i):
        self.p[i].send_signal(signal.SIGKILL)
        self.p[i].wait()

    def stop(self, i):
        self.p[i].send_signal(signal.SIGSTOP)

    def cont(self, i):
        self.p[i].send_signal(signal.SIGCONT)

    def ask(self, i, line):
        return ask(self.ports[i], line)

    def acceptor(self, i, m):
        """This witness's saved acceptor state for machine m, from its file."""
        out = {}
        try:
            for line in open(self.state(i)):
                k, *rest = line.split()
                if len(rest) >= 2 and rest[0] == m:
                    out[k] = " ".join(rest[1:])
        except FileNotFoundError:
            pass
        return out

    def chosen(self, m):
        """READ m from every witness: [(version, body)] per witness."""
        r = []
        for i in range(3):
            line = self.ask(i, f"READ {m}")
            parts = line.split(" ", 2)
            r.append((int(parts[1]), parts[2]) if len(parts) == 3 and parts[0] == "STATE" else (None, line))
        return r

    def close(self):
        for p in self.p:
            if p and p.poll() is None:
                p.send_signal(signal.SIGCONT)
                p.kill()
                p.wait()
        shutil.rmtree(self.work, ignore_errors=True)


def propose(w, i, m, body):
    """DEBUG PROPOSE through witness i: (reply, version or None, [ballots])."""
    r = w.ask(i, f"DEBUG PROPOSE {m} {body}")
    bs = r.split("ballots=", 1)[1] if "ballots=" in r else ""
    ballots = [int(x) for x in bs.split(",") if x]
    v = None
    if r.startswith("OK v="):
        v = int(r.split()[1][2:])
    return r, v, ballots


def two_values_at_one_version(w, m):
    """Every value any witness reports as chosen at the same version must
    agree. Returns {version: set(bodies)} where they do not."""
    seen = {}
    for v, body in w.chosen(m):
        if v:
            seen.setdefault(v, set()).add(body)
    return {v: sorted(b) for v, b in seen.items() if len(b) > 1}


def restart_rolled_back(w, i, b_used):
    """Crash witness i and start it again with its clock below b_used's
    counter (so its first ballots are at or below b_used)."""
    w.crash(i)
    now_ms = int(time.time() * 1000)
    offset = (b_used >> 6) - now_ms - 200  # 200 ms below B's counter
    w.start(i, offset_ms=offset)
    return offset


def c3():
    w = Witnesses()
    try:
        before = [int(w.ask(0, "DEBUG NEXT-BALLOT").split()[1])]
        r, v, bs = propose(w, 0, "debug/c3", "x")
        assert v == 1, r
        before += bs
        w.crash(0)
        w.start(0)
        after = int(w.ask(0, "DEBUG NEXT-BALLOT").split()[1])
        res = {"before": before, "after_restart": after, "above_all": after > max(before)}
        res["pass"] = res["above_all"]
        return res
    finally:
        w.close()


def c1():
    w = Witnesses()
    try:
        assert w.ask(0, "DEBUG SKIP-COMMIT 1") == "OK"
        r, v, bs = propose(w, 0, "debug/c1", "X")
        assert v == 1, r
        B = bs[-1]
        accs = {i: w.acceptor(i, "debug/c1") for i in range(3)}
        offset = restart_rolled_back(w, 0, B)
        gen = []
        for _ in range(400):  # about 200 ms of counter below B, then past it
            gen.append(int(w.ask(0, "DEBUG NEXT-BALLOT").split()[1]))
            if gen[-1] > B:
                break
        res = {"B": B, "B_counter": B >> 6, "accepted_before_restart": accs, "offset_ms": offset,
               "first_after_restart": gen[0], "generated": len(gen),
               "reused_at_or_below_B": sum(1 for g in gen if g <= B), "B_itself_generated": B in gen}
        # The characterization: does a rolled-back restart generate ballots
        # it already used? (Expected: yes; the counter is not persisted.)
        res["pass"] = res["reused_at_or_below_B"] > 0
        return res
    finally:
        w.close()


def c2a():
    """Ballot B (witness 0's) promised by witnesses 0 and 1; X accepted at B
    by witnesses 1 and 2, a majority: X is chosen, and witness 0 (the
    proposer, crashed before it learned) knows only its promise. Witness 0
    restarts rolled back and proposes Y with B exactly, in the same
    instance."""
    w = Witnesses()
    try:
        m = "debug/c2a"
        B = ballot(int(time.time() * 1000), 0)
        prom = [w.ask(i, f"PREPARE {m} 1 {B} 0 -") for i in (0, 1)]
        assert all(p.startswith("PROMISE") for p in prom), prom
        acc = [w.ask(i, f"ACCEPT {m} 1 {B} 1 X") for i in (1, 2)]
        assert acc == ["ACCEPTED"] * 2, acc
        restart_rolled_back(w, 0, B)
        assert w.ask(0, f"DEBUG SET-COUNTER {(B >> 6) - 1}") == "OK"
        r2, v2, bs2 = propose(w, 0, m, "Y")
        res = {"B": B, "promises": prom, "accept_x": acc, "reply": r2, "ballots": bs2,
               "first_ballot_is_B": bool(bs2) and bs2[0] == B,
               "chosen": w.chosen(m), "acceptors": {i: w.acceptor(i, m) for i in range(3)}}
        res["conflicts"] = two_values_at_one_version(w, m)
        # Y can only follow X: chosen at version 2, after B was refused and
        # a higher ballot adopted X at version 1.
        res["pass"] = res["first_ballot_is_B"] and not res["conflicts"] and v2 == 2 and len(bs2) >= 3
        return res
    finally:
        w.close()


def c2l():
    """As c1 (X chosen at B, COMMIT dropped), with the proposer witness's
    own learned value on disk: the restarted witness proposes Y with B."""
    w = Witnesses()
    try:
        assert w.ask(0, "DEBUG SKIP-COMMIT 1") == "OK"
        r, v, bs = propose(w, 0, "debug/c2l", "X")
        assert v == 1, r
        B = bs[-1]
        restart_rolled_back(w, 0, B)
        assert w.ask(0, f"DEBUG SET-COUNTER {(B >> 6) - 1}") == "OK"
        r2, v2, bs2 = propose(w, 0, "debug/c2l", "Y")
        res = {"B": B, "reply": r2, "ballots": bs2, "first_ballot_is_B": bool(bs2) and bs2[0] == B,
               "chosen": w.chosen("debug/c2l"), "acceptors": {i: w.acceptor(i, "debug/c2l") for i in range(3)}}
        res["conflicts"] = two_values_at_one_version(w, "debug/c2l")
        # It knows X is chosen (saved before it answered): B goes to the
        # next instance, where it was never used.
        res["pass"] = res["first_ballot_is_B"] and not res["conflicts"] and v2 == 2
        return res
    finally:
        w.close()


def c2b():
    """Ballot B (witness 0's) promised by witnesses 0 and 1; X accepted by
    witness 0 only (the ACCEPT to witness 1 lost). Witness 0 crashes,
    restarts rolled back, and proposes Y with B exactly."""
    w = Witnesses()
    try:
        m = "debug/c2b"
        B = ballot(int(time.time() * 1000), 0)
        prom = [w.ask(i, f"PREPARE {m} 1 {B} 0 -") for i in (0, 1)]
        assert all(p.startswith("PROMISE") for p in prom), prom
        acc = w.ask(0, f"ACCEPT {m} 1 {B} 1 X")
        assert acc == "ACCEPTED", acc
        restart_rolled_back(w, 0, B)
        assert w.ask(0, f"DEBUG SET-COUNTER {(B >> 6) - 1}") == "OK"
        r2, v2, bs2 = propose(w, 0, m, "Y")
        res = {"B": B, "promises": prom, "accepted_by_0": acc, "reply": r2, "ballots": bs2,
               "first_ballot_is_B": bool(bs2) and bs2[0] == B,
               "chosen": w.chosen(m), "acceptors": {i: w.acceptor(i, m) for i in range(3)}}
        res["conflicts"] = two_values_at_one_version(w, m)
        # X, accepted at B by one acceptor only, was not chosen; a higher
        # ballot's phase 1 finds it (witness 0 reports it) and chooses it at
        # version 1 (it may have been chosen, as far as anyone knows), then Y
        # at version 2: three ballots, B refused.
        res["pass"] = res["first_ballot_is_B"] and not res["conflicts"] and v2 == 2 and len(bs2) >= 3
        return res
    finally:
        w.close()


def c2x():
    """Not reachable by the proposer code: X accepted at B by witnesses 0
    and 1 (chosen), then ACCEPT B Y sent to witnesses 1 and 2 without a
    phase 1 at B. Shows the acceptor takes an ACCEPT at a ballot equal to
    its promise, so B stays single-valued only because no proposer gets a
    second majority of promises at B (PREPARE needs a strictly higher
    ballot)."""
    w = Witnesses()
    try:
        m = "debug/c2x"
        B = ballot(int(time.time() * 1000), 0)
        prom = [w.ask(i, f"PREPARE {m} 1 {B} 0 -") for i in (0, 1)]
        accx = [w.ask(i, f"ACCEPT {m} 1 {B} 1 X") for i in (0, 1)]
        # A second phase 1 at B: refused by the two that promised B.
        prom2 = [w.ask(i, f"PREPARE {m} 1 {B} 0 -") for i in (1, 2)]
        accy = [w.ask(i, f"ACCEPT {m} 1 {B} 1 Y") for i in (1, 2)]
        res = {"B": B, "promises": prom, "accept_x": accx, "second_prepare_at_B": prom2, "accept_y": accy,
               "acceptors": {i: w.acceptor(i, m) for i in range(3)}}
        res["second_phase1_majority"] = sum(p.startswith("PROMISE") for p in prom2) >= 2
        res["y_accepted_by_majority"] = sum(a == "ACCEPTED" for a in accy) >= 2
        # Characterization only: the second phase 1 at B fails, so a correct
        # proposer never sends these ACCEPTs.
        res["pass"] = not res["second_phase1_majority"]
        return res
    finally:
        w.close()


def c4(wipe):
    """Witness 1 accepts X (with witness 0: chosen); witness 2 is down.
    Witness 1's state is then restored to before (or deleted), witness 0
    is down, witness 2 back: witness 1 proposes Y."""
    w = Witnesses()
    m = "debug/c4"
    try:
        snap = f"{w.work}/w1.snapshot"
        r0, v0, _ = propose(w, 0, "debug/before", "a")  # some state on disk first
        assert v0 == 1, r0
        shutil.copy(w.state(1), snap)  # before X: the "backup"
        # Down, not stopped: a stopped process still takes connections, and
        # would accept X late from its socket backlog when continued.
        w.crash(2)
        assert w.ask(0, "DEBUG SKIP-COMMIT 1") == "OK"
        r, v, bs = propose(w, 0, m, "X")
        assert v == 1, r
        acc1 = w.acceptor(1, m)
        w.crash(0)
        w.crash(1)
        if wipe:
            os.remove(w.state(1))
        else:
            shutil.copy(snap, w.state(1))
        w.start(1)
        w.start(2)
        r2, v2, bs2 = propose(w, 1, m, "Y")
        w.start(0)
        res = {"x": r, "w1_accepted_x": acc1, "restored": "deleted" if wipe else "snapshot before X",
               "y": r2, "chosen": w.chosen(m)}
        res["conflicts"] = two_values_at_one_version(w, m)
        # Characterization: with an acceptor's memory lost, two values can
        # be chosen at one version (expected).
        res["two_values_chosen"] = bool(res["conflicts"])
        res["pass"] = res["two_values_chosen"]
        return res
    finally:
        w.close()


SCENARIOS = {
    "c3_ordinary_restart": c3,
    "c1_rollback_reuse": c1,
    "c2a_reuse_over_chosen": c2a,
    "c2l_reuse_after_learning": c2l,
    "c2b_reuse_over_minority_accept": c2b,
    "c2x_accept_without_phase1": c2x,
    "c4a_restored_directory": lambda: c4(False),
    "c4b_wiped_state": lambda: c4(True),
}


def main():
    names = ONLY or list(SCENARIOS)
    failed = []
    for name in names:
        t0 = time.time()
        try:
            res = SCENARIOS[name]()
        except Exception as e:  # noqa: BLE001
            res = {"pass": False, "error": f"{type(e).__name__}: {e}"}
        res["scenario"], res["seconds"] = name, round(time.time() - t0, 1)
        print(("PASS " if res["pass"] else "FAIL ") + name + " " + json.dumps(res, default=str))
        sys.stdout.flush()
        if JSON_DIR:
            os.makedirs(JSON_DIR, exist_ok=True)
            with open(f"{JSON_DIR}/{name}.json", "w") as f:
                json.dump(res, f, indent=1, default=str)
        if not res["pass"]:
            failed.append(name)
    print("test_paxos_ballots: " + ("OK" if not failed else f"FAILED {failed}"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
