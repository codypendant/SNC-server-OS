# Garnet durability under dm-log-writes (B1 baseline, slice 4)

This is a harness result, not a comparative result.

- Workflow run: https://github.com/codypendant/SNC-server-OS/actions/runs/45083258684 (PR #193, head 284e771), on a GitHub-hosted ubuntu-24.04 VM.
- The loop device runs with write back and fua 0, as in `docs/DIRECTION_COMPARISON_PLAN.md` 4.1.
- Garnet 2.2.13 runs `bench/garnet` with `--aof --aof-commit-freq 0 --aof-commit-wait`.
- The load is `resp_bench` at 2,000 ops/s for 16 s after a 2 s warm-up: 50% transfers, 20% reads, 3,000 accounts.
- There are 20 marks, each with the stale-image negative control.

| `--device-type` | marks failed | negative controls rejected / void | verdict |
|---|---|---|---|
| Native (the Linux default) | 19 of 20 | 19 / 1 | **FAIL**: loses acknowledged writes |
| RandomAccess | 19 of 20 | (same pattern) | **FAIL** |
| FileStream | 0 of 20 | 19 / 1 | **PASS** |

FACT, from the run logs:
- **Native and RandomAccess.** For the first 9 marks after the load, the replayed image held none of the 3,000 accounts, although every create had been acknowledged. Later marks held the accounts but lacked hundreds to thousands of acknowledged deposits and transfers.
- **FileStream.** Every mark was exact. The negative control rejected every image that was missing 14 to 19 acknowledged updates.

INF: Native and RandomAccess acknowledge a commit before any flush or FUA covers it, so the data can sit in a volatile cache. This matches the source reading in `docs/research/TRACKB_PREREQUISITES.md`, which found that Native never flushes and FileStream opens its files with O_SYNC.

## Consequence (plan 4.2 gating order, unchanged)
- Garnet Native fails the durability test, so its numbers are diagnostic only.
- Garnet FileStream is the gating Garnet configuration, and only configurations that pass this test may be screened or counted.
- The R640 runs each counted configuration only with the binary hash, configuration and command line that passed here.
