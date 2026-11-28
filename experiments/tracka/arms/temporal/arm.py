import asyncio
import hashlib
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta
from pathlib import Path
from urllib.parse import urlparse

from google.protobuf.duration_pb2 import Duration
from temporalio.api.workflowservice.v1 import (
    DescribeNamespaceRequest,
    RegisterNamespaceRequest,
)
from temporalio.client import Client
from temporalio.service import RPCError, RPCStatusCode
from temporalio.worker import Worker

from agent import ACTIVITIES, Agent

ARM = Path(__file__).resolve().parent
R = Path(os.environ["TRACKA_RUN"]) / "temporal"
PG = urlparse(os.environ["TRACKA_PG"])
PORT = int(os.environ["TRACKA_PORTS"])
ADDR = f"127.0.0.1:{PORT}"
NS, QUEUE, RETENTION = "tracka", "tracka", timedelta(days=30)
AGENT, TASK = os.environ["TRACKA_AGENT"], os.environ["TRACKA_TASK"]
WF = f"agent-{AGENT}"

CONFIG = """
log: {{stdout: true, level: warn}}
persistence:
  defaultStore: default
  visibilityStore: visibility
  numHistoryShards: 4
  datastores:
    default: {{sql: {{pluginName: postgres12, databaseName: temporal, connectAddr: "{pg}", connectProtocol: tcp, user: {user}}}}}
    visibility: {{sql: {{pluginName: postgres12, databaseName: temporal_visibility, connectAddr: "{pg}", connectProtocol: tcp, user: {user}}}}}
global:
  membership: {{maxJoinDuration: 30s, broadcastAddress: 127.0.0.1}}
services:
  frontend: {{rpc: {{grpcPort: {p0}, httpPort: {p1}, membershipPort: {p2}, bindOnLocalHost: true}}}}
  history: {{rpc: {{grpcPort: {p3}, membershipPort: {p4}, bindOnLocalHost: true}}}}
  matching: {{rpc: {{grpcPort: {p5}, membershipPort: {p6}, bindOnLocalHost: true}}}}
  worker: {{rpc: {{grpcPort: {p7}, membershipPort: {p8}, bindOnLocalHost: true}}}}
clusterMetadata:
  enableGlobalNamespace: false
  failoverVersionIncrement: 10
  masterClusterName: active
  currentClusterName: active
  clusterInformation:
    active: {{enabled: true, initialFailoverVersion: 1, rpcName: frontend, rpcAddress: "{addr}"}}
"""


def server():
    conf = R / "server.yaml"
    if not conf.exists():
        R.mkdir(parents=True, exist_ok=True)
        for db, schema in (
            ("temporal", "temporal"),
            ("temporal_visibility", "visibility"),
        ):
            sql = [ARM / "bin/temporal-sql-tool", "--plugin", "postgres12"]
            sql += [
                "--ep",
                PG.hostname,
                "-p",
                str(PG.port),
                "-u",
                PG.username,
                "--db",
                db,
            ]
            subprocess.run(sql + ["create-database"], check=True)
            subprocess.run(sql + ["setup-schema", "-v", "0.0"], check=True)
            name = f"postgresql/v12/{schema}"
            subprocess.run(sql + ["update-schema", "--schema-name", name], check=True)
        ports = {f"p{i}": PORT + i for i in range(9)}
        pg = f"{PG.hostname}:{PG.port}"
        conf.write_text(CONFIG.format(pg=pg, user=PG.username, addr=ADDR, **ports))
    bin = str(ARM / "bin/temporal-server")
    os.execv(bin, [bin, "--config-file", str(conf), "--allow-no-auth", "start"])


async def client():
    return await Client.connect(ADDR, namespace=NS)


async def ready():
    await (await client()).service_client.check_health()


async def setup():
    c = await client()
    retention = Duration()
    retention.FromTimedelta(RETENTION)
    req = RegisterNamespaceRequest(
        namespace=NS, workflow_execution_retention_period=retention
    )
    await c.workflow_service.register_namespace(req)
    await c.count_workflows()
    print(json.dumps({"namespace": NS, "retention": await retention_of(c)}))


async def retention_of(c):
    d = await c.workflow_service.describe_namespace(
        DescribeNamespaceRequest(namespace=NS)
    )
    ttl = d.config.workflow_execution_retention_ttl.ToSeconds() // 3600
    return f"{ttl}h (namespace {NS} workflow execution retention)"


async def submit():
    task = {"agent": AGENT, "id": TASK, "scenario": os.environ["TRACKA_SCENARIO"]}
    await (await client()).start_workflow(
        Agent.run,
        id=WF,
        task_queue=QUEUE,
        task_timeout=timedelta(seconds=2),
        start_signal="submit",
        start_signal_args=[task],
    )


async def worker():
    c = await client()
    identity = f"{os.environ['TRACKA_WORKER']}@{os.getpid()}"
    with ThreadPoolExecutor(8) as pool:
        await Worker(
            c,
            task_queue=QUEUE,
            workflows=[Agent],
            activities=ACTIVITIES,
            activity_executor=pool,
            identity=identity,
            max_concurrent_activities=1,
            sticky_queue_schedule_to_start_timeout=timedelta(seconds=2),
        ).run()


async def deliver(task, action, decision):
    await (await client()).get_workflow_handle(WF).signal(
        Agent.decide, args=[task, action, decision]
    )


async def state(agent):
    c = await client()
    try:
        memo = await (await c.get_workflow_handle(f"agent-{agent}").describe()).memo()
    except RPCError as e:
        if e.status != RPCStatusCode.NOT_FOUND:
            raise
        memo = {}
    task = memo.get("task") or {"id": TASK, "status": "running"}
    out = {
        "task": task,
        "memory": memo.get("memory"),
        "retention": await retention_of(c),
    }
    print(json.dumps(out))


async def histories(c):
    runs, run = [], (await c.get_workflow_handle(WF).describe()).run_id
    while run:
        events = (await c.get_workflow_handle(WF, run_id=run).fetch_history()).events
        runs.insert(0, (run, events))
        run = events[0].workflow_execution_started_event_attributes
        run = run.continued_execution_run_id
    return runs


async def reconstruct(task):
    c = await client()
    dc = c.data_converter
    acts, decision = {}, None
    for run, events in await histories(c):
        for e in events:
            kind = e.WhichOneof("attributes")
            a = getattr(e, kind)
            if kind == "activity_task_scheduled_event_attributes":
                if a.activity_id.startswith(f"{task}:"):
                    acts[(run, e.event_id)] = {
                        "id": a.activity_id.split(":")[1:],
                        "input": await dc.decode(a.input.payloads),
                        "result": None,
                    }
            elif kind == "activity_task_completed_event_attributes":
                if x := acts.get((run, a.scheduled_event_id)):
                    x["result"] = (await dc.decode(a.result.payloads))[0]
            elif kind == "workflow_execution_signaled_event_attributes":
                args = await dc.decode(a.input.payloads)
                if a.signal_name == "decide" and args[0] == task and decision is None:
                    decision = {"decision": args[2]}
    model, policy, calls = [], [], {}
    for x in acts.values():
        (step, *n), r = x["id"], x["result"]
        req = x["input"][1] if len(x["input"]) > 1 else None
        if step == "model":
            model.append(
                {
                    "invocation_id": req["invocation_id"],
                    "request_id": r and r[0]["request_id"],
                    "input": req["body"],
                    "output_sha256": r and sha(r[1]["output"]),
                    "used": r is not None,
                }
            )
        elif step == "policy" and r:
            approval = decision if r["result"] == "needs_approval" else None
            policy.append({**r, "approval": approval})
        elif step.startswith("T-"):
            calls[(step, n[0])] = {
                "tool": step,
                **{k: req[k] for k in ("call_id", "key", "token", "args")},
                "attempts": [
                    {
                        "attempt": req["attempt"],
                        "outcome": r[0]["kind"] if r else "unknown",
                    }
                ],
                "outcome": r and r[0],
            }
        elif step == "owner" and r:
            call = calls[("T-N", n[0])]
            call["attempts"].append(
                {"attempt": int(n[0]), "outcome": f"owner:{r[0]['answer']}"}
            )
            if r[0]["answer"] == "happened":
                call["outcome"] = {"kind": "owner:happened", "result": None}
    out = {"model": model, "policy": policy, "calls": list(calls.values())}
    print(json.dumps({**out, "retention": await retention_of(c)}))


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


if __name__ == "__main__":
    cmd, *args = sys.argv[1:]
    if cmd == "server":
        server()
    else:
        asyncio.run(globals()[cmd](*args))
        sys.stdout.flush()
        os._exit(0)
