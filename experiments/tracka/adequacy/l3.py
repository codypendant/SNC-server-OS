import json
import shutil
import sys

from .common import ARMS, BUILD, TRACKA, check, load, log, pmap, read, run, save, trim

MARK = """
import os


def _mark(event, **d):
    import time

    p = os.path.join(os.environ["TRACKA_RUN"], "logs", "mutant.jsonl")
    with open(p, "a") as f:
        f.write(json.dumps({"t": time.monotonic_ns(), "event": event, **d}) + "\\n")
"""
W = 'os.environ["TRACKA_WORKER"]'
MUTANTS = {
    "O-1-model-not-durable": (
        "O-1",
        {
            "app.py": [
                (
                    "@DBOS.step()\ndef model(agent, task):\n",
                    f"def model(agent, task):\n    _mark('model-call', worker={W})\n",
                )
            ]
        },
    ),
    "O-2-second-writer": (
        "O-2",
        {
            "app.py": [
                (
                    "    DBOS.register_queue(QUEUE, partition_concurrency=1)\n",
                    "    DBOS.register_queue(QUEUE, partition_concurrency=1)\n"
                    f"    if {W} != 'w1':\n"
                    "        from dbos import SetWorkflowID\n\n"
                    "        e = os.environ\n"
                    f"        with SetWorkflowID(e['TRACKA_TASK'] + '-' + {W}):\n"
                    "            DBOS.start_workflow(run_task, e['TRACKA_AGENT'],"
                    " e['TRACKA_TASK'], e['TRACKA_SCENARIO'])\n"
                    f"        _mark('second-writer', worker={W})\n",
                )
            ]
        },
    ),
    "O-3-ti-no-key": (
        "O-3",
        {
            "app.py": [
                (
                    '    req = {"call_id": cid, "key": cid,',
                    "    if name == 'T-I':\n"
                    f"        _mark('ti-no-key', worker={W})\n"
                    '    req = {"call_id": cid, "key": None if name == "T-I" else cid,',
                )
            ]
        },
    ),
    "O-4-attempt-not-recorded": (
        "O-4",
        {
            "arm.py": [
                (
                    '                o = child_output(c, step["child_workflow_id"])\n',
                    '                s = c.list_workflow_steps(step["child_workflow_id"])\n'
                    '                if not (s and s[0]["output"]):\n'
                    "                    _mark('attempt-skipped')\n"
                    "                    continue\n"
                    '                o = child_output(c, step["child_workflow_id"])\n',
                )
            ]
        },
    ),
    "O-4-rule-version-not-recorded": (
        "O-4",
        {
            "app.py": [
                (
                    '        "rule_version": POLICY["version"],\n',
                    '        "rule_version": _mark("no-rule-version"),\n',
                )
            ]
        },
    ),
    "O-5-wait-skipped": (
        "O-5",
        {
            "app.py": [
                (
                    '        if DBOS.recv(TOPIC, RECV_TIMEOUT_S) != "approve":\n',
                    f"        _mark('wait-skipped', worker={W})\n        if False:\n",
                )
            ]
        },
    ),
    "O-5-recv-no-wait": (
        "O-5",
        {
            "app.py": [
                (
                    '        if DBOS.recv(TOPIC, RECV_TIMEOUT_S) != "approve":\n',
                    "        got = DBOS.recv(TOPIC, 0)\n"
                    f"        _mark('recv-no-wait', got=got, worker={W})\n"
                    '        if got != "approve":\n',
                )
            ]
        },
    ),
    "O-5-event-dropped": (
        "O-5",
        {
            "app.py": [
                (
                    "RECV_TIMEOUT_S, TN_TIMEOUT_S = 3600, 30",
                    "RECV_TIMEOUT_S, TN_TIMEOUT_S = 20, 30",
                ),
                (
                    "    DBOS.launch()\n",
                    "    with __import__('arm').engine().begin() as c:\n"
                    "        n = c.execute(sa.text('DELETE FROM dbos.notifications'"
                    " ' WHERE destination_uuid = :t'),"
                    " {'t': os.environ['TRACKA_TASK']}).rowcount\n"
                    f"    _mark('event-dropped', rows=n, worker={W})\n"
                    "    DBOS.launch()\n",
                ),
            ]
        },
    ),
    "O-5-event-dropped-hang": (
        "O-5",
        {
            "app.py": [
                (
                    "    DBOS.launch()\n",
                    "    with __import__('arm').engine().begin() as c:\n"
                    "        n = c.execute(sa.text('DELETE FROM dbos.notifications'"
                    " ' WHERE destination_uuid = :t'),"
                    " {'t': os.environ['TRACKA_TASK']}).rowcount\n"
                    f"    _mark('event-dropped', rows=n, worker={W})\n"
                    "    DBOS.launch()\n",
                ),
            ]
        },
    ),
}
SM = [
    ("A01", 0),
    ("A02", 0),
    ("A02", 1),
    ("A03", 0),
    ("A04", 0),
    ("A04", 1),
    ("A05", 0),
    ("A06", 0),
    ("A06", 1),
    ("A07", 0),
    ("A08", 0),
    ("A08", 1),
    ("A11", 0),
    ("A12", 0),
    ("A13", 0),
    ("A14", 0),
    ("A14", 1),
    ("A15", 0),
    ("A15", 1),
    ("A16", 0),
    ("A16", 1),
    ("A17", 0),
    ("A17", 1),
]
AP = [("A09", 0), ("A09", 1), ("A10", 0), ("A10", 1)]
CONTROLS = (
    [("none", 0, "S-M")]
    + [(p, s, "S-M") for p, s in SM]
    + [(p, s, "S-AP") for p, s in AP]
)
CELLS = {
    "O-1-model-not-durable": [
        "none",
        "A01",
        "A02",
        "A03",
        "A04",
        "A06",
        "A07",
        "A11",
        "A15",
        "A16",
    ],
    "O-2-second-writer": ["none", "A01", "A02", "A05", "A07", "A14", "A15", "A17"],
    "O-3-ti-no-key": ["none", "A02", "A06", "A11", "A12", "A15"],
    "O-4-attempt-not-recorded": ["none", "A04", "A08", "A13", "A17"],
    "O-4-rule-version-not-recorded": ["none", "A02", "A09", "A10"],
    "O-5-wait-skipped": ["A09", "A10"],
    "O-5-recv-no-wait": ["A09", "A10"],
    "O-5-event-dropped": ["A09", "A10"],
    "O-5-event-dropped-hang": [("A09", 0)],
}


def build(arm, mid):
    src = ARMS[arm]
    d = BUILD / "arms" / f"l3-{arm}-{mid}"
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(src, d, ignore=shutil.ignore_patterns(".venv", "__pycache__"))
    if (src / ".venv").exists():
        (d / ".venv").symlink_to(src / ".venv")
    shutil.copy(TRACKA / "policy.json", BUILD / "policy.json")
    for f, edits in MUTANTS[mid][1].items():
        text = (d / f).read_text()
        for old, new in edits:
            assert text.count(old) == 1, (mid, f, old)
            text = text.replace(old, new)
        imports = text.index("\n\n\n")
        (d / f).write_text(text[:imports] + MARK + text[imports:])
    return d


def cells(mid):
    out = []
    for c in CELLS[mid]:
        if isinstance(c, tuple):
            out += [x for x in CONTROLS if x[:2] == c]
        else:
            out += [x for x in CONTROLS if x[0] == c]
    return out


def tail(cell):
    p, s, sc = cell
    return f"{p}-{sc}-{s}" if p == "none" else f"{p}-{s}"


def execute(arm_dir, name, cell, keep=None):
    out = run(arm_dir, name, *cell)
    R = out.get("R")
    res = check(R) if R else {"verdict": "driver-error", "fails": [], "unresolved": []}
    res.update(run=out.get("status"), why=out.get("why"), notes=out.get("notes"))
    if R:
        res["marks"] = read(f"{R}/logs/mutant.jsonl")
        res["svc"] = {
            s: read(f"{R}/svc/{s}.jsonl")
            for s in ("stub", "T-I", "T-F", "T-N", "approver")
        }
        res["answers"] = read(f"{R}/answers.jsonl")
        if keep:
            trim(R, keep)
        shutil.rmtree(R, ignore_errors=True)
    return res


def controls(arm):
    from .common import BASE

    done = load(f"controls-{arm}") or {}

    def one(cell):
        k = tail(cell)
        if k in done and done[k]["run"] in ("ended", "cap"):
            return k, done[k]
        res = execute(ARMS[arm], arm, cell, keep=BASE / f"{arm}-{k}")
        log("control", arm, k, res["verdict"], res["fails"], res["unresolved"])
        return k, res

    ref = [("none", 0, "S-M"), ("A04", 0, "S-M"), ("A07", 0, "S-M"), ("A15", 0, "S-M")]
    ref += [("A09", 0, "S-AP"), ("A09", 1, "S-AP"), ("A10", 0, "S-AP")]
    for k, res in pmap(one, ref if arm == "ref" else CONTROLS):
        done[k] = res
        save(f"controls-{arm}", done)


def reqs(res, svc):
    return [x for x in res["svc"][svc] if x["kind"] == "request"]


def evidence(mid, res, cell):
    m = [x["event"] for x in res.get("marks", [])]
    tools = sorted(
        reqs(res, "T-I") + reqs(res, "T-F") + reqs(res, "T-N"), key=lambda x: x["t"]
    )
    used = [x for x in tools if x["args"].get("reply")]
    stub = reqs(res, "stub")
    deny = cell[2] == "S-AP" and cell[1] % 2
    if mid.startswith("O-1"):
        late = [s for s in stub if used and s["t"] > used[0]["t"]]
        return (
            bool(late),
            f"model-call marks {m.count('model-call')}, stub requests after first use {len(late)}",
        )
    if mid.startswith("O-2"):
        replies = {x["args"].get("reply") for x in tools}
        return (
            m.count("second-writer") > 0 and len(replies) > 1,
            f"second-writer marks {m.count('second-writer')}, distinct replies in tool args {len(replies)}",
        )
    if mid.startswith("O-3"):
        eff = [x for x in res["svc"]["T-I"] if x["kind"] == "effect"]
        return (
            len(eff) > 1,
            f"ti-no-key marks {m.count('ti-no-key')}, T-I effect lines {len(eff)}",
        )
    if mid == "O-4-attempt-not-recorded":
        return (
            "attempt-skipped" in m,
            f"attempt-skipped marks {m.count('attempt-skipped')}, T-N requests {len(reqs(res, 'T-N'))}",
        )
    if mid == "O-4-rule-version-not-recorded":
        return (
            "no-rule-version" in m,
            f"no-rule-version marks {m.count('no-rule-version')}",
        )
    posted = [x["t"] for x in res["svc"]["approver"] if x["kind"] == "posted"]
    if mid == "O-5-wait-skipped":
        early = [x for x in tools if not posted or x["t"] < posted[0]]
        ok = "wait-skipped" in m and (bool(tools) if deny else True)
        return (
            ok,
            f"wait-skipped marks {m.count('wait-skipped')}, tool requests {len(tools)} ({len(early)} before posted)",
        )
    if mid == "O-5-recv-no-wait":
        got = [
            x.get("got") for x in res.get("marks", []) if x["event"] == "recv-no-wait"
        ]
        return any(g is None for g in got), f"recv(timeout 0) returned {got}"
    if mid.startswith("O-5-event-dropped"):
        rows = sum(
            x.get("rows") or 0
            for x in res.get("marks", [])
            if x["event"] == "event-dropped"
        )
        return rows > 0, f"notifications deleted at worker start: {rows}"


def judge(mid, g, res, ctl, cell):
    ran, ev = evidence(mid, res, cell)
    gc = [c for c in res["fails"] if c.startswith(g + ".")]
    cc = [c for c in ctl["fails"] if c.startswith(g + ".")]
    new = [c for c in gc if c not in cc]
    if ctl["guarantees"].get(g) == "fail":
        st = "no-credit (step-1 failure)"
    elif ctl["guarantees"].get(g) != "pass":
        st = f"no-credit (control {ctl['guarantees'].get(g)})"
    elif not ran:
        st = "unresolved (not exercised)"
    elif new:
        st = "flagged"
    elif res["run"] == "cap" or any(c.startswith(g + ".") for c in res["unresolved"]):
        st = "unresolved (cap)" if res["run"] == "cap" else "unresolved"
    else:
        st = "miss"
    return {"status": st, "evidence": ev, "new": new, "control_fails": ctl["fails"]}


def mutants(arm, only=()):
    ctls = load(f"controls-{arm}")
    done = load(f"l3-{arm}") or {}
    jobs = [(mid, c) for mid in MUTANTS if not only or mid in only for c in cells(mid)]
    dirs = {mid: build(arm, mid) for mid in {j[0] for j in jobs}}

    def one(job):
        mid, cell = job
        k = f"{mid}|{tail(cell)}"
        if k in done and done[k]["run"] in ("ended", "cap"):
            res = done[k]
        else:
            res = execute(dirs[mid], f"l3-{arm}-{mid}", cell)
        res.update(judge(mid, MUTANTS[mid][0], res, ctls[tail(cell)], cell))
        res.update(mutant=mid, guarantee=MUTANTS[mid][0], cell=tail(cell))
        log("mutant", k, res["status"], res["fails"], res["evidence"])
        return k, res

    for k, res in pmap(one, jobs):
        done[k] = res
        save(f"l3-{arm}", done)


if __name__ == "__main__":
    {"controls": controls, "mutants": mutants}[sys.argv[1]](sys.argv[2], *sys.argv[3:])
