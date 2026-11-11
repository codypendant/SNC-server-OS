# S2: paced checkpoint walk (for review)

Slice S2 of [CHECKPOINT_BACKPRESSURE_DESIGN.md](CHECKPOINT_BACKPRESSURE_DESIGN.md)
(DESIGN-R), authorized by [handoff/PR74_S12_Review.txt](handoff/PR74_S12_Review.txt)
sections 12 to 15: checkpoint **walk** pacing only.

**Classification:** REFINE (checkpoint scheduling). **CC clauses touched:**
none changed; CC-1 is the one at risk and is argued in section 6.

## 0. Summary

Before S2, the walk visited a fixed 256 snapshot IDs per shard loop
iteration (`kCheckpointSlice`). That is a count, not a time. On the dev VM
it measures about 0.6 to 0.8 µs per ID, so about 150 to 200 µs of each
iteration. The cost per ID varies with object size and cache state, and
nothing reacts to WAL or follow-up pressure.

S2 makes the walk **time-budgeted**:
- Each iteration the walk visits IDs in batches of 16 (`ckpt_walk_batch`)
  and reads the clock after each batch.
- It stops at the first batch boundary at or after its budget. It always
  visits at least one batch.
- A pure controller sets the budget per shard, between a floor and a
  ceiling, from two signals: WAL flush latency against a learned baseline,
  and the follow-up backlog against its gate.
- It also reports the target completion time and the hard maximum age.
- `ckpt_pacing = false` (`--ckpt-pacing off`) restores the walk exactly as
  it was.

What the walk visits is unchanged. The one-step ID snapshot, writeback,
seal, commit, reclaim, recovery, the WAL and the wire format are all as
before. So is the loop order (section 2).

## 1. As built

| Piece | Where |
|---|---|
| Controller: `reset`, `pressure`, `behind`, `step`, `valid` (pure; no clock, no I/O) | `src/core/ckpt_pacer.h` |
| Paced walk, one controller step per `checkpoint_step` (in every phase) | `Shard::checkpoint_step`, `Shard::pace` (`src/core/shard.cpp`) |
| Config, with validation in `Engine::Engine` (`std::invalid_argument`) | `Engine::Config::ckpt_*` (`include/snc/snc.h`); `src/core/engine.cpp` |
| 17 gauges (section 4) | `include/snc/gauges.h`; `Shard::publish_gauges` |
| HEALTH: `ckpt_state`, `ckpt_past_target`, `ckpt_catchup` (worst shard) | `tools/snc_server.cpp` |
| Server flags `--ckpt-pacing on\|off`, `--ckpt-target-ms N`, `--ckpt-max-age-ms N`; the walk values `--ckpt-walk-batch N`, `--ckpt-walk-min-us N`, `--ckpt-walk-max-us N`, `--ckpt-walk-ceiling-us N`, `--ckpt-walk-step-us N` (added for the R640, PR #98 review section 7; defaults unchanged; the effective values are printed at startup as `ckpt_pacing: ...`) | `tools/snc_server.cpp`, `tests/test_ckpt_cli.py` |
| Test hook `Engine::debug_ckpt_walk_limit(n)`: the walk visits no index ≥ n | `src/core/shard.cpp` |
| Measurement tool for the defaults | `bench/ckpt_walk_cost.cpp` |

**The walk step** (pacing on), once per loop iteration:

```
deadline = now + budget
do { visit up to `batch` snapshot IDs; t = now } while (IDs remain && t < deadline)
```

Pacing off is `visit(256)`, as before S2.

## 2. Not changed, and why

- **Loop order.** DESIGN-R 6.3 proposed moving `checkpoint_step` after
  `app_poll`. The PR #74 review (section 13) bars unrelated loop
  reordering, and S1 kept the order too. The time budget bounds the step
  wherever it runs, so the move is not needed for S2's bound. It stays a
  separate, measurable change for later.
- **The begin snapshot** stays one step: O(live objects), about 50 ms per
  shard at 625,000 objects (DESIGN-R 1.1). S2 does not bound it.
  Bounding it is B2, deferred by DESIGN-R until S6 says otherwise. This is
  the one walk-related monopolization that remains, and it is stated in
  section 7.
- **Writeback.** Chunk writes in flight stay unbounded; the window is S3.
  Staggered sync is S4. I/O priority is S5. Follow-up intake backpressure
  is the pre-S6 slice. No checkpoint I/O mechanism changed.
- **Degraded never changes pacing.** The ceiling is not raised, the
  checkpoint is not abandoned, and no WAL is reclaimed early. Whether a
  breach should raise the ceiling stays DESIGN-R open question 12.

## 3. Controller

`step(config, state, signals)` runs on every `checkpoint_step` of the
checkpoint in progress, so the age and target reports stay current while
the part drains, waits for WAL durability or syncs.

**Pressure.**
- High if the WAL flush EWMA is above `wal_high_pct` of the baseline, or
  the backlog is above `backlog_high_pct` of `kEmitBacklog` (1,024,
  unchanged).
- Low only if both are below their low marks (low < high: hysteresis).
- Hold otherwise.

**The baseline** is the WAL flush EWMA at checkpoint begin, learned while
no checkpoint ran, since a shard's checkpoints never overlap. If no flush
has happened yet, the baseline is unknown and the WAL signal is left out.

**Behind.** A checkpoint is behind when `done / total < elapsed / target`,
the linear schedule to the target. Unfinished work at or past the target
is always behind. DESIGN-R 7.3 said "remaining work at the floor rate would
miss it". The floor rate in IDs per second depends on the iteration rate,
which the shard does not know in advance. The linear schedule is an
observable proxy with the same intent; it is noted as a deviation in
section 10.

**Budget** (at most one change per `update_ns`):
- In catch-up (behind), add `step`, up to `ceiling`, whatever the
  pressure.
- In normal mode, first return within `[min, max]`. Then high halves (not
  below `min`), low adds `step` (not above `max`), and hold keeps the
  budget.
- At begin, the budget resets to `min` and the mode to normal.
- So the budget is always within `[min, ceiling]`. Constant high pressure
  keeps it at `min`, with no oscillation (P1).

**Reports.**
- Past target: `elapsed > target`. It stays set for that checkpoint and
  is counted once.
- Warn: `elapsed ≥ warn_pct × max_age`.
- Degraded: `elapsed ≥ max_age`, counted once.
- Diagnostic only (HL-4): nothing promotes, withdraws or grants on them.

## 4. Observability

Gauges, per shard:

| Gauge | Meaning |
|---|---|
| `ckpt_pacing` | 1 when time-budgeted |
| `ckpt_walk_budget_ns` | current walk budget per iteration |
| `ckpt_mode` | 0 normal, 1 catch-up |
| `ckpt_pressure` | 0 low, 1 hold, 2 high |
| `ckpt_age_state` | 0 ok, 1 warn, 2 degraded |
| `ckpt_past_target` | the checkpoint in progress passed its target |
| `ckpt_past_target_count`, `ckpt_degraded_count` | since start, once per checkpoint each |
| `ckpt_target_ms`, `ckpt_max_age_ms` | configured |
| `ckpt_walk_total`, `ckpt_walk_done` | progress through the snapshot |
| `ckpt_walk_steps`, `ckpt_catchup_steps` | iterations that walked; of those, in catch-up |
| `ckpt_step_max_ns`, `ckpt_step_max_objects` | longest step, most IDs in one step |
| `ckpt_wal_baseline_us` | the learned baseline |

- Mode, pressure, age state, past target and budget describe the
  checkpoint in progress and read 0 when idle.
- Walk and step figures cover the one in progress or the last one.

With pacing off, `ckpt_walk_budget_ns` and `ckpt_mode` read 0 (nothing
applies them) and catch-up steps are not counted. Pressure, age, target
and the step figures are still reported.

The controller runs either way, at a cost of a few clock reads and integer
operations per iteration. A budget, age or target change publishes at
once. A mode change alone waits for the regular 50 µs publish, because
near the schedule line the mode can flip every iteration.

**HEALTH** adds the worst shard's `ckpt_state=ok|warn|degraded`, plus
`ckpt_past_target` and `ckpt_catchup` (any shard). The S0 phase and age
keys are unchanged.

Diagnosing a stall with these gauges:
- An age climbing while `ckpt_walk_done` stands still means the walk is
  stuck.
- `ckpt_phase` names where it sits.
- `ckpt_pressure` and `wal_flush_ewma_us` against the baseline say
  whether pacing is holding it back.
- `committed_start_lsn` and `wal_replay_records` (S0) give the WAL a
  recovery would replay.

## 5. Values: hypotheses, with what they were derived from

Every value is configurable, and the deterministic tests set their own.
The defaults are provisional, as the review requires (section 14). None of
DESIGN-R's withdrawn placeholders is used as a default without a
measurement behind it.

**Measured on the dev VM** (`bench/ckpt_walk_cost`; 1,000,000 accounts on
one shard, three checkpoints per mode; evidence `walk-cost.txt`):
- Walk cost: 0.6 to 0.8 µs per ID, measured as one unpaced step over all
  1,000,000 IDs (620 to 825 ms).
- The 256-ID step before S2: about 150 to 200 µs per iteration.
- Longest single steps at every budget: 0.4 to 40 ms. These are VM
  preemption, not walk work, since the object count per step stays
  exact. A time bound per step can therefore only be stated net of
  scheduling.

| Value | Default | Basis |
|---|---|---|
| `ckpt_walk_batch` | 16 IDs | about 10 to 13 µs between clock reads: the overshoot past a budget |
| `ckpt_walk_min_ns` | 20 µs | about two batches; the floor below that is the batch itself |
| `ckpt_walk_max_ns` | 100 µs | about half of the 256-ID step before S2 per iteration, with no pressure |
| `ckpt_walk_ceiling_ns` | 250 µs | catch-up may slightly exceed the step before S2, never more |
| `ckpt_walk_step_ns` | 10 µs | min to max in 8 updates |
| `ckpt_pace_update_ns` | 1 ms | 8 ms from min to max: fast against checkpoints of seconds, slow against one iteration |
| WAL high / low | 200% / 125% | a doubling of flush latency is pressure; 1.25× is noise |
| backlog high / low | 50% / 25% of 1,024 | DESIGN-R 9 names 50% of the gate as the target the backlog should stay under |
| `ckpt_target_ms` | 30,000 | placeholder: needs campaign data on checkpoint duration under load |
| `ckpt_max_age_ms` | 300,000 | placeholder: needs WAL disk budget and replay rate (DESIGN-R 7.3) |
| `ckpt_warn_pct` | 80% | placeholder |

`ckpt_walk_min_ns` = 20 µs equals one of the "20/200 µs" values that
DESIGN-R 7.2 withdrew as a default. It is used here only because the
dev-VM measurement above puts two batches at about that cost. It is not
carried over from revision 1.

**Known risk on faster hardware (independent review M3).** On a machine
where the walk is much cheaper per ID, the 256-ID step before S2 takes
less time. For example, at about 100 ns per ID (R640's snapshot measured
about 80 ns per ID; its walk is unmeasured) it takes about 25 µs. There,
the default normal budget of 100 µs and the 250 µs ceiling would spend
**more** time walking per iteration than the walk before S2, 4 to 10
times more.

That is the opposite of S2's aim, so these walk-budget defaults are
dev-VM values only. They must be re-derived from `bench/ckpt_walk_cost`
on the R640 (or set by flag) before any physical run. The target, maximum
age and thresholds must be preregistered before S6 from S0/S2 gauge
data.

## 6. Correctness mapping (review 15.A)

| Clause | S2 impact | Why it holds | Test anchors |
|---|---|---|---|
| CC-1 coverage | **at risk, unchanged** | Pacing changes *when* each snapshot ID is visited, never *which*. The ID vector is still built in one step at begin. The cursor only increases. The walk ends only when the cursor equals the snapshot size. Every untouched ID is still found by `map_.find` and imaged as before begin. The debug limit only delays visits. | P2 (`ckpt_objects == ckpt_walk_total` at budgets of 1 ns, a time budget, a 60 s budget and pacing off), P4, P5 |
| CC-2 redo completeness | unchanged | WAL format and reclaim-after-commit untouched; a slower checkpoint only retains more WAL | P4 (history at 21 walk positions recovers), P5, P6 (`committed_start_lsn` does not move while the checkpoint is held) |
| CC-3 no future state | unchanged | The C5 check (`lsn_local_durable(max_lsn)` before `fdatasync`) is untouched | P5 `durable_wait`: an applied, never-durable deposit is not in the recovered state |
| CC-4 idempotent redo | unchanged | Version rule in `install` and replay untouched; a later visit only images a newer version | P4, P5 (mutations both ahead of and behind the cursor) |
| CC-5 atomic publication | unchanged | Commit and reclaim untouched; a degraded checkpoint is never abandoned or committed early | P5 (crash at four positions: previous checkpoint used), P6 |

The existing suites (persistence, crash, replication, migration with
checkpoints, B1, sessions) run with pacing **on** by default. This is
DESIGN-R T12.

## 7. Progress argument (review 15.B)

**Claim 1: the walk cannot monopolize a shard.** A shard calls
`checkpoint_step` once per loop iteration (`Shard::poll`). One walk step:
- stops at the first batch boundary at or after `now + budget`;
- has a budget of at most `walk_ceiling_ns` (section 3);
- so its walk work is at most the ceiling plus one batch: with the
  defaults, 250 µs + about 13 µs on the dev VM.

Every other stage of the loop runs in each iteration: the ring drain,
control tasks, I/O completions, `Shard::poll`'s other work, `app_poll` and
the batch flush.

P2 proves the count form exactly: with the budget pinned at 1 ns, every
step visits exactly one batch. With pacing off it visits 256, and with a
60 s budget it visits everything in one step.

What is **not** bounded by S2:
- the one-step begin snapshot (section 2);
- time lost to preemption (section 5);
- the per-iteration time foreground work takes. S1 bounds its resources,
  not its time.

**Claim 2: the walk cannot stop making progress forever.** While a
checkpoint is active:
- `Shard::poll` calls `checkpoint_step` every iteration and reports itself
  busy, so the loop does not sleep.
- Each walk step visits at least `min(batch, remaining)` IDs: the
  `do … while` visits one batch before it reads the clock.
- So the walk ends within `⌈total / batch⌉` iterations of its first step,
  whatever the budget, pressure or foreground load. P2 shows the step
  count is exactly that at a 1 ns budget.
- Foreground load can make iterations longer, but cannot stop them. S1
  admission bounds the work each iteration admits: the ring drain batch,
  epoll events per poll, and the waiter, WAL-buffer and pending budgets.
- After the walk:
  - drain waits on writes in flight, whose completions are reaped every
    iteration;
  - durable-wait waits for local WAL durability of `max_lsn`, which the
    WAL's group commit advances every iteration;
  - sync is one `fdatasync`.

A device that stops completing I/O stops all of these, and the WAL with
them. The age reports exist for that case: P6 shows warn and degraded
appear while the checkpoint is held, without abandoning it. No mode turns
pacing off, and no mode lets the walk stop: the budget never drops below
`walk_min_ns ≥ 1`, and the floor batch does not depend on the budget.

**Completion within the target under load** is not a guarantee. If the
target is missed even at the ceiling, the checkpoint takes longer. That is
counted (`past_target`) and reported, up to the hard maximum.

## 8. Tests (`tests/test_s2_pacing.cpp`)

| Test | Proves | DESIGN-R / review |
|---|---|---|
| P1 | Controller traces, step by step, all exact:<br>• floor at reset; +10 per update to max under low pressure<br>• halve to the floor under high pressure, and stay there<br>• the hold band in any order; the low and high edges<br>• unknown baseline; one change per update interval<br>• catch-up to the ceiling and not above, even under high pressure; back within max after it<br>• past target, warn and degraded at their exact boundaries; no target or max<br>• validation; a 200,000-step seeded fuzz of the bounds | T15 |
| P2 | Step bound in IDs:<br>• 1 ns budget → exactly one batch per step (16, 64), `⌈total/batch⌉` steps<br>• off → 256 per step<br>• 60 s budget → one step<br>• every snapshot ID imaged in each mode | T7, 15.B, CC-1 |
| P3 | 100,000 IDs at one batch per iteration: about 3,000 reads and 3,000 applied deposits answered while the walk was in progress, and it advanced across them | 15.C |
| P4 | Two shards; the walk paced at 1 ns and held at 21 positions. 315 to 320 durable deposits, creates and deletes landed ahead of and behind the cursor. The checkpoint commits, and recovery from it is exact | T1, 15.D |
| P5 | A child process crashes with its checkpoint at the first ID, the middle, the last ID, and in DURABLE_WAIT (an applied, never-durable deposit):<br>• recovery uses the previous checkpoint and gives the exact acknowledged model<br>• then a paced checkpoint commits and recovers exactly | T2 (walk positions), 15.E |
| P6 | Held walk; target 40 ms, max 160 ms, warn 50%:<br>• past target, warn and degraded each reported no earlier than its threshold<br>• catch-up raises the budget to the ceiling and no higher<br>• foreground deposits stay durable<br>• the committed start LSN does not move<br>• released, it commits; the reports clear; the counts went up by one each | T16, 15.F |
| P7 | Inconsistent config refused at construction | |
| P8 | End to end with the real backlog signal: shard 0 with 601 follow-ups held at the floor (high); shard 1 with none rose to the max (low) | 7.1 |

- HEALTH and GAUGES keys are checked by `tests/test_gauges_admin.py`
  against a live server.
- Negative checks, each expected to fail, are in the evidence (section 9).

## 9. Validation and evidence

`bench/results/s2-validation-devvm-20261108/` (README lists every run in
order).

**Independent review.** It returned APPROVE WITH NITS, with findings M1-M3
and L1-L6. All are resolved before the headline runs:

| Finding | Resolution |
|---|---|
| M1: evidence not yet committed | committed |
| M2: `snc_server` aborted on a target above the maximum age | the flags are checked; it exits with code 2 |
| M3: defaults on faster hardware | documented in section 5; the defaults are dev-VM only |
| L1: unbounded config values | `valid()` caps the ceiling, step and update interval at 60 s and requires `warn_pct ≥ 1`; P1 and P7 cover both |
| L2: gauge churn on mode flips | a mode change no longer forces a publish |
| L3: pacing off reported a controller that was not acting | budget and mode read 0 when off |
| L4: names array not checked | `static_assert` that every gauge has a name |
| L5: baseline limits | recorded in section 10 |
| L6: comments | fixed |

## 10. Deviations from DESIGN-R, and open items

1. **Catch-up rule.** It uses the linear schedule to the target, not "the
   remaining work at the floor rate" (section 3).
2. **The baseline** is the EWMA at begin, not a separately maintained
   EWMA.
   - After a slow checkpoint, the next baseline can start high and decays
     with flushes.
   - It is whole microseconds with α = 1/8, so a baseline of a few
     microseconds moves the ratio in coarse steps.
   - It cannot tell foreground load from checkpoint interference.

   S3-D or S6 should look at all three.
3. **Loop order unchanged** (section 2).
4. **Not added:**
   - the window (`ckpt_window`, S3);
   - `wal_retained_bytes`. S0's `committed_start_lsn` and
     `wal_replay_records` already give the WAL a recovery would replay.
5. **The pressure signal is wired, but no default is tuned on real
   pressure.** P8 shows the backlog path end to end. The WAL path is
   covered by P1 only, because injecting device latency deterministically
   on the dev VM is not available. S3-D or S6 should exercise it.
6. **Open:**
   - whether a degraded checkpoint should raise the ceiling (DESIGN-R
     question 12);
   - the target and maximum age defaults, which wait for campaign data.
