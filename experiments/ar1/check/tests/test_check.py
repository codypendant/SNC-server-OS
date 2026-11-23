import hashlib
import json
import shutil
import tempfile
import unittest

from check.check import Check
from check.tests.fake import World, happy


def check(w):
    db, recs = w.write()
    return Check(w.R, db_rows=db, wal_recs=recs, standby_recs=recs).check()


def clauses(r):
    return {v["clause"] for v in r["violations"]}


def start(R, point, upto="P-dec", **kw):
    w = World(R, point=point, **kw)
    w.accept()
    if upto != "C-acc":
        w.model()
        w.policy()
    return w


def finish(w, tools=("T-I", "T-F", "T-N"), observe=True):
    for t in tools:
        w.tool(t)
    w.done()
    if observe:
        w.observe()
    w.final()
    return check(w)


class Base(unittest.TestCase):
    def setUp(self):
        self.R = self.tmp()

    def tmp(self):
        d = tempfile.mkdtemp(prefix="ar1-check-unit-")
        self.addCleanup(shutil.rmtree, d, True)
        return d

    def assertClean(self, r):
        self.assertEqual(r["violations"], [], json.dumps(r["violations"], indent=1))
        self.assertEqual(r["unresolved"], [], json.dumps(r["unresolved"], indent=1))
        self.assertEqual(r["invalid"], [])
        self.assertEqual(r["verdict"], "pass")

    def assertFlags(self, r, *cl):
        self.assertTrue(set(cl) <= clauses(r), (cl, clauses(r), r["unresolved"]))


class Correct(Base):
    def test_no_kill(self):
        self.assertClean(check(happy(self.R)))

    def test_kill_after_each_step(self):
        for k in ("C-acc", "model", "P-dec", "T-I", "T-F", "T-N", "M-done"):
            self.R = self.tmp()
            r = check(happy(self.R, kill_after=k, point=f"A/a4:{k}"))
            self.assertClean(r)
            self.assertIn("1.lower", r["exercised"])

    def test_classes_L_then_R(self):
        w = start(self.R, "A/a3:TI-disp")
        w.prep("T-I")
        w.sync = False
        tok = w.dispatch("T-I")
        w.marker("reached", "a3:TI-disp")
        w.kill()
        w.observe(commits=[c for c in w.visible() if c["token"] != tok])
        w.logs["answers"][-1]["answer"]["calls"]["c-T-I"]["state"] = "prepared"
        w.resume()
        w.gw(call_id="c-T-I", attempt=1)
        w.send("T-I")
        w.outcome("T-I")
        r = finish(w, tools=("T-F", "T-N"), observe=False)
        self.assertClean(r)
        self.assertEqual(r["classes"][tok]["class"], "R")
        self.assertEqual(r["classes"][tok]["at_cuts"]["k1"], "L")

    def test_plain_arm_happy(self):
        r = check(happy(self.R, arm="plain1"))
        self.assertClean(r)


class Mutations(Base):
    def test_M1_send_before_establishment(self):
        w = start(self.R, "A/a3:TI-disp", mutation="M1")
        w.prep("T-I")
        w.sync = False
        w.dispatch("T-I")
        w.send("T-I")
        w.marker("reached", "a3:TI-disp")
        w.kill()
        w.resume()
        w.outcome("T-I")
        r = finish(w, tools=("T-F", "T-N"))
        self.assertFlags(r, "4.established")
        e = [v for v in r["violations"] if v["clause"] == "4.established"][0]["evidence"]
        self.assertEqual(e["svc"], "T-I")

    def cs(self, mutation, stale="TF-prep", rejected=None, at="T-I"):
        w = World(self.R, point=f"C-s/a4:{'I-out' if at == 'model' else 'TI-out'}", mutation=mutation)
        w.accept()
        w.model()
        if at == "T-I":
            w.policy()
        if at == "T-I":
            w.tool("T-I")
        w.marker("reached", w.run["point"].split("/")[1])
        w.takeover()
        w.marker("reached", "post-takeover")
        w.marker("stale-decision-attempt", decision=stale, epoch=1)
        if rejected:
            w.marker("stale-decision-rejected", reason=rejected)
        else:
            if stale == "TF-prep":
                w.prep("T-F")
                w.row("tool_call", id="c-T-F")["epoch"] = 1
                w.commit("TF-disp", "decision", epoch=1, moves=[("tool_call", "c-T-F", "dispatched")])
                w.send("T-F", epoch=1)
                w.outcome("T-F", epoch=1)
            else:
                w.commit(stale, "decision", epoch=1)
            w.marker("stale-decision-committed")
        w.marker("released", who="new")
        tools = [
            t
            for t in ("T-I", "T-F", "T-N")
            if not (t == "T-I" and at == "T-I") and not (t == "T-F" and not rejected and stale == "TF-prep")
        ]
        return w, tools

    def test_M2_stale_prepare_and_dispatch(self):
        w, tools = self.cs("M2")
        for x in w.tables["commit_token"]:
            if x["name"] in ("TF-prep",):
                x["epoch"] = 1
        r = finish(w, tools=tools)
        names = {v["evidence"].get("name") for v in r["violations"] if v["clause"] == "1.stale_epoch"}
        self.assertEqual(names, {"TF-prep", "TF-disp"})
        self.assertEqual(r["unresolved"], [])

    def test_positive_control_pass(self):
        w, tools = self.cs(None, rejected="epoch check")
        self.assertClean(finish(w, tools=tools))

    def test_positive_control_wrong_reason(self):
        w, tools = self.cs(None, rejected="lock timeout")
        r = finish(w, tools=tools)
        self.assertTrue(any("another reason" in u["reason"] for u in r["unresolved"]))

    def test_stale_commit_without_mutation_is_violation(self):
        w, tools = self.cs(None, stale="P-dec", at="model")
        r = finish(w, tools=tools)
        self.assertFlags(r, "1.stale_epoch")
        self.assertEqual(r["unresolved"], [])

    def test_cs_wrong_interleaving(self):
        w = start(self.R, "C-s/a4:TI-out")
        w.tool("T-I")
        w.marker("reached", "a4:TI-out")
        w.marker("stale-decision-attempt", decision="TF-prep", epoch=1)
        w.takeover()
        w.marker("stale-decision-rejected", reason="epoch")
        r = finish(w, tools=("T-F", "T-N"))
        self.assertTrue(any("interleaving" in u["reason"] for u in r["unresolved"]))

    def test_M10_stale_policy_decision(self):
        w, tools = self.cs("M10", stale="P-dec", at="model")
        self.assertFlags(finish(w, tools=tools), "1.stale_epoch")

    def b3_model(self, mutation):
        w = World(self.R, point="B/b3:model", mutation=mutation)
        w.accept()
        w.model(crash="b3")
        return w

    def test_M3_regenerated_same_id(self):
        w = self.b3_model("M3")
        w.log(
            "stub",
            svc="stub",
            kind="request",
            invocation_id="i1",
            retry_of=None,
            agent="A",
            epoch=1,
            body=w.logs["stub"][0]["body"],
            body_sha256=w.logs["stub"][0]["body_sha256"],
            request_id="req-i1-again",
            read=w.read(),
        )
        h = hashlib.sha256(b"output-i1").hexdigest()
        w.log("stub", svc="stub", kind="reply", invocation_id="i1", request_id="req-i1-again", output_sha256=h)
        inv = w.row("invocation", id="i1")
        inv["provider_request_id"], inv["output"] = "req-i1-again", "output-i1"
        w.commit("I-out", "outcome", moves=[("invocation", "i1", "output")])
        w.policy()
        r = finish(w)
        self.assertFlags(r, "3.recall", "3.crash_unknown", "3.no_regenerate")
        self.assertIn("not from the first physical request", json.dumps(r["violations"]))

    def test_output_dict_from_first_request(self):
        w = happy(self.R)
        inv = w.row("invocation", id="i1")
        inv["output"] = {"output": inv["output"], "chunks": 1}
        self.assertClean(check(w))
        inv["output"] = {"output": "other"}
        self.assertIn("output hash differs", json.dumps(check(w)["violations"]))

    def test_M3_regenerated_new_id_as_original(self):
        w = self.b3_model("M3")
        w.log(
            "stub",
            svc="stub",
            kind="request",
            invocation_id="i2",
            retry_of="i1",
            agent="A",
            epoch=1,
            body="{}",
            body_sha256="44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a",
            read=w.read(),
        )
        w.log("stub", svc="stub", kind="reply", invocation_id="i2", request_id="req-i2", output_sha256=None)
        inv = w.row("invocation", id="i1")
        inv["provider_request_id"], inv["output"] = "req-i2", "x"
        w.commit("I-out", "outcome", moves=[("invocation", "i1", "output")])
        w.policy()
        self.assertFlags(finish(w), "3.no_regenerate", "3.crash_unknown")

    def test_b3_model_correct(self):
        w = self.b3_model(None)
        w.commit("I-unknown", "recovery", moves=[("invocation", "i1", "result_unknown")])
        w.model(iid="i2", retry_of="i1")
        w.policy()
        self.assertClean(finish(w))

    def test_M12_recall_without_retry_of_and_late_not_marked(self):
        w = self.b3_model("M12")
        w.commit("I-unknown", "recovery", moves=[("invocation", "i1", "result_unknown")])
        w.model(iid="i2")
        w.row("invocation", id="i1")["output"] = "late"
        w.commit("I-out", "outcome", moves=[("invocation", "i1", "output")])
        w.policy()
        self.assertFlags(finish(w), "3.recall", "3.late_marked")

    def b2_tool(self, tool, mutation):
        w = start(self.R, f"B/b2:{tool}", mutation=mutation)
        for t in ("T-I", "T-F", "T-N"):
            if t == tool:
                break
            w.tool(t)
        w.prep(tool)
        w.dispatch(tool)
        w.send(tool)
        w.marker("reached", f"b2:{tool}")
        w.kill()
        return w

    def rest(self, tool):
        order = ("T-I", "T-F", "T-N")
        return order[order.index(tool) + 1 :]

    def test_M4_automatic_TN_retry(self):
        w = self.b2_tool("T-N", "M4")
        w.dispatch("T-N", n=2)
        w.send("T-N", n=2)
        w.outcome("T-N")
        r = finish(w, tools=())
        self.assertFlags(r, "5.TN_no_retry", "2.no_repeat")

    def test_M5_policy_read_unestablished(self):
        w = World(self.R, point="A/a3:C-acc", mutation="M5")
        w.sync = False
        w.accept()
        w.marker("reached", "a3:C-acc")
        w.kill()
        w.policy()
        w.cut("pre-resume", "pre-resume")
        w.resume()
        w.observe()
        w.model()
        r = finish(w, observe=False)
        self.assertFlags(r, "4.policy_read")
        self.assertEqual(r["mutation"]["detected"], ["4.policy_read"])
        self.assertTrue(r["mutation"]["exercised"])
        self.assertEqual(r["oracle_verdict"], "violation")

    def test_M5_positive_control_policy_after_resume(self):
        w = World(self.R, point="A/a3:C-acc", mutation="M5")
        w.sync = False
        w.accept()
        w.marker("reached", "a3:C-acc")
        w.kill()
        w.cut("pre-resume", "pre-resume")
        w.resume()
        w.observe()
        w.model()
        w.policy()
        r = finish(w, observe=False)
        self.assertClean(r)
        self.assertFalse(r["mutation"]["exercised"])

    def a3_answer(self, early):
        w = World(self.R, point="A/a3:C-acc")
        w.sync = False
        w.accept()
        w.marker("reached", "a3:C-acc")
        w.kill()
        w.marker("answer-requested")
        if early:
            w.marker("answer-returned")
        w.cut("pre-resume", "pre-resume")
        w.resume()
        w.marker("standby-resumed")
        w.observe()
        w.model()
        w.policy()
        return finish(w, observe=False)

    def test_upper_answer_before_establishment(self):
        r = self.a3_answer(True)
        self.assertFlags(r, "1.upper")
        self.assertEqual(r["oracle_flagged"], ["1.upper"])

    def test_upper_answer_after_resume(self):
        r = self.a3_answer(False)
        self.assertClean(r)
        self.assertIn("1.upper", r["exercised"])

    def test_upper_not_exercised_without_unestablished_commit(self):
        r = check(happy(self.R))
        self.assertNotIn("1.upper", r["exercised"])

    def test_policy_read_not_exercised_outside_P(self):
        w = World(self.R, point="A/a3:C-acc")
        w.sync = False
        w.accept()
        w.marker("reached", "a3:C-acc")
        w.kill()
        w.cut("pre-resume", "pre-resume")
        w.resume()
        w.observe()
        w.model()
        w.policy()
        self.assertNotIn("4.policy_read", finish(w, observe=False)["exercised"])

    def test_prefix_gap_in_snapshot_exempt(self):
        w = happy(self.R)
        ans = next(a for a in w.logs["answers"] if a["call"] == "state")["answer"]
        dropped = ans["commits"].pop(2)
        x = int(dropped["xid"])
        ans["snapshot"] = f"{x - 1}:{x + 50}:{x}"
        r = check(w)
        self.assertNotIn("1.prefix", clauses(r))
        self.assertEqual([e["xid"] for e in r["exemptions"]], [dropped["xid"]])
        ans["snapshot"] = f"{x + 1}:{x + 50}:"
        self.assertIn("1.prefix", clauses(check(w)))

    def cp(self, reject):
        w = start(self.R, "C-p/a4:TI-prep", mutation=None if reject else "M6")
        w.prep("T-I")
        w.marker("reached", "a4:TI-prep")
        w.takeover(reject=reject)
        w.marker("reached", "post-takeover")
        w.marker("stale-decision-attempt", decision="TI-disp", epoch=1)
        w.marker("stale-decision-rejected", reason="epoch")
        w.marker("released", who="new")
        w.prep("T-I", call="c2")
        w.dispatch("T-I", call="c2")
        w.send("T-I", call="c2")
        w.outcome("T-I", call="c2")
        return finish(w, tools=("T-F", "T-N"))

    def test_M6_takeover_keeps_prepared(self):
        self.assertFlags(self.cp(False), "6.prepared_rejected")

    def test_Cp_correct(self):
        r = self.cp(True)
        self.assertClean(r)
        self.assertIn("6.prepared_rejected", r["exercised"])

    def test_M7_reconstruction_drops_attempt(self):
        w = self.b2_tool("T-I", "M7")
        w.log("T-I", svc="T-I", kind="status", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.outcome("T-I")
        for t in ("T-F", "T-N"):
            w.tool(t)
        w.done()
        w.observe()
        w.reconstruct_all(drop_attempts=("c-T-I",))
        w.final()
        self.assertFlags(check(w), "7.executed")

    def test_b2_TI_correct_status(self):
        w = self.b2_tool("T-I", None)
        w.log("T-I", svc="T-I", kind="status", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.outcome("T-I")
        self.assertClean(finish(w, tools=("T-F", "T-N")))

    def test_b2_TI_resolved_without_status(self):
        w = self.b2_tool("T-I", None)
        w.outcome("T-I")
        w.row("tool_call", id="c-T-I")["result"] = "guessed"
        self.assertFlags(finish(w, tools=("T-F", "T-N")), "6.TI_status")

    def test_M9_resend_with_new_key(self):
        w = self.b2_tool("T-I", "M9")
        w.dispatch("T-I", n=2)
        w.send("T-I", n=2, key="k-new")
        w.outcome("T-I")
        self.assertFlags(finish(w, tools=("T-F", "T-N")), "5.TI_one_effect", "5.TI_same_key")

    def cb2(self, tool, mutation, old_outcome=True, release="after"):
        w = start(self.R, f"C-b2/b2:{tool}", mutation=mutation, release=release)
        for t in ("T-I", "T-F", "T-N"):
            if t == tool:
                break
            w.tool(t)
        w.prep(tool)
        w.dispatch(tool)
        req = w.log(
            tool,
            svc=tool,
            kind="request",
            call_id=f"c-{tool}",
            key=f"k-c-{tool}",
            attempt=1,
            agent="A",
            epoch=1,
            args={},
            read=w.read(),
        )
        w.marker("reached", f"b2:{tool}")
        w.takeover()
        w.marker("reached", "post-takeover")
        return w, req

    def test_M8_outcome_blocked(self):
        w, _ = self.cb2("T-I", "M8")
        w.marker("released", who="old")
        w.log("T-I", svc="T-I", kind="effect", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.marker("released", who="new")
        w.log("T-I", svc="T-I", kind="status", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.outcome("T-I")
        self.assertFlags(finish(w, tools=("T-F", "T-N")), "6.outcome_exempt")

    def test_Cb2_correct(self):
        w, _ = self.cb2("T-I", None)
        w.marker("released", who="old")
        w.log("T-I", svc="T-I", kind="effect", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.outcome("T-I", epoch=1)
        w.derive("c-T-I")
        w.marker("released", who="new")
        r = finish(w, tools=("T-F", "T-N"))
        self.assertClean(r)
        self.assertIn("6.outstanding", r["exercised"])

    def test_Cb2_wrong_release_order(self):
        w, _ = self.cb2("T-I", None, release="before")
        w.marker("released", who="old")
        w.log("T-I", svc="T-I", kind="effect", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.outcome("T-I", epoch=1)
        r = finish(w, tools=("T-F", "T-N"))
        self.assertTrue(any("release order" in u["reason"] for u in r["unresolved"]))

    def test_M11_no_outstanding_set(self):
        w = start(self.R, "C-b2/b2:T-I", mutation="M11", release="after")
        w.prep("T-I")
        w.dispatch("T-I")
        w.send("T-I")
        w.marker("reached", "b2:T-I")
        w.marker("released", who="old")
        w.takeover(outstanding=[])
        w.outcome("T-I", epoch=1)
        self.assertFlags(finish(w, tools=("T-F", "T-N")), "6.outstanding")

    def test_M11_cancel_without_outstanding(self):
        w = start(self.R, "D/b2:T-I", mutation="M11")
        w.prep("T-I")
        w.dispatch("T-I")
        w.send("T-I")
        w.marker("reached", "b2:T-I")
        w.commit("CANCEL", "cancel", moves=[("task", "T", "cancelled")])
        w.outcome("T-I")
        w.observe()
        w.final()
        self.assertFlags(check(w), "6.outstanding")

    def test_M13_not_derived(self):
        w, _ = self.cb2("T-I", "M13")
        w.marker("released", who="old")
        w.log("T-I", svc="T-I", kind="effect", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.outcome("T-I", epoch=1)
        w.marker("released", who="new")
        w.dispatch("T-I", n=2)
        w.send("T-I", n=2, kind="duplicate")
        w.outcome("T-I")
        self.assertFlags(finish(w, tools=("T-F", "T-N")), "6.derive")

    def test_M14_conflict_rule(self):
        w, _ = self.cb2("T-N", "M14")
        w.marker("released", who="new")
        w.dispatch("T-N", n=2)
        w.send("T-N", n=2)
        w.marker("released", who="old")
        w.log("T-N", svc="T-N", kind="effect", call_id="c-T-N", key="k-c-T-N", result="ok")
        w.outcome("T-N", epoch=1)
        r = finish(w, tools=())
        self.assertFlags(r, "5.conflict")

    def test_TN_owner_path_correct(self):
        w, _ = self.cb2("T-N", None)
        w.marker("released", who="new")
        w.commit("TN-indoubt", "recovery", moves=[("tool_call", "c-T-N", "in_doubt")])
        w.log("owner", task="T", call_id="c-T-N", answer="did not happen")
        w.dispatch("T-N", n=2)
        w.send("T-N", n=2)
        w.marker("released", who="old")
        w.outcome("T-N")
        w.log("T-N", svc="T-N", kind="effect", call_id="c-T-N", key="k-c-T-N", attempt=1, result="ok")
        tok = w.commit("TN-dup", "outcome", epoch=1)
        w.tables["run_event"].append(
            {"id": 9, "agent": "A", "kind": "duplicate", "epoch": 1, "token": tok, "outstanding": None, "call": "c-T-N"}
        )
        r = finish(w, tools=())
        self.assertClean(r)
        self.assertIn("5.TN_duplicate_recorded", r["exercised"])
        self.assertIn("6.in_doubt_owner", r["exercised"])

    def test_TN_killed_reply_needs_no_duplicate_record(self):
        w = self.b2_tool("T-N", None)
        w.commit("TN-indoubt", "recovery", moves=[("tool_call", "c-T-N", "in_doubt")])
        w.log("owner", task="T", call_id="c-T-N", answer="did_not_happen")
        w.dispatch("T-N", n=2)
        w.send("T-N", n=2)
        w.outcome("T-N")
        r = finish(w, tools=())
        self.assertClean(r)

    def test_reply_after_dispatch_timeout_not_received(self):
        w, _ = self.cb2("T-N", None)
        w.marker("released", who="new")
        w.tick(2500)
        w.log("T-N", svc="T-N", kind="effect", call_id="c-T-N", key="k-c-T-N", attempt=1, epoch=1, result="ok")
        w.commit("TN-indoubt", "recovery", moves=[("tool_call", "c-T-N", "in_doubt")])
        w.log("owner", task="T", call_id="c-T-N", answer="happened")
        w.marker("released", who="old")
        w.outcome("T-N")
        w.derive("c-T-N")
        r = finish(w, tools=())
        self.assertNotIn("6.outcome_exempt", clauses(r))
        self.assertNotIn("5.recorded", clauses(r))

    def test_M6_reused_call_id(self):
        w = start(self.R, "C-p/a4:TI-prep", mutation="M6")
        w.prep("T-I")
        w.marker("reached", "a4:TI-prep")
        w.takeover(reject=False)
        w.marker("reached", "post-takeover")
        w.marker("stale-decision-attempt", decision="TI-disp", epoch=1)
        w.marker("stale-decision-rejected", reason="epoch")
        w.row("tool_call", id="c-T-I")["epoch"] = 2
        w.dispatch("T-I")
        w.send("T-I")
        w.outcome("T-I")
        self.assertFlags(finish(w, tools=("T-F", "T-N")), "6.prepared_rejected")

    def test_TF_stale_in_response_order(self):
        w = start(self.R, "C-s/a4:TF-out")
        w.prep("T-F")
        w.dispatch("T-F")
        w.send("T-F", epoch=2)
        w.log("T-F", svc="T-F", kind="request", call_id="c-T-F", key="k-c-T-F", attempt=1, epoch=1, read=w.read())
        w.log("T-F", svc="T-F", kind="effect", call_id="c-T-F", key="k-c-T-F", attempt=1, epoch=1, result="x")
        self.assertIn("5.TF_stale_rejected", clauses(check(w)))

    def test_TN_duplicate_across_calls(self):
        w, _ = self.cb2("T-N", None)
        w.marker("released", who="new")
        w.prep("T-N", call="c2", key="k-c-T-N")
        w.dispatch("T-N", call="c2")
        w.send("T-N", call="c2")
        w.outcome("T-N", call="c2")
        w.log("T-N", svc="T-N", kind="effect", call_id="c-T-N", key="k-c-T-N", attempt=1, epoch=1, result="ok")
        w.marker("released", who="old")
        w.outcome("T-N", epoch=1)
        w.derive("c-T-N")
        r = finish(w, tools=())
        self.assertIn("5.TN_duplicate_recorded", clauses(r))

    def test_delivered_entries(self):
        w, _ = self.cb2("T-N", None)
        w.marker("released", who="new")
        w.log("T-N", svc="T-N", kind="effect", call_id="c-T-N", key="k-c-T-N", attempt=1, epoch=1, result="ok")
        w.log("T-I", svc="T-I", kind="delivered", call_id="none")
        w.log("T-N", svc="T-N", kind="delivered", call_id="other")
        w.commit("TN-indoubt", "recovery", moves=[("tool_call", "c-T-N", "in_doubt")])
        w.log("owner", task="T", call_id="c-T-N", answer="happened")
        w.marker("released", who="old")
        w.outcome("T-N")
        r = finish(w, tools=())
        self.assertNotIn("5.recorded", clauses(r))

    def test_executor_aware_kill(self):
        w = happy(self.R, kill_after="T-N", point="B/b2:T-N")
        w.logs["markers"][-1]["data"] = {"executor": "e2"}
        r = check(w)
        self.assertNotIn("5.recorded", clauses(r))
        c = Check(w.R, *w.write(), standby_recs=w.write()[1])
        self.assertFalse(c.killed_after(0, "e1"))
        self.assertTrue(c.killed_after(0, "e2"))

    def test_attempt_history_not_counted(self):
        w = happy(self.R)
        tok = w.commit("TN-dup", "outcome")
        w.tables["history"].append(
            {
                "id": 99,
                "kind": "attempt",
                "obj": "c-T-N",
                "from_state": None,
                "to_state": "duplicate",
                "epoch": 1,
                "token": tok,
            }
        )
        w.logs["answers"] = [a for a in w.logs["answers"] if a["call"] != "reconstruct"]
        w.final()
        r = check(w)
        self.assertFalse({"3.states", "7.executed", "1.state", "2.no_lost_output"} & clauses(r), r["violations"])

    def p_run(self, mutation):
        w = World(self.R, point="P/pre-policy", mutation=mutation)
        w.accept()
        w.model()
        w.marker("reached", "pre-policy")
        w.marker("standby-stopped")
        w.sync = False
        x = w.commit("X-mem", "outcome", epoch=0)
        w.row("commit_token", token=x)["agent"] = "B"
        w.marker("injected", xid=w.xid)
        if mutation:
            w.policy()
        w.cut("pre-resume", "pre-resume")
        w.resume()
        w.marker("standby-resumed")
        if not mutation:
            w.policy()
        return finish(w)

    def test_P_correct_positive_control(self):
        r = self.p_run(None)
        self.assertClean(r)
        self.assertIn("4.policy_read", r["exercised"])

    def test_P_M5(self):
        r = self.p_run("M5")
        self.assertFlags(r, "4.policy_read")
        self.assertEqual(r["mutation"]["detected"], ["4.policy_read"])

    def ac_run(self, mutation):
        w = World(self.R, point="Ac/I-disp", mutation=mutation)
        w.accept()
        w.marker("reached", "a3:I-disp")
        w.marker("standby-stopped")
        w.sync = False
        w.marker("syncrep-cancel-on")
        if mutation:
            w.logs["gateway"] = []
            w.model()
            w.logs["gateway"] = []
            w.cut("pre-resume", "pre-resume")
            w.resume()
            w.marker("standby-resumed")
        else:
            w.commit("I-prep-x", "decision")
            w.cut("pre-resume", "pre-resume")
            w.resume()
            w.marker("standby-resumed")
            w.model()
        w.policy()
        return finish(w)

    def test_Ac_correct(self):
        self.assertClean(self.ac_run(None))

    def test_Ac_M1_without_gateway_log(self):
        r = self.ac_run("M1")
        self.assertFlags(r, "4.established")
        self.assertIn("4.established", r["mutation"]["detected"])

    def test_release_after_outcome_is_after(self):
        w, _ = self.cb2("T-I", None, release="after-outcome")
        w.marker("released", who="old")
        w.log("T-I", svc="T-I", kind="effect", call_id="c-T-I", key="k-c-T-I", result="ok")
        w.outcome("T-I", epoch=1)
        w.derive("c-T-I")
        w.marker("released", who="new")
        self.assertClean(finish(w, tools=("T-F", "T-N")))


class Conditions(Base):
    def test_missing_marker(self):
        w = happy(self.R)
        w.logs["markers"] = [m for m in w.logs["markers"] if m["event"] != "reached"]
        self.assertTrue(any(u["reason"] == "missing reached marker" for u in check(w)["unresolved"]))

    def test_never_established(self):
        w = happy(self.R)
        w.logs["cuts"][-1]["standby"]["flushed_lsn"] = "0/3000000"
        r = check(w)
        self.assertTrue(any("not D or R" in u["reason"] for u in r["unresolved"]))

    def test_commit_not_in_wal(self):
        w = happy(self.R)
        w.wal.pop(3)
        r = check(w)
        self.assertTrue(any("not found in pg_waldump" in u["reason"] for u in r["unresolved"]))

    def test_oracle_invalid_and_primary_ahead(self):
        w = happy(self.R)
        w.logs["cuts"].append({"kind": "invalid", "label": "x", "reason": "freeze over 1 s", "t": w.tick()})
        w.logs["cuts"][0]["primary"]["flush_lsn"] = "FF/0"
        r = check(w)
        self.assertTrue(r["invalid"])
        self.assertTrue(any("primary flush" in u["reason"] for u in r["unresolved"]))

    def test_receiver_read_not_sync_is_invalid(self):
        w = start(self.R, "A/a4:TI-out")
        w.prep("T-I")
        w.dispatch("T-I")
        w.send("T-I", read={"flush_lsn": None, "sync_state": "async", "walsender_pid": 11, "tli": 1, "error": None})
        w.outcome("T-I")
        w.marker("reached", "a4:TI-out")
        r = finish(w, tools=("T-F", "T-N"))
        self.assertTrue(r["invalid"])

    def test_walsender_change_turns_violation_into_invalid(self):
        w = start(self.R, "A/a3:TI-disp")
        w.prep("T-I")
        w.sync = False
        w.dispatch("T-I")
        rd = w.read()
        rd["walsender_pid"] = 99
        w.send("T-I", read=rd)
        w.marker("reached", "a3:TI-disp")
        w.kill()
        w.resume()
        w.outcome("T-I")
        r = finish(w, tools=("T-F", "T-N"))
        self.assertNotIn("4.established", clauses(r))
        self.assertTrue(any("walsender" in i["reason"] for i in r["invalid"]))

    def test_plain_arm_cs_unmet(self):
        w = happy(self.R, arm="plain1", point="C-s/a4:TI-out")
        r = check(w)
        self.assertEqual(r["clauses"]["1.stale_epoch"], "unmet")
        self.assertEqual(r["clauses"]["6.prepared_rejected"], "unmet")

    def test_gateway_disagreement(self):
        w = happy(self.R)
        w.logs["gateway"] = [
            dict(g, decision="waiting") if g.get("call_id") == "c-T-F" else g for g in w.logs["gateway"]
        ]
        self.assertFlags(check(w), "4.gateway")

    def test_denied_call_sent(self):
        w = happy(self.R, scenario="S-AP")
        w.logs["approver"].append({"t": 1, "task": "T", "call_id": "c-T-I", "answer": "deny"})
        self.assertFlags(check(w), "4.denied")


if __name__ == "__main__":
    unittest.main()


class Robustness(Base):
    def test_error_answer_unresolved(self):
        w = happy(self.R)
        w.log("answers", call="state", arg="A", answer={"error": "boom"})
        self.assertTrue(any("error" in u["reason"] for u in check(w)["unresolved"]))

    def test_partial_last_line(self):
        w = happy(self.R)
        db, recs = w.write()
        with open(w.R / "markers.jsonl", "a") as f:
            f.write('{"t": 1, "ev')
        r = Check(w.R, db_rows=db, wal_recs=recs, standby_recs=recs).check()
        self.assertEqual(r["verdict"], "pass", r["unresolved"])

    def test_missing_run_json(self):
        from check.check import main

        main(self.R)
        self.assertEqual(json.load(open(f"{self.R}/check.json"))["verdict"], "unresolved")

    def test_score(self):
        from check.score import score

        w = World(self.R, point="A/a3:C-acc", mutation="M5")
        w.sync = False
        w.accept()
        w.marker("reached", "a3:C-acc")
        w.kill()
        w.policy()
        w.cut("pre-resume", "pre-resume")
        w.resume()
        w.observe()
        r = finish(w, observe=False)
        s = score([{"mutation": "M5", "check": r}, {"mutation": "", "harness_error": "x"}])
        self.assertEqual((s["M5"]["exercised"], s["M5"]["detected"]), (1, 1))
        self.assertEqual(s["none"]["not_scored"], {"harness_error": 1})
        self.assertEqual(s["M5"]["by_clause"], {"4.policy_read": 1})
        s = score([{"mutation": "M5", "check": r, "unresolved": "run cap"}])
        self.assertEqual((s["M5"]["detected"], s["M5"]["not_scored"]), (0, {"unresolved": 1}))
