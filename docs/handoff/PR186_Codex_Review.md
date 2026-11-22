# Codex review of PR #186 (relayed by cody, 2026-11-22 02:34 UTC; filed verbatim)

SNC — Codex Review of PR #186
VERDICT: HOLD — TARGETED PLAN CORRECTIONS REQUIRED.
The two-track direction is supported.
The plan appropriately preserves both SNC's original state-native substrate research and the focused agent-runtime hypothesis.
However, several preregistration and decision-gate issues must be corrected before approval.
1. Track A Arithmetic
Section 3.4 lists 17 crash points totaling 114 runs per arm, not 18 points and 110 runs.
Correct the table, totals and section 9. Verify against an independently generated matrix before freezing criteria.
2. Equal Durability — Track B
Section 4.2 allows Garnet to remain the gating baseline even if its device configuration cannot demonstrate durable acknowledgment.
That is not acceptable for an equal-durability comparison.
Garnet documents `--aof-commit-wait` as waiting for the AOF commit to flush, but the underlying storage guarantee must also be established.
Require verification of durable acknowledgment through the selected device and storage stack.
If equivalent durability cannot be established, the Garnet result becomes diagnostic, not gating. Specify a valid alternative gating baseline before running.
Apply the same principle to Restate's acknowledgment semantics in B2.
3. Align Transfer Completion Semantics
Clarify the exact B1 workload mix at the 20% transfer point.
SNC's saga acknowledges at durable debit; the competing atomic transaction acknowledges after the complete transfer.
Do not use those different boundaries as a decisive comparison.
Provide either:

* An equal-completion-boundary comparison; or
* Separate debit-acknowledgment and full-settlement measurements, with only equivalent guarantees used for architectural decisions.

Apply the same rule to B2, including recovery and settlement throughput.
Preserve the documented semantic differences and label which system they favor.
4. Restrict What the Results Can Establish
Revise section 6.
B1 and B2 test object execution and durable messaging.
They do not establish the performance or viability of every aspect of a complete operating system.
Both PASS means continued state-native substrate research is justified, not that a full OS has been validated.
Both FAIL means the tested substrate approach has failed its criteria. Do not automatically generalize that to every possible state-native architecture.
Keep product direction, research continuation and implementation authorization separate.
5. Objective Track A Differentiation
Resolve the Rust-versus-Python L=300 comparison before any result.
Use a preregistered classification of ordinary application code, configuration, framework glue and new infrastructure.
Report code size by language.
Do not allow an unbounded subjective assessment to determine whether SNC remains a runtime or becomes an SDK.
The comparison must still use idiomatic implementations and equivalent observable guarantees.
6. Preserve Existing Boundaries

* PR #185's G2 adequacy gaps remain open.
* AR-1 G3 remains on HOLD.
* Track A cannot execute until its prerequisites are met and the owner authorizes it.
* Track B requires its own owner authorization.
* No raft-rs integration.
* No custom replication work.
* No changes to SNC's engine.
* No production dependency selection.
* No implicit decision to abandon the original OS.

Retain the pinned prior-art sources and the directive.
7. Next Action
Fix these issues on site.
Perform focused independent review of the revised benchmark equivalence and decision rules.
Return PR #186 for Codex approval.
FINAL: HOLD PR #186. SUPPORT THE TWO-TRACK EXPERIMENTAL DIRECTION, BUT DO NOT FREEZE OR RUN THE COMPARISON UNTIL ITS CRITERIA ARE VALID.
