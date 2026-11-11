# R640 S2 pacing re-derivation (2026-11-11)

Authorized by [docs/handoff/PR97_Manifest_Closure_Final_Review.md](../../../docs/handoff/PR97_Manifest_Closure_Final_Review.md)
("NEXT AUTHORIZED TASK", A-F), which carries forward PR #75 review section 6:
the S2 walk budgets are dev-VM hypotheses and must be re-derived on the
R640 before any physical run. Evidence only. No product code was changed,
and no S6 data exists.

## A. Code identity

| | |
|---|---|
| Merged post-PR97 `main` | `0dbd4c77b993572fb7b771fa44e7520c3197fe1b` (Merge PR #97), clean tree |
| Build | `build-s6/`, fresh, `cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo` (the campaign build type), GCC 13.3.0, CMake 3.28.3 (`build.log`) |
| `ckpt_walk_cost` SHA-256 | `4707dac135a6fc72c41a712075678aa24ccbf7ad74d546962623026584b5d2d2` |
| Manifest | e, `bench/manifests/r640-main-20261111.txt`, SHA-256 `768056bbee1d7f178ed0be1c195b821f0638a1de28a4aca1ea0afdf5444c27b6` |

## B. Host and filesystem check (`host-check.txt`)

Reapplied and recorded before any measurement (14:32 UTC):
- Governor `performance` on 72 of 72 CPUs.
- `kernel.numa_balancing = 0`.
- RAM as manifest e: 12 × 32 GB M393A4K40CB1-CRC, 2400 MT/s, rank 2, in
  A1-A6 and B1-B6; `free -g` 376 GiB total; nodes 0 and 1 at 192,028 and
  193,517 MB.
- Data SSD: `/dev/sdb`, model `ARFX3840S5xnFTRI`, serial S48LNA0M700325.
- `/srv/snc-data`: ext4, `rw,relatime,stripe=2`, the same mount options
  as manifests d and e.

**The manifest d (2.2 GB used) vs manifest e (44 KB used) difference.** I
recorded what can be verified and did not assume a cause:

| Fact | Value |
|---|---|
| Filesystem UUID | `3bf857c8-0cb4-4c57-b9a5-14328e70b061`, label `snc-data`, PARTUUID `63f8b7cb-a1af-4e2e-8ee9-07860802d97a` |
| `Filesystem created` | **Fri Nov 6 06:00:03 2026**. Manifest d was captured at 2026-11-06T12:59:32Z, so this is the **same filesystem** manifest d measured. It was not recreated. |
| Lifetime writes | 3,516 GB |
| Mount count | 5; last mount 2026-11-11 13:50:40 (this boot) |
| Block / inode size | 4096 / 256 |
| Features | has_journal ext_attr resize_inode dir_index filetype needs_recovery extent 64bit flex_bg sparse_super large_file huge_file dir_nlink extra_isize metadata_csum |
| Reserved blocks | 46,884,211 |
| Top level | `diag`, `diag2`, `diag3`, `step1` (all empty; last modified 2026-11-06 20:01 to 2026-11-07 06:22) and `lost+found` (2026-11-06 06:00) |

- The data that made up the 2.2 GB is gone, and the directories that held
  benchmark data remain, empty.
- Nothing in the repository or the shell history on this host records
  what removed it. The cause is **not determined**, and this record does
  not supply one.
- It does not matter for the measurements here: what was checked is that
  the filesystem is the same one, and that it is not initializing.
- Device name: manifest d saw this SSD as `/dev/sda1`, manifest e and this
  check as `/dev/sdb1`. The model, mount point and filesystem creation
  time identify it, not the device letter.

**ext4 lazy initialization.** It cannot contaminate storage measurements:
- `dumpe2fs`: 28,616 of 28,616 block groups `ITABLE_ZEROED`, 0 not.
- There is no `ext4lazyinit` thread.
- `/proc/diskstats` for `sdb1`: no write in 10 s idle.

No filesystem configuration was changed.

**Afterwards:** the walk-cost scratch (653 MB under
`/srv/snc-data/walkcost-20261111`) was removed. `df` is back to 44 KB
(last line of `host-check.txt`).

## C. Walk cost on the R640

### C.1 What `bench/ckpt_walk_cost` alone could and could not show (`walk-cost/`)

`run_walk_cost.sh` setup:
- 625,000 IDs on one shard: the S6 per-shard count (5,000,000 objects over
  8 shards). 1,000,000 IDs as well, for comparison with the dev VM.
- Modes: off, a 10 s budget, and budgets of 1, 2, 5, 10, 20, 25, 50, 100
  and 250 µs.
- 3 processes × 3 checkpoints each.
- Placement L0 = `numactl --physcpubind=2 --membind=0`. The sensitivity
  runs X1 (remote memory) and L2 (`--physcpubind=2,4`) use 1 process each.
- `raw.txt` has every line; `walk-cost-summary` is below.

Two findings make its summary figures unusable as the pacing input:
1. **`walk_us` is not walk CPU.** With S3's dsync window (4 chunks), a
   full window stops the walk step. So even the 10 s budget took 41 steps
   of at most 20,464 IDs, and the walk phase time mostly waits on the SSD.
   `walk_ns_per_object` (900-1000 ns) is therefore an I/O-bound wall
   time, not the per-ID cost.
2. **On one CPU, step maxima measure preemption.** At L0 the longest step
   is about 850-1,000 µs in every mode, including a 1 µs budget. Two CPUs
   (L2) remove it, so it comes from another thread of the process (or its
   I/O worker) sharing the shard's CPU, not from the walk.

The gauges also give only the maximum step, not its distribution.

### C.2 Per-step timing of the unchanged binary (`probe/`)

To get the distribution without changing any code, `step_probe.bt` (bpftrace
uprobes) timed, in the same binary:
- each `Shard::checkpoint_step` that walked;
- each call of its visit lambda: one call of 256 IDs with pacing off, one
  call of one batch with pacing on.

Setup:
- `run_probe.sh`: 625,000 IDs, placements L2 and L0, nine modes, 3
  processes × 3 checkpoints each (54 runs, all rc 0, 14:43-14:47 UTC).
- Raw events: `probe/*.events.gz`. Summary: `probe-summary.txt`, made by
  `summarize_probe.py`.
- Each probed call carries about 1-2 µs of probe overhead. Every probed
  figure is therefore an **upper bound** on the true cost.

| L2 (CPUs 2,4; node 0) | n | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| **Legacy 256-ID visit** (pacing off) | 21,978 | **171.6 µs** | | **187.7** | | 417.6 |
| Legacy whole step (pacing off) | 21,978 | 178.0 | 183.6 | 194.3 | 202.7 | 424.2 |
| **Batch-16 visit** (all paced modes) | 351,567 per mode | **13.3 µs** | | **16.4** | | 221-412 |
| Paced step, budget 1/5/10 µs (1 batch) | 351,567 | 19.3 | 19.8 | 22.5 | 29.8 | ≤414 |
| Paced step, budget 20 µs (2 batches) | 176,349 | 35.5 | 36.4 | 39.3 | 46.4 | 263 |
| Paced step, budget 50 µs | 90,821 | 67.9 | 68.7 | 71.4 | 78.8 | 268 |
| Paced step, budget 100 µs | 51,245 | 116.5 | 117.9 | 120.1 | 127.0 | 331 |
| Paced step, budget 250 µs | 22,087 | 262.2 | 266.5 | 269.1 | 273.6 | 386 |

Other observations:
- **Per-ID walk cost:** 171.6 / 256 ≈ **670 ns** (p99 733 ns). An
  unprobed batch is therefore about 16 × 670 ≈ 10.7 µs; the probed
  13.3 µs is the upper bound.
- **Where it goes** (`perf/off-L2.top.txt`): `snc::encode_record`
  (imaging each object into the chunk) is the largest symbol, at 37.7% of
  all process samples. So the walk is not the "about 80 ns per ID"
  snapshot cost that S2 section 5 guessed for the R640; it costs about the
  same per ID as the dev VM's 0.6-0.8 µs.
- **Paced steps** end at budget + at most one batch: the 1 µs budget
  visits exactly 16 IDs per step, and 20 µs visits 2 batches. The probe
  inflates the batches (the probed run at 100 µs visits about 110 IDs per
  step; unprobed, about 149).
- **Run-to-run variation:** across the 3 processes of a mode, step p50
  differs by under 1% (pacing off: 177.6-178.5 µs; 100 µs: 116.4-116.5;
  20 µs: 35.5 in all three). Visit-cost p50 is 13.20-13.33 µs across all
  eight paced modes.
- **L0 (one CPU)** has the same p50s (256-ID visit 174.4 µs, batch 13.3 µs)
  but a p99 of 0.2-1.0 ms in every mode, pacing off included: the
  preemption of C.1. It is a placement effect that pacing cannot change.
  It is recorded as an S6 risk (prereg section 8).
- **X1 (remote memory)** was run only with the tool (C.1). Its I/O-bound
  figures are within the spread of L0's and say nothing about walk CPU.
  It was not probed.

## D. Derivation of the R640 values

**Inputs** (L2, probe upper bounds):
- T256 = 171.6 µs (p50 of the legacy 256-ID visit). The p50 is used, not
  the p99, because a smaller reference gives smaller budgets.
- b = 16.4 µs (p99 of one batch-16 visit). This is the worst routine
  overshoot past a budget.

**Rules.** Each states S2's intent (PR97 handoff D; S2_PACING section 5)
as an inequality, rounded down to 5 µs:

| Value | Rule | Arithmetic | R640 | Dev-VM default | Supported? |
|---|---|---|---|---|---|
| `ckpt_walk_ceiling_ns` | the worst catch-up step (ceiling + one batch) must not monopolize more than the legacy step: ceiling + b ≤ T256 | 171.6 − 16.4 = 155.2 | **155 µs** | 250 µs | **No.** 250 + 16.4 = 266 µs = 1.55 × T256 |
| `ckpt_walk_max_ns` | normal mode at most half the legacy step, overshoot included: max + b ≤ T256 / 2 | 85.8 − 16.4 = 69.4 | **65 µs** | 100 µs | **No.** 100 + 16.4 = 116 µs = 0.68 × T256 |
| `ckpt_walk_min_ns` | non-zero progress of about two batches per iteration: one batch < min ≤ two batches | measured: a 20 µs budget visits 2 batches per step (31.9 IDs mean); 10 µs visits 1 | **20 µs** | 20 µs | **Yes**, independently measured here |
| `ckpt_walk_batch` | overshoot bounded and accounted for in both rules above; clock reads negligible | b = 16.4 µs ≈ 11% of the ceiling | **16** | 16 | **Yes** |
| `ckpt_walk_step_ns` | min to max in about 8 updates (S2's rule) | (65 − 20) / 8 = 5.6 | **5 µs** (9 updates, 9 ms) | 10 µs | re-derived from the new max |
| `ckpt_pace_update_ns` | fast against checkpoints of seconds, slow against one iteration | unchanged reasoning | **1 ms** | 1 ms | not hardware-specific |
| WAL 200/125%, backlog 50/25% | | | **inherited, not tuned** | | PR #75 section 5 forbids tuning them from dev-VM data, and no R640 pressure data exists. Choosing them from S6 would be choosing after the data. They are recorded as hypotheses. |

Check against the legacy step, worst routine case (budget + b):
- min: 36.4 µs (0.21 × T256);
- max: 81.4 µs (0.47 ×);
- ceiling: 171.4 µs (1.00 ×).

The walk can therefore never take materially more per loop than the
256-ID step did.

Notes:
- **Under load** the walk is likely slower per ID (8 shards walking at
  once share memory bandwidth). A time budget then visits *fewer* IDs. The
  monopolization bound is in time, so it still holds; only progress per
  step falls. This is the safe direction.
- **Progress against the target.** At the floor (2 batches per step), a
  625,000-ID walk needs about 19,500 walking iterations: about 650 per
  second to finish within a 30 s target. Catch-up raises the budget up to the
  ceiling: about 155 / 0.67 + 16 ≈ 250 IDs per step, about 2,500 steps.
  This is an estimate from the per-ID cost, not measured at 155 µs.
- **No value was chosen from S6 results.** None exist.

**Blocker (found here, not fixed).** `snc_server` has flags for
`--ckpt-pacing`, `--ckpt-target-ms` and `--ckpt-max-age-ms`, but **none for
the walk batch, min, max, ceiling or step** (`tools/snc_server.cpp`; they
are `Engine::Config` fields only). The derived max (65), ceiling (155) and
step (5) differ from the compiled defaults. So the frozen SHA
`0dbd4c7` cannot run S6 with the derived values. This is the case the
handoff names: a separately reviewed change is required. The proposal is
in the prereg, section 2.

## Files

| File | What |
|---|---|
| `host-check.txt` | governor, NUMA balancing, DIMMs, SSD, filesystem identity, lazy-init, idle I/O, cleanup |
| `build.log` | the build at `0dbd4c7` |
| `run_walk_cost.sh`, `walk-cost/` | the repository tool's runs (C.1): `identity.txt`, `create.log`, `raw.txt` |
| `step_probe.bt`, `run_probe.sh`, `probe/` | the probed runs (C.2): `runs.txt`, per-run `.stdout` and `.events.gz` |
| `summarize_probe.py`, `probe-summary.txt` | the summary table |
| `perf/` | profile summary of one pacing-off run (L2) |
