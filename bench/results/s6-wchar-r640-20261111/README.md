# S6 write-window causal characterization: result (2026-11-11), for review

Diagnostic only ([PREREG.md](PREREG.md), committed before data at
`c13153f`).
- **Not an S6 run.** S6 remains **FAIL (rule 4 p99.9)**.
- No product code changed. No W value is selected here.

## Run

```
wchar.py run bench/results/s6-wchar-r640-20261111/run --scratch /dev/shm/snc-s6w
```

- HEAD `c13153f`; 18:58:34 to 19:06:23 UTC; exit 0.
- **Preflight:** no problems (machine, storage, binaries, isolation, clean
  tree, as S6's). **Postflight:** every check true.
- 6 reps (W1, W2 rep-major × 3). No VOID, no rerun. Every rep proved a
  checkpoint began inside its window.
- **Sampler:** 767-772 samples per rep, 0 failed, 0 skipped, largest
  interval 100.07 ms.
- **In every rep:** 0 refusals, 0 errors, stale or index errors; drained;
  client and server rc 0; lateness 0.025%.
- `run/` holds the runner's files, unedited. `table.md` is
  `wchar.py table run ../s6-r640-20261111`; W4 rows are the frozen S6 P
  reps through the same extraction.

## Result (`table.md`; ms unless marked; p99.9 values are net_bench histogram bucket edges)

| | W4 (S6 P, 3 reps) | W2 (3 reps) | W1 (3 reps) | N (S6, no checkpoint) |
|---|---|---|---|---|
| **chunk write EWMA**, per shard | 9.7-35.0 | 6.2-16.2 | **3.2-7.8** | n/a |
| **chunk write max**, per shard | 30.1-52.9 | 22.0-31.3 | **11.1-16.3** | n/a |
| **WAL EWMA peak** | 6.9-7.4 | 6.5-7.0 | **4.4-4.6** | 0.44-0.84 |
| **time above WAL-high** (worst shard) | 0.8-1.1 s | 0.6-0.8 s | **0** | 0 |
| **deposit / transfer p99.9** | 17.3-17.8 | 16.8-17.3 | **12.1-12.6** | 1.15-1.18 |
| total p99.9 (S6 rule 4 limit 10.0) | 14.16-14.68 | 13.89-14.42 | 9.70 (all 3) | 0.90-0.93 |
| total p99 | 0.85-0.87 | 0.87-0.88 | 0.90 | 0.64-0.66 |
| read p99.9 | 2.9-3.4 | 3.0-3.7 | 2.7-3.0 | about 0.34 |
| index p99.9 | 6.4-7.6 | 6.6-7.7 | 5.9-6.4 | about 0.8 |
| max | 48.8-49.3 | 48.9-49.1 | 49.0-49.3 | 4.5-8.6 |
| window-full steps, per shard | 0-46,310 | 10,205-58,042 | 25,593-76,344 | n/a |
| checkpoint total (shard 0) / completion line | 1.41-1.45 s / 1.49-1.56 s | 1.44-1.45 s / 1.51-1.53 s | 1.47-1.56 s / 1.55-1.59 s | none |
| begin snapshot (shard 0) | 27.0 | 26.9-27.0 | 26.8-27.1 | none |
| chunks per shard | 153 | 153 | 153 | n/a |

## Against the frozen interpretation (PREREG section 5)

These are stated for the review; the review decides.

**W1 against W4: coherent reduction in all three named metrics:**
1. chunk write latency: EWMA about 3-4× lower, max about 3× lower;
2. WAL EWMA peak 4.4-4.6 against 6.9-7.4 ms; time above WAL-high 0
   against 0.8-1.1 s;
3. mutation p99.9 12.1-12.6 against 17.3-17.8 ms.

Checkpoint completion stays at about 1.5 s, far inside 60 s. No catch-up
pathology: one rep had 0.1 s of catch-up.

**W2 against W4: write latency is about halved, but the tail barely moves:**
- WAL EWMA peak (6.5-7.0) and time above WAL-high (0.6-0.8 s) are within
  or near W4's range;
- mutation p99.9 (16.8-17.3) and total p99.9 (13.9-14.4) are near the W4
  failure.

**Ordering W1 ≤ W2 ≤ W4:** it holds for write latency, WAL EWMA peak, time
above WAL-high and mutation p99.9. For WAL pressure and the tail, W2 sits
close to W4, and the step from W2 to W1 is where most of the change
happens.

**W1 does not reach the no-checkpoint level:**
- mutation p99.9 is 12.1-12.6 ms against N's about 1.2 ms, and WAL EWMA
  peak 4.4 against 0.4-0.8 ms;
- read and index p99.9 change little across W1, W2 and W4;
- max is about 49 ms in every cell, window included.

So the window explains part of the mutation tail, not all of it.

**W1's total p99.9 of 9.70 ms is under S6's 10 ms limit**, at one
histogram bucket edge, in all three reps. This is a diagnostic
observation, not an S6 result: no S6 rule is evaluated here, the
comparison is across sessions (PREREG section 2 confound), and choosing W
is the review's decision.

## Other observations (not causal claims)

- **Max about 49 ms in every P-shaped rep, at every window.** The
  per-shard `wal_flush_max_us` reads about 46-48 ms in **every** S6 cell,
  N included, where the client max is 4.5-8.6 ms. That gauge is a
  high-water mark since server start, so it includes the load phase. It
  therefore does not explain P's max and is not offered as a cause. The
  about-49 ms max stays unexplained by this characterization.
- **Window-full steps rise as W falls**, as expected: the walk waits on
  the window more. The walk budget ranges and the checkpoint durations
  stay within the approved envelope.

## Files

| File | What |
|---|---|
| `PREREG.md`, `wchar.py`, `commands.txt` | before data, commit `c13153f` |
| `run/` | runner output (preflight, postflight, summary, `char.json`, `raw/` with server and client logs, 100 ms samples, disk rows, final GAUGES and HEALTH); `console.log` copied in after the run |
| `table.md` | per-rep W1 / W2 / W4 table |
