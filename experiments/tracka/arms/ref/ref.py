import fcntl
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
from contextlib import contextmanager
from pathlib import Path

R = Path(os.environ["TRACKA_RUN"])
D = R / "ref"
SVC = os.environ["TRACKA_SVC"]
AGENT, TASK, SCENARIO = (
    os.environ["TRACKA_AGENT"],
    os.environ["TRACKA_TASK"],
    os.environ["TRACKA_SCENARIO"],
)
POLICY = json.loads((Path(__file__).resolve().parents[2] / "policy.json").read_text())
ACTION = "T-I,T-F,T-N"
TOOLS = ("T-I", "T-F", "T-N")


def write(path, value):
    tmp = path.with_suffix(".tmp")
    with open(tmp, "w") as f:
        json.dump(value, f)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, path)
    fd = os.open(path.parent, os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)


def load():
    try:
        return json.loads((D / "state.json").read_text())
    except FileNotFoundError:
        return {
            "owner": 0,
            "submitted": False,
            "status": "running",
            "model": [],
            "reply": None,
            "policy": [],
            "calls": {},
        }


@contextmanager
def locked():
    with open(D / "lock", "w") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        st = load()
        yield st
        write(D / "state.json", st)


def http(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        SVC + path, data, {"Content-Type": "application/json"}, method=method
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def args_for(st, tool):
    eff = {
        t: st["calls"][t]["outcome"]["result"]["effect"]
        for t in TOOLS
        if st["calls"].get(t, {}).get("outcome")
    }
    return {
        "T-I": lambda: {"reply": st["reply"]},
        "T-F": lambda: {"reply": st["reply"], "ti": eff["T-I"]},
    }.get(tool, lambda: {"reply": st["reply"], "tf": eff["T-F"]})()


def model(me):
    with locked() as st:
        if st["owner"] != me:
            return
        n = len(st["model"])
        body = {"agent": AGENT, "task": TASK, "prompt": "plan T-I, T-F, T-N"}
        retry = st["model"][-1]["invocation_id"] if n else None
        st["model"].append(
            {
                "invocation_id": f"inv-{n + 1}",
                "request_id": None,
                "input": body,
                "output_sha256": None,
                "used": False,
            }
        )
    _, raw = http(
        "POST",
        "/stub",
        {
            "invocation_id": f"inv-{n + 1}",
            "retry_of": retry,
            "agent": AGENT,
            "task": TASK,
            "body": body,
        },
    )
    head, out = [json.loads(x) for x in raw.decode().splitlines()]
    with locked() as st:
        m = st["model"][n]
        m["request_id"], m["output_sha256"] = (
            head["request_id"],
            hashlib.sha256(out["output"].encode()).hexdigest(),
        )
        if st["owner"] == me and st["reply"] is None:
            m["used"], st["reply"] = True, head["request_id"]


def approval(st):
    try:
        d = json.loads((D / "decision.json").read_text())
    except FileNotFoundError:
        if st.get("asked"):
            return
        st["asked"] = True
        return "ask"
    st["policy"][0]["approval"] = {"decision": d["decision"]}
    if d["decision"] == "deny":
        st["memory"], st["status"] = {"reply": st["reply"], "denied": True}, "denied"


def tool(me, st, t):
    c = st["calls"].setdefault(
        t,
        {
            "tool": t,
            "call_id": f"{t}:{TASK}",
            "key": f"{t}:{TASK}",
            "token": me,
            "args": args_for(st, t),
            "attempts": [],
            "outcome": None,
        },
    )
    last = c["attempts"][-1] if c["attempts"] else None
    if last and last["outcome"] == "unknown" and t == "T-I" and not last.get("checked"):
        last["checked"] = True
        return "status"
    if last and last["outcome"] == "unknown" and t == "T-N":
        return "owner"
    c["token"] = me
    c["attempts"].append(
        {"attempt": len(c["attempts"]) + 1, "outcome": "unknown", "token": me}
    )
    return "send"


def finish(st):
    c = st["calls"]
    tn = c["T-N"]["outcome"]
    st["memory"] = {
        "reply": st["reply"],
        "ti": c["T-I"]["outcome"]["result"]["effect"],
        "tf": c["T-F"]["outcome"]["result"]["effect"],
        "tn": tn["result"]["effect"] if tn["kind"] == "effect" else "owner:happened",
    }
    st["status"] = "done"


def step(me):
    with locked() as st:
        if st["owner"] != me:
            return None, None, None
        if not st["policy"]:
            need = POLICY["rules"][SCENARIO]["approval"]
            result = "needs_approval" if need else "allow"
            st["policy"].append(
                {
                    "rule_version": POLICY["version"],
                    "action": ACTION,
                    "result": result,
                    "approval": None,
                }
            )
        if (
            st["policy"][0]["result"] == "needs_approval"
            and not st["policy"][0]["approval"]
        ):
            return approval(st), None, None
        t = next(
            (t for t in TOOLS if not (st["calls"].get(t) or {}).get("outcome")), None
        )
        if not t:
            return finish(st), None, None
        return tool(me, st, t), t, json.loads(json.dumps(st["calls"][t]))


def worker():
    me = int(os.environ["TRACKA_WORKER"][1:])
    with locked() as st:
        st["owner"] = max(st["owner"], me)
    while True:
        st = load()
        try:
            if st["owner"] != me or not st["submitted"] or st["status"] != "running":
                time.sleep(0.05)
            elif st["reply"] is None:
                model(me)
            else:
                todo, t, c = step(me)
                if todo == "ask":
                    http("POST", "/approval", {"task": TASK, "action": ACTION})
                elif todo:
                    act(me, t, todo, c)
                else:
                    time.sleep(0.05)
        except (OSError, ValueError) as e:
            print("retry:", repr(e), flush=True)
            time.sleep(0.2)


def act(me, t, todo, c):
    n = len(c["attempts"])
    if todo == "status":
        ans = json.loads(http("GET", f"/status?key={c['key']}")[1])
        res = {"kind": "effect", "result": ans["result"]} if ans["found"] else None
        outcome = res and "effect"
    elif todo == "owner":
        outcome = (
            "owner:"
            + json.loads(http("GET", f"/owner?task={TASK}&call_id={c['call_id']}")[1])[
                "answer"
            ]
        )
        res = {"kind": outcome, "result": None} if outcome == "owner:happened" else None
    else:
        body = {k: c[k] for k in ("call_id", "key", "token", "args")} | {
            "attempt": n,
            "agent": AGENT,
            "task": TASK,
        }
        res = json.loads(http("POST", f"/{t}", body)[1])
        outcome = res["kind"]
    with locked() as st:
        cc = st["calls"][t]
        a = cc["attempts"][n - 1]
        if a["outcome"] == "unknown" and outcome:
            a["outcome"], a["result"] = outcome, res and res["result"]
        if (
            res
            and res["kind"] != "rejected_stale"
            and st["owner"] == me
            and not cc["outcome"]
        ):
            cc["outcome"] = res


def by_token(c):
    out = []
    for a in c["attempts"]:
        if not out or out[-1]["token"] != a["token"]:
            out.append({**c, "token": a["token"], "attempts": []})
        out[-1]["attempts"].append({"attempt": a["attempt"], "outcome": a["outcome"]})
        out[-1]["outcome"] = {"kind": a["outcome"], "result": a.get("result")}
    out[-1]["outcome"] = c["outcome"] or out[-1]["outcome"]
    return out


def main():
    cmd = sys.argv[1]
    D.mkdir(exist_ok=True)
    if cmd == "worker":
        worker()
    elif cmd == "submit":
        with locked() as st:
            st["submitted"] = True
    elif cmd == "deliver":
        write(
            D / "decision.json",
            {"task": sys.argv[2], "action": sys.argv[3], "decision": sys.argv[4]},
        )
    elif cmd == "state":
        st = load()
        print(
            json.dumps(
                {
                    "task": {"id": TASK, "status": st["status"]},
                    "memory": st.get("memory"),
                    "retention": "forever",
                }
            )
        )
    elif cmd == "reconstruct":
        st = load()
        calls = [e for t in TOOLS if t in st["calls"] for e in by_token(st["calls"][t])]
        print(
            json.dumps(
                {
                    "model": st["model"],
                    "policy": st["policy"],
                    "calls": calls,
                    "retention": "forever",
                }
            )
        )


if __name__ == "__main__":
    main()
