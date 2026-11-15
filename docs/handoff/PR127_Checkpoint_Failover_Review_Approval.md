# PR #127 review: checkpoint failover blocking (cody, 2026-11-15 00:42 UTC)

Transcribed from a screenshot cody posted in the project thread. Only the end of the review was visible; the
part above it summarized the local results (51/51, two independent QA reviews).

> The hosted suite passed 50/51. The remaining `test_migrate` failure is also present on `main` and was
> reproduced with F1's scheduling change disabled. It is a separate unresolved issue, not demonstrated to be
> caused by PR #127.
>
> One architectural limitation remains: promotion still waits for the standby's own checkpoint to finish. That
> is an existing serialization guarantee, not a regression introduced by this PR.
>
> **Decision: Approve PR #127 for merge.**
>
> Next, I recommend investigating the `test_migrate` failure as a dedicated correctness issue before expanding
> functionality. The reported failure rates are too high to treat it as harmless CI noise.
