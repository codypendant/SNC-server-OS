# CP-1A Profile-A objective-rate duration validation: result (2026-11-12), for independent review

**One session**, run as preregistered in [PREREG.md](PREREG.md) (commit `a5d082b`, pushed before data),
under [docs/handoff/PR111_CP1A_Result_Final_Review.md](../../../docs/handoff/PR111_CP1A_Result_Final_Review.md):

```
python3 -B bench/results/cp1a-a-duration-r640-20261112/aduration.py run \
    /home/solo/cp1a-a-duration-r640-20261112 --scratch /dev/shm/snc-aduration \
    --manifest bench/manifests/r640-main-20261111.txt --build build-s6
```

- HEAD `a5d082b` (the preregistration commit), tree clean.
- 01:59:54 to 02:23:33 UTC; no abort; 9 of 9 runs in the preregistered order.

`out/pilot.json` and `out/raw/` (18 logs) are the driver's output, written outside the tree and copied in
unchanged (`cmp` and `diff -r` identical; `pilot.json` SHA-256 `acbcc7d7…`). `out/console.log` is the
console. `aduration.py judge out` on the copy reproduces the verdict. This README is a reading of those
files. Where it differs from them, they are right. Nothing was rerun, tuned or replaced.

## Preflight (`out/pilot.json` `environment`)

- CP-1A freeze gate: no problems.
- `git_status` empty.
- `snc_server` `69a51b69…`, `net_bench` `9fc54944…`, plan `568e9874…` and manifest e `768056bb…`: the
  CP-1A record.
- Governors `performance`, `numa_balancing` 0, RelWithDebInfo.
- Isolation problems: none.
- Data root and scratch were removed afterwards; no SNC process left running.

## Verdict: **PASS**

All 9 reps are sustained by `pilot.sustained()`, and both drift bounds hold. They are 0 errors, 0 EXEC
refusals, 0 INDEX refusals, drained, ≥ 99% completed, end backlog ≤ 1%.

- D = 3 × 0.019286005545956553 = 0.0578580166.
- M60 = 1,474,560 ns, so the bound is M60 × (1 + D) = **1,559,875.1 ns**.

| Cell | median total p99 | ≤ bound | sustained | median p99.9 (report only) | median max (report only) | periodic checkpoints per rep |
|---|---|---|---|---|---|---|
| 60 s | **1,474,560** | (reference) | 3 / 3 | 10,747,904 | 50.1 ms | 1 |
| 120 s | **1,146,880** | yes (73.5% of bound) | 3 / 3 | 9,437,184 | 74.1 ms | 2 |
| 240 s | **1,507,328** | yes (96.6% of bound) | 3 / 3 | 9,699,328 | 59.5 ms | 4 |

| rep (run order) | p99 ns | p99.9 ns | max ms | EXEC overloaded | INDEX refused | checkpoints |
|---|---|---|---|---|---|---|
| 120s-r1 | 1,179,648 | 9,175,040 | 74.1 | 0 | 0 | 2 |
| 120s-r0 | 1,146,880 | 9,961,472 | 75.3 | 0 | 0 | 2 |
| 120s-r2 | 1,048,576 | 9,437,184 | 53.2 | 0 | 0 | 2 |
| 60s-r2 | 1,146,880 | 10,747,904 | 50.0 | 0 | 0 | 1 |
| 240s-r2 | 1,474,560 | 9,437,184 | 59.5 | 0 | 0 | 4 |
| 240s-r0 | 1,507,328 | 10,223,616 | 56.6 | 0 | 0 | 4 |
| 240s-r1 | 1,540,096 | 9,699,328 | 59.7 | 0 | 0 | 4 |
| 60s-r1 | 1,474,560 | 11,010,048 | 50.1 | 0 | 0 | 1 |
| 60s-r0 | 1,474,560 | 10,223,616 | 50.5 | 0 | 0 | 1 |

Under PREREG.md, a PASS means the 60 s Profile-A window is validated, the CP-1A results stand, and the
next step is the formal `CRITERIA.md` review. That decision is the reviewer's.

## For the reviewer (facts, not rulings)

1. **The p99 level of this session is above CP-1A's at the same rate and on the same run path.**
   - CP-1A's A reps at 654,569 ops/s had total p99 of 0.90–0.95 ms. The 3 sweep reps and the 5 fixed
     reps each have a median of 933,888 ns.
   - Here, the 60 s reps are 1.15–1.47 ms (median 1,474,560 ns), about 1.58 × CP-1A.
   - The CP-1A sweep reps used the same `Pilot.one(..., memory=False)` path and the same 60 s window as
     these 60 s reps. So the PREREG design point (no memory-settle pauses) does not explain the gap.
   - The preregistered test compares windows **within** this session only. It does not measure a shift
     between sessions, and D (from CP-1A's within-session CV_p99 of 1.9%) was not built for one.
   - The cause is not diagnosed here. No extra run was made.
2. **The 240 s cell sits at 96.6% of the bound** (margin 52,547 ns). The p99 histogram's 1/32 bucket
   width is about 47,000 ns at this level (`PILOT_PLAN.md` §7). So the margin is about one bucket.
3. **The cells are not monotonic in window length.** The 120 s median (1.15 ms) is below both the 60 s
   (1.47 ms) and the 240 s (1.51 ms) medians. Within the 60 s cell the reps span 1.15–1.47 ms.
4. **p99.9 and max are report only.** p99.9 is 9.2–11.0 ms in every cell, with no growth with window
   length. Max is 50–75 ms. CP-1A's fixed reps had p99.9 of 8.4–8.9 ms.
5. **Checkpoints scale as expected:** 1 per 60 s run, 2 per 120 s, 4 per 240 s. Every run, including all
   4 checkpoints in the 240 s runs, had 0 refusals.
6. **Unrelated to this result:** `test_pilot` is still red on `main` (`tests/test_pilot.py:366`, since #110).
