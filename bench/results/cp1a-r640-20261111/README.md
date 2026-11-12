# CP-1A R640: full SNC-only pilot result (2026-11-11/12), for independent review

**One session**, run under the GO record
[docs/handoff/CP1A_R640_GO_20261111.md](../../../docs/handoff/CP1A_R640_GO_20261111.md):

```
python3 -B bench/pilot.py --out /home/solo/cp1a-r640-20261111 --build build-s6 \
  --manifest bench/manifests/r640-main-20261111.txt --scratch /dev/shm/snc-pilot
```

- HEAD `b2948a3` (the PR #110 GO-record merge, no commit in between); runner commit `4250b32`.
- 2026-11-11 21:18:13 UTC to 2026-11-12 00:27:53 UTC; exit 0; **no abort** (`results.aborted` unset).
- Profiles A then C, allocation PILOT-L, 8 shards baseline.

`pilot.json` and `raw/` (372 files) are the runner's output, written outside the tree during the run
and copied in unchanged afterwards (`diff -r` and `cmp` identical; `pilot.json` SHA-256 `71c24e9c…73b7`).
`console.log` is the runner's console. This README is a reading of those files. Where it differs from
them, they are right. Nothing was rerun, tuned, skipped or hand-selected.

## Identity and preflight (`pilot.json` `environment`)

- `git_head` `b2948a31…`, `git_status` empty; `freeze_checked` true, `freeze_problems` [].
- Plan SHA-256 `568e9874…68c7` and manifest-e SHA-256 `768056bb…27b6`: the frozen values.
- `snc_server` `69a51b69…`, `net_bench` `9fc54944…`: the frozen `build-s6` binaries.
- RelWithDebInfo, no extra CXX flags; all governors `performance`; `numa_balancing` 0; kernel 6.8.0-149.
- Scratch `/dev/shm/snc-pilot`, data `/srv/snc-data/pilot`, out on `/dev/sda2`: no shared physical disk with the data SSD.
- Afterwards the data and scratch directories were removed by the runner; no SNC process left running.

## Headline results

| | Profile A (LOCAL_DURABLE) | Profile C (memory) |
|---|---|---|
| Calibration P0 (ops/s) | 4,363,795 | 6,079,563 |
| **Stable peak S** | **1,090,948** | **5,775,584** |
| Objective (7/10, largest grid rate ≤ f·S) | 654,569 | 3,951,715 |
| Objective p99 (total, median of 3) | 933,888 ns | 524,288 ns |
| Knee (reported only) | none (no rate ≤ S above 2 × p99 at R_1) | 911,934 |
| Sweep invalid | no | no |
| Fixed reps at objective: sustained / CV_p99 | 5/5 / 0.0193 | 5/5 / 0.0 |
| Memory (median RSS = PSS per object) | 515.5 B | 515.3 B |
| Capacity reps (closed loop): median / CV_thr | 4,031,898 / 0.0035 | 6,030,447 / 0.0042 |
| Noise input 3 × max(CV_thr, CV_p99) | 0.0579 | 0.0127 |
| Shard sweep errors | 0 | 0 |

## Points for the reviewer (facts, not rulings)

1. **Profile A's stable peak is about 27% of its closed-loop capacity** (1.09 M vs 4.03 M ops/s). The
   boundary point 1,309,138 is unsustained because **one of three reps refused 33 ops (`overloaded`)**;
   the other two had 0. Under the pre-registered rule (a point is sustained only if all 3 reps are),
   that sets S = 1,090,948 (0 refusals in all 3 reps). Every A point from 1.31 M up is unsustained by
   refusals (685/870/687 at 1.53 M), with no point `client_limited_suspect`. Profile C has no refusal up
   to S = 5.78 M and refuses heavily from 6.08 M (≈ 0.8–1.2 M per rep).
2. **Profile A's duration check drifts.** It runs at 2,181,897 (the largest grid rate ≤ P0/2), which is
   above A's S. The 30 s window is sustained (0 refusals); 60/120/240 s are not (6,900 / 15,328 / 48,381
   refusals), and total p99 goes 1.03 → 2.10 → 2.23 → 2.29 ms. PILOT_PLAN §6: "A drift sends the window
   back to review." The plan's A run length (each sweep run > 60 s from server start, so it includes a
   checkpoint) was pre-registered; the 30 s duration run may contain no checkpoint. Profile C's duration
   check shows no drift (p99 434,176 ns in all four windows, all sustained).
3. **`test_pilot` is red on `main`** since PR #110 (`tests/test_pilot.py:366` asserts `go_record is None`),
   as flagged in #110. The runner does not run tests; this did not affect the run.

### Profile A: sweep (`results.A.sweep.points`)

| rate | sustained | median p99 ns | median achieved | reasons |
|---|---|---|---|---|
| 218,189 | yes | 770,048 | 218,188 |  |
| 436,379 | yes | 868,352 | 436,378 |  |
| 654,569 | yes | 933,888 | 654,567 |  |
| 872,759 | yes | 1,081,344 | 872,756 |  |
| 1,090,948 | yes | 1,245,184 | 1,090,944 |  |
| 1,309,138 | **no** | 1,343,488 | 1,309,133 | refused (overloaded) |
| 1,527,328 | **no** | 1,540,096 | 1,527,309 | refused (overloaded) |
| 1,745,518 | **no** | 1,736,704 | 1,745,467 | refused (overloaded) |
| 1,963,707 | **no** | 1,900,544 | 1,963,626 | refused (overloaded) |
| 2,181,897 | **no** | 2,162,688 | 2,181,772 | refused (overloaded) |
| 2,400,087 | **no** | 2,359,296 | 2,399,900 | refused (overloaded) |
| 2,618,277 | **no** | 2,752,512 | 2,617,991 | refused (overloaded) |
| 2,836,466 | **no** | 3,014,656 | 2,836,057 | refused (overloaded) |
| 3,054,656 | **no** | 2,949,120 | 3,051,819 | refused (overloaded) |
| 3,272,846 | **no** | 3,014,656 | 3,262,054 | refused (overloaded) |
| 3,491,036 | **no** | 2,424,832 | 3,461,301 | refused (overloaded) |
| 3,709,225 | **no** | 2,555,904 | 3,662,814 | completed < owed; refused (overloaded) |
| 3,927,415 | **no** | 5,767,168 | 3,872,718 | completed < owed; refused (overloaded) |
| 4,145,605 | **no** | 6,946,816 | 3,927,859 | completed < owed; refused (overloaded) |
| 4,363,795 | **no** | 15,728,640 | 4,115,133 | completed < owed; refused (overloaded) |
| 4,581,984 | **no** | 24,117,248 | 3,915,266 | completed < owed; refused (overloaded) |
| 4,800,174 | **no** | 16,252,928 | 4,196,950 | completed < owed; refused (overloaded) |

### Profile A: duration check

| window s | achieved | p99 ns | sustained |
|---|---|---|---|
| 30 | 2,181,871 | 1,032,192 | yes |
| 60 | 2,181,770 | 2,097,152 | **no** |
| 120 | 2,181,726 | 2,228,224 | **no** |
| 240 | 2,181,561 | 2,293,760 | **no** |

### Profile A: shard sweep (closed loop, median of 3)

| shards | ops/s | ops/s per core | errors |
|---|---|---|---|
| 1 | 1,401,014 | 1,401,014 | 0 |
| 2 | 1,784,041 | 892,020 | 0 |
| 4 | 3,203,823 | 800,955 | 0 |
| 8 | 4,044,638 | 505,579 | 0 |

### Profile C: sweep (`results.C.sweep.points`)

| rate | sustained | median p99 ns | median achieved | reasons |
|---|---|---|---|---|
| 303,978 | yes | 34,816 | 303,978 |  |
| 607,956 | yes | 61,440 | 607,956 |  |
| 911,934 | yes | 106,496 | 911,933 |  |
| 1,215,912 | yes | 180,224 | 1,215,910 |  |
| 1,519,890 | yes | 249,856 | 1,519,886 |  |
| 1,823,868 | yes | 303,104 | 1,823,862 |  |
| 2,127,847 | yes | 344,064 | 2,127,839 |  |
| 2,431,825 | yes | 376,832 | 2,431,814 |  |
| 2,735,803 | yes | 401,408 | 2,735,790 |  |
| 3,039,781 | yes | 434,176 | 3,039,766 |  |
| 3,343,759 | yes | 466,944 | 3,343,741 |  |
| 3,647,737 | yes | 499,712 | 3,647,717 |  |
| 3,951,715 | yes | 524,288 | 3,951,691 |  |
| 4,255,694 | yes | 557,056 | 4,255,670 |  |
| 4,559,672 | yes | 622,592 | 4,559,641 |  |
| 4,863,650 | yes | 671,744 | 4,863,617 |  |
| 5,167,628 | yes | 737,280 | 5,167,591 |  |
| 5,471,606 | yes | 802,816 | 5,471,563 |  |
| 5,775,584 | yes | 917,504 | 5,775,536 |  |
| 6,079,563 | **no** | 3,276,800 | 6,049,444 | refused (overloaded) |
| 6,383,541 | **no** | 4,128,768 | 6,085,097 | completed < owed; refused (overloaded) |
| 6,687,519 | **no** | 5,505,024 | 5,985,827 | completed < owed; refused (overloaded) |

### Profile C: duration check

| window s | achieved | p99 ns | sustained |
|---|---|---|---|
| 15 | 3,039,750 | 434,176 | yes |
| 30 | 3,039,765 | 434,176 | yes |
| 60 | 3,039,774 | 434,176 | yes |
| 120 | 3,039,777 | 434,176 | yes |

### Profile C: shard sweep (closed loop, median of 3)

| shards | ops/s | ops/s per core | errors |
|---|---|---|---|
| 1 | 2,620,890 | 2,620,890 | 0 |
| 2 | 3,182,032 | 1,591,016 | 0 |
| 4 | 4,122,496 | 1,030,624 | 0 |
| 8 | 5,977,522 | 747,190 | 0 |
