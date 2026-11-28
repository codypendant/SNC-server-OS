import hashlib
import http.client
import json
import os
import select
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

import pytest

from tracka import jlog
from tracka.run import HERE, free_ports


@pytest.fixture
def svc(tmp_path):
    R = tmp_path / "R"
    (R / "ctl").mkdir(parents=True)
    port = free_ports()
    run = {
        "point": "A02",
        "seed": 1,
        "seed_string": "tracka:x:A02:1",
        "ports": {"svc": port},
    }
    (R / "run.json").write_text(json.dumps(run))
    r, w = os.pipe()
    p = subprocess.Popen(
        [sys.executable, "-m", "tracka.services"],
        cwd=HERE,
        env={**os.environ, "TRACKA_RUN": str(R), "TRACKA_NOTIFY": str(w)},
        pass_fds=(w,),
    )
    os.close(w)
    while not (R / "ctl" / "svc-ready").exists():
        time.sleep(0.01)
    s = type(
        "S", (), {"R": R, "url": f"http://127.0.0.1:{port}", "port": port, "pipe": r}
    )
    yield s
    p.kill()
    p.wait()


def call(s, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        s.url + path, data, {"Content-Type": "application/json"}, method=method
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def tool(s, svc, key="k", token=1, attempt=1, **kw):
    body = {
        "call_id": "c",
        "key": key,
        "attempt": attempt,
        "agent": "A",
        "task": "T",
        "token": token,
        "args": {},
    }
    code, text = call(s, "POST", f"/{svc}", {**body, **kw})
    return code, json.loads(text)


def log(s, svc):
    return jlog.read(s.R / "svc" / f"{svc}.jsonl")


def note(s, timeout=5):
    assert select.select([s.pipe], [], [], timeout)[0], "no notification"
    return json.loads(os.read(s.pipe, 4096).decode().splitlines()[0])


def arm(s, svc, phase, aid=1):
    (s.R / "ctl" / "arm").write_text(
        json.dumps({"id": aid, "svc": svc, "phase": phase})
    )


def test_stub_chunked_ndjson_and_log(svc):
    body = {"b": 1, "a": "é"}
    code, text = call(
        svc,
        "POST",
        "/stub",
        {
            "invocation_id": "i1",
            "retry_of": None,
            "agent": "A",
            "task": "T",
            "body": body,
        },
    )
    head, out = map(json.loads, text.splitlines())
    canon = jlog.canonical(body)
    bsha = hashlib.sha256(canon.encode()).hexdigest()
    assert code == 200 and out["model"] == "stub-1"
    assert (
        out["output"]
        == hashlib.sha256(
            f"tracka:x:A02:1:{head['request_id']}:{bsha}".encode()
        ).hexdigest()
    )
    req, rep, dlv = log(svc, "stub")
    assert (req["kind"], req["body"], req["body_sha256"], req["invocation_id"]) == (
        "request",
        canon,
        bsha,
        "i1",
    )
    assert rep["output_sha256"] == hashlib.sha256(out["output"].encode()).hexdigest()
    assert dlv == {"kind": "delivered", "request_id": head["request_id"], "t": dlv["t"]}
    _, text2 = call(
        svc,
        "POST",
        "/stub",
        {
            "invocation_id": "i2",
            "retry_of": "i1",
            "agent": "A",
            "task": "T",
            "body": body,
        },
    )
    assert json.loads(text2.splitlines()[0])["request_id"] != head["request_id"]


def test_every_line_has_t(svc):
    tool(svc, "T-I")
    call(svc, "GET", "/owner?task=T&call_id=c")
    assert all(
        isinstance(x["t"], int)
        for f in (svc.R / "svc").glob("*.jsonl")
        for x in jlog.read(f)
    )


def test_ti_dedupe_and_status(svc):
    assert call(svc, "GET", "/status?key=k") == (200, '{"found": false}')
    _, a = tool(svc, "T-I")
    _, b = tool(svc, "T-I", attempt=2)
    assert a == {"kind": "effect", "result": {"effect": "T-I-1"}} and b == {
        "kind": "duplicate",
        "result": a["result"],
    }
    assert json.loads(call(svc, "GET", "/status?key=k")[1]) == {
        "found": True,
        "result": a["result"],
    }
    assert [x["kind"] for x in log(svc, "T-I")][:6] == [
        "status",
        "request",
        "effect",
        "delivered",
        "request",
        "duplicate",
    ]


def test_tf_stale_before_dedupe_and_epoch(svc):
    assert tool(svc, "T-F", token=2)[0] == 200
    code, r = tool(svc, "T-F", token=1)
    assert code == 409 and r == {"kind": "rejected_stale", "result": {"highest": 2}}
    assert (
        tool(svc, "T-F", key="k2", token=None, epoch=1)[1]["kind"] == "rejected_stale"
    )
    assert tool(svc, "T-F", key="k2", token=None, epoch=3)[1] == {
        "kind": "effect",
        "result": {"effect": "T-F-2"},
    }
    assert tool(svc, "T-F", token=3)[1]["kind"] == "duplicate"
    assert [x["token"] for x in log(svc, "T-F") if x["kind"] == "request"] == [
        2,
        1,
        1,
        3,
        3,
    ]
    assert tool(svc, "T-F", token=None)[0] == 400


def test_tn_always_new_effect(svc):
    assert [tool(svc, "T-N")[1]["result"]["effect"] for _ in range(3)] == [
        "T-N-1",
        "T-N-2",
        "T-N-3",
    ]


def test_owner_truthful_and_approval(svc):
    owner = lambda: json.loads(call(svc, "GET", "/owner?task=T&call_id=c")[1])["answer"]
    assert owner() == "did_not_happen"
    tool(svc, "T-I")
    assert owner() == "did_not_happen"
    tool(svc, "T-N")
    assert owner() == "happened"
    assert [x["answer"] for x in log(svc, "owner")] == ["did_not_happen"] * 2 + [
        "happened"
    ]
    assert call(svc, "POST", "/approval", {"task": "T", "action": "T-I,T-F,T-N"}) == (
        202,
        '{"status": "pending"}',
    )
    assert note(svc) == {"event": "approval", "task": "T", "action": "T-I,T-F,T-N"}
    assert [x["kind"] for x in log(svc, "approver")] == ["request", "delivered"]


def test_hold_at_receipt_then_dead_socket(svc):
    arm(svc, "T-I", "receipt", 7)
    c = socket.create_connection(("127.0.0.1", svc.port))
    b = json.dumps(
        {
            "call_id": "c",
            "key": "k",
            "attempt": 1,
            "agent": "A",
            "task": "T",
            "token": 1,
            "args": {},
        }
    )
    c.sendall(
        f"POST /T-I HTTP/1.1\r\nHost: x\r\nContent-Length: {len(b)}\r\n\r\n{b}".encode()
    )
    n = note(svc)
    assert (n["event"], n["id"], n["svc"], n["phase"]) == (
        "reached",
        7,
        "T-I",
        "receipt",
    )
    assert jlog.read(svc.R / "markers.jsonl")[0]["data"] == {
        "point": "A02",
        "svc": "T-I",
        "phase": "receipt",
    }
    time.sleep(0.1)
    assert [x["kind"] for x in log(svc, "T-I")] == ["request"]
    c.close()
    (svc.R / "ctl" / "release-7").touch()
    while not (svc.R / "ctl" / "done-7").exists():
        time.sleep(0.01)
    assert [x["kind"] for x in log(svc, "T-I")] == ["request", "effect"]
    assert tool(svc, "T-I", attempt=2)[1]["kind"] == "duplicate"


def test_only_first_request_triggers(svc):
    arm(svc, "T-N", "after_reply")
    tool(svc, "T-N")
    tool(svc, "T-N")
    assert note(svc)["phase"] == "after_reply"
    assert not select.select([svc.pipe], [], [], 0.2)[0]
    assert [x["event"] for x in jlog.read(svc.R / "markers.jsonl")] == ["reached"]


def test_after_reply_signals_after_delivered(svc):
    arm(svc, "stub", "after_reply")
    call(
        svc,
        "POST",
        "/stub",
        {"invocation_id": "i", "agent": "A", "task": "T", "body": 1},
    )
    assert note(svc)["svc"] == "model"
    m = jlog.read(svc.R / "markers.jsonl")[0]
    assert (
        m["t"] > log(svc, "stub")[-1]["t"]
        and log(svc, "stub")[-1]["kind"] == "delivered"
    )


def test_any_arm_holds_get_and_approval_arm(svc):
    arm(svc, "any", "receipt", 3)
    c = http.client.HTTPConnection("127.0.0.1", svc.port, timeout=10)
    c.request("GET", "/status?key=z")
    assert note(svc)["svc"] == "T-I"
    (svc.R / "ctl" / "release-3").touch()
    assert json.loads(c.getresponse().read()) == {"found": False}
    arm(svc, "approver", "approval", 4)
    c.request("POST", "/approval", json.dumps({"task": "T", "action": "a"}))
    n = note(svc)
    assert (n["svc"], n["req"]) == ("approver", {"task": "T", "action": "a"})
    (svc.R / "ctl" / "release-4").touch()
    assert c.getresponse().status == 202


def test_partial_log_line_is_ignored_by_reader(tmp_path):
    p = tmp_path / "x.jsonl"
    jlog.append(p, {"a": 1})
    with open(p, "a") as f:
        f.write('{"a": ')
    assert [x["a"] for x in jlog.read(p)] == [1]


def test_extra_fields_logged(svc):
    call(
        svc,
        "POST",
        "/approval",
        {"task": "T", "action": "a", "awakeable": "w-1", "t": 5},
    )
    req = log(svc, "approver")[0]
    assert req["awakeable"] == "w-1" and req["t"] != 5
    tool(svc, "T-N", key="ignored", extra=1)
    assert log(svc, "T-N")[0]["extra"] == 1
