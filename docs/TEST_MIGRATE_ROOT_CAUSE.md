# test_migrate hosted failures: root cause and fix (2026-11-15)

Assignment: cody, 2026-11-15 00:44Z, after PR #127 ("Proceed with the test_migrate correctness
investigation as the next dedicated engineering slice", F1 discipline). Background:
`docs/F1_Q10_LIVENESS_ROOT_CAUSE.md`; the hosted A/B noted in the PR #127 handoff (AGENTS.md).

Labels: **Observed** (read from a log, counter, kernel file or source), **Inferred** (follows from
observations, not shown directly), **Open** (not explained).

## Summary

The hosted `test_migrate` failures are CPU starvation caused by the test's own CPU placement. They are
not a migration, replication, fencing, session or node-link defect.

1. `test_migrate` runs 3 lanes of scenarios at once. Each scenario has 4 node processes with 2 pinned
   shards each, so 12 node processes share a 4-vCPU runner. Every node used the runtime's default
   placement: shard *i* goes to the *i*-th allowed CPU. So all 12 nodes put shard 0 on CPU 0 and
   shard 1 on CPU 1, and no shard ran on CPUs 2 and 3. **Observed** (source: `src/native/runtime.cpp`
   `run`; `tests/test_migrate.cpp` `node_main` set no CPUs).
2. Under that load on the hosted 7.0 kernels, the per-CPU kernel threads that complete O_DIRECT
   WAL writes were runnable but got no CPU time. On the stuck CPU, `kworker/N:M-dio` and `ksoftirqd/N`
   were in state R and ran 0.0 ms in 2 s. Meanwhile each pinned `test_migrate` thread on that CPU ran
   about 290 ms. **Observed** (run 43680, section 3.6).
3. The WAL writes of several nodes' shards on that CPU then stayed in flight for 30 to 90 s, all at
   the same time. This was seen in all 21 failed rounds of one run. **Observed** (per-shard gauges
   `wal_flush_max_us` and `wal_flush_since_ns`, section 3.5).
4. With a standby's WAL stuck, the standby stops acknowledging. Its primary blocks, because sync
   replication without degrade means acks wait for the standby. After `T_block` the primary leaves the
   node links, as designed. Clients get no answer and fail over every 3 s for 60 s. Then the test
   reports `requests unanswered 60 s after the end` or a stuck driver step. **Observed** (node logs,
   client counters).
5. F1's pause (PR #126) was in every run. F1 makes a shard sleep once **its own** oldest I/O is 1 ms
   old. It was proven at q10's density, 4 nodes on a CPU. At 12 per CPU it is not enough. **Observed**
   (as-is arm still fails with F1 in the code). **Open:** which of the co-pinned threads keep the
   kernel threads ineligible. F1's open question about the scheduler applies here unchanged.

**Fix:** a test change only. Each node pins its shards round the allowed CPUs, offset by lane and node
(`shard_cpus`). On 4 CPUs every CPU then runs 2 shards per lane, 6 in all, where before CPUs 0 and 1
ran 12 each. A new spread check in every scenario fails when one CPU runs more of the scenario's 8 shard
threads than an even split gives. No product code, timeout or existing assertion changed.

**Results.** All hosted rounds are full 67-scenario runs on 4-vCPU runners, with the connection table
drained between rounds.

| Arm | Rounds failed |
|---|---|
| As is, paired run (43671956993) | 8 / 36 |
| As is, later runs (43678477178, 43680152298) | 21 / 72 and 24 / 72 |
| Spread, 3 lanes (paired run) | 0 / 36 |
| Spread, 2 lanes (paired run) | 0 / 36 |
| With the fix (verification run, section 4) | VERIFY_RESULT |

Locally, 2 of 2 full runs pass, as before the fix. The spread check fails with the old placement:
"CPU 0 runs 4 of the scenario's 8 shard threads (at most 2 on 4 CPUs)".

## 1. Classification

The assignment asks whether the failures involve migration correctness, request delivery,
reconnection, state transitions or test infrastructure. The answer is **test infrastructure**: the
test's process placement. The table lists what was tested.

| Hypothesis | Test | Result |
|---|---|---|
| Connection-table exhaustion (nf_conntrack 262144 on Azure; about 100k connections per run) | Rounds started on a cold table, with as-is and spread arms paired on the same runners | **Refuted as the cause.** An earlier back-to-back loop did fill the table, from carry-over within the 120 s TIME_WAIT window, and that confounded the first runs. With a cold table, as-is still failed 8/36 and spread 0/36 |
| TIME_WAIT reuse or SYN drops | Per-round TCP counters | **Not the cause.** SYN retransmits run in the thousands per round in every arm, passing rounds included |
| A request delivery or session defect (a replay never answered) | Client counters in the failure line | **Refuted.** Every group client fails over about every 3 s (19 timeouts in 60 s). Every reconnect succeeds, and nothing is refused as a replay (`rr=0`). The node simply answers nothing |
| A node-link (D.2) defect in block, leave and rejoin | In-process experiment: P's standby refused for longer than `T_block`, cross-node load running both ways, then restored | **Refuted.** P left and rejoined, and every request was answered (lost 0/0/0), money conserved |
| A whole-process stall that SNC fails to recover from | Each node SIGSTOPped for 1.5, 4, 5 or 8 s mid-workload (local) | **Refuted.** Every scenario recovers and passes |
| A standby shard thread stuck in code | gdb, all threads of all 4 nodes of a stuck scenario (3 failures) | **Refuted.** Shard threads are alive, polling or idle. The one seen in `accept` was a spinning loop caught mid-syscall |
| The standby's WAL write never completes | Per-shard gauges of every node, written every 250 ms | **Confirmed.** The writes of several nodes' shards with the same index stay in flight 30 to 90 s together (section 3.5) |
| The kernel's completion threads are starved | Per-thread CPU time on CPUs 0 and 1 over 2 s in the stuck state | **Confirmed.** `kworker/*-dio` and `ksoftirqd` are runnable and run 0.0 ms (section 3.6) |
| The test's CPU placement is the cause | A/B: as-is vs spread placement, same jobs, paired rounds | **Confirmed.** As-is 8/36 failed, spread 0/36 and 0/36 (2 lanes) |

**No safety violation was seen.** In every completed scenario, all acknowledged transfers were present.
Balances were explained by an acknowledged-plus-in-doubt subset. Probe violations were 0. The failures
are liveness only.

## 2. Mechanism

| Step | Evidence |
|---|---|
| 12 nodes' shard 0 pinned to CPU 0, shard 1 to CPU 1 | source; `cpu#0 nr_running 13` in the scheduler dump (run 43671956993) |
| A standby's WAL write on CPU *c* stalls; its completion threads on *c* get no CPU time | gauges; per-thread run time |
| The standby stops acknowledging; its primary blocks (`degrade_ms` is 0 outside the standby-failure scenarios: acks wait for the standby) | node log `blocked for T_block (no standby acknowledging), leaving the node links` |
| Peers quarantine the blocked primary's links instance, as designed (D.2 2.12) | `link 0 member 0 incarnation 2 quarantined (blocked for T_release)` |
| Clients wait; each group client times out (3 s), fails over, resends, and times out again | `fo=19 to=19 rr=0` in 60 s |
| Primaries' WAL writes on the same CPU stall too, so even an unblocked primary answers nothing | gauges: n1s0, n2s0, n3s0 all in flight 61 s |

The protocol behaves as specified throughout. A sync primary whose standby makes nothing durable
acknowledges nothing; that is by design (docs/REPLICATION.md). What is wrong is that the host cannot
complete I/O.

## 3. Evidence

Each diagnostic was a commit on this branch, run on 12 hosted runners. Each commit message ends
`[diag-migrate]`. All of them were removed in 826b8f4 and stay in this branch's history.

### 3.1 First runs (back-to-back loop)

Run 43670006106 (0fd1979), with the connection table carried over from the previous round:

| Arm | Result |
|---|---|
| as is, 3 lanes | 12 / 24 failed |
| spread, 3 lanes | 1 / 24 failed |
| spread, 2 lanes | 5 / 24 failed |
| spread, 1 lane | 0 / 24 failed |

Failures tracked a near-full table at the start of the round. That is how the loop was confounded; see
3.2.

### 3.2 Paired arms on a cold table

Run 43671956993 (87bc68c). In each job the round's arm cycles: as is, spread (3 lanes), spread (2
lanes). Before each round it waits for `nf_conntrack_count` to fall below 2000.

| Arm | Rounds failed | Where |
|---|---|---|
| as is | 8 / 36 | 6975P (3), 9V74 (4), 9V45 (1); 7763 and 8370C passed |
| spread, 3 lanes | 0 / 36 | |
| spread, 2 lanes | 0 / 36 | |

### 3.3 The stuck client

Run 43674371376 (f187337) added client counters to the failure line. One example:
`12 in flight, 0 to retry | DIAG last_answer_ago=60.1s answers_after_stop=0 [grp0 fo=19 to=19 rs=57 rr=0]
[grp1 fo=19 to=19 rs=171 rr=0]`. It shows no answer for 60 s and one failover per 3 s. Every reconnect
succeeds (TOPOLOGY and the probe are answered) and nothing is refused as a replay.

### 3.4 Thread backtraces

Run 43676517093 (6c6c08c) attached gdb to all 4 nodes of the first stuck scenario per round. There are
8 such sections over 3 jobs. The shard threads are in `NativeRuntime::Impl::loop`, either in `poll`
(idle) or in `Receiver::poll` (spinning). None is blocked in SNC code.

### 3.5 Per-shard gauges

Run 43678477178 (484814e): 21 of 72 rounds failed, and all 21 have gauges. In every client-stuck
failure, the WAL writes of the same shard index on several nodes stalled 30 to 90 s together. Examples:

| Scenario | Shards with a stuck write |
|---|---|
| prepared/driver/abort | n1 s0 `wal_flush_max_us` 61,964,471 |
| prepared/driver | n1 s0, n2 s0, n3 s0 all in flight 61 s |
| copying/src-primary | n2 s0, n3 s0 58.1 s |
| committed/dst-restart | n1 s1, n2 s1, n3 s1 54 s |

### 3.6 Who holds the CPU

Run 43680152298 (a11697c) recorded each thread's run time over 2 s, from `schedstat`, on CPUs 0 and 1 in
the stuck state. 24 of 72 rounds failed. In the first failure, on CPU 0:

- `kworker/0:8-dio`, `kworker/0:4+dio` and `ksoftirqd/0`: state R, **0.0 ms**;
- each of the top 8 `test_migrate` threads: about 286 ms.

Other failures show the same on CPU 1: `kworker/1:6+dio`, `kworker/1:11+dio` and `kworker/1:10-dio`
are R with 0.0 ms, while `test_migrate` threads run 335 to 350 ms each.

### 3.7 Local reproductions tried

None reproduced the failure locally. The local container has 4 vCPUs, kernel 6.18, and the same 12
nodes on CPUs 0 and 1. Its results:

- as is: 12 of 12 runs pass;
- as is with 4 busy loops pinned to CPU 0: 4 of 4 pass;
- whole-process freezes (SIGSTOP) of each node: all pass.

As in F1, the starvation shows only on the hosted 7.0 kernels. That is why the regression test checks
the placement deterministically rather than reproducing the starvation.

## 4. The change and its verification

`tests/test_migrate.cpp` only:

- `Env` records the scenario's lane.
- `shard_cpus(lane, k, allowed)` gives node *k*'s shard *s* the CPU
  `allowed[(lane*8 + k*2 + s) % allowed.size()]`.
- `node_main` sets `runtime.cpus` from it and writes the CPUs its shards actually run on (from
  `runtime_view().topology`) to `cpus.K`.
- `check_spread`, run in every scenario once its four nodes are up, fails if a shard is not pinned. It
  also fails if one CPU runs more than ceil(8 / allowed CPUs) of the scenario's shard threads.

Regression evidence:

- **Before** (runtime default placement, with the check): `CPU 0 runs 4 of the scenario's 8 shard
  threads (at most 2 on 4 CPUs)`. It fails on the first scenario, every time.
- **After:** the check passes, and all 67 scenarios pass locally, 2 of 2 runs.
- **Hosted:** VERIFY_DETAIL.

Guarantees: no product code changed. Every assertion the scenarios make stands unchanged:

- all acknowledged transfers are present;
- balances are explained;
- epochs and phases follow each step;
- the probe finds no two owners;
- fencing and promotion behave as they did.

So migration safety, durability, ordering, fencing and acknowledgment are covered as before. The full
local suite was run on the fix (CTEST_RESULT).

## 5. Unresolved risks (reported, not fixed)

- **R1, the runtime's default placement with several SNC processes on one host.** Every process puts
  shard *i* on the *i*-th allowed CPU. Several SNC processes on one host, with no `--cpus` or cpuset of
  their own, therefore stack their shards on the same CPUs. On hosted 7.0 kernels that starves WAL
  completion for tens of seconds. F1's pause covers 4 per CPU, not 12. Deployment is one node per host
  (docs/BENCHMARKS.md: one pinned shard per core), so this is a configuration hazard, not a defect in
  that model. Two possible later slices, for the reviewer to choose:
  - (a) the runtime refuses, or warns about, a CPU that another SNC process has already pinned;
  - (b) a pause policy that does not depend on this shard's own I/O age.

  Not started.
- **R2, other multi-process tests.** `snc_server` clusters in the Python tests (q10 and others) also
  use default placement, at lower density. F1's fix covers q10. Their hosted stability is not
  re-measured here.
- **R3, the scheduler question.** Why the hosted kernel leaves runnable per-CPU kernel threads
  unscheduled behind pinned user threads is still **Open**, as in F1.
- **R4, the connection churn.** `test_migrate` opens about 100k TCP connections per run, through
  per-request `ha::ask` for probes and `node_info`. That approaches Azure's 262144 conntrack limit when
  runs are back to back within the 120 s TIME_WAIT window. It did not cause these failures (3.2), but a
  loop of runs without a pause can fill the table. Not changed.

## Classification

**REFINE** of the `test_migrate` harness: node CPU placement. KEEP everything else: the runtime's
default placement for one node per host, F1's pause, and the replication, node-link, session and
migration protocols.

## Evidence index

Diag commits on `claude/project-thread-n8tebd`: bb0d15a to a11697c, listed by
`git log --grep diag 2532f40..826b8f4`. Hosted runs: 43670006106, 43671956993, 43674371376, 43676517093,
43678477178, 43680152298, and the verification run VERIFY_RUN.
