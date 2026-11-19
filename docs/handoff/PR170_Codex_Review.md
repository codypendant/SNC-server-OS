# Codex review of PR #170 (filed verbatim)

Status (2026-11-19): PR #170 merged at `1558aa6` after this approval. **Nothing in this review is an owner
decision**; its section 3 items are Codex's recommendations. The "Next authorized work" of section 4 was started in
`docs/AFR_001_DURABILITY_DECISIONS.md`.

---

SNC Server OS — Codex Review of PR #170

VERDICT: APPROVED FOR DOCUMENTATION MERGE. ARCHITECTURAL ACCEPTANCE REMAINS ON HOLD.

I reviewed PR #170, including "AFR_001_DURABILITY_CONTRACT.md", the consolidated fault model, twelve-step recovery state machine, refusal-to-repair matrix, R9-A′ revision, reseed comparison, prior-art analysis, and independent review findings.

The consolidation is a substantial improvement and satisfies the requested documentation deliverable.

However, this approval does not establish that the integrated durability protocol is correct.

1. Findings accepted

The following are sound directions for continued verification:

- One authoritative fault model, FM-1 through FM-16.
- Explicit distinction between a single media fault and correlated loss of multiple acknowledged writes.
- Two-phase recovery: read-only classification followed by permitted idempotent writes.
- V8-L protection extending to every descriptor carrying asynchronous direct-I/O writes.
- A refusal-to-repair matrix.
- R9-A′ as an explicit proposed strengthening of acknowledgment semantics.
- Whole-node in-place reseed as the preferred initial direction.
- Preservation of OD-7, OD-8 and OD-9.
- A consolidated D-06 revision inventory.
- Targeted experiments rather than another unrestricted model campaign.

2. Required corrections and verification

A. Independent re-review

Round two found 23 MAJOR issues. The resulting fixes have not been independently re-reviewed.

Require a focused independent three-way review of the final corrected contract.

Reviewers must examine the actual revised text, not merely the previous review summaries.

Specifically challenge:

- Recovery classification before repair.
- V8 lock coverage and lock lifetime.
- PENDING completion and floor reconstruction.
- Recovery watermark advancement.
- Reseed state transitions.
- R9-A′ effects on frozen protocol semantics.
- The stated fault budget and excluded correlated failures.

Do not treat the absence of a new blocker in an earlier review as acceptance of later corrections.

B. G-48 and initial seeding

The contract identifies an existing crash-only refusal during a new node's initial seed.

Determine whether this is an explicitly accepted exception to G-48 or an unresolved violation.

Do not silently classify it as acceptable.

Provide the exact existing contract text and the smallest possible correction or owner decision.

C. Fault-model consistency

Verify that FM-1 through FM-16 consistently distinguish:

- Ordinary crashes and process kills.
- Torn writes caused by crashes.
- One independent media fault.
- Lost directory operations.
- Correlated loss of multiple acknowledged writes.
- File-level metadata rollback.
- Device failure.
- Read errors versus write errors.

Check that no safety claim exceeds the declared fault budget.

The watermark must not be described as protecting against a device that acknowledges durability and subsequently loses both the WAL data and its verification evidence.

D. Recovery composition

The twelve-step recovery state machine is the leading proposed procedure.

Verify that phase-one classification cannot destroy or overlook evidence needed for refusal.

In particular, inspect:

- Metadata generation selection.
- PENDING streams at "L = retain_from − 1".
- Spare adoption.
- Checkpoint and floor validation.
- WAL segment locks.
- Watermark advancement.
- Segment replacement.
- Restart after an interrupted phase-two operation.

Identify any cross-mechanism invariant that remains supported only by argument.

Do not launch a new model without a specific authorization.

E. Acknowledgment semantics

R9-A′ remains a proposed frozen-contract revision.

Preserve the exact changes to R9-A, R9-B, "ack_p", lease eligibility, G-30, R6.3′ and the other affected clauses.

A replica must not count toward HA_DURABLE until it has applied the entry and its durable watermark covers it.

Do not claim this alone closes P5's excluded silent-loss clause.

F. Reseeding

Continue recommending whole-node in-place reseed as the initial direction.

Verify:

- Durable RESEEDING intent.
- Memory and lease-state invalidation.
- Term and incarnation binding.
- Checkpoint and WAL pin lifetime.
- "next_lease" preservation.
- Clear-at-publication ordering.
- Subsequent R6.2 positioning.
- Repeated crashes and term changes.

R6.5's proposed revision requires an explicit owner decision.

G. Performance and platform assumptions

Keep E-1, E-2 and E-3 as separate proposed experiments.

Before requesting execution authorization, confirm that their procedures cannot modify or destroy production data.

E-2 is a preliminary device-cost measurement, not an X-2 replacement or an HA_DURABLE performance result.

Record the unverified power-loss protection and kernel-specific assumptions.

3. Owner decisions

My current recommendations are:

- Y5: Support the R9-A′ direction, but defer formal approval until the corrected contract is re-reviewed.
- Y6a: Support V8-1/V8-L/V8-3 as design directions, conditional on platform verification.
- Y6b: Support the specified HeaderFirst and temporary-file recovery direction.
- Y6c-1: Support whole-node in-place reseed as the initial direction.
- Y6c-2: Support the proposed R6.5 ordering direction, but require review of the exact frozen-contract implications.
- Y7: HOLD pending meaningful replicated performance evidence.
- Y4: HOLD pending the prerequisite decisions and consolidated byte-level specification.

These are Codex recommendations, not owner approvals.

4. Next authorized work

Perform the focused re-review and resolve the G-48 initial-seed exception.

Produce a concise final owner-decision package containing:

1. The exact decisions requiring approval.
2. Their effects on previously accepted or frozen contracts.
3. Supporting evidence.
4. Remaining assumptions and risks.
5. Any unresolved blockers that prevent architectural acceptance.

Do not start implementation or experiments.

5. Restrictions

D-02, R-23 and O-FLOOR remain open.

No X-2, R640 execution, new formal-model campaign, C++ or Rust implementation, migration, or final D-06 freeze is authorized.

Preserve m = 2 and all existing guarantees unless the owner explicitly approves a recorded change.

STOP after the corrected-contract review and owner-decision package. Submit them for Codex and owner review.

FINAL VERDICT: PR #170 APPROVED FOR DOCUMENTATION MERGE. NO ARCHITECTURAL ACCEPTANCE OR IMPLEMENTATION AUTHORIZATION.
