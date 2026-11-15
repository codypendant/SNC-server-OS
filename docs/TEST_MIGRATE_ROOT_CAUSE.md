# test_migrate hosted failures: root cause and fix (2026-11-15)

Assignment: cody, 2026-11-15 00:44Z, after PR #127 ("Proceed with the test_migrate correctness
investigation as the next dedicated engineering slice", F1 discipline). Background:
`docs/F1_Q10_LIVENESS_ROOT_CAUSE.md`, and the PR #127 handoff in AGENTS.md (hosted A/B, 240 runs).

Labels: **Observed** (read from a log, counter, kernel file or source), **Inferred** (follows from
observations, not shown directly), **Open** (not explained).

## Summary

The hosted `test_migrate` failures are CPU starvation produced by the test's own process placement.
They are liveness failures only. No migration, replication, fencing, session or node-link defect was
found. No safety violation was seen.

1. `test_migrate` ran 3 lanes of scenarios at once. Each scenario has 4 node processes with 2 pinned
   shards each, so 12 node processes shared a 4-vCPU runner. Every node used the runtime's default
   placement, which puts shard *i* on the *i*-th allowed CPU. All 12 nodes therefore put shard 0 on
   CPU 0 and shard 1 on CPU 1, and no shard ran on CPUs 2 and 3. **Observed** (source:
   `src/native/runtime.cpp` `Impl::run`; `tests/test_migrate.cpp` `node_main` set no CPUs).
2. Under that load on the hosted 7.0 kernels, the per-CPU kernel threads that complete WAL writes went
   unscheduled. On the stuck CPU, `kworker/N:M-dio` and `ksoftirqd/N` were runnable (state R) and ran
   0.0 ms in 2 s. Over the same 2 s, each pinned `test_migrate` thread on that CPU ran about 290 ms.
   **Observed** (section 3.6).
3. The WAL writes of several nodes' shards on that CPU then stayed in flight for 30 to 90 s, all at
   once. This shows in all 21 failed rounds of the run that recorded gauges. **Observed** (section 3.5).
4. A standby whose WAL is stuck acknowledges nothing, so its primary blocks. Sync replication without
   degrade, as here, makes acks wait for the standby. After `T_block` the primary leaves the node links,
   as designed (D.2, 2.12). Primaries with stuck WALs answer nothing either. Clients time out and fail
   over every 3 s, but no answer comes. The test then fails with `requests unanswered 60 s after the
   end` or a stuck driver step. **Observed** (node logs, client counters).
5. F1's pause (PR #126) was in every run. It makes a shard sleep once its own oldest I/O is 1 ms old,
   and it was proven at q10's density of 4 shard threads per CPU. At 12 per CPU it is not enough.
   **Observed:** the as-is arm still fails with F1 in the code. **Open:** why the scheduler keeps those
   kernel threads off the CPU (F1's open question, unchanged).

**Fix** (`tests/test_migrate.cpp` only):

- Each node now pins its shards round the allowed CPUs, offset by lane and node.
- The default lane count now follows the CPU count, so no CPU runs more than 4 pinned shard threads.
  That is 2 lanes on 4 CPUs.
- A new spread check in every scenario fails if a CPU runs more than an even share of the scenario's
  8 shard threads.

No product code, timeout or existing assertion changed.

**Results.** Every hosted round is a full 67-scenario run on a 4-vCPU runner, with the connection table
drained before each round.

| Configuration | Rounds failed | Runs |
|---|---|---|
| Before: default placement, 3 lanes | **53 / 180** | 43671956993, 43678477178, 43680152298 |
| Spread, 3 lanes (6 per CPU) | 4 / 108 | 43671956993, 43682094376, 43683480773 |
| Spread, 2 lanes (4 per CPU) | 0 / 72 | 43671956993, 43683480773 |
| Spread, 1 lane (2 per CPU) | 0 / 36 | 43683480773 |
| **After: the final test at its default (2 lanes)** | **0 / 96** | 43685840387 |

The 4 failures at 3 lanes were all on Azure EPYC 9V45 and 9V74, the runner classes that failed most
before. The final run had 6 such runners and 48 rounds on them. Locally, the full suite passes 51 of
51, and test_migrate passes with the final code in 44 s (32 s before).

## 1. Classification

The assignment's question is whether the failures involve migration correctness, request delivery,
reconnection, state transitions or test infrastructure. They are **test infrastructure**: the test
packs a host's processes onto two CPUs.

| Hypothesis | Test | Result |
|---|---|---|
| Connection-table exhaustion (nf_conntrack 262144 on Azure; one run opens about 100k connections) | As-is and spread arms paired on the same runners, each round starting from a cold table | **Not the cause.** A back-to-back loop did fill the table (carry-over within the 120 s TIME_WAIT window) and confounded the first runs (3.1). From a cold table, as-is still failed 8/36 and spread 0/36 |
| TIME_WAIT reuse or SYN drops | TCP counters per round | **Not the cause.** SYN retransmits number in the thousands per round in every arm, passing rounds included |
| A request-delivery or session defect (a replay never answered) | Client counters added to the failure line | **Refuted.** Each group client fails over about every 3 s (19 times in 60 s). Every reconnect succeeds, and nothing is refused as a replay (`rr=0`); the node just answers nothing |
| A node-link (D.2) defect in block, leave and rejoin | In-process experiment: P's standby refused past `T_block`, with cross-node load both ways, then restored | **Refuted.** P left and rejoined; every request was answered (0 lost in each of 3 loads) and money was conserved |
| A whole-process stall SNC does not recover from | Each node in turn SIGSTOPped for 1.5 to 8 s mid-workload (local) | **Refuted.** Every scenario recovers and passes |
| A shard thread stuck in SNC code | gdb on all threads of all 4 nodes of a stuck scenario (3 failures) | **Refuted.** Shard threads are alive, polling or idle. One seen in `accept` was its busy loop caught mid-syscall |
| WAL writes not completing | Per-shard gauges of every node, written every 250 ms | **Confirmed** (3.5) |
| The kernel's completion threads are starved | Per-thread CPU time on CPUs 0 and 1 over 2 s in the stuck state | **Confirmed** (3.6) |
| The test's placement and density are the cause | As-is vs spread, and 1, 2 and 3 lanes, paired on the same runners | **Confirmed** (Results) |

**No safety violation:** in every completed scenario, all acknowledged transfers were present, balances
were explained by acknowledged plus in-doubt transfers, and the probe saw no second owner.

## 2. Mechanism

| Step | Evidence |
|---|---|
| 12 nodes' shard 0 pinned to CPU 0, shard 1 to CPU 1 | source; scheduler dump `cpu#0 nr_running 13` (run 43671956993) |
| The CPU's completion threads get no time, and WAL writes on it stall | 3.5, 3.6 |
| A standby stops acknowledging; its primary blocks (`degrade_ms` is 0 outside the standby-failure scenarios, so acks wait for the standby) | node log `blocked for T_block (no standby acknowledging), leaving the node links` |
| Peers quarantine the blocked primary's links instance, as designed | `link 0 member 0 incarnation 2 quarantined (blocked for T_release)` |
| Primaries' own WAL writes on that CPU stall too, so they answer nothing | gauges: n1 s0, n2 s0 and n3 s0 all in flight for 61 s |
| Clients time out every 3 s, fail over, resend, and time out again | `fo=19 to=19 rr=0` |

The protocol behaves as specified. A sync primary whose standby makes nothing durable acknowledges
nothing (docs/REPLICATION.md). What fails is the host, which completes no I/O on that CPU.

## 3. Evidence

Each diagnostic was a commit on this branch, tagged `[diag-migrate]`, and was run on 12 hosted runners.
All diagnostics were removed in 826b8f4 and remain in this branch's history (bb0d15a to a11697c).

### 3.1 Back-to-back loop (confounded)

Run 43670006106 (0fd1979), with each round starting on the connection table the previous round left:

| Arm | Rounds failed |
|---|---|
| default placement, 3 lanes | 12 / 24 |
| spread, 3 lanes | 1 / 24 |
| spread, 2 lanes | 5 / 24 |
| spread, 1 lane | 0 / 24 |

Failures tracked a nearly full conntrack table at the start of the round, so every later run drained
the table first.

### 3.2 Paired arms on a cold table

Run 43671956993 (87bc68c). Each job cycles three arms, round by round, and waits for
`nf_conntrack_count` below 2000 before each round:

| Arm | Rounds failed |
|---|---|
| default placement | 8 / 36, on 6975P (3), 9V74 (4) and 9V45 (1); none on 7763 or 8370C |
| spread, 3 lanes | 0 / 36 |
| spread, 2 lanes | 0 / 36 |

### 3.3 The stuck client

Run 43674371376 (f187337) added client counters to the failure line. A typical line:
`12 in flight, 0 to retry | DIAG last_answer_ago=60.1s answers_after_stop=0 [grp0 fo=19 to=19 rs=57 rr=0]
[grp1 fo=19 to=19 rs=171 rr=0]`. It shows no answer for 60 s and one failover every 3 s. Each reconnect
is answered (TOPOLOGY and the standby probe), and nothing is refused as a replay.

### 3.4 Thread backtraces

Run 43676517093 (6c6c08c) captured gdb backtraces of all threads of all 4 nodes, 8 sections over 3
jobs. Every shard thread is in `NativeRuntime::Impl::loop`, either in `poll` (idle) or in
`Receiver::poll` (spinning). None is blocked in SNC code.

### 3.5 Per-shard gauges

Run 43678477178 (484814e): 21 of 72 rounds failed, all with gauges. In each, WAL writes stall together
on the same shard index of several nodes. Examples:

| Scenario | Stuck WAL writes |
|---|---|
| prepared/driver/abort | n1 s0: `wal_flush_max_us` 61,964,471 |
| prepared/driver | n1 s0, n2 s0, n3 s0: in flight 61 s |
| copying/src-primary | n2 s0, n3 s0: 58.1 s |
| committed/dst-restart | n1 s1, n2 s1, n3 s1: 54 s |

### 3.6 Who holds the CPU

Run 43680152298 (a11697c), with 24 of 72 rounds failed, recorded each thread's run time over 2 s in the
stuck state (`/proc/*/task/*/schedstat`, CPUs 0 and 1). In the first failure, on CPU 0:

- `kworker/0:8-dio`, `kworker/0:4+dio` and `ksoftirqd/0` were in state R and ran **0.0 ms**;
- the top 8 `test_migrate` threads ran about 286 ms each.

Other failures show the same on CPU 1. `kworker/1:6+dio`, `kworker/1:11+dio` and `kworker/1:10-dio`
were in state R at 0.0 ms, while `test_migrate` threads ran 335 to 350 ms each.

### 3.7 Local attempts

No local attempt reproduced the starvation. The local machine has 4 vCPUs, kernel 6.18, and the same
12 nodes on CPUs 0 and 1. Results:

| Local attempt | Outcome |
|---|---|
| Default placement | 12 of 12 runs pass |
| The same with 4 busy loops pinned to CPU 0 | 4 of 4 pass |
| SIGSTOP of each node in turn | all pass |

As in F1, the starvation appears only on the hosted 7.0 kernels. The regression test therefore checks
the placement deterministically instead of reproducing the starvation.

## 4. The change

`tests/test_migrate.cpp`:

- `Env` records the scenario's lane.
- `shard_cpus(lane, k, allowed)` places node *k*'s shard *s* on
  `allowed[(lane*8 + k*2 + s) % allowed.size()]`, where `allowed` is the process's affinity mask.
- `node_main` sets `runtime.cpus` from `shard_cpus`. It writes the CPUs its shards actually run on
  (`runtime_view().topology`) to `cpus.K`.
- `check_spread` runs in every scenario once its four nodes are up. It fails if a shard is not pinned,
  or if one CPU runs more than ceil(8 / allowed CPUs) of the scenario's shard threads.
- The default lane count is floor(allowed CPUs × 4 / 8), clamped to 1..3: 2 on 4 CPUs, 3 from 6 CPUs.
  The aim is at most 4 pinned shard threads per CPU, the density F1 verified. `SNC_MIGRATE_LANES` still
  overrides it, and the summary line now names the lane count.

Regression evidence:

| Case | Result |
|---|---|
| **Before** (default placement; spread check in place) | `CPU 0 runs 4 of the scenario's 8 shard threads (at most 2 on 4 CPUs)`. It fails on the first scenario, every time, on any host with 3 or more CPUs |
| **After** | The check passes; all 67 scenarios pass locally |
| Hosted | 0 of 96 failed at the default, against 53 of 180 before |

Guarantees: no product code changed, and every assertion the scenarios make is unchanged:

- all acknowledged transfers present;
- balances explained;
- epochs and phases after each step;
- no second owner seen by the probe;
- fencing and promotion as before.

So the test covers migration safety, durability, ordering, fencing and acknowledgment as it did before.
The full local suite passes on the fix, 51 of 51.

What the test gives up: the 6 and 12 shard threads per CPU it used to impose, which overloaded the
host rather than exercising a migration path, and about 12 s of run time (32 s to 44 s locally).

## 5. Unresolved risks (reported, not fixed)

- **R1. The runtime's default placement with several SNC processes on one host.** Every process puts
  shard *i* on the *i*-th allowed CPU. Several SNC processes on one host, without `--cpus` or cpusets of
  their own, therefore stack on the same CPUs. On hosted 7.0 kernels that starved WAL completion for
  tens of seconds. F1's pause covers 4 shard threads per CPU, not 6 or 12. Deployment is one node per
  host with one pinned shard per core (docs/BENCHMARKS.md), so this is a configuration hazard, not a
  defect in that model. Possible later slices, for the reviewer:
  - (a) the runtime warns about, or refuses, a CPU another SNC process already pinned;
  - (b) a pause policy that does not depend on the shard's own I/O age.

  Neither is started.
- **R2. Other multi-process tests.** `snc_server` clusters in the Python tests (q10 and others) also use
  the default placement, at lower density. F1's fix covers q10. Their hosted stability was not
  re-measured here.
- **R3. The scheduler question** (why runnable per-CPU kernel threads go unscheduled behind pinned user
  threads on the hosted kernels) stays **Open**, as in F1.
- **R4. Connection churn.** One run opens about 100k TCP connections, through per-request `ha::ask` for
  probes and `node_info`. Runs back to back within the 120 s TIME_WAIT window can fill Azure's 262144
  conntrack table. That was not the cause here (3.2), and nothing was changed for it.
- **R5. The residual at 3 lanes.** The spread alone left 4 failures in 108 rounds at 6 shard threads
  per CPU. They were not traced further. At 4 per CPU, 0 of 168 rounds failed (72 paired, 96 final).

## Classification

**REFINE**: the `test_migrate` harness's CPU placement and default lane count.

**KEEP**: the runtime's default placement for one node per host, F1's pause, and the replication,
node-link, session and migration protocols.

## Evidence index

| Item | Where |
|---|---|
| Diag commits | `claude/project-thread-n8tebd`, bb0d15a to a11697c (`git log --oneline 2532f40..826b8f4`) |
| Diagnostic hosted runs | 43670006106, 43671956993, 43674371376, 43676517093, 43678477178, 43680152298 |
| Verification commits | 19758d5, 5b6b715, 1d42cd8 (workflow `verify-migrate`, removed afterwards) |
| Verification runs | 43682094376 (spread, 3 lanes: 4/72), 43683480773 (1, 2 and 3 lanes paired: 0/108), 43685840387 (final default: 0/96) |
| Local experiments (block, leave and rejoin; SIGSTOP; hogs) | described in 3.7 and section 1; their code is in diag commit 484814e (`x36` in `tests/test_xnode.cpp`, `SNC_DIAG_STOP_STANDBY_MS` in `tests/test_migrate.cpp`) |
