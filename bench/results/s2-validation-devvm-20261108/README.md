# S2 validation evidence (dev VM, 2026-11-08)

Slice S2 (checkpoint walk pacing). Dev VM only: no R640, no CP-1A, no
comparators. Every run made for this slice is listed below, in order.
Nothing is combined: a headline is one complete serial suite.

## Runs, in order

| # | What | Code | Result | File |
|---|------|------|--------|------|
| 1 | Release pre-check, `ctest -j2` (not headline) | before review fixes | 39/39 | release-precheck-j2.log |
| 2 | Walk cost bench (`bench/ckpt_walk_cost.cpp`), 1M IDs, 1 shard | ac8c580 | see below | walk-cost.txt |
| 3 | Negative checks: 4 mutations of the pacer/walk, each must fail a test | base ac8c580 | 6/6 failed as required | negative/ |
| 4 | **Headline Release**, `ctest -j1` | 7c143fd | **39/39** | release-run1-j1.log |
| 5 | **Headline ASan+UBSan**, `ctest -j1` | 7c143fd | **39/39, 0 reports** | asan-run1-j1.log |
| 6 | **Headline TSan**, `ctest -j1` | 7c143fd | **39/39, 0 reports** | tsan-run1-j1.log |

Runs 4-6 were one script, one after another (runs.txt: start 03:27:52Z,
Release done 03:53:49, ASan 04:21:02, TSan 04:52:42). Sanitizer reports were
checked with `grep -E "ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer"`:
0 matches in each log. Code is unchanged between ac8c580 and 7c143fd
(7c143fd adds evidence files only).

## Negative checks (run 3)

| Mutation | Test | rc | Meaning |
|----------|------|----|---------|
| no floor (budget may reach 0) | P2 | 124 (timeout) | walk stalls; caught |
| budget ignored (walk unbounded per step) | P2 | 1 | bounded-step check fails |
| budget ignored | P3 | 124 (timeout) | foreground starves; caught |
| no catch-up | P1 | 1 | controller trace fails |
| no catch-up | P6 | 139 | CHECK failed, then crash on exit |
| no back-off under HIGH pressure | P1 | 1 | controller trace fails |

## Walk cost (run 2)

About 0.6-0.8 us per ID on this VM in every mode. Pacing off (256-ID step)
is about 150-200 us per step. Step max outliers up to 40 ms are VM preemption
(they appear in every mode, including off). The pacing defaults in
docs/S2_PACING.md section 5 are derived from these numbers and are
hypotheses, not frozen values.
