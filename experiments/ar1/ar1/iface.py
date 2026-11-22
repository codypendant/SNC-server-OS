import json
import os
import sys

os.environ.setdefault("AR1_EXECUTOR", "iface")
from . import rt  # noqa: E402

COMMITS = ("SELECT token, xid::text, epoch, kind, name FROM snc.commit_token ORDER BY token", ())


def commits(rows):
    return [dict(zip(("token", "xid", "epoch", "kind", "name"), r)) for r in rows]


def state(agent):
    ag, task, mem, calls, invs, toks, snap = rt.authoritative(
        [
            ("SELECT id, epoch, version FROM snc.agent WHERE id=%s", (agent,)),
            ("SELECT id, status FROM snc.task WHERE agent=%s", (agent,)),
            ("SELECT id, version, content_hash FROM snc.memory WHERE agent=%s ORDER BY id, version", (agent,)),
            ("SELECT id, state, attempts, result FROM snc.tool_call WHERE agent=%s ORDER BY id", (agent,)),
            ("SELECT id, state, late, retry_of FROM snc.invocation WHERE agent=%s ORDER BY id", (agent,)),
            COMMITS,
            ("SELECT pg_current_snapshot()::text", ()),
        ]
    )
    return {
        "agent": dict(zip(("id", "epoch", "version"), ag[0])),
        "task": dict(zip(("id", "status"), task[0])) if task else None,
        "memory": [dict(zip(("id", "version", "content_hash"), m)) for m in mem],
        "calls": {c[0]: {"state": c[1], "attempts": c[2], "result": c[3]} for c in calls},
        "invocations": {i[0]: {"state": i[1], "late": i[2], "retry_of": i[3]} for i in invs},
        "commits": commits(toks),
        "snapshot": snap[0][0],
    }


def effects(agent):
    calls, invs, mem = rt.authoritative(
        [
            ("SELECT id FROM snc.tool_call WHERE agent=%s ORDER BY id", (agent,)),
            ("SELECT id FROM snc.invocation WHERE agent=%s ORDER BY id", (agent,)),
            ("SELECT DISTINCT id FROM snc.memory WHERE agent=%s AND token <> 'setup' ORDER BY id", (agent,)),
        ]
    )
    return {"effects": [r[0] for r in calls + invs + mem]}


def _manifest(inv_row):
    items, whole = inv_row
    return {"items": items, "whole_hash": whole}


def reconstruct(effect):
    call, inv, att, hist, pol, apr, toks, invs, mem = rt.authoritative(
        [
            ("SELECT id, tool, key, state, policy_id, approval_id, result FROM snc.tool_call WHERE id=%s", (effect,)),
            (
                "SELECT id, state, manifest, whole_hash, provider_request_id, late, retry_of FROM snc.invocation WHERE id=%s",
                (effect,),
            ),
            ("SELECT n, epoch, key, token FROM snc.attempt WHERE call_id=%s ORDER BY n", (effect,)),
            (
                "SELECT kind, obj, from_state, to_state, epoch, detail, token FROM snc.history WHERE obj=%s ORDER BY id",
                (effect,),
            ),
            (
                "SELECT id, rule_version, inputs_hash, result, obligations, token FROM snc.policy_decision ORDER BY id",
                (),
            ),
            ("SELECT id, policy_id, decision, token FROM snc.approval ORDER BY id", ()),
            COMMITS,
            ("SELECT id, state, manifest, whole_hash FROM snc.invocation WHERE state='output' ORDER BY id", ()),
            ("SELECT id, version, content_hash, token FROM snc.memory WHERE id=%s ORDER BY version", (effect,)),
        ]
    )
    tokens = {h[6] for h in hist} | {a[3] for a in att} | {m[3] for m in mem}
    if call:
        tokens |= {x[5] for x in pol if x[0] == call[0][4]} | {x[3] for x in apr if x[0] == call[0][5]}
    tokens |= {t[0] for t in toks if t[3] == "takeover"}
    shown = None
    authorized = None
    executed = {"attempts": [], "states": []}
    if call:
        c = call[0]
        p = next((x for x in pol if x[0] == c[4]), None)
        a = next((x for x in apr if x[0] == c[5]), None)
        authorized = {
            "rule_version": p[1] if p else None,
            "policy_id": c[4],
            "approval": {"id": a[0], "decision": a[2]} if a else None,
        }
        attempts = [{"n": n, "epoch": e, "key": k} for n, e, k, _ in att]
        if rt.MUT == "M7" and attempts:
            attempts = attempts[:-1]
        executed = {"attempts": attempts, "states": [h[3] for h in hist if h[0] == "call"], "result": c[6]}
        if invs:
            shown = _manifest(invs[0][2:4])
    elif inv:
        i = inv[0]
        shown = _manifest(i[2:4])
        executed = {
            "attempts": [],
            "states": [h[3] for h in hist if h[0] == "invocation"],
            "provider_request_id": i[4],
            "late": i[5],
            "retry_of": i[6],
        }
    elif mem:
        executed = {"attempts": [], "states": ["written"]}
    transitions = [{"from": h[2], "to": h[3], "epoch": h[4], "token": h[6], "kind": h[0], "detail": h[5]} for h in hist]
    return {
        "shown": shown,
        "authorized": authorized,
        "executed": executed,
        "committed": {"transitions": transitions},
        "commits": [c for c in commits(toks) if c["token"] in tokens],
    }


def main():
    port, cmd, arg = sys.argv[1], sys.argv[2], sys.argv[3]
    rt.PORT = int(port)
    try:
        out = {"state": state, "reconstruct": reconstruct, "effects": effects}[cmd](arg)
    except rt.TimelineChanged as e:
        out = {"refused": str(e)}
    print(json.dumps(out, default=str))


if __name__ == "__main__":
    main()
