# P1 report: follow-up intake D.1 regression matrix (dev VM, 2026-11-10)

This applies the rules of `PREREG.md`, which was committed before the
run, at 9f6fac6.

- **Build and run:** a Release build at 9f6fac6. The run took 16 minutes
  (02:18 to 02:34 UTC): 5 reps × 22 cells × 8 s, 2 shards, durable WAL.
- **Validity:** all 110 runs are valid (rc 0, errors 0). No run was
  dropped or re-run.
- **Raw data:** `runs.json` and `runs.log`; the table is `summary.txt`.
- **Scope:** a regression check on a 4-vCPU VM. No conclusion about
  target hardware, and no default changes.

## Overhead (ratio of the ops/s median to rollback's median)

**Uniform** (rollback's range over reps is 147,886 to 157,671 ops/s):
- **Not distinguishable from rollback:** w16-x1024 0.977, w64-x1024
  (the default) 0.971, w64-x4096 0.991, w256-x1024 0.982, w256-x4096
  0.976, and no-window 0.980. Each median lies within rollback's own
  range.
- **Below rollback's minimum:**
  - every X = 256 cell: w16 0.948, w64 0.950, w256 0.948, so about 5%;
  - w16-x4096: 0.957, about 4%.

  The X = 256 cells also turn away about 0.04% of ops.

**Cross**, where every transfer is an exchange (rollback's range is
132,026 to 145,183 ops/s):
- Every cell's median lies within rollback's range, so none is
  distinguishable from rollback. The ratios run from 0.988 to 1.053.
- The medians at X ≥ 1024 are 3 to 5% above rollback's median, but still
  inside its range. Under the preregistered rule this is noise, and no
  gain is claimed.

## What the gates refuse (median turned-away share of ops; rollback is 0)

| Cell | uniform | cross |
|---|---|---|
| X = 256 (any W) | 0.03-0.04% | 3.9-5.0% |
| w64-x1024 (default) | 0 | 0.15% |
| w256-x1024 | 0 | 1.55% |
| no-window (W = 0), X = 1024 | 0 | 1.28% |
| X = 4096 (any W) | 0 | 0 |

These refusals are the budget X working: an exchange-producing transfer
is refused (E_OVERLOADED) while its shard has X exchanges unfinished. A
refused op is not counted in ops/s, so the cross cells at X = 256 do less
work than their throughput ratio suggests.

At X = 1024, a larger window refuses more: W = 64 refuses 0.15%, W = 256
refuses 1.55%, and no window refuses 1.28%. With a small window, items
wait at the sender. With a large one, they wait in the receiver's
staging, and the exchanges stay live longer, so they hit X sooner. This
is an observation only, and nothing is tuned from it.

## p99 latency

The medians run from 819 to 983 us across all cells. Rollback is 836 us
(uniform) and 950 us (cross). No cell shows a p99 regression beyond about
150 us, and the cross X = 256 cells (918 to 983 us) sit within the run's
spread.

## Not concluded

- Nothing here sets W, X, H or L. Those stay hypotheses for the R640
  preregistration.
- The dev VM's spread (about 7% of the median within one arm) is larger
  than most of the differences. A 4 to 5% overhead at X = 256 (uniform)
  is the only effect outside it.
