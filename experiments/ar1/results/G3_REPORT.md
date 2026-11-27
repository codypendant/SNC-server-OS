# AR-1 G3 report: counted campaign

- **Outcome (section 9): NARROW.** Plain DBOS step 2 meets items 1-7 with zero violations over the full counted
  matrix, and it does so in 284 added lines, against L = 300.
- **Authorization and run:**
  - cody's G3 go is in [handoff/AR1_G3_Owner_Decision.md](../../../docs/handoff/AR1_G3_Owner_Decision.md), under
    prereg revision 3.
  - The campaign ran on a dedicated Vultr VM (6 vCPU, 16 GB, NVMe), a deviation recorded before the run in
    [AR1_G3_Freeze.md](../../../docs/handoff/AR1_G3_Freeze.md). It ran from 2026-11-27 02:00 to 21:35 UTC, with no
    restarts.
- **Frozen commit:** `575f493`. The manifest's freeze line matches the freeze record's trees, and every counted row
  carries code hash `57b0bd493b10645f`.
- **Raw data:** `results/g3/ar1-g3-results.tgz` (sha256 `2de25abe…e16c`), plus `summary.json`, `manifest.txt` and
  `driver.log`.
- **Independent re-check:** I rescored every row from the raw files with the frozen code. The verdicts, the outcome
  and the seed sets are identical. The counted seed sets equal `seed(point, i, "counted:575f493…")`, so no seed was
  re-rolled. There were no invalid runs, no repeats and no caps hit.

## Phase 0: hard gate passed

| Check | Valid runs | Pass |
|---|---|---|
| OI-1a, OI-1a control, OI-1a-v, OI-1b, OI-1c, OI-1d, C-7, C-7b | 20 each | 20 each |
| OI-1e (1,000 freezes) | 20 | all subchecks pass, including the negative control |

## Checker adequacy on the frozen code

All 14 mutations were run, and every one of the 265 runs was flagged with the clause the mutation breaks.

## Counted arms (128 points × 20 seeds = 2,560 runs each)

| Arm | Pass | Violation | Missing point | Unresolved | Points reached | Unexercised clauses |
|---|---|---|---|---|---|---|
| SNC (`snc_rt`) | 2,560 | 0 | 0 | 0 | 128 | none |
| Plain DBOS step 1 | 0 | 1,480 | 1,080 | 0 | 74 | 10 (expected; it is data) |
| Plain DBOS step 2 | 2,560 | 0 | 0 | 0 | 128 | 4.gateway, 3.late_marked |

- The two clauses that plain step 2 never exercises are not applicable to it. This reading was recorded before the
  run:
  - 4.gateway checks SNC's own gateway decision log, which is a consistency check only;
  - 3.late_marked is the other branch of item 3. Plain step 2 leaves a crashed invocation result unknown, which item
    3 also allows.
- **Thin coverage** on both arms: 5.TN_duplicate_recorded was exercised in 1 run on each arm. 5.TF_stale_rejected
  was exercised in 3 runs on each arm. 5.conflict was exercised in 9 runs on SNC and 1 on plain step 2. Each clause
  passed every time it was exercised.
- **Plain step 1's main failures:**
  - unmet: 3.states, 3.late_marked and 7.committed (1,480 runs each), 3.no_regenerate (1,460) and 7.shown (1,310);
  - violations: 4.mapped and 7.executed (1,460 each), 2.no_repeat and 3.recall (260 each), 2.end_state (220) and
    5.conflict (209).

## Latency budget B (1,000 tasks per arm, interleaved, stub at 500 ms)

| | SNC | Plain step 2 |
|---|---|---|
| Net ms per task, p50 | 597.2 | 521.2 |
| Net ms per task, p99 | 864.7 | 686.4 |
| Consequential commits per task | 16 | 16 |
| WAL commit records per task | 65 | 54 |
| WAL bytes per task | 62,888 | 37,568 |
| Blob bytes per task | 104 | 56 |

- B at p50 is 4.74 ms per consequential commit, against a limit of 10.
- B at p99 is 11.14 ms per consequential commit, against a limit of 50.
- **B holds**, at a factor of 0.47.
- All 2,000 tasks completed.

## Determinism divergence (reported, not gated)

T-I and T-F were swapped between a b3 crash and its recovery, with the app version pinned. DBOS reported nothing in
any arm:

| Arm | Result |
|---|---|
| SNC | The workflow failed with `NoResultFound`, did no further tool calls, and gave a 2.end_state violation |
| Plain step 1 | Completed, with 4.mapped and 7.executed violations |
| Plain step 2 | Completed. It queried T-I's status, then called T-F and T-N once each, with no violation |

## What NARROW means here

It means only what section 9 says. On these candidates, plain DBOS with about 284 added lines meets items 1-7 over
the full matrix. At that size, SNC's runtime shows no measured correctness advantage. SNC costs about 4.7 ms per
consequential commit at p50, within the budget.

The decision package (OD-P1..P8) said that if plain DBOS plus a little code suffices, S1 narrows towards an SDK. Any
change of direction is cody's decision, and this report decides nothing. The S1 product gate and the Track A/B
comparisons are separate and not affected.

**Limits:**
- one VM;
- one scenario family;
- PostgreSQL 18.7 and DBOS 3.6.0;
- plain step 2 was written with knowledge of the checker's clauses;
- the coverage of three clauses was thin.
