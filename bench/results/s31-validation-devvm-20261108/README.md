# S3.1 validation, dev VM, 2026-11-08

Validation of S3.1, the engine lifecycle fix for teardown with checkpoint
I/O in flight (`docs/S3_WRITEBACK.md` section 10). The work order is
`docs/handoff/PR77_S3_Review.txt` sections 9-14 and 18.

- Machine: dev VM with 4 vCPU, kernel 6.18, ext4 on virtio.
- This is functional evidence only. No R640 load was run.

Headline evidence comes only from complete serial runs (`ctest -j1`) at
da88df2. Every run is listed in order. Failed and stopped runs are kept
and listed, and are never combined with reruns.

## Runs in order

| # | SHA | Run | Result | Counted |
|---|---|---|---|---|
| 1 | parent of 1e1a4e5 | reproductions before the fix, `diag/repro-*` | stop during `checkpoint()`: stop returned and the checkpoint thread hung. `std::exit` with the engine running: ASan heap-use-after-free, read in `Shard::publish_gauges`, freed by the static `EngineState` destructor | diagnostic |
| 2 | 1e1a4e5 (worktree) | T14-T18 alone in Release, ASan and TSan | Release and ASan passed. TSan found 3 races in the T16 test: the main thread read gauges of an Engine being deleted. Fixed in the test (`diag/tsan-T14-T18-first-T16-race.log`) | diagnostic |
| 3 | 1e1a4e5 | negatives, attempt 1 (`negative/attempt1/`) | 2 of 2 caught, then I stopped it because the independent review changed the code | no |
| 4 | 791a813 | negatives, attempt 2 (`negative/attempt2/`) | **5 of 5 caught** | **yes** |
| 5a | da88df2 | Release serial, `release-run1-j1.log` | **40/40** | **yes** |
| 5b | da88df2 | ASan+UBSan serial, `asan-run1-j1.log` | **40/40, 0 reports** | **yes** |
| 5c | da88df2 | TSan serial, `tsan-run1-j1.log` | **40/40, 0 reports** | **yes** |
| 5d | da88df2 | Release serial with `SNC_CKPT_IO=legacy`, `release-legacy-run1-j1.log` | **FAILED 39/40**: `test_net_bench_rate`, see below | no |
| 5e | da88df2 | the same, run again in full, `release-legacy-run2-j1.log` | **40/40** | yes (rerun) |
| 6 | da88df2 | `test_net_bench_rate` alone, 10 times (`diag/net_bench_rate-10x-standalone.txt`) | 10/10 passed | diagnostic |

- 791a813 and da88df2 differ only in evidence files.
- Sanitizer reports were counted with
  `grep -cE "ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer"`.
- `runs.txt` is the raw record.

## The legacy failure (5d)

- **What failed:** `test_net_bench_rate` `scenario_stall_open` asserts `end_backlog == 0`. It reported 1: of the 6000 ops scheduled in the window, 5999 completed in it, and 1 was issued after the window ended.
- **Why it is not this change:** the test starts an in-memory server with no data directory, so no checkpoint runs and `SNC_CKPT_IO` does not apply. S3.1 changes only `stop()`/`checkpoint()` serialization and who owns the engine's state; neither runs during the measured window.
- **Cause (inferred, not proven):** an exact-zero check at a timing boundary, with the client late on the last slot after the 400 ms stall.
- **Follow-up:** run 5e passed, and so did 10 standalone runs. Per the rules this is recorded, not combined. The CP-1A harness is not changed here; the finding is left for review.

## Negative mutations (run 4)

Script: `negative/run_negatives.sh`. Each mutation is applied to a `git archive` copy of 791a813 and must make the named test fail.

| Mutation | Test | Build | What caught it |
|---|---|---|---|
| `stop()` without the lifecycle lock | T14 | Release | the waiter never returned (`becomes(bg.done)`), so it was abandoned |
| the same, through stop then delete | T16 | Release | the waiter never returned |
| `checkpoint()` without the lock | T15 | Release | the waiter never returned |
| the engine state back on a static owner | T16b | ASan | the child reported a heap-use-after-free: the PR #77 defect again |
| `stop()` without the lock, and a quiesce that ignores the checkpoint | T14 | Release | stop returned within 300 ms, so the shard was stopped under an active checkpoint |

## Independent review

A separate read-only review of the lifetime and ownership code found no blocker. The findings and what was done about each are in `docs/S3_WRITEBACK.md` section 10.4.
