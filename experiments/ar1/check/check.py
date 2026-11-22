import hashlib
import json
import sys
import traceback
from pathlib import Path

from check import wal
from check.oracle import lsn_int

TOOLS = ("T-I", "T-F", "T-N")
SVC = ("stub",) + TOOLS
DISP = {"model": "I-disp", "T-I": "TI-disp", "T-F": "TF-disp", "T-N": "TN-disp"}
OUT = {"model": "I-out", "T-I": "TI-out", "T-F": "TF-out", "T-N": "TN-out"}
SNC_TABLES = (
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
GROUPS = ("A", "B", "C-a1", "C-b2", "C-p", "C-s", "D", "E", "R-takeover", "R-two")
INV_MOVES = {
    (None, "prepared"),
    ("prepared", "dispatched"),
    ("prepared", "abandoned"),
    ("dispatched", "accepted"),
    ("dispatched", "output"),
    ("dispatched", "result_unknown"),
    ("accepted", "output"),
    ("accepted", "result_unknown"),
    ("result_unknown", "output"),
    ("result_unknown", "result_unknown"),
}
TERMINAL = ("output", "completed", "rejected")
DISPATCH_S = 2.0
NON_DECISION_KINDS = ("derive", "attempt")
MUTATION_CLAUSES = {
    "M1": ("4.established", "4.gateway"),
    "M2": ("1.stale_epoch",),
    "M3": ("3.no_regenerate", "3.crash_unknown"),
    "M4": ("5.TN_no_retry", "2.no_repeat"),
    "M5": ("4.policy_read",),
    "M6": ("6.prepared_rejected",),
    "M7": ("7.executed",),
    "M8": ("5.recorded", "6.outcome_exempt"),
    "M9": ("5.TI_same_key", "5.TI_one_effect", "2.no_repeat"),
    "M10": ("1.stale_epoch",),
    "M11": ("6.outstanding",),
    "M12": ("3.recall", "3.late_marked"),
    "M13": ("6.derive",),
    "M14": ("5.conflict",),
}
ORACLE_CLAUSES = ("1.lower", "1.upper", "1.prefix", "4.established", "4.policy_read")
OPEN = ("prepared", "dispatched", "in_doubt", "result_unknown")
CLAUSES = [
    "1.lower",
    "1.upper",
    "1.prefix",
    "1.state",
    "1.stale_epoch",
    "2.end_state",
    "2.no_repeat",
    "2.no_lost_output",
    "3.hash",
    "3.states",
    "3.crash_unknown",
    "3.recall",
    "3.late_marked",
    "3.no_regenerate",
    "4.denied",
    "4.mapped",
    "4.established",
    "4.policy_read",
    "4.gateway",
    "5.TI_one_effect",
    "5.TI_same_key",
    "5.TN_no_retry",
    "5.TN_duplicate_recorded",
    "5.TF_stale_rejected",
    "5.recorded",
    "5.conflict",
    "6.prepared_rejected",
    "6.outstanding",
    "6.no_deadlock",
    "6.in_doubt_owner",
    "6.TI_status",
    "6.outcome_exempt",
    "6.derive",
    "7.reconstructed",
    "7.shown",
    "7.authorized",
    "7.executed",
    "7.committed",
]


def jl(p, bad=None):
    p = Path(p)
    if not p.exists():
        return []
    lines = [x for x in p.read_text().splitlines() if x.strip()]
    out = []
    for k, x in enumerate(lines):
        try:
            out.append(json.loads(x))
        except ValueError:
            if k < len(lines) - 1 and bad is not None:
                bad.append(f"{p.name} line {k + 1}")
    return out


def not_happened(answer):
    return str(answer).lower().replace("_", " ").replace("-", " ") in ("did not happen", "not happened", "false", "no")


def ids(xs, keys=("id", "call_id", "invocation_id")):
    out = set()
    for x in xs or []:
        if isinstance(x, dict):
            out |= {str(x[k]) for k in keys if x.get(k) is not None}
        elif x is not None:
            out.add(str(x))
    return out


def attempt_nums(xs):
    out = set()
    for x in xs or []:
        if isinstance(x, dict):
            n = x.get("n", x.get("attempt"))
            if n is not None:
                out.add(int(n))
        elif isinstance(x, (int, str)) and str(x).isdigit():
            out.add(int(x))
    return out


def parse_point(p):
    """(kind, marker point) from a matrix point id, e.g. 'C-s/TI-out' -> ('C-s', 'a4:TI-out')."""
    try:
        from ar1 import matrix

        m = next((x for x in matrix.points() if x["id"] == p), None)
        if m:
            return m["kind"], m["at"]
    except ImportError:
        pass
    p = str(p or "")
    if "/" in p:
        g, site = p.split("/", 1)
        return g, site
    return ("A" if p[:1] == "a" else "B" if p[:1] == "b" else None), p


class Check:
    def __init__(self, R, db_rows=None, wal_recs=None, standby_recs=None):
        self.R = Path(R)
        self.violations, self.unresolved, self.invalid = [], [], []
        self.exercised, self.unmet, self.bad = set(), set(), []
        self.run = json.loads((self.R / "run.json").read_text())
        self.arm = self.run.get("arm")
        self.snc = self.arm == "snc"
        self.group, self.site = parse_point(self.run.get("point"))
        b = self.bad
        self.markers = sorted(jl(self.R / "markers.jsonl", b), key=lambda m: m["t"])
        self.svc = {s: jl(self.R / "svc" / f"{s}.jsonl", b) for s in SVC}
        self.approver = jl(self.R / "approver.jsonl", b)
        self.owner = jl(self.R / "owner.jsonl", b)
        self.gateway = jl(self.R / "gateway.jsonl", b)
        self.cuts = jl(self.R / "cuts.jsonl", b)
        self.answers = sorted(jl(self.R / "answers.jsonl", b), key=lambda a: a["t"])
        self.db = db_rows if db_rows is not None else (self.read_db() if self.snc else {})
        self._wal, self._swal = wal_recs, standby_recs

    # inputs

    def read_db(self):
        import psycopg

        pg = self.run["pg"]
        rows = {}
        with psycopg.connect(
            host="127.0.0.1",
            port=pg["primary_port"],
            user="ar1",
            dbname="ar1",
            autocommit=True,
            application_name="check-ar1",
        ) as c:
            c.execute("SET default_transaction_read_only = on")
            for t in SNC_TABLES:
                try:
                    rows[t] = [r[0] for r in c.execute(f"SELECT to_jsonb(x) FROM snc.{t} x")]
                except psycopg.Error:
                    rows[t] = []
            rows["deadlocks"] = c.execute("SELECT deadlocks FROM pg_stat_database WHERE datname = 'ar1'").fetchone()[0]
        return rows

    def wal_recs(self):
        if self._wal is None:
            pg = self.run["pg"]
            self._wal = wal.dump(Path(pg["primary_data"]) / "pg_wal", pg["bin"])
        return self._wal

    def standby_recs(self):
        if self._swal is None:
            pg = self.run["pg"]
            self._swal = wal.dump(Path(pg["standby_data"]) / "pg_wal", pg["bin"])
        return self._swal

    # results

    def v(self, item, clause, **evidence):
        self.violations.append({"item": item, "clause": clause, "evidence": evidence})
        self.exercised.add(clause)

    def u(self, text, **evidence):
        self.unresolved.append({"reason": text, **evidence})

    def ex(self, clause, cond=True):
        if cond:
            self.exercised.add(clause)

    # derived facts

    def markers_of(self, event):
        return [m for m in self.markers if m["event"] == event]

    def build_tokens(self):
        toks = {}
        if self.snc:
            for r in self.db.get("commit_token", []):
                toks[r["token"]] = dict(r)
        else:
            for a in self.answers:
                ans = a.get("answer") or {}
                for c in ans.get("commits") or []:
                    toks.setdefault(c["token"], dict(c))
        commits, aborts = wal.commits(self.wal_recs())
        self.aborts = aborts
        self.wal_commits = commits
        for t, r in toks.items():
            if r.get("xid") is None:
                self.u("commit token has no xid", token=t)
                continue
            found = commits.get(wal.xid32(r["xid"]))
            if not found:
                self.u("commit record not found in pg_waldump", token=t, xid=r["xid"])
                continue
            r["tli"], r["start"], r["end"] = max(found)
        self.tokens = toks
        self.pos = sorted((r["end"], t) for t, r in toks.items() if "end" in r)

    def build_cuts(self):
        self.cut_list = [c for c in self.cuts if c.get("kind") in ("kill", "takeover", "cut", "final", "pre-resume")]
        bounds = {c["label"]: c.get("kill_boundary") for c in self.cuts if c.get("kind") == "kill_boundary"}
        for c in self.cut_list:
            if c["kind"] == "kill":
                c["kill_boundary"] = bounds.get(c["label"])
                if c["kill_boundary"] is None:
                    self.u("kill cut without kill boundary", label=c["label"])
        for c in self.cuts:
            for r in c.get("invalid") or []:
                self.invalid.append({"source": "oracle", "label": c.get("label"), "reason": r})
            if c.get("kind") == "invalid":
                self.invalid.append({"source": "oracle", "label": c.get("label"), "reason": c.get("reason")})
            for r in c.get("unresolved") or []:
                self.u(f"oracle: {r}", label=c.get("label"))
            pf, sf = lsn_int((c.get("primary") or {}).get("flush_lsn")), self.flushed(c)
            if pf is not None and sf is not None and pf > sf:
                self.u("primary flush_lsn above standby flushed_lsn", label=c.get("label"))
            if c.get("kind") == "unresolved":
                self.u(f"oracle: {c.get('reason')}", label=c.get("label"))
        self.kill_cuts = [c for c in self.cut_list if c["kind"] in ("kill", "takeover")]
        finals = [c for c in self.cut_list if c["kind"] == "final"]
        self.final = finals[-1] if finals else None
        if not self.final:
            self.u("no final cut")
        if self.markers_of("killed") + self.markers_of("restarted") and not any(
            c["kind"] == "kill" for c in self.cut_list
        ):
            self.u("kill without kill cut")
        tlis = {c.get("primary_tli") for c in self.cut_list} | set(self.wal_recs())
        tlis |= {c["standby"].get("received_tli") for c in self.cut_list if c.get("standby")}
        tlis.discard(None)
        if len(tlis) > 1:
            self.u("timeline change in a counted run", timelines=sorted(tlis))

    @staticmethod
    def flushed(c):
        return lsn_int((c.get("standby") or {}).get("flushed_lsn"))

    def at_or_below(self, tok, c):
        f = self.flushed(c)
        tli = (c.get("standby") or {}).get("received_tli")
        return f is not None and tok["end"] <= f and (tli is None or tli == tok["tli"])

    def classify(self):
        self.classes = {}
        last = self.kill_cuts[-1] if self.kill_cuts else None
        for t, r in self.tokens.items():
            if "end" not in r:
                continue
            per = {}
            for c in self.kill_cuts:
                b = lsn_int(c.get("kill_boundary"))
                per[c["label"]] = "D" if self.at_or_below(r, c) else ("L" if b is not None and r["end"] <= b else "-")
            if last is None:
                cls = "D" if self.final and self.at_or_below(r, self.final) else "-"
            elif per[last["label"]] == "D":
                cls = "D"
            else:
                later = [c for c in self.cut_list if c["t"] > last["t"]]
                cls = "R" if any(self.at_or_below(r, c) for c in later) else per[last["label"]]
            self.classes[t] = {"class": cls, "at_cuts": per, "end_lsn": wal.lsn_str(r["end"]), "tli": r["tli"]}
            if cls not in ("D", "R"):
                self.u("commit not D or R at the final cut", token=t, name=r.get("name"), cls=cls)
        self.presence_check()

    def presence_check(self):
        need = [t for t, c in self.classes.items() if c["class"] == "D" and self.kill_cuts]
        if not need:
            return
        sc, _ = wal.commits(self.standby_recs())
        for t in need:
            r = self.tokens[t]
            if not any(s == r["start"] for _, s, _ in sc.get(wal.xid32(r["xid"]), [])):
                self.u("D commit missing from the standby's WAL", token=t)

    def by_name(self, name, epoch=None):
        return [
            r
            for r in self.tokens.values()
            if r.get("name") == name and "end" in r and (epoch is None or r.get("epoch") == epoch)
        ]

    def hist(self, obj=None, all_kinds=False):
        rows = [
            dict(h)
            for h in self.db.get("history", [])
            if (obj is None or str(h.get("obj")) == str(obj)) and (all_kinds or h.get("kind") not in NON_DECISION_KINDS)
        ]
        for h in rows:
            h["_end"] = self.tokens.get(h.get("token"), {}).get("end")
        return sorted(rows, key=lambda h: (h["_end"] is None, h["_end"] or 0, str(h.get("id"))))

    def end_of(self, token):
        return self.tokens.get(token, {}).get("end")

    def requests(self, svc):
        return [e for e in self.svc[svc] if e.get("kind") == "request"]

    def responses(self, svc, kinds=("effect", "duplicate", "rejected_stale")):
        return [e for e in self.svc[svc] if e.get("kind") in kinds]

    def request_of(self, svc, x):
        k = "invocation_id" if svc == "stub" else "call_id"
        req = [
            e
            for e in self.requests(svc)
            if str(e.get(k)) == str(x.get(k))
            and e["t"] <= x["t"]
            and (x.get("attempt") is None or str(e.get("attempt")) == str(x.get("attempt")))
        ]
        return req[-1] if req else None

    def received(self, svc, x):
        """Delivered with no kill of its executor within 1 s after; without delivered entries, the old window rule."""
        k = "invocation_id" if svc == "stub" else "call_id"
        dl = [e for e in self.svc[svc] if e.get("kind") == "delivered"]
        req = self.request_of(svc, x)
        ep = (req or x).get("epoch")
        ex = None if ep is None else f"e{ep}"
        if dl:
            return any(
                str(e.get(k)) == str(x.get(k))
                and (
                    x.get("attempt") is None
                    or e.get("attempt") is None
                    or str(e.get("attempt")) == str(x.get("attempt"))
                )
                and e["t"] >= x["t"]
                and not self.killed_after(x["t"], ex, e["t"] + 1e9)
                for e in dl
            )
        if self.killed_after(x["t"], ex):
            return False
        return req is None or x["t"] - req["t"] <= DISPATCH_S * 1e9

    def killed_after(self, t, executor=None, until=float("inf")):
        return any(
            t < m["t"] < until and (executor is None or (m.get("data") or {}).get("executor") in (None, executor))
            for m in self.markers_of("killed") + self.markers_of("restarted")
        )

    def rows(self, table, **kw):
        return [r for r in self.db.get(table, []) if all(str(r.get(k)) == str(v) for k, v in kw.items())]

    def state_answers(self):
        return [
            a
            for a in self.answers
            if a.get("call") == "state"
            and isinstance(a.get("answer"), dict)
            and "refused" not in a["answer"]
            and "error" not in a["answer"]
        ]

    def answer_errors(self):
        for a in self.answers:
            if not isinstance(a.get("answer"), dict) or "error" in a["answer"]:
                self.u("interface answer is an error", call=a.get("call"), arg=a.get("arg"))
        for b in self.bad:
            self.u("unparsable log line", line=b)

    def outcome_tokens(self, obj):
        out = []
        for h in self.hist(obj):
            t = self.tokens.get(h.get("token"))
            if t and t.get("kind") == "outcome" and "end" in t and h.get("to_state") in TERMINAL:
                out.append(t)
        return out

    def outcome_any(self, obj):
        toks = {h.get("token") for h in self.hist(obj, True)}
        toks |= {r.get("token") for r in self.db.get("run_event", []) if str(obj) in json.dumps(r, default=str)}
        return [self.tokens[t] for t in toks if t in self.tokens and self.tokens[t].get("kind") == "outcome"]

    def takeovers(self):
        return sorted(
            (r for r in self.tokens.values() if r.get("kind") == "takeover" and "end" in r), key=lambda r: r["end"]
        )

    def disp_token(self, svc, e):
        if not self.snc:
            return None
        if svc == "stub":
            for h in self.hist(e.get("invocation_id")):
                if h.get("to_state") == "dispatched":
                    return self.tokens.get(h.get("token"))
            return None
        a = self.rows("attempt", call_id=e.get("call_id"), n=e.get("attempt"))
        return self.tokens.get(a[0].get("token")) if a else None

    # reached-point rule (5.4)

    def reached(self):
        if self.group not in GROUPS:
            self.u("unclassified run: unknown point", point=self.run.get("point"))
            return
        if not any(m.get("point") == self.site for m in self.markers_of("reached")):
            self.u("missing reached marker", point=self.site)
        g = self.group
        tc = self.markers_of("takeover-committed")
        if g.startswith("C"):
            if not tc and g in ("C-s", "C-p"):
                self.u("missing marker takeover-committed")
            if not [c for c in self.cut_list if c["kind"] == "takeover"]:
                self.u("missing takeover cut")
            post = [m for m in self.markers_of("reached") if m.get("point") == "post-takeover"]
            rel_new = [m for m in self.markers_of("released") if (m.get("data") or {}).get("who") == "new"]
            if post and rel_new and rel_new[-1]["t"] - post[0]["t"] > 10e9:
                self.u("post-takeover hold over 10 s")
        if g == "C-a1":
            w, rel = self.markers_of("takeover-waiting"), self.markers_of("released")
            if not w:
                self.u("missing marker takeover-waiting")
            elif rel and rel[0]["t"] < w[0]["t"]:
                self.u("wrong interleaving: old activation released before takeover waited")
        if g == "C-b2":
            self.release_order(tc)

    def release_order(self, tc):
        want = self.run.get("release") or ("after" if (int(self.run.get("seed") or 0) >> 1) & 1 == 0 else "before")
        rel = [m for m in self.markers_of("released") if (m.get("data") or {}).get("who") != "new"]
        if rel and tc:
            if (rel[0]["t"] > tc[0]["t"]) != (want == "after"):
                self.u("wrong interleaving: release order", want=want)
            return
        tk = self.takeovers()
        old = [
            r
            for r in self.tokens.values()
            if r.get("kind") == "outcome"
            and "end" in r
            and r.get("name") == OUT.get(self.site.split(":")[-1])
            and str(r.get("epoch")) == "1"
        ]
        if not tk or not old:
            return
        if want == "after" and old[0]["end"] < tk[0]["end"]:
            self.u("wrong interleaving: release order (by commit LSN)", want=want)

    def c_s(self):
        if self.group != "C-s":
            return
        tc = self.markers_of("takeover-committed")
        att = self.markers_of("stale-decision-attempt")
        rej = self.markers_of("stale-decision-rejected")
        com = self.markers_of("stale-decision-committed")
        if not att:
            self.u("missing marker stale-decision-attempt")
            return
        if tc and att[0]["t"] < tc[0]["t"]:
            self.u("wrong interleaving: stale-decision-attempt before takeover-committed")
        d = att[0].get("data") or {}
        tk = self.takeovers()
        for r in self.by_name(d.get("decision"), d.get("epoch")):
            if tk and r["end"] < tk[0]["end"]:
                self.u("wrong interleaving: stale decision committed below the takeover", token=r["token"])
        if not self.snc or self.run.get("mutation"):
            return
        if com:
            if not any(x["clause"] == "1.stale_epoch" for x in self.violations):
                self.v(1, "1.stale_epoch", source="marker stale-decision-committed", data=d)
            return
        if not rej:
            self.u("positive control: no stale-decision-rejected")
            return
        rd = rej[0].get("data") or {}
        if "epoch" not in str(rd.get("reason", "")).lower():
            self.u("positive control: rejected for another reason", runtime_reason=rd.get("reason"))
        x = rd.get("xid")
        if x is not None and wal.xid32(x) in self.wal_commits:
            self.v(1, "1.stale_epoch", source="rejected stale decision has a commit record", xid=x)

    # item 1

    def item1(self):
        obs = [c for c in self.cut_list if c["kind"] == "cut"]
        if not obs and self.group == "D" and self.final:
            obs = [self.final]
            prior = self.state_answers()[-1:]
        elif not obs:
            self.u("no item-1 observation cut")
            return
        else:
            prior = [a for a in self.state_answers() if a["t"] < obs[-1]["t"]]
        obs = obs[-1]
        if not prior:
            self.u("no state answer before the observation cut")
            return
        a = prior[-1]["answer"]
        A = {c["token"] for c in a.get("commits") or []}
        lastk = [c for c in self.kill_cuts if c["t"] < prior[-1]["t"]]
        if lastk:
            lower = {t for t, r in self.tokens.items() if "end" in r and self.at_or_below(r, lastk[-1])}
            self.ex("1.lower", bool(lower))
            for t in sorted(lower - A):
                self.v(1, "1.lower", token=t, name=self.tokens[t].get("name"), cut=lastk[-1]["label"])
        f, ins = self.flushed(obs), lsn_int(obs.get("insert_lsn"))
        self.ex("1.upper", f is not None and any(f < e <= (ins or 0) for e, _ in self.pos))
        for t in sorted(A):
            r = self.tokens.get(t)
            if r is None or "end" not in r:
                self.v(1, "1.prefix", token=t, why="answer lists a commit the run does not have")
            elif not self.at_or_below(r, obs):
                self.v(1, "1.upper", token=t, name=r.get("name"), end=wal.lsn_str(r["end"]), cut=obs["label"])
        ends = [self.tokens[t]["end"] for t in A if "end" in self.tokens.get(t, {})]
        self.ex("1.prefix", bool(ends))
        if ends:
            for e, t in self.pos:
                if e < max(ends) and t not in A:
                    self.v(1, "1.prefix", token=t, name=self.tokens[t].get("name"), why="gap in the prefix")
        self.item1_state(a, A)
        self.item1_stale()

    def item1_state(self, a, A):
        eps = [self.tokens[t].get("epoch") for t in A if self.tokens.get(t, {}).get("epoch") is not None]
        got = (a.get("agent") or {}).get("epoch")
        if eps and got is not None:
            self.ex("1.state")
            if int(got) != max(map(int, eps)):
                self.v(1, "1.state", field="agent.epoch", answer=got, derived=max(eps))
        if not self.snc:
            return
        last = {}
        for h in self.hist():
            if h.get("token") in A:
                last[str(h.get("obj"))] = h.get("to_state")
        for kind in ("calls", "invocations"):
            for oid, s in (a.get(kind) or {}).items():
                if oid in last:
                    self.ex("1.state")
                    if (s or {}).get("state") != last[oid]:
                        self.v(1, "1.state", obj=oid, answer=(s or {}).get("state"), derived=last[oid])

    def item1_stale(self):
        tk = self.takeovers()
        if self.group in ("C-s", "C-p") and not self.snc:
            self.unmet.add("1.stale_epoch")
            return
        self.ex("1.stale_epoch", bool(tk) and self.group in ("C-s", "C-p", "C-a1", "C-b2"))
        for k in tk:
            for r in self.tokens.values():
                if r.get("kind") in ("outcome", "takeover") or "end" not in r or r.get("epoch") is None:
                    continue
                if int(r["epoch"]) < int(k["epoch"]) and r["end"] > k["end"]:
                    self.v(
                        1,
                        "1.stale_epoch",
                        token=r["token"],
                        name=r.get("name"),
                        epoch=r["epoch"],
                        end=wal.lsn_str(r["end"]),
                        takeover=k["token"],
                        takeover_epoch=k["epoch"],
                        takeover_end=wal.lsn_str(k["end"]),
                    )

    # item 2

    def final_state(self):
        s = self.state_answers()
        return s[-1]["answer"] if s else {}

    def item2(self):
        names = {r.get("name") for r in self.tokens.values() if "end" in r}
        fs = self.final_state()
        tool_entries = [e for s in TOOLS for e in self.requests(s)]
        denied = any(not str(x.get("answer", "")).lower().startswith("approv") for x in self.approver)
        ok = "M-done" in names
        ok |= self.run.get("scenario") == "S-AP" and denied and "AP-dec" in names and not tool_entries
        ok |= self.group == "D" and any(r.get("kind") == "cancel" for r in self.tokens.values())
        if not ok and "TN-disp" in names and "TN-out" not in names:
            tn = {str(e.get("call_id")) for e in self.requests("T-N")}
            open_ = {
                i
                for i, c in (fs.get("calls") or {}).items()
                if (c or {}).get("state") in ("prepared", "dispatched", "in_doubt")
            }
            answered = {str(o.get("call_id")) for o in self.owner}
            ok = bool(open_) and open_ <= tn and not open_ & answered
        self.ex("2.end_state")
        if not ok:
            self.v(2, "2.end_state", commits=sorted(n for n in names if n), final=fs.get("task"))
        disrupted = bool(self.kill_cuts) or self.group == "D"
        for s in TOOLS:
            eff = self.responses(s, ("effect",))
            if s == "T-N":
                allow = 1 + sum(not_happened(o.get("answer")) for o in self.owner)
            else:
                allow = 1
            self.ex("2.no_repeat", bool(eff) and disrupted)
            if len(eff) > allow:
                self.v(
                    2,
                    "2.no_repeat",
                    tool=s,
                    effects=len(eff),
                    allowed=allow,
                    calls=sorted({str(e.get("call_id")) for e in eff}),
                )
        self.item2_lost()

    def item2_lost(self):
        seen = {}
        for a in self.state_answers():
            ans = a["answer"]
            for kind, term in (("calls", ("completed", "rejected")), ("invocations", ("output",))):
                for oid, s in (ans.get(kind) or {}).items():
                    s = s or {}
                    if oid in seen:
                        self.ex("2.no_lost_output")
                        if (s.get("state"), s.get("result")) != seen[oid]:
                            self.v(
                                2,
                                "2.no_lost_output",
                                obj=oid,
                                before=seen[oid],
                                after=[s.get("state"), s.get("result")],
                            )
                    elif s.get("state") in term:
                        seen[oid] = (s.get("state"), s.get("result"))
        if not self.snc:
            return
        for table, term in (("tool_call", ("completed", "rejected")), ("invocation", ("output",))):
            for row in self.db.get(table, []):
                if self.outcome_tokens(row["id"]):
                    self.ex("2.no_lost_output")
                    if row.get("state") not in term:
                        self.v(2, "2.no_lost_output", obj=row["id"], state=row.get("state"))

    # item 3

    def item3(self):
        reqs = self.requests("stub")
        replies = {}
        for e in self.responses("stub", ("reply",)):
            replies.setdefault(str(e.get("invocation_id")), []).append(e)
        inv = {str(r["id"]): r for r in self.db.get("invocation", [])}
        fs_inv = self.final_state().get("invocations") or {}
        seen = {}
        for e in reqs:
            i = str(e.get("invocation_id"))
            self.ex("3.hash")
            body = e.get("body")
            if isinstance(body, str) and hashlib.sha256(body.encode()).hexdigest() != e.get("body_sha256"):
                self.u("stub log body does not match its body_sha256", invocation=i)
            if self.snc:
                wh = (inv.get(i) or {}).get("whole_hash")
                if wh != e.get("body_sha256"):
                    self.v(3, "3.hash", invocation=i, whole_hash=wh, stub=e.get("body_sha256"))
            if i in seen:
                self.v(3, "3.recall", invocation=i, why="invocation id sent twice")
            elif seen:
                self.ex("3.recall")
                if e.get("retry_of") is None or str(e.get("retry_of")) not in seen:
                    self.v(3, "3.recall", invocation=i, retry_of=e.get("retry_of"), why="re-call without retry_of")
                if self.snc and str((inv.get(i) or {}).get("retry_of")) != str(e.get("retry_of")):
                    self.v(
                        3,
                        "3.recall",
                        invocation=i,
                        db_retry_of=(inv.get(i) or {}).get("retry_of"),
                        stub_retry_of=e.get("retry_of"),
                    )
            seen[i] = e
            if self.killed_after(e["t"], f"e{e.get('epoch')}") and self.site in ("b2:model", "b3:model"):
                outs = [
                    t
                    for t in (self.outcome_tokens(i) if self.snc else self.by_name("I-out"))
                    if t.get("name") == "I-out"
                ]
                bounds = [lsn_int(c.get("kill_boundary")) for c in self.kill_cuts if c["t"] > e["t"]]
                if not any(b is not None and t["end"] <= b for t in outs for b in bounds):
                    self.ex("3.crash_unknown")
                    st = inv.get(i) or fs_inv.get(i) or {}
                    if not (st.get("state") == "result_unknown" or (st.get("state") == "output" and st.get("late"))):
                        self.v(3, "3.crash_unknown", invocation=i, state=st.get("state"), late=st.get("late"))
        self.item3_db(inv, replies)
        for a in self.state_answers():
            for i, s in (a["answer"].get("invocations") or {}).items():
                if (s or {}).get("state") == "output":
                    self.ex("3.no_regenerate")
                    if i not in replies:
                        self.v(3, "3.no_regenerate", invocation=i, why="output shown with no stub reply")

    def item3_db(self, inv, replies):
        for i, row in inv.items():
            prev = None
            hs = self.hist(i)
            for h in hs:
                self.ex("3.states")
                if (prev, h.get("to_state")) not in INV_MOVES or (h.get("from_state") not in (None, prev)):
                    self.v(
                        3,
                        "3.states",
                        invocation=i,
                        move=[prev, h.get("to_state")],
                        recorded_from=h.get("from_state"),
                        token=h.get("token"),
                    )
                prev = h.get("to_state")
            late = any(a.get("to_state") == "result_unknown" for a in hs) and row.get("state") == "output"
            self.ex("3.late_marked", late)
            if late and not row.get("late"):
                self.v(3, "3.late_marked", invocation=i)
            if row.get("state") == "output":
                self.ex("3.no_regenerate")
                rs = replies.get(i, [])
                if not rs:
                    self.v(3, "3.no_regenerate", invocation=i, why="output with no stub reply for this id")
                elif str(row.get("provider_request_id")) not in {str(r.get("request_id")) for r in rs}:
                    self.v(
                        3,
                        "3.no_regenerate",
                        invocation=i,
                        provider_request_id=row.get("provider_request_id"),
                        stub=[r.get("request_id") for r in rs],
                    )
                out = row.get("output")
                if (
                    isinstance(out, str)
                    and all(r.get("output_sha256") for r in rs)
                    and rs
                    and hashlib.sha256(out.encode()).hexdigest() not in {r["output_sha256"] for r in rs}
                ):
                    self.v(3, "3.no_regenerate", invocation=i, why="output hash differs from the stub reply")

    # item 4

    def walsender_changed(self):
        pids = {(e.get("read") or {}).get("walsender_pid") for s in SVC for e in self.requests(s)}
        pids |= {(c.get("primary") or {}).get("walsender_pid") for c in self.cut_list}
        pids.discard(None)
        return len(pids) > 1

    def item4(self):
        deny_log = [x for x in self.approver if not str(x.get("answer", "")).lower().startswith("approv")]
        denied = {str(x.get("call_id")) for x in deny_log}
        denied |= {
            str(p.get("call_id"))
            for p in self.db.get("policy_decision", [])
            if str(p.get("result", "")).lower().startswith("den")
        }
        deny_all = self.run.get("scenario") == "S-AP" and bool(deny_log)
        self.ex("4.denied", bool(denied))
        for s in TOOLS:
            for e in self.requests(s):
                if deny_all or str(e.get("call_id")) in denied:
                    self.v(4, "4.denied", tool=s, call_id=e.get("call_id"))
        changed = self.walsender_changed()
        for s in SVC:
            name = "I-prep" if s == "stub" and not self.snc else DISP["model" if s == "stub" else s]
            seq = sorted(self.by_name(name), key=lambda r: r["end"])
            for k, e in enumerate(self.requests(s)):
                self.ex("4.mapped")
                tok = self.disp_token(s, e)
                if self.snc:
                    if tok is None:
                        self.v(4, "4.mapped", svc=s, entry=self.entry_id(e), why="no dispatched record")
                    elif s != "stub":
                        self.policy_link(s, e)
                else:
                    tok = seq[k] if k < len(seq) else None
                    if tok is None:
                        self.v(4, "4.mapped", svc=s, entry=self.entry_id(e), why=f"no {name} commit for this entry")
                self.read_check(s, e, tok, changed)
                self.gateway_check(s, e, tok)
        self.policy_read()

    @staticmethod
    def entry_id(e):
        return {k: e.get(k) for k in ("invocation_id", "call_id", "attempt", "epoch", "t") if k in e}

    def policy_link(self, s, e):
        tc = self.rows("tool_call", id=e.get("call_id"))
        if not tc:
            self.v(4, "4.mapped", svc=s, entry=self.entry_id(e), why="no tool_call row")
            return
        tc = tc[0]
        pol = self.rows("policy_decision", id=tc.get("policy_id")) if tc.get("policy_id") is not None else []
        if not pol or str(pol[0].get("result", "")).lower().startswith("den"):
            self.v(
                4,
                "4.mapped",
                svc=s,
                entry=self.entry_id(e),
                why="no allowing policy decision",
                policy=tc.get("policy_id"),
            )
        if self.run.get("scenario") == "S-AP":
            ap = self.rows("approval", id=tc.get("approval_id")) if tc.get("approval_id") is not None else []
            if not ap or not str(ap[0].get("decision", "")).lower().startswith("approv"):
                self.v(4, "4.mapped", svc=s, entry=self.entry_id(e), why="no approval", approval=tc.get("approval_id"))
            if not any(str(x.get("answer", "")).lower().startswith("approv") for x in self.approver):
                self.v(4, "4.mapped", svc=s, entry=self.entry_id(e), why="approver log has no approval")

    def read_check(self, s, e, tok, changed):
        rd = e.get("read") or {}
        if rd.get("error") or rd.get("flush_lsn") is None or rd.get("sync_state") != "sync":
            self.invalid.append(
                {
                    "source": f"receiver {s}",
                    "reason": "receiver-side read null, failed or not sync",
                    "read": rd,
                    "entry": self.entry_id(e),
                }
            )
            return
        if tok is None or "end" not in tok:
            return
        self.ex("4.established")
        ok = lsn_int(rd["flush_lsn"]) >= tok["end"] and (rd.get("tli") is None or int(rd["tli"]) == tok["tli"])
        if ok:
            return
        if changed:
            self.invalid.append(
                {
                    "source": f"receiver {s}",
                    "reason": "read below commit after a walsender change",
                    "entry": self.entry_id(e),
                }
            )
        else:
            self.v(
                4,
                "4.established",
                svc=s,
                entry=self.entry_id(e),
                read=rd,
                commit=tok.get("token"),
                commit_end=wal.lsn_str(tok["end"]),
            )

    def gateway_check(self, s, e, tok):
        if not self.gateway:
            return
        key = ("invocation_id", e.get("invocation_id")) if s == "stub" else ("call_id", e.get("call_id"))
        g = [
            x
            for x in self.gateway
            if str(x.get(key[0])) == str(key[1])
            and x["t"] < e["t"]
            and (s == "stub" or x.get("attempt") is None or str(x.get("attempt")) == str(e.get("attempt")))
        ]
        self.ex("4.gateway")
        if not g or g[-1].get("decision") != "established":
            self.v(
                4,
                "4.gateway",
                svc=s,
                entry=self.entry_id(e),
                why="no established gateway decision before the entry",
                gateway=g[-1] if g else None,
            )
            return
        x = g[-1]
        if (
            x.get("sync_state") != "sync"
            or lsn_int(x.get("flush_lsn")) is None
            or lsn_int(x["flush_lsn"]) < (lsn_int(x.get("bound")) or 0)
        ):
            self.v(
                4,
                "4.gateway",
                svc=s,
                entry=self.entry_id(e),
                why="gateway established contrary to its own read",
                gateway=x,
            )
        if tok and "end" in tok and (lsn_int(x.get("bound")) or 0) < tok["end"]:
            self.v(
                4,
                "4.gateway",
                svc=s,
                entry=self.entry_id(e),
                why="gateway bound below the commit end",
                gateway=x,
                commit_end=wal.lsn_str(tok["end"]),
            )

    def policy_read(self):
        for p in self.by_name("P-dec"):
            own = [
                r
                for r in self.tokens.values()
                if "end" in r
                and r["end"] <= p["start"]
                and (r.get("epoch") == p.get("epoch") or r.get("kind") == "takeover")
            ]
            for c in self.cut_list:
                ins = lsn_int(c.get("insert_lsn"))
                pend = [r for r in own if not self.at_or_below(r, c) and ins is not None and r["end"] <= ins]
                self.ex("4.policy_read", bool(pend) and p["start"] <= ins)
                if pend and p["end"] <= ins:
                    self.v(
                        4, "4.policy_read", policy=p["token"], cut=c["label"], unestablished=[r["token"] for r in pend]
                    )
                    return

    # item 5

    def item5(self):
        ti = self.requests("T-I")
        eff = self.responses("T-I", ("effect",))
        self.ex("5.TI_one_effect", bool(eff))
        if len({str(e.get("key")) for e in eff}) > 1 or len(eff) > 1:
            self.v(5, "5.TI_one_effect", effects=[self.entry_id(e) | {"key": e.get("key")} for e in eff])
        keys = {}
        for e in ti:
            keys.setdefault(str(e.get("call_id")), set()).add(str(e.get("key")))
        if self.snc:
            for r in self.db.get("tool_call", []):
                if r.get("tool") == "T-I" and str(r["id"]) in keys:
                    keys[str(r["id"])].add(str(r.get("key")))
        for c, ks in keys.items():
            self.ex("5.TI_same_key", len(ti) > 1 or bool(self.kill_cuts))
            if len(ks) > 1:
                self.v(5, "5.TI_same_key", call_id=c, keys=sorted(ks))
        self.tn()
        self.tf()
        self.recorded()
        self.conflict()

    def tn(self):
        reqs = self.requests("T-N")
        by_call, attempts = {}, set()
        for e in reqs:
            by_call.setdefault(str(e.get("call_id")), []).append(e)
            a = (str(e.get("call_id")), str(e.get("attempt")))
            if a in attempts:
                self.v(5, "5.TN_no_retry", entry=self.entry_id(e), why="two entries for one attempt")
            attempts.add(a)
        for c, es in by_call.items():
            self.ex("5.TN_no_retry", bool(self.kill_cuts) or len(es) > 1)
            nh = sorted(o["t"] for o in self.owner if str(o.get("call_id")) == c and not_happened(o.get("answer")))
            for k, e in enumerate(es[1:]):
                if k >= len(nh) or nh[k] > e["t"]:
                    self.v(5, "5.TN_no_retry", call_id=c, entry=self.entry_id(e), owner_not_happened=len(nh))
        by_key = {}
        for x in self.responses("T-N", ("effect",)):
            if self.received("T-N", x):
                by_key.setdefault(str(x.get("key")), []).append(x)
        for k, effs in by_key.items():
            if len(effs) > 1:
                self.ex("5.TN_duplicate_recorded")
                calls = sorted({str(x.get("call_id")) for x in effs})
                if not any(self.duplicate_recorded(c) for c in calls):
                    self.v(5, "5.TN_duplicate_recorded", key=k, calls=calls, effects=len(effs))

    def duplicate_recorded(self, c):
        if self.snc:
            rows = self.hist(c, True) + [r for r in self.db.get("run_event", []) if c in json.dumps(r)]
            rows += self.rows("tool_call", id=c)
            return any("duplicate" in json.dumps(r).lower() for r in rows)
        rec = [a["answer"] for a in self.answers if a.get("call") == "reconstruct" and str(a.get("arg")) == c]
        return any("duplicate" in json.dumps(r).lower() for r in rec)

    def tf(self):
        seen = 0
        for x in self.responses("T-F"):
            req = self.request_of("T-F", x)
            ep = x.get("epoch", (req or {}).get("epoch"))
            if ep is None:
                continue
            ep = int(ep)
            if ep < seen:
                self.ex("5.TF_stale_rejected")
                if x.get("kind") != "rejected_stale":
                    self.v(5, "5.TF_stale_rejected", entry=self.entry_id(x), why="tool did not reject a stale request")
                elif self.received("T-F", x) and self.snc:
                    if not any(h.get("to_state") == "rejected" for h in self.hist(x.get("call_id"), True)):
                        self.v(5, "5.TF_stale_rejected", entry=self.entry_id(x), why="rejection not recorded")
            elif x.get("kind") != "rejected_stale":
                seen = max(seen, ep)

    def recorded(self):
        for s in TOOLS:
            for x in self.responses(s):
                if not self.received(s, x):
                    continue
                self.ex("5.recorded")
                self.ex("6.outcome_exempt", self.group in ("C-b2", "D"))
                c = str(x.get("call_id"))
                outs = self.outcome_tokens(c) if self.snc else self.by_name(OUT[s])
                if not outs:
                    self.v(
                        5,
                        "5.recorded",
                        tool=s,
                        entry=self.entry_id(x) | {"kind": x.get("kind")},
                        why="tool response with no recorded outcome",
                    )
                    continue
                req = [
                    e
                    for e in self.requests(s)
                    if str(e.get("call_id")) == c
                    and e["t"] <= x["t"]
                    and (x.get("attempt") is None or str(e.get("attempt")) == str(x.get("attempt")))
                ]
                ep = req[-1].get("epoch") if req else None
                if ep is not None and self.snc and not any(str(o.get("epoch")) == str(ep) for o in self.outcome_any(c)):
                    self.v(
                        6,
                        "6.outcome_exempt",
                        tool=s,
                        call_id=c,
                        epoch=ep,
                        outcomes=[(o["token"], o.get("epoch")) for o in outs],
                    )
        for x in self.responses("stub", ("reply",)):
            if not self.received("stub", x):
                continue
            self.ex("5.recorded")
            i = str(x.get("invocation_id"))
            outs = self.outcome_tokens(i) if self.snc else self.by_name("I-out")
            if not any(o.get("name") == "I-out" for o in outs):
                self.v(5, "5.recorded", svc="stub", invocation=i, why="stub reply with no committed output")

    def resolved_before(self, s, r1, r2):
        c = str(r1.get("call_id"))
        if any(str(o.get("call_id")) == c and o["t"] < r2["t"] for o in self.owner):
            return True
        if s == "T-I" and any(
            x.get("kind") == "status" and str(x.get("key")) == str(r1.get("key")) and x["t"] < r2["t"]
            for x in self.svc["T-I"]
        ):
            return True
        t2 = self.disp_token(s, r2)
        if self.snc and t2 and "end" in t2:
            return any(o["end"] < t2["end"] for o in self.outcome_tokens(c))
        return False

    def conflict(self):
        for s in ("T-I", "T-N"):
            rs = self.requests(s)
            for j, r2 in enumerate(rs):
                for r1 in rs[:j]:
                    e1, e2 = r1.get("epoch"), r2.get("epoch")
                    if e1 is None or e2 is None or int(e1) >= int(e2):
                        continue
                    self.ex("5.conflict")
                    if not self.resolved_before(s, r1, r2):
                        self.v(5, "5.conflict", tool=s, older=self.entry_id(r1), newer=self.entry_id(r2))

    # item 6

    def item6(self):
        marks = sorted(
            self.takeovers() + [r for r in self.tokens.values() if r.get("kind") == "cancel" and "end" in r],
            key=lambda r: r["end"],
        )
        if not self.snc:
            if marks or self.group in ("C-a1", "C-b2", "C-p", "C-s", "D"):
                self.unmet |= {"6.prepared_rejected", "6.outstanding"}
        else:
            for k in marks:
                self.at_takeover(k)
        dl = self.db.get("deadlocks")
        base = (self.run.get("pg") or {}).get("deadlocks_baseline", 0)
        self.ex("6.no_deadlock", self.group == "C-a1")
        if dl is not None and dl > base:
            self.v(6, "6.no_deadlock", deadlocks=dl, baseline=base)
        if self.group == "C-a1" and not self.takeovers() and self.snc:
            self.v(6, "6.no_deadlock", why="takeover never committed at C a1")
        self.in_doubt()
        self.derive()

    def state_before(self, obj, end):
        st = ep = None
        for h in self.hist(obj):
            if h["_end"] is not None and h["_end"] < end:
                st = h.get("to_state")
                if st == "prepared":
                    ep = self.tokens.get(h.get("token"), {}).get("epoch", h.get("epoch"))
        return st, ep

    def at_takeover(self, k):
        is_tk = k.get("kind") == "takeover"
        objs = [("tool_call", r) for r in self.db.get("tool_call", [])]
        if is_tk:
            objs += [("invocation", r) for r in self.db.get("invocation", [])]
        expect = set()
        for table, r in objs:
            oid = str(r["id"])
            st, ep = self.state_before(oid, k["end"])
            if st == "prepared" and (not is_tk or int(ep or 0) < int(k["epoch"])):
                self.ex("6.prepared_rejected")
                ok = any(
                    h.get("token") == k["token"] and h.get("to_state") in ("rejected", "abandoned")
                    for h in self.hist(oid)
                )
                if not ok:
                    self.v(6, "6.prepared_rejected", obj=oid, by=k["token"], kind=k.get("kind"))
            if st in ("dispatched", "accepted", "in_doubt"):
                expect.add(oid)
        rec = [ev for ev in self.db.get("run_event", []) if ev.get("token") == k["token"]]
        self.ex("6.outstanding", bool(expect))
        got = set()
        for ev in rec:
            o = ev.get("outstanding")
            got |= ids(json.loads(o) if isinstance(o, str) else o)
        if (expect or rec) and (not rec or got != expect):
            self.v(
                6,
                "6.outstanding",
                by=k["token"],
                kind=k.get("kind"),
                recorded=sorted(got) if rec else None,
                derived=sorted(expect),
            )

    def in_doubt(self):
        if not self.snc:
            return
        pending_stop = "M-done" not in {r.get("name") for r in self.tokens.values()}
        for r in self.db.get("tool_call", []):
            c = str(r["id"])
            if not any(h.get("to_state") == "in_doubt" for h in self.hist(c)):
                continue
            self.ex("6.in_doubt_owner")
            if r.get("tool") == "T-I":
                self.v(6, "6.TI_status", call_id=c, why="T-I call went in doubt instead of a status query")
            if not any(str(o.get("call_id")) == c for o in self.owner) and not (
                pending_stop and r.get("tool") == "T-N"
            ):
                self.v(6, "6.in_doubt_owner", call_id=c, why="in-doubt call never answered by its owner")
        for r in self.db.get("tool_call", []):
            if r.get("tool") != "T-I":
                continue
            c = str(r["id"])
            reqs = [e for e in self.requests("T-I") if str(e.get("call_id")) == c]
            if not reqs:
                continue
            outs = self.outcome_tokens(c)
            for k in self.kill_cuts:
                b = lsn_int(k.get("kill_boundary"))
                if k["t"] < reqs[0]["t"] or b is None or any(o["end"] <= b for o in outs):
                    continue
                self.ex("6.TI_status")
                from_reply = k["kind"] == "takeover" and any(
                    str(o.get("epoch")) == str(reqs[0].get("epoch")) for o in outs
                )
                from_reply |= any(
                    x.get("result") == r.get("result") and str(x.get("call_id")) == c
                    for x in self.responses("T-I", ("effect", "duplicate"))
                )
                status = any(
                    x.get("kind") == "status" and str(x.get("key")) == str(r.get("key")) and x["t"] > k["t"]
                    for x in self.svc["T-I"]
                )
                if outs and not from_reply and not status:
                    self.v(
                        6,
                        "6.TI_status",
                        call_id=c,
                        cut=k["label"],
                        why="outstanding T-I call resolved without its status query",
                    )

    def derive(self):
        if not self.snc:
            return
        for table in ("tool_call", "invocation"):
            for r in self.db.get(table, []):
                c = str(r["id"])
                outs = self.outcome_tokens(c)
                if not outs:
                    continue
                o = min(x["end"] for x in outs)
                tk = self.takeovers()
                stale = [
                    x
                    for x in outs
                    if any(int(x.get("epoch") or 0) < int(k["epoch"]) and k["end"] < x["end"] for k in tk)
                ]
                self.ex("6.derive", bool(stale))
                for h in self.hist(c):
                    if h["_end"] is not None and h["_end"] > o and h.get("to_state") in OPEN:
                        self.v(
                            6,
                            "6.derive",
                            obj=c,
                            why="call reopened after its outcome",
                            token=h.get("token"),
                            to_state=h.get("to_state"),
                        )
                for a in self.rows("attempt", call_id=c):
                    e = self.end_of(a.get("token"))
                    if e is not None and e > o:
                        self.v(6, "6.derive", obj=c, why="new attempt after the outcome", attempt=a.get("n"))
                for i in self.db.get("invocation", []) if table == "invocation" else []:
                    e = min((h["_end"] for h in self.hist(i["id"]) if h["_end"] is not None), default=None)
                    if str(i.get("retry_of")) == c and e is not None and e > o:
                        self.v(6, "6.derive", obj=c, why="re-call after the output committed", recall=i["id"])
                sent = [
                    h["_end"]
                    for i in self.db.get("invocation", [])
                    if str(i.get("retry_of")) == c
                    for h in self.hist(i["id"])
                    if h.get("to_state") not in (None, "prepared", "abandoned") and h["_end"] is not None
                ]
                for x in stale:
                    der = self.hist(c, True)
                    der = [h for h in der if h.get("kind") == "derive"]
                    cur = max(int(k["epoch"]) for k in tk)
                    if any(h.get("token") == x["token"] for h in der):
                        self.v(
                            6, "6.derive", obj=c, why="stale writer derived the dependent transitions", token=x["token"]
                        )
                    elif not sent and not any(
                        int(self.tokens.get(h.get("token"), {}).get("epoch") or 0) >= cur for h in der
                    ):
                        self.v(6, "6.derive", obj=c, why="dependent transitions not derived by the current epoch")
                    for h in self.db.get("history", []):
                        if h.get("token") == x["token"] and str(h.get("obj")) != c and h.get("kind") != "derive":
                            self.v(
                                6,
                                "6.derive",
                                obj=c,
                                why="stale writer committed a dependent transition",
                                token=x["token"],
                                dependent=h.get("obj"),
                            )

    # item 7

    def item7(self):
        rec = {}
        for a in self.answers:
            if a.get("call") == "reconstruct":
                rec[str(a.get("arg"))] = a.get("answer")
        need = {str(e.get("call_id")) for s in TOOLS for e in self.requests(s)}
        need |= {str(e.get("invocation_id")) for e in self.requests("stub")}
        listed = None
        for a in self.answers:
            if a.get("call") == "effects" and isinstance(a.get("answer"), dict):
                listed = {str(x) for x in a["answer"].get("effects") or []}
                need |= listed
        stub = {str(e.get("invocation_id")): e for e in self.requests("stub")}
        for eid in sorted(need):
            if eid not in rec and listed is not None and eid not in listed:
                self.v(7, "7.reconstructed", effect=eid, why="receiver saw it but the effects list omits it")
                continue
            if eid not in rec:
                self.u("reconstruct not asked", effect=eid)
                continue
            r = rec[eid]
            self.ex("7.reconstructed")
            if not isinstance(r, dict) or "refused" in r:
                self.v(7, "7.reconstructed", effect=eid, answer=r)
                continue
            self.shown(eid, r.get("shown"), stub)
            self.authorized(eid, r.get("authorized"))
            self.executed(eid, r.get("executed"))
            self.committed(eid, r)

    def shown(self, eid, sh, stub):
        if sh is None:
            if eid in stub or eid in self.called():
                self.unmet.add("7.shown")
            return
        self.ex("7.shown")
        for it in sh.get("items") or []:
            h = it.get("sha256")
            p = self.R / "blobs" / str(h)
            if not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest() != h:
                self.v(7, "7.shown", effect=eid, item=it, why="manifest item not in the blob store")
        wh = sh.get("whole_hash")
        if eid in stub and wh != stub[eid].get("body_sha256"):
            self.v(7, "7.shown", effect=eid, whole_hash=wh, stub=stub[eid].get("body_sha256"))
        elif eid in self.called() and wh is not None and wh not in {e.get("body_sha256") for e in stub.values()}:
            self.v(7, "7.shown", effect=eid, whole_hash=wh, why="whole hash matches no stub request")
        if self.snc and eid in stub:
            row = self.rows("invocation", id=eid)
            man = row[0].get("manifest") if row else None
            if isinstance(man, str):
                man = json.loads(man)
            if isinstance(man, list):
                if {str(m.get("sha256")) for m in man if isinstance(m, dict)} != {
                    str(i.get("sha256")) for i in sh.get("items") or []
                }:
                    self.v(7, "7.shown", effect=eid, why="items differ from the recorded manifest")

    def called(self):
        return {str(e.get("call_id")) for s in TOOLS for e in self.requests(s)}

    def authorized(self, eid, au):
        if eid not in self.called():
            return
        if au is None or au.get("rule_version") is None:
            self.unmet.add("7.authorized")
            return
        self.ex("7.authorized")
        want = self.run.get("policy_version")
        if self.snc:
            tc = self.rows("tool_call", id=eid)
            pol = self.rows("policy_decision", id=tc[0].get("policy_id")) if tc else []
            if pol:
                want = want or pol[0].get("rule_version")
                if str(pol[0].get("rule_version")) != str(au["rule_version"]):
                    self.v(7, "7.authorized", effect=eid, answer=au["rule_version"], db=pol[0].get("rule_version"))
        if want is not None and str(want) != str(au["rule_version"]):
            self.v(7, "7.authorized", effect=eid, answer=au["rule_version"], policy_file=want)
        if self.run.get("scenario") == "S-AP":
            log = [x for x in self.approver if str(x.get("call_id")) in (eid, "None")] or self.approver
            ap = au.get("approval")
            got = ap.get("decision") if isinstance(ap, dict) else ap
            if not log or got is None or str(got).lower()[:4] != str(log[-1].get("answer")).lower()[:4]:
                self.v(7, "7.authorized", effect=eid, approval=ap, approver_log=log[-1] if log else None)

    def executed(self, eid, ex):
        if ex is None:
            self.unmet.add("7.executed")
            return
        self.ex("7.executed")
        logged = set()
        for s in TOOLS:
            logged |= {
                int(e["attempt"])
                for e in self.requests(s)
                if str(e.get("call_id")) == eid and e.get("attempt") is not None
            }
        got = attempt_nums(ex.get("attempts"))
        if logged - got:
            self.v(7, "7.executed", effect=eid, why="attempts missing", logged=sorted(logged), answer=sorted(got))
        if self.snc:
            db_att = {int(a["n"]) for a in self.rows("attempt", call_id=eid) if a.get("n") is not None}
            if db_att and db_att != got:
                self.v(
                    7,
                    "7.executed",
                    effect=eid,
                    why="attempts differ from the records",
                    db=sorted(db_att),
                    answer=sorted(got),
                )
            hs = [h.get("to_state") for h in self.hist(eid)]
            states = [x.get("state", x.get("to_state")) if isinstance(x, dict) else x for x in ex.get("states") or []]
            extra = {h.get("to_state") for h in self.hist(eid, True) if h.get("kind") == "attempt"} - set(hs)
            if hs and set(hs) != set(states) - extra:
                self.v(7, "7.executed", effect=eid, why="states differ from the history", db=hs, answer=states)
        if any(e.get("invocation_id") == eid for e in self.requests("stub")):
            states = {x.get("state", x.get("to_state")) if isinstance(x, dict) else x for x in ex.get("states") or []}
            if "dispatched" not in states:
                self.v(7, "7.executed", effect=eid, why="stub received it but no dispatched state")

    def committed(self, eid, r):
        cm = r.get("committed")
        if cm is None:
            self.unmet.add("7.committed")
            return
        self.ex("7.committed")
        toks = {c["token"]: c for c in r.get("commits") or []}
        for t in toks:
            cls = self.classes.get(t, {}).get("class")
            if cls not in ("D", "R"):
                self.v(7, "7.committed", effect=eid, token=t, cls=cls, why="not D or R at the final cut")
            mine = self.tokens.get(t)
            if mine and toks[t].get("xid") is not None and str(mine.get("xid")) != str(toks[t]["xid"]):
                self.v(
                    7, "7.committed", effect=eid, token=t, why="xid differs", answer=toks[t]["xid"], db=mine.get("xid")
                )
        for tr in cm.get("transitions") or []:
            t = tr.get("token") if isinstance(tr, dict) else None
            if t is not None and self.classes.get(t, {}).get("class") not in ("D", "R"):
                self.v(7, "7.committed", effect=eid, transition=tr, why="transition not D or R at the final cut")
        if self.snc:
            hs = {h.get("token") for h in self.hist(eid, True)}
            if hs - set(toks):
                self.v(7, "7.committed", effect=eid, why="history commits missing", missing=sorted(hs - set(toks)))
            if len(cm.get("transitions") or []) != len(self.hist(eid, True)):
                self.v(
                    7,
                    "7.committed",
                    effect=eid,
                    why="transition count differs",
                    answer=len(cm.get("transitions") or []),
                    db=len(self.hist(eid, True)),
                )

    # run

    def check(self):
        try:
            self.build_tokens()
            self.build_cuts()
            self.classify()
            self.reached()
            self.answer_errors()
            for f in (self.item1, self.item2, self.item3, self.item4, self.item5, self.item6, self.item7, self.c_s):
                f()
        except Exception:
            self.u("unclassified run: checker error", trace=traceback.format_exc())
        return self.result()

    def result(self):
        status = {}
        bad = {v["clause"] for v in self.violations}
        for c in CLAUSES:
            status[c] = (
                "violation"
                if c in bad
                else "unmet" if c in self.unmet else "pass" if c in self.exercised else "not_exercised"
            )
        flagged = lambda cs: sorted(c for c in cs if status[c] in ("violation", "unmet"))
        orc = flagged(ORACLE_CLAUSES)
        inv = flagged(c for c in CLAUSES if c not in ORACLE_CLAUSES)
        m = self.run.get("mutation") or None
        want = MUTATION_CLAUSES.get(m, ())
        return {
            "run": {k: self.run.get(k) for k in ("point", "seed", "arm", "mutation", "scenario")},
            "oracle_verdict": "violation" if orc else "pass",
            "oracle_flagged": orc,
            "invariant_verdict": "violation" if inv else "pass",
            "invariant_flagged": inv,
            "invalid_verdict": "invalid" if self.invalid else "valid",
            "unresolved_verdict": "unresolved" if self.unresolved else "resolved",
            "mutation": m
            and {
                "name": m,
                "clauses": list(want),
                "exercised": any(c in self.exercised for c in want),
                "detected": flagged(want),
                "flagged": orc + inv,
            },
            "group": self.group,
            "violations": self.violations,
            "unresolved": self.unresolved,
            "invalid": self.invalid,
            "classes": getattr(self, "classes", {}),
            "clauses": status,
            "exercised": sorted(self.exercised),
            "verdict": (
                "unresolved"
                if self.unresolved
                else "invalid" if self.invalid else "violation" if self.violations or self.unmet else "pass"
            ),
        }


def main(R):
    try:
        res = Check(R).check()
    except Exception:
        res = {
            "verdict": "unresolved",
            "violations": [],
            "invalid": [],
            "unresolved": [{"reason": "unclassified run: checker error", "trace": traceback.format_exc()}],
        }
    out = Path(R) / "check.json"
    out.write_text(json.dumps(res, indent=1, sort_keys=True, default=str))
    print(
        json.dumps(
            {
                "verdict": res["verdict"],
                "violations": len(res["violations"]),
                "unresolved": len(res["unresolved"]),
                "invalid": len(res["invalid"]),
            }
        )
    )


if __name__ == "__main__":
    main(sys.argv[1])
