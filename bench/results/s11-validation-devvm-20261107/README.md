# S1.1 validation, dev VM, 2026-11-07

Code SHA for the headline runs: `263fbcec0189a35855806bab0e72ab01573d663d`
(later commits on the branch change docs and evidence only). No R640 load.

## Every run, in order

| # | SHA | Suite | Result | Log |
|---|---|---|---|---|
| 1 | ffaa31d | Release, ctest -j1 | **stopped by operator** at test 25/38 (24 passed, 0 failed) to apply the independent-review fixes; not a headline | `stopped-ffaa31d/` |
| 2 | 263fbce | Release (RelWithDebInfo), ctest -j1 | 38/38 | `release-run1-j1.log` |
| 3 | 263fbce | ASan+UBSan, ctest -j1 | 38/38, 0 sanitizer reports | `asan-run1-j1.log` |
| 4 | 263fbce | TSan, ctest -j1 | 38/38, 0 sanitizer reports | `tsan-run1-j1.log` |

`runs.txt` has the start time and each return code. Sanitizer check:
`grep -E "ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer"`
matches nothing in runs 2-4. Every build was rebuilt from 263fbce before
run 2.

**Headline:** runs 2, 3 and 4, each a complete serial run, all clean.
`test_s1_admission` (A1-A15) and `test_baseline_ha` (h1_measurement,
q3_runtime, q3_restart, q4_normal) pass in all three.

## Negative checks (`negative/`, Release, targeted tests)

Each removes one S1.1 mechanism and shows the test that guards it fails.

| Log | Change | Fails |
|---|---|---|
| neg1-A10 | max_size cap at resize reverted | A10: the over-max child no longer aborts |
| neg1-A15 | same | A15: inbox grows past 1024 entries |
| neg1b-A15 | same, run to restart | recovery aborts: `invalid object image` (the pre-existing bug the cap fixes) |
| neg2-A13 | owner-side answer-wait admission removed | A13: 17 reads expected done, more held |
| neg3-A13 | pending budget removed | A13: refused_pending count wrong |

## q4 drain rule (`q4-rule-check/`, Release, at 263fbce)

- `h1_measurement.json`: sequential sum 286 vs a true total of 256; the
  old single-sample rule fails a drained series; the rule passes it and
  fails blocked, stalled, growing, one-shard-stalled and short series.
- `q4_normal.json`: drained, excess 0 in 48/48 samples, bounded.
- `q4_delayed10.json`: while blocked, 93 samples, median excess 256 in
  98.9% of them, not drained; after the grant, excess 0 in 47/47,
  drained, bounded.

## Independent review

One read-only source review: APPROVE, no blocking defect. Fixed before
runs 2-4: drain progress per shard (medium), A10 fork guard, fewer
required samples, Waiter size static_assert. Documented: pending_budget 0,
gauge index shift, h1 self-test scope. See `docs/S1_1_CORRECTIONS.md`
section 6.
