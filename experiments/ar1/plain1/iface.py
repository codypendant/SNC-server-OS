import json
import os
import sys

import psycopg

port, call, arg = int(sys.argv[1]), sys.argv[2], sys.argv[3]
c = psycopg.connect(
    host="127.0.0.1",
    port=port,
    user="ar1",
    dbname="ar1",
    autocommit=True,
    application_name=f"ar1-{os.environ.get('AR1_EXECUTOR', 'iface')}-iface",
)


def rows(sql, *p):
    return c.execute(sql, p).fetchall()


def commits(where="true", *p):
    return [
        {"token": t, "xid": x, "epoch": e, "kind": k, "name": n}
        for t, x, e, k, n in rows(
            f"SELECT token, xid::text::bigint, epoch, kind, name FROM plain.commits WHERE {where} ORDER BY xid",
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
            i: {"state": s, "late": None, "retry_of": None}
            for i, s in rows("SELECT id, state FROM plain.invocation WHERE agent=%s", arg)
        },
        "commits": commits(),
    }


def reconstruct():
    out = {
        "shown": None,
        "authorized": None,
        "executed": None,
        "committed": None,
        "commits": commits("ref=%s", arg),
    }
    inv = rows(
        "SELECT manifest, whole_hash, state, epoch FROM plain.invocation WHERE id=%s",
        arg,
    )
    call = rows("SELECT state, attempts, epoch FROM plain.call WHERE id=%s", arg)
    if inv:
        items = [
            {
                "kind": i["kind"],
                "id": i["id"],
                "version": i["version"],
                "sha256": i["sha256"],
            }
            for i in inv[0][0]
        ]
        out["shown"] = {"items": items, "whole_hash": inv[0][1]}
        out["executed"] = {
            "attempts": [{"n": 1, "epoch": inv[0][3]}],
            "states": [inv[0][2]],
        }
    if call:
        out["executed"] = {
            "attempts": [{"n": n, "epoch": call[0][2]} for n in range(1, call[0][1] + 1)],
            "states": [call[0][0]],
        }
        d = rows("SELECT rule_version, approval FROM plain.decision WHERE id=%s", arg.rsplit(":", 1)[0] + ":P")
        if d:
            out["authorized"] = {"rule_version": d[0][0], "approval": d[0][1]}
    return out


def effects():
    ids = [
        r[0]
        for r in rows(
            "SELECT id FROM plain.invocation UNION ALL SELECT id FROM plain.call UNION ALL SELECT id FROM plain.memory"
        )
    ]
    return {"effects": ids}


print(
    json.dumps(
        {"state": state, "reconstruct": reconstruct, "effects": effects}[call](),
        default=str,
    )
)
