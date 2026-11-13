# Continuous integration (M1-0)

Minimal automated regression protection, established before the M1 correctness fixes
(`docs/handoff/PR121_Audit_Final_Approval_PR122_M1-0.md`; audit section 9,
`docs/ARCHITECTURE_EVIDENCE_AUDIT_20261113.md`).

## What runs, and when

The workflow is `.github/workflows/ci.yml`, on GitHub-hosted `ubuntu-24.04` runners.

| Job | Triggers | Runs | Time limit |
|---|---|---|---|
| `fast` (build + fast tests) | every pull request (any base), every push to `main`, nightly, on demand | clean RelWithDebInfo build of every target, then `ctest -LE slow` | 30 min |
| `full` (build + full suite) | pushes to `main`, nightly (07:17 UTC), on demand (`workflow_dispatch`) | clean build, then the whole `ctest` suite | 90 min |

Both jobs install only the three documented dependencies (`libmimalloc-dev libxxhash-dev
liburing-dev`), check out full git history (`tests/test_pilot.py` verifies the CP-1A freeze
against it), print the output of any failing test (`--output-on-failure`), and upload the
JUnit file and `LastTest.log` as an artifact (`ctest-fast`, `ctest-full`) even when tests fail.

A red job is the signal. No job is allowed to fail ("continue-on-error" is not used), no test is
skipped or retried, and no test is excluded from the full suite.

## The two tiers

Tests are tiered by a ctest label in `CMakeLists.txt`, not by a separate list:

- **slow** (label `slow`): `test_xnode`, `test_health`, `test_fail_closed`, `test_s1`,
  `test_baseline_retry`, `test_sync_readiness`, `test_baseline_move`, `test_baseline_ha`,
  `test_ready_order`. Each takes about a minute or more serially (QA timings,
  `docs/audit-20261113-working-notes/qa/ctest_serial.log`).
- **fast**: every other registered test, including every C++ unit test other than `test_xnode`
  (object model, pages, runtime, execute, persistence, crash recovery, transfers, replication,
  cluster, migration, admission, pacing, writeback, lifecycle, sync tokens, WAL capacity, intake)
  and the quick Python tests (`test_pilot`, `test_s6`, `test_profile_check`, `test_ckpt_cli`, ...).

A new test is fast by default; label it `slow` only if it takes about a minute or more serially.

Run locally:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build -LE slow -j4 --output-on-failure   # what every PR runs
ctest --test-dir build -j4 --output-on-failure            # the full suite
```

## What CI does not run (by design)

These stay on the R640 or a dedicated host, run by hand under their own preregistration:

- benchmarks, pilots and campaigns (`bench/pilot.py`, `bench/s6.py`, the A2-a drivers), which need
  the R640's pinned CPUs, NUMA layout, governors, data SSD and manifest;
- anything needing root, sudoers, `perf`, `fstrim` or a specific block device;
- the long scenarios run by name only (for example `test_baseline_retry` q1_blocked, q4_delayed*,
  q3_runtime20; `test_xnode_proc` x9 with `--old`).

## Runner requirements

Hosted runners are enough for both tiers today (verified in PR #122): Linux with io_uring
available to unprivileged processes, `RWF_DSYNC`, loopback TCP on the fixed test port ranges, a
writable `/tmp` and `/dev/shm`, 4 vCPUs and Python 3. If a hosted runner stops meeting this (for
example io_uring blocked by the runner's seccomp policy), move the affected job to a self-hosted
Linux runner with the same packages; do not mark the tests skipped.

## Not yet in CI (follow-up slices, not authorized here)

- ASan/UBSan and TSAN builds (README section "Sanitizer builds"; the audit's M1-0 bullet). None is
  automated, and no TSAN run is on record.
- A required-status branch protection rule on `main` (a repository setting, owned by the
  repository owner).
