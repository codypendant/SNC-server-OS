# Codex review of PR #179 (filed verbatim)

Status (2026-11-19): PR #179 merged at `4b4419e` after this approval. Codex supports the live-grant direction; nothing
is accepted. The next scope it names (part A, L-1; part B, the LiveGrant model and preregistration, no counted runs)
is in `docs/AFR_001_L1_AUTHORITY.md` and `docs/AFR_001_LIVE_GRANT_PREREG.md`.

---

SNC Server OS — Codex Review of PR #179

VERDICT: APPROVED FOR DOCUMENTATION MERGE. SUPPORT THE LIVE-GRANT ARCHITECTURAL DIRECTION. NO PROTOCOL ACCEPTANCE OR MODEL EXECUTION AUTHORIZATION.

I reviewed PR #179, including "AFR_001_LIVE_GRANT_PROPOSAL.md", the LG-1 through LG-6 rules, message and state transitions, safety argument, frozen-contract impact, proposed model plan, and independent review findings.

1. Architectural direction

The live-grant approach is preferable to continuing to extend the blocked Y9-T report-identity design.

It replaces reliance on cached replica state with a promise that the replica will not destructively change the reported data while a corresponding authority grant remains valid.

I recommend YES to pursuing LG-1 through LG-6 and the R1.1 modification as design directions only.

The following protections must remain:

- Incarnation identity changes on boot and destructive-operation markers.
- Grants and reports identify the matching replica incarnation.
- Counted coverage requires a matching live grant and exact log-entry identity.
- Truncation, reseed and wipe cannot destroy durable state while an earlier grant may remain valid.
- Witness-version ordering prevents a stale Grow from racing a reseed.
- Replication batches enforce append continuity.
- R1.1 admits a member to A only after positioning.

These are not accepted frozen-contract revisions.

2. L-1 — Priority authority-safety blocker

The newly identified overlap between authority holders under R7 case 1 requires independent resolution.

Scenario:

1. The primary proposes a shrink removing a replica.
2. The shrink is chosen, but its reply is lost.
3. The old primary continues relying on grants from the removed replica.
4. The witness record is degraded and READY.
5. The operator promotes another node through R7.
6. The old primary's grants may remain valid while the new primary begins serving.

This potentially violates exclusive-primary authority.

Open L-1 as a high-priority protocol safety obligation.

Do not assume that switching immediately to witness authority resolves it. Before the shrink is chosen, witness authority may not exist.

The analysis must specify whether the primary must quiesce while a shrink is unresolved, when it can acquire degraded witness authority, and precisely how the R3 barrier excludes overlapping authority.

Examine delayed shrink replies, repeated CAS attempts, witness grants, operator promotion and primary crashes.

Preserve the existing R7 and G-35 distinctions. Do not classify an authority overlap as an accepted data-loss exception.

3. Live-grant verification

I support preparing the proposed "LiveGrant.tla" model and a revised preregistration.

Before any counted run:

1. Correct all identified fidelity defects from "ReportReseed.tla".
2. Represent R6.2 requests as delayed, duplicated and reorderable messages.
3. Include grant incarnation identity and grant expiration.
4. Model the witness-version CAS and racing Grow proposals.
5. Model destructive truncation, reseed and wipe.
6. Include promotion inside and outside S.
7. Check AckSound, GrowSound, LeaseSound, AckedPreserved and log-identity invariants.
8. Reproduce the previously observed counterexample traces as mandatory regressions.
9. Require positive controls demonstrating that the corresponding mutations reproduce those failures.
10. Require three-way adversarial fidelity review of the actual TLA+ specification.

The model's timing abstraction must remain explicit. An untimed grant-expiration guard does not verify that the real CLOCK_BOOTTIME wait achieves the required safety boundary.

Use predetermined bounds, mutations, reachability probes and stopping criteria.

Do not execute counted runs without a separate owner GO.

4. LG-4 and LG-5

Pay particular attention to the destructive-action transition.

LG-4 must apply to all data-removing operations, including R6.2 truncation, RS-2, wipe and restart from a replaced disk.

LG-5 must establish that:

- A pending Grow chosen before the witness-version bump is observed.
- A Grow based on an obsolete version cannot subsequently commit.
- A new Grow after the bump cannot be admitted using stale coverage or expired grants.
- The node does not proceed with reseed while it remains in S.
- A failed or conflicting CAS does not lead to destructive changes.

Check the full check-then-act boundary, not merely the final witness version.

5. Append continuity

LG-6 is a necessary independent safeguard against stale positioning and delayed R6.2 messages.

Verify that the predecessor check works for:

- Ordinary replicated WAL batches.
- Repeated or reordered batches.
- Divergent log tails.
- Checkpoint floors where the preceding physical WAL record has been reclaimed.
- Reseed and recovery after compaction.

LG-6 must not silently assume that the predecessor record always remains physically available.

Any dependency on FL-floor or O-FLOOR must be recorded.

6. Contract revisions

The proposal affects R3, R1.1, R6.3′, R9-A′, R9-B, D-06 incarnation semantics and replication wire formats.

None is automatically revised or accepted.

Do not remove W-1, W-2, W-3 or the existing Y9-T checks merely because the new design appears to subsume them.

Demonstrate their redundancy using the model and the complete safety argument first.

7. Review quality

The independent reviewers found two blockers and approximately fifteen major findings in the original proposal.

The revisions are recorded, but the final changes have not been independently re-reviewed.

Require independent review of the corrected protocol and the revised formal-model plan.

Preserve all unresolved assumptions, particularly premise P, D-02, R-23, incarnation uniqueness and the real-time grant-expiration argument.

8. Next authorized scope recommendation

Proceed in two tightly scoped parts:

A. L-1 authority safety

Produce the exact counterexample, proposed correction, contract impact and minimum verification necessary to establish exclusive authority across the missed-shrink/R7 transition.

B. LiveGrant model preparation

Produce the revised preregistration and model specification, including three-way fidelity review and mandatory regression traces.

Do not start counted verification until the owner authorizes it separately.

9. Restrictions

- Y9-T remains BLOCKED.
- LG-1 through LG-6 and the R1.1 change remain proposed.
- L-1 remains unresolved.
- Y10, Y6c-2, E32, Y4 and Y7 remain unaccepted or on HOLD.
- D-02, R-23 and O-FLOOR remain open.
- No unauthorized experiment, X-2 or R640 run.
- No C++ or Rust implementation.
- No wire-format changes, migration or architectural freeze.
- Preserve m = 2 and the existing guarantees.

STOP after the targeted L-1 analysis and revised LiveGrant model/preregistration package. Submit both for Codex and owner approval.

FINAL: PR #179 APPROVED FOR DOCUMENTATION MERGE. LIVE-GRANT DIRECTION SUPPORTED. L-1 MUST BE CLOSED BEFORE FINAL PROTOCOL ACCEPTANCE.
