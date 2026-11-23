import hashlib
import json
from pathlib import Path

from check.wal import lsn_str

TOOL_PFX = {"T-I": "TI", "T-F": "TF", "T-N": "TN"}


class World:
    """A synthetic, internally consistent run: tokens, history, WAL records, receiver logs, cuts and answers."""

    def __init__(self, R, point="A/a2:TI-disp", arm="snc", scenario="S-M", mutation=None, **run):
        self.R = Path(R)
        (self.R / "svc").mkdir(parents=True, exist_ok=True)
        (self.R / "blobs").mkdir(exist_ok=True)
        self.run = {
            "point": point,
            "seed": 1,
            "arm": arm,
            "mutation": mutation,
            "scenario": scenario,
            "pg": {"primary_port": 1, "standby_port": 2},
            "app_name_prefix": "app",
            **run,
        }
        self.lsn, self.xid, self.t = 0x3000028, 700, 1_000_000_000
        self.flushed = self.lsn
        self.sync = True
        self.wal, self.tables = [], {
            k: []
            for k in (
                "commit_token",
                "agent",
                "task",
                "memory",
                "invocation",
                "tool_call",
                "attempt",
                "policy_decision",
                "approval",
                "run_event",
                "history",
            )
        }
        self.tables["deadlocks"] = 0
        self.logs = {
            k: [] for k in ("markers", "approver", "owner", "gateway", "cuts", "answers", "stub", "T-I", "T-F", "T-N")
        }
        self.epoch = 1
        self.tables["agent"].append({"id": "A", "epoch": 1, "version": 1})
        self.tables["task"].append({"id": "T", "agent": "A", "status": "running", "version": 1})

    # primitives

    def tick(self, ms=1):
        self.t += int(ms * 1e6)
        return self.t

    def row(self, table, **kw):
        return next(r for r in self.tables[table] if all(r.get(k) == v for k, v in kw.items()))

    def commit(self, name, kind, epoch=None, moves=(), established=None):
        epoch = self.epoch if epoch is None else epoch
        self.xid += 1
        start = self.lsn
        self.wal.append([start, start + 40, None, "Transaction", self.xid, "COMMIT"])
        self.lsn = start + 40
        tok = f"tok-{name}-{epoch}-{self.xid}"
        self.tables["commit_token"].append(
            {"token": tok, "xid": str(self.xid), "epoch": epoch, "kind": kind, "name": name, "agent": "A"}
        )
        for table, oid, to in moves:
            r = self.row(table, id=oid)
            self.tables["history"].append(
                {
                    "id": len(self.tables["history"]) + 1,
                    "kind": table,
                    "obj": oid,
                    "from_state": r.get("state"),
                    "to_state": to,
                    "epoch": epoch,
                    "token": tok,
                }
            )
            r["state"] = to
            r["token"] = tok
        if self.sync if established is None else established:
            self.flushed = self.lsn
        self.tick()
        return tok

    def end_of(self, tok):
        x = int(self.row("commit_token", token=tok)["xid"])
        return next(r[1] for r in self.wal if r[4] == x)

    def log(self, name, **e):
        e.setdefault("t", self.tick())
        self.logs[name].append(e)
        return e

    def marker(self, event, point=None, **data):
        return self.log("markers", event=event, point=point, data=data)

    def read(self, flush=None):
        return {
            "flush_lsn": lsn_str(self.flushed if flush is None else flush),
            "sync_state": "sync",
            "walsender_pid": 11,
            "tli": 1,
            "error": None,
        }

    def cut(self, kind, label):
        c = {
            "kind": kind,
            "label": label,
            "unresolved": [],
            "invalid": [],
            "t": self.tick(),
            "standby": {
                "source": "walreceiver",
                "walreceiver_pid": 10,
                "flushed_lsn": lsn_str(self.flushed),
                "received_tli": 1,
            },
            "primary": {"flush_lsn": lsn_str(self.flushed), "sync_state": "sync", "walsender_pid": 11},
            "primary_tli": 1,
            "insert_lsn": lsn_str(self.lsn),
        }
        if kind == "takeover":
            c["kill_boundary"] = lsn_str(self.lsn)
        self.logs["cuts"].append(c)
        return c

    def kill(self, label="k1"):
        self.cut("kill", label)
        self.marker("killed")
        self.logs["cuts"].append(
            {
                "kind": "kill_boundary",
                "label": label,
                "invalid": [],
                "t": self.tick(),
                "kill_boundary": lsn_str(self.lsn),
            }
        )

    def resume(self):
        self.sync = True
        self.flushed = self.lsn

    def gw(self, **k):
        self.log(
            "gateway",
            bound=lsn_str(self.lsn),
            tli=1,
            flush_lsn=lsn_str(self.flushed),
            sync_state="sync",
            decision="established" if self.flushed >= self.lsn else "waiting",
            **k,
        )

    # scenario steps

    def accept(self):
        return self.commit("C-acc", "decision", moves=[("task", "T", "running")])

    def model(self, iid="i1", crash=None, retry_of=None, after_reached=None):
        blob = b"memory item"
        h = hashlib.sha256(blob).hexdigest()
        (self.R / "blobs" / h).write_bytes(blob)
        body = json.dumps({"input": "x", "invocation": iid}, sort_keys=True)
        bh = hashlib.sha256(body.encode()).hexdigest()
        self.tables["invocation"].append(
            {
                "id": iid,
                "agent": "A",
                "task": "T",
                "epoch": self.epoch,
                "state": None,
                "manifest": [{"kind": "memory", "id": "m0", "version": 1, "sha256": h}],
                "whole_hash": bh,
                "provider_request_id": None,
                "output": None,
                "late": False,
                "retry_of": retry_of,
                "token": None,
            }
        )
        self.commit("I-prep", "decision", moves=[("invocation", iid, "prepared")])
        self.commit("I-disp", "decision", moves=[("invocation", iid, "dispatched")])
        self.gw(invocation_id=iid, attempt=1)
        if crash == "b1":
            return self.crash_point("b1:model")
        self.log(
            "stub",
            svc="stub",
            kind="request",
            invocation_id=iid,
            retry_of=retry_of,
            agent="A",
            epoch=self.epoch,
            body=body,
            body_sha256=bh,
            request_id=f"req-{iid}",
            read=self.read(),
        )
        if crash == "b2":
            return self.crash_point("b2:model")
        out = f"output-{iid}"
        self.log(
            "stub",
            svc="stub",
            kind="reply",
            invocation_id=iid,
            request_id=f"req-{iid}",
            output_sha256=hashlib.sha256(out.encode()).hexdigest(),
        )
        if crash == "b3":
            return self.crash_point("b3:model")
        r = self.row("invocation", id=iid)
        r["provider_request_id"] = f"req-{iid}"
        self.commit("I-acc", "outcome", moves=[("invocation", iid, "accepted")])
        r["output"] = out
        self.commit("I-out", "outcome", moves=[("invocation", iid, "output")])

    def crash_point(self, point):
        self.marker("reached", point)
        self.kill()
        return "crashed"

    def policy(self, call="c-T-I"):
        self.tables["policy_decision"].append(
            {
                "id": f"p-{call}",
                "task": "T",
                "call_id": call,
                "rule_version": "v1",
                "inputs_hash": "h",
                "result": "allow",
                "obligations": None,
                "epoch": self.epoch,
                "token": None,
            }
        )
        self.commit("P-dec", "decision")

    def prep(self, tool, call=None, key=None):
        call = call or f"c-{tool}"
        self.tables["tool_call"].append(
            {
                "id": call,
                "agent": "A",
                "task": "T",
                "tool": tool,
                "key": key or f"k-{call}",
                "epoch": self.epoch,
                "state": None,
                "args_hash": "a",
                "attempts": 0,
                "result": None,
                "policy_id": "p-c-T-I",
                "approval_id": None,
                "token": None,
            }
        )
        self.commit(f"{TOOL_PFX[tool]}-prep", "decision", moves=[("tool_call", call, "prepared")])
        return call

    def dispatch(self, tool, call=None, n=1, established=None):
        call = call or f"c-{tool}"
        tok = self.commit(
            f"{TOOL_PFX[tool]}-disp", "decision", moves=[("tool_call", call, "dispatched")], established=established
        )
        self.tables["attempt"].append({"call_id": call, "n": n, "epoch": self.epoch, "token": tok})
        self.row("tool_call", id=call)["attempts"] = n
        self.gw(call_id=call, attempt=n)
        return tok

    def send(self, tool, call=None, n=1, key=None, epoch=None, read=None, kind="effect"):
        call = call or f"c-{tool}"
        key = key or self.row("tool_call", id=call)["key"]
        e = self.log(
            tool,
            svc=tool,
            kind="request",
            call_id=call,
            key=key,
            attempt=n,
            agent="A",
            epoch=self.epoch if epoch is None else epoch,
            args={},
            read=read or self.read(),
        )
        self.log(tool, svc=tool, kind=kind, call_id=call, key=key, attempt=n, result="ok")
        return e

    def outcome(self, tool, call=None, epoch=None, to="completed"):
        call = call or f"c-{tool}"
        self.row("tool_call", id=call)["result"] = "ok"
        return self.commit(f"{TOOL_PFX[tool]}-out", "outcome", epoch=epoch, moves=[("tool_call", call, to)])

    def derive(self, obj):
        tok = self.commit("DERIVE", "decision")
        self.tables["history"].append(
            {
                "id": len(self.tables["history"]) + 1,
                "kind": "derive",
                "obj": obj,
                "from_state": None,
                "to_state": "derived",
                "epoch": self.epoch,
                "token": tok,
            }
        )

    def tool(self, tool, prep=True):
        if prep:
            self.prep(tool)
        self.dispatch(tool)
        self.send(tool)
        self.outcome(tool)

    def done(self):
        self.tables["memory"].append({"id": "m1", "version": 1, "agent": "A", "content_hash": "c", "token": None})
        self.commit("M-done", "decision", moves=[("task", "T", "completed")])

    def takeover(self, outstanding=None, reject=True):
        self.cut("takeover", f"tk{self.epoch}")
        self.epoch += 1
        self.row("agent", id="A")["epoch"] = self.epoch
        moves, out = [], []
        for c in self.tables["tool_call"] + self.tables["invocation"]:
            tbl = "tool_call" if c in self.tables["tool_call"] else "invocation"
            if c["state"] == "prepared" and reject:
                moves.append((tbl, c["id"], "rejected" if tbl == "tool_call" else "abandoned"))
            if c["state"] in ("dispatched", "accepted"):
                out.append(c["id"])
        tok = self.commit("TAKEOVER", "takeover", moves=moves)
        data = {oid: to for _, oid, to in moves} | {
            o: "outstanding" for o in (out if outstanding is None else outstanding)
        }
        self.row("commit_token", token=tok)["data"] = data
        self.tables["run_event"].append(
            {
                "id": len(self.tables["run_event"]) + 1,
                "agent": "A",
                "kind": "takeover",
                "epoch": self.epoch,
                "token": tok,
                "outstanding": out if outstanding is None else outstanding,
            }
        )
        self.marker("takeover-committed")
        return tok

    # answers

    def visible(self):
        keys = ("token", "xid", "epoch", "kind", "name", "data")
        return [{k: r[k] for k in keys if k in r} for r in self.tables["commit_token"]]

    def state(self, commits=None):
        a = {
            "agent": dict(self.row("agent", id="A")),
            "task": {
                "id": "T",
                "status": self.row("task", id="T")["state"] if "state" in self.row("task", id="T") else "running",
            },
            "memory": [],
            "calls": {
                c["id"]: {"state": c["state"], "attempts": c["attempts"], "result": c["result"]}
                for c in self.tables["tool_call"]
            },
            "invocations": {
                i["id"]: {"state": i["state"], "late": i["late"], "retry_of": i["retry_of"]}
                for i in self.tables["invocation"]
            },
            "commits": self.visible() if commits is None else commits,
        }
        return self.log("answers", call="state", arg="A", answer=a)

    def observe(self, commits=None):
        self.state(commits)
        self.cut("cut", "obs")

    def reconstruct_all(self, drop_attempts=()):
        toks = {r["token"]: r for r in self.tables["commit_token"]}
        for table in ("tool_call", "invocation", "memory"):
            for r in self.tables[table]:
                hs = [h for h in self.tables["history"] if h["obj"] == r["id"]]
                inv = r if table == "invocation" else (self.tables["invocation"] or [None])[0]
                shown = (
                    {"items": inv["manifest"], "whole_hash": inv["whole_hash"]}
                    if inv
                    else {"items": [], "whole_hash": None}
                )
                att = [
                    {"n": a["n"]}
                    for a in self.tables["attempt"]
                    if a["call_id"] == r["id"] and r["id"] not in drop_attempts
                ]
                ans = {
                    "shown": shown,
                    "authorized": {"rule_version": "v1", "approval": None},
                    "executed": {
                        "attempts": att,
                        "states": [h["to_state"] for h in hs if h["kind"] != "derive"],
                        "provider_request_id": r.get("provider_request_id"),
                        "output_sha256": r.get("output") and hashlib.sha256(r["output"].encode()).hexdigest(),
                    },
                    "committed": {"transitions": [{"token": h["token"], "to": h["to_state"]} for h in hs]},
                    "commits": [
                        {k: toks[h["token"]][k] for k in ("token", "xid", "epoch", "kind", "name")} for h in hs
                    ],
                }
                self.log("answers", call="reconstruct", arg=r["id"], answer=ans)

    def final(self):
        if not any(a["call"] == "reconstruct" for a in self.logs["answers"]):
            self.reconstruct_all()
        self.resume()
        self.cut("final", "final")

    def write(self):
        (self.R / "run.json").write_text(json.dumps(self.run))
        paths = {
            "markers": "markers.jsonl",
            "approver": "approver.jsonl",
            "owner": "owner.jsonl",
            "gateway": "gateway.jsonl",
            "cuts": "cuts.jsonl",
            "answers": "answers.jsonl",
        }
        for k, v in self.logs.items():
            p = self.R / (paths[k] if k in paths else f"svc/{k}.jsonl")
            p.write_text("".join(json.dumps(x) + "\n" for x in v))
        recs = [list(r) for r in self.wal]
        for a, b in zip(recs, recs[1:]):
            a[2] = b[0]
        return self.tables, {1: [tuple(r) for r in recs]}


def happy(R, kill_after=None, **kw):
    """A correct S-M run; kill_after names a commit after which the app is killed and recovers."""
    w = World(R, **kw)
    seq = (
        [("C-acc", w.accept), ("model", w.model), ("P-dec", w.policy)]
        + [(t, (lambda t=t: w.tool(t))) for t in ("T-I", "T-F", "T-N")]
        + [("M-done", w.done)]
    )
    for name, f in seq:
        f()
        if name == kill_after:
            w.marker("reached", w.run["point"].split("/")[-1])
            w.kill()
            w.observe()
    if kill_after is None:
        w.marker("reached", w.run["point"].split("/")[-1])
        w.observe()
    w.final()
    return w
