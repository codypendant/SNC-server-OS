# INVALID FOR CRITERIA / INVALID FOR CAMPAIGN EVIDENCE

This directory holds the output of the CP-1A pilot that a Claude Code
session on the R640 started on 2026-11-06, before the CP-1A review.
cody stopped it during its second run. One run finished.

It is kept as a record. **No number in it may be used** to choose N, f,
V_MAX, M_B, grid spacing, thresholds or claims, or to feed `CRITERIA.md`
(PR #61 review, `docs/handoff/PR61_Review_Handoff.txt`). That includes
its 1,278,015 ops/s, its ~1.016 ms p99 and its 488.5 bytes/object.

Why it is invalid:
- CP-1A was not authorized when it ran.
- The load was closed-loop, so its p99 suffers coordinated omission
  (docs/CP1A_DRAFT_REVIEW.md F1; `tests/test_net_bench_rate.py`
  `stall_closed` shows a closed loop hiding a 400 ms stall).
- Its mix flags were nested, so the effective mix was read 59.85%,
  deposit 25.65%, transfer 9.50%, index 5.00%, not the plan's wording.
- The CPU governor was `schedutil`, and the client ran cross-socket
  (PILOT-X), not the baseline allocation.
- It ran from an unreviewed, uncommitted state.

The superseding plan is `docs/PILOT_PLAN.md`.
