# Codex review of PR #172 (filed verbatim)

Status (2026-11-19): PR #172 merged at `87513ca` after this approval. **Nothing in this review is an owner decision.**
Its section 4 targeted review, section 3 (Y9) and section 5 items 1-2 were started in
`docs/AFR_001_DURABILITY_DECISIONS.md` sections 8-9 and `docs/AFR_001_REPORT_IDENTITY.md`.

---

SNC Server OS — Codex Review of PR #172

VERDICT: APPROVED FOR DOCUMENTATION MERGE. ARCHITECTURAL ACCEPTANCE REMAINS ON HOLD.

I reviewed PR #172, including "AFR_001_R65_INVARIANTS.md", the owner-decision record, the corrected durability contract, and the round-four adversarial review.

The documentation is approved for merge.

1. Owner decisions

OD-10 through OD-14 are recorded consistently with the owner's approval.

Y6c-2 remains CONDITIONAL and is not accepted.

Y4 and Y7 remain on HOLD.

Do not treat approval of this PR as acceptance of any additional protocol revision.

2. R6.5 and I2–I5

The analysis of a complete-but-unpositioned node is useful and addresses the requested categories.

U3 and U4 are necessary additions:

- U3: the donor serves the reseed only while it remains primary in the bound term and incarnation.
- U4: RS-1 establishes a new incarnation so previous positioning records cannot survive an in-place reseed.

However, the analysis depends on premise P: the primary's active checkpoint corresponds to its current log through "max_lsn".

Premise P remains dependent on D-02 and is blocker B9.

Do not accept Y6c-2 until premise P is established and the revised I2–I5 argument has been independently reviewed.

Preserve the proposed clear-at-publication, position-through-R6.2 direction.

3. Y9 — Acknowledgment identity

The counterexample involving an unpositioned replica reporting an old-term LSN is significant.

I recommend Y9 as a strengthening of R9-A′:

A replica's acknowledgment report may count only after the replica is positioned on the current primary's log in the current term.

The primary must not count a report from an unpositioned replica.

Also evaluate whether the report should carry "(term, LSN)" rather than only LSN.

Do not assume that positioning alone solves every stale-report scenario across incarnation changes, delayed messages or promotion.

Determine whether an explicit report identity binding is needed, and reconcile it with existing R9-B, R9-C and lease semantics.

This is a proposed frozen-contract change requiring owner approval.

4. Round-four corrections

The review found 17 major issues and eight documented counterexamples.

The corrections are valuable, particularly:

- Failed step-9 synchronization must produce a named refusal rather than an unsafe retry.
- "wal_sync=none" remains outside the durability guarantees.
- Orphan-directory handling must not delete acknowledged WAL.
- Initial-seed cleanup must preserve node-local identity files.
- RS-1 must establish a new incarnation.
- Checkpoint consistency premise P must be explicit.
- Experiments must remain isolated from production data.

The final corrections have not been independently re-reviewed.

Perform one focused adversarial review of the corrected sections and report only new substantive findings.

5. Next engineering priorities

After the targeted review:

1. Resolve Y9's acknowledgment identity and positioning requirements.
2. Establish the exact evidence needed to discharge premise P.
3. Keep Y6c-2 conditional until that evidence exists.
4. Prepare the previously proposed V8-L and watermark-cost experiments for separate owner authorization.
5. Continue toward O-FLOOR verification and the consolidated D-06 specification only within separately authorized scope.

Do not initiate a new unrestricted model campaign.

6. Restrictions

- D-02 remains on HOLD.
- R-23 remains unresolved.
- O-FLOOR remains unresolved.
- Y4 and Y7 remain on HOLD.
- Y6c-2 and Y9 are not accepted.
- No X-2 or R640 execution.
- No unauthorized model or experiment.
- No C++ or Rust implementation.
- No migration or final architectural freeze.
- Preserve m = 2 and all existing guarantees.

STOP after the targeted review and updated decision package. Submit the results for Codex and owner approval.

FINAL: PR #172 APPROVED FOR DOCUMENTATION MERGE. NO IMPLEMENTATION OR FINAL ARCHITECTURAL ACCEPTANCE.
