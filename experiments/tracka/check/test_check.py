import copy
import itertools
import json
import tempfile
import unittest
from pathlib import Path

from check.check import canon, check, get, sha

BODY = {"messages": [{"role": "user", "content": "plan T"}]}


CLOCK = itertools.count(10**9, 10**6)


def row(**kw):
    return dict(kw, t=next(CLOCK))


def mark(event, **data):
    return row(event=event, data=data)


def tool(name, call_id, key, attempt, token, args, kind, effect, delivered=True):
    q = {"call_id": call_id, "key": key, "attempt": attempt}
    rows = [row(**q, kind="request", agent="A", task="T", token=token, args=args)]
    rows.append(row(**q, kind=kind, token=token, result={"effect": effect}))
    return rows + ([row(**q, kind="delivered")] if delivered else [])


def stub_rows(rid, inv, delivered=True):
    rows = [
        row(
            kind="request",
            request_id=rid,
            invocation_id=inv,
            retry_of=None,
            agent="A",
            task="T",
            body=canon(BODY),
            body_sha256=sha(BODY),
        ),
        row(kind="reply", request_id=rid, output_sha256="o-" + rid),
    ]
    return rows + ([row(kind="delivered", request_id=rid)] if delivered else [])


def model_entry(rid, inv, used):
    return {
        "invocation_id": inv,
        "request_id": rid,
        "input": BODY,
        "output_sha256": rid and "o-" + rid,
        "used": used,
    }


def call(t, call_id, key, token, args, attempts, outcome):
    kind, eff = outcome or ("unknown", None)
    return {
        "tool": t,
        "call_id": call_id,
        "key": key,
        "token": token,
        "args": args,
        "attempts": [{"attempt": a, "outcome": o} for a, o in attempts],
        "outcome": {"kind": kind, "result": {"effect": eff} if eff else None},
    }


def s_m(scenario="S-M", seed=0):
    """Correct run. S-M: A02, kill at receipt of T-I, T-I retried and deduplicated.
    S-AP: A09, kill at the approval request, decision posted, then a new worker."""
    sap, deny = scenario == "S-AP", scenario == "S-AP" and seed % 2 == 1
    point, kind, boundary = (
        ("A09", "kill_at_approval", "approver")
        if sap
        else ("A02", "kill_at_receipt", "T-I")
    )
    ti, tf, tn = (
        {"reply": "r1"},
        {"reply": "r1", "ti": "ei"},
        {"reply": "r1", "tf": "ef"},
    )
    svc = {n: [] for n in ("stub", "T-I", "T-F", "T-N", "approver", "owner")}
    m = [mark("started", worker="w1")]
    svc["stub"] = stub_rows("r1", "m1")
    dec = "deny" if deny else "approve"
    if sap:
        svc["approver"].append(row(kind="request", task="T", action="T-I,T-F,T-N"))
        m += [
            mark("reached", point=point, svc="approver", phase="approval"),
            mark("killed"),
        ]
        svc["approver"].append(
            row(kind="posted", task="T", action="T-I,T-F,T-N", decision=dec, exit=0)
        )
        m += [mark("posted"), mark("started", worker="w2")]
        calls = [call("T-I", "c1", "k1", 2, ti, [(1, "effect")], ("effect", "ei"))]
        if not deny:
            svc["T-I"] = tool("T-I", "c1", "k1", 1, 2, ti, "effect", "ei")
    else:
        svc["T-I"] = tool("T-I", "c1", "k1", 1, 1, ti, "effect", "ei", delivered=False)
        m += [
            mark("reached", point=point, svc="T-I", phase="receipt"),
            mark("killed"),
            mark("started", worker="w2"),
        ]
        svc["T-I"] += tool("T-I", "c1", "k1", 2, 2, ti, "duplicate", "ei")
        calls = [
            call("T-I", "c1", "k1", 1, ti, [(1, "unknown")], None),
            call("T-I", "c1", "k1", 2, ti, [(2, "duplicate")], ("duplicate", "ei")),
        ]
    if not deny:
        svc["T-F"] = tool("T-F", "c2", "k2", 1, 2, tf, "effect", "ef")
        svc["T-N"] = tool("T-N", "c3", None, 1, 2, tn, "effect", "en")
        calls += [
            call("T-F", "c2", "k2", 2, tf, [(1, "effect")], ("effect", "ef")),
            call("T-N", "c3", None, 2, tn, [(1, "effect")], ("effect", "en")),
        ]
    pol = {
        "rule_version": "p1",
        "action": "T-I,T-F,T-N",
        "result": "allow",
        "approval": None,
    }
    if sap:
        pol.update(result="needs_approval", approval={"decision": dec})
    return {
        "run": {
            "point": point,
            "seed": seed,
            "seed_string": f"tracka:x:{point}:{seed}",
            "arm": "fake",
            "scenario": scenario,
            "kind": kind,
            "boundary": boundary,
            "varies": "",
            "ports": {},
            "unsupported": [],
        },
        "markers": m,
        "svc": svc,
        "state": {
            "task": {"id": "T", "status": "denied" if deny else "done"},
            "memory": (
                {"reply": "r1", "denied": True}
                if deny
                else {"reply": "r1", "ti": "ei", "tf": "ef", "tn": "en"}
            ),
            "retention": "7d",
        },
        "recon": {
            "model": [model_entry("r1", "m1", True)],
            "policy": [pol],
            "calls": [] if deny else calls,
            "retention": "7d",
        },
    }


def s_ap(seed):
    return s_m("S-AP", seed)


def zombie(gap_s, boundary="T-I"):
    d = s_m()
    d["run"].update(point="A15", kind="zombie_takeover", boundary=boundary)
    stop = next(CLOCK)
    d["markers"] = [
        mark("started", worker="w1"),
        mark("reached", point="A15", svc=boundary, phase="receipt"),
        {"t": stop, "event": "stopped", "data": {}},
        {"t": stop + 1, "event": "started", "data": {"worker": "w2"}},
        {"t": stop + int(gap_s * 1e9), "event": "resumed", "data": {}},
    ]
    return d


def in_doubt():
    d = s_m()
    d["run"].update(point="A04", seed=1, boundary="T-N")
    d["markers"][1]["data"] = {"point": "A04", "svc": "T-N", "phase": "receipt"}
    tn = d["svc"]["T-N"][0]["args"]
    d["svc"]["T-N"] = tool("T-N", "c3", None, 1, 2, tn, "effect", "en", delivered=False)
    d["svc"]["owner"] = [row(kind="answer", task="T", call_id="c3", answer="happened")]
    d["state"].update(task={"id": "T", "status": "stopped_in_doubt"}, memory=None)
    d["recon"]["calls"][3] = call(
        "T-N", "c3", None, 2, tn, [(1, "owner:happened")], ("owner:happened", None)
    )
    return d


def tn_retry(first_effect=False):
    """T-N killed at receipt; the owner, asked about it, answers truthfully; then a resend."""
    d = s_m()
    d["run"].update(point="A04", kind="kill_at_receipt", boundary="T-N")
    d["markers"][1]["data"] = {"point": "A04", "svc": "T-N", "phase": "receipt"}
    tn = d["svc"]["T-N"][0]["args"]
    ans = "happened" if first_effect else "did_not_happen"
    first = tool("T-N", "c3", None, 1, 2, tn, "effect", "en", delivered=False)
    first = first if first_effect else first[:1]
    d["svc"]["owner"] = [row(kind="answer", task="T", call_id="c3", answer=ans)]
    d["svc"]["T-N"] = first + tool("T-N", "c4", None, 1, 3, tn, "effect", "en2")
    d["state"]["memory"]["tn"] = "en2"
    d["recon"]["calls"][3] = call(
        "T-N", "c3", None, 2, tn, [(1, "owner:" + ans)], ("owner:" + ans, None)
    )
    d["recon"]["calls"].append(
        call("T-N", "c4", None, 3, tn, [(1, "effect")], ("effect", "en2"))
    )
    return d


def write(d, root, quiesce_s=11):
    R = Path(root)
    (R / "svc").mkdir(parents=True)
    if d["run"] is not None:
        (R / "run.json").write_text(json.dumps(d["run"]))
    lines = lambda xs: "".join(json.dumps(x) + "\n" for x in xs)
    end = (
        max(x.get("t", 0) for xs in [d["markers"], *d["svc"].values()] for x in xs) + 1
    )
    markers = d["markers"]
    t = end + int(quiesce_s * 1e9)
    if not any(x["event"] == "quiesced" for x in markers):
        status = d.get("run_end", "ended")
        seen = get(d["state"], "task", "status")
        markers = markers + [
            {"t": end + 10**10, "event": "quiesced", "data": {}},
            {"t": t + 1, "event": "run-end", "data": {"status": status}},
        ]
        if status != "cap":
            markers.insert(
                -2, {"t": end, "event": "end-state", "data": {"status": seen}}
            )
    (R / "markers.jsonl").write_text(lines(markers))
    for name, xs in d["svc"].items():
        (R / "svc" / f"{name}.jsonl").write_text(lines(xs))
    ans = [
        {"t": t, "call": "state", "arg": "A", "answer": d["state"], "exit": 0},
        {"t": t, "call": "reconstruct", "arg": "T", "answer": d["recon"], "exit": 0},
    ]
    (R / "answers.jsonl").write_text(lines(ans))
    return R


def run(d, edit=None):
    with tempfile.TemporaryDirectory() as tmp:
        R = write(d, tmp)
        if edit:
            edit(R)
        return check(R)


def status(res):
    return {c["id"]: c["status"] for c in res["clauses"]}


class Correct(unittest.TestCase):
    def assert_pass(self, d, ne=()):
        res = run(d)
        st = status(res)
        self.assertEqual(res["verdict"], "pass", res)
        for cid, s in st.items():
            self.assertEqual(s, "not_exercised" if cid in ne else "pass", (cid, res))

    def test_s_m(self):
        ne = (
            "O-1.takeover",
            "O-3.deny_no_tool",
            "O-4.approval",
            "O-5.accepted",
            "O-5.once",
            "O-5.waits",
            "O-5.proceeds",
        )
        self.assert_pass(s_m(), ne)

    def test_s_ap_approve(self):
        self.assert_pass(s_ap(0), ("O-1.takeover", "O-3.deny_no_tool"))

    def test_s_ap_deny(self):
        ne = ("O-1.takeover", "O-3.ti_once", "O-3.tn_no_repeat", "O-3.tf_stale")
        self.assert_pass(s_ap(1), ne)

    def test_in_doubt(self):
        self.assertEqual(run(in_doubt())["verdict"], "pass")

    def test_in_doubt_after_did_not_happen(self):
        d = in_doubt()
        d["svc"]["T-N"] = d["svc"]["T-N"][:1]
        d["svc"]["owner"][0]["answer"] = "did_not_happen"
        d["recon"]["calls"][3]["attempts"][0]["outcome"] = "owner:did_not_happen"
        d["recon"]["calls"][3]["outcome"]["kind"] = "owner:did_not_happen"
        self.assertEqual(run(d)["verdict"], "pass")

    def test_resend_under_new_token_is_own_entry(self):
        d = tn_retry()
        d["recon"]["calls"][4]["token"] = 2
        self.assertEqual(status(run(d))["O-4.calls"], "fail")

    def test_tn_retry_after_did_not_happen(self):
        self.assertEqual(run(tn_retry())["verdict"], "pass")

    def test_zombie_takeover(self):
        res = run(zombie(1.5))
        self.assertEqual(
            (res["verdict"], status(res)["O-1.takeover"]), ("pass", "pass")
        )

    def test_zombie_model_concurrent_recall(self):
        d = zombie(0.5, "model")
        r2 = stub_rows("r2", "m1")
        for n, x in enumerate(r2, 1):
            x["t"] = d["svc"]["stub"][2]["t"] + n
        d["svc"]["stub"][3:3] = r2
        d["recon"]["model"].append(model_entry(None, "m1", False))
        self.assertEqual(run(d)["verdict"], "pass")

    def test_unsupported(self):
        d = s_ap(0)
        d["run"]["unsupported"] = [
            "O-5.accepted",
            "O-5.once",
            "O-5.proceeds",
            "O-4.approval",
        ]
        d["recon"]["policy"][0]["approval"] = None
        res = run(d)
        self.assertEqual(res["guarantees"]["O-5"], "unsupported")
        self.assertEqual(res["guarantees"]["O-4"], "unsupported")
        self.assertEqual(status(res)["O-4.approval"], "unsupported")
        self.assertEqual(res["verdict"], "pass")

    def test_unsupported_not_inferred(self):
        d = s_m()
        d["run"]["unsupported"] = ["O-5"]
        d["svc"]["T-F"][0]["args"]["reply"] = "r0"
        st = status(run(d))
        self.assertEqual((st["O-1.one_reply"], st["O-2.single_exec"]), ("fail", "fail"))

    def test_guarantee_id_is_not_a_clause_id(self):
        d = s_ap(0)
        d["run"]["unsupported"] = ["O-5"]
        d["recon"]["policy"][0]["approval"] = None
        self.assertEqual(status(run(d))["O-5.once"], "fail")

    def test_listed_causality_clauses(self):
        d = s_m()
        d["run"]["unsupported"] = ["O-1.one_reply", "O-2.memory"]
        d["state"]["memory"] = {"other": 1}
        st = status(run(d))
        self.assertEqual((st["O-1.one_reply"], st["O-2.memory"]), ("unsupported",) * 2)

    def test_unsupported_unexercised_rolls_up_not_exercised(self):
        d = s_m()
        d["run"]["unsupported"] = ["O-4.approval", "O-5.accepted"]
        res = run(d)
        self.assertEqual(
            (res["guarantees"]["O-4"], res["guarantees"]["O-5"]),
            ("pass", "not_exercised"),
        )


def mutate(base, f):
    d = copy.deepcopy(base)
    f(d)
    return d


def set_(path, value):
    def f(d):
        *head, last = path
        x = d
        for p in head:
            x = x[p]
        x[last] = value

    return f


def extra_ti_effect(d):
    ti = d["svc"]["T-I"][0]["args"]
    d["svc"]["T-I"] += tool("T-I", "c9", "k9", 1, 2, ti, "effect", "ei2")
    d["recon"]["calls"].append(
        call("T-I", "c9", "k9", 2, ti, [(1, "effect")], ("effect", "ei2"))
    )


def extra_tf(kind, token):
    def f(d):
        tf = d["svc"]["T-F"][0]["args"]
        d["svc"]["T-F"] += tool("T-F", "c8", "k8", 1, token, tf, kind, "ef2")
        d["recon"]["calls"].append(
            call("T-F", "c8", "k8", token, tf, [(1, kind)], (kind, "ef2"))
        )

    return f


def recall(d):
    d["svc"]["stub"] += stub_rows("r2", "m2")
    d["recon"]["model"].append(model_entry("r2", "m2", False))


def doubt_without_fault(d):
    d["markers"][1]["data"]["svc"] = "T-I"
    d["svc"]["T-N"].append(row(kind="delivered", call_id="c3", key=None, attempt=1))


def second_approval(d):
    d["recon"]["policy"].append(copy.deepcopy(d["recon"]["policy"][0]))


VIOLATIONS = [
    ("O-1.end_state", s_m, set_(("state", "task", "status"), "running")),
    ("O-1.end_state", in_doubt, doubt_without_fault),
    ("O-1.one_reply", s_m, set_(("svc", "T-N", 0, "args", "reply"), "r9")),
    ("O-1.no_recall", s_m, recall),
    ("O-2.memory", s_m, set_(("state", "memory", "ti"), "other")),
    ("O-2.memory", s_m, set_(("state", "memory"), None)),
    ("O-2.memory", lambda: s_ap(1), set_(("state", "memory", "reply"), "r7")),
    ("O-2.single_exec", s_m, extra_tf("effect", 3)),
    ("O-2.single_exec", s_m, set_(("svc", "T-F", 0, "agent"), "B")),
    ("O-3.ti_once", s_m, extra_ti_effect),
    ("O-3.tn_no_repeat", lambda: tn_retry(True), lambda d: None),
    ("O-3.tf_stale", s_m, extra_tf("duplicate", 1)),
    ("O-3.tf_stale", s_m, set_(("svc", "T-F", 0, "token"), None)),
    (
        "O-3.deny_no_tool",
        lambda: s_ap(1),
        lambda d: d["svc"].update(**{"T-I": s_m()["svc"]["T-I"]}),
    ),
    ("O-4.model", s_m, set_(("recon", "model", 0, "input"), {"messages": []})),
    ("O-4.model", s_m, set_(("recon", "model"), [])),
    ("O-4.model_used", s_m, set_(("recon", "model", 0, "used"), None)),
    ("O-4.model_used", s_m, set_(("recon", "model", 0, "request_id"), None)),
    ("O-4.calls", s_m, lambda d: d["recon"]["calls"].pop(0)),
    (
        "O-4.calls",
        s_m,
        set_(("recon", "calls", 2, "attempts", 0, "outcome"), "unknown"),
    ),
    (
        "O-4.calls",
        s_m,
        set_(("recon", "calls", 1, "outcome", "result"), {"effect": "x"}),
    ),
    ("O-4.calls", s_m, set_(("recon", "calls", 2, "token"), None)),
    ("O-4.policy", s_m, set_(("recon", "policy", 0, "rule_version"), None)),
    ("O-4.policy", s_m, set_(("recon", "policy", 0, "result"), "needs_approval")),
    ("O-4.approval", lambda: s_ap(0), set_(("recon", "policy", 0, "approval"), None)),
    ("O-4.approval", lambda: s_ap(1), set_(("recon", "calls"), None)),
    ("O-4.retention", s_m, set_(("recon", "retention"), None)),
    ("O-5.accepted", lambda: s_ap(0), set_(("svc", "approver", 1, "exit"), 1)),
    ("O-5.accepted", lambda: s_ap(0), set_(("svc", "approver"), [])),
    ("O-5.once", lambda: s_ap(0), second_approval),
    ("O-5.waits", lambda: s_ap(0), set_(("svc", "approver", 1, "t"), 10**15)),
    ("O-5.proceeds", lambda: s_ap(0), set_(("state", "task", "status"), "running")),
    ("O-5.proceeds", lambda: s_ap(1), set_(("state", "task", "status"), "done")),
]


class Violations(unittest.TestCase):
    def test_each_clause_flagged(self):
        for cid, base, f in VIOLATIONS:
            with self.subTest(cid=cid):
                res = run(mutate(base(), f))
                self.assertEqual(status(res)[cid], "fail", res)
                self.assertIn(res["verdict"], ("violation", "invalid"))

    def test_every_clause_has_a_violation(self):
        from check.check import CLAUSES

        covered = {v[0] for v in VIOLATIONS} | {"O-1.takeover"}
        self.assertEqual(covered, {c for c, _ in CLAUSES})

    def test_used_flag_split_from_model(self):
        d = s_m()
        d["recon"]["model"][0]["used"] = False
        st = status(run(d))
        self.assertEqual((st["O-4.model"], st["O-4.model_used"]), ("pass", "fail"))

    def test_tn_repeat_before_owner_answer(self):
        d = tn_retry()
        d["svc"]["owner"][0]["t"] = d["svc"]["T-N"][-1]["t"] + 1
        self.assertEqual(status(run(d))["O-3.tn_no_repeat"], "fail")

    def test_cap_is_unresolved(self):
        d = s_ap(0)
        d["run_end"] = "cap"
        d["state"]["task"]["status"] = "running"
        d["state"]["memory"] = None
        res = run(d)
        st = status(res)
        self.assertEqual(res["verdict"], "unresolved", res)
        self.assertEqual(st["O-1.end_state"], "unresolved")
        self.assertEqual(st["O-5.proceeds"], "unresolved")

    def test_cap_keeps_other_violations(self):
        d = s_m()
        d["run_end"] = "cap"
        d["state"]["task"]["status"] = "running"
        extra_ti_effect(d)
        self.assertEqual(run(d)["verdict"], "violation")

    def test_in_doubt_without_owner(self):
        d = in_doubt()
        d["svc"]["owner"] = []
        self.assertEqual(status(run(d))["O-1.end_state"], "fail")

    def test_owner_contradicting_truth_is_invalid(self):
        d = in_doubt()
        d["svc"]["owner"][0]["answer"] = "did_not_happen"
        self.assertEqual(run(d)["verdict"], "invalid")

    def test_zombie_no_takeover_unresolved(self):
        res = run(zombie(61))
        self.assertEqual(
            (res["verdict"], status(res)["O-1.takeover"]), ("unresolved", "unresolved")
        )
        self.assertEqual(res["guarantees"]["O-1"], "unresolved")

    def test_zombie_model_two_recalls(self):
        d = zombie(0.5, "model")
        d["svc"]["stub"] += stub_rows("r2", "m1") + stub_rows("r3", "m1")
        d["recon"]["model"] += [
            model_entry("r2", "m1", False),
            model_entry("r3", "m1", False),
        ]
        self.assertEqual(status(run(d))["O-1.no_recall"], "fail")

    def test_null_answers_no_crash(self):
        d = s_ap(0)
        d["state"], d["recon"] = None, None
        res = run(d)
        st = status(res)
        for cid in (
            "O-1.end_state",
            "O-2.memory",
            "O-4.model",
            "O-4.calls",
            "O-4.policy",
            "O-5.once",
        ):
            self.assertEqual(st[cid], "fail", cid)

    def test_null_fields_no_crash(self):
        d = s_m()
        d["recon"] = {
            "model": [None, {"used": None}],
            "policy": [None],
            "calls": [None, {"attempts": "x"}],
        }
        d["state"] = {"task": None, "memory": "x", "retention": None}
        res = run(d)
        self.assertEqual(res["verdict"], "violation")


class Invalid(unittest.TestCase):
    def test_no_run_json(self):
        d = s_m()
        d["run"] = None
        self.assertEqual(run(d)["verdict"], "invalid")

    def test_cut_log(self):
        def cut(R):
            p = R / "svc" / "T-F.jsonl"
            p.write_text(p.read_text()[:-5])

        self.assertEqual(run(s_m(), cut)["verdict"], "invalid")

    def test_missing_boundary_log(self):
        self.assertEqual(
            run(s_m(), lambda R: (R / "svc" / "T-I.jsonl").unlink())["verdict"],
            "invalid",
        )

    def test_short_quiesce(self):
        with tempfile.TemporaryDirectory() as tmp:
            R = write(s_m(), tmp, quiesce_s=9)
            self.assertEqual(check(R)["verdict"], "invalid")

    def test_run_end_error(self):
        d = s_m()
        d["run_end"] = "error"
        self.assertEqual(run(d)["verdict"], "invalid")

    def test_line_without_t(self):
        d = s_m()
        del d["svc"]["T-F"][1]["t"]
        self.assertEqual(run(d)["verdict"], "invalid")

    def test_harness_error(self):
        d = s_m()
        d["markers"].append({"t": 9, "event": "error", "data": {"msg": "boom"}})
        self.assertEqual(run(d)["verdict"], "invalid")

    def test_violation_still_reported_in_invalid_run(self):
        d = s_m()
        extra_ti_effect(d)
        d["markers"].append({"t": 9, "event": "error", "data": {}})
        res = run(d)
        self.assertEqual(
            (res["verdict"], status(res)["O-3.ti_once"]), ("invalid", "fail")
        )

    def test_cli_writes_check_json(self):
        import subprocess
        import sys

        with tempfile.TemporaryDirectory() as tmp:
            R = write(s_m(), tmp)
            cwd = Path(__file__).resolve().parent.parent
            out = subprocess.run(
                [sys.executable, "-m", "check.check", str(R)],
                cwd=cwd,
                capture_output=True,
                text=True,
            )
            self.assertEqual(json.loads(out.stdout)["verdict"], "pass", out.stderr)
            self.assertEqual(
                json.loads((R / "check.json").read_text())["verdict"], "pass"
            )


if __name__ == "__main__":
    unittest.main()
