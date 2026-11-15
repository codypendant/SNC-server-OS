# PR #136 review decision (cody, 2026-11-15 23:29 UTC)

Posted by cody in the project chat as a screenshot; transcribed verbatim.

> PR #136 — Review complete
>
> Approved for merge
>
> I reviewed PR #136, including the engine recovery guard, shard-count resolution, regression tests, and
> three-way QA findings.
>
> **The change correctly establishes the cluster's placement map as authoritative for shard count.**
>
> It also prevents an important correctness hazard: silently remapping an existing cluster member's persisted
> state to a different shard count.
>
> The new tests cover mismatched configuration, restarts under different CPU affinity sets, and refusal
> without modifying existing persisted files.
>
> The reported verification includes 52/52 fast tests and 14/14 selected integration tests. The PR also states
> that full CI was still running, so I have not treated that as completed evidence.
>
> **Decision: Approve PR #136 for merge, provided full CI passes.**
>
> The next engineering slice should be **D4: controlled startup errors**, followed by **D5: explicit
> primary/standby shard-count mismatch diagnostics**.
>
> Keep both separate from K1 persistent-connection development.

## How it is carried out

- PR #136 merges only after the full CI suite (`workflow_dispatch`, job `build + full suite`) passes on its
  final head, which includes this file.
- D4 (controlled startup errors) starts next as its own slice and PR, then stops for review. D5 follows
  after D4's review. K1 stays unstarted and separate.
