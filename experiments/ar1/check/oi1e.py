import json
import os
import random
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from ar1.pg import ADMIN, BIN, Pair, connect
from check import wal
from check.check import Check
from check.oracle import Oracle, lsn_int, now

PORT = int(os.environ.setdefault("AR1_PORT", "25320"))
APP = "oi1e-app"
RESUME_S = 5.0

CLIENT = """
import sys, time, psycopg
c = psycopg.connect(host="127.0.0.1", port=int(sys.argv[1]), user="ar1", dbname="ar1", autocommit=True,
                    application_name=sys.argv[2])
with c.transaction():
    c.execute("SET LOCAL synchronous_commit TO local")
    x = c.execute("INSERT INTO t(tok) VALUES ('local') RETURNING pg_current_xact_id()::text").fetchone()[0]
print(x, flush=True)
time.sleep(600)
"""


def pair(root):
    p = Pair(root, PORT)
    try:
        p.create()
        with p.primary.conn() as c:
            c.execute("CREATE TABLE t(id bigserial primary key, tok text, pad text)")
    except Exception:
        p.destroy()
        raise
    return p


def commit(port, tok, pad="", local=False, app="oi1e-load"):
    with connect(port, app=app) as c:
        with c.transaction():
            if local:
                c.execute("SET LOCAL synchronous_commit TO local")
            return c.execute(
                "INSERT INTO t(tok, pad) VALUES (%s, %s) RETURNING pg_current_xact_id()::text", (tok, pad)
            ).fetchone()[0]


def vary(p, rnd):
    k = rnd.randint(0, 1000)
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(lambda i: commit(p.primary.port, f"dummy{i}"), range(k)))
    ckpt = rnd.random() < 0.5
    if ckpt:
        with connect(p.primary.port, ADMIN, "postgres") as c:
            c.execute("CHECKPOINT")
    pad = rnd.randint(0, 16000)
    commit(p.primary.port, "pad", "x" * pad)
    return {"dummy_writes": k, "checkpoint": ckpt, "pad": pad}


def run_dir(root, name, p):
    R = Path(root) / name
    R.mkdir(parents=True)
    (R / "run.json").write_text(
        json.dumps(
            {
                "arm": "snc",
                "point": "OI-1e",
                "pg": {
                    "bin": str(BIN),
                    "primary_port": p.primary.port,
                    "standby_port": p.standby.port,
                    "primary_data": str(p.primary.data),
                    "standby_data": str(p.standby.data),
                },
            }
        )
    )
    return R


def classes(R, toks):
    ch = Check(
        R,
        db_rows={
            "commit_token": [{"token": k, "xid": x, "epoch": 1, "kind": "outcome", "name": k} for k, x in toks.items()]
        },
    )
    ch.build_tokens()
    ch.build_cuts()
    ch.classify()
    return ch


def sub1(root, p, rnd):
    """Local commit during a stop: L at the kill cut, R at the final cut; the earlier 'on' commit is D."""
    out = {"vary": vary(p, rnd)}
    R = run_dir(root, "s1", p)
    o = Oracle(R, p.primary.port, p.standby.port, APP)
    x_on = commit(p.primary.port, "on")
    p.stop_standby()
    cl = subprocess.Popen([sys.executable, "-c", CLIENT, str(p.primary.port), APP], stdout=subprocess.PIPE, text=True)
    x_local = cl.stdout.readline().strip()
    o.kill_cut("k1", cl.kill, standby_stopped=True)
    cl.wait()
    p.resume_standby()
    o.final_cut()
    with p.standby.conn() as c:
        old_rule = c.execute("SELECT pg_last_wal_receive_lsn()::text").fetchone()[0]
    o.close()
    ch = classes(R, {"on": x_on, "local": x_local})
    c = ch.classes
    out.update(
        classes={k: c.get(k) for k in ("on", "local")}, old_rule=old_rule, unresolved=ch.unresolved, invalid=ch.invalid
    )
    out["pass"] = (
        c.get("local", {}).get("at_cuts", {}).get("k1") == "L"
        and c["local"]["class"] == "R"
        and c.get("on", {}).get("class") == "D"
        and not ch.unresolved
        and not ch.invalid
    )
    end = ch.tokens.get("local", {}).get("end")
    out["negative_control_fired"] = end is not None and lsn_int(old_rule) >= end
    return out


def classify_x(old_wal, new_wal, xids):
    """X: in the old timeline's WAL with end above the switchpoint the new timeline's history gives for it."""
    sw = wal.history(new_wal)
    cm, _ = wal.commits(wal.dump(old_wal, BIN))
    res = {}
    for k, x in xids.items():
        found = cm.get(wal.xid32(x))
        if not found:
            res[k] = None
            continue
        tli, _, end = max(found)
        res[k] = {
            "class": "X" if tli in sw and end > sw[tli] else "kept",
            "end": wal.lsn_str(end),
            "tli": tli,
            "switchpoint": wal.lsn_str(sw[tli]) if tli in sw else None,
        }
    return res


def below_switch_lost(old_wal, new_wal):
    """Old-timeline commits from the start of the standby's WAL up to the switchpoint that its WAL does not hold."""
    sw = wal.history(new_wal)
    old, _ = wal.commits(wal.dump(old_wal, BIN))
    nd = wal.dump(new_wal, BIN)
    new, _ = wal.commits(nd)
    lo = min(r[0] for rs in nd.values() for r in rs)
    lost = []
    for x, recs in old.items():
        for tli, start, end in recs:
            if tli in sw and lo <= start and end <= sw[tli] and start not in {s for _, s, _ in new.get(x, [])}:
                lost.append({"xid": x, "end": wal.lsn_str(end)})
    return lost


def sub2(root, rnd):
    """OI-1c's schedule: X committed locally while the standby is down, then the standby is promoted."""
    p = pair(Path(root) / "pair2")
    try:
        out = {"vary": vary(p, rnd)}
        on = {f"on{i}": commit(p.primary.port, f"on{i}") for i in range(3)}
        p.standby.stop("fast")
        x = commit(p.primary.port, "X", local=True)
        p.primary.stop("immediate")
        with open(p.standby.data / "postgresql.auto.conf", "a") as f:
            f.write("primary_conninfo = ''\nrestore_command = ''\n")
        p.standby.start()
        if not p.promote_standby():
            return {"invalid": "pg_promote returned false"}
        with connect(p.standby.port) as c:
            c.execute("SET synchronous_commit = local")
            while int(c.execute("SELECT pg_current_xact_id()::text").fetchone()[0]) <= int(x):
                c.execute("INSERT INTO t(tok) VALUES ('after')")
            present = {r[0] for r in c.execute("SELECT tok FROM t WHERE tok LIKE 'on%' OR tok = 'X'")}
        cx = classify_x(p.primary.wal_dir(), p.standby.wal_dir(), {"X": x, **on})
        lost = below_switch_lost(p.primary.wal_dir(), p.standby.wal_dir())
        out.update(classes=cx, present=sorted(present), lost_below_switchpoint=lost)
        out["pass"] = bool(
            cx["X"]
            and cx["X"]["class"] == "X"
            and "X" not in present
            and all(cx[k] and cx[k]["class"] == "kept" and k in present for k in on)
            and lost == []
        )
        return out
    finally:
        p.destroy()


def sub3(root, p, n):
    """n freeze/thaw cycles under streaming load."""
    R = run_dir(root, "s3", p)
    o = Oracle(R, p.primary.port, p.standby.port, APP)
    stop, done, lock = threading.Event(), [], threading.Lock()

    def load(i):
        with connect(p.primary.port, app="oi1e-load") as c:
            while not stop.is_set():
                x = c.execute("INSERT INTO t(tok) VALUES ('load') RETURNING pg_current_xact_id()::text").fetchone()[0]
                with lock:
                    done.append((x, now()))

    th = [threading.Thread(target=load, args=(i,), daemon=True) for i in range(4)]
    for t in th:
        t.start()
    freezes, fails = [], []
    try:
        for j in range(n):
            time.sleep(random.uniform(0.005, 0.05))
            t0 = now()
            f = o.freeze(f"f{j}")
            r1 = o.standby_read()
            time.sleep(0.1)
            r2 = o.standby_read()
            c = o._cut("cut", f"f{j}", boundary=True)
            c.update(o.thaw())
            o.write(c)
            ok_pid, resumed = None, None
            end = time.monotonic() + RESUME_S
            while time.monotonic() < end:
                s = o.standby_read()
                ok_pid = s["walreceiver_pid"] == f["pid"]
                if lsn_int(s["flushed_lsn"]) >= lsn_int(c["insert_lsn"]):
                    resumed = True
                    break
                time.sleep(0.005)
            with lock:
                before = [x for x, t in done if t < t0]
            freezes.append(
                {
                    "label": f"f{j}",
                    "equal": r1["flushed_lsn"] == r2["flushed_lsn"],
                    "pid_kept": ok_pid,
                    "resumed": bool(resumed),
                    "overdue": c.get("overdue"),
                    "before": before,
                    "freeze_ms": c.get("freeze_ms"),
                }
            )
    finally:
        stop.set()
        for t in th:
            t.join(5)
    o.final_cut()
    o.close()
    toks = {}
    for f in freezes:
        for x in f["before"]:
            toks[f"x{x}"] = x
    with lock:
        toks.update({f"x{x}": x for x, _ in done})
    ch = classes(R, toks)
    cuts = {c["label"]: c for c in ch.cut_list}
    l_then_r = 0
    for f in freezes:
        c = cuts[f["label"]]
        if f["overdue"]:
            continue
        bad = (
            [x for x in f["before"] if not ch.at_or_below(ch.tokens[f"x{x}"], c)]
            if all("end" in ch.tokens[f"x{x}"] for x in f["before"])
            else ["missing"]
        )
        if bad:
            fails.append({"freeze": f["label"], "not_D": bad[:5]})
        pf, sf = lsn_int(c["primary"]["flush_lsn"]), ch.flushed(c)
        if pf is not None and pf > sf:
            fails.append(
                {
                    "freeze": f["label"],
                    "primary_above_standby": [c["primary"]["flush_lsn"], c["standby"]["flushed_lsn"]],
                }
            )
        if not (f["equal"] and f["pid_kept"] and f["resumed"]):
            fails.append({"freeze": f["label"], **{k: f[k] for k in ("equal", "pid_kept", "resumed")}})
        b = lsn_int(c["kill_boundary"])
        l_then_r += sum(
            1
            for t in ch.tokens.values()
            if "end" in t and ch.flushed(c) < t["end"] <= b and ch.at_or_below(t, ch.final)
        )
    overdue = sum(1 for f in freezes if f["overdue"])
    ms = sorted(f["freeze_ms"] or 0 for f in freezes)
    return {
        "freezes": n,
        "overdue": overdue,
        "fails": fails[:20],
        "n_fails": len(fails),
        "l_then_r": l_then_r,
        "load_commits": len(done),
        "unresolved": [u for u in ch.unresolved if "not D or R" not in u["reason"]][:5],
        "freeze_ms_p50": ms[len(ms) // 2] if ms else None,
        "freeze_ms_max": ms[-1] if ms else None,
        "pass": not fails and l_then_r > 0 and overdue == 0,
    }


def verdict(results, key):
    if any(not r.get(key) or "error" in r[key] or r[key].get("invalid") for r in results):
        return "UNRESOLVED"
    return "pass" if all(r[key].get("pass") for r in results) else "fail"


def main(workdir, n_runs, freezes=1000, seed=1):
    workdir = Path(workdir)
    rnd = random.Random(seed)
    per = max(1, freezes // n_runs)
    results = []
    for i in range(n_runs):
        root = workdir / f"run{i}"
        res = {}
        p = None
        try:
            p = pair(root / "pair1")
            for k, f in (("1", lambda: sub1(root, p, rnd)), ("3", lambda: sub3(root, p, per))):
                try:
                    res[k] = f()
                except Exception as e:
                    res[k] = {"error": repr(e)}
                if k == "1":
                    p.wait_sync()
        finally:
            if p:
                p.destroy()
        try:
            res["2"] = sub2(root, rnd)
        except Exception as e:
            res["2"] = {"error": repr(e)}
        for _ in range(3):
            if res.get("2", {}).get("invalid"):
                res["2"] = sub2(root / "retry", rnd)
        results.append(res)
    neg = all(r.get("1", {}).get("negative_control_fired") for r in results)
    out = {
        "runs": n_runs,
        "freezes": per * n_runs,
        "subchecks": {
            "1": verdict(results, "1"),
            "1-negative-control": "pass" if neg else "fail",
            "2": verdict(results, "2"),
            "3": verdict(results, "3"),
        },
        "detail": [{k: {kk: vv for kk, vv in v.items() if kk != "before"} for k, v in r.items()} for r in results],
    }
    out["overall"] = "pass" if all(v == "pass" for v in out["subchecks"].values()) else "UNRESOLVED"
    return out


if __name__ == "__main__":
    a = sys.argv[1:]
    r = main(a[0], int(a[1]), int(a[2]) if len(a) > 2 else 1000)
    print(json.dumps(r, indent=1, default=str))
