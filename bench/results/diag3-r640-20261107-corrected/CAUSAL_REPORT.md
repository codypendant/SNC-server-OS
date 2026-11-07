# Corrected proof and causal cells: report (for review)

- **Run:** frozen driver 69d612f, R640, 2026-11-07 05:59-06:21 UTC. Evidence: `run/` (commit c88cb70).
- **Phase analysis:** `analysis/phases.py`, output `analysis/phases.json`. It is **post-hoc**: the
  preregistration fixed the cells and gates, not this split.
- **Status:** the evidence is complete. The classification below is Claude's proposal; the reviewer
  decides. No engine change, no pilot rerun.

## 1. Preflight and gates

- **Preflight passed.** The scratch was `/dev/shm/snc-diag3`. tmpfs counts as its swap disk, which is
  `sdc`, the BOSS disk. The out directory was also on `sdc`, and the data on `sda` (the SAS SSD). All
  identity checks held at preflight and again at postflight. The T build was unchanged.
- **G0-G4 all pass**, so the driver went on into the causal cells as authorized.

| Proof, 3 reps | B (pilot build) | T (telemetry) |
|---|---|---|
| closed, median ops/s | 4,338,624 (-0.6% vs P0 4,366,466) | 4,332,541 (99.9% of B) |
| w30 2,183,233, refusals | 0 / 0 / 0 | 0 / 0 / 0 |
| w30 p99 / p99.9 | 0.999 ms / 1.245 ms in every rep | 0.999-1.016 ms / 1.245 ms |
| w30 `late_over_1ms` | 0 / 0 / 0 | 0 / 0 / 0 |

With scratch isolated, the w30 cell is refusal-free in both arms. The contaminated harness gave
0 / 256 / 1,016 refusals and about 200,000 late ops per rep. The closed loop still refuses a little
(B 488 / 514 / 359, T 0 / 619 / 0), because each closed run contains one checkpoint.

## 2. Causal cells (T arm, 3 reps, 60 s window)

| Cell | Refusals (share of scheduled_in_window) | p99 | p99.9 | `late_over_1ms` |
|---|---|---|---|---|
| 436,646, checkpoint 60 s | 2,267 / 6,488 / 9,455 (0.009-0.036%) | 1.61 / 1.61 / 1.97 ms | 53 / 62 / 69 ms | 54,119 / 60,105 / 104,274 |
| 436,646, no checkpoint | **0 / 0 / 0** | 0.66 / 0.64 / 0.64 ms | 0.85 / 1.08 / 0.84 ms | **0 / 0 / 0** |
| 2,183,233, checkpoint 60 s | 1,062,187 / 955,970 / 955,428 (0.73-0.81%) | 172 / 176 / 319 ms | 294 / 352 / 554 ms | 7.17M / 7.44M / 7.20M |

Every checkpoint run logged exactly one checkpoint, about 54 s after `run_start`, inside the measured
window. The no-checkpoint runs logged none.

## 3. Where the refusals fall (post-hoc, 50 ms buckets)

| Cell | Walk | Chunk-write drain | fdatasync | 1 s after | Elsewhere |
|---|---|---|---|---|---|
| 436,646 + checkpoint | 0 | 0 | **all** (2,267 / 6,488 / 9,455) | 0 | 0 |
| 2,183,233 + checkpoint | 17k / 126k / 111k | 543k / 370k / 622k | 498k / 454k / 211k | 4k-11k | 0 |

- **Phase lengths.** At 436,646 the walk took 1.17-1.20 s and the fdatasync 1.13-1.21 s. At
  2,183,233 the walk took 1.69-1.71 s and the fdatasync 1.00-1.26 s.
- **WAL device write await.** The median before the checkpoint was 0.048-0.065 ms. The maximum
  during fdatasync was 10.7-12.1 ms.
- **Late ops.** All late ops are inside the checkpoint, plus the 50 ms bucket that contains
  `begin_checkpoint`. That bucket alone held about 7,000 late ops at 436,646; that phase is O(map).
- **Resolution caveat.** Phase edges are the latest shard's mark, and the buckets are 50 ms. The
  split between walk and drain, and between drain and fdatasync, is therefore approximate. The
  totals inside and outside the checkpoint are not.

## 4. What this establishes

1. **The checkpoint is causal for the refusals at 436,646, open loop.** With a checkpoint, all
   3 reps refused. Without one, all 3 refused 0 and had 0 late ops. The same build, rate and
   isolated harness were used in both.
2. **At 436,646, refusals occur only during the checkpoint's fdatasync.** That phase lasts about
   1.2 s, all 8 shards are in it together, and the WAL's device write await rises about 200x. The
   walk costs throughput but caused no refusals at this rate.
3. **At 2,183,233, the walk also refuses.** The shard CPU taken by the walk is enough at 5x the
   rate. Refusals still stay inside the checkpoint (plus less than 1 s of recovery), and none occur
   between checkpoints.
4. **The original pilot's transition at R_2 = 436,646 is explained by the checkpoint, not by the
   ID file.** With the ID file isolated, 436,646 still refuses whenever a checkpoint falls in the
   window. So a clean CP-1A rerun under the current engine would again place S at R_1 under the
   approved zero-refusal sustained rule. Claude infers this; it has not been run.

## 5. Proposed classification (reviewer decides)

- **ID-file writeback:** BENCHMARK DEFECT. Fixed (PR #64) and confirmed by this proof.
- **Refusals at R_2 and above:** caused by an **unpaced checkpoint**. All 8 shards flush and
  fdatasync together on the WAL's device, so durable capacity collapses for about 1 s.

  Claude proposes classifying this as an **engine design gap (REFINE candidate)**, not an "expected
  limit". The review already listed two gaps; this run confirms both:
  - Refusals land on every request type during the stall. The EXEC backlog gate is checked before
    the op type, so reads are refused too.
  - A periodic, unpaced durability collapse, which this run shows to be the only refusal source at
    436,646.

Per the review, nothing is fixed yet, and `kEmitBacklog` stays at 1,024. The options listed in
`docs/handoff/PR62_Codex_Review.txt` remain the design candidates:
- separate read admission from mutation admission;
- checkpoint pacing;
- WAL/checkpoint I/O isolation or prioritization;
- staggered shard sync;
- incremental or copy-on-write checkpoints.

## 6. Unchanged and not authorized

- No full CP-1A rerun, Profile C, competitors or engine optimization.
- `r640-diag-telemetry` stays off main.
- The first pilot remains diagnostic history.
