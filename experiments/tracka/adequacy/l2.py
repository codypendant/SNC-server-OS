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
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    px = [PY, str(ADQ / "proxy.py"), fault, str(real)]
    for k in CMDS:
        if k in cfg:
            cfg[k] = px + [k]
    for i, e in enumerate(cfg.get("engines", [])):
        e["start"], e["ready"] = px + [f"engine:{i}:start"], px + [f"engine:{i}:ready"]
    (d / "arm.json").write_text(json.dumps(cfg, indent=1))
    return d


CAPPED = ("O-1.end_state", "O-2.memory", "O-5.proceeds")


def status(expected, res, applied):
    if res["verdict"] in ("checker-crash",) or res.get("run") not in ("ended", "cap"):
        return "harness-error"
    if res["run"] == "cap":
        hit = [c for c in expected if c in res["fails"] and c not in CAPPED]
        return (
            "flagged (run invalid: cap)"
            if hit
            else "unresolved (cap; checker says invalid + violation)"
        )
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
    out = run(wrapper(arm, fault), f"l2-{arm}-{fault}", point, seed, scen)
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


def main():
    arm = sys.argv[1]
    only = set(sys.argv[2:])
    jobs = [
        (arm, f, c, e) for f, cells, e in CASES for c in cells if not only or f in only
    ]
    done = load(f"l2-{arm}") or {}
    for key, res in pmap(one, jobs):
        done[key] = res
        save(f"l2-{arm}", done)


if __name__ == "__main__":
    main()
