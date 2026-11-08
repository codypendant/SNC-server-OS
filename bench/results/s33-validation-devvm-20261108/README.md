# S3.3 validation, dev VM, 2026-11-08

Validation of S3.3: the data-plane lease that replaces `Engine::runtime()`,
and the serialization of `promote()` with `checkpoint()`
(`docs/ENGINE_LIFECYCLE.md` sections 9-11). The work order is
`docs/handoff/PR79_S32_Review.txt` sections 6-14 and 18.

- Machine: dev VM with 4 vCPU, kernel 6.18, ext4 on virtio.
- This is functional evidence only. No R640 load was run.

Headline evidence comes only from complete serial runs (`ctest -j1`) at
68fb8a8. Every run is listed in order. Failed and invalid runs are kept and
listed, and are never combined with reruns.

## Runs in order

| # | SHA | Run | Result | Counted |
|---|---|---|---|---|
| 1 | 63f8219 (worktree) | `test_lifecycle` alone in Release, ASan, TSan | all passed, 0 reports | diagnostic |
| 2 | 63f8219 | negatives, attempt 1 (`negative/attempt1/`) | 4 of 4 caught. But `checkpoint-unserialized` was caught by order A's probe, not order B as the script claimed (both orders ran under one test name). Then the independent review changed the code | no |
| 3 | 055cfbf | negatives, attempt 2 (`negative/attempt2/`) | **INVALID**: `promote-unserialized` reported rc=0 because its pattern no longer matched the code, the patch step failed silently, and the unmutated build ran. The other 4 were caught. The script now stops on a failed patch | no |
| 4 | 1194619 | negatives, attempt 3 (`negative/attempt3/`) | **5 of 5 caught**, see below | **yes** |
| 5a | 68fb8a8 | Release serial, `release-run1-j1.log` | **41/41** | **yes** |
| 5b | 68fb8a8 | ASan+UBSan serial, `asan-run1-j1.log` | **41/41, 0 reports** | **yes** |
| 5c | 68fb8a8 | TSan serial, `tsan-run1-j1.log` | **41/41, 0 reports** | **yes** |
| 5d | 68fb8a8 | Release serial with `SNC_CKPT_IO=legacy`, `release-legacy-run1-j1.log` | **41/41** | **yes** |

- 1194619 and 68fb8a8 differ only in evidence files.
- Sanitizer reports were counted with
  `grep -c "ERROR: AddressSanitizer\|runtime error:\|WARNING: ThreadSanitizer"`.
- `runs.txt` has the start and end times of each run.
- The 41 tests include `test_lifecycle` with R1-R3 and C1-C4. C1 runs
  under TSan in run 5c.

## Negative mutations, attempt 3

`negative/run_negatives.sh`: each mutation is applied to a copy of the
repository outside it, built in Release, and runs one test.

| # | Mutation | Test | Caught by |
|---|---|---|---|
| R4 | the lease counts nothing | R | rc=1: stop never stayed in STOPPING (it did not wait for the lease) |
| R4b | a lease is granted in STOPPING | R | rc=1: `!eng.data_plane()` in STOPPING failed |
| C5 | `promote()` without the durable-control lock | C1A | rc=1: the promotion never waited at the lock (`debug_promote_at_control` never seen) while the checkpoint was held |
| C5b | `checkpoint()` without it | C1B | rc=1: the checkpoint completed inside the parked promotion |
| S3 | `DataPlane::run_on_all` from a shard thread not refused | R | rc=1: the child ended by its alarm (it waited for its own shard), not SIGABRT |

Aborting mutations leave their `/tmp/snc-life-*` directories; I removed
them by name after each attempt.
