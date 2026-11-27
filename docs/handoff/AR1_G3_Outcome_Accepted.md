# AR-1 G3 outcome: NARROW, accepted (2026-11-27)

- **Decision:** cody relayed Codex's final review of PR #196 on 2026-11-27 at 23:33 UTC. The review approves the
  merge and accepts NARROW as the formal G3 result. It is filed verbatim in
  [PR196_Codex_Final_Review.txt](PR196_Codex_Final_Review.txt). PR #196 was merged at 22:26 UTC (`1a5f1b0`).
- **Evidence:** [experiments/ar1/results/G3_REPORT.md](../../experiments/ar1/results/G3_REPORT.md). The raw archive,
  `experiments/ar1/results/g3/ar1-g3-results.tgz` (sha256 `2de25abe…e16c`), is kept with the manifest, the summary
  and the driver log. Codex independently re-checked the checksum, the counted seeds and every verdict.

## Recorded with the outcome (kept, not rewritten)

- **Thin coverage:** 5.TN_duplicate_recorded was exercised in 1 run, 5.TF_stale_rejected in 3 runs, and 5.conflict in
  9 SNC runs. These are valid passes, but not broad statistical coverage.
- **Determinism divergence (not gated):**
  - SNC's recovery failed after a workflow step reorder (`NoResultFound`; no further tool calls).
  - Plain DBOS step 2 completed with no observed violation.
  - DBOS reported neither divergence.

  Its architectural meaning is an open item. The outcome stands either way.
- **Other limits:** one VM (6 vCPU, NVMe; recorded as a deviation before the run), one scenario family, PostgreSQL
  18.7 and DBOS 3.6.0. The author of plain step 2 knew the checker's clauses.

## What NARROW does and does not mean (from the review)

- AR-1 tested SNC's focused agent runtime built on PostgreSQL and DBOS. It did not test the state-native substrate or
  the original OS concept, and it is not a failure of either.
- Within AR-1's scope, SNC showed no correctness guarantee beyond plain DBOS with 284 added lines. SNC's extra cost
  per consequential commit is 4.7 ms at p50 and 11.1 ms at p99.
- The review's consequences:
  - don't expand the custom S1 runtime to duplicate DBOS;
  - treat an SDK or a light state and provenance layer as a serious candidate;
  - keep all of the original SNC research;
  - continue Track B;
  - don't choose a final direction from AR-1 alone.

## Not decided

- **No direction change.** There is no SDK, runtime or hybrid pivot, no S1 runtime development, and no product-gate
  work.
- **Track A:** cody gave P-A the same evening ([TRACK_A_PA_Owner_Decision.md](TRACK_A_PA_Owner_Decision.md)); the execution plan is [docs/TRACK_A_EXECUTION_PLAN.md](../TRACK_A_EXECUTION_PLAN.md), docs
  only.
- **Track B** continues under its approved P-B and the criteria frozen for it.
- **Not resumed:** replication, raft-rs and the full OS implementation each need their own decision.
