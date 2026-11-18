# Owner decision: term history derived from WAL records (cody, 2026-11-18)

This is the owner's decision record, in the format of `PR148_Owner_Decisions.md` and `PR155_Owner_Decision.md`.

## The decision

cody's message in the project, at 2026-11-18 00:00 UTC (6:00 PM CT on 2026-11-17), verbatim:

> Yes.

It answered the coordinator's question in the project chat: "do you approve deriving the term history from the WAL
records, with Codex's four conditions? I recommend yes." That question followed Codex's review of PR #157
(`docs/handoff/PR157_Codex_Review.md`), which recommended conditional approval.

The decision is to derive the term history `H` from per-record WAL terms, instead of keeping a separate history
structure in the style of KIP-101. It is conditionally approved under Codex's four conditions:
1. `H` must be reconstructible from validated, retained WAL records and a durable compaction floor.
2. WAL corruption must never silently become an acceptable history truncation.
3. The revised contract must explicitly preserve G-14(a)'s refusal behavior.
4. Final acceptance depends on D-02's design of the durability boundary.

## What changes

The `H_i` paragraph of `docs/AFR_001_SAFETY_ANALYSIS.md` (Member state) is revised. The revision is recorded, with the
four conditions written in, and is conditionally approved. The R6.4 sentence on a damaged `H_k` and G-14(a)'s
term-history sentence follow it. The revised text is the contract; the earlier text is kept in the paragraph's
revision note.

## What does not change

- **D-06 as a whole is not accepted or frozen.** `docs/AFR_001_D06_STORAGE_METADATA.md` stays PROPOSED.
- **Metadata rollback stays an explicit D-02/R-23 blocker.** This covers a metadata write that was acknowledged and
  then disappears, and silent fallback to the older A/B copy. The format does not solve it.
- **The completion procedure is not accepted.** The small, preregistered crash-consistency model goes first (Codex's
  review of #157).
- **Migration and mixed-version decisions stay separate.**
- **Not authorized:** C++ or Rust work, R640 runs, the X-2 harness, D-02 acceptance, an R9-F freeze, any change to
  m = 2.
