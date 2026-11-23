import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid
from contextlib import contextmanager
from pathlib import Path

import sqlalchemy as sa
from dbos import DBOS, DBOSConfig, SetWorkflowID, SQLAlchemyDatasource

from ar1 import fault, jlog

from . import rt

MODE, EPOCH = sys.argv[1], int(sys.argv[2])
WID = f"T:e{EPOCH}"
BOOT = uuid.uuid4().hex
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
tx = ds.transaction(isolation_level="READ COMMITTED")
POLICY = json.loads((Path(__file__).parent.parent / "policy.json").read_text())


def q(sql, **p):
    return ds.sql_session().execute(sa.text(sql), p)


def lock(kind):
    e, status = q(
        "SELECT epoch, (SELECT status FROM plain.task WHERE id='T') FROM plain.agent WHERE id='A' FOR SHARE"
    ).one()
    if kind != "outcome" and (e != EPOCH or status == "cancelled"):
        raise RuntimeError(f"rejected: activation epoch {EPOCH}, agent epoch {e}, task {status}")


def commit(name, kind, ref, state, data=None):
    q(rt.COMMIT, e=EPOCH, k=kind, n=name, r=ref, s=state, d=json.dumps(data))
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
    rt.wait()
    fault.at(f"a4:{name}")


def send(path, body=None):
    data = json.dumps(body).encode() if body else None
    req = urllib.request.Request(
        f"http://127.0.0.1:{rt.RUN['svc_port']}{path}", data, {"Content-Type": "application/json"}
    )
    try:
        return urllib.request.urlopen(req)
    except urllib.error.HTTPError as e:
        return e


@DBOS.step()
def get(path):
    return json.loads(send(path).read())


@tx
def read(sql, **p):
    r = q(sql, **p).first()
    return r and dict(r._mapping)


@tx
def move(name, table, x, to):
    kind = "decision" if to == "dispatched" else "recovery"
    lock(kind)
    sql = (
        f"UPDATE plain.{table} SET state=:to, attempts=attempts+:d WHERE id=:i AND state=:s AND attempts=:n RETURNING 1"
    )
    if q(sql, to=to, d=int(to == "dispatched"), i=x["id"], s=x["state"], n=x["attempts"]).first():
        commit(name, kind, x["id"], to)
        return BOOT


@tx
def accept():
    e = q("SELECT epoch FROM plain.agent WHERE id='A' FOR UPDATE").scalar()
    if e >= EPOCH:
        raise RuntimeError(f"epoch {EPOCH} is not above {e}")
    q("UPDATE plain.agent SET epoch=:e, version=version+1 WHERE id='A'", e=EPOCH)
    status = q(
        "UPDATE plain.task SET status=CASE status WHEN 'requested' THEN 'accepted' ELSE status END,"
        " version=version+1 WHERE id='T' RETURNING status"
    ).scalar()
    data = q(rt.SUPERSEDE, e=EPOCH).scalar()
    commit("C-acc" if EPOCH == 1 else "TAKEOVER", "decision" if EPOCH == 1 else "takeover", "A", "accepted", data)
    return status


@tx
def prepare_model(prev):
    lock("decision")
    goal = q("SELECT goal FROM plain.task WHERE id='T'").scalar()
    rows = q("SELECT id, version, content_hash FROM plain.memory WHERE id IN ('m1','m2','m3') ORDER BY id")
    items = [{"kind": "memory", "id": i, "version": v, "sha256": h} for i, v, h in rows]
    body = {"agent": "A", "task": "T", "goal": goal, "items": items}
    whole = hashlib.sha256(jlog.canonical(body).encode()).hexdigest()
    n = prev["n"] + 1 if prev else 1
    retry_of = prev["id"] if prev and prev["state"] == "result_unknown" else None
    sql = "INSERT INTO plain.invocation VALUES (:i, :n, :e, 'prepared', 0, :b, :w, NULL, NULL, false, :r)"
    q(sql, i=f"{WID}:I{n}", n=n, e=EPOCH, b=json.dumps(body), w=whole, r=retry_of)
    commit("I-prep", "decision", f"{WID}:I{n}", "prepared")


@tx
def accepted(i, rid):
    lock("outcome")
    sql = "UPDATE plain.invocation SET request_id=:r, state=CASE state WHEN 'dispatched' THEN 'accepted' ELSE state END"
    q(sql + " WHERE id=:i", r=rid, i=i)
    commit("I-acc", "outcome", i, "accepted")


# A dispatch replayed from its checkpoint returns the BOOT of the process that committed it, so a re-executed send
# step can tell that its attempt may already have gone out.
@DBOS.step()
def call_model(i, by):
    if by != BOOT:
        return None
    fault.at("b1:model")
    req = {"invocation_id": i["id"], "retry_of": i["retry_of"], "agent": "A", "epoch": EPOCH, "body": i["body"]}
    with send("/stub", req) as r:
        rid = json.loads(r.readline())["request_id"]
        accepted(i["id"], rid)
        after("I-acc")
        out = json.loads(r.readline())["output"]
    fault.at("b3:model")
    return rid, out


@tx
def record_model(i, rid, out):
    lock("outcome")
    sql = "UPDATE plain.invocation SET state='output', output=:o, request_id=:r, late=(state='result_unknown')"
    q(sql + " WHERE id=:i", o=out, r=rid, i=i)
    commit("I-out", "outcome", i, "output")


def run_model():
    while True:
        i = read("SELECT * FROM plain.invocation ORDER BY n DESC LIMIT 1")
        s = i and i["state"]
        if s == "output":
            return i["id"]
        if s in ("dispatched", "accepted"):
            DBOS.sleep(4)
            move("I-unknown", "invocation", i, "result_unknown")
        elif s == "prepared":
            by = move("I-disp", "invocation", i, "dispatched")
            if by:
                after("I-disp")
                r = call_model(i, by)
                if r:
                    record_model(i["id"], *r)
                    after("I-out")
                    return i["id"]
        else:
            prepare_model(i)
            after("I-prep")


@tx
def decide(inv):
    fault.at("pre-policy")
    inputs = q("SELECT id, version, content_hash FROM plain.memory ORDER BY id").all()
    rt.wait()  # before the lock: its WAL record would sit unflushed in the bound until this transaction commits
    lock("decision")
    rule = POLICY["rules"][rt.RUN["scenario"]]
    res, h = "allow" if rule["allow"] else "deny", hashlib.sha256(repr(inputs).encode()).hexdigest()
    a = "required" if rule["approval"] else None
    sql = "INSERT INTO plain.decision VALUES (:i, 'T', :inv, :v, :h, :res, :a, :e)"
    q(sql, i=f"{WID}:P", inv=inv, v=POLICY["version"], h=h, res=res, a=a, e=EPOCH)
    commit("P-dec", "decision", f"{WID}:P", res)
    return f"{WID}:P" if rule["allow"] else None


@tx
def approval(pol, answer, name):
    lock("decision")
    q("UPDATE plain.decision SET approval=:a WHERE id=:i", a=answer, i=pol)
    if answer == "deny":
        q("UPDATE plain.task SET status='denied', version=version+1 WHERE id='T'")
    commit(name, "decision", pol, answer)


@tx
def prepare(tool, pol):
    lock("decision")
    cid = f"{WID}:{tool}"
    q(
        "INSERT INTO plain.call VALUES (:i, 'A', 'T', :t, :i, :e, 'prepared', 0, NULL, :p)",
        i=cid,
        t=tool,
        e=EPOCH,
        p=pol,
    )
    commit(tool.replace("-", "") + "-prep", "decision", cid, "prepared")


@DBOS.step()
def call_tool(c, by):
    if by != BOOT:
        return None
    fault.at(f"b1:{c['tool']}")
    args = {"call_id": c["id"], "key": c["key"], "attempt": c["attempts"] + 1, "agent": "A", "epoch": EPOCH, "args": {}}
    res = json.loads(send(f"/{c['tool']}", args).read())
    fault.at(f"b3:{c['tool']}")
    return res


@tx
def outcome(c, n, res):
    lock("outcome")
    state = "rejected" if res["kind"] == "rejected_stale" else "completed"
    sql = "UPDATE plain.call SET state=:s, result=CAST(:r AS jsonb) WHERE id=:i AND attempts=:n AND state NOT IN"
    ok = q(sql + " ('completed', 'rejected') RETURNING 1", s=state, r=json.dumps(res["result"]), i=c["id"], n=n).first()
    commit(c["tool"].replace("-", "") + "-out", "outcome", c["id"], state if ok or state == "rejected" else "duplicate")


def run_tool(tool, pol):
    p = tool.replace("-", "")
    while True:
        c = read("SELECT * FROM plain.call WHERE tool=:t ORDER BY epoch DESC LIMIT 1", t=tool)
        s = c and c["state"]
        if s == "completed":
            return
        if s in (None, "rejected"):
            with decision(f"{p}-prep"):
                prepare(tool, pol)
            after(f"{p}-prep")
            continue
        if s == "dispatched" and tool == "T-N":
            DBOS.sleep(4)
            move("TN-indoubt", "call", c, "in_doubt")
            continue
        if s == "dispatched" and tool == "T-I":
            st = get(f"/status?key={c['key']}")
            if st["found"]:
                outcome(c, c["attempts"], {"kind": "status", "result": st["result"]})
                continue
        if s == "in_doubt":
            DBOS.sleep(10)
            if get(f"/owner?task=T&call_id={c['id']}")["answer"] == "happened":
                outcome(c, c["attempts"], {"kind": "owner", "result": "happened"})
                continue
        by = move(f"{p}-disp", "call", c, "dispatched")
        if by:
            after(f"{p}-disp")
            res = call_tool(c, by)
            if res:
                outcome(c, c["attempts"] + 1, res)
                after(f"{p}-out")
                return


@tx
def finish():
    lock("decision")
    h = rt.blob(rt.R / "blobs", f"result of {WID}".encode())
    q(
        "INSERT INTO plain.memory VALUES ('m4', 1, 'A', :h) ON CONFLICT (id) DO UPDATE SET"
        " version=plain.memory.version+1, content_hash=:h",
        h=h,
    )
    q("UPDATE plain.task SET status='completed', version=version+1 WHERE id='T'")
    commit("M-done", "decision", "m4", "written")


@DBOS.workflow()
def task():
    status = accept()
    after("C-acc" if EPOCH == 1 else "TAKEOVER")
    fault.at("post-takeover")
    if status != "accepted":
        return status
    inv = run_model()
    with decision("P-dec"):
        pol = decide(inv)
    after("P-dec")
    if not pol:
        return "denied"
    if rt.RUN["scenario"] == "S-AP":
        approval(pol, "requested", "AP-req")
        after("AP-req")
        answer = get(f"/approve?task=T&action={pol}")["answer"]
        approval(pol, answer, "AP-dec")
        after("AP-dec")
        if answer == "deny":
            return "denied"
    for tool in ("T-I", "T-F", "T-N"):
        run_tool(tool, pol)
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
