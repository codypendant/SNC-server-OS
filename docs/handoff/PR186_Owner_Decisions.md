# PR #186: owner decisions P-G1 and P-B (2026-11-22 03:16 UTC)

## What cody said
cody, in the project chat, 2026-11-22 03:16:12 UTC, replying "yes on both" to the coordinator's 03:15:26 UTC post.
That post asked for two decisions and recommended yes to each:

> **1. P-G1, approve the plan's numbers and scope.** I recommend yes as proposed:
> - **B1:** pass is 2x Garnet at a 1 ms p99, and at least parity with 20% transfers.
> - **B2:** pass is 3x Restate Virtual Objects, with recovery no slower.
> - **Track A:** the 1.5x factor between the Python and Rust line limits.
> - The criteria get frozen in CRITERIA.md before any comparative result exists.
>
> **2. P-B, build the Track B harness and run it on the R640.** I recommend yes. It needs no engine change, it tests
> the original idea directly, and it would start with the durability and equivalence checks before any measured run.
> The two R640 sessions need your hands, and I'd give you copy-paste cards for each.
>
> Track A (P-A) is not up for decision yet. It waits for the G2 gaps in PR #185 to close, and G3 stays on hold.

Before that, Codex approved revision 3 for documentation merge (`PR186_Codex_Review_rev3.md`).

## What is decided
- **P-G1.** The scope and every PROPOSED number in `docs/DIRECTION_COMPARISON_PLAN.md` section 9 are approved as
  written. The numbers are copied into `bench/results/dc1-trackb/CRITERIA.md`, which is frozen before any comparative
  result exists.
- **P-B.** Build the Track B harness, and run B1 then B2 on the R640 in two sessions, with cody's hands. The work
  starts with the durability and equivalence checks (plan 4.1), never with a measured run.

## What is not decided
- P-A (Track A). It waits until PR #185's G2 gaps are closed and reviewed.
- AR-1's G3, which stays on HOLD.
- Any change to SNC's engine. If the harness seems to need one, the work stops and is reported.
- Replication, custom replication or raft-rs.
- Selection of any production dependency. Garnet, PostgreSQL and Restate are baselines only.
- Any decision about the original OS.

Codex's six conditions before testing (plan section 9) apply. Invalid or incomplete results are reported as
UNRESOLVED, and all original SNC research and model evidence is kept.
