import copy
import json
import os
import subprocess
import sys
from pathlib import Path


def calls(r, tool=None):
    return [c for c in r.get("calls") or [] if tool in (None, c.get("tool"))]


def first(xs):
    return xs[0] if xs else {}


def drop_attempt(r):
    c = max(calls(r), key=lambda c: len(c.get("attempts") or []))
    c["attempts"].pop()


def set_path(*path, value):
    def f(r):
        o = r
        for k in path[:-1]:
            o = o[k]
        o[path[-1]] = value(o[path[-1]]) if callable(value) else value

    return f


def dup_policy(r):
    r["policy"].append(copy.deepcopy(r["policy"][0]))


def flip(d):
    return {"approve": "deny", "deny": "approve"}[d]


STATE = {
    "mem_tf": set_path("memory", "tf", value="T-F-99"),
    "mem_extra_key": set_path("memory", "extra", value=1),
    "mem_drop_tn": lambda r: r["memory"].pop("tn"),
    "mem_reply": set_path("memory", "reply", value=lambda v: v + "0"),
    "state_retention": set_path("retention", value=lambda v: f"{v} (edited)"),
    "state_task_id": set_path("task", "id", value="T2"),
    "status_done_to_doubt": set_path("task", "status", value="stopped_in_doubt"),
}
RECON = {
    "model_input": lambda r: r["model"][0]["input"].update(prompt="edited"),
    "model_output_sha": set_path("model", 0, "output_sha256", value="0" * 64),
    "model_used_false": set_path("model", 0, "used", value=False),
    "model_extra": lambda r: r["model"].append(
        dict(r["model"][0], invocation_id="T:model:x", request_id=None, used=False)
    ),
    "model_drop": set_path("model", value=[]),
    "attempt_drop": drop_attempt,
    "call_drop": lambda r: r["calls"].remove(first(calls(r, "T-F"))),
    "attempt_outcome": lambda r: first(calls(r, "T-I"))["attempts"][0].update(
        outcome="duplicate"
    ),
    "call_args": lambda r: first(calls(r, "T-I"))["args"].update(reply="rid-edited"),
    "call_token": lambda r: first(calls(r, "T-F")).update(
        token=first(calls(r, "T-F"))["token"] + 1
    ),
    "outcome_result": lambda r: first(calls(r, "T-I"))["outcome"].update(
        result={"effect": "T-I-99"}
    ),
    "rule_version_null": set_path("policy", 0, "rule_version", value=None),
    "policy_action": set_path("policy", 0, "action", value="T-I,T-F"),
    "policy_result": set_path(
        "policy",
        0,
        "result",
        value=lambda v: "allow" if v == "needs_approval" else "needs_approval",
    ),
    "policy_drop": set_path("policy", value=[]),
    "policy_dup": dup_policy,
    "approval_without_event": set_path(
        "policy", 0, "approval", value={"decision": "approve"}
    ),
    "approval_flip": set_path(
        "policy", 0, "approval", value=lambda a: {"decision": flip(a["decision"])}
    ),
    "approval_null": set_path("policy", 0, "approval", value=None),
    "recon_retention": set_path("retention", value=lambda v: f"{v} (edited)"),
}
DELIVER = ("deliver_drop", "deliver_drop_fail", "deliver_twice", "deliver_flip")


def main():
    fault, real, cmd, args = sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4:]
    cfg = json.loads((real / "arm.json").read_text())
    if cmd.startswith("engine:"):
        _, i, k = cmd.split(":")
        base = cfg["engines"][int(i)][k]
    else:
        base = cfg[cmd]
    argv = [a.replace("{arm}", str(real)) for a in base] + args
    os.chdir(real)
    table = {"state": STATE, "reconstruct": RECON}.get(cmd, {})
    if fault in table:
        p = subprocess.run(argv, stdout=subprocess.PIPE)
        out = p.stdout.decode()
        if p.returncode == 0 and os.environ.get("TRACKA_WORKER") == cmd:
            try:
                r = json.loads(out)
                table[fault](r)
                out = json.dumps(r) + "\n"
                note(fault, cmd)
            except Exception as e:
                note(fault, cmd, error=repr(e))
        sys.stdout.write(out)
        sys.exit(p.returncode)
    if cmd == "deliver" and fault in DELIVER:
        note(fault, cmd, args=args)
        if fault == "deliver_drop":
            sys.exit(0)
        if fault == "deliver_drop_fail":
            sys.exit(1)
        if fault == "deliver_flip":
            argv[-1] = flip(argv[-1])
        rc = subprocess.run(argv).returncode
        if fault == "deliver_twice" and rc == 0:
            rc = subprocess.run(argv).returncode
        sys.exit(rc)
    os.execvp(argv[0], argv)


def note(fault, cmd, **kw):
    import time

    path = Path(os.environ["TRACKA_RUN"]) / "logs" / "adequacy.jsonl"
    with open(path, "a") as f:
        rec = {"t": time.monotonic_ns(), "fault": fault, "cmd": cmd, **kw}
        f.write(json.dumps(rec) + "\n")


if __name__ == "__main__":
    main()
