import json
import os
import pwd
import signal
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

AR1 = Path(__file__).resolve().parents[3] / "ar1"
sys.path.insert(0, str(AR1))
from ar1 import pg  # noqa: E402

TR = Path(os.environ["TRACKA_RUN"])
S = TR / "snc"
PAIR = pg.Pair(S / "pg", int(os.environ["TRACKA_PORTS"]) - 1)
UP = S / "primary-up"
RETENTION = (
    "all: snc_rt deletes no record of an agent, task, invocation, call or attempt"
)
PGUSER = os.environ.get("TRACKA_PGUSER", "pgrun")


def ar1_env(executor):
    os.environ.update(
        AR1_RUN=str(S),
        AR1_EXECUTOR=executor,
        AR1_SEED=os.environ["TRACKA_SEED"],
        AR1_POINTS="",
        AR1_MUTATION="",
        DBOS__APPVERSION="ar1-snc-1",
    )
    os.chdir(AR1)


def as_pgrun():
    S.mkdir(exist_ok=True)
    if os.geteuid() == 0:
        u = pwd.getpwnam(PGUSER)
        os.chown(S, u.pw_uid, u.pw_gid)
        os.initgroups(PGUSER, u.pw_gid)
        os.setgid(u.pw_gid)
        os.setuid(u.pw_uid)


def hold(node, on_stop=lambda: None):
    def stop(*_):
        on_stop()
        node.stop("fast")
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    while True:
        time.sleep(1)


def primary():
    as_pgrun()
    if (PAIR.primary.data / "PG_VERSION").exists():
        PAIR.primary.start()
    else:
        PAIR.create()
    UP.write_text("1")
    hold(PAIR.primary, lambda: UP.unlink(missing_ok=True))


def standby():
    as_pgrun()
    if not (PAIR.standby.data / "postmaster.pid").exists():
        PAIR.standby.start()
    hold(PAIR.standby)


def ready(node):
    with PAIR.primary.conn() as c:
        if node == "primary":
            sys.exit(0 if UP.exists() and c.execute("SELECT 1").fetchone() else 1)
        r = c.execute(
            "SELECT sync_state FROM pg_stat_replication WHERE application_name='s1'"
        ).fetchone()
        sys.exit(0 if r and r[0] == "sync" else 1)


def setup():
    svc = urlparse(os.environ["TRACKA_SVC"])
    assert svc.hostname == "127.0.0.1"
    tr = json.loads((TR / "run.json").read_text())
    (S / "ctl").mkdir()
    (S / "ctl" / "go-takeover").write_text("1")
    run = {
        "point": tr["point"],
        "seed": tr["seed"],
        "arm": "snc",
        "mutation": "",
        "scenario": os.environ["TRACKA_SCENARIO"],
        "svc_port": svc.port,
        "app_name_prefix": "ar1-",
        "policy_version": json.loads((AR1 / "policy.json").read_text())["version"],
        "pg": {
            "bin": str(pg.BIN),
            "primary_port": PAIR.primary.port,
            "standby_port": PAIR.standby.port,
            "primary_data": str(PAIR.primary.data),
            "standby_data": str(PAIR.standby.data),
        },
    }
    (S / "run.json").write_text(json.dumps(run, indent=1))
    ar1_env("setup")
    from ar1 import rt

    rt.setup_db(S / "blobs")


def worker():
    n = int(os.environ["TRACKA_WORKER"][1:])
    ex = f"e{n}"
    with PAIR.primary.conn(pg.ADMIN, "postgres") as c:
        db = f"{pg.DB}_sys_{ex}"
        if not c.execute(
            "SELECT 1 FROM pg_database WHERE datname=%s", (db,)
        ).fetchone():
            c.execute(f"CREATE DATABASE {db} OWNER {pg.RUNTIME}")
    ar1_env(ex)
    sys.argv = ["ar1.app", "start" if n == 1 else "takeover", str(n)]
    from ar1 import app

    app.main()
    while True:
        signal.pause()


def deliver(*_):
    sys.exit("snc: no asynchronous approval ingress (plan section 3); S-AP is not run")


def read():
    ar1_env("iface")
    from ar1 import iface, rt

    return iface, rt


def blob(h):
    return (S / "blobs" / h).read_text()


def state(agent):
    iface, _ = read()
    st = iface.state(agent)
    mine = set(iface.effects(agent)["effects"])
    mem = {m["id"]: blob(m["content_hash"]) for m in st["memory"] if m["id"] in mine}
    status = (st["task"] or {}).get("status")
    return {
        "task": {
            "id": "T",
            "status": "running" if status in (None, "accepted") else status,
        },
        "memory": next(iter(mem.values())) if len(mem) == 1 else mem or None,
        "retention": RETENTION,
    }


def model_input(rt, shown):
    rows = [(i["id"], i["version"], i["sha256"]) for i in shown["items"]]
    _, body = rt.manifest(rows)
    return body if rt.sha(body) == shown["whole_hash"] else None


def attempt_outcome(n, trans, result):
    for t in trans:
        d = t["detail"] or {}
        if d.get("attempt") != n:
            continue
        if t["kind"] == "call" and t["to"] in ("completed", "rejected"):
            if d.get("by") == "owner":
                return "owner:happened", result
            return ("effect" if t["to"] == "completed" else "rejected_stale"), result
        if t["kind"] == "attempt":
            kind = "duplicate" if t["to"] == "duplicate" else "rejected_stale"
            return kind, d.get("result")
    return "unknown", None


def calls(rt, cid, r):
    tool = dict((pre, tool) for tool, pre in rt.TOOLS)[cid.split(".")[0]]
    trans = r["committed"]["transitions"]
    out = {}
    for a in r["executed"]["attempts"]:
        kind, res = attempt_outcome(a["n"], trans, r["executed"]["result"])
        e = out.setdefault(
            a["epoch"],
            {
                "tool": tool,
                "call_id": cid,
                "key": a["key"],
                "token": a["epoch"],
                "args": rt.tool_args(tool),
                "attempts": [],
            },
        )
        e["attempts"].append({"attempt": a["n"], "outcome": kind})
        e["outcome"] = {"kind": kind, "result": res}
    return list(out.values())


def reconstruct(task):
    iface, rt = read()
    st = iface.state("A")
    model, policy, entries, pids = [], [], [], {}
    for iid, inv in sorted(st["invocations"].items()):
        r = iface.reconstruct(iid)
        if "dispatched" not in r["executed"]["states"]:
            continue
        sums = [
            t["detail"]["output_sha256"]
            for t in r["committed"]["transitions"]
            if t["kind"] == "derive" and "output_sha256" in (t["detail"] or {})
        ]
        model.append(
            {
                "invocation_id": iid,
                "request_id": r["executed"]["provider_request_id"],
                "input": model_input(rt, r["shown"]),
                "output_sha256": sums[0] if sums else None,
                "used": inv["state"] == "output",
            }
        )
    for cid in sorted(st["calls"]):
        r = iface.reconstruct(cid)
        entries += calls(rt, cid, r)
        pids[r["authorized"]["policy_id"]] = r["authorized"]
    for c in st["commits"]:
        if c["name"] == "P-dec":
            pids.setdefault(f"P.e{c['epoch']}", None)
    for pid, auth in sorted(pids.items()):
        dec = [
            t["to"]
            for t in iface.reconstruct(pid)["committed"]["transitions"]
            if t["kind"] == "policy"
        ]
        if not dec:
            continue
        ap = auth and auth["approval"]
        policy.append(
            {
                "rule_version": auth and auth["rule_version"],
                "action": ",".join(tool for tool, _ in rt.TOOLS),
                "result": "needs_approval" if ap else dec[0],
                "approval": {"decision": ap["decision"]} if ap else None,
            }
        )
    return {"model": model, "policy": policy, "calls": entries, "retention": RETENTION}


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd in ("state", "reconstruct"):
        out = {"state": state, "reconstruct": reconstruct}[cmd](*args)
        print(json.dumps(out, default=str))
        return
    {
        "primary": primary,
        "standby": standby,
        "ready": ready,
        "setup": setup,
        "worker": worker,
        "deliver": deliver,
    }[cmd](*args)


if __name__ == "__main__":
    main()
