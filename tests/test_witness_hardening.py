#!/usr/bin/env python3
"""Witness hardening before the baseline freeze (work order
docs/handoff/PR51_Review_Handoff.txt; docs/PAXOS_BALLOTS.md 9). Real
snc_witness processes, no pairs (test machines debug/... and hand-written
state files).

  h1  state file missing, no --new-state: refused, nothing created
  h2  state file missing, --new-state 1: created durably before serving;
      a restart without the flag runs on it (h12: ordinary restart)
  h3  state file present, --new-state 1: refused, file untouched
  h4  --peers naming one endpoint twice (also via another host name):
      refused
  h5  --id not naming this witness (other port; an address not on this
      machine; another entry reaching this witness's port): refused
  h6  a save failing at the directory open or fsync stops the witness
      before the PROMISE / ACCEPTED / COMMIT reply, and before the
      proposer answers OK for a value it learned
  h7  the same for the temporary file's open, write, fsync and the rename
  h8  a legacy (pre-machine-name) state file still loads as ha/0, and
      survives a save and a restart
  h9  an all-digit machine name in a modern file stays itself (never ha/0),
      across a save and a restart; a stray READ creates nothing; a file
      mixing both kinds, or with a line it cannot read, is refused

Usage: test_witness_hardening.py BUILD [scenario...] [--json DIR]
"""
import hashlib
import json
import re
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
WIT = f"{BUILD}/snc_witness"


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
    """The reply line, "" if the connection closed without one."""
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


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def run_once(args, timeout=10):
    """A witness that should refuse to start: (exit code, output)."""
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr).strip()


class Cluster:
    """Three witnesses on 127.0.0.1, --debug 1."""

    def __init__(self, files=None):
        self.work = tempfile.mkdtemp(prefix="snc-h-")
        self.ports = free_ports(3)
        self.peers = ",".join(f"127.0.0.1:{p}" for p in self.ports)
        self.p = [None] * 3
        for i, text in (files or {}).items():
            with open(self.state(i), "w") as f:
                f.write(text)
        for i in range(3):
            self.start(i)

    def state(self, i):
        return f"{self.work}/w{i}.state"

    def args(self, i):
        a = [WIT, "--port", str(self.ports[i]), "--state", self.state(i), "--id", str(i), "--peers", self.peers,
             "--debug", "1"]
        return a if os.path.exists(self.state(i)) else a + ["--new-state", "1"]

    def start(self, i):
        self.p[i] = subprocess.Popen(self.args(i), stdout=open(f"{self.work}/w{i}.log", "a"),
                                     stderr=subprocess.STDOUT)
        t = time.time() + 10
        while time.time() < t:
            if ask(self.ports[i], "READ debug/ping", 0.5).startswith("STATE"):
                return
            if self.p[i].poll() is not None:
                raise AssertionError(f"witness {i} exited {self.p[i].returncode}: {self.log(i)}")
            time.sleep(0.05)
        raise AssertionError(f"witness {i} did not start")

    def log(self, i):
        with open(f"{self.work}/w{i}.log") as f:
            return f.read()

    def exited(self, i, timeout=5):
        try:
            return self.p[i].wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def stop(self, i):
        if self.p[i] and self.p[i].poll() is None:
            self.p[i].send_signal(signal.SIGTERM)
            self.p[i].wait()

    def close(self):
        for p in self.p:
            if p and p.poll() is None:
                p.kill()
                p.wait()
        shutil.rmtree(self.work, ignore_errors=True)


def h1():
    work = tempfile.mkdtemp(prefix="snc-h-")
    try:
        (port,) = free_ports(1)
        state = f"{work}/w.state"
        code, out = run_once([WIT, "--port", str(port), "--state", state])
        res = {"exit": code, "message": out, "created": os.path.exists(state)}
        res["pass"] = code == 2 and "does not exist" in out and not res["created"]
        return res
    finally:
        shutil.rmtree(work, ignore_errors=True)


def h2():
    work = tempfile.mkdtemp(prefix="snc-h-")
    procs = []
    try:
        (port,) = free_ports(1)
        state = f"{work}/w.state"
        p = subprocess.Popen([WIT, "--port", str(port), "--state", state, "--new-state", "1"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        procs.append(p)
        t = time.time() + 10
        served_first = False
        while time.time() < t and not served_first:
            served_first = ask(port, "READ ha/0", 0.5).startswith("STATE")
            time.sleep(0.05)
        exists_when_serving = served_first and os.path.exists(state)
        p.send_signal(signal.SIGTERM)
        p.wait()
        # h12: an ordinary restart (no flag) runs on the file.
        p2 = subprocess.Popen([WIT, "--port", str(port), "--state", state], stdout=subprocess.DEVNULL,
                              stderr=subprocess.STDOUT)
        procs.append(p2)
        t = time.time() + 10
        served = False
        while time.time() < t and not served:
            served = ask(port, "READ ha/0", 0.5).startswith("STATE")
            time.sleep(0.05)
        res = {"first_start_serves": served_first, "exists_when_serving": exists_when_serving,
               "restart_without_flag_serves": served}
        res["pass"] = served_first and exists_when_serving and served
        return res
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        shutil.rmtree(work, ignore_errors=True)


def h3():
    work = tempfile.mkdtemp(prefix="snc-h-")
    try:
        (port,) = free_ports(1)
        state = f"{work}/w.state"
        with open(state, "w") as f:
            f.write("chosen ha/0 3 abc 0 1 0\nopen ha/0 4\npromised ha/0 77\naccepted ha/0 0 0 -\n")
        before = sha(state)
        code, out = run_once([WIT, "--port", str(port), "--state", state, "--new-state", "1"])
        res = {"exit": code, "message": out, "unchanged": sha(state) == before}
        res["pass"] = code == 2 and "exists" in out and res["unchanged"]
        return res
    finally:
        shutil.rmtree(work, ignore_errors=True)


def refused_config(peers, wid, port, why):
    work = tempfile.mkdtemp(prefix="snc-h-")
    try:
        code, out = run_once([WIT, "--port", str(port), "--state", f"{work}/w.state", "--new-state", "1", "--id",
                              str(wid), "--peers", peers])
        return {"peers": peers, "id": wid, "exit": code, "message": out,
                "refused": code == 2 and why in out and not os.path.exists(f"{work}/w.state")}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def h4():
    a, b = free_ports(2)
    cases = [refused_config(f"127.0.0.1:{a},127.0.0.1:{b},127.0.0.1:{b}", 0, a, "twice"),
             refused_config(f"127.0.0.1:{a},localhost:{b},127.0.0.1:{b}", 0, a, "twice"),
             # Two local addresses of another witness on this machine: one
             # listener (INADDR_ANY), so one acceptor counted twice.
             refused_config(f"127.0.0.1:{a},127.0.0.1:{b},127.0.0.2:{b}", 0, a, "twice")]
    return {"cases": cases, "pass": all(c["refused"] for c in cases)}


def h5():
    a, b, c = free_ports(3)
    cases = [
        # --id names another port.
        refused_config(f"127.0.0.1:{a},127.0.0.1:{b},127.0.0.1:{c}", 1, a, "--id must index"),
        # --id names an address that is not this machine's (TEST-NET-1).
        refused_config(f"192.0.2.1:{a},127.0.0.1:{b},127.0.0.1:{c}", 0, a, "not an address of this machine"),
        # Another entry reaches this witness's own port on this machine.
        refused_config(f"127.0.0.1:{a},127.0.0.2:{a},127.0.0.1:{c}", 0, a, "twice"),
    ]
    return {"cases": cases, "pass": all(x["refused"] for x in cases)}


STEPS_DIR = ["dir-open", "dir-fsync"]
STEPS_FILE = ["tmp-open", "tmp-write", "tmp-fsync", "rename"]


def persist_failures(steps):
    """For each step: an acceptor whose save fails at it, for PREPARE,
    ACCEPT and COMMIT; and a proposer whose learning save fails."""
    out = []
    for step in steps:
        for kind in ("PREPARE", "ACCEPT", "COMMIT", "PROPOSER"):
            c = Cluster()
            try:
                m, b = "debug/h", (int(time.time() * 1000) << 6) | 2
                w = 0 if kind == "PROPOSER" else 1
                if kind == "PROPOSER":
                    # Saves on witness 0 for its own proposal: its PROMISE, its
                    # ACCEPTED, then (COMMIT dropped) its learning the value.
                    assert ask(c.ports[0], "DEBUG SKIP-COMMIT 1") == "OK"
                    assert ask(c.ports[0], f"DEBUG FAIL-PERSIST {step} 2") == "OK"
                    reply = ask(c.ports[0], f"DEBUG PROPOSE {m} X", 15)
                else:
                    assert ask(c.ports[w], f"DEBUG FAIL-PERSIST {step}") == "OK"
                    line = {"PREPARE": f"PREPARE {m} 1 {b} 0 -", "ACCEPT": f"ACCEPT {m} 1 {b} 1 X",
                            "COMMIT": f"COMMIT {m} 1 X"}[kind]
                    reply = ask(c.ports[w], line)
                code = c.exited(w)
                log = c.log(w)
                if kind == "PROPOSER":
                    # Proof that the failing save was the learning one: the
                    # others accepted X (its PROMISE and ACCEPTED saves passed).
                    acc = []
                    for o in (1, 2):
                        with open(c.state(o)) as f:
                            acc.append(any(re.fullmatch(rf"accepted {m} \d+ 1 X", l) for l in f.read().splitlines()))
                    r_acc = all(acc)
                else:
                    r_acc = True
                r = {"step": step, "request": kind, "reply": reply, "exit": code,
                     "said": [l for l in log.splitlines() if "cannot save state" in l][-1:]}
                # Then the witness restarts on what is on disk, whatever made it.
                c.start(w)
                r["restarted"] = True
                r["others_accepted"] = r_acc
                r["ok"] = reply == "" and code == 3 and bool(r["said"]) and r_acc
                out.append(r)
            finally:
                c.close()
    return {"cases": out, "pass": all(r["ok"] for r in out)}


def h6():
    return persist_failures(STEPS_DIR)


def h7():
    return persist_failures(STEPS_FILE)


LEGACY = "chosen 2 abcdef 0 1 0\nopen 3\npromised 4242\naccepted 0 0 -\n"


def h8():
    c = Cluster(files={0: LEGACY})
    try:
        r1 = ask(c.ports[0], "READ ha/0")
        # A save (a proposal on a test machine), a restart: ha/0 unchanged,
        # the file now written with machine names.
        p = ask(c.ports[0], "DEBUG PROPOSE debug/x a", 15)
        c.stop(0)
        with open(c.state(0)) as f:
            text = f.read()
        c.start(0)
        r2 = ask(c.ports[0], "READ ha/0")
        res = {"read_legacy": r1, "propose": p, "saved": text, "read_after_restart": r2}
        res["pass"] = (r1 == "STATE 2 abcdef 0 1 0" and r2 == r1 and p.startswith("OK v=1")
                       and "chosen ha/0 2 abcdef 0 1 0" in text and "promised ha/0 4242" in text)
        return res
    finally:
        c.close()


def h9():
    modern = "chosen 5 7 x\nopen 5 8\npromised 5 0\naccepted 5 0 0 -\n"
    c = Cluster(files={0: modern})
    res = {}
    try:
        res["read_ha0"] = ask(c.ports[0], "READ ha/0")
        res["read_5"] = ask(c.ports[0], "READ 5")
        res["stray_read"] = ask(c.ports[0], "READ 9")
        res["propose"] = ask(c.ports[0], "DEBUG PROPOSE debug/x a", 15)
        c.stop(0)
        with open(c.state(0)) as f:
            res["saved"] = f.read()
        c.start(0)
        res["read_ha0_after"] = ask(c.ports[0], "READ ha/0")
        res["read_5_after"] = ask(c.ports[0], "READ 5")
    finally:
        c.close()
    # Files the loader must refuse.
    work = tempfile.mkdtemp(prefix="snc-h-")
    try:
        (port,) = free_ports(1)
        refusals = {}
        for name, text in {"mixed": LEGACY + modern, "unreadable line": modern + "promised 5 notanumber\n",
                           "unknown line": modern + "frobnicate 5 1\n",
                           "version glued to body": "chosen 5 7x\nopen 5 8\npromised 5 0\naccepted 5 0 0 -\n",
                           "negative ballot": "chosen 5 7 x\nopen 5 8\npromised 5 -1\naccepted 5 0 0 -\n"}.items():
            with open(f"{work}/w.state", "w") as f:
                f.write(text)
            code, out = run_once([WIT, "--port", str(port), "--state", f"{work}/w.state"])
            refusals[name] = {"exit": code, "message": out}
        res["refusals"] = refusals
    finally:
        shutil.rmtree(work, ignore_errors=True)
    res["pass"] = (res["read_ha0"] == "STATE 0 -" and res["read_5"] == "STATE 7 x" and res["stray_read"] == "STATE 0 -"
                   and res["propose"].startswith("OK v=1") and "chosen 5 7 x" in res["saved"]
                   and "chosen 9 " not in res["saved"] and "ha/0" not in res["saved"]
                   and res["read_ha0_after"] == "STATE 0 -" and res["read_5_after"] == "STATE 7 x"
                   and all(r["exit"] == 2 and "not starting" in r["message"] for r in res["refusals"].values()))
    return res


SCENARIOS = {"h1_missing_state_refused": h1, "h2_first_start_creates": h2, "h3_existing_state_new_refused": h3,
             "h4_duplicate_peers_refused": h4, "h5_id_inconsistent_refused": h5,
             "h6_dir_durability_fatal": h6, "h7_file_durability_fatal": h7,
             "h8_legacy_state_loads": h8, "h9_numeric_machine_stays_itself": h9}


def main():
    failed = []
    for name in ONLY or list(SCENARIOS):
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
    print("test_witness_hardening: " + ("OK" if not failed else f"FAILED {failed}"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
