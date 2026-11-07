# S0 overhead micro-check and suite evidence (dev VM, 2026-11-07)

**Not a campaign benchmark. No performance conclusion is drawn from it.**
It exists to catch obvious telemetry perturbation
(docs/handoff/PR68_DesignR_Review.txt section 12). The R640 was not used.

**Host:** the cloud dev VM: 4 vCPUs, ext4 root disk (the WAL opened
`O_DIRECT`). It is shared and noisy.

**Builds:** both RelWithDebInfo, same flags:
- `main` = 9f3df47, the build before S0;
- the S0 build is this branch.

**Command:**
`bench/s0_overhead.py MAIN_BUILD S0_BUILD --reps 3 --seconds 10`
(2 shards, net_bench with 2 threads, 50% reads, 200k objects,
`--checkpoint-s 5`). The order of the arms alternates each rep.

| Arm | Median ops/s (ratio to main) | Median p99 |
|---|---|---|
| main | 427,780 (1.000) | 934 µs |
| s0-unread | 466,314 (1.090) | 770 µs |
| s0-rare (GAUGES 1/s) | 449,088 (1.050) | 852 µs |
| s0-hot (GAUGES + HEALTH every 10 ms) | 435,818 (1.019) | 885 µs |

Run-to-run spread within an arm is about ±7% (see `s0_overhead.json`).
That is larger than any difference between the arms. No arm is slower
than main beyond the noise, so the check found no obvious perturbation.
It cannot resolve effects smaller than a few percent.

## Suites

The `suite-*.log` files are summaries of the ctest output.

| Suite | First run | Rerun |
|---|---|---|
| RelWithDebInfo | 36/37 | `test_gauges` only, 2/2 |
| ASan+UBSan | 37/37 | none needed |
| TSan | 34/37, 0 ThreadSanitizer reports | the three failures, one at a time: 3/3 |

- **RelWithDebInfo:** the one failure was the new `test_gauges` ring
  test. With tiny rings it created its accounts in a window, and under
  load those creates were refused. It now creates them one at a time.
- **TSan:** the three failures were the HA timing tests
  `test_sync_readiness`, `test_baseline_retry` and `test_ready_order`.
  All three ran at the same time under `-j2`, with "a witness never saw
  the primary" and a restart-ordering timeout.
