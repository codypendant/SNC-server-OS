# S1 admission validation (dev VM, 2026-11-07)

Required by `docs/handoff/PR71_Harness_Review.txt` section 14. The R640 was
not used.

**Host:** the cloud dev VM (4 vCPUs, ext4 root disk, shared and noisy).
Before the runs: no stray `snc_*`, `test_*` or `net_bench` processes, the
root disk at 61% used.

**Code:** every run used binaries built from 7d687e4. The later commits
on the branch (a25825a, b20a9e0) change docs and one comment only.

## Headline: clean complete runs

| Req. | Build | Command | Result | Sanitizer reports | Log |
|---|---|---|---|---|---|
| A | RelWithDebInfo (`build/`) | `ctest -j1` | **38/38** | n/a | `release-run1-j1.log` |
| B | ASan+UBSan (`build-asan/`) | `SNC_SANITIZED=1 ctest -j1` | **38/38** | **0** | `asan-run2-j1.log` |
| C | TSan (`build-tsan/`) | `SNC_SANITIZED=1 ctest -j1` | **38/38** | **0** | `tsan-run1-j1.log` |

`test_s1_admission` (A1-A9) passed in all four suite runs. Debug-build
`check_reserved` ("handler contract violated") fired in none.

## Every run, in order

| # | Run | Result |
|---|---|---|
| 1 | Release, `ctest -j1` | 38/38 (headline A) |
| 2 | ASan+UBSan, `ctest -j1` | **37/38**, 0 sanitizer reports: `test_baseline_ha` scenario `q4_normal` failed |
| 3 | TSan, `ctest -j1` | 38/38, 0 reports (headline C) |
| 4 | ASan+UBSan, `test_baseline_ha.py q4_normal` alone, ×3 | 3/3 pass (`q4-investigation/`) |
| 5 | ASan+UBSan, `ctest -j1` | 38/38, 0 reports (headline B) |

Headline B is run 5 alone. It is not combined with run 4.

## The failed run (run 2)

- **Which check failed:** `waiters_drained` and `undurable_drained`.
  - Everything else passed: the grant came at 3.6 s, there was no
    overflow, and retention was capped.
  - Both checks read one sample, the last one, taken while the load is
    still running, and require it to be below the clients' window: 4
    threads × depth 64 = 256.
  - That sample read 270 waiting answers and 270 undurable records.
- **What 270 means:**
  - It is the full client window, all waiting on one slow local WAL
    sync at that instant.
  - It is above 256 because RESOURCES adds up `eng.stats(s)` one shard
    at a time (`tools/snc_server.cpp`). An answer counted on one shard
    can be replaced by a new op counted on the next, so the sum is not a
    snapshot.
- **Why S1 is not implicated:**
  - The load is durable deposits routed to the owner.
  - S1 changes no WAL flush, sync or loop order. Its budgets (32 MiB of
    waiters, 30 MiB of WAL buffer) are far above 256 answers.
  - The suite's TSan and Release runs passed this scenario, and so did
    the three isolated ASan reruns, whose last-5-second maximum was
    155-172.
- **Not dismissed as a flake.** The mechanism is that the check depends
  on a single end sample under a sanitizer-slowed sync.
- **Proposed harness change, for review (not made here):** judge "drained"
  on the median of the last 5 s, not the last sample, and read RESOURCES
  as one snapshot.
