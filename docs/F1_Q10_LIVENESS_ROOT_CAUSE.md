# F1: q10 primary liveness failure, root cause and fix (2026-11-14)

Assignment: `docs/handoff/PR125_Diagnosis_Approval_F1_Assignment.md` (F1 only; F2, F3 and F4 are untouched).
Background: `docs/CI_FAILURE_DIAGNOSIS_20261114.md`, section 1.

Three independent QA reviews checked this document, the code and the test; their findings and what was
done about each are in section 6. Every finding cited there was re-checked against the logs or source
before this document was changed.

Labels: **Observed** (read from a log, counter, kernel file or source), **Inferred** (follows from
observations, not shown directly), **Open** (not explained).

## Summary

The q10 failure is caused by the shard runtime's CPU sharing, not by replication, the witnesses,
fencing or the checkpoint. One real control-plane coupling was confirmed along the way (the checkpoint
holds the server's main loop) and is reported, not fixed (section 7).

1. Each SNC process pins shard *i* to the *i*-th CPU it may use (`src/native/runtime.cpp`, `pin` on by
   default). On a 4-CPU hosted runner the four q10 nodes all put shard 0 on CPU 0. **Observed** (source;
   `ps` CPU column in round 12: most running `snc_server` threads on CPU 0).
2. While a shard has file I/O in flight, its loop called `sched_yield()` every `io_yield_us` (50 µs). It
   never slept, because a shard with work never idles; while a checkpoint is active a shard always
   counts as having work (`src/core/shard.cpp:931-934`). **Observed** (source).
3. A WAL write is `O_DIRECT` + `RWF_DSYNC` through io_uring. On ext4 its completion runs in kernel
   threads of the submitting CPU (`ksoftirqd/0`, the DIO completion worker `kworker/0:N-dio`), and can
   wait on the ext4 journal thread (`jbd2`). **Observed** (stacks in rounds 6 and 10).
4. On the hosted 7.0 kernels (`7.0.0-1017-azure`, `7.0.0-1014-aws`), with pinned threads calling
   `sched_yield()` in a loop on CPU 0, those kernel threads stayed runnable with their run time frozen
   for 10 s to minutes. The write's completion did not arrive. **Observed** (rounds 6, 7, 10); round 12
   shows the yield loop is the cause.
5. A primary and its standby have shard 0 on the same CPU, so both stall on the same LSN together. The
   standby stops acking; the primary's own WAL is stuck too; the primary's checkpoint waits for local
   durability and holds the main loop, so no DEGRADE is sent. Clients time out. **Observed** (round 5
   samples).

The fix (`src/native/runtime.cpp`): while I/O is in flight, the periodic pause is still `sched_yield()`
while I/O completes promptly. Once the oldest in-flight op has been in flight for `io_block_after_us`
(1000 µs), the pause is a 1 µs `clock_nanosleep` instead, which takes the shard thread off the run
queue so the CPU's kernel threads run. The pause period restarts after the pause, so the loop still
does `io_yield_us` of work between pauses. Nothing in replication, the witness protocol, fencing, acks
or the checkpoint changed.

Results:
- Before: hosted q10 fails 20 of 100 scenario runs (round 12, `sched_yield` arm).
- First form of the fix (always sleep): 0 of 100 (round 12) and 0 of 200 (round 13).
- Final form: 0 of 200 on 10 runners (round 16), 7 of them of the classes that failed before.
- Durable throughput on the local 4-CPU VM equals `main` (section 5).

## 1. Root-cause analysis

### What was assumed and tested

The diagnosis PR left one leading hypothesis: the checkpoint holds the server's main loop, so the
primary cannot reach the witnesses. The assignment said not to assume it. Each candidate was tested
against runtime evidence:

| Hypothesis | Test | Result |
|---|---|---|
| Checkpoint blocks failover | HEALTH and GAUGES of all 4 nodes every 0.5 s (round 5) | **True but not the cause.** The main loop is held, but the primary's own WAL write is stuck too, so a DEGRADE would not have restored writes |
| Replication or ack logic loses acks | Same samples | **Refuted.** The standby's WAL write is stuck at the same LSN as the primary's (started 0.16 to 0.23 ms apart), and they end together |
| Disk device stall | `/sys/block/*/inflight`, `/proc/diskstats`, D-state tasks, PSI (round 6) | **Not the main cause, but I/O does back up.** Usually no request is in flight at the device; at most 1 on `nvme0n1`. There are D-state tasks: `jbd2` in `wait_on_buffer`, SNC and probe threads in `jbd2_log_wait_commit` and `do_get_write_access`, and 24 hung-task reports (`r6c6.txt`) |
| io_uring loses the completion | Ring fdinfo (round 7) | **Refined.** The stuck ring has exactly 1 SQE more than CQEs, unchanged over 8 dumps; no CQ overflow, no submit error, iou-wrk idle. The kernel holds the completion |
| Pinning only | CPU 0 run queue (`/proc/sched_debug`) during a stall (round 10) | **Refined.** `kworker/0:N-dio` and `ksoftirqd/0` are runnable with frozen sum-exec across successive dumps, while CPU 0 holds the pinned shard-0 threads of all four nodes. Most dumps mark them not eligible (`N`), some eligible (`E`) |
| The yield loop is the cause | A/B in the same jobs (round 12) | **Confirmed.** See below |

A first A/B (round 11, checkpoint waits made idle) was inconclusive: other work kept the loop busy, so
it still never slept.

**Inferred:** the journal waits are the same starvation carried through ext4: the journal commit waits
for a buffer whose completion needs the starved per-CPU threads, and other writers on the file system
wait for that commit. The device stall seen on an AWS runner in round 10 (`dm-0` with 17 to 30
requests in flight, `r10c1.txt`) is a separate, real device stall on that runner class.

### The decisive A/B (round 12, 99c55d4)

Same 10 runners (8 Azure, 2 AWS, 4 CPUs each), same build. Each job alternates the arms, A then B, for
50 rounds; each round runs q10_sealed and q10_committed. The arm is one environment variable read by
the runtime. Sampler, trace and probes ran in both arms.

| Arm | q10 runs failed | Log sections with a stuck WAL flush (over 1.5 s) |
|---|---|---|
| A: `sched_yield()` (as on `main`) | 20 / 100 | 35 |
| B: 1 µs `clock_nanosleep` | 0 / 100 | 0 |

**Observed.** Logs were kept whenever a stuck flush was captured, pass or fail, so B's 0 is a real zero.
Failures came from 6 of the 10 runners (Azure EPYC 9V45, 9V74 and Xeon 8573C; per job 5, 1, 4, 2, 6
and 2). On all 6, A was worse than B. The EPYC 7763 and the two AWS runners failed in neither arm. A
failures appear across all of the odd rounds, so arm order does not explain them.

20 of 100 is the rate under the diag load (sampler, trace and probes), not the base rate on `main`.

### The checkpoint's part

`Shard::poll` reports busy while a checkpoint is active, so a shard waiting on checkpoint durability
spins at 100% on its CPU. **Inferred:** that widens the window. It is not required: in round 5 the
standby was not checkpointing and stalled too.

The checkpoint's shard part waits only on local durability, never on the standby, and the WAL flush is
never gated by replication (`Shard::poll` calls `wal_->poll` on every pass, `src/core/shard.cpp:927-929`).
So the standby cannot hold a checkpoint. **Observed** (source). But the checkpoint does hold the main
loop for its whole duration, every time, not only during an I/O stall (section 7).

### Effects outside SNC

During SNC stalls, a separate `dd` probe on the same runner stalled 7 to 18 s, and once 121 s. The
standalone io_uring probe (`diag/uring_probe.c` on the diag branch) stalled up to 215 s, but it ran
pinned to CPU 0 alongside q10, so it was not a test without SNC. With the fix (round 13), no probe saw
an event over 1 s. **Observed.**

**Open:** why the hosted kernel leaves these runnable kernel threads unscheduled (and mostly not
eligible) behind pinned `sched_yield` loops for so long. This was not traced into the scheduler. The
fix does not depend on the answer: a sleeping thread is off the run queue.

### What the evidence says about the protocol

No q10 failure showed a replication, witness or checkpoint defect once local I/O was moving. When only
a standby stalls and the primary's WAL keeps flushing, the primary logs `stopped acking` and then
`witness: granted, continuing without the standby`, and the run passes (round 12, job 132386055437,
r3 q10_committed). The two open items in the diagnosis doc fit the same cause: round 1's drops
followed by checkpoints without a grant, and the 94.8 s run that rejoined with throughput still 0,
are both a primary whose own WAL was stuck. **Inferred** for those two; they predate the sampler.

## 2. The change

`src/native/runtime.cpp`, `include/snc/native_runtime.h`:

- Each in-flight io_uring op records its submit time and sits on a per-worker list in submission order,
  so the oldest is the head. O(1) on submit and on completion. Only the shard thread touches it.
- The pause every `io_yield_us` is `sched_yield()` while the oldest op is younger than
  `io_block_after_us`, and a 1 µs `clock_nanosleep` after that. The kernel's timer slack (50 µs by
  default) sets the actual pause.
- The period restarts when the pause ends, so the loop runs for at least `io_yield_us` between pauses.
- New `RuntimeConfig::io_block_after_us`, default 1000. It is not exposed as a command-line option.

Three intermediate forms were tried on this branch and rejected:

| Commit | Form | Why rejected |
|---|---|---|
| c0a6ec7 | always sleep | 15 to 18% less durable throughput with reads (section 5) |
| 7d8522a | sleep once no op has completed for 1 ms | quick I/O completing on the same shard kept the loop yielding while one op stayed stuck |
| c4edb12 | oldest op's age, period measured from before the pause | a pause (about 65 µs) outlasts the 50 µs period, so once an op was 1 ms old the loop slept on almost every pass (QA 2) |

What is unchanged: replication, witness authorization, fencing, durable ack semantics, the checkpoint,
failover and recovery. No timeout was raised, and no existing test changed.

## 3. Regression test

`tests/test_io_yield.cpp` (ctest `test_io_yield`, fast label). One shard whose `poll` always reports
work, so the loop never idles. A 4 KiB `sri_write` to a full pipe stays in flight until the pipe is
drained: no disk and no timing luck. A blocking pause is a voluntary context switch
(`voluntary_ctxt_switches` in `/proc`); `sched_yield` never is, whatever else is runnable. Each window
is 1 s.

| Phase | Check |
|---|---|
| 1. Grace (threshold 60 s, so the held write stays young) | fewer than 100 voluntary switches: still `sched_yield` |
| 2. Stuck op (default threshold) | at least 100 voluntary switches, and at least 10 loop passes per switch |
| 3. Stuck op while the shard completes a `/dev/null` write on every pass | at least 100 voluntary switches, at least 100 other writes completed |
| 4. Pipe drained | the held write completes with result 4096 |
| 5. Nothing in flight | fewer than 100 voluntary switches |

It skips if io_uring is unavailable. Local results, 3 runs each (**Observed**):

| Code | Result |
|---|---|
| `main` (0e6ec18) | FAIL phase 2: 0 switches, 20 to 26M passes |
| 7d8522a | FAIL phase 2: about 13,000 to 14,500 switches, only 3 passes per switch |
| c4edb12 | FAIL phase 2: same (3 passes per switch) |
| Always sleep | FAIL phase 1: about 7,300 switches with a young write |
| This PR (3392771) | PASS: phase 1: 0 switches. Phase 2: 7,000 to 8,200 switches with 7.2 to 7.9M passes. Phase 3: about 8,000 switches with about 1M other writes. Phase 5: 0 switches |

QA 2 ran the earlier 300 ms version of this test under load. It passed 20 of 20 runs pinned to one CPU,
and 20 of 20 with 4 to 6 busy threads. It failed 8 to 11 of 20 at 2x oversubscription: 77 switches per
300 ms, one wakeup per scheduler tick. The 1 s windows give about 250 switches at that rate. CI runs
tests one at a time (`ctest -j1`).

The test checks the mechanism (block versus yield). It does not reproduce the kernel starvation itself;
locally this kernel does not starve. The system-level regression is q10 on hosted runners.

## 4. Verification

### Hosted runners

| Round | Code | Runners | q10 runs | Failed |
|---|---|---|---|---|
| 12 | `sched_yield` arm (`main` behaviour) | 10 | 100 | 20 |
| 12 | sleep arm | 10 | 100 | 0 |
| 13 | always sleep (c0a6ec7) | 10 | 200 | 0 |
| 16 | this PR (3392771) | 10 | 200 | 0 |

Round 13 ran on AMD EPYC 7763, 9V74 and 9V45 (Azure) and Intel Xeon 6975P-C (AWS), 6973P-C and 8573C
(Azure). Only the 9V45, 9V74 and 8573C runners failed without the fix in round 12, so those are the
runners where a pass carries information. Round 16 ran on EPYC 9V74 (4), 9V45 (1), Xeon 8573C (2),
EPYC 7763 (2) and Xeon 6975P-C (1): 7 of 10 of those classes. It kept no stuck-flush section, the probes'
longest event was 0.46 s, and `test_io_yield` passed on all 10 (6,449 to 9,493 pauses a second
in phase 2). Rounds 14 and 15 tested 7d8522a and c4edb12 and were
cancelled when those forms were rejected.

### Local

- Full suite, `ctest -j1`, 4-CPU VM, kernel 6.18, on 3392771: 50 of 50 passed (1,765 s).
- `test_io_yield` as in section 3.

### Failure injection

The starvation cannot be caused on demand locally: this kernel does not starve the kernel threads.
`test_io_yield` injects the condition the fix responds to (an op that does not complete, under a busy
loop, with and without other I/O completing). The round 12 A/B is the real fault injected on hosted
runners.

## 5. Performance

`snc_bench`, durable, 4 shards, 10 s, depth 256, local 4-CPU VM, 3 runs each, A/B interleaved in one
session. Ops/s and p99:

| Workload | `main` | This PR |
|---|---|---|
| `--remote 0 --reads 50` | 1.97–2.07M, p99 1.47–1.64 ms | 1.99–2.06M, p99 1.51–1.64 ms |
| `--remote 50 --reads 50` | 2.03–2.09M, p99 1.67–1.77 ms | 2.08–2.22M, p99 1.51–1.67 ms |
| `--remote 0 --reads 0` | 0.89–0.97M, p99 1.84–2.29 ms | 0.93–0.97M, p99 1.80–1.93 ms |

For comparison, the always-sleep form in an earlier session: 1.62–1.65M and 1.62–1.63M with reads,
0.91–0.94M writes only, against `main` at 1.88–2.05M, 1.87–2.00M and 0.92–0.97M in that session.

**Observed.** Nothing in `bench/results/` changed. The R640 was not re-measured; that needs your
authorization.

## 6. Independent QA

Three reviewers worked separately, each given the code or evidence rather than this document's
conclusions: one on the runtime change, one on the test (they ran it), and one on the root-cause claims
against the raw logs.

| Finding | Reviewer | Checked | Action |
|---|---|---|---|
| No correctness defect in list maintenance, counters, teardown, thread use, or completion order | 1 | Read the paths cited | None |
| `io_block_after_us` is not exposed outside code | 1 | Yes | Noted (section 2) |
| An unchecked `io_uring_submit` failure leaves the op counted (as before); its age then includes time before the kernel saw it | 1 | Yes, pre-existing | None: it can only make the pause block sooner |
| With `io_yield_us = 0` there is still no pause at all | 1 | Yes, documented behaviour | None |
| Loop slept on almost every pass once an op was over 1 ms old (period measured from before the pause) | 2 | Yes: 3 passes per switch on c4edb12 | **Fixed** in 3392771; test phase 2 checks passes per switch |
| Test did not check the 1 ms grace: an always-sleep variant passed | 2 | Yes | **Fixed**: test phase 1 |
| Test flaked at 2x CPU oversubscription | 2 | Their numbers | Windows lengthened to 1 s |
| Round 6 "no block I/O in flight, no D tasks" was wrong | 3 | Yes (`r6c6.txt`: jbd2 and SNC threads in D, `nvme0n1` inflight 1, 24 hung-task reports) | **Corrected** (section 1) |
| Round 10 kernel threads are mostly not eligible (`N`), not eligible as claimed | 3 | Yes (`r10c1.txt`) | **Corrected**; the Open question rephrased |
| The checkpoint holds the main loop for every checkpoint's whole duration, not only during an I/O stall; `replication_status` also waits on every shard | 3 | Yes (`src/core/engine.cpp:1395-1440`, `tools/snc_server.cpp:1350`, `:1437`) | **Corrected**; reported as a remaining defect (section 7) |
| The 215 s probe stall ran alongside q10, not without SNC | 3 | Yes (workflow at a3c58f3) | **Corrected** |
| "taskset" evidence for pinning does not exist; it is source plus the `ps` CPU column | 3 | Yes | **Corrected** |
| Round 12 rate is under diag load; failures come from 6 runners | 3 | Yes | Stated (section 1) |
| The device stall was in round 10 (AWS, `dm-0`), not round 6 | 3 | Yes (`r10c1.txt`) | **Corrected** |
| The diagnosis doc's F1 acceptance test (block a standby during a checkpoint, require a grant within bound) is not done | 3 | Yes | Section 7: it tests the coupling, not this failure; proposed with that fix |

## 7. Downstream consequences

- **Remaining defect: the checkpoint holds the main loop.** `Engine::checkpoint()` runs synchronously in
  the server's main loop (`tools/snc_server.cpp:1437`) and returns only when every shard has written and
  synced its part and the manifest is written (`src/core/engine.cpp:1395-1440`). With pacing, a
  checkpoint aims at 30 s and may take longer (`docs/S2_PACING.md`). For that whole time the primary
  sends no DEGRADE: if its standby fails mid-checkpoint, writes wait until the checkpoint ends. This
  is not what failed in q10 (there the primary's own WAL was stuck, so DEGRADE could not have helped),
  but it is a real failover-latency bound. Fixing it means running the checkpoint or the witness
  exchange on another thread, which changes the control plane's threading. It is not in this PR. The
  deterministic test the diagnosis doc proposed (block a standby during a checkpoint, require a grant
  within the normal bound) belongs with that fix and would fail on `main` today. **Observed** (source);
  not demonstrated at runtime. Your call whether it becomes its own slice.
- **`replication_status()` waits on every shard thread** before the DEGRADE decision
  (`src/core/engine.cpp:1959-1983`). A shard thread that stops running its queue would hold DEGRADE
  too. In the q10 stalls the shard threads kept running (they were spinning), so this did not matter
  there.
- **Shard-per-CPU pinning co-locates nodes.** Several SNC processes on one host put shard *i* on the same
  CPU. That is what made a primary and its standby stall together. Production runs one node per host,
  but the lab and CI do not.
- **Kernels.** Any kernel that leaves a per-CPU kernel thread behind a pinned `sched_yield` loop would
  show this. The fix bounds the wait to about 1 ms plus one timer-slack sleep instead of the
  scheduler's choice. While an op is stuck past 1 ms, the shard spends more than half its time in
  pauses (50 µs of work, then the sleep: 7,000 to 8,200 pauses a second in `test_io_yield`, and a
  third of `main`'s loop passes). On a disk where ordinary writes take over 1 ms, that
  costs shard CPU time; it was not measured on such a disk.
- **The R640.** The campaign numbers were measured with the old pause. Under normal I/O latency (well
  under 1 ms) the new code behaves as the old one, so no change is expected, but this was not
  measured there.
- **`io_block_after_us` is a new knob.** Lower values pause by sleeping sooner; 0 makes every pause a
  sleep (the round 13 form).

## Evidence index

All on draft PR #124 (branch `diag-ci-failures`, never to be merged). Workflow runs:

| Round | Commit | Run |
|---|---|---|
| 5 | 3ea0eef, 56eb845 | 43563301650, 43563330828 |
| 6 | 253feb9 | 43564720023 |
| 7 | 5f1958e | 43566415285 |
| 8, 9 | a3c58f3, 13e321f | 43567811273, 43569192302 |
| 10 | 8033b09 | 43571137645 |
| 11 | 4c076c1 | 43572430792 |
| 12 | 99c55d4 | 43574620830 |
| 13 | a1409b7 | 43577587932 |
| 14, 15 (cancelled) | d850aee, 75e4832 | 43580817641, 43581569089 |
| 16 | 2b25fe9 | 43582792318 |
