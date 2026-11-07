# S0.1 suite validation (dev VM, 2026-11-07)

Required by `docs/handoff/PR69_S0_Review.txt` section 12 after the
seqlock memory-order fix in `include/snc/gauges.h`. The R640 was not used.

**Host:** the cloud dev VM (4 vCPUs, ext4 root disk, shared and noisy).
Before each run: no stray `snc_*`, `test_*` or `net_bench` processes, and
the root disk at 60% used.

**Code:**
- Release run 1 used 60d51b7 (the fix).
- Every other run used 1be6d4a, the final code. 1be6d4a only rewords the
  proof comment in `gauges.h`; there is no non-comment change.

## Headline: the clean final validation runs

Each one is a single complete ctest run of all 37 tests on 1be6d4a.

| Req. | Build | Command | Result | Sanitizer reports | Log |
|---|---|---|---|---|---|
| A | RelWithDebInfo (`build/`) | `ctest -j1` | **37/37** | n/a | `release-run2-j1.log` |
| B | ASan+UBSan (`build-asan/`) | `SNC_SANITIZED=1 ctest -j1` | **37/37** | **0** | `asan-run2-j1.log` |
| C | TSan (`build-tsan/`) | `SNC_SANITIZED=1 ctest -j1` (serial from the start) | **37/37** | **0** | `tsan-j1.log` |
| D | TSan `test_gauges` alone | `SNC_SANITIZED=1 build-tsan/test_gauges` | pass; T7: 2,521 consistent snapshots, 0 torn | **0** | `tsan-test_gauges.log` |

The TSan suite was run fully serially from the start (the review's
second option), not as a parallel run followed by reruns.

## Every run, in order (raw history)

| # | Run | Result |
|---|---|---|
| 1 | Release, `ctest -j2`, 60d51b7 | 36/37: `test_lab_witness_guard` failed |
| 2 | Release, `ctest -j1`, 1be6d4a | 37/37 (headline A) |
| 3 | ASan+UBSan, `ctest -j1`, 1be6d4a | 36/37, 0 sanitizer reports: `test_ready_order` failed |
| 4 | TSan, `ctest -j1`, 1be6d4a | 37/37, 0 ThreadSanitizer reports (headline C) |
| 5 | ASan+UBSan, `ctest -j1`, 1be6d4a | 37/37, 0 sanitizer reports (headline B) |
| 6 | TSan `test_gauges` alone, 1be6d4a | pass (headline D) |

No targeted reruns were used. Runs 2, 4 and 5 are complete suites.

## The two failed runs

Neither failure touches the gauges. Both come from pre-existing test
harness races, traced as follows. Neither is fixed here, because S0.1 is
limited to the seqlock fix and the evidence. Proposed patches follow.
*Both were fixed later, after the PR #70 review: see
`../harness-races-devvm-20261107/`.*

**Run 1: `test_lab_witness_guard`, case `clean`, `KeyError: None`.**
- Scenario `p3` calls `lab.primary(0)` once, 2.7 s into the load
  (`bench/lab/lab.py`, `p3`).
- `primary()` asks each node for `STATUS` with a 1 s timeout and returns
  `None` when no answer names a primary. `lab.nodes[None]` then raises.
- STATUS is a per-shard-task verb and was not changed by S0 or S0.1.
  Under `-j2` the other HA tests ran at the same time on 4 vCPUs, and no
  STATUS answered within 1 s.
- Proposed patch: in `p3`, wait for a primary instead of sampling once:
  `k = wait(lambda: lab.primary(0), "primary known", timeout=30)`.

**Run 3: `test_ready_order`, case `c2_restart_held`, `grounded: false`.**
- The witnesses already held READY (`ready_again` = `[7, 0, 0, false]`),
  but the test read the primary's output one instant too early and found
  no `READY after v=` line (`new_ready_line` = "").
- The witnesses commit READY before their reply reaches the primary, and
  only then does `tools/snc_server.cpp` print
  `witness: standby back in sync (READY after v=N)`.
- `c2_restart_held` (`tests/test_ready_order.py`) looks for that line
  once, right after `wait_ready` sees READY in the witnesses. Under ASan
  the gap is wider. The run's other checks held: the old READY was
  refused, the failover was exact, with 0 mismatches over 2,457,881
  deposits.
- Proposed patch: look for the line with
  `wait_until(lambda: next(...), 10)` instead of one `next(...)`.
