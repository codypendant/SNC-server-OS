# Owner decisions on PR #168 items Y1-Y3 (cody, 2026-11-18)

cody, 2026-11-18 at 23:09:37 UTC, answering the coordinator's recommendation in the project chat (YES now on Y1, Y2
and Y3 as design directions; HOLD on Y4, Y5 and Y6; start the focused design note):

> Yes, I'm going with your recommendations.

That recommendation followed Codex's review of #168 (`docs/handoff/PR168_Codex_Review.md`). The decisions use Codex's
wording.

## Decided: design directions only

- **OD-7 (Y1).** "R-CHAIN should validate every retained WAL segment after the first. Do not narrow the check to the
  newest segment." This supersedes OD-6's "direction to verify" for this point. R-CHAIN is still not frozen.
- **OD-8 (Y2).** "Use HeaderFirst spare ordering: write and synchronize the take-time header before rename, with
  `RENAME_NOREPLACE` and a clearly defined `spare.tmp` recovery namespace."
- **OD-9 (Y3).** "A replaced WAL segment preserves its nonce and identity."

In Codex's words: "The YES decisions above authorize recording the selected design directions only. They do not
authorize implementation or a final D-06 revision."

## Not decided

- **Y4** (the D-06 format revisions), **Y5** (how rule 4 reads R2′, R9-A, R9-B and `ack_p`) and **Y6** (the V8 rules
  and the per-stream reseed) stay on **HOLD**. cody's yes does not rule on Y5. It stays open for section D of the
  design note.
- D-02, R-23, O-FLOOR and final D-06 acceptance stay open.
- **Not authorized:** implementation (C++ or Rust), a final D-06 revision, a TLA+ revision, X-2, R640 runs, and
  migration.
