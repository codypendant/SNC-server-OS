import itertools
import json
import threading
import urllib.request
from urllib.error import HTTPError

import sqlalchemy as sa
from dbos import DBOS, SQLAlchemyDatasource

from arm import APP, PG, POLICY, QUEUE, SVC, TOPIC, model_input

RECV_TIMEOUT_S, TN_TIMEOUT_S = 3600, 30

ds = SQLAlchemyDatasource.create(PG, run_migrations=False)


def post(path, body, timeout=None):
    req = urllib.request.Request(
        SVC + path, json.dumps(body).encode(), {"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return [json.loads(line) for line in r if line.strip()]
    except HTTPError as e:
        if e.code != 409:
            raise
        return [json.loads(e.read())]


@DBOS.step()
def model(agent, task):
    inv = f"{task}:model"
    body = {"invocation_id": inv, "retry_of": None, "agent": agent, "task": task}
    head, reply = post("/stub", {**body, "body": model_input(agent, task)})
    return {"invocation_id": inv, "request_id": head["request_id"], **reply}


@DBOS.step()
def decide(scenario):
    result = "needs_approval" if POLICY["rules"][scenario]["approval"] else "allow"
    return {
        "rule_version": POLICY["version"],
        "action": "T-I,T-F,T-N",
        "result": result,
    }


@DBOS.step()
def ask_approval(task, action):
    return post("/approval", {"task": task, "action": action})[0]


@DBOS.step()
def tool(name, agent, task, args, attempt=1):
    token = DBOS.get_workflow_status(task).dequeued_at if name == "T-F" else None
    cid = f"{task}:{name}"
    req = {"call_id": cid, "key": cid, "attempt": attempt, "agent": agent}
    req.update(task=task, token=token, args=args)
    timeout = TN_TIMEOUT_S if name == "T-N" else None
    return {"tool": name, **req, "outcome": post("/" + name, req, timeout)[0]}


@DBOS.step()
def owner(task):
    with urllib.request.urlopen(f"{SVC}/owner?task={task}&call_id={task}:T-N") as r:
        return json.load(r)["answer"]


@DBOS.workflow(name="send_tn", max_recovery_attempts=0)
def send_tn(agent, task, args, attempt):
    return tool("T-N", agent, task, args, attempt)


def tn(agent, task, args):
    for attempt in itertools.count(1):
        try:
            child = DBOS.start_workflow(send_tn, agent, task, args, attempt)
            return child.get_result()["outcome"]["result"]["effect"]
        except Exception:
            if owner(task) == "happened":
                return "owner:happened"


@ds.transaction()
def finish(agent, task, status, memory):
    s = ds.sql_session()
    s.execute(
        sa.text("UPDATE agent SET memory = :m WHERE id = :a"),
        {"m": json.dumps(memory), "a": agent},
    )
    s.execute(
        sa.text("INSERT INTO task (id, agent, status) VALUES (:t, :a, :s)"),
        {"t": task, "a": agent, "s": status},
    )
    return status


@DBOS.workflow(name="run_task")
def run_task(agent, task, scenario):
    rid = model(agent, task)["request_id"]
    policy = decide(scenario)
    if policy["result"] == "needs_approval":
        ask_approval(task, policy["action"])
        if DBOS.recv(TOPIC, RECV_TIMEOUT_S) != "approve":
            return finish(agent, task, "denied", {"reply": rid, "denied": True})
    ti = tool("T-I", agent, task, {"reply": rid})["outcome"]["result"]["effect"]
    tf = tool("T-F", agent, task, {"reply": rid, "ti": ti})["outcome"]["result"]
    memory = {"reply": rid, "ti": ti, "tf": tf["effect"]}
    memory["tn"] = tn(agent, task, {"reply": rid, "tf": tf["effect"]})
    return finish(agent, task, "done", memory)


def main():
    DBOS(config={"name": APP, "system_database_url": PG})
    DBOS.launch()
    DBOS.register_queue(QUEUE, partition_concurrency=1)
    threading.Event().wait()


if __name__ == "__main__":
    main()
