#!/usr/bin/env python3
"""G2 in the lab (docs/PAXOS_BALLOTS.md 9.1; work order
docs/handoff/PR52_Review_Handoff.txt): a Profile B run of bench/lab/lab.py
on loopback checks every witness's state at its campaign boundaries (before
each fault, after each heal, after each load phase, before the final check,
at the end), and a violation at a boundary stops the run there and marks it
invalid.

  clean     p3 with a stop fault: ok, and the boundary checks were made in
            order
  violation p3 whose witness 0 state directory loses the run nonce just
            before the fault: the check before the fault stops the run (the
            fault is never applied); result invalid, not ok
  primary   Lab.wait_primary, against scripted STATUS answers (no
            processes). This is the race that once failed `clean` with
            KeyError: None (PR #70 review 5A): p3 sampled lab.primary(0)
            once, and a STATUS timed out under load.
            - One-shot primary() returns None when the STATUS times out:
              the race, reproduced.
            - wait_primary returns the node once it answers.
            - It keeps waiting while two members claim primary.
            - It returns node id 0 (no truthiness test).
            - It raises within its timeout, naming the last roles, when no
              primary appears.

Usage: test_lab_witness_guard.py BUILD
"""
import json
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "bench", "lab"))
sys.path.insert(0, os.path.join(REPO, "bench"))
import lab  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build"))


def run(out_root, inject=None):
    inv = os.path.join(out_root, "inv.toml")
    with open(inv, "w") as f:
        f.write(f'''[lab]
build = "{BUILD}"
dir = "{out_root}/run"
shards = 2
objects = 20000
base_port = {21000 + (os.getpid() % 200) * 40}
profile = "B"

[hosts.a]
addr = "127.0.0.1"
ssh = "local"

[layout]
witnesses = ["a", "a", "a"]
groups = [["a", "a"]]
client = "a"
''')
    lab.REPO = out_root  # results under the temporary root, not the repository
    orig_fault = lab.Lab.fault
    if inject:
        def fault(self, *a, **kw):
            inject(self)
            return orig_fault(self, *a, **kw)
        lab.Lab.fault = fault
    sys.argv = ["lab.py", inv, "p3", "--fault", "stop", "--seconds", "8", "--accounts", "2000", "--threads", "1",
                "--depth", "4"]
    try:
        code = lab.main()
    finally:
        lab.Lab.fault = orig_fault
    res = [os.path.join(d, f) for d, _, fs in os.walk(os.path.join(out_root, "bench", "results")) for f in fs
           if f == "result.json"]
    assert len(res) == 1, res
    with open(res[0]) as f:
        return code, json.load(f)


def clean():
    root = tempfile.mkdtemp(prefix="snc-labg-")
    try:
        code, r = run(root)
        checks = r.get("witness_state_checks", [])
        want = ["before fault stop node", "after heal", "after load load", "before the final check", "end of run"]
        pos = [next((i for i, c in enumerate(checks) if c.startswith(w)), -1) for w in want]
        ok = code == 0 and r.get("ok") is True and "invalid" not in r and all(p >= 0 for p in pos) \
            and pos == sorted(pos)
        return ok, {"exit": code, "ok": r.get("ok"), "checks": checks, "error": r.get("error")}
    finally:
        shutil.rmtree(root, ignore_errors=True)


def violation():
    root = tempfile.mkdtemp(prefix="snc-labg-")
    try:
        def lose_nonce(self):
            os.remove(f"{self.run_dir}/w0.state.run")
        code, r = run(root, inject=lose_nonce)
        faults = [e for e in r.get("events", []) if e["event"] == "fault"]
        ok = code != 0 and r.get("ok") is False and "at before fault stop node" in r.get("invalid", "") \
            and "state directory replaced" in r.get("invalid", "") \
            and not faults and "witness_state" in r
        return ok, {"exit": code, "ok": r.get("ok"), "invalid": r.get("invalid"), "faults_applied": len(faults),
                    "checks": r.get("witness_state_checks")}
    finally:
        shutil.rmtree(root, ignore_errors=True)


def primary():
    import types
    script = {}  # node id -> list of STATUS answers, consumed one per ask

    def fake_status(addr, port):
        seq = script[port]
        r = seq.pop(0) if len(seq) > 1 else seq[0]
        return None if r is None else {"role": r}

    def mk(ids):
        lb = object.__new__(lab.Lab)
        lb.nodes = {k: {"host": types.SimpleNamespace(addr="127.0.0.1"), "group": 0, "admin": k} for k in ids}
        return lb

    real = lab.status
    lab.status = fake_status
    out = {}
    try:
        # The race: the primary's STATUS times out (None) on the first ask.
        lb = mk([1, 2])
        script.update({1: [None, None, "p"], 2: ["f"]})
        out["one_shot"] = lb.primary(0)
        assert out["one_shot"] is None, out  # what made lab.nodes[None] raise
        out["waited"] = lb.wait_primary(0, timeout=5, every=0.01)
        assert out["waited"] == 1, out
        # Two members claim primary: not an answer until one withdraws.
        lb = mk([1, 2])
        script.update({1: ["p", "p", "f"], 2: ["p"]})
        out["two_then_one"] = lb.wait_primary(0, timeout=5, every=0.01)
        assert out["two_then_one"] == 2, out
        # Node id 0 is a legitimate answer.
        lb = mk([0, 5])
        script.update({0: ["n"], 5: [None]})
        out["id_zero"] = lb.wait_primary(0, timeout=5, every=0.01)
        assert out["id_zero"] == 0, out
        # No primary ever: bounded, and the error names the last roles.
        lb = mk([1, 2])
        script.update({1: [None], 2: ["f"]})
        t = time.time()
        try:
            lb.wait_primary(0, timeout=0.5, every=0.01)
            raise AssertionError("wait_primary returned without a primary")
        except RuntimeError as e:
            out["none_error"] = str(e)
            out["none_s"] = round(time.time() - t, 2)
        assert "last roles {1: None, 2: 'f'}" in out["none_error"] and out["none_s"] < 3, out
    finally:
        lab.status = real
    return True, out


def main():
    failed = []
    for name, fn in (("primary", primary), ("clean", clean), ("violation", violation)):
        try:
            ok, detail = fn()
        except Exception as e:  # noqa: BLE001
            ok, detail = False, f"{type(e).__name__}: {e}"
        print(("PASS " if ok else "FAIL ") + name + " " + json.dumps(detail, default=str)[:1500])
        sys.stdout.flush()
        if not ok:
            failed.append(name)
    print("test_lab_witness_guard: " + ("OK" if not failed else f"FAILED {failed}"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
