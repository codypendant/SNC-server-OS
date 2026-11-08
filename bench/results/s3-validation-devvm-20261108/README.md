# S3 validation, dev VM, 2026-11-08

Validation gate of the PR #76 review (section 17) for S3 (C3 bounded
checkpoint writeback, `docs/S3_WRITEBACK.md`). Dev VM: 4 vCPU, kernel
6.18, ext4 on virtio. Functional evidence only; no performance claim and
no R640 load.

Every run is listed in the order it happened. Headline evidence is the
run-2 series only: complete serial runs (`ctest -j1`) at one SHA. Failed
and stopped runs are kept and listed, never combined with reruns.

## Runs in order

| # | When (UTC) | SHA | Run | Result | Counted |
|---|---|---|---|---|---|
| 1 | before 06:21 | (pre-review fixes) | Release `ctest -j2` pre-check, `release-precheck-j2-stopped.log` | stopped by me: the code changed under it | no |
| 2 | before 06:21 | 70e5814 | negatives attempt 1, `negative/summary-attempt1-invalid.txt` | invalid: relative OUT path, logs deleted with the build dir, every rc=1 ambiguous | no |
| 3 | before 06:21 | 2635934 | negatives attempt 2, `negative/attempt2-diskfull/` | invalid: the first no-release mutation corrupted chunks and filled the disk (see its NOTE.txt) | no |
| 4 | before 06:21 | 72da67d | negatives attempt 3, `negative/summary.txt` | 7 of 7 caught | superseded by #9 (the test changed) |
| 5 | 06:21-06:47 | df836a2 | Release serial, `release-run1-j1.log` | 40/40 | no (series failed) |
| 6 | 06:47-08:21 | df836a2 | ASan+UBSan serial, `asan-run1-j1.log` | **FAILED**: hung in test_s3_writeback, killed at the 2 h task limit; TSan and legacy never started | no |
| 7 | 08:21-08:42 | 6b9c122 (fix) | test_s3_writeback alone under ASan, before and after the fix, `diag/` | before: T2 then T2b failed `writes_inflight_peak == W`; after: all passed, 0 reports | diagnostic |
| 8a | 08:42-09:09 | 6b9c122 | Release serial, `release-run2-j1.log` | **40/40** | **yes** |
| 8b | 09:09-09:38 | 6b9c122 | ASan+UBSan serial, `asan-run2-j1.log` | **40/40, 0 reports** | **yes** |
| 8c | 09:38-10:13 | 6b9c122 | TSan serial, `tsan-run2-j1.log` | **40/40, 0 reports** | **yes** |
| 8d | 10:13-10:39 | 6b9c122 | Release serial, `SNC_CKPT_IO=legacy`, `release-legacy-run2-j1.log` | **40/40** | **yes** |
| 9 | after 10:39 | 6b9c122 | negatives attempt 4, `negative/attempt4-after-T2-fix/` | **7 of 7 caught** | **yes** |

`runs.txt` is the raw run record. Sanitizer reports were counted with
`grep -cE "ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer"`.
In 8d, ctest prints a test's stderr only on failure, so the log does not
show the override; that it applies was checked separately
(`SNC_CKPT_IO=legacy ./test_persistence` prints
`snc: SNC_CKPT_IO=legacy overrides ckpt_io`).

## The failure in run 6

T2 released one held completion every 10 ms and assumed the walk fills a
chunk faster than that, so the window would reach W. Under ASan the walk
is slower, the peak stayed below W, and `CHECK_EQ(writes_inflight_peak, W)`
failed. The CHECK's `exit(1)` then ran with the engine still running and
writes in flight, hit the known teardown use-after-free
(`docs/S3_WRITEBACK.md` section 9, open item), and hung inside the ASan
report. Fix (commit 6b9c122, tests only, no engine change): T2 and T2b
release a completion only once the window gauge reads full or the walk has
ended; `test_s3_writeback` gets a ctest timeout so a hang fails the suite.

## Negative mutations (run 9)

Each mutation is applied to a `git archive` copy of 6b9c122, built in
Release, and must make the named test fail (`negative/run_negatives.sh`).

| Mutation | Test | rc | What caught it |
|---|---|---|---|
| no-window: the window never refuses | T1 | 139 | wait for a full window timed out |
| advisory-window: a refused chunk does not stop the walk | T1 | 139 | retained buffer bytes above the W envelope |
| no-release: completed buffers kept to the end | T2 | 1 | peak buffer bytes above the W envelope |
| fifo-free: the oldest busy slot is freed, not the completed one | T4 | 134 | the freed in-flight buffer produced a short write, storage_fatal |
| final-early: walk done before the last chunk is submitted | T5 | 139 | wait for the final-pending state timed out |
| short-accepted: a positive short write accepted | T6 | 1 | the child did not abort |
| sync-uncounted: synchronous fallback not counted | T10 | 1 | sync count != chunk count |

rc 139 is the CHECK's exit hitting the same teardown item after the
failure was printed; the CHECK line in each log is the cause.

## Independent review

A separate source review of the S3 diff (no blockers) and what was done
about each finding: `docs/S3_WRITEBACK.md` section 9.
