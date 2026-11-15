# PR #133 review: R1 multi-process CPU placement (cody, 2026-11-15 20:09 UTC)

Transcribed from two screenshots cody posted in the project thread.

> **PR #133 --- Review complete** (Approved for merge)
>
> I reviewed PR #133, including the CPU-affinity fix, regression test, architecture analysis, and QA findings.
>
> **The correction is valid and appropriately scoped.**
>
> The replication catch-up reader previously widened its CPU affinity beyond the process's assigned CPUs. The
> fix captures the allowed CPU set when the engine starts and applies it to the reader.
>
> **Verification**
>
> - Regression test demonstrated failure on the original implementation and success after correction.
> - End-to-end verification confirmed correct CPU affinity, including after promotion.
> - Fast suite: 48/48 passed.
> - Three independent QA reviews reported no blockers.
>
> **Important follow-up**
>
> The review identified another confirmed defect: `snc_server` derives its default shard count from the host's
> CPU count rather than the process's allowed CPU set.
>
> That can cause serious oversubscription in containers or restricted CPU environments.
>
> **Decision: Approve PR #133 for merge.**
>
> I recommend **D2-a (correct default shard count)** as the next dedicated engineering slice. Keep the broader
> CPU-placement policy changes separate until their behavior and deployment implications are reviewed.

Recorded for: D2-a in `docs/CPU_PLACEMENT_R1.md` section 5, started next as its own slice. R1-a, R1-b, R1-c
and R2-a stay unstarted until reviewed.
