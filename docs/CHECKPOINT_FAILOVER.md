# A running checkpoint must not hold failover (2026-11-14)

Assignment: cody, 2026-11-14 12:05 UTC, after the PR #126 review
(`docs/handoff/PR126_F1_Review_Approval.md`): "Address checkpoint-induced failover blocking as a separate
engineering slice", with a deterministic standby-failure-during-checkpoint regression test.

**Classification:** REFINE (where `snc_server` runs its periodic checkpoint). No engine, replication, witness,
fencing, checkpoint or recovery code changed.

Labels: **Observed** (run or read from source), **Inferred** (follows, not shown directly).

## 1. The defect

`snc_server` has one main loop (`tools/snc_server.cpp`). Every pass it does two jobs in turn:
- it asks the witnesses for DEGRADE when a shard is blocked on a lost standby, and records NOT READY and READY;
- it runs the periodic checkpoint (`--checkpoint-s`) by calling `Engine::checkpoint()`.

`Engine::checkpoint()` returns only when every shard has written and synced its part and the manifest and
superblock are written (`src/core/engine.cpp`, `Engine::checkpoint`). For that whole time the loop asks the
witnesses nothing. A standby lost mid-checkpoint is therefore granted away only after the checkpoint ends.
Until then, writes that need the standby's ack wait. **Observed** (section 4, before the fix).

How long that is depends on the checkpoint. Paced checkpoints aim at 30 s by default and may take longer
(`docs/S2_PACING.md`). The checkpoint itself never needs the standby: a shard's part waits only on local
durability. So the wait is pure coupling, not a dependency.

## 2. The change

The main loop now starts each periodic checkpoint on its own thread and keeps running:
- One checkpoint at a time. The next is scheduled from the end of the last, as before (the thread records
  the end time).
- The thread clears `RESOURCES` `ckpt_running` and counts `ckpts` the moment the checkpoint returns, so
  both keep their meaning.
- While a checkpoint runs, the loop wakes at least every 20 ms to see whether it has ended. Then it joins the
  thread, prints the same `checkpoint: ...` line and schedules the next one.
- On shutdown, a running checkpoint is joined before the engine stops, as when the loop ran it.

New test hook, `--debug-admin` only: `DEBUG CKPT-HOLD commit|sync 1|0` (and `?`). It sets the existing engine
hooks `debug_ckpt_hold_commit` (a checkpoint stops after every part is written and synced, before the commit)
and `debug_ckpt_hold_sync` (the parts' final fdatasync completions are held, so the shards stay in their
checkpoint). The reply says whether a checkpoint waits at the commit hold.

## 3. Why it is safe

The engine already allows these calls from different threads. **Observed** (source):

| Main-loop call, now concurrent with a checkpoint | Why it is safe |
|---|---|
| `replication_status()` | It takes an operation permit and submits to each shard, as the admin thread's STATUS and RESOURCES already do during checkpoints |
| `allow_degrade()`, `withdraw()`, `fenced()` | Atomics |
| `term()`, `lease_block()` | They read the superblock view under `sb_view_mu_`, copied after each superblock write (S3.3) |
| `stats()` | It takes a permit and runs a task on the shard, as the admin and sampler threads already do during checkpoints |
| `promote()` (standby, on SIGUSR1) | It takes `control_mu_`, which `checkpoint()` holds for its whole run. The two still linearize (`docs/ENGINE_LIFECYCLE.md` section 10) |
| `stop()` | Waits for every permit, including the checkpoint's. The main loop joins the checkpoint thread before it stops the engine anyway |

S3.3 built `control_mu_` for exactly this case: an embedder that checkpoints and promotes from different
threads. `snc_server` is now such an embedder.

The grant itself is unchanged. The primary asks only when a shard reports its standby blocked. The witnesses
decide as before, and fencing and terms are untouched. A checkpoint that runs while the primary runs alone is
the same checkpoint as before.

**Inferred:** if `Engine::checkpoint()` throws, the process ends with `std::terminate` from the thread. Before,
the exception left `main` uncaught, which also ends in `std::terminate`. No caller caught it before either.

## 4. Tests

`tests/test_ckpt_failover.py` (ctest `test_ckpt_failover`, fast tier, about 11 s). Real processes: the sync
pair of `tests/test_health.py` with `--checkpoint-s 1`, three witnesses, and a create load. The checkpoint is
held with `DEBUG CKPT-HOLD`, so nothing depends on how long a checkpoint happens to take.

| Scenario | Fault | Required |
|---|---|---|
| k1_commit | primary's checkpoint held before its commit; standby SIGSTOPped | grant within 20 s; writes acknowledged after the grant while the checkpoint is still held; the witnesses keep node 0 primary, degraded; the standby returns and is proven READY, still during the held checkpoint; released, the checkpoint commits; no acknowledged write missing |
| k2_sync | same, held inside the shards (final fdatasyncs held) | same |
| k3_promote | standby's own checkpoint held; primary SIGKILLed | the witnesses choose node 1, but it stays a standby while its checkpoint is held; promoted once released; no acknowledged write missing |

Results, local 4-CPU VM (**Observed**):

| Code | k1_commit | k2_sync | k3_promote |
|---|---|---|---|
| Before (main + the test hook only) | FAIL: no grant in 20 s | FAIL: no grant in 20 s | pass |
| After | pass, grant in 0.55 s, 3 batches acknowledged while held | pass, grant in 0.55 s, 4 batches | pass, promoted 0.05 s after release |

Repetitions on the final code: 3 more local runs of all three scenarios passed (grant 0.55 to 0.60 s,
READY while held every time), and an independent reviewer ran each scenario 5 times on its own build: all
passed. The same reviewer rebuilt the old synchronous loop with the test hook: k1 and k2 failed with no grant
in 20 s, and k3 passed.

Full local suite, `ctest -j1`, on the final code: 51 of 51 passed (1,776 s). Hosted full suite
(workflow_dispatch on the branch, 5d0039d): 50 of 51 passed, `test_ckpt_failover` included. `test_migrate`
failed (`copied/src-restart`: requests unanswered 60 s after the end). It runs no periodic checkpoint, and it
also failed on main's own nightly run (37cdf51, `committed/driver/abort`, same symptom). **Observed.**

To see whether F1 (PR #126) caused it, `test_migrate` alone ran 24 times on each of 10 hosted runners,
alternating F1's pause off (A, the old `sched_yield` loop) and on (B, as merged). **Observed:**

| | A, pause off | B, pause on |
|---|---|---|
| Round 1 on a fresh runner (A only) | 2 of 10 fail | |
| Before the runner's first failure | 8 of 23 fail | 2 of 15 fail |
| After a passing round | 21 of 51 fail | 27 of 68 fail |
| After a failing round | 29 of 59 fail | 38 of 52 fail |
| All | 52 of 120 fail | 65 of 120 fail |

**Inferred:** `test_migrate` is flaky on hosted runners without F1's pause too, and a failure makes the next
run on the same runner likelier to fail. In comparable conditions the two arms fail alike; the difference in
the totals comes from runs after a failure. So F1 is not shown to cause it, nor fully cleared. Its failure
modes vary (unanswered requests, STUCK deadlines, epoch asserts, no quorum at placement init). This is a
separate defect, not fixed here: it needs its own root cause.

Independent QA (two reviewers, one on thread safety and one on the test) found no defect. What they raised
and what was done:

| Finding | Action |
|---|---|
| `ckpt_running` and `ckpts` were updated when the loop joined the thread, up to a witness round late | The thread updates them when the checkpoint returns |
| The next checkpoint was scheduled from the join, not the checkpoint's end | Scheduled from the end the thread records |
| `stats()` described wrongly in section 3 | Corrected |
| A standby shows role `m` earlier while its promotion waits | Documented (section 5) |
| k3 did not check that the witnesses had chosen node 1 | It now asserts that before checking the role |
| READY during a held checkpoint, and shutdown during one, were untested | k1/k2 now prove READY while the checkpoint is held; shutdown is documented (it waits, as before) |
| `--checkpoint-s` with no data dir makes the loop spin | Pre-existing, unchanged, out of scope |

## 5. What remains

- **A promotion still waits for the standby's own running checkpoint.** This is deliberate (S3.3,
  `docs/ENGINE_LIFECYCLE.md` section 10): a checkpoint runs wholly as the standby or wholly as the primary. k3
  records it. The wait is bounded by that checkpoint's remaining time. Shortening it means a checkpoint
  that can be abandoned or hurried when a promotion is waiting. That changes checkpoint semantics and needs
  its own design and review.
- **`replication_status()` waits on every shard thread.** A shard thread that stops running its queue would
  hold DEGRADE too. No such case is known: shard threads keep running during checkpoints and I/O stalls
  (`docs/F1_Q10_LIVENESS_ROOT_CAUSE.md`).
- **A standby shows role `m` (promoting) for longer.** A SIGUSR1 that arrives during the standby's own
  checkpoint is now handled at once: `promoting` is set and `promote()` waits on `control_mu_`. Before, the
  signal waited unhandled until the checkpoint ended, and the role stayed `s` until then. Either way the
  standby's main loop waits (HEALTH `main_ms` grows) until its checkpoint ends; only where it waits changed.
  The failover latency is the same.
- **Shutdown waits for a running checkpoint**, as before: SIGTERM ends the loop, which joins the checkpoint
  thread before stopping the engine.
- **`main_ms` in HEALTH** no longer rises during checkpoints. It is diagnostic only
  (`docs/HEALTH_DECOUPLING.md`), and nothing acts on it.
