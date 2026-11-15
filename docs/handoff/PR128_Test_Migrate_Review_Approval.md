# PR #128 review: test_migrate hosted failures (cody, 2026-11-15 11:56 UTC)

Transcribed from a screenshot cody posted in the project thread. Only the end of the review was visible; its
first visible line says the correction distributes pinned shard threads across the available CPUs and limits
the default concurrency to the tested density.

> **Remaining concerns**
>
> Two findings deserve separate follow-up:
>
> - **Multi-process CPU contention:** SNC's default placement can still overload individual CPUs when multiple
>   nodes share a host.
> - **Connection tracking exhaustion:** Repeated migration tests can exhaust the host's conntrack table through
>   excessive connection churn.
>
> Neither blocks this test-harness correction. The 96 successful hosted rounds were performed with drained
> conntrack tables, so that limitation remains explicit.
>
> **Decision: Approve PR #128 for merge.**
>
> I recommend addressing connection churn next, followed by a review of SNC's multi-process CPU placement
> behavior.

Recorded for: R4 (connection churn) and R1 (multi-process placement) in `docs/TEST_MIGRATE_ROOT_CAUSE.md`.
Neither is started; each waits for its own assignment.
