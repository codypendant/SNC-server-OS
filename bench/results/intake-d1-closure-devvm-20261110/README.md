# D.1 closure: validation on the dev VM (2026-11-10)

The closure PR answers the PR #87 review (`docs/handoff/PR87_D1_Review.txt`).
Dev VM numbers are regression checks only. Every full suite is a complete
serial run (`ctest -j1`) in its own build directory, and every attempt is
listed in order. `runs.txt` is the raw log, including the number of
/tmp/snc-* directories before and after each suite.

## Headline (all at d3495c9, the final code head)

| Suite | Result |
|---|---|
| Release, run 2 | 45 of 45 passed |
| ASan + UBSan, run 2 | 45 of 45 passed, 0 AddressSanitizer, UBSan or LeakSanitizer reports |
| TSan, run 2 | 45 of 45 passed, 0 ThreadSanitizer warnings |
| P1 matrix (at 9f6fac6) | 110 of 110 runs valid; `p1/REPORT.md` |

No suite left a /tmp/snc-* directory behind. The count was unchanged
across every complete suite; the two directories added between runs came
from the TSan run that a container restart cut off.

## Every run, in order

1. **P1 matrix (9f6fac6)**, preregistered in `p1/PREREG.md` before the run.
2. **Release run 1 (c73526d)**: 45 of 45.
3. **ASan + UBSan run 1 (c73526d)**: 45 of 45, 0 reports.
4. **TSan run 1 (c73526d)**: failed, then was interrupted.
   - test_intake failed `CHECK(g.consistent)`. This was a test defect.
     The new fresh_gauges helper read the gauge seqlock once, but the
     shard republishes every 50 us and a read gives up after 64 tries
     that overlap a publish, which is likely under TSan. Fixed in d3495c9:
     it now reads until it gets a whole snapshot.
   - The container then restarted during test 28. Tests 1 to 27 had no
     other failure, with 0 warnings.
   - After the fix, test_intake passed 5 of 5 standalone under TSan
     (`tsan-test_intake-fix-rerun1.log` is the first). Kept; not headline.
5. **TSan run 2 (d3495c9)**: 45 of 45, 0 warnings.
6. **Release run 2 (d3495c9)**: 45 of 45.
7. **ASan + UBSan run 2 (d3495c9)**: 45 of 45, 0 reports.

Between c73526d and d3495c9 only that test helper changed.

## Witness regression (test_health T12)

T12 is deterministic. With the fix stashed, the old witness failed it
3 of 3 times ("w2 never printed primary up"). With the fix, it passed
3 of 3, and again inside every suite above.
