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
                            |                                  ^
                            +--(start throws)--> FAILED --stop()+
```

The state is explicit: it is `Engine::life_`, and `Engine::lifecycle()`
reads it.

| Call | In state | Result |
|---|---|---|
| `start()` | CONSTRUCTED | moves to STARTING, then RUNNING. If it throws: FAILED (section 2a) |
| `start()` | STARTING (another thread) | waits, then returns if RUNNING |
| `start()` | RUNNING | returns at once |
| `start()` | STOPPING, STOPPED, FAILED | throws `std::logic_error`: restart is not supported. Nothing restarted an Engine before S3.2, and nothing retried a failed start (all callers checked) |
| `stop()` | CONSTRUCTED | no-op |
| `stop()` | STARTING | waits for the start, then stops |
| `stop()` | RUNNING | moves to STOPPING, waits for operations to leave, stops the runtime, moves to STOPPED |
| `stop()` | STOPPING (another thread) | waits until STOPPED |
| `stop()` | STOPPED | returns |
| `stop()` | FAILED | tears down what the failed start brought up (threads, and the runtime if it started), moves to STOPPED |
| `stop()` | from inside an operation of this engine on the same thread (at any nesting depth) | aborts with a message (it would wait for itself) |
| `stop()` | from a shard thread (an application hook) | aborts with a message (an operation waiting on that shard would wait for it, and it for the operation) |

**2a. FAILED.** Before the review, a start that threw left the engine
RUNNING when the runtime had come up, or CONSTRUCTED otherwise. RUNNING
admitted operations on a half-started engine (recovered shards, follow-ups
still held, not serving); CONSTRUCTED allowed a second start on in-memory
state the first attempt had already changed (superblock, leases). FAILED
admits no operation and no second start. Only `stop()` or the destructor
is accepted, and either tears down what came up. Callers checked:
`snc_server` exits on a failed start; `test_persistence` checks the throw
only.

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

**Nested calls** (review 11C). The thread-local `tl_held` is a small stack
of the engines this thread holds a permit (or lifecycle step) for,
innermost last.
- A permit taken while this thread already holds one for the same engine,
  at any depth, is granted without counting again and without checking
  the state. The outer permit already keeps `stop()` waiting. This holds
  across another engine in between (A -> B -> A, L10); a single-slot mark,
  the first version, lost it (review finding 4).
- So `seal_slot -> wait_lsn -> on_shard -> slot_report`,
  `promote -> update_placement / release_emits -> finish_barrier`, and
  recovery's `checkpoint()` / `stats()` neither self-deadlock nor get
  refused half-way.
- None of this relies on recursive locking. The permit holds no lock while
  the operation runs.
- Permits and `OwnerScope` are RAII, so the stack unwinds in order. More
  than 16 engines deep aborts with a message.

**Lifecycle owner.** `start()` and `stop()` mark their own thread
(`OwnerScope`), so their internal shard calls pass the gate:
- during STARTING, no other operation can enter;
- during the shutdown steps, all operations have already left.

**Structural enforcement.** Every shard submission from an Engine method
goes through `run_on_all_`, `submit_`, `on_shard` or `on_shard_steps`.
Each of these calls `require_op()`, which aborts with a message unless
this thread holds a permit for this engine or is its lifecycle owner. The only direct
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
- `epoch_seen`, `cluster`, `slot_owner`, `shard_count`;
- `space_uuid`, `recovery`: written during STARTING, so safe once
  `start()` has returned (as before; not safe from another thread during a
  start, review finding 7);
- `gauges`, `checkpoint_gauges` (Engine-owned blocks, a seqlock read);
- `last_stage_report`, `allow_degrade`, every `debug_*`;
- `term()` and `lease_block()`. These read superblock fields that are
  written only by control operations. That pre-existing contract is
  unchanged here.

**Not gated, by design: `runtime()`. For the reviewer's decision.** This
is the raw data-plane handle (`NativeRuntime&`, with `submit` and
`run_on_all`). Its callers: the network front ends and `DEBUG STALL` in
`tools/snc_server.cpp`, the test harness's op windows (`tests/harness.h`),
and direct uses in `test_net`, `test_replication`, `test_gauges`,
`test_execute` and `bench/*`.
- A caller that submits through it while another thread runs `stop()`
  can still wait on a latch no worker counts down: the S3.2 defect class
  on the data plane (review finding 1).
- `snc_server` is safe in practice: it stops the admin loop and drains
  the movers before `eng.stop()` (`tools/snc_server.cpp`, the shutdown
  sequence). The tests and benches stop their own threads first.
- Gating it means an API change (a gated handle or a permit type in the
  public API) and touches every data-plane caller, beyond "one generic
  lifecycle gate over the Engine's shard-submitting methods". S3.2 leaves
  it as before and documents it; whether to gate it is the reviewer's
  call.

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
| L9 | a failed start, before the runtime came up (no init) and after (seed port taken, after recovery's checkpoint): FAILED, every category-2 method refused, a second start refused, `stop()` ends STOPPED |
| L10 | on one thread, an A operation calls B, which calls back into A, while A is STOPPING: the inner A call is nested (granted, live result) and `stop()` waits only for the outer one |
| L11 | in child processes: `stop()` from inside an operation, and from a shard thread (a `for_each_object` callback), each abort with their own message (checked in the child's stderr, so a failed CHECK cannot pass for it) |

L3 waits until the stage hold has begun (`debug_stage_pending()`), not a
fixed sleep (review finding 9).

The 300 ms probes are evidence that something has not happened yet; they
are not a safety mechanism. Every wait after a release is bounded, so a
defect fails a CHECK instead of hanging.

The negative mutations, the validation runs and the review findings are
recorded in `bench/results/s32-validation-devvm-20261108/`.

## 8. Validation findings and the review

**TSan, first run (before the review).** A refused `stats()` during
STOPPING read `final_stats` while the workers' `hook_fini` wrote it. The
fix: `final_stats` is read only once STOPPED, which is set under
`life_mu_` after the runtime joined the workers. Log:
`bench/results/s32-validation-devvm-20261108/tsan-test_lifecycle-first-stats-race.log`.

**Independent review** (read-only, of `ab76766`, focused on lifecycle state
and nested locks). What it confirmed: every shard-submitting method holds a
permit; the only direct runtime calls left are its start and stop and the
shard-side resubmit; permits are RAII (released on exceptions); `life_mu_`
is never held across blocking work; the lock order is permit, then
`checkpoint_mu_`, then `seed_->mu`, with no inversion; the `final_stats`
happens-before is real.

| # | Finding | Resolution |
|---|---|---|
| 1 | `runtime()` hands out ungated `submit` / `run_on_all` | not changed: an API change beyond S3.2's scope. Documented in section 4 for the reviewer's decision |
| 2 | a failed start can land in RUNNING (or CONSTRUCTED, allowing a retry on changed state) | fixed: FAILED (section 2a), L9, mutation 6 |
| 3 | negative mutation 3 was caught by the structural abort, not by the abandoned waiter it claimed | fixed: it now removes the checks in `on_shard` and `submit_` too and replaces the permit with an uncounted park; L2's stop-waits probe catches it |
| 4 | the nested mark is single-slot: A -> B -> A is lost, and `A.stop()` there is not caught | fixed: a per-thread stack (section 3), L10, mutation 7 |
| 5 | `stop()` from a shard thread is not detected | fixed: aborts with a message, L11, mutation 8 |
| 6 | `stop()` now waits for long permit holders (`wait_emits(30)`, `wait_lsn` timeouts, `promote`'s settle loop, `finish_barrier`, a start in recovery) | intended: finish-then-stop, as the S3.1 checkpoint contract (PR #78 review section 5). Bounded or deliberate latency, not deadlock |
| 7 | pre-existing data races: `promote` writes `cfg_.standby` while a concurrent `checkpoint()` reads it; `recovery()` / `space_uuid()` race a reader during STARTING | not changed (pre-existing, not lifecycle): the second is now stated in section 4. The first is recorded here for a separate fix: it is a role-change race, and fixing it would touch promotion, outside S3.2's scope |
| 8 | behaviour changes: other threads are refused during STARTING (seal / stage answer `E_SHUTTING_DOWN`, `stats()` zeros); the placement watcher's first refresh can fall in STARTING and waits one poll | accepted and stated. `snc_server` starts its admin loop after `start()` and reads stats after STOPPED |
| 9 | tests narrower than claimed: L3 relied on a 150 ms sleep; no tests for failed start, cross-engine nesting, stop-from-op | fixed: L3 waits on the hold, L9 to L11 added. Nested paths inside one engine (`seal_slot -> wait_lsn`, `promote -> update_placement`) run in the existing suites (`test_migrate`, `test_cluster`, `test_replication`, `test_proof`) under the new gate; a missing nested permit there would abort in `require_op` |

The negative mutations and validation runs: `bench/results/s32-validation-devvm-20261108/README.md`.

