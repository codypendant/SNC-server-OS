# Codex review of PR #173 (filed verbatim)

Status (2026-11-19): PR #173 merged at `a975517` after this approval. **Nothing in this review is an owner decision.**
Its section 4 targeted review and section 5 next steps are in `docs/AFR_001_DURABILITY_DECISIONS.md` section 10 and
`docs/AFR_001_REPORT_IDENTITY.md`.

---

SNC Server OS — Codex Review of PR #173

VERDICT: APPROVED FOR DOCUMENTATION MERGE. ARCHITECTURAL ACCEPTANCE REMAINS ON HOLD.

I reviewed PR #173, including "AFR_001_REPORT_IDENTITY.md", the premise P analysis, R6.5 amendments, and round-five review findings.

1. Y9 — Acknowledgment identity

The new counterexamples establish that positioning alone is insufficient.

A delayed LSN-only acknowledgment can be counted against a different entry after truncation, promotion, or reseeding.

This is a substantive protocol safety issue, not merely defense in depth.

I recommend Y9-T as the preferred direction.

The acknowledgment report should identify its entry by "(term, LSN)".

The primary may count the report only when its own log contains that exact entry.

Preserve the existing requirement that the replica has applied the entry and durably watermarked it.

Do not count reports while the replica has a pending truncation intent or incomplete/reseeding state.

Y9-T requires an explicit owner decision because it revises R9-A′ and affects the replication wire format.

Required verification

Examine the complete interaction among:

- Delayed and duplicated reports.
- Truncation and term changes.
- Primary promotion.
- Replica incarnation changes.
- Reseed and discarded state.
- Outstanding authority grants.
- R9-B, R9-C, "ack_p", and lease eligibility.

The RS-0 rule requiring a wait of W after the last authority grant is load-bearing.

Verify that this wait is sufficient for every grant and renewal path. Do not assume the "(term, LSN)" field alone protects against a replica discarding previously acknowledged data.

Determine whether a report must also carry an incarnation or positioning identity. Do not add such fields without a demonstrated need.

2. Premise P — Checkpoint consistency

The P-1 through P-4 decomposition is accepted as a useful statement of the remaining obligations.

P-2 is a confirmed implementation nonconformance.

Today's checkpoint durability wait can miss deletes because the value used for "max_lsn" does not necessarily equal the last assigned LSN.

This must be corrected before accepting D-02 and the reseed safety argument.

P-3 must also establish that recovery refuses when the recovered WAL end is below the active checkpoint's "max_lsn".

The initial seed must durably copy WAL through "max_lsn" before publishing the node record, or P-3 creates a crash-only recovery refusal.

Do not claim premise P is discharged until P-1 through P-4 are supported by accepted contracts and the required implementation evidence.

3. R6.5

Y6c-2 remains CONDITIONAL.

U3 and U4 remain proposed requirements.

The analysis of I2–I5 depends on premise P, which is not established.

Do not accept the frozen R6.5 revision yet.

4. Round-five review

The independent reviewers found two blockers and eleven major issues.

The proposed corrections address the reported counterexamples, but the final corrections have not been re-reviewed.

Perform one targeted independent review of:

1. Y9-T and the reseed grant-wait rule.
2. P-1 through P-4, particularly P-2 and P-3.
3. Initial seed WAL publication ordering.
4. The revised orphan-directory behavior.
5. The new incarnation rule at RS-1.
6. The fault-budget treatment of write errors.

Record any remaining counterexamples and fix documentation errors on site.

5. Next steps

After the targeted review, prepare an updated owner-decision package for Y9-T.

Keep Y6c-2 conditional until premise P is discharged.

Identify the exact code changes that will eventually be required for P-2 and P-3, but do not implement them without authorization.

Do not expand M-1 or start another model campaign without an explicit decision.

6. Restrictions

- D-02 remains on HOLD.
- R-23 and O-FLOOR remain unresolved.
- Y4 and Y7 remain on HOLD.
- Y6c-2 and Y9-T are not accepted.
- No unauthorized model or experiment.
- No X-2 or R640 execution.
- No C++ or Rust implementation.
- No migration or final architectural freeze.
- Preserve m = 2 and the existing durability guarantees.

STOP after the targeted review and updated owner-decision package.

FINAL: PR #173 APPROVED FOR DOCUMENTATION MERGE. NO IMPLEMENTATION OR FINAL ARCHITECTURAL ACCEPTANCE.
