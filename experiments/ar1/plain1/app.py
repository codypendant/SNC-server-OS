import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
from contextlib import contextmanager
from pathlib import Path

import sqlalchemy as sa
from dbos import DBOS, DBOSConfig, SetWorkflowID, SQLAlchemyDatasource

from ar1 import fault, jlog

from . import rt

MODE, EPOCH = sys.argv[1], int(sys.argv[2])
WID = f"T:e{EPOCH}"
APP = {"connect_args": {"application_name": f"ar1-{rt.EX}-dbos"}}
DBOS(
    config=DBOSConfig(
        name="ar1",
        system_database_url=rt.URL + f"ar1_sys_{rt.EX}",
        executor_id=rt.EX,
        db_engine_kwargs=APP,
        log_level="WARNING",
    )
)
ds = SQLAlchemyDatasource.create(rt.URL + "ar1", engine_kwargs=APP, schema="plain_ds")
POLICY = json.loads((Path(__file__).parent.parent / "policy.json").read_text())


def q(sql, **p):
    return ds.sql_session().execute(sa.text(sql), p)


def commit(name, kind, ref):
    q(
        "INSERT INTO plain.commits VALUES (:t, pg_current_xact_id(), :e, :k, :n, 'A', :r)",
        t=f"{WID}:{name}",
        e=EPOCH,
        k=kind,
        n=name,
        r=ref,
    )
    fault.at(f"a1:{name}")
    fault.at(f"a3:{name}")


@contextmanager
def decision(name):
    stale = fault.stale_begin(EPOCH, name)
    try:
        yield
    except Exception as e:
        if stale:
            fault.stale_end("rejected", repr(e))
        raise
    if stale:
        fault.stale_end("committed")


def after(name):
    fault.at(f"a2:{name}")
    fault.at(f"a4:{name}")


def send(path, body=None):
    data = json.dumps(body).encode() if body else None
    req = urllib.request.Request(
        f"http://127.0.0.1:{rt.RUN['svc_port']}{path}",
        data,
        {"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req) as r:
            return [json.loads(line) for line in r]
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"{e.code} {e.read().decode()}")


@ds.transaction()
def accept():
    q("UPDATE plain.agent SET epoch=:e, version=version+1 WHERE id='A'", e=EPOCH)
    q("UPDATE plain.task SET status='accepted', version=version+1 WHERE id='T'")
    commit(
        "C-acc" if EPOCH == 1 else "TAKEOVER",
        "decision" if EPOCH == 1 else "takeover",
        "T",
    )


@ds.transaction()
def prepare_model():
    goal = q("SELECT goal FROM plain.task WHERE id='T'").scalar()
    rows = q("SELECT id, version, content_hash FROM plain.memory WHERE id IN ('m1','m2','m3') ORDER BY id")
    items = [{"kind": "memory", "id": i, "version": v, "sha256": h} for i, v, h in rows]
    body = {"agent": "A", "task": "T", "goal": goal, "items": items}
    whole = hashlib.sha256(jlog.canonical(body).encode()).hexdigest()
    q(
        "INSERT INTO plain.invocation VALUES (:i, 'A', 'T', :e, 'prepared', :m, :w, NULL)",
        i=f"{WID}:I",
        e=EPOCH,
        m=json.dumps(items),
        w=whole,
    )
    commit("I-prep", "decision", f"{WID}:I")
    return body


@DBOS.step()
def call_model(body):
    fault.at("b1:model")
    req = {
        "invocation_id": f"{WID}:I",
        "retry_of": None,
        "agent": "A",
        "epoch": EPOCH,
        "body": body,
    }
    out = send("/stub", req)[-1]["output"]
    fault.at("b3:model")
    return out


@ds.transaction()
def record_model(out):
    q(
        "UPDATE plain.invocation SET state='output', output=:o WHERE id=:i",
        o=out,
        i=f"{WID}:I",
    )
    commit("I-out", "outcome", f"{WID}:I")


@ds.transaction()
def decide():
    approval = "required" if POLICY["rules"][rt.RUN["scenario"]]["approval"] else None
    q(
        "INSERT INTO plain.decision VALUES (:i, 'T', :c, :v, :a, :e)",
        i=f"{WID}:P",
        c=f"{WID}:T-I",
        v=POLICY["version"],
        a=approval,
        e=EPOCH,
    )
    commit("P-dec", "decision", f"{WID}:T-I")


@ds.transaction()
def approval_requested():
    commit("AP-req", "decision", f"{WID}:T-I")


@DBOS.step()
def ask_approver():
    return send(f"/approve?task=T&action={WID}:T-I")[-1]["answer"]


@ds.transaction()
def approval_decided(answer):
    q("UPDATE plain.decision SET approval=:a WHERE id=:i", a=answer, i=f"{WID}:P")
    if answer == "deny":
        q("UPDATE plain.task SET status='denied', version=version+1 WHERE id='T'")
    commit("AP-dec", "outcome", f"{WID}:T-I")


@DBOS.step()
def call_tool(tool):
    fault.at(f"b1:{tool}")
    args = {
        "call_id": f"{WID}:{tool}",
        "key": f"{WID}:{tool}",
        "attempt": 1,
        "agent": "A",
        "epoch": EPOCH,
        "args": {},
    }
    res = send(f"/{tool}", args)[-1]["result"]
    fault.at(f"b3:{tool}")
    return res


@ds.transaction()
def record_call(tool, result):
    cid = f"{WID}:{tool}"
    q(
        "INSERT INTO plain.call VALUES (:i, 'A', 'T', :t, :i, :e, 'completed', 1, :r)",
        i=cid,
        t=tool,
        e=EPOCH,
        r=json.dumps(result),
    )
    commit(tool.replace("-", "") + "-out", "outcome", cid)


@ds.transaction()
def finish():
    h = rt.blob(rt.R / "blobs", f"result of {WID}".encode())
    q(
        "INSERT INTO plain.memory VALUES ('m4', 1, 'A', :h) ON CONFLICT (id) DO UPDATE SET"
        " version=plain.memory.version+1, content_hash=:h",
        h=h,
    )
    q("UPDATE plain.task SET status='completed', version=version+1 WHERE id='T'")
    commit("M-done", "outcome", "m4")


@DBOS.workflow()
def task():
    accept()
    fault.at("post-takeover")
    after("C-acc" if EPOCH == 1 else "TAKEOVER")
    body = prepare_model()
    after("I-prep")
    record_model(call_model(body))
    after("I-out")
    with decision("P-dec"):
        decide()
    after("P-dec")
    if rt.RUN["scenario"] == "S-AP":
        approval_requested()
        after("AP-req")
        answer = ask_approver()
        approval_decided(answer)
        after("AP-dec")
        if answer == "deny":
            return "denied"
    for tool in ("T-I", "T-F", "T-N"):
        with decision(tool.replace("-", "") + "-prep"):
            record_call(tool, call_tool(tool))
        after(tool.replace("-", "") + "-out")
    with decision("M-done"):
        finish()
    after("M-done")
    return "completed"


def main():
    DBOS.launch()
    ctl = rt.R / "ctl"
    if MODE == "takeover":
        (ctl / f"ready-{rt.EX}").write_text("1")
        while not (ctl / "go-takeover").exists():
            time.sleep(0.005)
    try:
        if MODE == "recover":
            handle = DBOS.retrieve_workflow(WID)
        else:
            with SetWorkflowID(WID):
                handle = DBOS.start_workflow(task)
        out = handle.get_result()
    except Exception as e:
        out = f"error: {e!r}"
    (ctl / f"result-{rt.EX}").write_text(str(out))
    DBOS.destroy()
    os._exit(0)


if __name__ == "__main__":
    main()
