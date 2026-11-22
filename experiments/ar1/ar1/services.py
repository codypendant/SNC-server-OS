"""Stub model provider, fake tools T-I/T-F/T-N, approver and owner: one local HTTP process per run."""

import hashlib
import json
import os
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import psycopg

from . import jlog

R = Path(sys.argv[1]) if __name__ == "__main__" else None
RUN = json.loads((R / "run.json").read_text()) if R else {}
SEED = RUN.get("seed", 0)
LATENCY = RUN.get("stub_latency_ms", 20) / 1000
lock = threading.Lock()
tools = {"T-I": {}, "T-F": {}, "T-N": {}}
fence = {}
local = threading.local()


def flush_read():
    end = time.time() + 10
    err = None
    while time.time() < end:
        try:
            c = getattr(local, "conn", None)
            if c is None or c.closed:
                c = local.conn = psycopg.connect(
                    host="127.0.0.1",
                    port=RUN["pg"]["primary_port"],
                    user="ar1",
                    dbname="ar1",
                    autocommit=True,
                    application_name="recv-monitor",
                    connect_timeout=2,
                )
            r = c.execute(
                "SELECT flush_lsn::text, sync_state, pid, ('x'||substr(pg_walfile_name(pg_current_wal_insert_lsn()),1,8))::bit(32)::int"
                " FROM (SELECT 1) one LEFT JOIN pg_stat_replication ON application_name = 's1'"
            ).fetchone()
            return {"flush_lsn": r[0], "sync_state": r[1], "walsender_pid": r[2], "tli": r[3], "error": None}
        except psycopg.Error as e:
            err = repr(e)
            local.conn = None
            time.sleep(0.2)
    return {"flush_lsn": None, "sync_state": None, "walsender_pid": None, "tli": None, "error": err}


def log(svc, rec):
    return jlog.append(R / "svc" / f"{svc}.jsonl", {"svc": svc, **rec})


def hold(svc):
    h = R / "ctl" / f"hold-{svc}"
    if not h.exists():
        return
    h.unlink()
    jlog.append(R / "markers.jsonl", {"event": "reached", "point": f"b2:{svc}", "data": {}})
    rel = R / "ctl" / f"release-b2:{svc}"
    while not rel.exists():
        time.sleep(0.002)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def body(self):
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n).decode()

    def reply(self, code, obj, headers=()):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        path = urlparse(self.path).path.strip("/")
        raw = self.body()
        if path == "stub":
            return self.stub(raw)
        if path in tools:
            return self.tool(path, json.loads(raw))
        self.reply(404, {})

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        path = u.path.strip("/")
        if path == "status":
            with lock:
                res = tools["T-I"].get(q["key"])
            log("T-I", {"kind": "status", "key": q["key"], "result": res})
            return self.reply(200, {"found": res is not None, "result": res})
        if path == "approve":
            answer = "deny" if SEED % 2 else "approve"
            jlog.append(R / "approver.jsonl", {"task": q.get("task"), "call_id": q.get("action"), "answer": answer})
            return self.reply(200, {"answer": answer})
        if path == "owner":
            answer = "happened" if SEED % 2 else "did_not_happen"
            jlog.append(R / "owner.jsonl", {"task": q.get("task"), "call_id": q.get("call_id"), "answer": answer})
            return self.reply(200, {"answer": answer})
        self.reply(404, {})

    def stub(self, raw):
        req = json.loads(raw)
        canon = jlog.canonical(req["body"])
        read = flush_read()
        rid = "req-" + uuid.uuid4().hex
        log(
            "stub",
            {
                "kind": "request",
                "request_id": rid,
                "invocation_id": req["invocation_id"],
                "retry_of": req.get("retry_of"),
                "agent": req["agent"],
                "epoch": req["epoch"],
                "body": canon,
                "body_sha256": hashlib.sha256(canon.encode()).hexdigest(),
                "read": read,
            },
        )
        hold("model")
        out = hashlib.sha256(f"{SEED}:{rid}:{hashlib.sha256(canon.encode()).hexdigest()}".encode()).hexdigest()
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("X-Request-Id", rid)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        self.chunk({"request_id": rid})
        time.sleep(LATENCY)
        self.chunk({"output": out, "model": "stub-1"})
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()
        log(
            "stub",
            {
                "kind": "reply",
                "invocation_id": req["invocation_id"],
                "request_id": rid,
                "output_sha256": hashlib.sha256(out.encode()).hexdigest(),
            },
        )
        log("stub", {"kind": "delivered", "invocation_id": req["invocation_id"], "request_id": rid})

    def chunk(self, obj):
        b = (json.dumps(obj) + "\n").encode()
        self.wfile.write(f"{len(b):x}\r\n".encode() + b + b"\r\n")
        self.wfile.flush()

    def tool(self, svc, req):
        read = flush_read()
        log(
            svc,
            {
                "kind": "request",
                **{k: req.get(k) for k in ("call_id", "key", "attempt", "agent", "epoch", "args")},
                "read": read,
            },
        )
        hold(svc)
        ids = {k: req[k] for k in ("call_id", "key", "attempt", "epoch")}
        with lock:
            seen = tools[svc]
            if svc == "T-F":
                scope = (req["agent"], "T-F")
                if req["epoch"] < fence.get(scope, 0):
                    res = {"kind": "rejected_stale", "result": {"highest": fence[scope]}}
                    log(svc, {**res, **ids})
                    self.reply(409, res)
                    return log(svc, {"kind": "delivered", **ids})
                fence[scope] = max(fence.get(scope, 0), req["epoch"])
            if svc != "T-N" and req["key"] in seen:
                res = {"kind": "duplicate", "result": seen[req["key"]]}
            else:
                n = sum(1 for _ in seen) + 1
                result = {"effect": f"{svc}-{n}", "key": req["key"], "attempt": req["attempt"]}
                if svc == "T-N":
                    seen[f"{req['key']}#{n}"] = result
                else:
                    seen[req["key"]] = result
                res = {"kind": "effect", "result": result}
            log(svc, {**res, **ids})
        self.reply(200, res)
        self.wfile.flush()
        log(svc, {"kind": "delivered", **ids})


def main():
    (R / "svc").mkdir(exist_ok=True)
    (R / "ctl").mkdir(exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", RUN["svc_port"]), H)
    srv.daemon_threads = True
    (R / "ctl" / "svc-ready").write_text(str(os.getpid()))
    srv.serve_forever()


if __name__ == "__main__":
    main()
