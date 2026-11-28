from .common import TRACKA

PY_MARK = """
import os


def _mark(event, **d):
    import time

    p = os.path.join(os.environ["TRACKA_RUN"], "logs", "mutant.jsonl")
    with open(p, "a") as f:
        f.write(json.dumps({"t": time.monotonic_ns(), "event": event, **d}) + "\\n")
"""
RS_MARK = """
fn mark(event: &str) {
    use std::io::Write;
    let path = format!("{}/logs/mutant.jsonl", var("TRACKA_RUN").unwrap_or_default());
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "{}", json!({"t": 0, "event": event}));
    }
}
"""
TW = "    with workflow.unsafe.sandbox_unrestricted():\n"
RS = "src/main.rs"

RESTATE = {
    "O-1-model-not-durable": (
        "O-1",
        {
            RS: [
                (
                    "        let model = ctx\n            .run(|| async move {\n",
                    "        let model = (|| async move {\n"
                    '                mark("model-call");\n',
                ),
                (
                    "                Ok(Json(Value::Object(reply)))\n            })\n"
                    '            .name("model")\n            .await?\n',
                    "                Res::Ok(Json(Value::Object(reply)))\n            })()\n"
                    "            .await?\n",
                ),
            ]
        },
    ),
    "O-2-second-writer": (
        "O-2",
        {
            RS: [
                (
                    "            server.await?;\n",
                    "            if n > 1 {\n"
                    '                let task = var("TRACKA_TASK")?;\n'
                    '                let req = json!({"task": task, "scenario": var("TRACKA_SCENARIO")?});\n'
                    "                let client = ReqwestClient::connect(addr(0).parse()?)?;\n"
                    '                AgentIngressClient::from_client(client, var("TRACKA_AGENT")?)\n'
                    "                    .run(Json(req))\n"
                    '                    .idempotency_key(format!("{task}-w{n}"))\n'
                    "                    .send()\n"
                    "                    .await?;\n"
                    '                mark("second-writer");\n'
                    "            }\n"
                    "            server.await?;\n",
                )
            ]
        },
    ),
    "O-3-ti-no-key": (
        "O-3",
        {
            RS: [
                (
                    '    json!({"call_id": key, "key": key,',
                    '    let key2 = if tool == "T-I" { Value::Null } else { json!(key) };\n'
                    '    json!({"call_id": key, "key": key2,',
                )
            ]
        },
    ),
    "O-4-attempt-not-recorded": (
        "O-4",
        {
            RS: [
                (
                    "        let mut attempts: Vec<Value> = (1..n)\n",
                    "        if n > 1 {\n"
                    '            mark("attempt-skipped");\n'
                    "        }\n"
                    "        let mut attempts: Vec<Value> = (n..n)\n",
                )
            ]
        },
    ),
    "O-4-rule-version-not-recorded": (
        "O-4",
        {
            RS: [
                (
                    '                    json!({"rule_version": p["version"], "action"',
                    '                    json!({"rule_version": ({ mark("no-rule-version"); Value::Null }), "action"',
                )
            ]
        },
    ),
    "O-5-wait-skipped": (
        "O-5",
        {
            RS: [
                (
                    '            if decision.await? == "deny" {\n',
                    '            mark("wait-skipped");\n'
                    "            drop(decision);\n"
                    "            if false {\n",
                )
            ]
        },
    ),
}

TEMPORAL = {
    "O-1-model-not-durable": (
        "O-1",
        {
            "agent.py": [
                (
                    '        head, _ = await self.retry(\n            id,\n            "model",\n'
                    "            lambda n: (\n"
                    '                "/stub",\n'
                    '                {**stub, "invocation_id": inv(n), "retry_of": inv(n - 1)},\n'
                    "            ),\n        )\n",
                    "    "
                    + TW
                    + '            head = send("/stub", {**stub, "invocation_id": inv(1), "retry_of": None})[0]\n'
                    '            _mark("model-call")\n',
                )
            ]
        },
    ),
    "O-2-second-writer": (
        "O-2",
        {
            "arm.py": [
                (
                    "    with ThreadPoolExecutor(8) as pool:\n",
                    "    if os.environ['TRACKA_WORKER'] != 'w1':\n"
                    '        task = {"agent": AGENT, "id": TASK, "scenario": os.environ["TRACKA_SCENARIO"]}\n'
                    "        await c.start_workflow(Agent.run, id=WF + '-' + os.environ['TRACKA_WORKER'],"
                    " task_queue=QUEUE, task_timeout=timedelta(seconds=2),"
                    ' start_signal="submit", start_signal_args=[task])\n'
                    '        _mark("second-writer")\n'
                    "    with ThreadPoolExecutor(8) as pool:\n",
                )
            ]
        },
    ),
    "O-3-ti-no-key": (
        "O-3",
        {
            "agent.py": [
                (
                    '                "key": f"{wf}:{id}:{name}",\n',
                    '                "key": None if name == "T-I" else f"{wf}:{id}:{name}",\n',
                )
            ]
        },
    ),
    "O-4-attempt-not-recorded": (
        "O-4",
        {
            "arm.py": [
                (
                    '        elif step.startswith("T-"):\n',
                    '        elif step.startswith("T-") and r is None:\n'
                    '            _mark("attempt-skipped")\n'
                    '        elif step.startswith("T-"):\n',
                ),
                (
                    '        elif step == "owner" and r:\n',
                    '        elif step == "owner" and r and ("T-N", n[0]) in calls:\n',
                ),
            ]
        },
    ),
    "O-4-rule-version-not-recorded": (
        "O-4",
        {
            "agent.py": [
                (
                    '    return {"rule_version": p["version"],',
                    '    return {"rule_version": _mark("no-rule-version"),',
                )
            ]
        },
    ),
    "O-5-wait-skipped": (
        "O-5",
        {
            "agent.py": [
                (
                    "            await workflow.wait_condition(lambda: id in self.decisions)\n"
                    "            asked.cancel()\n"
                    '            if self.decisions[id] == "deny":\n',
                    "        " + TW + '                _mark("wait-skipped")\n'
                    "            if False:\n",
                )
            ]
        },
    ),
    "O-5-event-dropped": (
        "O-5",
        {
            "agent.py": [
                (
                    "        self.decisions[task] = decision\n",
                    '        if getattr(self, "waiting", False):\n'
                    "            self.decisions[task] = decision\n"
                    "        else:\n"
                    "        " + TW + '                _mark("event-dropped")\n',
                ),
                (
                    "            await workflow.wait_condition(lambda: id in self.decisions)\n",
                    "            self.waiting = True\n"
                    "            try:\n"
                    "                await workflow.wait_condition(\n"
                    "                    lambda: id in self.decisions, timeout=timedelta(seconds=20)\n"
                    "                )\n"
                    "            except TimeoutError:\n"
                    '                self.decisions[id] = "deny"\n',
                ),
            ]
        },
    ),
}

LANGGRAPH = {
    "O-1-model-not-durable": (
        "O-1",
        {
            "lg.py": [
                (
                    "                None if snap.created_at else new,\n",
                    '                _mark("restart-from-scratch") or new,\n',
                )
            ]
        },
    ),
    "O-2-second-writer": (
        "O-2",
        {
            "lg.py": [
                (
                    "            snap = graph.get_state(cfg)\n",
                    "            if env['TRACKA_WORKER'] != 'w1':\n"
                    "                other = thread(env['TRACKA_TASK'] + '-' + env['TRACKA_WORKER'])\n"
                    "                threading.Thread(target=run, args=(graph, other, new), daemon=True).start()\n"
                    '                _mark("second-writer")\n'
                    "            snap = graph.get_state(cfg)\n",
                )
            ]
        },
    ),
    "O-3-ti-no-key": (
        "O-3",
        {
            "lg.py": [
                (
                    '            "key": call_id,\n',
                    '            "key": None if name == "T-I" else call_id,\n',
                )
            ]
        },
    ),
    "O-4-attempt-not-recorded": (
        "O-4",
        {
            "lg.py": [
                (
                    '        "calls": [c for f in ("ti", "tf", "tn") for c in distinct(snaps, f)]\n',
                    '        "calls": [_known(c) for f in ("ti", "tf", "tn") for c in distinct(snaps, f)]\n',
                ),
                (
                    "def thread(task):\n",
                    "def _known(c):\n"
                    '    keep = [a for a in c["attempts"] if a["outcome"] != "unknown"]\n'
                    '    if len(keep) < len(c["attempts"]):\n'
                    '        _mark("attempt-skipped")\n'
                    '    return {**c, "attempts": keep}\n\n\n'
                    "def thread(task):\n",
                ),
            ]
        },
    ),
    "O-4-rule-version-not-recorded": (
        "O-4",
        {
            "lg.py": [
                (
                    '            "rule_version": POLICY["version"],\n',
                    '            "rule_version": _mark("no-rule-version"),\n',
                )
            ]
        },
    ),
    "O-5-wait-skipped": (
        "O-5",
        {
            "lg.py": [
                (
                    '    return {"decision": interrupt({"task": s["task"], "action": ACTION})}\n',
                    '    _mark("wait-skipped")\n    return {"decision": "approve"}\n',
                )
            ]
        },
    ),
}

AR1_LINE = 'AR1 = Path(__file__).resolve().parents[3] / "ar1"\n'
AR1_FIX = (AR1_LINE, f'AR1 = Path("{TRACKA.parent / "ar1"}")\n')
SNC = {
    "O-4-attempt-not-recorded": (
        "O-4",
        {
            "snc.py": [
                AR1_FIX,
                (
                    '        e["attempts"].append({"attempt": a["n"], "outcome": kind})\n',
                    '        if kind == "unknown":\n'
                    '            _mark("attempt-skipped")\n'
                    "        else:\n"
                    '            e["attempts"].append({"attempt": a["n"], "outcome": kind})\n',
                ),
            ]
        },
    ),
    "O-4-rule-version-not-recorded": (
        "O-4",
        {
            "snc.py": [
                AR1_FIX,
                (
                    '                "rule_version": auth and auth["rule_version"],\n',
                    '                "rule_version": _mark("no-rule-version"),\n',
                ),
            ]
        },
    ),
}

SM = ["none", "A01", "A02", "A03", "A04", "A05", "A06", "A07", "A08"]
SM += ["A11", "A12", "A13", "A14", "A15", "A16", "A17"]
COMMON = {
    "O-1-model-not-durable": ["none", ("A02", 0), "A04", "A06", "A11", "A15"],
    "O-2-second-writer": ["none", ("A02", 0), ("A05", 0), "A15"],
    "O-3-ti-no-key": ["none", "A02", "A06", ("A11", 0), "A15"],
    "O-4-attempt-not-recorded": ["none", "A02", "A04", "A15"],
    "O-4-rule-version-not-recorded": ["none", "A09", "A10"],
    "O-5-wait-skipped": ["A09", "A10"],
    "O-5-event-dropped": ["A09", "A10"],
}
SPECS = {
    "restate": RESTATE,
    "temporal": TEMPORAL,
    "langgraph": LANGGRAPH,
    "snc": SNC,
}
SNC_CELLS = {
    "O-4-attempt-not-recorded": ["none", "A02", "A04", ("A06", 0), ("A11", 0)],
    "O-4-rule-version-not-recorded": ["none", ("A02", 0), ("A04", 0)],
}
