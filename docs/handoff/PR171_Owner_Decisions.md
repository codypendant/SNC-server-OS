# Owner decisions on the PR #171 package (cody, 2026-11-19)

cody, 2026-11-19 at 06:37:41 UTC, in the project chat, with a screenshot of Codex's recommendation table and Codex's
review of #171 (`docs/handoff/PR171_Codex_Review.md`):

> I'm saying yes. I'm agreeing with Codex on everything except for the one. I just sent a screenshot. I'm agreeing
> with all of those points.

and at 06:37:57 UTC:

> Do it exactly like Kodak said.

("Kodak" is voice-to-text for Codex, as the coordinator read it.) The screenshot's table reads: "Y5 — Watermarked acknowledgments: YES, recorded
revision; Y6a — Restart protection: YES, design direction; Y6b — Temporary-file recovery: YES, design direction;
Y6c-1 — Whole-node reseed: YES, initial direction; Y6c-2 — R6.5 revision: CONDITIONAL, complete I2–I5 review first;
Y8 — Correct both seed-crash failures: YES, design direction", and "Y4 and Y7 remain on HOLD". "The one" is read as Y6c-2, the
only item Codex did not recommend as a yes (the recorder's reading; "Do it exactly like Kodak said" gives the same
record either way). The decisions below use Codex's wording and strength.

## Decided

- **OD-10 (Y5). R9-A′, a recorded revision.** "A replica counts toward HA_DURABLE only after applying the entry and
  durably covering it with its watermark." Recorded in place, with the earlier text kept:
  - **Words changed:** R9-A becomes R9-A′ (`docs/AFR_001_SAFETY_ANALYSIS.md`, R9); R9-B's lease test ("judged by the
    position `m` reports (R9-A′)") and its `ack_p` definition ("reported … as acknowledged (R9-A′)"); G-30 (A) and
    (B) (`docs/SNC_GUARANTEES.md`).
  - **Meaning changed, words kept:** R9-C's count; R9-F and G-30 (F) (`ack_p` lags, so `c_m` waits longer); R2′ rule
    2 and S5 (the count is of R9-A′ reports); R6.3′ item 1 (the durable end reported is the watermarked end, through
    write-path rule 4); "caught up", R1.1 and S admission (the watermarked end); section 0's acknowledgement ("has
    `e` durably, under its watermark"); G-11's "two durable copies" and the HA_DURABLE definition
    (`docs/CORE_CONTRACTS.md` 126, 148): a copy counts once watermarked; D-03's "durable acknowledgment"; PR155's
    catching-up argument and the M_ackLeased/M_ackDurable citation (they checked R9-A, not R9-A′); SA section 3's
    preservation table (its R9 rows argue R9-A); the primary's own copy (covered through write-path rule 3, argued
    only); D-06 E18, E20, E25.
  - **Asked with Y5, not in Codex's Y5 wording:** recovery's sync of `(W, E]` and then its raise of the watermark to
    `E`, after the segment barrier (contract 2.2 step 9, contract 11). Codex listed step 9 for the targeted review;
    it is recorded as part of the proposal Y5 decided, under that review (round 4, `docs/AFR_001_DURABILITY_DECISIONS.md`
    section 7), not as a separately accepted rule.
  - **Freeze status (the recorder's reading):** R9-A′ takes R9-A's place in the conditionally frozen set R9-A to R9-E,
    under PR155's conditions. Its condition names the D-02 watermark, and D-02 is on HOLD, so R9-A′ has no accepted
    mechanism until D-02 is accepted.
  - **Not closed:** P5's excluded silent-loss clause. R9-A′ is necessary, not sufficient (contract 5.3).
  - Unchanged: `m = 2`, R9-D, R9-E, P1-P4, P6, P7, S1-S4.
- **OD-11 (Y6a). Design direction.** V8-1, V8-L and V8-3, "including the expanded locking requirements for
  asynchronous direct-I/O files" (contract 2.3, E26). "V8-R remains the fallback if the target-kernel verification
  invalidates V8-L."
- **OD-12 (Y6b). Design direction.** "HeaderFirst, the temporary-file namespaces, "RENAME_NOREPLACE", and the
  documented recovery behavior" (note B.4; refusal on `EEXIST`).
- **OD-13 (Y6c-1). Initial direction.** "Prefer whole-node in-place reseeding. Retain per-stream reseeding as a
  possible later optimization." (Contract 6.4, 6.5.)
- **OD-14 (Y8). Design direction.** "Correct both G-48 crash-only failures: 1. Introduce a durable seed marker before
  transferring data, allowing an interrupted initial seed to restart automatically. 2. Create and synchronize
  "links_generation" before the first node-record write. Do not introduce new G-48 exceptions merely to accommodate
  these failures." (E30, E31.)

## Not decided

- **Y6c-2 (the R6.5 revision): CONDITIONAL, not accepted.** "Support the proposed R6.5 direction … However, do not
  accept the frozen-contract revision until I2–I5 have been explicitly re-argued for the state where a node is
  complete but not yet positioned. The analysis must cover acknowledgments, leases, promotion, membership,
  crash/restart, and reconciliation." R6.5's frozen text is unchanged. The re-argument is
  `docs/AFR_001_R65_INVARIANTS.md`; Y6c-2 returns to the owner after it is reviewed.
- **Y4 and Y7: HOLD.**
- D-02 (HOLD), R-23 and O-FLOOR stay open. No final D-06 freeze.

In Codex's words: "NO IMPLEMENTATION OR FINAL ARCHITECTURAL ACCEPTANCE." Not authorized: C++ or Rust, migration, X-2,
R640 runs, any model or experiment (E-1, E-2, E-3, M-1), each needing its own go. Codex's priorities 2-5 (V8-L on the
target kernel, the watermark's device cost, O-FLOOR, the D-06 byte-level specification) are not started.
