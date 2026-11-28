import hashlib
import json
import os
import socket
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from . import jlog

R = Path(os.environ.get("TRACKA_RUN", "."))
RUN = json.loads((R / "run.json").read_text()) if (R / "run.json").exists() else {}
NOTIFY = int(os.environ.get("TRACKA_NOTIFY", "-1"))
lock = threading.Lock()
seen = {"T-I": {}, "T-F": {}}
count = {"T-I": 0, "T-F": 0, "T-N": 0}
highest = {}
tn_done = set()


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


def extra(req):
    return {k: v for k, v in req.items() if k not in ("t", "kind")}


def log(svc, rec):
    jlog.append(R / "svc" / f"{svc}.jsonl", rec)


def notify(msg):
    try:
        os.write(NOTIFY, (json.dumps(msg) + "\n").encode())
    except OSError:
        pass


def armed(svc, post):
    p = R / "ctl" / "arm"
    with lock:
        try:
            a = json.loads(p.read_text())
        except FileNotFoundError:
            return None
        if a["svc"] == "any" or (post and a["svc"] == svc):
            p.unlink()
            return a


def reach(a, svc, req):
    data = {
        "point": RUN["point"],
        "svc": "model" if svc == "stub" else svc,
        "phase": a["phase"],
    }
    rec = {"t": jlog.now(), "event": "reached", "data": data}
    fd = os.open(R / "markers.jsonl", os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    os.write(fd, (json.dumps(rec, sort_keys=True) + "\n").encode())
    # notify before fsync: the harness's after-reply delay starts here
    notify({"event": "reached", "id": a["id"], "req": req, **data})
    os.fsync(fd)
    os.close(fd)


def hold(a, svc, req=None):
    if a and a["phase"] != "after_reply":
        reach(a, svc, req)
        rel = R / "ctl" / f"release-{a['id']}"
        while not rel.exists():
            time.sleep(0.002)


def finish(a, svc):
    if a:
        if a["phase"] == "after_reply":
            reach(a, svc, None)
        (R / "ctl" / f"done-{a['id']}").touch()


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def alive(self):
        try:
            return self.connection.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT) != b""
        except BlockingIOError:
            return True
        except OSError:
            return False

    def send(self, code, chunks, ctype="application/json"):
        if not self.alive():
            self.close_connection = True
            return False
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            if ctype == "application/json":
                body = json.dumps(chunks[0]).encode()
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for c in chunks:
                    b = (json.dumps(c) + "\n").encode()
                    self.wfile.write(f"{len(b):x}\r\n".encode() + b + b"\r\n")
                    self.wfile.flush()
                self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            return True
        except OSError:
            self.close_connection = True
            return False

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            req = json.loads(
                self.rfile.read(int(self.headers.get("Content-Length", 0)))
            )
        except ValueError:
            req = None
        if not isinstance(req, dict):
            return self.send(400, [{"error": "bad json"}])
        if path == "/stub":
            return self.stub(req)
        if path in ("/T-I", "/T-F", "/T-N"):
            return self.tool(path[1:], req)
        if path == "/approval":
            return self.approval(req)
        self.send(404, [{"error": "not found"}])

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/status":
            a = armed("T-I", False)
            with lock:
                res = seen["T-I"].get(q.get("key"))
            ans = {"found": True, "result": res} if res else {"found": False}
            log("T-I", {"kind": "status", "key": q.get("key"), **ans})
            hold(a, "T-I")
            self.send(200, [ans])
            return finish(a, "T-I")
        if u.path == "/owner":
            a = armed("owner", False)
            with lock:
                ans = "happened" if q.get("call_id") in tn_done else "did_not_happen"
            log(
                "owner",
                {
                    "kind": "answer",
                    "task": q.get("task"),
                    "call_id": q.get("call_id"),
                    "answer": ans,
                },
            )
            hold(a, "owner")
            self.send(200, [{"answer": ans}])
            return finish(a, "owner")
        self.send(404, [{"error": "not found"}])

    def stub(self, req):
        a = armed("stub", True)
        body = jlog.canonical(req.get("body"))
        rid = "rid-" + uuid.uuid4().hex
        ids = {k: req.get(k) for k in ("invocation_id", "retry_of", "agent", "task")}
        log(
            "stub",
            {
                **extra(req),
                "kind": "request",
                "request_id": rid,
                **ids,
                "body": body,
                "body_sha256": sha(body),
            },
        )
        hold(a, "stub")
        out = sha(f"{RUN['seed_string']}:{rid}:{sha(body)}")
        log("stub", {"kind": "reply", "request_id": rid, "output_sha256": sha(out)})
        if self.send(
            200,
            [{"request_id": rid}, {"output": out, "model": "stub-1"}],
            "application/x-ndjson",
        ):
            log("stub", {"kind": "delivered", "request_id": rid})
        finish(a, "stub")

    def tool(self, svc, req):
        a = armed(svc, True)
        token = req["token"] if req.get("token") is not None else req.get("epoch")
        ids = {k: req.get(k) for k in ("call_id", "key", "attempt")}
        agent, key = req.get("agent"), req.get("key")
        log(
            svc,
            {
                **extra(req),
                "kind": "request",
                **ids,
                "agent": agent,
                "task": req.get("task"),
                "token": token,
                "args": req.get("args"),
            },
        )
        hold(a, svc)
        code = 200
        with lock:
            if svc == "T-F" and not isinstance(token, int):
                code, res = 400, {
                    "kind": "bad_request",
                    "result": {"error": "T-F needs an integer token"},
                }
            elif svc == "T-F" and token < highest.get(agent, token):
                code, res = 409, {
                    "kind": "rejected_stale",
                    "result": {"highest": highest[agent]},
                }
            else:
                if svc == "T-F":
                    highest[agent] = max(highest.get(agent, token), token)
                if svc != "T-N" and key is not None and key in seen[svc]:
                    res = {"kind": "duplicate", "result": seen[svc][key]}
                else:
                    count[svc] += 1
                    res = {
                        "kind": "effect",
                        "result": {"effect": f"{svc}-{count[svc]}"},
                    }
                    if svc != "T-N" and key is not None:
                        seen[svc][key] = res["result"]
                    if svc == "T-N":
                        tn_done.add(req.get("call_id"))
            log(svc, {**res, **ids, "token": token})
        if self.send(code, [res]):
            log(svc, {"kind": "delivered", **ids})
        finish(a, svc)

    def approval(self, req):
        a = armed("approver", True)
        rec = {"task": req.get("task"), "action": req.get("action")}
        log("approver", {**extra(req), "kind": "request", **rec})
        if a:
            hold(a, "approver", rec)
        else:
            notify({"event": "approval", **rec})
        if self.send(202, [{"status": "pending"}]):
            log("approver", {"kind": "delivered", **rec})
        finish(a, "approver")


def main():
    (R / "svc").mkdir(exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", RUN["ports"]["svc"]), H)
    srv.daemon_threads = True
    (R / "ctl" / "svc-ready").write_text(str(os.getpid()))
    srv.serve_forever()


if __name__ == "__main__":
    main()
