# CP-1A R640 pilot abort: diagnosis report

Evidence: pilot `r640-pilot-cp1a` at 18373583. Diagnostics: this directory, run once each from
`diag_cp1a.py`, analysed by `an.py`. Build: `build-pilot` from 51a5cb6, unchanged.

## Classification: UNRESOLVED (narrowed)

- The checkpoint is strongly implicated, and the mechanism below fits the evidence.
- No benchmark defect found in counting, the open-loop schedule or the abort logic.
- Choosing between ENGINE DEFECT and EXPECTED ARCHITECTURAL LIMIT needs a clean measurement and a
  Codex judgement. No fix was made and none is proposed yet.

## Major caveat: the diagnostic telemetry perturbed the system

| Measure | Pilot (no telemetry) | Diagnostic (telemetry) |
|---|---|---|
| Closed loop, depth 512 | 4,366,466 ops/s | 1,365,190 ops/s |
| 2.18M offered, 30s window, no checkpoint: p99 | 1.05 ms | 46 ms |
| Same run: EXEC / INDEX refusals | 0 / 0 | 1,611 / 11,625 |
| 2.18M offered, 60s window: p99 | 268 ms | 1,242 ms |
| Same run: EXEC refusals | 767,464 | 95,938 |
| 436k offered: p99 | 1.5-2.1 ms | 21-30 ms |

The likely cause is the 100 ms admin `RESOURCES` poll: `Engine::stats` and `replication_status`
submit work to every shard thread and wait (`engine.cpp:1371`, `:1346`). This was not isolated from
`--report-ms`, `--admin-port`, iostat or pidstat. It was a flaw in this harness.

Use the diagnostic runs for event order and same-harness A/B only. Do not use their absolute
latency, throughput or refusal counts as SNC results. Each cell ran once.

## Raw-log timeline

Times are seconds after `run_start`. Load takes about 6s. The first checkpoint starts near 46s in
every cell (server uptime 60s). Pilot windows run from 10s to 10s + window.

| Run | Window end | Checkpoints in run | Result |
|---|---|---|---|
| Pilot 30s, 2.18M | 40s | 0 | 0 refusals, p99 1.05 ms, p99.9 37.7 ms |
| Pilot 60s, 2.18M | 70s | 1 (3.10s) | 767,464 EXEC + 24,796 INDEX |
| Pilot 120s, 2.18M | 130s | 2 | 1,631,112 + 107,664 |
| Pilot 240s, 2.18M | 250s | 4 | 5,267,176 + 276,630 |
| Pilot 218k, 3 reps | 70s | 1 each (2.3s) | 0 refusals, p99.9 45-48 ms |
| Pilot 436k, 3 reps | 70s | 1 each (2.5s) | 598-23,726 EXEC, 0 INDEX |

Pilot EXEC refusals per checkpoint at 2.18M: about 767k, 815k, 1,317k. The pilot logs have no time
series, so they show correlation only.

Inside a checkpoint (diagnostic, sums over 8 shards):

| Cell | `ckpt_running` | Backlog spike | Peak `emits_in_flight` | Peak `wal_undurable` |
|---|---|---|---|---|
| 2183233, w60 | 45.8-49.0 | 47.8-49.0 | 6,531 | 10,326 |
| 2183233, w130 (1st) | 46.0-49.3 | 48.0-49.3 | 6,105 | 9,871 |
| 2183233, w130 (2nd) | 109.2-112.7 | 111.2-112.4 | 6,250 | 10,205 |
| 436646 | 45.8-48.4 | 48.1-48.4 | 7,507 | 12,426 |
| 218323 | 45.9-48.4 | 48.0-48.4 | 3,621 | 6,738 |

The spike comes in the last 0.3-1.2s of the checkpoint and clears within about 0.1s of its end.

## First overload transition

- Across the grid, the first failing point is 436,646 ops/s. All 3 pilot reps refused, while 218,323
  did not.
- Within a run, refusals were **not timestamped** in any log. The in-run transition is inferred from
  the backlog spike, not observed.
- The gate is per shard at 1,024, but telemetry only gives the sum across shards (8 x 1,024 = 8,192).
  The 436k peak of 7,507 averages 938 per shard, so uneven shards could cross it. The 218k peak of
  3,621 averages 453. This fits the grid transition but was not measured per shard.

## Checkpoint relationship

| Same-harness A/B | With checkpoint | No checkpoint |
|---|---|---|
| 436,646: EXEC refusals | 26,770 | **0** |
| 2,183,233, w130: EXEC refusals | 150,748 | 1,799 |
| 2,183,233, w130: INDEX refusals | 67,238 | 37,796 |
| 2,183,233, w130: p99 | 1,275 ms | 47 ms |

Correlation is established for EXEC refusals. **The checkpoint is not the only source:** with no
checkpoint, 2.18M still refused under telemetry, and INDEX refusals barely depend on the checkpoint.
Whether that survives without telemetry is unknown, though the pilot's 30s run suggests it may not.

Separate unexplained stall: `waiters` spikes recur without a checkpoint, about every 8s at 436k and
every 4s in the closed-loop run. The pilot's 30s run also has p99.9 of 37.7 ms with no checkpoint.
The cause is not identified. It may be telemetry, or something else such as WAL segment rotation
(untested).

## E_OVERLOADED source path (code reading)

**EXEC:** `shard.cpp:110`, `if (emits_in_flight() > kEmitBacklog)`, with `kEmitBacklog = 1024`
(`shard.h:357`).
- **Resource:** `emit_ready_count_ + emit_wait_.size()`, meaning transfer follow-ups queued or
  waiting for durability.
- **Scope:** per shard, instantaneous.
- **Applies to:** every client `execute`, so reads are refused too, though they need no WAL.
- **Other sources:** `shard.cpp:954` and `:993` need forwarding, and `route=owner` shows 0.0%
  forwarded. `shard.cpp:2625` and `engine.cpp:1065` are slot-move and seal/stage paths not used here.

**INDEX:** `shard.cpp:810`. A gather's `sri_send` fails when a ring and its overflow are both full
(`runtime.cpp`, `overflow_limit`; `ring_slots = 1024`). **It is a different resource from EXEC.** The
report-only `overflow` gauge reached 2,000-12,000 at 2.18M and stayed 0 at 436k and below, matching 0
INDEX refusals at 436k. The `overflow_limit` value was not checked.

**Relationship to checkpointing:** there is no direct link in the code. Follow-ups to another shard
wait in `emit_wait_` until durable (`queue_emits`), so anything that delays WAL durability grows the
backlog.

**Why my earlier bound was wrong:** I first estimated about 410 follow-ups per shard from client
slots. A transfer chains credit, settle and forget follow-ups (`account.cpp:196-301`), and after the
client's answer these are no longer limited by client slots.

**Bursts at low average load:** yes. The gate has no time averaging, so a short durability stall
trips it regardless of average rate.

**Intended or limitation:** the gate is intended backpressure, per the comment at `shard.cpp:108`
and `docs/TRANSFERS.md`. Its size relative to a storage stall, and refusing reads, are open questions
for Codex. No threshold was changed.

## Calibration semantics

Verified in `net_bench.cpp`:
- `total ops/s` counts served operations only. That includes `E_APP` (no funds) as served and
  excludes refusals, so 541 refusals out of 262M do not inflate P0.
- Refusals do count in the latency histograms (`on_run`), which can pull latency percentiles down in
  refusing runs.
- `overloaded` counts the whole run, including warm-up and drain.

Why 4.37M and 436k differ: they measure different things.
- Closed loop self-throttles. With 4,096 slots, a durability stall stops new work, so little backlog
  builds. Open loop keeps offering work through the stall, with 8,192 slots, and follow-ups continue
  after each answer.
- P0 is a sound estimate of average throughput ceiling. Open-loop achieved rates flatten at about
  4.2-4.3M, which corroborates it. It is not a refusal-free capacity.

Conclusion: refusals do not make P0 miscounted, and closed-loop self-throttling makes it unsuitable
only as a refusal-free capacity estimate. For grid placement, it is why the 5% grid's first step
(218k) is already the last passing point. Any change to that rule is a preregistration decision and
was not made.

## Open-loop scheduler sanity check

| Pilot run | Issue lateness p50 / p99 / max | Late >1 ms | End backlog |
|---|---|---|---|
| 218,323 r0 | 400 ns / 3.2 us / 0.13 ms | 0 | 0 |
| 436,646 r0 | 440 ns / 4.9 us / 26.8 ms | 59,942 | 1 |

- Offered and scheduled counts match the rate at both (218,323 / 436,646 ops/s; 13,099,380 /
  26,198,760 scheduled in window).
- The generator is on time at p99. The rare lateness at 436k appears when slots are busy, which the
  stall would cause.
- `client_limited_suspect` is false at every rate: confirmed. The diagnostic 436k no-checkpoint cell
  also shows 0 refusals with a 5 ms maximum lateness, so the client can offer that rate.
- **Not verified:** whether each refusal came before lateness. Neither is timestamped. They occur
  in the same stall as far as the data can resolve.

## Storage and WAL observations (diagnostic)

| Phase, 2.18M w60 | MB/s | Write await | Flushes/s | Peak in-flight |
|---|---|---|---|---|
| Before checkpoint | 381 | 0.07 ms | 6,484 | 31 |
| During checkpoint | 603 | 1.58 ms | 4,153 | 198 |
| After checkpoint | 416 | 0.09 ms | 6,073 | 128 |

The same pattern appears at 218k (0.05 to 2.10 ms) and 436k (0.05 to 1.63 ms).

Leading mechanism, partly inferred:
1. Each shard writes its checkpoint part with buffered writes, 1 MiB at a time, then runs
   `fdatasync` at the end (`shard.cpp:1582`, `:1646`). I found no pacing.
2. That data reaches the same SAS SSD as the WAL's `RWF_DSYNC` writes at about 600 MB/s.
3. WAL durability lags, `emit_wait_` grows, and the gate refuses work.

Not established: that the final spike is exactly the `fdatasync` phase, since phases were not
timestamped. Also unproven is that device queueing, rather than the kernel's handling of flushes, is
the delay. Per-thread CPU data is in `*.pidstat.log` and per-second device data in `*.iostat.log`;
**pidstat was not analysed**, so CPU or worker stalls remain unchecked.

## Minimal proposed next experiment (needs approval)

1. Add non-blocking telemetry for diagnostics only: refusals bucketed per 100 ms in `net_bench`, and
   per-shard gauges read without submitting work to shard threads. Log phase timestamps for the
   checkpoint write and `fdatasync`.
2. Rerun only these, 3 reps each, 1 Hz iostat at most: 436,646 with and without a checkpoint;
   2,183,233 w30 and w60; and closed loop without RESOURCES polling to confirm the perturbation.
3. Use the result to decide:
   - refusals only during `fdatasync` backlog: ENGINE DEFECT or ARCHITECTURAL LIMIT, Codex's call;
   - refusals remain with no checkpoint: look for another stall source first.

## Compliance

- No full pilot rerun. The 8 diagnostic cells and one smoke run were the only load, about 15 minutes.
- No competitor testing and no Profile C.
- No threshold, f, sustained-rule, grid or plan change. No engine or benchmark code edited.
- No SSD, filesystem or controller change. Governor and `numa_balancing=0` stayed as set for the
  pilot.
- No-checkpoint cells are diagnostic only and do not replace Profile A.
