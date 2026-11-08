# S1.2 validation: dev VM, 2026-11-07/08

S1.2 bounds replay waiters (docs/S1_2_REPLAY.md). All runs were on the dev VM. No R640 load was run.
Each suite is a complete serial run (`ctest -j1`). Every run is listed in the order it ran. No run is combined with another.

## Headline: run 2 at a3d414309971709239084b4e4b891544fb5489e7

| Suite | Result | Sanitizer reports | Log |
|---|---|---|---|
| RelWithDebInfo | 38/38 | n/a | release-run2-j1.log |
| ASan+UBSan | 38/38 | 0 | asan-run2-j1.log |
| TSan | 38/38 | 0 | tsan-run2-j1.log |

Reports were counted with `grep -E "ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer"`.
The start time and each suite's exit code are in runs.txt.

## Run 1 at 8b98a90 (not headline)

| Suite | Result | Log |
|---|---|---|
| RelWithDebInfo | 37/38: test_baseline_q4_normal failed | release-run1-j1.log |
| ASan+UBSan | 38/38, 0 reports | asan-run1-j1.log |
| TSan | 38/38, 0 reports | tsan-run1-j1.log |

The q4 failure was in the harness, not the engine. The S1.1 drain rule had a trend clause on undurable LSNs, and that clause fails healthy runs.
- q4-diagnosis-old-rule/: five reruns under the old rule. Three of them failed on `undurable_bounded`.
- q4-rule-recheck/: the corrected rule passed q4_normal 6 times out of 6. q4_delayed and the h1 self-test also passed.

The corrected rule is a window bound plus slack of 16, described in docs/S1_2_REPLAY.md section 9. Because a3d4143 changed the harness, run 1 is kept here for the record but is not the headline.

## Negative checks (negative/)

- `neg-replay-exempt-A16/A18/A20/A21.log`: replay waiters were made exempt from admission again, which is the pre-S1.2 behaviour. A16, A18, A20 and A21 each fail.
- `neg-client-no-retry-A21.log`: net::Client was made to deliver a flagged refused replay instead of resending it. A21 fails.
