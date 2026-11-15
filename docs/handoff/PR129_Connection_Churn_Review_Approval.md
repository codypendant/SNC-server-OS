# PR #129 review: test_migrate connection churn (cody, 2026-11-15 15:10 UTC)

Transcribed from a screenshot cody posted in the project thread. Only the end of the review was visible; the
part above it held a results table, of which the last rows read: runs with sufficient capacity 26/26 passed;
regression tests (partly hidden); independent QA 3 reviews, no blockers; production code changes none.

> One limitation is documented: the capacity check is an estimate taken at startup, not a guarantee that the
> table cannot fill during execution.
>
> **Important architectural finding**
>
> SNC currently creates a new TCP connection for every control-plane request. That could become a significant
> scaling problem for distributed deployments.
>
> I recommend a separate engineering slice to design persistent, reusable control-plane connections,
> preserving witness liveness detection, fencing, and failover semantics.
>
> **Decision: Approve PR #129 for merge.**

Recorded for: C1 in `docs/TEST_MIGRATE_CONNECTION_CHURN.md`. Not started; waits for its own assignment.
