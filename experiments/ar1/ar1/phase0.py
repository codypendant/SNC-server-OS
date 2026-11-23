"""Phase 0 (prereg section 6): python -m ar1.phase0 <check|all> <runs> <out_dir>."""

import json
import os
import random
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

os.environ.setdefault("AR1_PORT", "26320")

from check import wal  # noqa: E402
from check.check import Check  # noqa: E402
from check.oracle import Oracle, lsn_int  # noqa: E402

from . import jlog, rt  # noqa: E402
from .app import end_previous_backends  # noqa: E402
from .pg import ADMIN, BIN, NOMON, Pair, connect  # noqa: E402

PY = sys.executable
HERE = Path(__file__).resolve().parent.parent
APP = "ar1-p0"
HOLD_S = 3.0
TLI_SQL = "('x'||substr(pg_walfile_name(pg_current_wal_insert_lsn()),1,8))::bit(32)::int"


def admin(port, sql, *args):
    with connect(port, ADMIN, app=f"{APP}-admin") as c:
        c.execute("SET synchronous_commit = local")
        r = c.execute(sql, args)
        return r.fetchall() if r.description else None


def new_pair(root, snc=False):
    p = Pair(root, int(os.environ["AR1_PORT"]))
    p.create()
    with p.primary.conn() as c:
        c.execute("CREATE TABLE p0(id bigserial PRIMARY KEY, tok text, pad text)")
        c.execute("ALTER TABLE p0 ALTER pad SET STORAGE EXTERNAL")
    rt.PORT = p.primary.port
    rt.USER = "ar1"
    rt._local.c = None
    if snc:
        rt.setup_db(Path(root) / "blobs")
    return p


def commit(port, tok, mode=None, pad="", app=f"{APP}-load"):
    with connect(port, app=app) as c:
        with c.transaction():
            if mode:
                c.execute(f"SET LOCAL synchronous_commit TO {mode}")
            return c.execute(
                "INSERT INTO p0(tok, pad) VALUES (%s, %s) RETURNING pg_current_xact_id()::text", (tok, pad)
            ).fetchone()[0]


def vary(p, rnd):
    k = rnd.randint(0, 1000)
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(lambda i: commit(p.primary.port, f"d{i}"), range(k)))
    ckpt = rnd.random() < 0.5
    if ckpt:
        admin(p.primary.port, "CHECKPOINT")
    pad = rnd.randint(0, 8000)
    commit(p.primary.port, "pad", pad=rnd.randbytes(pad).hex())
    return {"dummy_writes": k, "checkpoint": ckpt, "pad": pad}


def background(fn, *a):
    box = {}

    def go():
        try:
            box["r"] = fn(*a)
        except Exception as e:
            box["e"] = repr(e)

    t = threading.Thread(target=go, daemon=True)
    t.start()
    return t, box


def wait_for(pred, timeout, step=0.01):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def in_syncrep(port, app):
    return bool(admin(port, "SELECT 1 FROM pg_stat_activity WHERE application_name=%s AND wait_event='SyncRep'", app))


def commit_end(waldir, xid):
    cm, _ = wal.commits(wal.dump(waldir, BIN))
    found = cm.get(wal.xid32(xid))
    return max(found) if found else None


def run_json(R, p, point, seed):
    R.mkdir(parents=True, exist_ok=True)
    (R / "ctl").mkdir(exist_ok=True)
    run = {
        "point": point,
        "seed": seed,
        "arm": "snc",
        "mutation": None,
        "scenario": "S-M",
        "svc_port": p.primary.port + 10,
        "stub_latency_ms": 20,
        "app_name_prefix": APP,
        "pg": {
            "bin": str(BIN),
            "primary_port": p.primary.port,
            "standby_port": p.standby.port,
            "primary_data": str(p.primary.data),
            "standby_data": str(p.standby.data),
        },
    }
    (R / "run.json").write_text(json.dumps(run))
    rt.R = R


def classify(R, toks):
    ch = Check(
        R,
        db_rows={
            "commit_token": [{"token": k, "xid": x, "epoch": 1, "kind": "outcome", "name": k} for k, x in toks.items()]
        },
    )
    ch.build_tokens()
    ch.build_cuts()
    ch.classify()
    return ch.classes, ch.unresolved, ch.invalid


# ---- OI-1a with C-1, C-2, C-3 --------------------------------------------------------------------------

MEM_SQL = ("SELECT id, version, content_hash FROM snc.memory ORDER BY id, version", ())


def write_memory(port, mode, app, notices=None):
    with connect(port, app=app) as c:
        if notices is not None:
            c.add_notice_handler(lambda d: notices.append(d.message_primary))
        with c.transaction():
            if mode:
                c.execute(f"SET LOCAL synchronous_commit TO {mode}")
            c.execute("INSERT INTO snc.memory VALUES ('m1', 2, 'A', 'h2', 'p0:M-done')")
            return c.execute(
                "INSERT INTO snc.commit_token VALUES ('p0:M-done', pg_current_xact_id(), 1, 'outcome', 'M-done', 'A')"
                " RETURNING xid::text"
            ).fetchone()[0]


def write_off(port):
    with connect(port, app=f"{APP}-off") as c:
        with c.transaction():
            c.execute("SET LOCAL synchronous_commit TO off")
            c.execute("INSERT INTO snc.memory VALUES ('m2', 2, 'A', 'h2', 'p0:off')")
            return c.execute(
                "INSERT INTO snc.commit_token VALUES ('p0:off', pg_current_xact_id(), 1, 'outcome', 'M-done', 'A')"
                " RETURNING xid::text"
            ).fetchone()[0]


def gateway(port, gated, box):
    gate = {"call_id": "TI.e1", "attempt": 1}
    rows = rt.authoritative([MEM_SQL], wait=False)[0]
    lsn, tli = rt.bound()
    if gated:
        rt.wait_established(lsn, tli, {**gate, "step": "read"})
    box["read"] = [list(r) for r in rows]
    with connect(port, app=f"{APP}-gw") as c:
        with c.transaction():
            c.execute("INSERT INTO snc.attempt VALUES ('TI.e1', 1, 1, 'A:T:T-I', 'p0', 'p0:TI-disp')")
            box["xid"] = c.execute(
                "INSERT INTO snc.commit_token VALUES ('p0:TI-disp', pg_current_xact_id(), 1, 'decision', 'TI-disp', 'A')"
                " RETURNING xid::text"
            ).fetchone()[0]
    lsn, tli = rt.bound()
    if gated:
        rt.wait_established(lsn, tli, gate)
    else:
        jlog.append(
            rt.R / "gateway.jsonl",
            {**gate, "bound": lsn, "tli": tli, "flush_lsn": None, "sync_state": None, "decision": "established"},
        )
    body = json.dumps(
        {"call_id": "TI.e1", "key": "A:T:T-I", "attempt": 1, "agent": "A", "epoch": 1, "args": {"tool": "T-I"}}
    ).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port + 10}/T-I", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        box["sent"] = r.status


def oi1a(root, rnd, variant, gated=True):
    p = new_pair(root, snc=True)
    R = Path(root) / "run"
    run_json(R, p, f"OI-1a/{variant}", rnd.randint(0, 1 << 30))
    env = {**os.environ, "AR1_RUN": str(R), "AR1_EXECUTOR": "svc"}
    svc = subprocess.Popen([PY, "-m", "ar1.services", str(R)], cwd=HERE, env=env)
    procs = []
    try:
        out = {"variant": variant, "gated": gated, "vary": vary(p, rnd)}
        o = Oracle(R, p.primary.port, p.standby.port, APP)
        port = p.primary.port
        p.stop_standby()
        notices, wapp = [], f"{APP}-w"
        if variant in ("off", "local"):
            xid = write_memory(port, variant, wapp)
        else:
            t, box = background(write_memory, port, None, wapp, notices)
            if not wait_for(lambda: in_syncrep(port, wapp), 10):
                return {**out, "invalid": "writer never waited in SyncRep"}
            if variant == "cancel":
                admin(port, "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name=%s", wapp)
                t.join(10)
                xid = box.get("r")
                out["client"] = {"returned": "r" in box, "error": box.get("e"), "notices": notices}
            else:
                out["off_xid"] = write_off(port)
                p.primary.restart(variant.split("-")[1])
                t.join(10)
                out["client"] = {"returned": "r" in box, "error": box.get("e")}
                rt._local.c = None
                r = admin(port, "SELECT xid::text FROM snc.commit_token WHERE token='p0:M-done'")
                xid = r[0][0] if r else None
                out["off_after_restart"] = {
                    "row": bool(admin(port, "SELECT 1 FROM snc.memory WHERE id='m2' AND version=2")),
                    "token": bool(admin(port, "SELECT 1 FROM snc.commit_token WHERE token='p0:off'")),
                }
        out["xid"] = xid
        out["physical"] = bool(admin(port, "SELECT 1 FROM snc.memory WHERE id='m1' AND version=2"))
        o.kill_cut("pre-api", lambda: None, standby_stopped=True)
        procs = [
            subprocess.Popen(
                [PY, "-m", "ar1.iface", str(port), cmd, arg], cwd=HERE, env=env, stdout=subprocess.PIPE, text=True
            )
            for cmd, arg in (("state", "A"), ("reconstruct", "m1"))
        ]
        lsn, tli = rt.bound()
        ack = background(rt.wait_established, lsn, tli)
        policy = background(rt.authoritative, [MEM_SQL])
        gbox = {}
        gw = background(gateway, port, gated, gbox)
        time.sleep(HOLD_S)
        held = {
            "state": procs[0].poll() is None,
            "reconstruct": procs[1].poll() is None,
            "ack": ack[0].is_alive(),
            "policy": policy[0].is_alive(),
            "gateway_send": "sent" not in gbox,
        }
        svc_before = jlog.read(R / "svc" / "T-I.jsonl") if (R / "svc" / "T-I.jsonl").exists() else []
        gw_before = jlog.read(R / "gateway.jsonl") if (R / "gateway.jsonl").exists() else []
        pre_resume = admin(port, "SELECT pg_current_wal_insert_lsn()::text")[0][0]
        p.resume_standby()
        answers = [pr.communicate(timeout=30)[0] for pr in procs]
        live = all(x[0].join(30) or not x[0].is_alive() for x in (ack, policy, gw))
        st = json.loads(answers[0])
        o.final_cut()
        o.close()
        disp = commit_end(p.primary.wal_dir(), gbox["xid"]) if "xid" in gbox else None
        classes, unresolved, invalid = classify(R, {"p0:M-done": xid} if xid else {})
        out.update(
            held=held,
            tool_entries_before_resume=len(svc_before),
            gateway_before_resume=[g["decision"] for g in gw_before],
            pre_resume_insert_lsn=pre_resume,
            dispatch_commit_start=wal.lsn_str(disp[1]) if disp else None,
            live=live,
            gateway=gbox,
            ack=ack[1],
            policy_read=policy[1].get("r"),
            state_after_resume=[m for m in st["memory"] if m["id"] == "m1"],
            oracle=classes.get("p0:M-done"),
            oracle_unresolved=unresolved,
            oracle_invalid=invalid,
        )
        no_effect = (
            all(held.values())
            and not svc_before
            and all(d in ("waiting", "refused") for d in out["gateway_before_resume"])
            and disp is not None
            and disp[1] >= lsn_int(pre_resume)
        )
        off = out.get("off_after_restart")
        off_ok = off is None or off["row"] == off["token"]
        succeeded = gbox.get("sent") == 200 and not any("e" in b for b in (gbox, ack[1], policy[1]))
        cls = classes.get("p0:M-done") or {}
        oracle_ok = cls.get("at_cuts", {}).get("pre-api") == "L" and not unresolved and not invalid
        after = any(m["version"] == 2 for m in out["state_after_resume"])
        out["violation_evidence"] = {
            "dispatch_commit_below_resume": disp is not None and disp[1] < lsn_int(pre_resume),
            "tool_entry_before_resume": bool(svc_before),
            "established_before_resume": "established" in out["gateway_before_resume"],
        }
        out["checks"] = dict(
            no_effect=no_effect,
            succeeded=succeeded,
            off_ok=off_ok,
            xid=xid is not None,
            oracle_ok=oracle_ok,
            after=after,
        )
        out["pass"] = live and out["physical"] and all(out["checks"].values())
        return out
    finally:
        for pr in procs:
            pr.kill()
        svc.kill()
        p.destroy()


OI1A_VARIANTS = ["off", "local", "cancel", "restart-immediate", "restart-fast"]


def run_oi1a(root, rnd, i):
    v = OI1A_VARIANTS[i % len(OI1A_VARIANTS)]
    return oi1a(root, rnd, v)


def run_oi1a_control(root, rnd, i):
    out = oi1a(root, rnd, OI1A_VARIANTS[i % len(OI1A_VARIANTS)], gated=False)
    out["pass"] = any(out.get("violation_evidence", {}).values())
    return out


# ---- OI-1a-v ---------------------------------------------------------------------------------------------


def not_established(lsn, tli, timeout=2.0):
    t, _ = background(rt.wait_established, lsn, tli)
    t.join(timeout)
    return t.is_alive()


def oi1av(root, rnd, i):
    p = new_pair(root)
    try:
        out = {"vary": vary(p, rnd)}
        port = p.primary.port
        lsn, tli = rt.bound()
        commit(port, "after")
        out["ar1_established"] = not not_established(lsn, tli, 5)
        rt.USER, rt._local.c = NOMON, None
        out["nomon_row"] = rt.q("SELECT flush_lsn::text, sync_state FROM pg_stat_replication")
        out["nomon_not_established"] = not_established(lsn, tli)
        rt.USER, rt._local.c = "ar1", None
        conninfo = f"host=127.0.0.1 port={port} user={ADMIN} application_name=s2"
        admin(p.standby.port, f"ALTER SYSTEM SET primary_conninfo = '{conninfo}'")
        admin(p.standby.port, "SELECT pg_reload_conf()")
        wait_for(
            lambda: admin(
                port, "SELECT 1 FROM pg_stat_replication WHERE application_name='s2'" " AND state='streaming'"
            ),
            30,
        )
        lsn, tli = rt.bound()
        commit(port, "s2", mode="local")
        out["s2_row"] = admin(port, "SELECT application_name, flush_lsn::text, sync_state FROM pg_stat_replication")
        out["s2_not_established"] = not_established(lsn, tli)
        admin(p.standby.port, f"ALTER SYSTEM SET primary_conninfo = '{conninfo[:-2]}s1'")
        admin(p.standby.port, "SELECT pg_reload_conf()")
        admin(port, "ALTER SYSTEM SET synchronous_standby_names = ''")
        admin(port, "SELECT pg_reload_conf()")
        wait_for(
            lambda: admin(
                port,
                "SELECT 1 FROM pg_stat_replication WHERE application_name='s1'"
                " AND state='streaming' AND sync_state='async'",
            ),
            30,
        )
        lsn, tli = rt.bound()
        commit(port, "async", mode="local")
        out["async_row"] = admin(port, "SELECT application_name, flush_lsn::text, sync_state FROM pg_stat_replication")
        out["async_not_established"] = not_established(lsn, tli)
        admin(port, "ALTER SYSTEM RESET synchronous_standby_names")
        admin(port, "SELECT pg_reload_conf()")
        out["restored_established"] = not not_established(lsn, tli, 10)
        out["pass"] = all(
            out[k]
            for k in (
                "ar1_established",
                "nomon_not_established",
                "s2_not_established",
                "async_not_established",
                "restored_established",
            )
        )
        return out
    finally:
        rt.USER, rt._local.c = "ar1", None
        p.destroy()


# ---- OI-1b -----------------------------------------------------------------------------------------------


def identify(port):
    r = subprocess.run(
        [
            str(BIN / "psql"),
            f"host=127.0.0.1 port={port} user={ADMIN} dbname=postgres " "replication=database",
            "-AtXc",
            "IDENTIFY_SYSTEM",
        ],
        capture_output=True,
        text=True,
    )
    return int(r.stdout.split("|")[1])


def candidate(c):
    try:
        return c.execute(f"SELECT pg_current_wal_insert_lsn()::text, {TLI_SQL}").fetchone()[1]
    except Exception as e:
        return f"error: {type(e).__name__}"


def oi1b(root, rnd, i):
    ckpt, restart, cutoff = bool(i & 1), bool(i & 2), bool(i & 4)
    p = new_pair(root)
    try:
        out = {"vary": vary(p, rnd), "restartpoint_before": ckpt, "restart_after": restart, "cut_off": cutoff}
        if ckpt:
            admin(p.standby.port, "CHECKPOINT")
        pre = connect(p.standby.port)
        out["pre_session_before"] = candidate(pre)
        p.primary.stop("immediate")
        if cutoff:
            pre.close()
            p.cut_off_standby()
            pre = connect(p.standby.port)
        if not p.promote_standby():
            return {**out, "invalid": "pg_promote returned false"}
        with connect(p.standby.port) as c:
            first = candidate(c)
        reads = {
            "first": first,
            "identify": identify(p.standby.port),
            "pre_session_after": candidate(pre),
            "control_checkpoint": admin(p.standby.port, "SELECT timeline_id FROM pg_control_checkpoint()")[0][0],
        }
        pre.close()
        admin(p.standby.port, "CHECKPOINT")
        reads["control_after_checkpoint"] = admin(p.standby.port, "SELECT timeline_id FROM pg_control_checkpoint()")[0][
            0
        ]
        if restart:
            p.standby.restart("fast")
            with connect(p.standby.port) as c:
                reads["after_restart"] = candidate(c)
            reads["identify_after_restart"] = identify(p.standby.port)
        out["reads"] = reads
        want = 2
        out["pass"] = (
            str(out["pre_session_before"]).startswith("error")
            and reads["first"] == want
            and reads["identify"] == want
            and reads["pre_session_after"] == want
            and reads["control_after_checkpoint"] == want
            and (not restart or (reads["after_restart"] == want and reads["identify_after_restart"] == want))
        )
        return out
    finally:
        p.destroy()


# ---- OI-1c -----------------------------------------------------------------------------------------------


def oi1c(root, rnd, i):
    early = bool(i & 1)
    p = new_pair(root, snc=True)
    try:
        out = {"vary": vary(p, rnd), "early_xid": early}
        c = connect(p.primary.port, app=f"{APP}-x")
        c.autocommit = False
        if early:
            x = c.execute("SELECT pg_current_xact_id()::text").fetchone()[0]
            lsn = admin(p.primary.port, "SELECT pg_log_standby_snapshot()::text")[0][0]
            wait_for(lambda: admin(p.standby.port, "SELECT pg_last_wal_replay_lsn() >= %s::pg_lsn", lsn)[0][0], 30)
        p.standby.stop("fast")
        switch = admin(p.primary.port, "SELECT pg_current_wal_insert_lsn()::text")[0][0]
        if not early:
            x = c.execute("SELECT pg_current_xact_id()::text").fetchone()[0]
        c.execute("SET LOCAL synchronous_commit TO local")
        c.execute("INSERT INTO snc.commit_token VALUES ('p0:X', pg_current_xact_id(), 1, 'outcome', 'X', 'A')")
        c.commit()
        c.close()
        out["xid"] = x
        p.primary.stop("immediate")
        end = commit_end(p.primary.wal_dir(), x)
        out["switchpoint"], out["x_commit_start"] = switch, wal.lsn_str(end[1]) if end else None
        if not end or end[1] < lsn_int(switch):
            return {**out, "invalid": "X commit not above the switchpoint"}
        with open(p.standby.data / "postgresql.auto.conf", "a") as f:
            f.write("primary_conninfo = ''\nrestore_command = ''\n")
        p.standby.start()
        if not p.promote_standby():
            return {**out, "invalid": "pg_promote returned false"}
        port = p.standby.port
        with connect(port) as s:
            s.execute("SET synchronous_commit = local")

            def status():
                try:
                    return s.execute("SELECT pg_xact_status(%s::xid8)", (x,)).fetchone()[0]
                except Exception as e:
                    return f"error: {str(e).splitlines()[0]}"

            def token():
                return bool(s.execute("SELECT 1 FROM snc.commit_token WHERE token='p0:X'").fetchall())

            out["before"] = {"xact_status": status(), "token": token()}
            reissued = None
            while True:
                with s.transaction():
                    y = s.execute(
                        "INSERT INTO p0(tok) VALUES ('after') RETURNING pg_current_xact_id()::text"
                    ).fetchone()[0]
                if y == x:
                    reissued = "committed"
                if int(y) > int(x):
                    break
            out["after"] = {"xact_status": status(), "token": token(), "reissuing_txn": reissued}
        out["pass"] = not out["before"]["token"] and not out["after"]["token"]
        return out
    finally:
        p.destroy()


# ---- OI-1d -----------------------------------------------------------------------------------------------

CLIENT = """
import sys, psycopg
c = psycopg.connect(host="127.0.0.1", port=int(sys.argv[1]), user="ar1", dbname="ar1", application_name=sys.argv[2])
i = 0
while True:
    tok = f"{sys.argv[3]}{i}"
    x = c.execute("INSERT INTO snc.commit_token VALUES (%s, pg_current_xact_id(), 1, 'outcome', 'p0', 'A')"
                  " RETURNING xid::text", (tok,)).fetchone()[0]
    print(tok, x, flush=True)
    c.commit()
    i += 1
    if len(sys.argv) > 4:
        break
"""


def recover_and_lookup(port, tok):
    rt.APP = f"{APP}-c"
    end_previous_backends()
    lsn, _ = rt.bound()
    found = admin(port, "SELECT xid::text FROM snc.commit_token WHERE token=%s", tok)
    return bool(found), lsn


def oi1d(root, rnd, i):
    p = new_pair(root, snc=True)
    port = p.primary.port
    app = f"{APP}-c-1"
    try:
        out = {"vary": vary(p, rnd)}
        p.stop_standby()
        cl = subprocess.Popen([PY, "-c", CLIENT, str(port), app, "det", "once"], stdout=subprocess.PIPE, text=True)
        tok, x = cl.stdout.readline().split()
        out["in_syncrep"] = wait_for(lambda: in_syncrep(port, app), 10)
        cl.kill()
        cl.wait()
        out["before_d7"] = {
            "token_visible": bool(admin(port, "SELECT 1 FROM snc.commit_token WHERE token=%s", tok)),
            "xact_status": admin(port, "SELECT pg_xact_status(%s::xid8)", x)[0][0],
        }
        committed, bound = recover_and_lookup(port, tok)
        p.resume_standby()
        end = commit_end(p.primary.wal_dir(), x)
        det = {
            "lookup": committed,
            "bound": bound,
            "commit_end": wal.lsn_str(end[2]) if end else None,
            "bound_ok": end is None or lsn_int(bound) >= end[2],
        }
        det["ok"] = committed == (end is not None) and det["bound_ok"]
        out["deterministic"] = det
        kills = []
        for k in range(20):
            cl = subprocess.Popen([PY, "-c", CLIENT, str(port), app, f"k{k}-"], stdout=subprocess.PIPE, text=True)
            last = None
            stop = time.monotonic() + rnd.uniform(0.02, 0.3)
            while time.monotonic() < stop:
                last = cl.stdout.readline().split() or last
            cl.kill()
            cl.wait()
            tok, x = last
            committed, bound = recover_and_lookup(port, tok)
            kills.append({"tok": tok, "xid": x, "lookup": committed, "bound": bound})
        time.sleep(1)
        cm, _ = wal.commits(wal.dump(p.primary.wal_dir(), BIN))
        bad = []
        for k in kills:
            later = bool(admin(port, "SELECT 1 FROM snc.commit_token WHERE token=%s", k["tok"]))
            e = cm.get(wal.xid32(k["xid"]))
            if (not k["lookup"] and later) or (e and lsn_int(k["bound"]) < max(e)[2]):
                bad.append(k)
        out["random"] = {"kills": len(kills), "committed": sum(k["lookup"] for k in kills), "violations": bad}
        out["negative_control_fired"] = (
            not out["before_d7"]["token_visible"] and out["before_d7"]["xact_status"] == "in progress"
        )
        out["pass"] = det["ok"] and not bad and out["in_syncrep"] and out["negative_control_fired"]
        return out
    finally:
        rt.APP = f"ar1-{rt.EXECUTOR}"
        p.destroy()


# ---- C-7 -------------------------------------------------------------------------------------------------

C7_N = int(os.environ.get("AR1_C7_N", "10000"))


def c7(root, rnd, i):
    mode = "on" if i % 2 == 0 else "off"
    p = new_pair(root)
    port = p.primary.port
    try:
        out = {"mode": mode, "vary": vary(p, rnd)}
        if mode == "off":
            admin(port, "ALTER SYSTEM SET wal_writer_delay = '10s'")
            admin(port, "SELECT pg_reload_conf()")
        rows = []
        with connect(port, app=f"{APP}-c7") as c:
            for n in range(C7_N):
                with c.transaction():
                    if mode == "off":
                        c.execute("SET LOCAL synchronous_commit TO off")
                    r = c.execute("INSERT INTO p0(tok) VALUES ('c7') RETURNING pg_current_xact_id()::text")
                    x = r.fetchone()[0]
                b, tli, raw, cur = c.execute(
                    f"SELECT b.*, pg_current_wal_insert_lsn()::text, pg_current_wal_lsn()::text FROM ({rt.BOUND}) b"
                ).fetchone()
                t0 = time.monotonic_ns()
                if mode == "on":
                    rt.wait_established(b, tli)
                rows.append((x, lsn_int(b), lsn_int(cur), time.monotonic_ns() - t0, lsn_int(raw)))
        if mode == "off":
            t0 = time.monotonic_ns()
            rt.wait_established(wal.lsn_str(rows[-1][1]), tli)
            rows[-1] = (*rows[-1][:3], time.monotonic_ns() - t0, rows[-1][4])
        d = wal.dump(p.primary.wal_dir(), BIN)
        cm, _ = wal.commits(d)
        nxt = {r[0]: r[2] for recs in d.values() for r in recs}
        below, cur_below, past_page, next_above, missing, lat, lat_past = 0, 0, 0, 0, 0, [], []
        for x, b, cur, ns, raw in rows:
            f = cm.get(wal.xid32(x))
            if not f:
                missing += 1
                continue
            tli, start, end = max(f)
            below += b < end
            cur_below += cur < end
            past = end % wal.BLCKSZ == 0 and raw - end in (24, 40)
            past_page += past
            (lat_past if past else lat).append(ns)
            next_above += b < (nxt.get(start) or start)
        lat.sort()
        lat_past.sort()
        out.update(
            commits=len(rows),
            missing=missing,
            bound_below_end=below,
            current_lsn_below_end=cur_below,
            bounds_past_page_boundary=past_page,
            bound_below_next_record_start=next_above,
            establish_ns={"p50": lat[len(lat) // 2], "p99": lat[int(len(lat) * 0.99)], "max": lat[-1]},
            establish_ns_past_page=lat_past,
        )
        out["negative_control_fired"] = cur_below > 0 if mode == "off" else None
        out["pass"] = below == 0 and missing == 0 and (mode == "on" or out["negative_control_fired"])
        return out
    finally:
        p.destroy()


def emit(c, n):
    with c.transaction():
        c.execute("SELECT pg_logical_emit_message(true, 'p', repeat('x', %s))", (n,))
        x = c.execute("SELECT pg_current_xact_id()::text").fetchone()[0]
    raw, cur = c.execute("SELECT pg_current_wal_insert_lsn()::text, pg_current_wal_lsn()::text").fetchone()
    b, tli = c.execute(rt.BOUND).fetchone()
    return x, lsn_int(raw), lsn_int(b), tli


def land(c, g, target):
    a = 96
    for _ in range(40):
        here = lsn_int(c.execute("SELECT pg_current_wal_insert_lsn()::text").fetchone()[0])
        n = g.bytepos(target) - g.bytepos(here) - a
        if n < 0:
            return None
        x, raw, b, tli = emit(c, n)
        got = g.bytepos(raw)
        if got == g.bytepos(target):
            return x, raw, b, tli
        a += got - g.bytepos(target)
        target = here + 2 * wal.BLCKSZ if target - here < 4 * wal.BLCKSZ else target
    return None


def raw_stalls(port, raw, limit=2.0):
    end = time.monotonic() + limit
    while time.monotonic() < end:
        if admin(
            port,
            "SELECT flush_lsn >= %s::pg_lsn FROM pg_stat_replication WHERE application_name='s1'",
            wal.lsn_str(raw),
        )[0][0]:
            return False
        time.sleep(0.01)
    return True


def c7b(root, rnd, i):
    """A-2 focused check: commits that end exactly on a page start (short header), on a segment start (long header),
    commit records that cross a page, and bounds read right after XLOG_SWITCH."""
    p = new_pair(root)
    port = p.primary.port
    g = wal.Geometry()
    cases = []
    try:
        with connect(port, ADMIN, "postgres", app=f"{APP}-c7b") as c:
            for kind, count in (("page", 10), ("segment", 2), ("cross", 10), ("segcross", 1), ("switch", 3)):
                for _ in range(count):
                    here = lsn_int(c.execute("SELECT pg_current_wal_insert_lsn()::text").fetchone()[0])
                    if kind == "page":
                        r = land(c, g, (here // wal.BLCKSZ + 2) * wal.BLCKSZ)
                    elif kind == "segment":
                        r = land(c, g, (here // g.seg + 1) * g.seg)
                    elif kind == "segcross":
                        r = land(c, g, (here // g.seg + 1) * g.seg + wal.LONG + 16)
                    elif kind == "cross":
                        r = land(c, g, (here // wal.BLCKSZ + 2) * wal.BLCKSZ + wal.SHORT + 16)
                    else:
                        x, _, _, _ = emit(c, rnd.randint(0, 4000))
                        c.execute("SELECT pg_switch_wal()")
                        raw = lsn_int(c.execute("SELECT pg_current_wal_insert_lsn()::text").fetchone()[0])
                        b, tli = c.execute(rt.BOUND).fetchone()
                        r = x, raw, lsn_int(b), tli
                    if r is None:
                        cases.append({"kind": kind, "landed": False})
                        continue
                    x, raw, b, tli = r
                    t0 = time.monotonic_ns()
                    rt.wait_established(wal.lsn_str(b), tli)
                    ns = time.monotonic_ns() - t0
                    stall = raw_stalls(port, raw) if kind in ("page", "segment") and raw != b else None
                    cases.append(
                        {"kind": kind, "landed": True, "xid": x, "raw": raw, "bound": b, "ns": ns, "raw_stalls": stall}
                    )
        cm, _ = wal.commits(wal.dump(p.primary.wal_dir(), BIN))
        for k in cases:
            if not k["landed"]:
                continue
            f = cm.get(wal.xid32(k["xid"]))
            tli_, start, end = max(f)
            k.update(
                start=wal.lsn_str(start),
                end=wal.lsn_str(end),
                raw_minus_end=k["raw"] - end,
                bound_minus_end=k["bound"] - end,
            )
            hdr = {"page": 24, "segment": 40}.get(k["kind"])
            span = {"cross": wal.BLCKSZ, "segcross": g.seg}.get(k["kind"])
            k["ok"] = (
                k["bound"] >= end
                and k["ns"] < 1e9
                and (hdr is None or (k["raw_minus_end"] == hdr and k["bound"] == end))
                and (span is None or (start // span < (end - 1) // span and k["raw_minus_end"] == 0))
                and (k["kind"] != "switch" or k["bound"] % g.seg == 0)
            )
            k["raw"], k["bound"] = wal.lsn_str(k["raw"]), wal.lsn_str(k["bound"])
        landed = [k for k in cases if k["landed"]]
        out = {
            "cases": cases,
            "landed": {
                kd: sum(k["kind"] == kd for k in landed) for kd in ("page", "segment", "cross", "segcross", "switch")
            },
            "negative_control_fired": any(k.get("raw_stalls") for k in landed),
        }
        out["pass"] = (
            all(k["ok"] for k in landed)
            and all(out["landed"][kd] > 0 for kd in out["landed"])
            and out["negative_control_fired"]
        )
        return out
    finally:
        p.destroy()


CHECKS = {
    "OI-1a": run_oi1a,
    "OI-1a-control": run_oi1a_control,
    "OI-1a-v": oi1av,
    "OI-1b": oi1b,
    "OI-1c": oi1c,
    "OI-1d": oi1d,
    "C-7": c7,
    "C-7b": c7b,
}


def main():
    which, runs, out_dir = sys.argv[1], int(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    work = Path(os.environ.get("AR1_RUNS", "/tmp/ar1-runs")) / "phase0"
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(1))
    prefix = f"phase0:{os.environ['AR1_FREEZE']}" if os.environ.get("AR1_FREEZE") else "phase0"
    for name in (CHECKS if which == "all" else [which]):
        results, invalid, i = [], [], 0
        while len(results) < runs and i < 2 * runs:
            rnd = random.Random(f"{prefix}:{name}:{i}")
            t0 = time.monotonic()
            try:
                r = CHECKS[name](work / f"{name}-{i}", rnd, i)
            except Exception as e:
                r = {"pass": False, "error": repr(e)}
            r["run"], r["seconds"] = i, round(time.monotonic() - t0, 1)
            (invalid if r.get("invalid") else results).append(r)
            i += 1
            print(
                json.dumps(
                    {
                        "check": name,
                        "run": r["run"],
                        "pass": r.get("pass"),
                        "invalid": r.get("invalid"),
                        "error": r.get("error"),
                        "seconds": r["seconds"],
                    }
                ),
                flush=True,
            )
        if invalid:
            (out_dir / f"{name}.invalid.json").write_text(json.dumps(invalid, indent=1, default=str))
        (out_dir / f"{name}.json").write_text(json.dumps(results, indent=1, default=str))


if __name__ == "__main__":
    main()
