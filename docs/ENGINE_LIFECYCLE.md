# Engine lifecycle (S3.2, S3.3)

Status: **S3.2 (sections 1-8) approved in design and implementation
(`docs/handoff/PR79_S32_Review.txt`); S3.3 (sections 9-11) built, for
review.** Work order: `docs/handoff/PR78_S31_Review.txt`
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
- `term()` and `lease_block()`. In S3.2 these read superblock fields
  with no lock; since S3.3 they read a published copy under a short lock,
  safe from any thread (section 10).

**`runtime()`: removed in S3.3.** The review did not accept the exemption
(PR #79 review section 6). Data-plane access now goes through a counted
lease (section 9); only read-only runtime facts stay available, through
`runtime_view()`.

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
| 1 | `runtime()` hands out ungated `submit` / `run_on_all` | not changed in S3.2; the review rejected the exemption, and S3.3 removed `runtime()` (section 9) |
| 2 | a failed start can land in RUNNING (or CONSTRUCTED, allowing a retry on changed state) | fixed: FAILED (section 2a), L9, mutation 6 |
| 3 | negative mutation 3 was caught by the structural abort, not by the abandoned waiter it claimed | fixed: it now removes the checks in `on_shard` and `submit_` too and replaces the permit with an uncounted park; L2's stop-waits probe catches it |
| 4 | the nested mark is single-slot: A -> B -> A is lost, and `A.stop()` there is not caught | fixed: a per-thread stack (section 3), L10, mutation 7 |
| 5 | `stop()` from a shard thread is not detected | fixed: aborts with a message, L11, mutation 8 |
| 6 | `stop()` now waits for long permit holders (`wait_emits(30)`, `wait_lsn` timeouts, `promote`'s settle loop, `finish_barrier`, a start in recovery) | intended: finish-then-stop, as the S3.1 checkpoint contract (PR #78 review section 5). Bounded or deliberate latency, not deadlock |
| 7 | pre-existing data races: `promote` writes `cfg_.standby` while a concurrent `checkpoint()` reads it; `recovery()` / `space_uuid()` race a reader during STARTING | not changed in S3.2. The second is stated in section 4 and was accepted by the review. The first, and the wider superblock race the review found, were fixed in S3.3 (section 10) |
| 8 | behaviour changes: other threads are refused during STARTING (seal / stage answer `E_SHUTTING_DOWN`, `stats()` zeros); the placement watcher's first refresh can fall in STARTING and waits one poll | accepted and stated. `snc_server` starts its admin loop after `start()` and reads stats after STOPPED |
| 9 | tests narrower than claimed: L3 relied on a 150 ms sleep; no tests for failed start, cross-engine nesting, stop-from-op | fixed: L3 waits on the hold, L9 to L11 added. Nested paths inside one engine (`seal_slot -> wait_lsn`, `promote -> update_placement`) run in the existing suites (`test_migrate`, `test_cluster`, `test_replication`, `test_proof`) under the new gate; a missing nested permit there would abort in `require_op` |

The negative mutations and validation runs: `bench/results/s32-validation-devvm-20261108/README.md`.

## 9. The data-plane lease (S3.3)

Work order: `docs/handoff/PR79_S32_Review.txt` sections 6, 7, 11 and 14.

**The invariant.** No code outside the Engine submits work to its runtime
unless it holds an Engine lifetime token that `stop()` waits on.

**`Engine::DataPlane`.**
- `data_plane()` grants a lease only while RUNNING, and counts it in
  `active_ops_` under `life_mu_`, the same count as the operation permits.
  `stop()` therefore waits for every lease exactly as it waits for
  operations. In every other state, the lease is refused (false).
- `submit` and `run_on_all` on a live lease reach a live worker. Using a
  refused or released lease aborts with a message. `run_on_all` waits for
  every shard, so from a shard thread it would wait for its own: it aborts
  with a message there (review S3).
- The holder keeps the lease until every wait on work it submitted has
  finished, so no waiter is abandoned. Fire-and-forget work (`DEBUG
  STALL`) may release at once: nothing waits on it.
- A lease is movable and not tied to a thread. Shard threads use it, for
  example the harness's op windows resubmitting their pumps. A moved-from
  lease is empty, and move-assignment releases the target's own lease
  first. So the count stays exact, which R checks.
- A thread must not call `stop()` while holding a lease, or `stop()` waits
  for it. Unlike a permit, a lease is not tracked per thread, because it
  can move between threads and a per-thread count would then abort a
  correct caller. So this is not detected as an abort. Instead, a `stop()`
  that has waited 5 s says on stderr how many operations or leases it is
  waiting for (review S4). Every migrated caller releases before its
  `stop()`.

**`runtime_view()`.** An `Engine::RuntimeView` facade with `progress`,
`ring_stats`, `topology` and `shard_count`, and no handle to the runtime
itself. These are reads that submit nothing; their workers exist from
construction. A first version returned `const NativeRuntime&`, which a
`const_cast` could have turned back into a submit path (review N1).

**Callers migrated** (mechanical, no data-plane semantic change):
- `tests/harness.h`, plus the windows and single ops in `test_execute`,
  `test_replication`, `test_net`, `test_gauges` and `test_s1_admission`;
- `bench/stage_stall.cpp`;
- `tools/snc_server.cpp` (`DEBUG STALL`; progress and ring stats through
  the view);
- `bench/snc_bench.cpp` (ring stats and topology through the view).

The network front ends need no lease. They run inside the shards'
application poll hooks, on the shard threads, which exist exactly as long
as the runtime runs.

**Tests (R1-R3, `test_lifecycle R`).**
- R1: a lease is held across `stop()`. `stop()` reaches STOPPING and waits.
  Work submitted through the lease then still runs, and a wait on it (a
  latch, a `run_on_all`) finishes. Only the release lets `stop()` return.
- R2: `data_plane()` during STOPPING is refused.
- R3: `data_plane()` after STOPPED is refused. Submitting through a
  refused lease aborts (checked in a child process for SIGABRT). There is
  no other public path to the runtime; `runtime()` no longer exists.
- The test also checks that move construction and move assignment keep
  the count exact: `stop()` returns, where a miscount would hang it.

## 10. Durable control: promote vs checkpoint (S3.3)

Work order: `docs/handoff/PR79_S32_Review.txt` sections 8-10, 12 and 13.

**The defect.** Both `promote()` and `checkpoint()` take an ordinary permit,
so the S3.2 gate admitted them together.
- `promote()` changed `cfg_.standby` and the superblock's `term`,
  `generation` and lease fields, and wrote the superblock.
- `checkpoint()` read `cfg_.standby`, changed `active_ckpt`, `generation`
  and `next_lease`, and wrote the superblock.
- Overlapped, the two raced on `sb_` and on the role. A checkpoint could
  start as a standby and commit after the promotion: a hybrid matching
  neither serial order.

**Runtime writers of the superblock and the role** (audited):

| Writer | When | Covered by |
|---|---|---|
| `recover()`: superblock load, new leases, lease block, start barrier, role adoption | STARTING | the lifecycle: STARTING admits no operation from another thread |
| `checkpoint()` | RUNNING | `control_mu_`, whole body |
| `promote()` | RUNNING | `control_mu_`, whole body |
| `take_lease_block()` | only from `recover()` | the lifecycle |
| seed fetch (`seed.cpp`) | STARTING, before `sb_` is loaded; writes the file only | the lifecycle |
| the seed server (`serve_seed`) | reads `sb_` | `seed_->mu`, which both writers hold around their `sb_` changes |
| `withdraw()` | atomic `fenced` flag | no superblock or role field |

**The fix.**
- **`control_mu_`** (formerly `checkpoint_mu_`, which serialized checkpoints
  only) is held for the whole of `checkpoint()` and the whole of
  `promote()`, after the permit. They therefore linearize: either the
  checkpoint runs wholly as a standby and then the promotion runs, or the
  promotion runs wholly and then the checkpoint runs as the primary.
- Other operations do not take `control_mu_`, so they are not serialized.
  Inside `promote()`, the nested `update_placement`, `release_emits` and
  `finish_barrier` do not take it either.
- **Lock order:** permit, then `control_mu_`, then `seed_->mu`, then
  `sb_view_mu_`. Nothing takes them in another order, and nothing waits
  for `control_mu_` while holding a later lock.
- **Runtime role.** `role_` is an atomic, PRIMARY or STANDBY. It is set
  from `cfg_.standby` at construction and changed only by `promote()`,
  under `control_mu_`. `cfg_` is no longer changed after construction.
  Every former runtime read of `cfg_.standby` reads the role.
- In `replica_config`, the one read that was inside the primary branch
  now reads the role parameter, which is the same value. So promotion's
  sender configuration is unchanged.
- **`term()` and `lease_block()`** used to read `sb_` with no lock from
  any thread. They now read a view that is copied under `sb_view_mu_`
  after every superblock write (`commit_superblock_`) and after recovery
  loads it.
- **`CheckpointReport::standby` and `::term`** are the role the checkpoint
  ran under and the term it committed under, both under `control_mu_`.
  Together they make a hybrid visible after the fact: a checkpoint that
  began as the standby and committed after a promotion would report
  standby true with the new term.
- **Callbacks.** `placement_sync` and `lease_blocks` now run under
  `control_mu_` during a promotion, and `seed_gate` runs under the seed
  lock, as before. None may call `checkpoint()` or `promote()` on the same
  Engine; the `Config` comments say so. `snc_server`'s callbacks call
  neither.
- **Latency.** A promotion waits behind a checkpoint in progress, and holds
  `control_mu_` across its control-plane calls. This adds failover
  latency for an embedder that checkpoints and promotes from different
  threads. `snc_server` already runs both from its main loop, one at a
  time.

**Unchanged:**
- the checkpoint and promotion steps themselves;
- the superblock format;
- the seed protocol;
- admission;
- placement semantics.

A checkpoint may now wait for a promotion in progress, and a promotion for
a checkpoint in progress. Both are bounded by the operation itself, and
this ordering is the point of the fix.

**Tests (`test_lifecycle C1` and `C4`).** The tests use a lone standby; no
primary connects, so promotion settles at once. A fake control plane
gives the lease block [5000, 9000).

| Test | Shows |
|---|---|
| C1/C2, order A | a checkpoint is held after its parts are written (before manifest and superblock). `promote()` is called, reaches the durable-control lock and stays there (`debug_promote_at_control()`: a structural check, not only a timing probe); `term()` and the on-disk term stay 0 and the generation is unchanged. Released, both finish. The superblock has term 1, the new checkpoint, the lease block, `next_lease` inside it, and generation exactly +2. Reopened as the primary, recovery succeeds with term 1 |
| C1/C2, order B | `promote()` is held after its superblock write. `checkpoint()` is called and does not return; on disk, term 1 and the old checkpoint. Released, the same final checks |
| C3 | `CheckpointReport::standby` and `::term` are true and 0 in order A, false and 1 in order B |
| C4 | the same two overlaps in a child process that exits while the first is held and the second waits. On disk: order A, term 0 and checkpoint 1 (neither); order B, term 1 and checkpoint 1 (the promotion only). Recovery succeeds on each, with that term |
| C1 under TSan | runs in the serial TSan suite with zero reports |

## 11. S3.3 validation and review

Runs and negative mutations: `bench/results/s33-validation-devvm-20261108/README.md`.

**Independent review** (read-only, of `63f8219`: lifetime-token escape
paths, superblock and role lock ordering, durable-state linearization).
It found no blocker.

What it confirmed:
- no public path to the runtime or the shards remains;
- the lease accounting is exact (move, move-assign, release, destructor);
- every migrated site holds its lease across its waits;
- the only runtime writers of `sb_` and the role are `checkpoint()` and
  `promote()`, both under `control_mu_`;
- the claimed lock order holds, with no inversion and no wait for
  `control_mu_` under it;
- two concurrent promotions now yield exactly one;
- the `replica_config` change is behaviour-neutral.

| # | Finding | Resolution |
|---|---|---|
| S1 | in order A, only a 300 ms timing probe told the hybrid apart; every postcondition held for it too | fixed: the test checks that the promotion stays at the durable-control lock (`debug_promote_at_control`), and `CheckpointReport::term` with `standby` exposes a hybrid after the fact |
| S2 | the negatives build from `git archive HEAD`, and `test_lifecycle` passed for an unknown test name | fixed: committed before the run; an unknown name exits 2 |
| S3 | `DataPlane::run_on_all` from a shard thread deadlocks, though the header allowed shard threads | fixed: it aborts with a message there; R checks it in a child; mutation `lease-run-on-all-shard` |
| S4 | holding a lease across one's own `stop()` hangs silently | a per-thread count would misfire for moved leases; `stop()` reports after 5 s what it waits for, and the contract is in the header |
| N1 | `runtime_view()` as `const NativeRuntime&` could be `const_cast` | fixed: the `RuntimeView` facade |
| N2 | the view was published before two superblock writes (lease block, fresh space) | fixed: published only after a write, or after loading an existing superblock |
| N3 | callbacks run under engine locks | documented on `Config` and above |
| N4 | failover latency behind a checkpoint | documented above |
| N5 | `test_s1_admission`'s `on_shard` releases its lease before the test waits on the ops it issued | not changed: the wait is on operations, not on the submitted task, and no stop runs concurrently there; noted here |
| N6 | a refused checkpoint reports `standby = false`; C4 covers the superblock only and two crash points | `CheckpointReport` says the fields are not meaningful at seq 0. A crash between a promotion's superblock write and its role file is a partial promotion, which is pre-existing and not a promote/checkpoint hybrid |

