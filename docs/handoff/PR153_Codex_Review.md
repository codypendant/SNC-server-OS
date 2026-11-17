# Codex review of PR #153 (relayed by cody, 2026-11-17 19:14 UTC)

Transcribed from the two screenshots cody sent in the project (cody's message had no text of its own). PR #153 merged
as `b8951d5` as proposed research evidence. R2′ and R9 are not frozen.

---

PR #153 — Review

PR #153 — Promotion and Reconciliation Safety — Conditional approval

Decision: Approve for documentation/model merge only. HOLD the R2′ and R9 architectural freeze.

I reviewed the revised protocol, `PromoteRecon.tla`, and its verification evidence.

What passed

- X1: 891,002 states, exhaustive.
- X2: 7,423,089 states, exhaustive.
- Mutation tests reproduced acknowledged-write loss when quiescence, durable completion, or reconciliation reply-term validation was removed.
- Self-repromotion was demonstrated reachable.
- The model also found a checkpoint race while a reconciliation reply was in flight; Claude corrected it.

Two remaining safety blockers

Independent QA found two additional failure scenarios. Both have been addressed in the written protocol but not verified in the model:

1. Delayed or duplicated reconciliation replies: A same-term reply arriving after restart can cause truncation of an acknowledged entry. The new incarnation/request binding must be tested.
2. Pending intents across multiple terms: An old truncation intent can survive an intermediate primary and subsequently delete acknowledged data. The cross-term blocking rule must be tested.

These are acknowledged-data-loss risks, not merely documentation issues.

My recommendation

Approve Claude's proposed additional bounded verification, with three focused cases:

- Duplicate or delayed replies across restart.
- Four-node, multi-term promotion with an older pending intent.
- Two-entry crash and degraded-primary self-repromotion.

I also agree with Claude's proposed read policy: standby reads should expose only entries through the primary's acknowledged LSN, not unacknowledged speculative data.

The existing R9-A through R9-E corrections can proceed through review independently. R2′ should remain unfrozen until the two safety blockers are verified.

Final verdict: PR #153 may merge as proposed research evidence. Do not freeze R2′ or authorize product implementation.
