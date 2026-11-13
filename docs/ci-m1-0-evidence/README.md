# M1-0 (PR #122) CI evidence

All runs are clean RelWithDebInfo builds of every target. Tests were not edited between runs except where noted.

| # | Where | Head | Command | Result | Time |
|---|---|---|---|---|---|
| 1 | GitHub hosted, ubuntu-24.04 (run 43369215185) | abb2a88 | `ctest -LE slow -j$(nproc)` | **FAIL** 39/40: `test_net_bench_rate` `scenario_stall_open`, `end_backlog 1` under CPU contention | 162 s |
| 2 | local 4-vCPU Ubuntu 24.04 container | abb2a88 | full suite, `ctest -j4` | **FAIL** 48/49: `test_baseline_retry` `q5_catchup_race`, `Address already in use` (log: `local-full-j4-abb2a88.log`) | 514 s |
| 3 | GitHub hosted (run 43369281597), deliberate regression branch `ci-red-check` (PR #123, closed) | e9cf975 | `ctest -LE slow -j$(nproc)` | **FAIL as intended** 34/40: the broken ObjectID separator is caught by `test_object_id`, `test_cluster`, `test_migrate`, `test_xnode_proc`, `test_lab_witness_guard`, `test_index_wire` | 138 s |
| 4 | GitHub hosted (run 43370027787) | c03e902 | `ctest -LE slow -j1` | **PASS** 40/40 | 358 s (job 7 min 15 s) |
| 5 | independent QA, local copy | abb2a88 binaries/tests (= c03e902) | `ctest -LE slow -j1` | **PASS** 40/40 | 285 s |
| 6 | GitHub hosted (run 43371057783) | 98588d1 | `ctest -LE slow -j1` | **PASS** 40/40 | job 6 min 51 s |
| 7 | local 4-vCPU container | 98588d1 | full suite, `ctest -j1` | **PASS** 49/49 (log: `local-full-j1-98588d1.log`) | 1,736 s (28.9 min) |

Runs 1 and 2 are why CI runs serially (`docs/CI.md`, "Why serial"). Run 3 shows the gate fails on a real
regression. The `full` job has not run on a hosted runner: it runs only on pushes to `main`, nightly and on
demand, so run 7 is its stand-in until the first post-merge run.

`test_pilot` before this PR: deterministically red (24 test functions; the 20th asserted `go_record is None`,
so 21 to 24 never ran). After: all 24 run and pass; mutations of the frozen state each fail it (QA report).

Independent QA: `QA_REPORT.md`.
