# Finding: the frozen SNC arm does not expose deterministic causality (2026-11-28)

**Label:** FACT, read in the frozen tree (`575f493`; `experiments/ar1/ar1/rt.py` is unchanged on main). This is
recorded after the criteria freeze (`8a0f787`) and is reported next to the results. CRITERIA.md is not edited.

**What Track A needs.** Plan 3.2 requires each tool request's arguments and the memory value to be fixed functions of
the stub reply id and earlier tool replies, so that the logs show which model reply every later step used
(`experiments/tracka/INTERFACES.md`, Scenario). Two subclauses depend on it:
- O-1: every downstream request uses one model reply id;
- O-2: the quiesced `state(agent)` equals the value derived from the replies the downstream requests used.

**What the frozen SNC arm does.**
- `rt.py:737-738`: `tool_args(tool)` returns `{"tool": tool, "op": f"perform {tool}", "task": TASK}`, the same for
  every model reply.
- `rt.py:455-461` (`op_done`): the memory written is the constant blob `"summary written by agent A"`.

**Effect.**
- Formal outcome: none. It is already row 1 (SNC O-5 UNSUPPORTED; cody chose to keep the rules).
- Diagnostic (O-1 to O-4): those two causality subclauses are **UNSUPPORTED for SNC**, through the SNC arm's
  `unsupported` list in `arm.json`. SNC's other clauses are still judged.
- **Competitors are still judged on both subclauses.** The label applies only to the arm whose `arm.json` lists it;
  the checker never infers it, and a competitor that does not follow the causality rule fails those clauses.
- No change is made to the frozen SNC runtime.

The coordinator agreed to this handling on 2026-11-28 and will show it to Codex with the next review package.
