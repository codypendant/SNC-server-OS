# Codex review of PR #167 (relayed by cody, 2026-11-18 22:27 UTC)

Pasted in full by cody into the project chat. Transcribed verbatim below.

---

SNC Server OS — Codex Review of PR #167

VERDICT: APPROVED FOR MERGE — documentation and formal model only.

I reviewed PR #167, including "SegChain.tla", the preregistered verification plan, results, and independent review.

1. Verification Results

- 123 counted TLC runs.
- 120 matched preregistered expectations.
- Three deviations were documented, traced, and explained.
- No timeout or unauthorized R640 execution.
- Independent review found no blocker.

The bounded verification supports R-CHAIN across rollover, spare adoption, recovery reopening, and the modeled reseed
lifecycle.

The deviations do not block merging the evidence, but they must remain recorded.

2. Engineering Decisions

R-CHAIN: Continue validating predecessor links on every retained WAL segment. Do not reduce validation to the newest
segment.

Spare-header ordering: HeaderFirst is the recommended design. Write and durably synchronize the take-time header in
"spare.tmp" before renaming the spare into the WAL namespace. The existing ordering has a demonstrated crash-only
refusal.

C15: Preserve segment identity when replacing a segment. Retain the same nonce. The bounded model supports this
approach, but late writes and storage-device assumptions remain unresolved.

Per-stream reseed: Do not declare O-R resolved. Verify that R6.5 actually satisfies the empty-directory requirement,
creates its own segment identities, and replaces the previous watermark binding.

These are recommendations and verification findings, not authorization to modify product code or silently revise
conditionally accepted D-06 contracts.

3. Next Authorized Work

Proceed with a consolidated D-02 architectural acceptance review, documentation and analysis only.

The review must:

1. Reconcile the proposed D-02 design with conditionally accepted D-06 items 4–14.
2. Inventory the evidence from MetaWatermark revisions 1–4 and SegChain revision 5.
3. Separate verified properties from assumptions, inferences, and untested failure modes.
4. Check R6.5 per-stream reseeding directly against the proposed storage requirements.
5. Examine V8: outstanding asynchronous writes after external process termination.
6. Evaluate spare-header ordering and segment replacement against the actual storage mechanics.
7. Identify remaining O-FLOOR dependencies.
8. Reconcile the proposed design with G-14, G-15, G-48, frozen R2′, R9-A through R9-E, and m = 2.
9. Produce a precise list of remaining blockers before D-02 can be accepted.
10. Recommend the smallest justified next verification or design decision.

Do not initiate another formal-model revision merely because an unresolved question exists. First determine whether
the question can be answered by examining the existing code, specifications, or evidence.

4. Mandatory Restrictions

- D-02 remains on HOLD.
- R-23 remains unresolved.
- O-FLOOR remains unresolved.
- No final D-06 architectural freeze.
- No X-2 or R640 execution.
- No C++ or Rust implementation.
- No migration work.
- No unapproved changes to frozen protocol contracts.

Apply independent three-way verification.

Always verify, never assume. Fix identified documentation errors on site. Preserve evidence and clearly identify
unresolved findings.

STOP after completing the consolidated architectural review and submit it for Codex and owner approval.

PR #167 is approved for merge. No additional implementation or experimental authorization is implied.

---

**Status** (recorded by Claude):
- #167 merged on this approval (9cc7514).
- Section 2 items are **Codex's recommendations, not owner decisions.** Those that would change a design direction
  (every-segment R-CHAIN, HeaderFirst, same-nonce replacement) need cody's explicit yes in words. They are listed in
  the consolidated review.
- Section 3 started under cody's standing rule for pasted reviews: `docs/AFR_001_D02_ACCEPTANCE_REVIEW.md`.
