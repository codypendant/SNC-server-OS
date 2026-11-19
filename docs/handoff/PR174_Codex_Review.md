# Codex review of PR #174 (filed verbatim)

Status (2026-11-19): PR #174 merged at `e6a3e0b` after this approval. **Nothing in this review is an owner decision.**
Its section 6 re-review and the updated package are in `docs/AFR_001_DURABILITY_DECISIONS.md` section 11.

---

SNC Server OS — Codex Review of PR #174

VERDICT: APPROVED FOR DOCUMENTATION MERGE. ARCHITECTURAL ACCEPTANCE REMAINS ON HOLD.

I reviewed PR #174, including the revised report-identity proposal, checkpoint durability obligations, reseed grant ordering, remap recovery changes, and round-six findings.

1. Y9 — Report identity

The new counterexample demonstrates that "(term, LSN)" alone is insufficient.

A report from before an in-place reseed can be counted after the replica rejoins, even though that replica no longer holds the reported entries.

I support the revised Y9-T direction:

- Every report identifies its entry by "(term, LSN)".
- Every report carries the reporter's incarnation.
- The primary counts the report only if its own log contains the identified entry.
- The primary verifies that the reporter's incarnation matches the incarnation under which it positioned that replica.
- No report is sent while a truncation intent is pending or the incomplete/reseeding flag is set.
- The existing applied-and-watermarked requirements remain mandatory.

This is a proposed revision of the accepted R9-A′ contract and requires owner approval.

Do not implement the wire-format change yet.

2. Reseed grant safety

The RS-0 grant wait is load-bearing.

I support moving the wait before RS-1.

The node must:

1. Stop granting authority and serving lease reads.
2. Wait W from the later of its last grant and current boot.
3. Only then durably enter RESEEDING and change incarnation.
4. Grant nothing while RESEEDING.
5. Resume granting only after RS-6 and successful R6.2 positioning in the new incarnation.

This prevents a crash from erasing the grant-wait obligation.

Verify the rule against delayed renewals, repeated crashes, stale reports, and primary promotion.

3. Checkpoint consistency — Premise P

P-2 remains a confirmed implementation defect.

The current checkpoint durability wait can miss:

- Deletes occurring during the checkpoint walk.
- Replicated records on standbys whose installed images carry "last_lsn = 0".

The proposed correction must use the last assigned WAL LSN sampled at the end of the checkpoint walk.

P-3 must verify that the recovered WAL end is not below the active checkpoint's "max_lsn".

The initial seed must durably copy the necessary WAL through "max_lsn" before publishing the node record.

These are required architectural and implementation obligations, not completed fixes.

No implementation is authorized by this review.

4. Remap recovery

The newly identified crash-only refusal during remap is significant.

Support the proposed step 9a ordering:

No record may be written into a newly added stream until the metadata record naming the remap checkpoint is durably published.

Verify the complete remap and recovery sequence, including interrupted checkpoint publication, orphan-directory classification, and repeated crashes.

Do not declare G-48 closed on this evidence alone.

5. Buffered WAL

The proposed exclusion of buffered WAL from durability guarantees requires explicit review.

Determine:

- Whether every supported platform can guarantee direct I/O.
- Whether the code can silently fall back to buffered operation.
- Whether an unsupported configuration is refused at startup.
- Whether the synchronous "pwritev2" fallback preserves the required durability flags.
- Whether the exclusion changes any accepted deployment or product guarantees.

Do not silently narrow the supported storage contract.

6. Required re-review

Round six found four blockers and six major findings.

All were addressed in the proposed text, but the fixes have not been independently re-reviewed.

Perform one focused adversarial review of:

- Reporter incarnation and Y9-T.
- RS-0 grant timing across crashes.
- P-2 and P-3.
- E30 seed WAL publication.
- Remap step 9a.
- Buffered WAL assumptions.

Record any remaining counterexamples and update the owner-decision package.

7. Restrictions

- Y9 remains proposed, not accepted.
- Y6c-2 remains conditional.
- Y4 and Y7 remain on HOLD.
- D-02, R-23 and O-FLOOR remain open.
- No unauthorized model or experiment.
- No X-2 or R640 execution.
- No C++ or Rust implementation.
- No migration or final architectural freeze.
- Preserve m = 2 and all existing guarantees.

STOP after the targeted review and updated decision package.

FINAL: PR #174 APPROVED FOR DOCUMENTATION MERGE. NO IMPLEMENTATION OR FINAL ARCHITECTURAL ACCEPTANCE.
