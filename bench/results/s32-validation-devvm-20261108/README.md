# S3.2 validation, dev VM, 2026-11-08

Validation of S3.2, the Engine lifecycle gate (`docs/ENGINE_LIFECYCLE.md`).
The work order is `docs/handoff/PR78_S31_Review.txt` sections 10-15 and 18.

- Machine: dev VM with 4 vCPU, kernel 6.18, ext4 on virtio.
- This is functional evidence only. No R640 load was run.

Headline evidence comes only from complete serial runs (`ctest -j1`) at
5a1dc9b. Every run is listed in order. Failed and stopped runs are kept
and listed, and are never combined with reruns.

## Runs in order

| # | SHA | Run | Result | Counted |
|---|---|---|---|---|
| 1 | ab76766 (worktree) | `test_lifecycle` alone in Release, ASan, TSan | Release and ASan passed. TSan found a race: a refused `stats()` during STOPPING read `final_stats` while the workers wrote it (`tsan-test_lifecycle-first-stats-race.log`). Fixed in the engine (read only once STOPPED) | diagnostic |
| 2 | ab76766 | negatives, attempt 1 (`negative/attempt1/`) | 3 caught, then I stopped it during mutation 4: the independent review changed the code. Mutation 3 there was caught by the structural abort, not the reason it claimed (review finding 3). Mutation 4's partial run overlapped a test run of the new code on the same ports, so it is not evidence | no |
| 3 | e44ab18 | negatives, attempt 2 (`negative/attempt2/`) | **8 of 8 caught**, see below | **yes** |
| 4a | 5a1dc9b | Release serial, `release-run1-j1.log` | **41/41** | **yes** |
| 4b | 5a1dc9b | ASan+UBSan serial, `asan-run1-j1.log` | **41/41, 0 reports** | **yes** |
| 4c | 5a1dc9b | TSan serial, `tsan-run1-j1.log` | **41/41, 0 reports** | **yes** |
| 4d | 5a1dc9b | Release serial with `SNC_CKPT_IO=legacy`, `release-legacy-run1-j1.log` | **41/41** | **yes** |

- e44ab18 and 5a1dc9b differ only in docs and evidence files.
- Sanitizer reports were counted with
  `grep -c "ERROR: AddressSanitizer\|runtime error:\|WARNING: ThreadSanitizer"`: 0 in every log.
- `runs.txt` has the start and end times of each run.

## Negative mutations, attempt 2

`negative/run_negatives.sh`: each mutation is applied to a copy of the
repository outside it, built in Release, and runs one test.

| # | Mutation | Test | Caught by |
|---|---|---|---|
| 1 | the permit counts nothing | L1 | rc=1: stop went straight through STOPPING to STOPPED (did not wait for the operation inside) |
| 2 | `slot_report` without its permit | L2 | rc=134: `require_op` abort, "shard work submitted outside an Engine operation permit" |
| 3 | the same, with the checks in `on_shard` and `submit_` removed and the permit replaced by an uncounted park | L2 | rc=1: L2's probe, stop returned while the `slot_report` was inside; released, it would submit to a stopped worker |
| 4 | STOPPING still admits operations (the gate checks `running_`) | L1 | rc=124 (the 600 s timeout): a `check_refused` call entered during STOPPING and parked, so the test hung. A hang is how this defect shows; the test does not bound it |
| 5 | `stop()` does not wait for a start in progress | L7 | rc=1: L7's probe, stop returned during STARTING |
| 6 | a failed start lands in RUNNING or CONSTRUCTED (the pre-review code) | L9 | rc=1: `lifecycle() == FAILED` |
| 7 | the nested mark is single-slot | L10 | rc=1: the inner A call was refused (`g_l10_inner == 1` failed) |
| 8 | `stop()` from a shard thread is not detected | L11 | rc=1: the child ended by its alarm (it waited for itself), not SIGABRT |

Aborting mutations leave their `/tmp/snc-life-*` directories; I removed
them by name after the run.
