import hashlib
import json
import sys
from pathlib import Path

ACTION = "T-I,T-F,T-N"
TOOLS = ("T-I", "T-F", "T-N")
LOGS = ("stub", "T-I", "T-F", "T-N", "approver", "owner")
KINDS = ("effect", "duplicate", "rejected_stale")
ZOMBIE_T = 60 * 10**9
QUIESCE_T = 10 * 10**9
NE = ("not_exercised", "")


def canon(v):
    return json.dumps(v, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def sha(v):
    return hashlib.sha256(canon(v).encode()).hexdigest()


def get(v, *path):
    for p in path:
        if not isinstance(v, dict):
            return None
        v = v.get(p)
    return v


def ok(errs):
    return ("fail", "; ".join(errs)) if errs else ("pass", "")


def rows(path, bad):
    if not path.exists():
        return None
    text = path.read_text(errors="replace")
    if text and not text.endswith("\n"):
        bad.append(f"{path.name}: last line cut")
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        try:
            v = json.loads(line)
        except ValueError:
            v = None
        if not isinstance(v, dict):
            bad.append(f"{path.name}:{n}: not a JSON object")
        elif not isinstance(v.get("t"), int):
            bad.append(f"{path.name}:{n}: no integer t")
        else:
            out.append(v)
    return out


def tok(q):
    t = q.get("token")
    return t if t is not None else q.get("epoch")


def calls(log, name, bad):
    reqs = [
        dict(x, out=None, delivered=False) for x in log if x.get("kind") == "request"
    ]
    ids = lambda x: (x.get("call_id"), x.get("key"), x.get("attempt"))
    for x in log:
        kind = x.get("kind")
        if kind in KINDS:
            c = [q for q in reqs if q["out"] is None and ids(q) == ids(x)]
            c.sort(key=lambda q: q.get("token") != x.get("token"))
        elif kind == "delivered":
            c = [
                q for q in reqs if q["out"] and not q["delivered"] and ids(q) == ids(x)
            ]
        else:
            continue
        if not c:
            bad.append(f"{name}: {kind} line without its request {ids(x)}")
        elif kind == "delivered":
            c[0]["delivered"] = True
        else:
            c[0]["out"] = x
    return reqs


def answer(a):
    if not a or a.get("exit") != 0 or not isinstance(a.get("answer"), dict):
        return None
    return a["answer"]


class Run:
    def __init__(self, R):
        bad = self.bad = []
        try:
            run = json.loads((R / "run.json").read_text())
        except (OSError, ValueError):
            run = None
        if not isinstance(run, dict):
            bad.append("run.json missing or unreadable")
            run = {}
        self.scenario, self.seed = run.get("scenario"), run.get("seed")
        self.kind, self.boundary = run.get("kind"), run.get("boundary")
        self.unsupported = set(run.get("unsupported") or [])
        self.sap = self.scenario == "S-AP"
        odd = self.seed % 2 == 1 if isinstance(self.seed, int) else None
        if odd is None:
            bad.append("run.json: seed is not an integer")
        self.decision = ("deny" if odd else "approve") if self.sap else None
        self.deny = self.decision == "deny"

        m = rows(R / "markers.jsonl", bad)
        if m is None:
            bad.append("markers.jsonl missing")
        self.markers = m = m or []
        ev = [x.get("event") for x in m]
        svcs = {get(x, "data", "svc") for x in m if x.get("event") == "reached"}
        self.reached = {"stub" if s == "model" else s for s in svcs}
        if "posted" in ev:
            self.reached.add("approver")
        for x in m:
            if x.get("event") == "error" or (
                x.get("event") == "run-end" and get(x, "data", "status") == "error"
            ):
                bad.append(f"markers: harness error {x.get('data')}")
        self.cap = any(
            x.get("event") == "run-end" and get(x, "data", "status") == "cap" for x in m
        )
        ends = [x["t"] for x in m if x.get("event") == "end-state"]
        if "quiesced" not in ev and not self.cap:
            bad.append("markers: no quiesced")
        if self.kind == "zombie_takeover" and "stopped" in ev and "resumed" not in ev:
            bad.append("markers: zombie stopped but never resumed")

        log = {}
        for name in LOGS:
            v = rows(R / "svc" / f"{name}.jsonl", bad)
            if v is None and name in self.reached:
                bad.append(f"svc/{name}.jsonl missing")
            log[name] = v or []
        self.stub = [x for x in log["stub"] if x.get("kind") == "request"]
        self.reply = {
            x.get("request_id"): x.get("output_sha256")
            for x in log["stub"]
            if x.get("kind") == "reply"
        }
        self.stub_delivered = {
            x.get("request_id") for x in log["stub"] if x.get("kind") == "delivered"
        }
        self.tools = {t: calls(log[t], t, bad) for t in TOOLS}
        self.all_tools = [q for t in TOOLS for q in self.tools[t]]
        self.approvals = [x for x in log["approver"] if x.get("kind") == "request"]
        self.posted = [x for x in log["approver"] if x.get("kind") == "posted"]
        for p in self.posted:
            if self.sap and p.get("decision") != self.decision:
                bad.append(
                    f"approver posted {p.get('decision')}, seed says {self.decision}"
                )
        self.owner, self.owner_rows = {}, []
        for x in log["owner"]:
            if x.get("kind") == "answer" and x.get("task") == "T":
                self.owner.setdefault(x.get("call_id"), []).append(x.get("answer"))
                self.owner_rows.append(x)
                done = any(
                    y.get("kind") == "effect"
                    and y.get("call_id") == x.get("call_id")
                    and y["t"] < x["t"]
                    for y in log["T-N"]
                )
                if x.get("answer") != ("happened" if done else "did_not_happen"):
                    bad.append(f"owner answered {x.get('answer')} against T-N's log")

        a = rows(R / "answers.jsonl", bad)
        if a is None and not self.cap:
            bad.append("answers.jsonl missing")
        last = {x.get("call"): x for x in a or []}
        for c in ("state", "reconstruct"):
            if c not in last and not self.cap:
                bad.append(f"answers: no {c} call")
        if ends and any(x["t"] < ends[0] + QUIESCE_T for x in a or []):
            bad.append("answers less than 10 s after the end-state detection")
        self.state, self.recon = answer(last.get("state")), answer(
            last.get("reconstruct")
        )
        self.status = get(self.state, "task", "status")
        self.mem = get(self.state, "memory")

        self.live = [q for q in self.all_tools if kind_of(q) != "rejected_stale"]
        used = [canon(get(q, "args", "reply")) for q in self.live]
        if not self.live and isinstance(self.mem, dict) and "reply" in self.mem:
            used = [canon(self.mem["reply"])]
        self.used = set(used)
        self.u = json.loads(used[0]) if len(self.used) == 1 else None
        ti = [
            get(q, "out", "result", "effect")
            for q in self.tools["T-I"]
            if kind_of(q) == "effect"
        ]
        tf = [
            get(q, "out", "result", "effect")
            for q in self.tools["T-F"]
            if kind_of(q) == "effect"
        ]
        self.ti, self.tf = (ti[0] if len(ti) == 1 else None), (
            tf[0] if len(tf) == 1 else None
        )
        self.n_ti, self.n_tf = len(ti), len(tf)


def kind_of(q):
    return get(q, "out", "kind")


def o1_end_state(r):
    errs = []
    if get(r.state, "task", "id") != "T":
        errs.append(f"state task id {get(r.state, 'task', 'id')!r}, not 'T'")
    allowed = ("denied",) if r.deny else ("done", "stopped_in_doubt")
    if r.status not in allowed:
        errs.append(f"status {r.status!r}, allowed {allowed}")
    if r.status == "stopped_in_doubt":
        tn = r.tools["T-N"]
        if not tn:
            errs.append("stopped_in_doubt without a T-N request")
        else:
            q = tn[-1]
            if not any(owner_after(r, q, a) for a in ("happened", "did_not_happen")):
                errs.append(f"owner not asked after T-N {q.get('call_id')!r}")
            if q["delivered"] and "T-N" not in r.reached:
                errs.append("last T-N reply was delivered with no fault at T-N")
    return ok(errs)


def owner_after(r, q, answer):
    return any(
        x.get("call_id") == q.get("call_id")
        and x.get("answer") == answer
        and x["t"] > q["t"]
        for x in r.owner_rows
    )


def capped(r, res):
    if res[0] == "fail" and r.cap:
        return ("unresolved", "run capped before the answers: " + res[1])
    return res


def o1_one_reply(r):
    if not r.live:
        return ("pass", "no downstream request")
    errs = []
    if len(r.used) != 1:
        errs.append(
            f"downstream requests use {len(r.used)} reply ids: {sorted(r.used)}"
        )
    else:
        rid = r.u
        if not any(s.get("request_id") == rid for s in r.stub):
            errs.append(f"reply id {rid!r} is not a stub request")
        elif rid not in r.reply or rid not in r.stub_delivered:
            errs.append(f"reply {rid!r} was never delivered")
    return ok(errs)


def o1_no_recall(r):
    rids = {canon(s.get("request_id")) for s in r.stub}
    uses = [q["t"] for q in r.all_tools if canon(get(q, "args", "reply")) in rids]
    if not uses:
        return ("pass", "no downstream use of a reply")
    late = [s.get("request_id") for s in r.stub if s["t"] > min(uses)]
    if late:
        return ("fail", f"model called again after a downstream request: {late}")
    return ("pass", "")


def o1_takeover(r):
    if r.kind != "zombie_takeover":
        return NE
    ts = lambda e: [
        x.get("t")
        for x in r.markers
        if x.get("event") == e and isinstance(x.get("t"), int)
    ]
    stop = ts("stopped")
    if not stop:
        return ("not_exercised", "boundary never reached")
    res = [t for t in ts("resumed") if t >= stop[0]]
    if not res:
        return ("unresolved", "zombie never resumed")
    if res[0] - stop[0] >= ZOMBIE_T:
        return (
            "unresolved",
            f"no takeover within 60 s ({(res[0] - stop[0]) / 1e9:.1f} s)",
        )
    return ("pass", "")


def expected_memory(r):
    if r.deny:
        rid = get(r.mem, "reply")
        if rid not in r.stub_delivered or rid not in r.reply:
            return None, f"memory reply {rid!r} is not a delivered stub reply"
        return {"reply": rid, "denied": True}, None
    if r.status == "stopped_in_doubt":
        return None, None
    if r.u is None:
        return None, "no single reply id"
    if r.n_ti != 1 or r.n_tf != 1 or not r.tools["T-N"]:
        return (
            None,
            f"T-I effects {r.n_ti}, T-F effects {r.n_tf}, T-N requests {len(r.tools['T-N'])}",
        )
    return {"reply": r.u, "ti": r.ti, "tf": r.tf}, None


def o2_memory(r):
    if r.state is None:
        return ("fail", "no state answer")
    if r.deny or r.status == "stopped_in_doubt":
        exp, err = expected_memory(r)
        if err:
            return ("fail", err)
        return ok([] if r.mem == exp else [f"memory {r.mem!r}, derived {exp!r}"])
    if r.status not in ("done",):
        return ("fail", f"status {r.status!r}: no end state to derive memory for")
    exp, err = expected_memory(r)
    if err:
        return ("fail", err)
    q = r.tools["T-N"][-1]
    tn = []
    if kind_of(q) == "effect" and q["delivered"]:
        tn.append(get(q, "out", "result", "effect"))
    if owner_after(r, q, "happened"):
        tn.append("owner:happened")
    mem = r.mem if isinstance(r.mem, dict) else {}
    rest = {k: v for k, v in mem.items() if k != "tn"}
    if set(mem) != {"reply", "ti", "tf", "tn"} or rest != exp or mem["tn"] not in tn:
        return ("fail", f"memory {r.mem!r}, derived {exp!r} with tn in {tn!r}")
    return ("pass", "")


def o2_single_exec(r):
    errs = [
        f"{q.get('call_id')!r}: agent/task {q.get('agent')!r}/{q.get('task')!r}"
        for q in r.all_tools
        if q.get("agent") != "A" or q.get("task") != "T"
    ]
    if r.deny:
        errs += (
            [f"{len(r.all_tools)} tool requests after a denial"] if r.all_tools else []
        )
        return ok(errs)
    if not r.all_tools:
        return ok(errs)
    if r.u is None:
        return (
            "fail",
            "; ".join(errs + ["no single reply id to derive requests from"]),
        )
    exp = {
        "T-I": {"reply": r.u},
        "T-F": {"reply": r.u, "ti": r.ti} if r.n_ti == 1 else None,
        "T-N": {"reply": r.u, "tf": r.tf} if r.n_tf == 1 else None,
    }
    for t in TOOLS:
        qs = [q for q in r.tools[t] if kind_of(q) != "rejected_stale"]
        if not qs:
            continue
        if exp[t] is None:
            errs.append(f"{t} sent, but its input effect is not unique")
            continue
        bad = [q.get("args") for q in qs if canon(q.get("args")) != canon(exp[t])]
        if bad:
            errs.append(f"{t} args {bad!r}, derived {exp[t]!r}")
        keys = {canon(q.get("key")) for q in qs}
        if t != "T-N" and len(keys) > 1:
            errs.append(f"{t} sent under {len(keys)} keys")
    return ok(errs)


def tools_clause(r, t):
    return None if r.tools[t] else (NE if r.deny else ("pass", f"no {t} request"))


def o3_ti_once(r):
    return tools_clause(r, "T-I") or ok(
        [] if r.n_ti == 1 else [f"{r.n_ti} T-I effects for one logical call"]
    )


def o3_tn_no_repeat(r):
    tn = r.tools["T-N"]
    if not tn:
        return tools_clause(r, "T-N")
    taken, errs = set(), []
    for prev, q in zip(tn, tn[1:]):
        a = next(
            (
                i
                for i, x in enumerate(r.owner_rows)
                if i not in taken
                and x.get("call_id") == prev.get("call_id")
                and x.get("answer") == "did_not_happen"
                and prev["t"] < x["t"] < q["t"]
            ),
            None,
        )
        if a is not None:
            taken.add(a)
        else:
            errs.append(
                f"T-N {q.get('call_id')!r}/{q.get('attempt')!r} sent after "
                f"{prev.get('call_id')!r} without owner did_not_happen"
            )
    return ok(errs)


def o3_tf_stale(r):
    tf = r.tools["T-F"]
    if not tf:
        return tools_clause(r, "T-F")
    hi, errs = {}, []
    for q in tf:
        t, agent = tok(q), q.get("agent")
        if not isinstance(t, int) or isinstance(t, bool):
            errs.append(f"T-F {q.get('call_id')!r} without an integer token")
            continue
        if agent in hi and t < hi[agent] and kind_of(q) != "rejected_stale":
            errs.append(f"stale token {t} < {hi[agent]} answered {kind_of(q)}")
        hi[agent] = max(hi.get(agent, t), t)
    return ok(errs)


def o3_deny_no_tool(r):
    if not r.deny:
        return NE
    return ok(
        [f"{len(r.all_tools)} tool requests after a denial"] if r.all_tools else []
    )


def model_pairs(r):
    ents = get(r.recon, "model")
    if not isinstance(ents, list):
        return None, ["reconstruct.model missing"]
    free, pairs, errs = [e for e in ents if isinstance(e, dict)], [], []
    if len(free) != len(ents):
        errs.append("model entry that is not an object")
    for s in r.stub:
        rid, inv = s.get("request_id"), s.get("invocation_id")
        c = [
            e
            for e in free
            if e.get("invocation_id") == inv and e.get("request_id") in (rid, None)
        ]
        c.sort(key=lambda e: e.get("request_id") is None)
        if not c:
            errs.append(f"stub request {rid!r} (invocation {inv!r}) missing")
            continue
        free.remove(c[0])
        pairs.append((s, c[0]))
    if free:
        errs.append(f"{len(free)} model entries with no stub request")
    return pairs, errs


def o4_model(r):
    pairs, errs = model_pairs(r)
    for s, e in pairs or []:
        rid = s.get("request_id")
        if e.get("input") is None or sha(e["input"]) != s.get("body_sha256"):
            errs.append(f"{rid!r}: input hash differs from body_sha256")
        out = e.get("output_sha256")
        if out is not None and out != r.reply.get(rid):
            errs.append(f"{rid!r}: output_sha256 differs from the stub reply")
    return ok(errs)


def o4_model_used(r):
    pairs, errs = model_pairs(r)
    if pairs is None:
        return ok(errs)
    errs = []
    for s, e in pairs:
        rid = s.get("request_id")
        used = canon(rid) in r.used
        if e.get("used") is not used:
            errs.append(f"{rid!r}: used {e.get('used')!r}, logs say {used}")
        if used and (
            e.get("request_id") != rid or e.get("output_sha256") != r.reply.get(rid)
        ):
            errs.append(f"{rid!r}: used reply without its request_id or output_sha256")
    return ok(errs)


def attempt_ok(r, t, q, later, o):
    if o is not None and o == kind_of(q):
        return True
    if o == "unknown":
        return not q["delivered"] or t in r.reached or later
    return soft_ok(r, t, q.get("call_id"), o)


def soft_ok(r, t, call_id, o):
    if o == "unknown":
        return True
    return (
        t == "T-N"
        and isinstance(o, str)
        and o.startswith("owner:")
        and o[6:] in r.owner.get(call_id, [])
    )


def o4_calls(r):
    ents = get(r.recon, "calls")
    if not isinstance(ents, list):
        return ("fail", "reconstruct.calls missing")
    ents = [e if isinstance(e, dict) else {} for e in ents]
    att = [
        e.get("attempts") if isinstance(e.get("attempts"), list) else [] for e in ents
    ]
    claimed, errs = {}, []
    for t in TOOLS:
        qs = r.tools[t]
        for n, q in enumerate(qs):
            same = lambda e: (
                e.get("tool") == t
                and e.get("call_id") == q.get("call_id")
                and e.get("key") == q.get("key")
                and e.get("token") == tok(q)
                and canon(e.get("args")) == canon(q.get("args"))
            )
            hit = next(
                (
                    (i, j)
                    for i, e in enumerate(ents)
                    if same(e)
                    for j, a in enumerate(att[i])
                    if (i, j) not in claimed and get(a, "attempt") == q.get("attempt")
                ),
                None,
            )
            name = f"{t} {q.get('call_id')!r} attempt {q.get('attempt')!r}"
            if hit is None:
                errs.append(f"{name}: not in reconstruct (or call fields differ)")
                continue
            claimed[hit] = q
            later = any(t == "T-N" or x.get("key") == q.get("key") for x in qs[n + 1 :])
            o = get(att[hit[0]][hit[1]], "outcome")
            if not attempt_ok(r, t, q, later, o):
                errs.append(f"{name}: outcome {o!r}, logged {kind_of(q)!r}")
    for i, e in enumerate(ents):
        if not att[i]:
            errs.append(f"call entry {e.get('call_id')!r} without attempts")
            continue
        for j, a in enumerate(att[i]):
            if (i, j) not in claimed and not soft_ok(
                r, e.get("tool"), e.get("call_id"), get(a, "outcome")
            ):
                errs.append(
                    f"call {e.get('call_id')!r} attempt {get(a, 'attempt')!r}: not in the logs"
                )
        num = lambda j: (
            get(att[i][j], "attempt")
            if isinstance(get(att[i][j], "attempt"), int)
            else -1
        )
        last = max(range(len(att[i])), key=num)
        want = get(att[i][last], "outcome")
        if get(e, "outcome", "kind") != want:
            errs.append(
                f"call {e.get('call_id')!r}: outcome {get(e, 'outcome', 'kind')!r}, last attempt {want!r}"
            )
        elif want in KINDS and canon(get(e, "outcome", "result")) != canon(
            get(claimed.get((i, last)), "out", "result")
        ):
            errs.append(
                f"call {e.get('call_id')!r}: outcome result differs from the log"
            )
    return ok(errs)


def policy(r):
    p = get(r.recon, "policy")
    return p if isinstance(p, list) else None


def o4_policy(r):
    p = policy(r)
    if p is None or len(p) != 1 or not isinstance(p[0], dict):
        return ("fail", f"expected one policy decision, got {p!r}")
    p, errs = p[0], []
    want = {
        "rule_version": "p1",
        "action": ACTION,
        "result": "needs_approval" if r.sap else "allow",
    }
    errs += [f"{k} {p.get(k)!r}, want {v!r}" for k, v in want.items() if p.get(k) != v]
    if not r.sap and p.get("approval") is not None:
        errs.append(f"approval {p.get('approval')!r} where the rule needs none")
    return ok(errs)


def o4_approval(r):
    if not r.sap:
        return NE
    p, errs = policy(r) or [], []
    got = [get(x, "approval", "decision") for x in p if isinstance(x, dict)]
    if got != [r.decision]:
        errs.append(f"approval decisions {got!r}, logs say {r.decision!r}")
    if not any(x.get("task") == "T" and x.get("action") == ACTION for x in r.approvals):
        errs.append("no approval request in the approver log")
    if not any(x.get("decision") == r.decision for x in r.posted):
        errs.append("decision never posted")
    if r.deny and get(r.recon, "calls") != []:
        errs.append(f"denied task reconstructs calls {get(r.recon, 'calls')!r}")
    return ok(errs)


def o4_retention(r):
    a, b = get(r.recon, "retention"), get(r.state, "retention")
    if a is None or b is None or a != b:
        return ("fail", f"retention reconstruct {a!r}, state {b!r}")
    return ("pass", "")


def o5_accepted(r):
    if not r.sap:
        return NE
    errs = [
        f"approval request {x.get('task')!r}/{x.get('action')!r}"
        for x in r.approvals
        if x.get("task") != "T" or x.get("action") != ACTION
    ]
    if not r.approvals:
        errs.append("the arm never asked the approver")
    if not r.posted:
        errs.append("no decision posted")
    errs += [
        f"deliver exited {p.get('exit')!r}" for p in r.posted if p.get("exit") != 0
    ]
    return ok(errs)


def o5_once(r):
    if not r.sap:
        return NE
    got = [
        get(x, "approval") for x in policy(r) or [] if get(x, "approval") is not None
    ]
    if len(got) != 1 or get(got[0], "decision") != r.decision:
        return ("fail", f"approval recorded {got!r}; one {r.decision!r} expected")
    return ("pass", "")


def o5_waits(r):
    if not r.sap:
        return NE
    if not r.all_tools:
        return ("pass", "no tool request")
    posted = [
        p["t"]
        for p in r.posted
        if p.get("task") == "T" and p.get("decision") == r.decision
    ]
    if not posted:
        return ("fail", "tool requests with no posted decision")
    early = [q.get("call_id") for q in r.all_tools if q["t"] < posted[0]]
    return ok(
        [f"tool requests before the decision was posted: {early}"] if early else []
    )


def o5_proceeds(r):
    if not r.sap:
        return NE
    if r.deny:
        return ok([] if r.status == "denied" else [f"denied task status {r.status!r}"])
    errs = (
        []
        if r.status in ("done", "stopped_in_doubt")
        else [f"approved task status {r.status!r}"]
    )
    if not r.tools["T-I"]:
        errs.append("approved task sent no T-I request")
    return ok(errs)


CLAUSES = [
    ("O-1.end_state", o1_end_state),
    ("O-1.one_reply", o1_one_reply),
    ("O-1.no_recall", o1_no_recall),
    ("O-1.takeover", o1_takeover),
    ("O-2.memory", o2_memory),
    ("O-2.single_exec", o2_single_exec),
    ("O-3.ti_once", o3_ti_once),
    ("O-3.tn_no_repeat", o3_tn_no_repeat),
    ("O-3.tf_stale", o3_tf_stale),
    ("O-3.deny_no_tool", o3_deny_no_tool),
    ("O-4.model", o4_model),
    ("O-4.model_used", o4_model_used),
    ("O-4.calls", o4_calls),
    ("O-4.policy", o4_policy),
    ("O-4.approval", o4_approval),
    ("O-4.retention", o4_retention),
    ("O-5.accepted", o5_accepted),
    ("O-5.once", o5_once),
    ("O-5.waits", o5_waits),
    ("O-5.proceeds", o5_proceeds),
]
CAPPED = (
    "O-1.end_state",
    "O-2.memory",
    "O-4.model",
    "O-4.model_used",
    "O-4.calls",
    "O-4.policy",
    "O-4.approval",
    "O-4.retention",
    "O-5.once",
    "O-5.proceeds",
)
ROLLUP = ("fail", "unresolved", "unsupported", "pass", "not_exercised")


def check(R):
    r = Run(Path(R))
    out, roll = [], {}
    for cid, f in CLAUSES:
        try:
            status, detail = f(r)
            if cid in CAPPED:
                status, detail = capped(r, (status, detail))
        except (TypeError, AttributeError, KeyError, ValueError, IndexError) as e:
            status, detail = "fail", f"malformed answer: {e!r}"
        g = cid.split(".")[0]
        if cid in r.unsupported:
            roll.setdefault(g, []).append(
                "not_exercised" if status == "not_exercised" else "unsupported"
            )
            status, detail = (
                "unsupported",
                f"listed in run.json unsupported (computed {status})",
            )
        else:
            roll.setdefault(g, []).append(status)
        out.append({"id": cid, "guarantee": g, "status": status, "detail": detail})
    guarantees = {g: next(s for s in ROLLUP if s in v) for g, v in roll.items()}
    sts = {c["status"] for c in out}
    verdict = (
        "invalid"
        if r.bad
        else (
            "violation"
            if "fail" in sts
            else "unresolved" if "unresolved" in sts else "pass"
        )
    )
    res = {"verdict": verdict, "guarantees": guarantees, "clauses": out}
    if r.bad:
        res["invalid"] = r.bad
    return res


def main(argv):
    R = Path(argv[1])
    res = check(R)
    text = json.dumps(res, indent=1, ensure_ascii=False)
    print(text)
    if R.is_dir():
        (R / "check.json").write_text(text + "\n")


if __name__ == "__main__":
    main(sys.argv)
