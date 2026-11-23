import json
import os
import sys

import psycopg

from . import rt

port, call, arg = int(sys.argv[1]), sys.argv[2], sys.argv[3]
c = psycopg.connect(
    host="127.0.0.1",
    port=port,
    user="ar1",
    dbname="ar1",
    autocommit=True,
    application_name=f"ar1-{os.environ.get('AR1_EXECUTOR', 'iface')}-iface",
)
c.execute("BEGIN ISOLATION LEVEL REPEATABLE READ")


def rows(sql, *p):
    return c.execute(sql, p).fetchall()


def commits(where="true", *p):
    return [
        {"token": t, "xid": x, "epoch": e, "kind": k, "name": n, "state": (d or {}).get(arg, s), "data": d}
        for t, x, e, k, n, s, d in rows(
            f"SELECT token, xid::text::bigint, epoch, kind, name, state, data FROM plain.commits WHERE {where} ORDER BY xid",
            *p,
        )
    ]


def state():
    a = rows("SELECT id, epoch, version FROM plain.agent WHERE id=%s", arg)[0]
    t = rows("SELECT id, status FROM plain.task WHERE agent=%s", arg)[0]
    return {
        "agent": {"id": a[0], "epoch": a[1], "version": a[2]},
        "task": {"id": t[0], "status": t[1]},
        "memory": [
            {"id": i, "version": v, "content_hash": h}
            for i, v, h in rows(
                "SELECT id, version, content_hash FROM plain.memory WHERE agent=%s ORDER BY id",
                arg,
            )
        ],
        "calls": {
            i: {"state": s, "attempts": n, "result": r}
            for i, s, n, r in rows("SELECT id, state, attempts, result FROM plain.call WHERE agent=%s", arg)
        },
        "invocations": {
            i: {"state": s, "late": l, "retry_of": r}
            for i, s, l, r in rows("SELECT id, state, late, retry_of FROM plain.invocation")
        },
        "commits": commits(),
        "snapshot": rows("SELECT pg_current_snapshot()::text")[0][0],
    }


def reconstruct():
    cs = commits("ref=%s OR data ? %s", arg, arg)
    disp = [x for x in cs if "-disp" in x["name"]]
    out = {
        "shown": None,
        "authorized": None,
        "executed": {
            "attempts": [{"n": k + 1, "epoch": x["epoch"]} for k, x in enumerate(disp)],
            "states": [x["state"] for x in cs],
        },
        "committed": {"transitions": [{k: x[k] for k in ("token", "name", "state")} for x in cs]},
        "commits": cs,
    }
    inv = rows("SELECT body, whole_hash FROM plain.invocation WHERE id=%s", arg)
    call = rows(
        "SELECT i.body, i.whole_hash, d.rule_version, d.approval FROM plain.call c"
        " JOIN plain.decision d ON d.id = c.policy JOIN plain.invocation i ON i.id = d.inv WHERE c.id=%s",
        arg,
    )
    if inv or call:
        out["shown"] = {"items": (inv or call)[0][0]["items"], "whole_hash": (inv or call)[0][1]}
    if call:
        out["authorized"] = {"rule_version": call[0][2], "approval": call[0][3]}
    return out


def effects():
    ids = [
        r[0]
        for r in rows(
            "SELECT id FROM plain.invocation UNION ALL SELECT id FROM plain.call UNION ALL SELECT id FROM plain.memory"
        )
    ]
    return {"effects": ids}


ans = {"state": state, "reconstruct": reconstruct, "effects": effects}[call]()
rt.wait()
print(json.dumps(ans, default=str))
