# Engine lifecycle (S3.2)

Status: **built, for review.** Work order: `docs/handoff/PR78_S31_Review.txt`
sections 10-15 and 18. This document builds on S3.1 (`docs/S3_WRITEBACK.md`
section 10): S3.1 made `checkpoint()` finish before `stop()` proceeds. S3.2
applies the same contract to every Engine entry point that submits work
to the shards.

## 1. The defect class

Before S3.2, the sequence was:
1. A caller saw the engine running.
2. `stop()` stopped the workers.
3. The caller then submitted a shard task and waited on a latch that no
   worker would ever count down.

S3.1 closed this for `checkpoint()` only. Every other public method that
calls `run_on_all`, `submit`, `on_shard` or `on_shard_steps` could still
hang the same way.

## 2. The state machine

```
CONSTRUCTED --start()--> STARTING --> RUNNING --stop()--> STOPPING --> STOPPED
```

The state is explicit: it is `Engine::life_`, and `Engine::lifecycle()`
reads it.

| Call | In state | Result |
|---|---|---|
| `start()` | CONSTRUCTED | moves to STARTING, then RUNNING. If it throws: RUNNING when the runtime came up (it must still be stopped), else CONSTRUCTED |
| `start()` | STARTING (another thread) | waits, then returns if RUNNING |
| `start()` | RUNNING | returns at once |
| `start()` | STOPPING, STOPPED | throws `std::logic_error`: restart is not supported. Nothing restarted an Engine before S3.2 (all callers checked) |
| `stop()` | CONSTRUCTED | no-op |
| `stop()` | STARTING | waits for the start, then stops |
| `stop()` | RUNNING | moves to STOPPING, waits for operations to leave, stops the runtime, moves to STOPPED |
| `stop()` | STOPPING (another thread) | waits until STOPPED |
| `stop()` | STOPPED | returns |
| `stop()` | from inside an operation on the same thread | aborts with a message (it would wait for itself) |

Destruction runs `~Engine`, which is `stop()` followed by the destructor.
Calling the Engine once its destructor has begun is not supported
(S3.1, T16).

## 3. The operation permit

`Engine::Op` (`src/core/engine.cpp`) is an RAII permit.

- **Granted:** only while RUNNING. Granting one increments `active_ops_`
  under `life_mu_`.
- **Refused:** in every other state. The method then returns its "not
  running" result at once and touches no shard (section 5).
- **Stop:** `stop()` sets STOPPING under the same mutex, so no permit is
  granted after that point. It then waits on `life_cv_` until
  `active_ops_ == 0`, and only then stops the runtime.
- **Scope:** a permit is held for the whole public call. That covers every
  shard task, every latch wait, the waits between steps (`wait_lsn`
  polling) and the units of a stage chain. A multi-step operation is
  therefore never cut off half-way.

This is a counting gate, not a lock around the calls. Operations do not
serialize each other. The only thing that waits on them is `stop()`
(review section 11).

**Nested calls** (review 11C). The thread-local `tl_op_engine` records
which engine this thread holds a permit for.
- A permit taken inside another one for the same engine is granted
  without counting again and without checking the state. The outer permit
  already keeps `stop()` waiting.
- So `seal_slot -> wait_lsn -> on_shard -> slot_report`,
  `promote -> update_placement / release_emits -> finish_barrier`, and
  recovery's `checkpoint()` / `stats()` neither self-deadlock nor get
  refused half-way.
- None of this relies on recursive locking. The permit holds no lock while
  the operation runs.
- A permit for engine B inside an operation of engine A saves and restores
  A's mark, in stack order.

**Lifecycle owner.** `start()` and `stop()` mark their own thread
(`OwnerScope`), so their internal shard calls pass the gate:
- during STARTING, no other operation can enter;
- during the shutdown steps, all operations have already left.

**Structural enforcement.** Every shard submission from an Engine method
goes through `run_on_all_`, `submit_`, `on_shard` or `on_shard_steps`.
Each of these calls `require_op()`, which aborts with a message unless
this thread holds a permit or is the lifecycle owner. The only direct
`rt_->` calls left are:
- `start` and `stop` of the runtime;
- the shard-thread resubmission inside `on_shard_steps`, which is covered
  by its caller's permit.

A method that is not gated therefore fails loudly in any test that
reaches it, instead of racing.

**Engine threads.** `stop()` tells the placement watcher and the seed
server to finish before it waits. The watcher's next refresh is refused
by the gate in any case.

## 4. Classification (review section 13)

**Category 2: these take the permit.** Each one submits shard work or
waits on it.

| Method | Refused result |
|---|---|
| `checkpoint()` | report with seq 0 |
| `release_emits()` | no-op |
| `wait_emits()` | false |
| `promote()` | throws `std::logic_error` ("not a running standby"), as before for not-running |
| `update_placement()` | false |
| `refresh_placement()` | false (gated in `update_placement`; the fetch itself touches no shard) |
| `slot_report()` | `SlotReport{}` |
| `slot_states()` | empty |
| `seal_slot`, `unseal_slot`, `export_slot`, `slot_objects`, `stage_slot` | `E_SHUTTING_DOWN` |
| `replication_status()` | counts all zero (shards set) |
| `stats()` | once STOPPED, the values each shard left at stop; before start, while STARTING (from another thread) or while STOPPING, zeros (the workers write the final values during STOPPING; TSan found the read racing them, section 8); never blocks |
| `for_each_object`, `for_each_served_object` | nothing visited |

Private helpers (`wait_lsn`, `finish_barrier`, `reseal_from`, `on_shard*`)
run only inside a permit. `require_op()` enforces this.

**Category 1: these do not take the permit.** They read atomics or
Engine-owned state, and stay safe in every state:
- `lifecycle`, `fenced`, `replicating`, `withdraw` (an atomic store);
- `epoch_seen`, `cluster`, `slot_owner`, `space_uuid`, `shard_count`,
  `recovery`;
- `gauges`, `checkpoint_gauges` (Engine-owned blocks, a seqlock read);
- `last_stage_report`, `allow_degrade`, every `debug_*`;
- `term()` and `lease_block()`. These read superblock fields that are
  written only by control operations. That pre-existing contract is
  unchanged here.

**Not gated, by design: `runtime()`.** This is the raw data-plane handle:
the harness's op windows and the network front ends submit through it.
Using it while `stop()` runs is the caller's responsibility, as before.
Wrapping it would mean an API change outside S3.2's scope.

## 5. What a refusal guarantees

A refused call returns at once, submits nothing and waits on nothing. In
particular it never waits for STOPPED. A shard thread that calls in
during STOPPING (for example from an application poll hook) must not
block, because the shards are still being drained and are waiting for it.

## 6. Unchanged

None of the following changed:
- checkpoint semantics (S3.1 contract: finish, then stop);
- persistence format;
- admission;
- placement semantics;
- S4;
- follow-up intake.

## 7. Tests (`tests/test_lifecycle.cpp`, review section 14)

Each operation is parked after it enters (`Engine::debug_park_ops`), so
`stop()` runs while the operation is known to be inside.

| Test | Shows |
|---|---|
| L1 `stats` | stop moves to STOPPING and waits. The operation completes on live shards, then stop returns |
| L2 `slot_report` (`on_shard`) | the same; the result is the live slot (serving) |
| L3 final stage chain (`on_shard_steps`) | stop waits while the chain is held between units. The chain completes OK, and after reopening, its 300 objects are durable |
| L4 `replication_status` | the same pattern as L1, for a task on every shard |
| L5 `update_placement` | the same pattern; the new epoch is adopted before the runtime stops |
| L6 | during STOPPING (inside L1, L2, L4, L5) and again at STOPPED: every category-2 method is refused with its defined result, all within 2 s in total (a `wait_emits(5)` that entered would take 5 s) |
| L7 | a second `start()` waits for the one in progress. `stop()` waits for a start, then stops. Restart is refused. `stop()` before start is a no-op. STARTING admits no operation from another thread |
| L8 | 6 threads issue operations while another thread stops. Every thread returns. Run under TSan as part of the serial TSan suite |

The 300 ms probes are evidence that something has not happened yet; they
are not a safety mechanism. Every wait after a release is bounded, so a
defect fails a CHECK instead of hanging.

The negative mutations, the validation runs and the review findings are
recorded in `bench/results/s32-validation-devvm-20261108/`.
