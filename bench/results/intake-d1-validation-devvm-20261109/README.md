# Follow-up intake D.1: validation on the dev VM (2026-11-09/10)

Dev VM numbers are regression checks only, not benchmark evidence. Every full
suite is a complete serial run (`ctest -j1`) in its own build directory. Every
attempt is listed in the order it ran, failed and invalid ones included;
`runs.txt` is the raw log written by the runner script.

## Headline

| Suite | Commit | Result |
|---|---|---|
| Release, run 4 | b2b31bb | 45 of 45 passed |
| ASan + UBSan, run 1 | bc31309 | 45 of 45 passed, 0 sanitizer reports |
| TSan, run 3 | b098667 | 45 of 45 passed, 0 ThreadSanitizer warnings |
| Negatives, attempt 3 | 9706752 | 12 of 12 mutations caught |

Engine code (src/, include/, tools/) is identical from b2b31bb to b098667. The
only later changes are test-only: tests/test_intake.cpp (F11 snapshot order,
fail-fast CHECK) and its ctest timeout in CMakeLists.txt. After those changes,
test_intake passed 3 of 3 standalone on Release, and 3 of 3 under TSan with 0
warnings (`tsan-test_intake-fix-rerun1.log` is the first of those). It passed
again inside TSan runs 2 and 3.

## Every run, in order

1. **Smoke at dc6b693**, run concurrently, not headline (`smoke-dc6b693-concurrent.log`).
2. **Negatives.** `negative/run_negatives.sh` applies one mutation per build and
   expects the named test to fail.
   - Attempt 1: 10 of 10 caught.
   - Attempt 2: 11 of 12. N7b (staging order only) was not caught by F1, so F17
     was added.
   - Attempt 3 at 9706752: 12 of 12 caught (`negative/attempt3/summary.txt`).
3. **Release run 1 (ea9d3b8)**: 44 of 45. Only test_baseline_retry b3_stalled_catchup failed.
4. **Release run 2 (ea9d3b8)**: 44 of 45, the same failure.
   - Cause of runs 1 and 2: the session's disk allowance was nearly used up.
     b3 failed the same way on main af5afeb, 3 of 3 (`b3-compare/`). After about
     8 GB was freed, b3 passed 2 of 2 on D.1.
   - Invalid environment. Both runs are kept.
5. **Release run 3 (b2b31bb)**: 44 of 45. test_baseline_retry q5_kill_kill failed
   at harness start-up: witness w2 never printed "primary up".
   - Re-run alone, it passed 4 of 4 (`q5-rerun/`).
   - Cause: tools/snc_witness.cpp prints "primary up" only when the witness's own
     proposal learns the space. A witness that sees the space chosen first prints
     nothing. This is a logging race in code D.1 does not touch, proposed as a
     separate fix.
   - Kept.
6. **Release run 4 (b2b31bb)**: 45 of 45. Headline.
7. **ASan + UBSan run 1 (bc31309)**: 45 of 45, with 0 AddressSanitizer, UBSan or
   LeakSanitizer reports. Headline.
8. **TSan run 1 (bc31309)**: 44 of 45, 0 ThreadSanitizer warnings.
   - test_intake F11 failed `x.emits_in_flight <= d.exchanges_live`. This was a
     test defect: the two shards' stats were read x first, so d could end
     exchanges between the reads. Both counts only fall in that loop, so reading
     d first is sound (fixed in cf0d51f).
   - The failed CHECK then called exit() while engine threads were still
     running. Static destructors raced a shard, the shard crashed, and TSan
     deadlocked in its own report path. The test hung for 4,490 s until it was
     killed by PID.
   - test_intake now uses _Exit on a failed check and has the 2,400 s ctest
     timeout the S3 tests use.
   - Tests 27 to 45 overlapped the standalone reruns. Kept; not headline.
9. **TSan run 2 (cf0d51f)**: the container restarted during test 36. Tests 1 to
   35 passed with 0 warnings. Incomplete; kept.
10. **TSan run 3 (b098667)**: 45 of 45, 0 ThreadSanitizer warnings. Headline.

## Not run here

N3, N5, N11, N12 and P1, as listed in docs/FOLLOWUP_INTAKE_DESIGN.md section
3.9.6.

Cross-node sender bounds are D.2.
