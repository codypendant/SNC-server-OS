# CP-1A dev-VM checks (PR #61 review, item J)

Correctness only, on the 4-vCPU development VM. **Not performance
evidence.**

| Log | Build | Code | Result |
|---|---|---|---|
| `ctest-release-draft-141213c.log` | RelWithDebInfo | the R640 draft (141213c) | 33/33 |
| `ctest-relwithdebinfo.log` | RelWithDebInfo | f207aab | 34/35: `test_s1` failed in setup (below) |
| `ctest-build-asan.log` | Debug, `-fsanitize=address,undefined` | f207aab | 35/35 |
| `ctest-build-tsan.log` | Debug, `-fsanitize=thread` | f207aab | 35/35 |
| `ctest-relwithdebinfo-rerun.log` | RelWithDebInfo | f207aab + the witness wait below | 35/35 |
| `final/ctest-relwithdebinfo.log` | RelWithDebInfo | 2fc46f6 (review C1-C3) | 35/35 |
| `final/ctest-build-asan.log` | Debug, ASan+UBSan | 2fc46f6 | 35/35 |
| `final/ctest-build-tsan.log` | Debug, TSan | 2fc46f6 | 35/35 |

The `test_s1` failure was "NO no control member answered" from
`snc_ctl init`, 0.37 s into the test, before any SNC operation.
- It passed 3 of 3 times when rerun alone on the same binaries, and
  passed in both sanitizer builds.
- **Cause (inferred, not observed):** `tests/test_fail_closed.py`
  started the witnesses and slept a fixed 0.3 s before `init`, which can
  lose the race on a loaded host. The failed run's witness logs were
  deleted with its work directory, so this is not confirmed.
- **Fix:** the harness now waits until every witness port accepts a
  connection, with a 15 s limit, before `init`. `test_s1` and
  `test_fail_closed` then passed, and the full Release rerun passed
  35/35.
- No product code changed.
