# Owner decisions on Codex's review of PR #165 (2026-11-18 20:44 UTC)

**Source.** The coordinator asked cody in the project chat: "Two of Codex's items are owner decisions that need your
word before they are recorded, and I recommend yes on both: 1. Treat a revived segment after a lost delete as harmful,
so recovery refuses it instead of leaving it to later cleanup. 2. Take the segment-link rule as the direction to
verify, not as a frozen design." cody answered: **"Yes, on both."**

**OD-5. Lost-unlink revival is harmful.** Recovery refuses a log in which an obsolete segment above `L` has revived
after a lost unlink. Later reconciliation is not relied on to clean it up. This is the same strict policy as OD-2 for a
lost binding.

**OD-6. R-CHAIN is the direction to verify, not a frozen design.** `docs/AFR_001_D02_OBSOLETE_SEGMENT.md` is the
design under verification. It is not accepted or frozen.

**Still open.** D-02, R-23 and O-FLOOR. Before final acceptance: the spare-header write ordering (U2), the per-stream
reseed (O-R) and the same-nonce C15 case. No X-2, R640 or implementation.
