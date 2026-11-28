import json
import shutil
import sys

from .common import ADQ, ARMS, BUILD, PY, check, load, log, pmap, read, run, save

CMDS = ("build", "setup", "submit", "worker", "deliver", "state", "reconstruct")
SM, A04 = ("none", 0, "S-M"), ("A04", 0, "S-M")
AP0, AP1, B0, B1 = (
    ("A09", 0, "S-AP"),
    ("A09", 1, "S-AP"),
    ("A10", 0, "S-AP"),
    ("A10", 1, "S-AP"),
)
CASES = [
    ("passthrough", [SM, A04, AP1, B0], []),
    ("mem_tf", [SM], ["O-2.memory"]),
    ("mem_extra_key", [SM], ["O-2.memory"]),
    ("mem_drop_tn", [SM], ["O-2.memory"]),
    ("mem_reply", [SM], ["O-2.memory"]),
    ("state_retention", [SM], ["O-4.retention"]),
    ("state_task_id", [SM], ["O-1.end_state"]),
    ("status_done_to_doubt", [SM], ["O-1.end_state"]),
    ("model_input", [SM], ["O-4.model"]),
    ("model_output_sha", [SM], ["O-4.model"]),
    ("model_used_false", [SM], ["O-4.model_used"]),
    ("model_extra", [SM], ["O-4.model"]),
    ("model_drop", [SM], ["O-4.model"]),
    ("attempt_drop", [SM, A04], ["O-4.calls"]),
    ("call_drop", [SM], ["O-4.calls"]),
    ("attempt_outcome", [SM], ["O-4.calls"]),
    ("call_args", [SM], ["O-4.calls"]),
    ("call_token", [SM], ["O-4.calls"]),
    ("outcome_result", [SM], ["O-4.calls"]),
    ("rule_version_null", [SM, AP0], ["O-4.policy"]),
    ("policy_action", [SM], ["O-4.policy"]),
    ("policy_result", [SM], ["O-4.policy"]),
    ("policy_drop", [SM], ["O-4.policy"]),
    ("policy_dup", [SM], ["O-4.policy"]),
    ("policy_dup", [AP0, AP1], ["O-5.once"]),
    ("approval_without_event", [SM], ["O-4.policy"]),
    ("approval_flip", [AP0, AP1], ["O-5.once", "O-4.approval"]),
    ("approval_null", [AP0, AP1], ["O-5.once", "O-4.approval"]),
    ("recon_retention", [SM], ["O-4.retention"]),
    ("deliver_flip", [AP0, AP1, B1], ["O-5.proceeds"]),
    ("deliver_twice", [AP0, B1], []),
    ("deliver_drop_fail", [AP1], ["O-5.accepted"]),
    ("deliver_drop", [AP0], ["O-5.proceeds"]),
]


def wrapper(arm, fault):
    real = ARMS[arm]
    cfg = json.loads((real / "arm.json").read_text())
    d = BUILD / "arms" / f"l2-{arm}-{fault}"
    d.mkdir(parents=True, exist_ok=True)
    px = [PY, str(ADQ / "proxy.py"), fault, str(real)]
    for k in CMDS:
        if k in cfg:
            cfg[k] = px + [k]
    for i, e in enumerate(cfg.get("engines", [])):
        e["start"], e["ready"] = px + [f"engine:{i}:start"], px + [f"engine:{i}:ready"]
    tmp = d / f".arm.{fault}.{id(cfg)}"
    tmp.write_text(json.dumps(cfg, indent=1))
    tmp.rename(d / "arm.json")
    return d


CAPPED = ("O-1.end_state", "O-2.memory", "O-5.proceeds")


def status(expected, res, applied):
    if res["verdict"] in ("checker-crash",) or res.get("run") not in ("ended", "cap"):
        return "harness-error"
    if res["run"] == "cap":
        hit = [c for c in expected if c in res["fails"] and c not in CAPPED]
        return "flagged (capped run)" if hit else "unresolved (cap)"
    if not expected:
        return "correct-pass" if not res["fails"] else "flagged-extra"
    if not applied:
        return "unresolved (proxy not applied)"
    missing = [c for c in expected if c not in res["fails"]]
    if not missing:
        return "flagged (run invalid)" if res["verdict"] == "invalid" else "flagged"
    if any(c in res["unresolved"] for c in missing):
        return "unresolved"
    return "miss"


def one(job):
    arm, fault, (point, seed, scen), expected = job
    key = f"{arm}|{fault}|{point}|{seed}|{scen}"
    old = (load(f"l2-{arm}") or {}).get(key)
    if old and old["status"] != "harness-error":
        n = int(old["evidence"].split()[2].rstrip("x,"))
        old["status"] = status(expected, old, n)
        return key, old
    from .l3 import env

    out = run(wrapper(arm, fault), f"l2-{arm}-{fault}", point, seed, scen, env=env(arm))
    R = out.get("R")
    res = check(R) if R else {"verdict": "driver-error", "fails": [], "unresolved": []}
    res["run"] = out.get("status")
    notes = [x for x in read(f"{R}/logs/adequacy.jsonl")] if R else []
    applied = [x for x in notes if "error" not in x]
    res.update(
        fault=fault,
        point=point,
        seed=seed,
        scenario=scen,
        expected=expected,
        evidence=f"proxy applied {len(applied)}x, errors {len(notes) - len(applied)}",
    )
    res["status"] = status(expected, res, applied)
    tail = f"{point}-{scen}-{seed}" if point == "none" else f"{point}-{seed}"
    ctl = (load(f"controls-{arm}") or {}).get(tail)
    res["control_fails"] = ctl["fails"] if ctl else None
    if ctl and res["status"] == "flagged" and any(c in ctl["fails"] for c in expected):
        res["status"] = "no-credit (control fails this clause)"
    log(key, res["status"], res["verdict"], res["fails"], res["unresolved"])
    if R and res["status"] != "harness-error":
        shutil.rmtree(R, ignore_errors=True)
    return key, res


LIVE = [
    ("passthrough", [SM, AP1], []),
    ("deliver_flip", [AP0, AP1], ["O-5.proceeds"]),
    ("deliver_twice", [AP0], []),
    ("deliver_drop_fail", [AP1], ["O-5.accepted"]),
]
REPLAY_AT = (
    "none-S-M-0",
    "A02-0",
    "A04-0",
    "A06-1",
    "A15-1",
    "A09-0",
    "A09-1",
    "A10-0",
)


def replay(arm):
    import copy

    from .common import BASE, RUNS, write
    from .proxy import RECON, STATE

    exp = {}
    for f, cells, e in CASES:
        for c in cells:
            exp.setdefault(f, {})[c[2]] = e
    out = {}
    for tail in REPLAY_AT:
        b = BASE / f"{arm}-{tail}"
        if not b.exists():
            continue
        ctl = check(b)
        scen = json.loads((b / "run.json").read_text())["scenario"]
        rows = read(b / "answers.jsonl")
        tn_fault = tail.split("-")[0] in ("A04", "A08", "A13", "A17")
        for f, e in ((f, x[scen]) for f, x in exp.items() if scen in x):
            if f == "status_done_to_doubt" and tn_fault:
                e = ["O-2.memory"]
            fn, call = (
                (STATE[f], "state") if f in STATE else (RECON.get(f), "reconstruct")
            )
            if not fn:
                continue
            new = copy.deepcopy(rows)
            try:
                fn([r for r in new if r["call"] == call][-1]["answer"])
            except Exception:
                continue
            d = RUNS / "l2r" / f"{arm}-{tail}-{f}"
            shutil.rmtree(d, ignore_errors=True)
            shutil.copytree(b, d)
            write(d / "answers.jsonl", new)
            res = check(d)
            shutil.rmtree(d)
            uns = [c for c in e if ctl["clauses"].get(c) == "unsupported"]
            if uns:
                st = "unsupported (clause listed in arm.json)"
            elif any(c in ctl["fails"] for c in e):
                st = "no-credit (control fails this clause)"
            elif all(c in res["fails"] for c in e):
                st = "flagged"
            else:
                st = "miss"
            res.update(fault=f, point=tail, seed="", scenario=scen, expected=e)
            res.update(
                status=st, evidence="replayed answer", control_fails=ctl["fails"]
            )
            out[f"{arm}|{f}|{tail}"] = res
            log(arm, f, tail, st, res["fails"])
    done = load(f"l2-{arm}") or {}
    save(f"l2-{arm}", {**done, **out})


def main():
    arm = sys.argv[1]
    if sys.argv[2:] == ["replay"]:
        return replay(arm)
    only = set(sys.argv[2:])
    cases = CASES if arm == "dbos" else LIVE if arm != "snc" else LIVE[:1]
    jobs = [
        (arm, f, c, e) for f, cells, e in cases for c in cells if not only or f in only
    ]
    if arm == "snc":
        jobs = [(arm, "passthrough", c, []) for c in (SM, A04)]
    done = load(f"l2-{arm}") or {}
    for key, res in pmap(one, jobs):
        done[key] = res
        save(f"l2-{arm}", done)


if __name__ == "__main__":
    main()
