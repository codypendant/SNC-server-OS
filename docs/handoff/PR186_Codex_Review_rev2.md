# Codex review of PR #186, revision 2 (relayed by cody, 2026-11-22 02:50 UTC, verbatim)

SNC — Codex Review of PR #186, Revision 2
VERDICT: HOLD — ONE TARGETED BENCHMARK CORRECTION REQUIRED.
The five findings from the previous HOLD review are adequately addressed.
Verified:

* Track A CSV: 17 distinct points, 114 runs, including 94 S-M and 20 S-AP.
* Durability verification has been redesigned around recoverable device-state evidence.
* Full durable settlement is the deciding transfer boundary.
* Section 6 no longer extrapolates two benchmarks into proof of a complete operating system.
* The Rust/Python differentiation rules are objective and preregistered.
* The independent review findings and unresolved assumptions are recorded.

Remaining Blocker — Settlement Probe Budget
`docs/DIRECTION_COMPARISON_PLAN.md`, section 4.1.
The current probe design samples 1% of transfers while limiting probe reads to 1% of all operations.
At the proposed polling intervals and settlement SLOs:

* B1 can require approximately 20 reads per sampled transfer, producing approximately 4% additional operations at the 20% transfer point.
* B2 can require approximately 20 reads per sampled transfer, producing approximately 20% additional operations.

The current cap cannot be guaranteed while preserving sufficient settlement observations.
This risks invalid comparisons, censored measurements or asymmetric polling overhead.
Fix on site:

1. Define a mathematically consistent sampling rate, polling interval and overhead budget for B1 and B2.
2. Preregister a minimum number of valid settlement observations sufficient to evaluate p99.
3. Specify what happens when the sampling budget or observation minimum cannot be met.
4. Ensure probe overhead is charged or isolated consistently across all systems.
5. Preserve the same observable durable-settlement boundary for SNC and every baseline.
6. Conduct a focused independent review of the correction.

Do not reopen the five previously resolved findings unless the correction affects them.
Authorization Boundaries

* P-G1 remains pending final plan approval.
* P-B and P-A require separate owner authorization.
* PR #185's G2 gaps remain open.
* AR-1 G3 stays on HOLD.
* No custom replication, raft-rs integration or SNC engine changes.
* No decision to abandon the original OS.
* Preserve both research tracks.

Return revision 3 for Codex review after the single correction.
FINAL: HOLD PR #186. THE TWO-TRACK DIRECTION IS SUPPORTED; ONLY THE SETTLEMENT-PROBE BUDGET REMAINS BLOCKING.
