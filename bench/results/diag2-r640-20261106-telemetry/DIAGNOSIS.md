# CP-1A R640 pilot abort: diagnosis report (revision 2)

Evidence: pilot at 18373583; first diagnosis at 5ffe278 (`../diag-r640-20261106-cp1a/REPORT.md`); the
stopped telemetry proof in this directory (`PREREG.md`, `gates.json`, `raw/`). Scripts: `analysis/`.
**No load was run for this revision.** Everything here reads existing logs and code, plus read-only
storage state.

**Status of the evidence: post-hoc.** The proof stopped on G0 and G2, so the preregistered causal
cells never ran. This analysis of the 12 proof runs was not preregistered. It can direct the next
experiment, but it does not replace those cells.

## Classification

| Finding | Class | Confidence |
|---|---|---|
| Refusals at 436,646 and above: checkpoint starves the WAL | **EXPECTED ARCHITECTURAL LIMIT** (provisional) | Mechanism measured closed-loop; open-loop link is one perturbed A/B rep |
| One stall per run with no checkpoint | **UNRESOLVED**, benchmark artefact suspected | Stall measured; source not identified |
| Calibration, open-loop schedule, abort logic | No defect found | Verified |
| Engine correctness | No defect found | No errors or lost work; refusal is documented behaviour |

No fix was made. The class is provisional until the causal cells run and the reviewer accepts it.

## Correction to the work order's facts

"30s: sustained, zero overloads" does not reproduce. On the unchanged pilot build, 2,183,233 w30
refused 0, 256 and 1,016 (`summary.json`). G0 and G2 failed only on their zero-refusal clauses; the
pilot's single run was a favourable draw. G1, G3 and G4 passed and T refusals (5,945 / 169 / 34)
partly overlap B's, so this telemetry shows no clear perturbation at this sample size, unlike the
100 ms polling in the first diagnosis.

## Raw-log timeline

Times are seconds after `run_start`, on one clock. T-closed = closed loop, depth 512, 3 reps.

| Phase (T-closed r0 / r1 / r2) | Duration | Client ops/s | WAL device await | Flushes/s | Polls per 50 ms |
|---|---|---|---|---|---|
| Before | - | 4.51 / 4.47 / 4.49M | 0.13-0.14 ms | ~4,000 | ~170 |
| `begin_checkpoint` | ~50 ms | 0.37-0.74M, first bucket | - | - | 8-40 |
| Walk | 1.67 / 1.56 / 1.67 s | 1.99 / 1.97 / 1.99M | 0.06 ms (r1 0.35) | ~6,800 | ~72 |
| `fdatasync` | 1.19 / 0.93 / 1.18 s | 0.45 / 0.35 / 0.57M | 7.1-9.3 ms | 123-192 | ~38,000 |
| After | - | 4.47 / 4.51 / 4.50M | back to normal | ~4,000 | ~170 |

Device during `fdatasync`: ~1,090 MB/s at 96% utilisation, the highest sustained rate seen.

Pilot runs (checkpoints from server logs):

| Run | Checkpoints | EXEC / INDEX refused | p99 |
|---|---|---|---|
| 218,323 r0-r2 | 1 each | 0 / 0 | 1.1 ms |
| 436,646 r0 / r1 / r2 | 1 each | 11,936 / 23,726 / 598; INDEX 0 | 1.5-2.1 ms |
| 654,969 r0 / r1 / r2 | 1 each | 18,020 / 75,922 / 1,557; INDEX 0 | 2.4-14.7 ms |
| 873,293 r0 / r1 / r2 | 1 each | 72,912 / 237,922 / 180,094; INDEX 0, 0, 7 | 13.1-47.2 ms |
| 2,183,233 r0 / r1 / r2 | 1 each | 861,260 / 1,105,615 / 764,278; INDEX 10,563, 7,208, 42,699 | 143-319 ms |
| 2,183,233 30s | **0** | 0 / 0 | 1.05 ms |
| 2,183,233 60s | 1 | 767,464 / 24,796 | 268 ms |
| 2,183,233 120s | 2 | 1,631,112 / 107,664 | 428 ms |
| 2,183,233 240s | 4 | 5,267,176 / 276,630 | 277 ms |

## First overload transition

- **Across the grid:** first failure is 436,646, in all 3 reps. Refused fraction 0.002-0.08% of
  scheduled ops; achieved at least 99.9% of offered. Only the zero-refusal clause fails it.
- **Within a run, no checkpoint** (T-w30, 50 ms resolution): one event in each of 3 reps, at 26.8,
  34.0 and 30.3 s. In order: all 8 shards stall in the same sample, `undurable` rises from ~430 to
  ~12,000 summed, `waiters` hits 8,192 (every client slot), then lateness, then refusals in the
  next bucket. Recovery takes about 100 ms.
- **Per shard, now measured:** the gate tripped unevenly, 7 of 8 shards in r0 but only 1 of 8 in r1
  and r2. Peak shard backlog was 1,101, 768 and 915 by sample; refusing shards crossed 1,024 between
  50 ms samples. Whether a run refuses at all therefore depends on where the backlog lands.

## Checkpoint relationship

Correlation is established; causation is measured closed-loop only.

- WAL await goes from 0.06 ms to 7-9 ms exactly when `fdatasync` starts, and recovers when it ends.
  This settles what the first report left open.
- `fdatasync` is storage, not CPU: shard polling rises over 200x from baseline, so threads are free and waiting for
  durability. Polling is never blocked, and io_uring is in use. pidstat (1 Hz, one perturbed run)
  shows process I/O wait of 0-3% across the checkpoint, with usr rising.
- The walk is different: it costs **CPU on the shard thread**. Polling drops 58%, throughput 55%,
  and the disk stays fast. 625,000 objects per shard / 256 per poll x 0.69 ms per poll = 1.68 s,
  matching the measured walk.
- Checkpoint work is unpaced: `shard.cpp:1582-1596` buffered 1 MiB writes, then one `fdatasync` per
  shard, all shards together on the WAL's device. All 8 start within 1 ms.

**Why 436,646 is the transition (derived from closed-loop data):** durable capacity during `fdatasync`
is 0.35-0.57M ops/s, against offered 218,323 (below it) and 436,646 (inside it). This also fits the
reps' wide spread (598 to 23,726), which is what a marginal point looks like. The open-loop check is
one perturbed A/B rep from the first diagnosis: 26,770 refusals with checkpoint, 0 without.

**Why the 30s to 60s jump (arithmetic estimate):** at 2.18M offered, a checkpoint leaves about 2.2M ops
behind: walk (0.18M/s x 1.6 s) plus `fdatasync` (1.7M/s x 1.1 s). That is about 1.7% of a 60 s window's
131M ops, over the 1% that sets p99, because latency is measured from intended send time. p99 therefore
lands inside the stall. It does not grow with duration (268, 428, 277 ms) and refusals scale with
checkpoint count (about 770k, 815k, 1.3M each). It is a per-checkpoint event, not cumulative degradation.

## E_OVERLOADED source path (lines at HEAD)

**EXEC** `src/core/shard.cpp:111`: `emits_in_flight() > kEmitBacklog`, 1,024 (`shard.h:357`).
- Resource: transfer follow-ups ready or waiting for durability (`queue_emits`, `shard.cpp:1803`).
- Per shard and instantaneous, with no time averaging, so a short stall trips it at low average load.
  Measured: about 100 ms at 2.18M.
- Checked before the op is looked up, so **reads are refused too**, though they need no WAL.
- Relationship to checkpointing is indirect: anything delaying WAL durability grows `emit_wait_`.

**INDEX** `shard.cpp:822`: `sri_send` fails when ring (1,024) and overflow (1,024, per destination,
`snc_server.cpp:185-186`) are both full. **A different resource from EXEC.** They share only the
status code.

Other sources are unused here: `:967`, `:1006` need forwarding (0.0% forwarded), `:2641` and
`engine.cpp:1065` are slot moves.

**Intended or limitation:** intended and documented (`docs/TRANSFERS.md:60`, comment at `shard.cpp:109`).
`docs/HEALTH_DECOUPLING.md` already lists `begin_checkpoint` as O(map). I found no document promising
checkpoints are paced or have bounded latency impact, which is why this is not called a defect. Two
points remain for the reviewer: refusing reads, and running checkpoint and WAL unpaced on one device.
Threshold unchanged.

## Calibration semantics

Verified: `total_ops_s` = (served EXEC + INDEX) / window.
Exact check on the pilot: 248,902,944 + 13,100,595 = 262,003,539 served; plus 541 refused =
262,004,080 = histogram n. So refusals do **not** inflate P0; they are counted in latency histograms
but not throughput. `overloaded` counts the whole run including warm-up, as `sustained()` documents.

P0 is a valid **time-averaged throughput including one checkpoint**: about 95% of the window ran near 4.5M, and the phase table reproduces it to within 1% (about 4.33M).
Open-loop achieved rates flatten at 4.2-4.3M, which corroborates it. It is not a refusal-free or
stall-tolerant capacity. The gap of 4.37M vs 436k is real: closed loop slows during a stall (`waiters`
pinned at 4,096 in `fdatasync`), and the 541 refusals are its small residue, 0-718 across later reps.
Open loop keeps offering.

Grid effect: 5% steps of P0 put 218k and 436k on either side of the real limit, so no point exists at
or below 0.70 x S. The harness aborted correctly. Any change to this is a preregistration decision;
none was made.

## Open-loop scheduler sanity check

| Pilot run | Offered / achieved | Lateness p50 / p99 / max | Late >1 ms | End backlog |
|---|---|---|---|---|
| 218,323 r0 | 218,323 / 218,323 | 400 ns / 3.2 us / 0.13 ms | 0 | 0 |
| 436,646 r0 | 436,646 / 436,446 | 440 ns / 4.9 us / 26.8 ms | 59,942 | 1 |

Scheduled counts match the rate. `client_limited_suspect` is false: confirmed.

The first report could not order refusals and lateness. At 2.18M it now can, and **lateness comes first
by one bucket in all 3 T-w30 reps** but is not a generator fault: `waiters` climbs to 8,192 by the
refusal bucket, so the generator runs out of slots because the server stalled. Lateness is zero between
stalls: in r0 the three stall buckets sum to exactly the run's `late_over_1ms` (189,964). Not
verified at 436k, as no clean time-resolved run exists there.

## Storage and WAL observations

Device: Samsung SAS SSD, ext4 ordered, `write_cache` write back, mq-deadline, `wbt_lat_usec` 2000,
0 grown defects; no I/O errors in `dmesg`. WAL: O_DIRECT + RWF_DSYNC, 64 MiB preallocated segments.
All read-only; nothing changed.

**The no-checkpoint stall is one-off, not periodic.** The 130 s no-checkpoint run has exactly one
event in its disk stats (first diagnosis's harness, await threshold 0.25 ms at 100 ms sampling). Each
pushes real excess data: 61-65 MB over baseline in T-w30, 34-36 MB in T-closed. Device await goes to
1.8-4.2 ms. The earlier report's "waiters spikes every 4-8 s" do not appear at that scale here and were
probably its RESOURCES polling.

**Suspect, not established:** `net_bench` writes the `--ids` file (5M x 16 B = 80 MB) buffered, without
fsync, onto the WAL's device right before the run (`net_bench.cpp:672`), and the pilot did too. Size and
one-off behaviour fit. **Timing does not fully fit:** `vm.dirty_expire_centisecs` = 3000 predicts 30-35 s,
but events came at 26.8-34.0 s in T-w30 and 2-7 s in T-closed and the first diagnosis. Filesystem
metadata writeback after large allocations is the main alternative.

Effect if real: the pilot's sweeps carry this stall too, which could add refusals. At 436k with
a 100 ms stall, the backlog would be far below 1,024, so it should not change the transition. Unverified.

## Minimal proposed next experiment (needs approval; not run)

1. **Isolate the one-off stall.** Pilot build, 2,183,233 w30, `--ids` off `sda` (e.g. `/dev/shm`),
   3 reps, about 4 minutes. No SSD or filesystem change.
   - Stall gone: BENCHMARK DEFECT. Fix the harness, add a test, preregister.
   - Stall remains: not the ids file; look at filesystem metadata.
2. **Decide G0 and G2.** They assume zero refusals in w30, which step 1 may show was never true of this
   harness. Changing or keeping them is your or the reviewer's decision; I have not changed them.
3. **Run the preregistered causal cells** (436,646 with and without checkpoint; 2,183,233 w60; T arm,
   3 reps, about 15 minutes). This confirms or refutes the 436k mechanism open-loop. Expect refusals only
   during `fdatasync` and none without a checkpoint.

## If confirmed: falsification evidence

With 5M objects, 8 shards and WAL and checkpoint on one SSD, Profile A cannot run refusal-free at about
10% of its closed-loop peak, because each checkpoint cuts durable capacity about 90% for about 1 s and
trips backpressure. Not to be optimised away on the strength of this result alone.

## Corrections to the first report

- Mechanism step on `fdatasync`, listed as not established: now measured (closed loop).
- "Pilot 30s: 0 refusals" is not repeatable; see above.
- `account.cpp` is at `src/types/account.cpp`. Shard line numbers moved slightly with telemetry.

## Compliance

No full pilot rerun. No SNC load of any kind in this revision. No competitor testing, no Profile C. No
change to f, sustained rule, grid, thresholds, engine, benchmark, or storage configuration. `sudo` was
used only for `dmesg`, `tune2fs -l` and `smartctl -a`. Nothing was committed when this revision was written.

## Recovery check, 2026-11-07

**Void runs: none.** All 12 proof cells (B and T, closed and w30, r0-r2) are complete: each has a client
log with its run section and a final total, a server log ending in its totals line, and `times.json`
with `run_end`; every `.gz` passes `gzip -t`; `summary.json` has 12 entries. The causal cells never ran,
because the gates stopped the driver. No partial result files were found.

**No reboot interrupted this work.** The current boot began 2026-11-06 17:51 UTC, before the pilot and
all diagnostics, and was still running at this check. The boot before it ended with `sudo shutdown -h now`
at 13:05. No run was in progress at either point.
