import itertools
import json
import os
import urllib.parse
import urllib.request
from datetime import timedelta
from pathlib import Path

from temporalio import activity, workflow
from temporalio.common import RetryPolicy
from temporalio.exceptions import ActivityError

ACTION = "T-I,T-F,T-N"
TIMEOUT = timedelta(seconds=5)
ONCE = RetryPolicy(maximum_attempts=1)


@activity.defn
def send(path: str, body: dict | None = None) -> list:
    data = body and json.dumps(body).encode()
    req = urllib.request.Request(
        os.environ["TRACKA_SVC"] + path, data, {"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return [json.loads(x) for x in r.read().splitlines()]


@activity.defn
def policy(scenario: str) -> dict:
    p = json.loads((Path(__file__).resolve().parents[2] / "policy.json").read_text())
    result = "needs_approval" if p["rules"][scenario]["approval"] else "allow"
    return {"rule_version": p["version"], "action": ACTION, "result": result}


ACTIVITIES = [send, policy]


@workflow.defn
class Agent:
    def __init__(self):
        self.queue, self.decisions = [], {}

    @workflow.signal
    def submit(self, task: dict):
        self.queue.append(task)

    @workflow.signal
    def decide(self, task: str, action: str, decision: str):
        self.decisions[task] = decision

    @workflow.run
    async def run(self, memory: dict | None = None):
        self.memory = memory
        while True:
            await workflow.wait_condition(
                lambda: self.queue or workflow.info().is_continue_as_new_suggested()
            )
            if not self.queue:
                await workflow.wait_condition(workflow.all_handlers_finished)
                workflow.continue_as_new(self.memory)
            await self.task(**self.queue.pop(0))

    def act(self, task, step, *args, fn=send, start=False, **kw):
        return (workflow.start_activity if start else workflow.execute_activity)(
            fn,
            args=args,
            activity_id=f"{task}:{step}",
            start_to_close_timeout=TIMEOUT,
            **kw,
        )

    async def retry(self, task, step, request):
        for n in itertools.count(1):
            try:
                return await self.act(
                    task, f"{step}:{n}", *request(n), retry_policy=ONCE
                )
            except ActivityError:
                pass

    def end(self, task, status, memory=None):
        self.memory = memory or self.memory
        workflow.upsert_memo(
            {"task": {"id": task, "status": status}, "memory": self.memory}
        )

    async def task(self, agent, id, scenario):
        self.end(id, "running")
        wf = workflow.info().workflow_id
        inv = lambda n: f"{wf}:{id}:model:{n}" if n else None
        body = {"agent": agent, "task": id, "prompt": "plan T-I, T-F, T-N"}
        stub = {"agent": agent, "task": id, "body": body}
        head, _ = await self.retry(
            id,
            "model",
            lambda n: (
                "/stub",
                {**stub, "invocation_id": inv(n), "retry_of": inv(n - 1)},
            ),
        )
        rid = head["request_id"]
        p = await self.act(id, "policy", scenario, fn=policy)
        if p["result"] == "needs_approval":
            ask = {"task": id, "action": ACTION}
            asked = self.act(id, "approval", "/approval", ask, start=True)
            await workflow.wait_condition(lambda: id in self.decisions)
            asked.cancel()
            if self.decisions[id] == "deny":
                return self.end(id, "denied", {"reply": rid, "denied": True})
        tool = lambda name, args: lambda n: (
            f"/{name}",
            {
                "call_id": f"{id}:{name}",
                "key": f"{wf}:{id}:{name}",
                "attempt": n,
                "agent": agent,
                "task": id,
                "token": workflow.time_ns(),
                "args": args,
            },
        )
        ti = await self.retry(id, "T-I", tool("T-I", {"reply": rid}))
        ti = ti[0]["result"]["effect"]
        tf = await self.retry(id, "T-F", tool("T-F", {"reply": rid, "ti": ti}))
        tf = tf[0]["result"]["effect"]
        tn = await self.tn(id, tool("T-N", {"reply": rid, "tf": tf}))
        self.end(id, "done", {"reply": rid, "ti": ti, "tf": tf, "tn": tn})

    async def tn(self, id, request):
        q = urllib.parse.urlencode({"task": id, "call_id": f"{id}:T-N"})
        for n in itertools.count(1):
            try:
                r = await self.act(id, f"T-N:{n}", *request(n), retry_policy=ONCE)
                return r[0]["result"]["effect"]
            except ActivityError:
                a = await self.retry(id, f"owner:{n}", lambda _: (f"/owner?{q}",))
                if a[0]["answer"] == "happened":
                    return "owner:happened"
