import ast
import hashlib
import json
import os
import sys
import threading
from collections import Counter
from pathlib import Path
from typing import TypedDict

import httpx
from langgraph.checkpoint.postgres import PostgresSaver
from langgraph.func import task
from langgraph.graph import END, START, StateGraph
from langgraph.runtime import Runtime
from langgraph.store.postgres import PostgresStore
from langgraph.types import Command, RetryPolicy, interrupt
from psycopg.rows import dict_row
from psycopg_pool import ConnectionPool

SVC, PG = os.environ["TRACKA_SVC"], os.environ["TRACKA_PG"]
POLICY = json.loads((Path(__file__).parents[2] / "policy.json").read_text())
ACTION, TIMEOUT = "T-I,T-F,T-N", 30
RETENTION = "forever (no checkpointer or store TTL)"


class S(TypedDict, total=False):
    agent: str
    task: str
    scenario: str
    model: dict
    policy: dict
    decision: str
    ti: dict
    tf: dict
    tn: dict
    status: str


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


def post(path, body):
    return httpx.post(f"{SVC}/{path}", json=body, timeout=TIMEOUT)


def effect(rec):
    return rec["outcome"]["result"]["effect"]


class Stale(Exception):
    pass


def attempt(name, runtime: Runtime):
    return runtime.context["tries"][name] + runtime.execution_info.node_attempt


def model(s: S, runtime: Runtime):
    n = attempt("model", runtime)
    inv = f"{s['task']}:model:{n}"
    body = {"agent": s["agent"], "task": s["task"], "prompt": f"plan {s['task']}"}
    r = post(
        "stub",
        {
            "invocation_id": inv,
            "retry_of": f"{s['task']}:model:{n - 1}" if n > 1 else None,
            "agent": s["agent"],
            "task": s["task"],
            "body": body,
        },
    )
    head, reply = map(json.loads, r.text.splitlines())
    rec = {"request_id": head["request_id"], "output": reply["output"]}
    return {"model": {"invocation_id": inv, "input": body, **rec}}


def policy(s: S):
    rule = POLICY["rules"][s["scenario"]]
    result = "needs_approval" if rule["approval"] else "allow"
    return {
        "policy": {
            "rule_version": POLICY["version"],
            "action": ACTION,
            "result": result,
        }
    }


@task
def ask(t):
    post("approval", {"task": t, "action": ACTION, "thread_id": t})


def approval(s: S):
    ask(s["task"]).result()
    return {"decision": interrupt({"task": s["task"], "action": ACTION})}


def tool(name, field, args):
    def node(s: S, runtime: Runtime, config):
        n, call_id = attempt(field, runtime), f"{s['task']}:{name}"
        rec = {
            "tool": name,
            "call_id": call_id,
            "key": call_id,
            "token": config["metadata"]["langgraph_step"] if name == "T-F" else None,
            "args": args(s),
        }
        attempts = [
            {"attempt": i, "outcome": "unknown"}
            for i in range(n if name == "T-F" else 1, n)
        ]
        if attempts and name == "T-N":
            q = {"task": s["task"], "call_id": call_id}
            answer = httpx.get(f"{SVC}/owner", params=q, timeout=TIMEOUT).json()
            attempts[-1]["outcome"] = f"owner:{answer['answer']}"
            if answer["answer"] == "happened":
                out = {"kind": "owner:happened", "result": None}
                return {field: {**rec, "attempts": attempts, "outcome": out}}
        req = {**rec, "attempt": n, "agent": s["agent"], "task": s["task"]}
        out = post(name, req).json()
        attempts.append({"attempt": n, "outcome": out["kind"]})
        rec = {field: {**rec, "attempts": attempts, "outcome": out}}
        if out["kind"] == "rejected_stale":
            raise Stale(json.dumps(rec[field]))
        return rec

    return node


def remember(s: S, runtime: Runtime):
    rid = s["model"]["request_id"]
    if s.get("decision") == "deny":
        memory, status = {"reply": rid, "denied": True}, "denied"
    else:
        tn = s["tn"]["outcome"]
        tn = tn["kind"] if tn["kind"] == "owner:happened" else tn["result"]["effect"]
        memory = {"reply": rid, "ti": effect(s["ti"]), "tf": effect(s["tf"]), "tn": tn}
        status = "done"
    runtime.store.put(("agents", s["agent"]), "memory", memory)
    return {"status": status}


g = StateGraph(S, context_schema=dict)
g.add_node(model)
g.add_node(policy)
g.add_node(approval)
g.add_node("ti", tool("T-I", "ti", lambda s: {"reply": s["model"]["request_id"]}))
g.add_node(
    "tf",
    tool(
        "T-F",
        "tf",
        lambda s: {"reply": s["model"]["request_id"], "ti": effect(s["ti"])},
    ),
)
g.add_node(
    "tn",
    tool(
        "T-N",
        "tn",
        lambda s: {"reply": s["model"]["request_id"], "tf": effect(s["tf"])},
    ),
    retry_policy=RetryPolicy(retry_on=httpx.TransportError),
)
g.add_node(remember)
g.add_edge(START, "model")
g.add_edge("model", "policy")
g.add_conditional_edges(
    "policy",
    lambda s: "approval" if s["policy"]["result"] == "needs_approval" else "ti",
)
g.add_conditional_edges(
    "approval", lambda s: "ti" if s["decision"] == "approve" else "remember"
)
g.add_edge("ti", "tf")
g.add_edge("tf", "tn")
g.add_edge("tn", "remember")
g.add_edge("remember", END)


def thread(task):
    return {"configurable": {"thread_id": task}}


def run(graph, cfg, value, prep=lambda: None):
    tries = Counter(n for h in graph.get_state_history(cfg) for n in h.next)
    prep()
    graph.invoke(value, cfg, context={"tries": tries}, durability="sync")


def distinct(snaps, field):
    seen = {
        json.dumps(s.values[field], sort_keys=True): s.values[field]
        for s in snaps
        if field in s.values
    }
    return list(seen.values())


def reconstruct(graph, task):
    snaps = list(graph.get_state_history(thread(task)))[::-1]
    last = snaps[-1].values if snaps else {}
    used = last.get("model", {}).get("request_id")
    model = [
        {
            "invocation_id": m["invocation_id"],
            "request_id": m["request_id"],
            "input": m["input"],
            "output_sha256": sha(m["output"]),
            "used": m["request_id"] == used,
        }
        for m in distinct(snaps, "model")
    ]
    stale = {
        t.error: json.loads(ast.literal_eval(t.error[6:-1]))
        for s in snaps
        for t in s.tasks
        if t.error and t.error.startswith("Stale(")
    }
    approval = {"decision": last["decision"]} if "decision" in last else None
    return {
        "model": model,
        "policy": [{**p, "approval": approval} for p in distinct(snaps, "policy")],
        "calls": [c for f in ("ti", "tf", "tn") for c in distinct(snaps, f)]
        + list(stale.values()),
        "retention": RETENTION,
    }


def main(cmd, *args):
    env = os.environ
    kwargs = {"autocommit": True, "prepare_threshold": 0, "row_factory": dict_row}
    with ConnectionPool(PG, kwargs=kwargs, check=ConnectionPool.check_connection) as p:
        saver, store = PostgresSaver(p), PostgresStore(p)
        graph = g.compile(checkpointer=saver, store=store)
        if cmd == "setup":
            saver.setup()
            store.setup()
        elif cmd == "worker":
            cfg = thread(env["TRACKA_TASK"])
            new = {
                "agent": env["TRACKA_AGENT"],
                "task": env["TRACKA_TASK"],
                "scenario": env["TRACKA_SCENARIO"],
            }
            snap = graph.get_state(cfg)
            run(
                graph,
                cfg,
                None if snap.created_at else new,
                lambda: snap.next and graph.update_state(cfg, None, as_node="__copy__"),
            )
            threading.Event().wait()
        elif cmd == "deliver":
            task, _, decision = args
            cfg = thread(task)
            if graph.get_state(cfg).interrupts:
                run(graph, cfg, Command(resume=decision))
            else:
                update = {"decision": decision}
                prep = lambda: graph.update_state(cfg, update, as_node="approval")
                run(graph, cfg, None, prep)
        elif cmd == "state":
            values = graph.get_state(thread(env["TRACKA_TASK"])).values
            item = store.get(("agents", args[0]), "memory")
            status = values.get("status", "running")
            print(
                json.dumps(
                    {
                        "task": {"id": env["TRACKA_TASK"], "status": status},
                        "memory": item and item.value,
                        "retention": RETENTION,
                    }
                )
            )
        elif cmd == "reconstruct":
            print(json.dumps(reconstruct(graph, args[0])))


if __name__ == "__main__":
    main(*sys.argv[1:])
