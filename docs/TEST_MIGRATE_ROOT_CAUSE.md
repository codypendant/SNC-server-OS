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
   once. This shows in 20 of the 21 failed rounds of the run that recorded gauges. **Observed** (section 3.5).
4. A standby whose WAL is stuck acknowledges nothing, so its primary blocks. In a group without
   degrade (both groups in 39 of the 67 scenarios, one group in the other 28), acks wait for the standby. After `T_block` the primary leaves the node links,
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
  8 shard threads. A restarted node must come back on the same CPUs.
- A run-wide check at start refuses a default lane count that would put more than 4 pinned shard
  threads on one CPU across all lanes. An explicit `SNC_MIGRATE_LANES` above that only warns.

No product code, timeout or existing assertion changed.

**Results.** Every hosted round is a full 67-scenario run on a 4-vCPU runner, with the connection table
drained before each round.

| Configuration | Rounds failed | Runs |
|---|---|---|
| Before: default placement, 3 lanes | **101 / 324** | 43671956993 (8/36), 43674371376 (27/72), 43676517093 (21/72), 43678477178 (21/72), 43680152298 (24/72) |
| Spread, 3 lanes (6 per CPU) | 4 / 144 | 43671956993 (0/36), 43682094376 (4/72), 43683480773 (0/36) |
| Spread, 2 lanes (4 per CPU) | 0 / 72 | 43671956993, 43683480773 |
| Spread, 1 lane (2 per CPU) | 0 / 36 | 43683480773 |
| **After: the final test at its default (2 lanes)** | **0 / 96** | 43685840387 |

The "before" rounds other than 43671956993 ran with diagnostic instrumentation (gauge files, /proc
scans, gdb); 43671956993 compares the arms under the same conditions (8/36 against 0/36). The 4
failures at 3 lanes were all on Azure EPYC 9V45 (3) and 9V74 (1), the runner classes that failed most
before (9V45 24/39, 9V74 27/63, 6975P-C 38/123, 7763 0/72 as-is). The final run had 6 such runners and 48 rounds on them. Locally, the full suite passes 51 of
51, and test_migrate passes with the final code in 44 s (32 s before).

## 1. Classification

The assignment's question is whether the failures involve migration correctness, request delivery,
reconnection, state transitions or test infrastructure. They are **test infrastructure**: the test
packs a host's processes onto two CPUs.

| Hypothesis | Test | Result |
|---|---|---|
| Connection-table exhaustion (nf_conntrack max 262144 on most runners, 1048576 on some; one passing run opens about 110k to 250k connections) | As-is and spread arms paired on the same runners, each round starting from a cold table; an arm with no connection tracking on loopback | **Not the cause of these failures, but a second, separate hazard.** Back-to-back rounds did fill the table (carry-over within the 120 s TIME_WAIT window), and that caused failures of its own in the first runs (3.1, 3.8). From a cold table, as-is still failed 8/36 and spread 0/36. With tracking off on loopback, as-is failed as often as with it on (6/32 each, 3.8) |
| TIME_WAIT reuse or SYN drops | TCP counters per round | **Not the cause.** SYN retransmits number in the thousands per round in every arm, passing rounds included |
| A request-delivery or session defect (a replay never answered) | Client counters added to the failure line | **Refuted.** Each group client fails over about every 3 s (19 times in 60 s, the most common value). On each failover it reaches some endpoint: a refused connect (`Connection refused`) or a standby's E_STANDBY answer shows in its last error. Nothing is refused as a replay (`rr=0`, 133 of 133 entries); the primaries just answer nothing |
| A node-link (D.2) defect in block, leave and rejoin | In-process experiment: P's standby refused past `T_block`, with cross-node load both ways, then restored | **Refuted.** P left and rejoined; every request was answered (0 lost in each of 3 loads) and money was conserved |
| A whole-process stall SNC does not recover from | Each node in turn SIGSTOPped for 1.5 to 8 s mid-workload (local) | **Refuted.** Every scenario recovers and passes |
| A shard thread stuck in SNC code | gdb on all threads of all 4 nodes of a stuck scenario (3 failures) | **Refuted.** Shard threads are alive, polling or idle. One seen in `accept` was its busy loop caught mid-syscall |
| The shard loop's `sched_yield` (F1's mechanism) | A/B: the pause as on main vs. always sleeping | **Not sufficient.** Always sleeping failed as often (33/60 vs. 27/60, back-to-back rounds, 3.8) |
| Pinning as such | Arm with the nodes unpinned | **Not the factor by itself.** Unpinned failed about as often as pinned in back-to-back rounds (11/40 vs. 14/40, 3.8). Unpinned, 24 busy shard threads still share 4 CPUs (6 per CPU), so this agrees with density, not placement, being what matters; those rounds also had no cold table |
| Disk or IOPS throttling on the runner | I/O pressure and D-state threads in the time series (3.8) | **Not the cause.** I/O pressure (`some` up to about 77%) shows tasks waiting on I/O, which the stuck writes produce either way. The fix changes only CPU placement and leaves the I/O volume as it was, yet the failures stop |
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
| A standby stops acknowledging; its primary blocks (in a group whose `degrade_ms` is 0, which is every group except the failed one in the standby and restart scenarios, acks wait for the standby) | node log `blocked for T_block (no standby acknowledging), leaving the node links` |
| Peers quarantine the blocked primary's links instance, as designed | `link 0 member 0 incarnation 2 quarantined (blocked for T_release)` |
| Primaries' own WAL writes on that CPU stall too, so they answer nothing | gauges: n1 s0, n2 s0 and n3 s0 all in flight for 61 s |
| Clients time out every 3 s, fail over, resend, and time out again | `fo=19 to=19 rr=0` |

The protocol behaves as specified. A sync primary whose standby makes nothing durable acknowledges
nothing (docs/REPLICATION.md). What fails is the host, which completes no I/O on that CPU.

## 3. Evidence

Each diagnostic was a commit on this branch (most tagged `[diag-migrate]`; also `[diag-twsyn]` and
`[diag-starve]`), and most ran on 12 hosted runners.
All diagnostics were removed in 826b8f4 and remain in this branch's history (bb0d15a to a11697c).

### 3.1 Back-to-back loop (confounded)

Run 43670006106 (0fd1979), with each round starting on the connection table the previous round left:

| Arm | Rounds failed |
|---|---|
| default placement, 3 lanes | 12 / 24 |
| spread, 3 lanes | 1 / 24 |
| spread, 2 lanes | 5 / 24 |
| spread, 1 lane | 0 / 24 |

Failures tracked a nearly full conntrack table: 10 of the 12 as-is failures and all 5 spread 2-lane
failures started with the table more than 80% full, and every one of those 5 dropped packets for a
full table during the round (`ctfull` 10 to 120). The one spread 3-lane failure ran on a runner with a
1048576 table and dropped nothing. Every later run drained the table first.

### 3.2 Paired arms on a cold table

Run 43671956993 (87bc68c). Each job cycles three arms, round by round, and waits for
`nf_conntrack_count` below 2000 before each round:

| Arm | Rounds failed |
|---|---|
| default placement | 8 / 36, on 6975P (3), 9V74 (4) and 9V45 (1); none on 7763 or 8370C |
| spread, 3 lanes | 0 / 36 |
| spread, 2 lanes | 0 / 36 |

### 3.3 The stuck client

Run 43674371376 (f187337) added client counters to the failure line. A typical line, abridged (the
full line also carries the epoch, `fw=0` and the client's last error):
`12 in flight, 0 to retry | DIAG last_answer_ago=60.113s answers_after_stop=0 [grp0 fo=19 to=19 rs=57 rr=0]
[grp1 fo=19 to=19 rs=171 rr=0]`. It shows no answer for 60 s and one failover every 3 s. The last errors
are a refused connect (28 group entries) or a standby answering E_STANDBY (90), and nothing is refused
as a replay.

### 3.4 Thread backtraces

Run 43676517093 (6c6c08c) captured gdb backtraces of all threads of all 4 nodes, about 13 captures in
9 of the 12 jobs. Every shard thread is in `NativeRuntime::Impl::run`'s loop: 76 in `poll`, 19 in
`clock_nanosleep` (F1's pause, so those shards had an I/O over 1 ms old), 5 in `accept` and 4 in
`epoll_wait` under the receiver's and connection set's polls, 1 in `recv`, 1 reading acks. None is
blocked in SNC code.

### 3.5 Per-shard gauges

Run 43678477178 (484814e): 21 of 72 rounds failed, all with gauges. In 20 of them, WAL writes stall
together on the same shard index of several nodes: in flight since the same moment, or with
`wal_flush_max_us` between 31 and 90 s. The exception (one job's round 6) failed with `timed out waiting
for up.0` and `no transfers acknowledged`; its gauges show a longest flush of about 2.7 s and nothing
in flight, so its cause is not shown. Examples:

| Scenario | Stuck WAL writes |
|---|---|
| prepared/driver/abort | n1 s0: `wal_flush_max_us` 61,964,471 |
| prepared/driver | n1 s0, n2 s0, n3 s0: in flight 61 s |
| copying/src-primary | n2 s0, n3 s0: 58.1 s |
| committed/dst-restart | n1 s1, n2 s1, n3 s1: 54 s |

### 3.6 Who holds the CPU

Run 43680152298 (a11697c), with 24 of 72 rounds failed, recorded each thread's run time between two
full scans of `/proc/*/task/*/schedstat` with `sleep 2` between them (so a little over 2 s), in the
stuck state. A thread is attributed to the CPU it last ran on, which is a sample, not exact accounting. In the first failure, on CPU 0:

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

### 3.8 Earlier diagnostics (back-to-back rounds, no cold table)

These ran before the table carry-over was found, so each mixes starvation with full-table drops.

| Commit, run | Question | Result |
|---|---|---|
| 86c48f8, 43664766143 | The pause as on main vs. always sleeping | 27/60 vs. 33/60 failed; a sleep-always pause does not prevent it |
| db87410, 43666006045 | A standalone reproducer, no SNC code: 4 or 12 pinned busy threads (sleep, yield or spin) on CPU 0 against loopback asks | No stall on 6 runners; `ksoftirqd/0` never runnable. It had no file I/O, so it does not test the dio completion path |
| 78e88b2, 43666376745 | Nodes pinned (as is) vs. unpinned vs. one lane | 14/40, 11/40, 11/40 failed |
| 742c6d9, 43667963821 | Loopback TIME_WAIT and connect delays | TIME_WAIT overflows and SYN retransmits in every round, passing ones included; not separating |
| 798727c, 43668079133 | Disk in flight, D-state threads, I/O pressure | 33/80 failed; I/O pressure `some` up to about 77% in stalls, `full` 0 |
| ea68fc5, 43669097367 | As is vs. no conntrack on loopback vs. a paced probe; ftrace on CPU 0 | 6/32, 6/32, 14/32 failed; ftrace showed CPU 0's busy shard threads taking about 95% of it |

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
  The aim is at most 4 pinned shard threads per CPU, the density F1 verified (on 2 CPUs one lane is
  already 4 per CPU, and on 1 CPU all 8 share it). `SNC_MIGRATE_LANES` still overrides it, and the
  summary line now names the lane count.
- At start, the run counts pinned shard threads per CPU over all lanes, using the same `shard_cpus`.
  Above 4 (on 2 or more CPUs), the default refuses to run and an explicit `SNC_MIGRATE_LANES` warns.
- A restarted node must report the same CPUs as before (`cpus.K.restart` equal to `cpus.K`).

What each check guards:

| Check | Guards | Fails if |
|---|---|---|
| `check_spread` (each scenario) | The spread within a scenario | Every node goes back to the runtime's default placement |
| Run-wide density (start) | The lane offset and the default lane count | The default goes back to 3 lanes on 4 CPUs; the lane offset is dropped on 6 or more CPUs |
| Restart CPUs | Placement after a restart | A restarted node lands on other CPUs |

Neither check reproduces the starvation, which no local machine shows (3.7); they hold the test to the
placement and density the hosted evidence found safe.

Regression evidence:

| Case | Result |
|---|---|
| **Before** (default placement; spread check in place) | `CPU 0 runs 4 of the scenario's 8 shard threads (at most 2 on 4 CPUs)`. It fails on the first scenario, every time, on any host with 3 or more CPUs |
| **Before** (old default of 3 lanes on 4 CPUs) | `3 lanes put 6 pinned shard threads on one of 4 CPUs (at most 4)`, exit 1 before any scenario |
| **After** | The check passes; all 67 scenarios pass locally |
| Hosted | 0 of 96 failed at the default, against 101 of 324 before |

Guarantees: no product code changed, and every assertion the scenarios make is unchanged:

- all acknowledged transfers present;
- balances explained;
- epochs and phases after each step;
- no second owner seen by the probe;
- fencing and promotion as before.

So the test covers migration safety, durability, ordering, fencing and acknowledgment as it did before.
The full local suite passes on the fix, 51 of 51.

What the test gives up: the 6 and 12 shard threads per CPU it used to impose, which overloaded the
host rather than exercising a migration path, and about 12 s of run time (32 s to 44 s locally). That
overload also made slow nodes and slow I/O by accident; slow-node coverage, if wanted, belongs in a
deliberate fault (such as the SIGSTOP experiment of 3.7), not in host overload.

New failure mode: a scenario now fails if a shard could not be pinned (`a shard is not pinned`), as
in a sandbox that refuses `sched_setaffinity`. Before, nothing checked pinning there.

## 5. Unresolved risks (reported, not fixed)

- **R1. The runtime's default placement with several SNC processes on one host.** Every process puts
  shard *i* on the *i*-th allowed CPU. Several SNC processes on one host, without `--cpus` or cpusets of
  their own, therefore stack on the same CPUs. On hosted 7.0 kernels that starved WAL completion for
  tens of seconds. F1's pause runs only while a shard has I/O in flight that is 1 ms old, so a busy
  shard with no such I/O does not yield the CPU to another process's completion threads. That other
  process may be another SNC node or not SNC at all. The verified safe envelope on hosted 7.0 kernels
  is at most 4 pinned shard threads per CPU; 6 per CPU failed 4 of 144 rounds even when spread.
  Deployment is one node per host with one pinned shard per core (docs/BENCHMARKS.md), so inside that
  model this does not arise, but it is a real product limitation outside it, not only a configuration
  slip. It is not stated in the deployment docs yet. Possible later slices, for the reviewer:
  - (a) the runtime warns about, or refuses, a CPU another SNC process already pinned;
  - (b) a pause policy that does not depend on the shard's own I/O age.

  Neither is started.
- **R2. Other multi-process tests.** `snc_server` clusters in the Python tests (q10 and others) also use
  the default placement, at lower density. F1's fix covers q10. Their hosted stability was not
  re-measured here.
- **R3. The scheduler question** (why runnable per-CPU kernel threads go unscheduled behind pinned user
  threads on the hosted kernels) stays **Open**, as in F1.
- **R4. Connection churn, a second and separate defect.** A passing run opens about 110k to 250k TCP
  connections, through per-request `ha::ask` for probes and `node_info`. Runs back to back within the
  120 s TIME_WAIT window filled the 262144-entry conntrack table and failed on their own, including at
  the new default (all 5 spread 2-lane failures in 3.1 dropped packets for a full table). Every
  verification round, the final 0/96 included, started from a drained table. One CI run of the suite
  runs `test_migrate` once, so it fills the table only if tests just before it leave enough entries;
  that was not measured. Nothing was changed for it here. Measured and guarded since in
  `docs/TEST_MIGRATE_CONNECTION_CHURN.md`.
- **R5. The residual at 3 lanes.** The spread alone left 4 failures in 144 rounds at 6 shard threads
  per CPU. They were not traced further. At 4 per CPU, 0 of 168 rounds failed (72 paired, 96 final).

## 6. Independent QA

Three reviewers worked from the diff, this document and the hosted logs, without this document's
conclusions as a brief: one on the code, one on every claim against the runs, one on guarantees and
scope. None found a blocking defect. Each finding was re-checked against the source or the logs
before it was applied.

| Finding | Re-check | Applied |
|---|---|---|
| `check_spread` sees one scenario, not all lanes: going back to 3 lanes on 4 CPUs, or dropping the lane offset, would still pass | Confirmed | Run-wide density check at start; it fails the old default (section 4) |
| `cpus.K.restart` was written but never read | Confirmed | A restarted node must report the same CPUs |
| The "spread, 3 lanes" total was 4/108; the cited runs give 4/144 | Confirmed | Corrected here and in AGENTS.md |
| Two cold-table as-is runs (27/72, 21/72) were left out of "before" | Confirmed | "Before" is 101/324 |
| Six earlier diagnostic runs were not reported; no disk-throttling row | Confirmed from their logs | Section 3.8 and three rows in section 1 |
| The 5 spread 2-lane failures of 3.1 had no signature | All 5 dropped packets for a full conntrack table | 3.1, and R4 made a separate defect with the cold-table assumption stated |
| R1 called this "a configuration hazard, not a defect" | F1's pause needs in-flight I/O 1 ms old, so a busy shard can starve another process | R1 reworded; safe envelope stated |
| "All 21 failed rounds" show the stall; it is 20 | Confirmed | Summary and 3.5 |
| "Every reconnect succeeds"; the quoted line was abridged | Connects were refused in 28 entries | Section 1 and 3.3 reworded |
| gdb counts and stacks; the 2 s window; tags; connections per run (110k to 250k); `degrade_ms` in 28 scenarios | Confirmed | Corrected in place |
| A sandbox refusing `sched_setaffinity` now fails the test | Confirmed by reading | Stated in section 4 |
| Comment claimed at most 4 per CPU even on 1 CPU | Confirmed | Comment corrected |

Not changed: `SNC_MIGRATE_LANES=0` or a filter matching nothing still prints `0 scenarios ... ok` (as on
main, outside this slice). After the changes: full `test_migrate` passes locally (67 scenarios, 2 lanes,
44 s), the restart scenarios pass, and the old 3-lane default exits 1 with the density message.

## Classification

**REFINE**: the `test_migrate` harness's CPU placement and default lane count.

**KEEP**: the runtime's default placement for one node per host, F1's pause, and the replication,
node-link, session and migration protocols.

## Evidence index

| Item | Where |
|---|---|
| Diag commits | `claude/project-thread-n8tebd`, bb0d15a to a11697c (`git log --oneline 2532f40..826b8f4`) |
| Diagnostic hosted runs | 43664766143, 43666006045, 43666376745, 43667963821, 43668079133, 43669097367 (3.8); 43670006106, 43671956993, 43674371376, 43676517093, 43678477178, 43680152298 |
| Verification commits | 19758d5, 5b6b715, 1d42cd8 (workflow `verify-migrate`, removed afterwards) |
| Verification runs | 43682094376 (spread, 3 lanes: 4/72), 43683480773 (1, 2 and 3 lanes paired: 0/108), 43685840387 (final default: 0/96) |
| Local experiments (block, leave and rejoin; SIGSTOP; hogs) | described in 3.7 and section 1; their code is in diag commit 484814e (`x36` in `tests/test_xnode.cpp`, `SNC_DIAG_STOP_STANDBY_MS` in `tests/test_migrate.cpp`) |
