import hashlib
import json
import os
import sys
from pathlib import Path

import sqlalchemy as sa

PG, SVC = os.environ["TRACKA_PG"], os.environ.get("TRACKA_SVC")
APP, QUEUE, TOPIC = "tracka", "agents", "approval"
POLICY = json.loads((Path(__file__).parent / "../../policy.json").read_text())
RETENTION = "forever: no DBOS workflow retention or garbage collection is configured"


def canon(v):
    return json.dumps(v, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def model_input(agent, task):
    return {"agent": agent, "task": task, "prompt": f"Plan task {task} for {agent}."}


def engine():
    return sa.create_engine(sa.make_url(PG).set(drivername="postgresql+psycopg"))


def client():
    from dbos import DBOSClient

    return DBOSClient(system_database_url=PG, application_name=APP)


def setup():
    from dbos import DBOS, SQLAlchemyDatasource

    DBOS.migrate(PG)
    SQLAlchemyDatasource.migrate(PG)
    with engine().begin() as c:
        c.execute(sa.text("CREATE TABLE agent (id text PRIMARY KEY, memory jsonb)"))
        c.execute(
            sa.text(
                "CREATE TABLE task (id text PRIMARY KEY,"
                " agent text NOT NULL REFERENCES agent, status text NOT NULL)"
            )
        )
        c.execute(
            sa.text("INSERT INTO agent (id) VALUES (:a)"),
            {"a": os.environ["TRACKA_AGENT"]},
        )


def submit():
    env = os.environ
    options = {
        "queue_name": QUEUE,
        "workflow_name": "run_task",
        "workflow_id": env["TRACKA_TASK"],
        "queue_partition_key": env["TRACKA_AGENT"],
    }
    client().enqueue(
        options, env["TRACKA_AGENT"], env["TRACKA_TASK"], env["TRACKA_SCENARIO"]
    )


def deliver(task, action, decision):
    client().send(task, decision, TOPIC, idempotency_key=f"{task}:{action}")


def state(agent):
    with engine().connect() as c:
        memory = c.execute(
            sa.text("SELECT memory FROM agent WHERE id = :a"), {"a": agent}
        ).scalar()
        status = c.execute(
            sa.text("SELECT status FROM task WHERE agent = :a"), {"a": agent}
        ).scalar()
    return {
        "task": {"id": os.environ["TRACKA_TASK"], "status": status or "running"},
        "memory": memory,
        "retention": RETENTION,
    }


def reconstruct(task):
    c = client()
    agent, _, _ = c.retrieve_workflow(task).get_status().input["args"]
    out = {"model": [], "policy": [], "calls": [], "retention": RETENTION}
    for step in c.list_workflow_steps(task):
        name, o = step["function_name"], step["output"]
        if name == "model":
            o = dict(o, input=model_input(agent, task), used=True)
            text = o.pop("output")
            text = text if isinstance(text, str) else canon(text)
            o["output_sha256"] = hashlib.sha256(text.encode()).hexdigest()
            o.pop("model")
            out["model"].append(o)
        elif name == "decide":
            out["policy"].append(dict(o, approval=None))
        elif name == "DBOS.recv":
            out["policy"][-1]["approval"] = {"decision": o}
        elif name in ("tool", "send_tn"):
            if name == "send_tn":
                o = child_output(c, step["child_workflow_id"])
            kind = o["outcome"]["kind"] if o["outcome"] else "unknown"
            attempt = {"attempt": o.pop("attempt"), "outcome": kind}
            if out["calls"] and out["calls"][-1]["tool"] == o["tool"]:
                out["calls"][-1]["attempts"].append(attempt)
                out["calls"][-1]["outcome"] = o["outcome"]
            else:
                out["calls"].append(dict(o, attempts=[attempt]))
        elif name == "owner":
            out["calls"][-1]["attempts"][-1]["outcome"] = f"owner:{o}"
            if o == "happened":
                out["calls"][-1]["outcome"] = {"kind": "owner:happened", "result": None}
    return out


def child_output(c, wid):
    steps = c.list_workflow_steps(wid)
    if steps and steps[0]["output"]:
        return steps[0]["output"]
    agent, task, args, attempt = c.retrieve_workflow(wid).get_status().input["args"]
    cid = f"{task}:T-N"
    call = {"tool": "T-N", "call_id": cid, "key": cid, "attempt": attempt}
    return dict(call, agent=agent, task=task, token=None, args=args, outcome=None)


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "worker":
        import app

        app.main()
    result = globals()[cmd](*args)
    if cmd in ("state", "reconstruct"):
        print(json.dumps(result))
