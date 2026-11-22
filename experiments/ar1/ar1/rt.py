"""snc_rt: the S1 runtime of AR-1 on PostgreSQL 18 and DBOS Transact Python (prereg section 3)."""

import hashlib
import json
import os
import threading
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

import psycopg
import sqlalchemy as sa
from dbos import DBOS, SQLAlchemyDatasource

from . import fault, jlog

AGENT, TASK = "A", "T"
MUT = os.environ.get("AR1_MUTATION", "")
R = Path(os.environ["AR1_RUN"]) if os.environ.get("AR1_RUN") else None
RUN = json.loads((R / "run.json").read_text()) if R else {}
PORT = RUN.get("pg", {}).get("primary_port")
EXECUTOR = os.environ.get("AR1_EXECUTOR", "e1")
APP = f"ar1-{EXECUTOR}"
INCARNATION = uuid.uuid4().hex
USER = "ar1"
SVC = f"http://127.0.0.1:{RUN.get('svc_port')}"
SCENARIO = RUN.get("scenario", "S-M")
POLICY = json.loads((Path(__file__).parent.parent / "policy.json").read_text())
DISPATCH_TIMEOUT, MARGIN, ESCALATION = 2.0, 2.0, 10.0
TOOLS = [("T-I", "TI"), ("T-F", "TF"), ("T-N", "TN")]
ds = None


class Stale(Exception):
    def __init__(self, msg, xid=None):
        super().__init__(msg)
        self.xid = xid


class Cancelled(Exception):
    pass


class CasFailed(Exception):
    pass


_local = threading.local()


def conn():
    c = getattr(_local, "c", None)
    if c is None or c.closed or c.broken:
        end = time.time() + 30
        while True:
            try:
                c = _local.c = psycopg.connect(
                    host="127.0.0.1",
                    port=PORT,
                    user=USER,
                    dbname="ar1",
                    autocommit=True,
                    application_name=f"{APP}-rt",
                    connect_timeout=2,
                )
                break
            except psycopg.OperationalError:
                if time.time() > end:
                    raise
                time.sleep(0.2)
    return c


def q(sql, *args):
    for _ in range(50):
        try:
            return conn().execute(sql, args).fetchall()
        except psycopg.OperationalError:
            _local.c = None
            time.sleep(0.2)
    raise RuntimeError("primary unreachable")


TLI = "('x'||substr(pg_walfile_name(pg_current_wal_insert_lsn()),1,8))::bit(32)::int"
BOUND = (
    "SELECT ('0/0'::pg_lsn + CASE WHEN mod(l, 16777216) = 40 THEN l - 40 WHEN mod(l, 8192) = 24 THEN l - 24"
    f" ELSE l END)::text, {TLI} FROM (SELECT pg_current_wal_insert_lsn() - '0/0'::pg_lsn AS l) x"
)


def bound():
    return q(BOUND)[0]


class TimelineChanged(Exception):
    pass


def _gw(gate, decision, lsn, tli, flush, sync):
    if gate:
        jlog.append(
            R / "gateway.jsonl",
            {**gate, "bound": lsn, "tli": tli, "flush_lsn": flush, "sync_state": sync, "decision": decision},
        )


def wait_established(lsn, tli, gate=None):
    logged = False
    while True:
        flush, sync, cur_tli, ok = q(
            f"SELECT flush_lsn::text, sync_state, {TLI}, flush_lsn >= %s::pg_lsn"
            " FROM (SELECT 1) one LEFT JOIN pg_stat_replication ON application_name = 's1'",
            lsn,
        )[0]
        if cur_tli != tli:
            _gw(gate, "refused", lsn, tli, flush, sync)
            raise TimelineChanged(f"timeline {tli} -> {cur_tli}")
        if ok and sync == "sync":
            _gw(gate, "established", lsn, tli, flush, sync)
            return flush
        if not logged:
            _gw(gate, "waiting", lsn, tli, flush, sync)
            logged = True
        time.sleep(0.002)


def authoritative(sql_list, wait=True):
    for _ in range(50):
        try:
            c = conn()
            with c.transaction():
                c.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ")
                out = [c.execute(s, a).fetchall() for s, a in sql_list]
                lsn, tli = c.execute(BOUND).fetchone()
            break
        except psycopg.OperationalError:
            _local.c = None
            time.sleep(0.2)
    else:
        raise RuntimeError("primary unreachable")
    if wait:
        wait_established(lsn, tli)
    return out


def sha(obj):
    return hashlib.sha256((obj if isinstance(obj, str) else jlog.canonical(obj)).encode()).hexdigest()


SCHEMA = """
CREATE SCHEMA snc;
CREATE TABLE snc.agent(id text PRIMARY KEY, epoch int NOT NULL, version int NOT NULL);
CREATE TABLE snc.task(id text PRIMARY KEY, agent text NOT NULL, status text NOT NULL, requester text, goal text,
  version int NOT NULL);
CREATE TABLE snc.memory(id text, version int, agent text, content_hash text, token text, PRIMARY KEY (id, version));
CREATE TABLE snc.invocation(id text PRIMARY KEY, agent text, task text, epoch int, state text, manifest jsonb,
  whole_hash text, provider_request_id text, output jsonb, late bool NOT NULL DEFAULT false, retry_of text,
  incarnation text, token text);
CREATE TABLE snc.tool_call(id text PRIMARY KEY, agent text, task text, tool text, key text, epoch int, state text,
  args_hash text, attempts int NOT NULL DEFAULT 0, result jsonb, policy_id text, approval_id text, token text);
CREATE TABLE snc.attempt(call_id text, n int, epoch int, key text, incarnation text, token text,
  t timestamptz NOT NULL DEFAULT clock_timestamp(),
  PRIMARY KEY (call_id, n));
CREATE TABLE snc.policy_decision(id text PRIMARY KEY, task text, rule_version text, inputs_hash text, result text,
  obligations jsonb, epoch int, token text);
CREATE TABLE snc.approval(id text PRIMARY KEY, policy_id text, approver text, action_hash text, decision text,
  token text);
CREATE TABLE snc.run_event(id bigserial PRIMARY KEY, agent text, kind text, epoch int, outstanding jsonb, token text);
CREATE TABLE snc.history(id bigserial PRIMARY KEY, kind text, obj text, from_state text, to_state text, epoch int,
  detail jsonb, token text);
CREATE TABLE snc.commit_token(token text PRIMARY KEY, xid xid8 NOT NULL, epoch int, kind text, name text, agent text);
"""


def setup_db(blob_dir):
    blob_dir.mkdir(exist_ok=True)
    with psycopg.connect(host="127.0.0.1", port=PORT, user=USER, dbname="ar1", autocommit=False) as c:
        c.execute(SCHEMA)
        c.execute("INSERT INTO snc.agent VALUES ('A', 1, 1)")
        for i in (1, 2, 3):
            body = f"memory item m{i} for agent A"
            h = sha(body)
            (blob_dir / h).write_text(body)
            c.execute("INSERT INTO snc.memory VALUES (%s, 1, 'A', %s, 'setup')", (f"m{i}", h))
        c.commit()


def _hist(s, kind, obj, a, b, epoch, tok, detail=None):
    s.execute(
        sa.text(
            "INSERT INTO snc.history(kind,obj,from_state,to_state,epoch,detail,token)" " VALUES (:k,:o,:a,:b,:e,:d,:t)"
        ),
        dict(k=kind, o=obj, a=a, b=b, e=epoch, d=json.dumps(detail) if detail is not None else None, t=tok),
    )


def _cas(s, table, obj, frm, to, extra="", **kw):
    frm = frm if isinstance(frm, (list, tuple)) else [frm]
    r = s.execute(
        sa.text(f"UPDATE snc.{table} SET state=:to {extra} WHERE id=:id AND state = ANY(:frm)" " RETURNING id"),
        dict(to=to, id=obj, frm=list(frm), **kw),
    ).fetchall()
    if not r:
        raise CasFailed(f"{table} {obj} not in {frm}")


def _open(s, kind, name, epoch, lock="SHARE", check=True):
    a = s.execute(sa.text(f"SELECT epoch, version FROM snc.agent WHERE id='A' FOR {lock}")).one()
    exempt = MUT == "M2" or (MUT == "M10" and name in ("P-dec", "M-done"))
    if check and a.epoch != epoch and not exempt:
        xid = s.execute(sa.text("SELECT pg_current_xact_id_if_assigned()::text")).scalar()
        raise Stale(f"agent epoch {a.epoch}, activation epoch {epoch}", xid)
    tok = f"{DBOS.workflow_id}:{DBOS.step_id}:{name}"
    s.execute(
        sa.text("INSERT INTO snc.commit_token VALUES (:t, pg_current_xact_id(), :e, :k, :n, 'A')"),
        dict(t=tok, e=epoch, k=kind, n=name),
    )
    if s.execute(sa.text("SELECT current_setting('synchronous_commit')")).scalar() not in ("on", "remote_apply"):
        raise RuntimeError("synchronous_commit lowered")
    return a, tok


def _active(s):
    st = s.execute(sa.text("SELECT status FROM snc.task WHERE id='T'")).scalar()
    if st == "cancelled":
        raise Cancelled()
    if st != "accepted":
        raise Stale(f"task {st}")


def _authorized(s, pid, aid):
    p = s.execute(sa.text("SELECT result, obligations FROM snc.policy_decision WHERE id=:p"), dict(p=pid)).one()
    if p.result != "allow":
        raise Stale(f"policy {pid} {p.result}")
    if p.obligations.get("approval"):
        d = s.execute(sa.text("SELECT decision FROM snc.approval WHERE id=:a"), dict(a=aid)).scalar()
        if d != "approve":
            raise Stale(f"approval {aid} {d}")


def _state(s, table, obj, cols="state"):
    return s.execute(sa.text(f"SELECT {cols} FROM snc.{table} WHERE id=:id FOR UPDATE"), dict(id=obj)).one()


def _end(name):
    fault.at(f"a1:{name}")
    fault.at(f"a3:{name}")


def op_accept(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    s.execute(sa.text("INSERT INTO snc.task VALUES ('T','A','accepted','coordinator','write the package',1)"))
    s.execute(sa.text("UPDATE snc.agent SET version=version+1 WHERE id='A'"))
    _hist(s, "task", "T", None, "accepted", epoch, tok)
    return tok


def op_inv_prep(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    if a.get("retry_of"):
        st = s.execute(
            sa.text("SELECT state FROM snc.invocation WHERE id=:id FOR SHARE"), dict(id=a["retry_of"])
        ).scalar()
        if st == "output":
            raise CasFailed(f"invocation {a['retry_of']} has its output")
    s.execute(
        sa.text(
            "INSERT INTO snc.invocation(id,agent,task,epoch,state,manifest,whole_hash,retry_of,token)"
            " VALUES (:id,'A','T',:e,'prepared',:m,:h,:r,:t)"
        ),
        dict(
            id=a["id"],
            e=epoch,
            m=json.dumps(a["manifest"]),
            h=a["whole_hash"],
            r=None if MUT == "M12" else a.get("retry_of"),
            t=tok,
        ),
    )
    _hist(s, "invocation", a["id"], None, "prepared", epoch, tok)
    return tok


def op_inv_disp(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    _cas(s, "invocation", a["id"], "prepared", "dispatched", ", incarnation=:inc", inc=INCARNATION)
    _hist(s, "invocation", a["id"], "prepared", "dispatched", epoch, tok)
    return tok


def _derive(s, epoch, tok, obj, detail):
    s.execute(sa.text("UPDATE snc.task SET version=version+1 WHERE id='T'"))
    _hist(s, "derive", obj, None, "derived", epoch, tok, detail)


def _outcome_open(s, name, epoch):
    a, tok = _open(s, "outcome", name, epoch, check=(MUT == "M8"))
    return a.epoch == epoch, tok


def op_inv_acc(s, epoch, name, a):
    _, tok = _outcome_open(s, name, epoch)
    st = _state(s, "invocation", a["id"]).state
    to = "accepted" if st == "dispatched" else st
    _cas(s, "invocation", a["id"], ["dispatched", "result_unknown"], to, ", provider_request_id=:r", r=a["request_id"])
    _hist(s, "invocation", a["id"], st, to, epoch, tok, {"request_id": a["request_id"]})
    return tok


def op_inv_out(s, epoch, name, a):
    current, tok = _outcome_open(s, name, epoch)
    st = _state(s, "invocation", a["id"]).state
    late = st == "result_unknown" and MUT != "M12"
    _cas(
        s,
        "invocation",
        a["id"],
        ["dispatched", "accepted", "result_unknown"],
        "output",
        ", output=:o, late=:l, provider_request_id=coalesce(provider_request_id,:r)",
        o=json.dumps(a["output"]),
        l=late,
        r=a["request_id"],
    )
    _hist(s, "invocation", a["id"], st, "output", epoch, tok, {"late": late})
    if current:
        _derive(s, epoch, tok, a["id"], {"output_sha256": sha(a["output"]["output"])})
    return tok


def op_inv_unknown(s, epoch, name, a):
    _, tok = _open(s, "recovery", name, epoch)
    st = _state(s, "invocation", a["id"]).state
    _cas(s, "invocation", a["id"], ["dispatched", "accepted"], "result_unknown")
    _hist(s, "invocation", a["id"], st, "result_unknown", epoch, tok)
    return tok


def op_policy(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    rule = POLICY["rules"][SCENARIO]
    s.execute(
        sa.text("INSERT INTO snc.policy_decision VALUES (:id,'T',:v,:h,:r,:o,:e,:t)"),
        dict(
            id=a["id"],
            v=POLICY["version"],
            h=a["inputs_hash"],
            r="allow" if rule["allow"] else "deny",
            o=json.dumps({"approval": rule["approval"]}),
            e=epoch,
            t=tok,
        ),
    )
    _hist(s, "policy", a["id"], None, "allow" if rule["allow"] else "deny", epoch, tok)
    return tok


def op_ap_req(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    s.execute(
        sa.text("INSERT INTO snc.approval VALUES (:id,:p,'approver',:h,'requested',:t)"),
        dict(id=a["id"], p=a["policy_id"], h=a["action_hash"], t=tok),
    )
    _hist(s, "approval", a["id"], None, "requested", epoch, tok)
    return tok


def op_ap_dec(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    s.execute(sa.text("UPDATE snc.approval SET decision=:d WHERE id=:id"), dict(d=a["decision"], id=a["id"]))
    _hist(s, "approval", a["id"], "requested", a["decision"], epoch, tok)
    if a["decision"] == "deny":
        s.execute(sa.text("UPDATE snc.task SET status='denied', version=version+1 WHERE id='T'"))
        _hist(s, "task", "T", "accepted", "denied", epoch, tok)
    return tok


def op_tool_prep(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    _authorized(s, a["policy_id"], a.get("approval_id"))
    s.execute(
        sa.text(
            "INSERT INTO snc.tool_call(id,agent,task,tool,key,epoch,state,args_hash,policy_id,approval_id,token)"
            " VALUES (:id,'A','T',:tool,:k,:e,'prepared',:h,:p,:ap,:t)"
        ),
        dict(
            id=a["id"],
            tool=a["tool"],
            k=a["key"],
            e=epoch,
            h=sha(a["args"]),
            p=a["policy_id"],
            ap=a.get("approval_id"),
            t=tok,
        ),
    )
    _hist(s, "call", a["id"], None, "prepared", epoch, tok)
    return tok


def op_tool_disp(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    c = _state(s, "tool_call", a["id"], "state, policy_id, approval_id")
    _authorized(s, c.policy_id, c.approval_id)
    frm = ["prepared"] if a["n"] == 1 else ["dispatched", "in_doubt"]
    _cas(s, "tool_call", a["id"], frm, "dispatched", ", attempts=:n", n=a["n"])
    s.execute(
        sa.text("INSERT INTO snc.attempt VALUES (:c,:n,:e,:k,:inc,:t)"),
        dict(c=a["id"], n=a["n"], e=epoch, k=a["key"], inc=INCARNATION, t=tok),
    )
    _hist(s, "call", a["id"], c.state, "dispatched", epoch, tok, {"attempt": a["n"], "key": a["key"]})
    return tok


def op_tool_out(s, epoch, name, a):
    current, tok = _outcome_open(s, name, epoch)
    st, attempts = _state(s, "tool_call", a["id"], "state, attempts")
    to = "rejected" if a["kind"] == "rejected_stale" else "completed"
    if st in ("completed", "rejected") or a["n"] < attempts:
        late = "rejected" if a["kind"] == "rejected_stale" else "duplicate"
        _hist(s, "attempt", a["id"], st, late, epoch, tok, {"result": a["result"], "attempt": a["n"]})
        return tok
    _cas(s, "tool_call", a["id"], ["dispatched", "in_doubt"], to, ", result=:r", r=json.dumps(a["result"]))
    _hist(s, "call", a["id"], st, to, epoch, tok, {"attempt": a["n"], "by": a.get("by", "tool")})
    if current:
        _derive(s, epoch, tok, a["id"], {"result": a["result"]})
    return tok


def op_tool_indoubt(s, epoch, name, a):
    _, tok = _open(s, "recovery", name, epoch)
    _cas(s, "tool_call", a["id"], "dispatched", "in_doubt")
    _hist(s, "call", a["id"], "dispatched", "in_doubt", epoch, tok, {"owner": "requester"})
    return tok


def op_derive(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _derive(s, epoch, tok, a["obj"], {"from_stale_outcome": True})
    return tok


def op_done(s, epoch, name, a):
    _, tok = _open(s, "decision", name, epoch)
    _active(s)
    body = "summary written by agent A"
    h = sha(body)
    (R / "blobs" / h).write_text(body)
    s.execute(sa.text("INSERT INTO snc.memory VALUES ('m4', 1, 'A', :h, :t)"), dict(h=h, t=tok))
    s.execute(sa.text("UPDATE snc.task SET status='done', version=version+1 WHERE id='T'"))
    s.execute(sa.text("UPDATE snc.agent SET version=version+1 WHERE id='A'"))
    _hist(s, "task", "T", "accepted", "done", epoch, tok)
    return tok


def op_takeover(s, epoch, name, a):
    ag, tok = _open(s, "takeover", name, epoch, lock="UPDATE", check=False)
    if ag.epoch != epoch - 1:
        raise Stale(f"takeover expected epoch {epoch - 1}, found {ag.epoch}")
    s.execute(sa.text("UPDATE snc.agent SET epoch=:e, version=version+1 WHERE id='A'"), dict(e=epoch))
    out = _reject_and_outstanding(s, epoch, tok, "superseded")
    s.execute(
        sa.text("INSERT INTO snc.run_event(agent,kind,epoch,outstanding,token) VALUES ('A','takeover',:e,:o,:t)"),
        dict(e=epoch, o=json.dumps([] if MUT == "M11" else out), t=tok),
    )
    return tok


def _reject_and_outstanding(s, epoch, tok, why, below=None):
    below = below or epoch
    if MUT != "M6":
        for (cid,) in s.execute(
            sa.text("SELECT id FROM snc.tool_call WHERE state='prepared' AND epoch<:e"), dict(e=below)
        ).fetchall():
            _cas(s, "tool_call", cid, "prepared", "rejected", ", result=:r", r=json.dumps({"rejected": why}))
            _hist(s, "call", cid, "prepared", "rejected", epoch, tok, {"why": why})
        for (iid,) in s.execute(
            sa.text("SELECT id FROM snc.invocation WHERE state='prepared' AND epoch<:e"), dict(e=below)
        ).fetchall():
            _cas(s, "invocation", iid, "prepared", "abandoned")
            _hist(s, "invocation", iid, "prepared", "abandoned", epoch, tok, {"why": why})
    out = [
        {"kind": "call", "id": i, "attempt": n}
        for i, n in s.execute(
            sa.text("SELECT id, attempts FROM snc.tool_call WHERE state IN ('dispatched','in_doubt') AND epoch<=:e"),
            dict(e=below),
        ).fetchall()
    ]
    out += [
        {"kind": "invocation", "id": i}
        for (i,) in s.execute(
            sa.text("SELECT id FROM snc.invocation WHERE state IN ('dispatched','accepted') AND epoch<=:e"),
            dict(e=below),
        ).fetchall()
    ]
    return out


OPS = {k[3:]: v for k, v in globals().items() if k.startswith("op_")}


def init_datasource(url):
    global ds, commit
    ds = SQLAlchemyDatasource.create(url)

    @ds.transaction(isolation_level="READ COMMITTED")
    def commit(op, epoch, name, args):
        s = ds.sql_session()
        tok = OPS[op](s, epoch, name, args)
        _end(name)
        return tok


@DBOS.step()
def settle_step(name, gate=None):
    fault.at(f"a2:{name}")
    lsn, tli = bound()
    if gate and MUT == "M1":
        _gw(gate, "established", lsn, tli, None, None)
        return lsn
    wait_established(lsn, tli, gate)
    return lsn


def decide(op, epoch, name, args, gate=None):
    armed = fault.stale_begin(epoch, name)
    try:
        tok = commit(op, epoch, name, args)
    except (Stale, CasFailed, Cancelled) as e:
        if armed:
            fault.stale_end("rejected", str(e) or type(e).__name__, getattr(e, "xid", None))
        raise
    if armed:
        fault.stale_end("committed")
    settle_step(name, gate)
    fault.at(f"a4:{name}")
    return tok


STATE_SQL = [
    ("SELECT id, epoch, version FROM snc.agent WHERE id='A'", ()),
    ("SELECT id, status, version FROM snc.task WHERE id='T'", ()),
    ("SELECT id, version, content_hash FROM snc.memory ORDER BY id, version", ()),
    ("SELECT id, tool, key, state, attempts FROM snc.tool_call ORDER BY id", ()),
    ("SELECT id, state, retry_of FROM snc.invocation ORDER BY id", ()),
    ("SELECT id FROM snc.policy_decision ORDER BY id", ()),
    ("SELECT id, decision FROM snc.approval ORDER BY id", ()),
    ("SELECT obj FROM snc.history WHERE kind='derive'", ()),
]


@DBOS.step()
def read_state_step():
    _, task, mem, calls, invs, pol, apr, der = authoritative(STATE_SQL)
    return {
        "task": task[0][1] if task else None,
        "memory": [list(m) for m in mem],
        "calls": {c[0]: {"tool": c[1], "key": c[2], "state": c[3], "attempts": c[4]} for c in calls},
        "invocations": {i[0]: {"state": i[1], "retry_of": i[2]} for i in invs},
        "policy": [p[0] for p in pol],
        "approvals": {a[0]: a[1] for a in apr},
        "derived": [d[0] for d in der],
    }


def _attempt_inc(call_id, n):
    r = q("SELECT incarnation FROM snc.attempt WHERE call_id=%s AND n=%s", call_id, n)
    return r[0][0] if r else None


_streams = {}


@DBOS.step()
def send_model(inv_id, epoch, body, retry_of):
    r = q("SELECT state, incarnation FROM snc.invocation WHERE id=%s", inv_id)[0]
    if r[0] != "dispatched" or (r[1] != INCARNATION and MUT != "M1"):
        return {"outstanding": True}
    fault.at("b1:model")
    retry_of = None if MUT == "M12" else retry_of
    req = json.dumps(
        {"invocation_id": inv_id, "retry_of": retry_of, "agent": AGENT, "epoch": epoch, "body": body}
    ).encode()
    try:
        resp = urllib.request.urlopen(
            urllib.request.Request(f"{SVC}/stub", req, {"Content-Type": "application/json"}), timeout=DISPATCH_TIMEOUT
        )
        first = json.loads(resp.readline())
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return {"outstanding": True, "error": repr(e)}
    _streams[inv_id] = resp
    return {"request_id": first["request_id"]}


@DBOS.step()
def receive_model(inv_id):
    resp = _streams.pop(inv_id, None)
    if resp is None:
        return {"lost": True}
    try:
        out = json.loads(resp.readline())
    except (TimeoutError, OSError) as e:
        return {"lost": True, "error": repr(e)}
    fault.at("b3:model")
    return {"output": out}


@DBOS.step()
def send_tool(tool, call_id, key, n, epoch, args):
    if _attempt_inc(call_id, n) != INCARNATION and MUT != "M1":
        return {"outstanding": True}
    fault.at(f"b1:{tool}")
    req = json.dumps(
        {"call_id": call_id, "key": key, "attempt": n, "agent": AGENT, "epoch": epoch, "args": args}
    ).encode()
    try:
        resp = urllib.request.urlopen(
            urllib.request.Request(f"{SVC}/{tool}", req, {"Content-Type": "application/json"}), timeout=DISPATCH_TIMEOUT
        )
        out = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        out = json.loads(e.read())
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return {"outstanding": True, "error": repr(e)}
    fault.at(f"b3:{tool}")
    return out


@DBOS.step()
def http_get(path):
    with urllib.request.urlopen(f"{SVC}/{path}", timeout=5) as r:
        return json.loads(r.read())


def manifest(memory):
    items = [{"kind": "memory", "id": i, "version": v, "sha256": h} for i, v, h in memory if i in ("m1", "m2", "m3")]
    body = {
        "task": "write the package",
        "items": items,
        "content": [(R / "blobs" / it["sha256"]).read_text() for it in items],
    }
    return items, body


def model_phase(epoch, st):
    try:
        return _model_phase(epoch, st)
    except CasFailed:
        return model_phase(epoch, read_state_step())


def _model_phase(epoch, st):
    invs = st["invocations"]
    if any(v["state"] == "output" for v in invs.values()):
        return
    retry_of = None
    for iid, v in invs.items():
        if v["state"] in ("dispatched", "accepted"):
            if MUT == "M3":
                return model_call(epoch, iid, st, regenerate=True)
            decide("inv_unknown", epoch, "I-unknown", {"id": iid})
            retry_of = iid
        elif v["state"] == "result_unknown":
            retry_of = iid
    n = len(invs) + 1
    model_call(epoch, f"inv.e{epoch}.{n}", st, retry_of=retry_of)


def model_call(epoch, iid, st, retry_of=None, regenerate=False):
    items, body = manifest(st["memory"])
    whole = sha(body)
    if not regenerate:
        decide("inv_prep", epoch, "I-prep", {"id": iid, "manifest": items, "whole_hash": whole, "retry_of": retry_of})
        decide("inv_disp", epoch, "I-disp", {"id": iid}, gate={"invocation_id": iid, "attempt": 1})
        r = send_model(iid, epoch, body, retry_of)
    else:
        r = {"outstanding": True}
    if r.get("outstanding"):
        if not regenerate:
            return model_phase(epoch, read_state_step())
        r = regen_model(iid, epoch, body)
    if "request_id" in r and not regenerate:
        decide("inv_acc", epoch, "I-acc", {"id": iid, "request_id": r["request_id"]})
        rr = receive_model(iid)
        if rr.get("lost"):
            return model_phase(epoch, read_state_step())
        r = {**r, **rr}
    decide("inv_out", epoch, "I-out", {"id": iid, "output": r["output"], "request_id": r["request_id"]})


@DBOS.step()
def regen_model(iid, epoch, body):
    req = json.dumps({"invocation_id": iid, "retry_of": None, "agent": AGENT, "epoch": epoch, "body": body}).encode()
    resp = urllib.request.urlopen(
        urllib.request.Request(f"{SVC}/stub", req, {"Content-Type": "application/json"}), timeout=DISPATCH_TIMEOUT
    )
    first = json.loads(resp.readline())
    return {"request_id": first["request_id"], "output": json.loads(resp.readline())}


def approve(epoch, aid):
    ans = http_get(f"approve?task=T&action={aid}")["answer"]
    decide("ap_dec", epoch, "AP-dec", {"id": aid, "decision": ans})


def policy_phase(epoch, st):
    if st["policy"]:
        aid = next(iter(st["approvals"]), None)
        if aid and st["approvals"][aid] == "requested":
            approve(epoch, aid)
        return st["policy"][-1], aid
    mem = authoritative(
        [("SELECT id, version, content_hash FROM snc.memory ORDER BY id, version", ())], wait=MUT != "M5"
    )[0]
    pid = f"P.e{epoch}"
    decide("policy", epoch, "P-dec", {"id": pid, "inputs_hash": sha([list(m) for m in mem])})
    if not POLICY["rules"][SCENARIO]["approval"]:
        return pid, None
    aid = f"AP.e{epoch}"
    decide("ap_req", epoch, "AP-req", {"id": aid, "policy_id": pid, "action_hash": sha("T-I,T-F,T-N")})
    approve(epoch, aid)
    return pid, aid


def tool_args(tool):
    return {"tool": tool, "op": f"perform {tool}", "task": TASK}


def tool_phase(epoch, tool, pre, st, pid, aid):
    try:
        return _tool_phase(epoch, tool, pre, st, pid, aid)
    except CasFailed:
        return tool_phase(epoch, tool, pre, read_state_step(), pid, aid)


def _tool_phase(epoch, tool, pre, st, pid, aid):
    mine = [c for c, v in st["calls"].items() if v["tool"] == tool]
    live = [c for c in mine if st["calls"][c]["state"] not in ("rejected",)]
    if any(st["calls"][c]["state"] == "completed" for c in live):
        return
    key = f"A:T:{tool}"
    if live:
        cid = live[0]
        return resolve(epoch, tool, pre, cid, st["calls"][cid])
    cid = f"{pre}.e{epoch}"
    decide(
        "tool_prep",
        epoch,
        f"{pre}-prep",
        {"id": cid, "tool": tool, "key": key, "args": tool_args(tool), "policy_id": pid, "approval_id": aid},
    )
    dispatch(epoch, tool, pre, cid, key, 1)


def dispatch(epoch, tool, pre, cid, key, n):
    decide("tool_disp", epoch, f"{pre}-disp", {"id": cid, "n": n, "key": key}, gate={"call_id": cid, "attempt": n})
    r = send_tool(tool, cid, key, n, epoch, tool_args(tool))
    if r.get("outstanding"):
        return resolve(epoch, tool, pre, cid, {"state": "dispatched", "attempts": n, "key": key, "epoch": epoch})
    decide("tool_out", epoch, f"{pre}-out", {"id": cid, "n": n, "kind": r["kind"], "result": r["result"]})


@DBOS.step()
def since_dispatch(cid, n):
    return q("SELECT extract(epoch FROM clock_timestamp() - t) FROM snc.attempt WHERE call_id=%s AND n=%s", cid, n)[0][
        0
    ]


def resolve(epoch, tool, pre, cid, call):
    n = call["attempts"]
    key = call["key"]
    if call["state"] == "prepared":
        return dispatch(epoch, tool, pre, cid, key, 1)
    if tool == "T-I":
        r = http_get(f"status?key={key}")
        if r["found"]:
            return decide(
                "tool_out",
                epoch,
                f"{pre}-out",
                {"id": cid, "n": n, "kind": "effect", "result": r["result"], "by": "status"},
            )
        return dispatch(epoch, tool, pre, cid, key if MUT != "M9" else f"{key}#{n + 1}", n + 1)
    if tool == "T-F":
        return dispatch(epoch, tool, pre, cid, key, n + 1)
    if MUT in ("M4", "M14"):
        return dispatch(epoch, tool, pre, cid, key, n + 1)
    if call["state"] == "dispatched":
        DBOS.sleep(max(0.0, DISPATCH_TIMEOUT + MARGIN - float(since_dispatch(cid, n))))
        state = read_state_step()["calls"][cid]["state"]
        if state not in ("dispatched", "in_doubt"):
            return
        if state == "dispatched":
            decide("tool_indoubt", epoch, f"{pre}-indoubt", {"id": cid})
    DBOS.sleep(ESCALATION)
    st = read_state_step()
    if st["calls"][cid]["state"] != "in_doubt":
        return
    ans = http_get(f"owner?task=T&call_id={cid}")["answer"]
    if ans == "happened":
        return decide(
            "tool_out",
            epoch,
            f"{pre}-resolve",
            {"id": cid, "n": n, "kind": "effect", "result": {"owner": "happened"}, "by": "owner"},
        )
    dispatch(epoch, tool, pre, cid, key, n + 1)


def derive_phase(epoch, st):
    if MUT == "M13":
        return
    recalled = {v["retry_of"] for v in st["invocations"].values() if v["state"] not in ("prepared", "abandoned")}
    done = set(st["derived"]) | recalled
    for cid, v in st["calls"].items():
        if v["state"] == "completed" and cid not in done:
            decide("derive", epoch, "DERIVE", {"obj": cid})
    for iid, v in st["invocations"].items():
        if v["state"] == "output" and iid not in done:
            decide("derive", epoch, "DERIVE", {"obj": iid})


@DBOS.workflow()
def activation(epoch, takeover=False):
    try:
        if takeover:
            decide("takeover", epoch, "TAKEOVER", {})
            fault.at("post-takeover")
        st = read_state_step()
        if st["task"] is None:
            decide("accept", epoch, "C-acc", {})
            st = read_state_step()
        if st["task"] in ("done", "denied", "cancelled"):
            return st["task"]
        derive_phase(epoch, st)
        model_phase(epoch, st)
        derive_phase(epoch, read_state_step())
        pid, aid = policy_phase(epoch, read_state_step())
        st = read_state_step()
        if st["task"] == "denied":
            return "denied"
        for tool, pre in TOOLS:
            tool_phase(epoch, tool, pre, read_state_step(), pid, aid)
        derive_phase(epoch, read_state_step())
        decide("done", epoch, "M-done", {})
        return "done"
    except Stale:
        return "superseded"
    except Cancelled:
        return "cancelled"


def cancel():
    eng = sa.create_engine(
        f"postgresql+psycopg://{USER}@127.0.0.1:{PORT}/ar1", connect_args={"application_name": "requester-cancel"}
    )
    tok = f"cancel:{uuid.uuid4().hex}"
    with eng.begin() as s:
        ep = s.execute(sa.text("SELECT epoch FROM snc.agent WHERE id='A' FOR UPDATE")).scalar()
        if not s.execute(
            sa.text(
                "UPDATE snc.task SET status='cancelled', version=version+1 WHERE id='T' AND status='accepted'"
                " RETURNING id"
            )
        ).fetchall():
            return None
        s.execute(
            sa.text("INSERT INTO snc.commit_token VALUES (:t, pg_current_xact_id(), :e, 'cancel', 'CANCEL', 'A')"),
            dict(t=tok, e=ep),
        )
        _hist(s, "task", "T", "accepted", "cancelled", ep, tok)
        out = _reject_and_outstanding(s, ep, tok, "cancelled", below=ep + 1)
        s.execute(
            sa.text("INSERT INTO snc.run_event(agent,kind,epoch,outstanding,token) VALUES ('A','cancel',:e,:o,:t)"),
            dict(e=ep, o=json.dumps([] if MUT == "M11" else out), t=tok),
        )
    eng.dispose()
    wait_established(*bound())
    return tok
