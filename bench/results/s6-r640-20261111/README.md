# S6 R640 checkpoint regression: result (2026-11-11), for independent review

**One session**, run as frozen under the GO record
[docs/handoff/S6_R640_GO_20261111.md](../../../docs/handoff/S6_R640_GO_20261111.md):

```
bench/s6.py bench/results/s6-r640-20261111 --scratch /dev/shm/snc-s6
```

- HEAD `e2aab62` (runner commit `63440b1`);
- 18:25:33 to 18:44:02 UTC; exit 0.

The runner wrote `preflight.json`, `postflight.json`, `summary.json`,
`verdict.json` and `raw/`, none of them edited. `console.log` is the
runner's console, kept outside the tree during the run and copied in
afterwards. This README is a reading of those files. Where it differs
from them, they are right.

## Verdict (`verdict.json`): **FAIL**

- **No VOID, no rerun:** 15 reps, all accepted.
- **Preflight:** no problems.
- **Postflight:** every machine and build check true.
- **Why FAIL:** every P rep fails **rule 4 only**: total p99.9 above
  10 ms. All six other rules pass in all three P reps, and the capacity
  guard holds.

| P rep | p99 (ref R = 638,976 ns; limit 958,464) | **p99.9 (limit 10,000,000)** | max | late_over_1ms / scheduled | refusals | emits_peak max | checkpoint | rules failed |
|---|---|---|---|---|---|---|---|---|
| 0 | 868,352 | **14,155,776** | 48.8 ms | 6,640 / 26,198,760 (0.025%) | 0 / 0 | 306 | 1.56 s | 4_p999 |
| 1 | 851,968 | **14,680,064** | 48.8 ms | 6,495 / 26,198,760 (0.025%) | 0 / 0 | 329 | 1.50 s | 4_p999 |
| 2 | 868,352 | **14,680,064** | 49.3 ms | 6,553 / 26,198,760 (0.025%) | 0 / 0 | 319 | 1.49 s | 4_p999 |

Reference R is the median of N's total p99: 655,360, 638,976 and 638,976
ns, so R = 638,976 ns. N: 0 refusals and 0 errors in all reps; p99.9
0.90-0.93 ms; max 4.5-8.6 ms.

**Capacity guard: holds.**
- Median C = 3,981,798 ops/s against median C0 = 4,016,161 ops/s: a ratio
  of 0.9914, at least 0.95.
- Rule 7 holds in all six C and C0 reps.

## Every rep (`summary.json`)

| Cell | rep | total ops/s | overloaded | idx refused | p99 ns | p99.9 ns | max ms | checkpoint s | client/server rc |
|---|---|---|---|---|---|---|---|---|---|
| N | 0 | 436,645 | 0 | 0 | 655,360 | 917,504 | 4.8 | none | 0/0 |
| P | 0 | 436,645 | 0 | 0 | 868,352 | 14,155,776 | 48.8 | 1.56 | 0/0 |
| H | 0 | 2,183,094 | 7,649 | 0 | 2,097,152 | 9,175,040 | 53.5 | 3.36 | 0/0 |
| C | 0 | 4,321,723 | 38,279,729 | 0 | 9,961,472 | 13,107,200 | 59.6 | 4.99 | 0/0 |
| C0 | 0 | 4,317,862 | 615 | 0 | 8,912,896 | 14,155,776 | 112.5 | 2.97 | 0/0 |
| N | 1 | 436,645 | 0 | 0 | 638,976 | 933,888 | 8.6 | none | 0/0 |
| P | 1 | 436,645 | 0 | 0 | 851,968 | 14,680,064 | 48.8 | 1.50 | 0/0 |
| H | 1 | 2,183,107 | 6,868 | 0 | 2,359,296 | 9,699,328 | 56.8 | 3.47 | 0/0 |
| C | 1 | 3,981,798 | 40,276,719 | 0 | 11,796,480 | 13,369,344 | 58.0 | 5.14 | 0/0 |
| C0 | 1 | 3,981,401 | 1,035 | 281 | 13,631,488 | 19,922,944 | 130.7 | 2.97 | 0/0 |
| N | 2 | 436,645 | 0 | 0 | 638,976 | 901,120 | 4.5 | none | 0/0 |
| P | 2 | 436,645 | 0 | 0 | 868,352 | 14,680,064 | 49.3 | 1.49 | 0/0 |
| H | 2 | 2,183,089 | 7,956 | 0 | 2,162,688 | 8,650,752 | 54.9 | 3.33 | 0/0 |
| C | 2 | 3,978,443 | 39,637,885 | 0 | 12,058,624 | 14,155,776 | 58.4 | 4.87 | 0/0 |
| C0 | 2 | 4,016,161 | 0 | 485 | 14,417,920 | 20,971,520 | 129.5 | 3.01 | 0/0 |

- In every P, H, C and C0 rep, exactly one checkpoint began inside the
  measured window. No window VOID.
- Errors, stale and index errors were 0 everywhere.
- All open-loop reps drained.
- No failure marker appeared in any server log.

## Controller evidence (reported, not judged)

The samples (100 ms) behaved as designed in every rep: 666-772 samples,
**0 failed, 0 skipped deadlines**, largest interval 100.08 ms.

| | P (3 reps) | H (3 reps) | C (3 reps) | C0 (3 reps) |
|---|---|---|---|---|
| WAL EWMA peak | 6.9-7.4 ms | 2.5-3.5 ms | 1.2-1.4 ms | 20.0-24.5 ms |
| WAL above high (worst shard) | 0.8-1.1 s | 0 | 0 | absent (no baseline gauge) |
| backlog > 25% / > 50% (worst shard) | 0 / 0 | 0.2-0.3 s / 0 | 59.9 s / 59.7-59.8 s | 1.3-1.7 s / 0.7-0.9 s |
| `emits_peak` max | 306-329 | 977-1,132 | 1,577-2,379 | 921-1,109 |
| catch-up (worst shard) | 0 | 0.1 s | 0.1 s | absent |
| walk budget range | 20-65 µs | 45-65 µs | 20-65 µs | absent |

Shard 0 phase times from the final GAUGES, rep 0:

| | snapshot | walk | drain | durable wait | sync | total | step max | `refused_followup` |
|---|---|---|---|---|---|---|---|---|
| P | 27.0 ms | 1,362 ms | 19.3 ms | 0 | 0.3 ms | 1,409 ms | 0.96 ms | 0 |
| C | 26.7 ms | 4,928 ms | 0.6 ms | 0 | 0.3 ms | 4,955 ms | 1.54 ms | 2,655,762 |
| C0 | 27.5 ms | 1,652 ms | 0.004 ms | 56.5 ms | 1,175 ms | 2,912 ms | absent | 0 |

## Observations for the review (hypotheses, not findings)

1. **The P failure is in the tail only.**
   - p99.9 is 14.2-14.7 ms and max 48.8-49.3 ms in all three P reps. N
     has p99.9 below 1 ms and max 4.5-8.6 ms.
   - Lateness is 0.025% of the scheduled ops, against a 0.1% limit.
   - The near-identical max in every P rep (about 49 ms) is close to the
     one-step begin snapshot, which DESIGN-R and S6_PREREG section 8
     record as unbounded by S2. Shard 0's snapshot gauge reads about
     27 ms; the other shards were not checked here.
   - The coincidence is not shown to be the cause. The P reps' raw data
     (`P-r*.client.log`; `samples.jsonl.gz` with phases at 100 ms) would
     be the place to test it.
2. **The final sync moved.** C0 (pre-S1) spends about 1.2 s in the final
   sync, with a WAL EWMA peak of 20-25 ms. Current builds (dsync window,
   S3/S4) sync in about 0.3 ms, with a WAL EWMA peak of 1-7 ms.
3. **C refuses heavily at closed-loop depth 512 while C0 barely does.**
   - C: 38-40 M EXEC refusals per rep (`refused_followup` 2.66 M on
     shard 0 alone; backlog above 50% for the whole window). C0: 0-1,035.
   - Completed throughput is still within 1% (the guard holds).
   - The capacity guard judges completed ops only, as preregistered.
     Whether this refusal pattern matters is for the review; it is not a
     rule.
4. **Reporting quirk.** For N (no checkpoint), the summary prints the
   walk-budget range as `[None, 0]`: its minimum skips zeros and its
   maximum does not. This is cosmetic, it touches no rule, and it is left
   as recorded.

## Not done

No rerun, no hand edit, no tuning, no CP-1A. The S6 result now goes to
independent review.
