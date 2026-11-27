# Track A: owner decision P-A (2026-11-27)

## What cody said
In the project chat, after Codex's final review of PR #196:
- 23:34:21 UTC: "Yes."
- The coordinator asked, at 23:34:26 UTC: "Is that a yes to P-A, meaning I can go ahead and run the Track A
  comparison (DBOS, Restate, Temporal, LangGraph) once the plan is reviewed? Or were you just confirming the plan
  above? Running Track A is a separate gate, so I want to be sure before I start it."
- cody, at 23:34:52 UTC, in reply: "Yes, run what you need to run."

## What is decided
- **P-A:** start Track A of `docs/DIRECTION_COMPARISON_PLAN.md` (revision 3, approved at P-G1).
- **Arms:** DBOS, Restate, Temporal and LangGraph, plus SNC's AR-1 arm, written idiomatically and judged only on the
  externally observable guarantees O-1 to O-5.
- **Order:** the thresholds and criteria are frozen in `bench/results/dc1-tracka/CRITERIA.md`, and Codex reviews the
  execution plan through cody, before any comparative result exists.
- **Arms built:** the question cody answered named all four engines. So all four are built, in the plan's order,
  and the plan's early stop does not cut the later arms. Section 6's rows are unchanged: the first competitor that
  meets O-1 to O-5 within L still decides the row ("SDK at most"). The later arms add evidence for criteria 1 and 3.
  See [TRACK_A_EXECUTION_PLAN.md](../TRACK_A_EXECUTION_PLAN.md) section 2.

## What is not decided
- **Direction:** no product pivot, no SDK or runtime choice, no S1 runtime development and no product-gate work. The
  results go to cody with a recommendation.
- **Dependencies:** none is selected for production. The competitor engines are experiment tools only.
- **SNC changes:** no change to SNC's engine, and no tuning of SNC's AR-1 arm to win.
- **Replication:** no replication, raft-rs or custom replication work.
- **Original research:** none of it is dropped.
