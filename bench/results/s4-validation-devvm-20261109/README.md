# S4 validation, dev VM, 2026-11-09

Validation of S4, the staggered final checkpoint sync with K node tokens
(`docs/S4_SYNC_TOKENS.md`). The work order is
`docs/handoff/PR80_S33_Review_S4_Authorization.txt`, sections 12 and 14.

- Machine: dev VM with 4 vCPU, kernel 6.18, ext4 on virtio.
- This is functional evidence only. No R640 load, CP-1A or comparator was
  run, and no K was chosen from these runs.

Headline evidence comes only from series 2: complete serial runs
(`ctest -j1`) of code at **6cb1cee**. Commits after 6cb1cee
(38f380f, 3c71d0d and later) change only files under `bench/`
(`git diff --stat 6cb1cee HEAD -- . ':!bench'` is empty). Every run is
listed in order. Failed and invalid runs are kept and never combined with
reruns.

## Runs in order

| # | Code | Run | Result | Counted |
|---|---|---|---|---|
| 1 | f9b0264 | `test_s4_sync_tokens` alone, Release, each test | all passed | diagnostic |
| 2 | f9b0264 | negatives, attempt 1 (`negative/attempt1-script-error/`) | **INVALID**: the script was cut short when copied (bash syntax error); no mutation ran | no |
| 3 | f9b0264 | negatives, attempt 2 (`negative/attempt2/`) | **6 of 6 caught**, see below | **yes** |
| 4a | fbc6a24 | series 1 Release serial, `release-run1-j1.log` | 42/42 | no (superseded) |
| 4b | fbc6a24 | series 1 ASan+UBSan serial, `asan-run1-j1.log` | 42/42, 0 reports | no (superseded) |
| 4c | fbc6a24 | series 1 TSan serial, `tsan-run1-j1.log` | **FAILED**: `test_s4_sync_tokens` T6. TSan reported a data race in the test's own SIGABRT handler: it read the engine pointer, a plain global written by the test thread, on a shard thread. Test code only; fixed at 6cb1cee (an atomic). The `rc=` line in `runs.txt` was added by hand: I stopped the wrapper shell by mistake while ctest kept running, and read the result from the log | no |
| 5a | 6cb1cee | series 2 Release serial, `release-run2-j1.log` | **42/42** | **yes** |
| 5b | 6cb1cee | series 2 ASan+UBSan serial, `asan-run2-j1.log` | **42/42, 0 reports** | **yes** |
| 5c | 6cb1cee | series 2 TSan serial, `tsan-run2-j1.log` | **42/42, 0 reports** | **yes** |
| 5d | 6cb1cee | series 2 Release, `SNC_CKPT_IO=legacy`, `release-legacy-run2-j1.log` | **42/42** | **yes** |
| 5e | 6cb1cee | series 2 Release, the whole suite at K=1 (`SNC_CKPT_SYNC_TOKENS=1`), `release-k1-run2-j1.log` | **42/42** | **yes** |

- Sanitizer reports counted with
  `grep -c "ERROR: AddressSanitizer\|runtime error:\|ERROR: LeakSanitizer\|WARNING: ThreadSanitizer"`.
- ctest prints test output only on failure, so the override banners are
  not in the logs. Checked separately at 6cb1cee: `test_persistence`
  prints `snc: SNC_CKPT_SYNC_TOKENS=1 overrides ckpt_sync_tokens` and
  `snc: SNC_CKPT_IO=legacy overrides ckpt_io`. `test_s4_sync_tokens` and
  `test_s3_writeback` ignore those overrides (each sets its own value).
- The default (runs 5a-5d) is K=0, unlimited: the S3 path.
- `server-health-check.txt`: a manual HEALTH and GAUGES check of
  `snc_server --ckpt-sync-tokens 2`.
- `runs.txt` has the start and end times of each run.

## Negative mutations (S4-T10), attempt 2

`negative/run_negatives.sh`: each mutation is applied to a copy of the
repository outside it (from `git archive` of f9b0264), built in Release,
and runs one test. The script stops on a pattern that does not match.

| # | Mutation | Test | Caught by |
|---|---|---|---|
| N1 | token bypass (`try_acquire` result ignored) | T1 | rc=1: `syncs_inflight == e` failed (all 8 in flight at K=1) |
| N2 | K+1 (`>=` to `>`) | T1 | rc=1: grants never settled at min(K, 8) |
| N3 | token leak on success | T2 | rc=124: hung in `Engine::start()`. The checkpoint recovery takes at start never got its token back, so its latch never opened (`token-leak-T2-backtrace.txt`, from a separate rerun of the same mutant). Caught, by a hang, not a diagnostic |
| N4 | token leak on the fatal path (release after `storage_fatal`) | T6 | rc=1: the abort saw grants=24 releases=23 |
| N5 | grants by free capacity, not ticket order | T3 | rc=1: a part outside the ticket window held a token |
| N6 | CC-3 check dropped before the ticket | T3 | rc=1: parts with their WAL held never stayed in DURABLE_WAIT |

The test fix after f9b0264 (6cb1cee: test waits for the syncs as well as
the grants, an atomic in the T6 handler) does not touch any mutated line
or the checks that caught them.

Aborting mutations left their `/tmp/snc-s4-*` directories; I removed
them by name.
