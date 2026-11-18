# Owner decision: D-06 items 4-14 conditionally accepted (cody, 2026-11-18)

This is the owner's decision record, in the format of `PR155_Owner_Decision.md` and `PR157_Owner_Decision.md`.

## The decision

cody's message in the project, at 2026-11-18 13:24 UTC, verbatim:

> Yeah, conditional acceptance. Yeah, you have it from me.

It answered Codex's recommendation in the review of PR #160 (`docs/handoff/PR160_Codex_Review.md`, section 2), which
cody pasted 34 seconds earlier: "I recommend that the owner conditionally accept D-06 items 4–14 as SNC's selected
storage-format direction. This recommendation is subject to the owner's explicit approval."

**D-06 items 4-14 (`docs/AFR_001_D06_FORMAT_DECISION.md` section 2) are CONDITIONALLY ACCEPTED as SNC's selected
storage-format direction**, under Codex's mandatory conditions, quoted verbatim:

1. Final D-06 acceptance remains dependent on D-02.
2. Metadata rollback, R-23, remains an unresolved safety blocker.
3. O-FLOOR must be discharged before checkpoint/reclaim conformance work.
4. Performance costs must be measured before performance claims are made.
5. No final architectural freeze is authorized.
6. No C++ or Rust implementation is authorized by this decision.

And, from the same review: "Preserve the conditionally frozen R2′ and R9-A through R9-E contracts. R9-F remains
outside the freeze. The default replication admission threshold remains m = 2. Do not silently reinterpret the
guarantees to accommodate implementation limitations."

## What changes

- `docs/AFR_001_D06_FORMAT_DECISION.md` and `docs/AFR_001_D06_STORAGE_METADATA.md` record items 4-14 as conditionally
  accepted. Items 1 and 3 were already decided; item 2 follows from item 1.

## What does not change

- **D-06 is not finally accepted and nothing is frozen.** Items 15-21 (D-02, R-23, R-01, wire layout, migration,
  R9-F fields, costs) stay as recorded.
- **D-02 is not decided.** The next task is the focused external storage-system comparison (Codex's review, section
  5), documentation only.
- **Not authorized:** C++ or Rust work, R640 runs, X-2, migration work, D-02 acceptance, declaring R-23 resolved.
