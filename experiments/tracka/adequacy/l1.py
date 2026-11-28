import copy
import json
import shutil
import sys
from pathlib import Path

from .common import BASE, RUNS, check, load, log, read, save, write

FILES = ["markers.jsonl", "answers.jsonl"] + [
    f"svc/{s}.jsonl" for s in ("stub", "T-I", "T-F", "T-N", "approver", "owner")
]
TOOLS = ("T-I", "T-F", "T-N")


class Skip(Exception):
    pass


class Run:
    def __init__(self, d):
        self.run = json.loads((d / "run.json").read_text())
        self.f = {k: read(d / k) for k in FILES if (d / k).exists()}
        self.state = self.answer("state")
        self.recon = self.answer("reconstruct")

    def answer(self, call):
        rows = [r for r in self.f["answers.jsonl"] if r["call"] == call]
        return rows[-1]["answer"] if rows else None

    def svc(self, s):
        return self.f.setdefault(f"svc/{s}.jsonl", [])

    def reqs(self, s):
        return [x for x in self.svc(s) if x["kind"] == "request"]

    def tool_reqs(self):
        return sorted((x for s in TOOLS for x in self.reqs(s)), key=lambda x: x["t"])

    def put(self, s, rec):
        path = s if s.endswith(".jsonl") else f"svc/{s}.jsonl"
        lines = self.f.setdefault(path, [])
        assert all(x["t"] != rec["t"] for x in lines)
        lines.append(rec)
        lines.sort(key=lambda x: x["t"])
        return rec

    def drop(self, s, pred):
        path = s if s.endswith(".jsonl") else f"svc/{s}.jsonl"
        n = len(self.f.get(path, []))
        self.f[path] = [x for x in self.f.get(path, []) if not pred(x)]
        return n - len(self.f[path])

    def tool_call(self, s, t, req, kind="effect", result=None, delivered=True):
        ids = {k: req[k] for k in ("call_id", "key", "attempt")}
        self.put(s, dict(req, t=t, kind="request"))
        self.put(
            s,
            {
                "t": t + 1000,
                "kind": kind,
                **ids,
                "token": req.get("token"),
                "result": result,
            },
        )
        if delivered:
            self.put(s, {"t": t + 2000, "kind": "delivered", **ids})

    def effects(self, s):
        return [x for x in self.svc(s) if x["kind"] == "effect"]

    def last_t(self, *files):
        return max(
            x["t"]
            for k in files
            for x in self.f.get(k if k.endswith(".jsonl") else f"svc/{k}.jsonl", [])
        )

    def deny(self):
        return self.run["scenario"] == "S-AP" and self.run["seed"] % 2

    def save(self, d):
        shutil.rmtree(d, ignore_errors=True)
        (d / "svc").mkdir(parents=True)
        (d / "run.json").write_text(json.dumps(self.run, indent=1))
        for k, v in self.f.items():
            write(d / k, v)


def need(cond):
    if not cond:
        raise Skip()


def non_deny(r):
    need(not r.deny() and r.state["task"]["status"] == "done")


def first_req(r, s):
    q = r.reqs(s)
    need(q)
    return {k: v for k, v in q[0].items() if k not in ("t", "kind")}


def ti_second_effect(r):
    non_deny(r)
    q = first_req(r, "T-I")
    r.tool_call(
        "T-I",
        r.last_t("T-I") + 1000,
        dict(q, attempt=q["attempt"] + 1),
        result={"effect": "T-I-2"},
    )


def ti_dup_to_effect(r):
    d = [x for x in r.svc("T-I") if x["kind"] == "duplicate"]
    need(d)
    d[0].update(kind="effect", result={"effect": "T-I-2"})


def ti_no_effect(r):
    non_deny(r)
    e = r.effects("T-I")
    need(len(e) == 1)
    e[0]["kind"] = "duplicate"


def tn_repeat(r, n=1, answer=None):
    non_deny(r)
    q = r.reqs("T-N")
    need(q)
    t = r.last_t("T-N", "owner") + 1000
    if answer:
        r.put(
            "owner",
            {
                "t": t,
                "kind": "answer",
                "task": "T",
                "call_id": q[-1]["call_id"],
                "answer": answer,
            },
        )
    base = {k: v for k, v in q[-1].items() if k not in ("t", "kind")}
    for i in range(n):
        t += 10000
        a = base["attempt"] + i + 1
        r.tool_call(
            "T-N",
            t,
            dict(base, attempt=a),
            result={"effect": f"T-N-{len(r.effects('T-N')) + 1}"},
        )


def tn_repeat_after_happened(r):
    tn_repeat(r, answer="happened")


def tn_dnh_before_prev(r):
    non_deny(r)
    q = r.reqs("T-N")
    need(len(q) == 1)
    r.put(
        "owner",
        {
            "t": q[0]["t"] - 1,
            "kind": "answer",
            "task": "T",
            "call_id": q[0]["call_id"],
            "answer": "did_not_happen",
        },
    )
    tn_repeat(r)


def tn_dnh_licenses(r, n):
    non_deny(r)
    q = r.reqs("T-N")
    need(len(q) == 1)
    e = [x for x in r.svc("T-N") if x["kind"] != "request"]
    need(e and e[0]["t"] - q[0]["t"] > 2)
    r.put(
        "owner",
        {
            "t": q[0]["t"] + 1,
            "kind": "answer",
            "task": "T",
            "call_id": q[0]["call_id"],
            "answer": "did_not_happen",
        },
    )
    tn_repeat(r, n)


def tf_stale(r, **edit):
    non_deny(r)
    q = first_req(r, "T-F")
    hi = max(x["token"] for x in r.reqs("T-F"))
    req = dict(q, attempt=q["attempt"] + 1, token=hi - 1)
    req.update(edit.pop("req", {}))
    kind = edit.pop("kind", "duplicate")
    res = r.effects("T-F")[0]["result"] if kind != "rejected_stale" else {"highest": hi}
    r.tool_call("T-F", r.last_t("T-F") + 1000, req, kind=kind, result=res)


def tf_stale_epoch(r):
    non_deny(r)
    q = first_req(r, "T-F")
    hi = max(x["token"] for x in r.reqs("T-F"))
    req = dict(q, attempt=q["attempt"] + 1, token=None, epoch=hi - 1)
    t = r.last_t("T-F") + 1000
    r.put("T-F", dict(req, t=t, kind="request"))
    ids = {k: req[k] for k in ("call_id", "key", "attempt")}
    r.put(
        "T-F",
        {
            "t": t + 1000,
            "kind": "duplicate",
            **ids,
            "token": hi - 1,
            "result": r.effects("T-F")[0]["result"],
        },
    )
    r.put("T-F", {"t": t + 2000, "kind": "delivered", **ids})


def tf_token_str(r):
    non_deny(r)
    for x in r.svc("T-F"):
        if x.get("token") is not None:
            x["token"] = str(x["token"])


def tool_after_deny(r, s="T-I"):
    need(r.deny())
    posted = [x["t"] for x in r.svc("approver") if x["kind"] == "posted"]
    req = {
        "call_id": f"T:{s}",
        "key": f"T:{s}",
        "attempt": 1,
        "agent": "A",
        "task": "T",
        "token": 1 if s == "T-F" else None,
        "args": {"reply": r.state["memory"]["reply"]},
    }
    r.tool_call(s, posted[-1] + 1000, req, result={"effect": f"{s}-1"})


def stub_call(r, t, delivered=True):
    q = r.reqs("stub")[0]
    rid = "rid-" + "f" * 32
    r.put("stub", dict(q, t=t, request_id=rid, retry_of=q["invocation_id"]))
    r.put(
        "stub",
        {"t": t + 1000, "kind": "reply", "request_id": rid, "output_sha256": "0" * 64},
    )
    if delivered:
        r.put("stub", {"t": t + 2000, "kind": "delivered", "request_id": rid})
    return rid


def model_recall_tight(r):
    q = r.tool_reqs()
    need(q)
    stub_call(r, q[0]["t"] + 1)


def model_recall_late(r):
    need(r.tool_reqs())
    stub_call(r, r.last_t("T-I", "T-F", "T-N") + 1000)


def tool_before_model(r):
    non_deny(r)
    s0 = r.reqs("stub")[0]["t"]
    ti = r.svc("T-I")
    for i, x in enumerate(ti):
        x["t"] = s0 - (len(ti) - i) * 1000


def outside(r, s, edit, kind="duplicate"):
    non_deny(r)
    q = first_req(r, s)
    req = copy.deepcopy(q)
    req["attempt"] += 1
    edit(req)
    res = r.effects(s)[0]["result"] if s != "T-N" else {"effect": "T-N-9"}
    r.tool_call(s, r.last_t(s) + 1000, req, kind=kind, result=res)


def tn_args_edit(r):
    non_deny(r)
    q = r.reqs("T-N")
    need(q)
    q[0]["args"]["tf"] = "T-F-9"


def one_reply_undelivered(r):
    need(r.tool_reqs())
    rid = r.tool_reqs()[0]["args"]["reply"]
    need(r.drop("stub", lambda x: x["kind"] == "delivered" and x["request_id"] == rid))


def one_reply_no_reply_line(r):
    need(r.tool_reqs())
    rid = r.tool_reqs()[0]["args"]["reply"]
    need(r.drop("stub", lambda x: x["kind"] == "reply" and x["request_id"] == rid))


def one_reply_two_rids(r):
    non_deny(r)
    q = r.reqs("T-F")
    need(q)
    s = r.svc("stub")
    rid = stub_call(r, s[-1]["t"] + 1000)
    need(rid and r.tool_reqs()[0]["t"] > s[-1]["t"])
    for x in q:
        x["args"]["reply"] = rid


def posted_late(r):
    need(r.run["scenario"] == "S-AP" and not r.deny())
    q = r.tool_reqs()
    need(q)
    t0 = q[0]["t"]
    p = [x for x in r.svc("approver") if x["kind"] == "posted" and x["t"] < t0]
    need(p)
    r.drop("approver", lambda x: x["kind"] == "posted" and x["t"] < t0)
    r.drop("markers.jsonl", lambda x: x["event"] == "posted" and x["t"] < t0)
    r.put("approver", dict(p[0], t=t0 + 1))
    m = {k: p[0][k] for k in ("task", "action", "decision", "exit")}
    r.put("markers.jsonl", {"t": t0 + 2, "event": "posted", "data": m})


def posted_exit(r):
    p = [x for x in r.svc("approver") if x["kind"] == "posted"]
    need(p)
    p[0]["exit"] = 1
    for m in r.f["markers.jsonl"]:
        if m["event"] == "posted":
            m["data"]["exit"] = 1
            break


def approval_action(r):
    q = r.reqs("approver")
    need(q)
    q[0]["action"] = "T-I"


def approval_other_task(r):
    q = r.reqs("approver")
    need(q)
    r.put("approver", dict(q[0], t=q[0]["t"] + 1, task="T2"))


def no_posted(r):
    need(r.reqs("approver"))
    r.drop("approver", lambda x: x["kind"] == "posted")
    r.drop("markers.jsonl", lambda x: x["event"] == "posted")


def posted_contradicts_seed(r):
    p = [x for x in r.svc("approver") if x["kind"] == "posted"]
    need(p)
    for x in p:
        x["decision"] = "approve" if x["decision"] == "deny" else "deny"
    for m in r.f["markers.jsonl"]:
        if m["event"] == "posted":
            m["data"]["decision"] = p[0]["decision"]


def takeover_late(r):
    res = [m for m in r.f["markers.jsonl"] if m["event"] == "resumed"]
    need(res)
    t0 = res[0]["t"]
    for k, lines in r.f.items():
        for x in lines:
            if x["t"] >= t0:
                x["t"] += 61 * 10**9


def owner_removed(r):
    need(r.state["memory"] and r.state["memory"].get("tn") == "owner:happened")
    need(r.drop("owner", lambda x: x["answer"] == "happened"))


def tn_not_delivered(r):
    non_deny(r)
    need(r.state["memory"]["tn"] != "owner:happened")
    need(r.drop("T-N", lambda x: x["kind"] == "delivered"))


def wrong_effect_id(r):
    non_deny(r)
    e = r.effects("T-F")
    need(len(e) == 1)
    e[0]["result"] = {"effect": "T-F-7"}


def tn_dnh_other_call(r):
    non_deny(r)
    q = r.reqs("T-N")
    need(len(q) == 1)
    t = r.last_t("T-N", "owner") + 1000
    cid = q[0]["call_id"] + ":2"
    r.put(
        "owner",
        {
            "t": t,
            "kind": "answer",
            "task": "T",
            "call_id": cid,
            "answer": "did_not_happen",
        },
    )
    base = {k: v for k, v in q[0].items() if k not in ("t", "kind")}
    req = dict(base, call_id=cid, key=cid, attempt=base["attempt"] + 1)
    r.tool_call("T-N", t + 10000, req, result={"effect": "T-N-2"})


def posted_other_task_early(r):
    need(r.run["scenario"] == "S-AP" and not r.deny())
    q = r.reqs("approver")
    need(q)
    p = [x for x in r.svc("approver") if x["kind"] == "posted"][0]
    t0 = q[0]["t"] + 1
    r.put("approver", dict(p, t=t0, task="T2"))
    posted_late(r)


def doubt(r, before):
    need(r.state["memory"] and r.state["memory"].get("tn") == "owner:happened")
    q = r.reqs("T-N")
    need(len(q) == 1)
    owner = r.svc("owner")
    need(owner)
    if before:
        a = dict(owner[0], t=q[0]["t"] - 1, answer="did_not_happen")
        r.drop("owner", lambda x: True)
        r.put("owner", a)
    for row in r.f["answers.jsonl"]:
        if row["call"] == "state":
            row["answer"]["task"]["status"] = "stopped_in_doubt"
            row["answer"]["memory"] = None
    for m in r.f["markers.jsonl"]:
        if m["event"] == "end-state":
            m["data"]["status"] = "stopped_in_doubt"


def deny_reply_undelivered(r):
    need(r.deny() and not r.tool_reqs())
    rid = r.state["memory"]["reply"]
    need(r.drop("stub", lambda x: x["kind"] == "delivered" and x["request_id"] == rid))


def unchanged(r):
    pass


FAULTS = [
    ("ti_second_effect", ti_second_effect, ["O-3.ti_once"]),
    ("ti_duplicate_logged_as_effect", ti_dup_to_effect, ["O-3.ti_once"]),
    ("ti_zero_effects", ti_no_effect, ["O-3.ti_once"]),
    ("tn_repeat_no_owner_answer", tn_repeat, ["O-3.tn_no_repeat"]),
    ("tn_repeat_after_owner_happened", tn_repeat_after_happened, ["O-3.tn_no_repeat"]),
    ("tn_repeat_dnh_before_previous_request", tn_dnh_before_prev, ["O-3.tn_no_repeat"]),
    ("tn_one_dnh_two_repeats", lambda r: tn_dnh_licenses(r, 2), ["O-3.tn_no_repeat"]),
    ("tf_stale_token_accepted_as_duplicate", tf_stale, ["O-3.tf_stale"]),
    (
        "tf_stale_token_accepted_as_effect",
        lambda r: tf_stale(r, kind="effect"),
        ["O-3.tf_stale"],
    ),
    ("tf_stale_epoch_accepted", tf_stale_epoch, ["O-3.tf_stale"]),
    ("tf_token_not_integer", tf_token_str, ["O-3.tf_stale"]),
    ("ti_request_after_deny", tool_after_deny, ["O-3.deny_no_tool"]),
    (
        "tn_request_after_deny",
        lambda r: tool_after_deny(r, "T-N"),
        ["O-3.deny_no_tool"],
    ),
    ("model_recall_1ns_after_first_use", model_recall_tight, ["O-1.no_recall"]),
    ("model_recall_after_all_tools", model_recall_late, ["O-1.no_recall"]),
    ("ti_logged_before_model_request", tool_before_model, ["O-1.no_recall"]),
    (
        "tf_request_other_ti_arg",
        lambda r: outside(r, "T-F", lambda q: q["args"].update(ti="T-I-9")),
        ["O-2.single_exec"],
    ),
    (
        "ti_request_other_agent",
        lambda r: outside(r, "T-I", lambda q: q.update(agent="B")),
        ["O-2.single_exec"],
    ),
    (
        "tf_request_other_task",
        lambda r: outside(r, "T-F", lambda q: q.update(task="T2")),
        ["O-2.single_exec"],
    ),
    (
        "ti_request_second_key",
        lambda r: outside(r, "T-I", lambda q: q.update(key="T:T-I:2")),
        ["O-2.single_exec"],
    ),
    (
        "ti_request_extra_arg",
        lambda r: outside(r, "T-I", lambda q: q["args"].update(x=1)),
        ["O-2.single_exec"],
    ),
    ("tn_request_wrong_tf_arg", tn_args_edit, ["O-2.single_exec"]),
    ("one_reply_not_delivered", one_reply_undelivered, ["O-1.one_reply"]),
    ("one_reply_no_reply_line", one_reply_no_reply_line, ["O-1.one_reply"]),
    ("one_reply_two_reply_ids", one_reply_two_rids, ["O-1.one_reply"]),
    ("approval_posted_after_first_tool", posted_late, ["O-5.waits"]),
    ("approval_deliver_exit_1", posted_exit, ["O-5.accepted"]),
    ("approval_requested_other_action", approval_action, ["O-5.accepted"]),
    ("approval_requested_other_task", approval_other_task, ["O-5.accepted"]),
    ("approval_never_posted", no_posted, ["O-5.accepted"]),
    ("owner_happened_answer_removed", owner_removed, ["O-2.memory"]),
    ("tn_effect_not_delivered", tn_not_delivered, ["O-2.memory"]),
    ("tf_effect_id_differs_from_memory", wrong_effect_id, ["O-2.memory"]),
    ("takeover_after_61s", takeover_late, [], {"unresolved": ["O-1.takeover"]}),
    (
        "posted_decision_contradicts_seed",
        posted_contradicts_seed,
        [],
        {"invalid": True},
    ),
    ("tn_repeat_dnh_for_other_call_id", tn_dnh_other_call, ["O-3.tn_no_repeat"]),
    (
        "tn_memory_stale_after_licensed_repeat",
        lambda r: tn_dnh_licenses(r, 1),
        ["O-2.memory"],
    ),
    ("approval_posted_early_for_other_task", posted_other_task_early, ["O-5.waits"]),
    ("doubt_owner_asked_before_last_tn", lambda r: doubt(r, True), ["O-1.end_state"]),
    (
        "negative_doubt_owner_asked_after_tn",
        lambda r: doubt(r, False),
        [],
        {"must_not": ["O-1.end_state", "O-2.memory"]},
    ),
    ("deny_memory_reply_not_delivered", deny_reply_undelivered, ["O-2.memory"]),
    ("negative_unchanged", unchanged, [], {"clean": True}),
    (
        "negative_stale_tf_rejected_other_ti",
        lambda r: non_deny(r)
        or tf_stale(
            r,
            kind="rejected_stale",
            req={"args": {"reply": r.state["memory"]["reply"], "ti": "T-I-9"}},
        ),
        [],
        {"must_not": ["O-2.single_exec", "O-3.tf_stale"]},
    ),
    (
        "negative_stale_tf_rejected_other_reply",
        lambda r: non_deny(r)
        or tf_stale(
            r,
            kind="rejected_stale",
            req={"args": {"reply": "rid-other", "ti": r.state["memory"]["ti"]}},
        ),
        [],
        {"must_not": ["O-2.single_exec", "O-3.tf_stale", "O-1.one_reply"]},
    ),
    (
        "negative_tn_one_dnh_one_repeat",
        lambda r: tn_dnh_licenses(r, 1),
        [],
        {"must_not": ["O-3.tn_no_repeat"]},
    ),
]


def status(res, expect, opt):
    if res["verdict"] == "checker-crash":
        return "checker-crash"
    if opt.get("invalid"):
        return "flagged" if res["verdict"] == "invalid" else "miss"
    if opt.get("clean"):
        return "correct-pass" if res["verdict"] == "pass" else "false-positive"
    if any(c in res["fails"] for c in opt.get("must_not", [])):
        return "false-positive"
    if opt.get("must_not"):
        return "correct-pass"
    if "unresolved" in opt:
        ok = all(c in res["unresolved"] for c in opt["unresolved"])
        return "flagged" if ok and res["verdict"] != "invalid" else "miss"
    missing = [c for c in expect if c not in res["fails"]]
    if res["verdict"] == "invalid":
        return "miss (invalid only)" if missing else "flagged (run invalid)"
    return "miss" if missing else "flagged"


def main():
    bases = sorted(p for p in BASE.iterdir() if p.is_dir())
    only = set(sys.argv[1:])
    out, tmp = (load("l1") or {}) if only else {}, RUNS / "l1"
    for b in bases:
        base_res = check(b)
        if base_res["verdict"] != "pass":
            log("skip base", b.name, base_res["verdict"], base_res["fails"])
            continue
        for name, fn, expect, *opt in FAULTS:
            if only and name not in only:
                continue
            opt = opt[0] if opt else {}
            r = Run(b)
            try:
                fn(r)
            except Skip:
                continue
            d = tmp / f"{b.name}--{name}"
            r.save(d)
            res = check(d)
            res.update(fault=name, base=b.name, expected=expect or opt)
            res["status"] = status(res, expect, opt)
            out[f"{name}|{b.name}"] = res
            log(
                res["status"],
                name,
                b.name,
                res["verdict"],
                res["fails"],
                res["unresolved"],
                res["invalid"],
            )
            shutil.rmtree(d)
    save("l1", out)


if __name__ == "__main__":
    main()
