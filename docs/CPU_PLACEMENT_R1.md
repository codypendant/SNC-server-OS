# R1: CPU placement with several SNC processes on one host (2026-11-15)

Status: **review, plus one defect fix (D1)**. The stacking itself (P1) is not changed here; the slices
proposed for it in section 5 wait for review.

Assignment: Codex's PR #128 review (`docs/handoff/PR128_Test_Migrate_Review_Approval.md`) asked for "a review
of SNC's multi-process CPU placement behavior" after connection churn (R4, PR #129), because "SNC's default
placement can still overload individual CPUs when multiple nodes share a host". It was recorded as R1 in
`docs/TEST_MIGRATE_ROOT_CAUSE.md` section 5.

## Summary

1. **Every thread SNC runs, and where it runs, was inventoried** (section 2), by reading the source and by
   sampling `/proc/*/task/*/status` every 50 ms while a primary/standby pair and seven multi-process test
   suites ran. Only two kinds of thread are on a single CPU: the pinned shard threads and the io_uring
   workers (`iou-wrk-*`) each shard's ring creates. Every other thread keeps the process's CPU set, except
   one: the catch-up-from-disk reader.
2. **D1, a confirmed defect, fixed here.** The catch-up reader (`repl::Sender::start_disk`) moved itself to
   CPUs `0 .. nprocs-1`, not to the process's CPUs. A process started with `taskset` or in a cpuset
   therefore ran its readers on CPUs it was never given, which may be another process's pinned CPUs.
   **Observed:** a primary under `taskset -c 2,3` called `sched_setaffinity(..., [0 1 2 3])` for its reader
   (strace, section 3). The new regression test fails on main (`reader ... runs on CPUs 0 1 2 3, not the
   process's 3`, exit 22) and passes with the fix.
3. **P1, the stacking itself, is confirmed and is a design limit, not a slip.** With no `--cpus`, every
   process puts shard *i* on the *i*-th CPU it is allowed, and no process knows of the others. Two
   `snc_server` processes under `taskset -c 2,3` both put shard 0 on CPU 2 and shard 1 on CPU 3
   (**observed**). An explicit `--cpus` that overlaps another process's is not detected either. Inside the
   deployment model (one node per host, one shard per core, `docs/BENCHMARKS.md`) this does not arise.
4. **P2, related, measured only.** Each shard's io_uring workers inherit the shard's single CPU, because
   `RuntimeConfig::io_worker_cpus` exists but nothing sets it (`snc_server` has no flag). In the PR #128
   failures the starved threads were the kernel's per-CPU `kworker/N:M-dio` and `ksoftirqd/N`, not
   `iou-wrk`, so no failure is attributed to P2. Recorded so a fix for P1 does not overlook it.
5. **R2 (the Python multi-process suites) measured:** see section 4.

No safety property depends on placement. Every effect found is liveness or performance.

## 1. Classification

| Item | Class | Why |
|---|---|---|
| D1 catch-up reader placement | **REFINE** (fixed) | It contradicted its own comment ("not on the shard's core ... behind the shards"): it widened past the process's CPUs |
| P1 default placement with several processes | **KEEP** for one node per host; **limitation** outside it | Correct inside the deployment model; outside it, nothing prevents stacking (section 5 proposes slices) |
| P2 io_uring worker placement | **KEEP**, open question | No failure attributed; changing it needs measurement first (benchmark every assumption) |
| F1's pause (PR #126) | **KEEP** | Unchanged; its limit (a busy shard with no 1 ms-old I/O never yields) stays as `TEST_MIGRATE_ROOT_CAUSE.md` R1 states it |

## 2. Inventory: where each thread runs

Source: `src/`, `tools/`, every `std::thread`, `sched_setaffinity` and `pthread_setaffinity_np`. Live check:
a 50 ms `/proc` sampler (comm, `Cpus_allowed_list`, nice) over the pair in section 3 and over
`test_replication`, `test_async_failover`, `test_health`, `test_fail_closed`, `test_baseline_move`,
`test_xnode_proc`, `test_ckpt_failover` (all passed, 581 s).

| Thread | Started by | CPUs | Evidence |
|---|---|---|---|
| Shard workers | `NativeRuntime` (`src/native/runtime.cpp` `Impl::run`) | one: `cfg.cpus[i % n]`, else the *i*-th CPU of the creating thread's set | source; sampler: one CPU each |
| io_uring workers `iou-wrk-<shard tid>` | the kernel, for each shard's ring | the shard's one CPU (inherited; `io_worker_cpus` unset) | sampler: `iou-wrk-1752` on CPU 2 with shard 1752 on CPU 2 |
| Catch-up reader (nice 10) | the shard thread (`Sender::start_disk`) | **before:** `0 .. nprocs-1`; **after:** the process's set (D1) | strace, section 3 |
| Main, admin loop and worker, checkpoint thread, move handlers, sampler | `snc_server` main thread and its children | the process's set (inherited) | sampler |
| Segment preparer, placement watcher, seed server | `Engine` on the constructing thread | the process's set | source (`Engine::recover`, `start`); sampler |
| `snc_witness`, `snc_ctl`, `snc_load` threads | their own processes | the process's set (never pin) | sampler |

No other thread is started from a shard thread, so D1 is the only thread that inherited a pinned CPU and had
to leave it.

## 3. D1: the catch-up reader

**Mechanism.** `start_disk` runs on the shard thread, so its new thread inherits that one CPU. The code then
set the thread's mask to CPUs `0 .. std::thread::hardware_concurrency()-1`. `hardware_concurrency` returned
the host's online CPU count here (4 under `taskset -c 2,3`, glibc 2.39), not the process's allowed set, and in any
case a count is not a set, so the mask named CPUs the process was never given.
The kernel accepts such a mask outside a cpuset (`taskset`, `sched_setaffinity` of a parent). Inside a cgroup
cpuset, it is intersected with the cpuset, so the defect shows only under `taskset`-style limits; under a
cpuset it was harmless by accident.

**Observed** (local, 4 vCPUs, kernel 6.18): primary and standby both under `taskset -c 2,3`, the standby
killed, 4 s of writes, `strace -f -e sched_setaffinity` on the primary, the standby restarted:

```
9565  sched_setaffinity(9565, 128, [0 1 2 3]) = 0
9565  setpriority(PRIO_PROCESS, 9565, 10) = 0
```

**Fix.** `Engine`'s constructor records the CPUs of the thread that constructs it, before the runtime starts
and pins its shards (`Engine::process_cpus_`). That is the set the runtime places shards from. It goes to
each shard's `Sender::Config::reader_cpus`, and the reader calls `repl::leave_shard_cpu(cpus)`, which sets
exactly that set (empty: the main thread's set, which no shard pins). Nice 10 is unchanged. If the set can
no longer be applied (a cpuset shrunk since start), the reader stays where it started, as before.

**Regression test** (`tests/test_replication.cpp` `test_reader_placement`, deterministic). The forked
primary restricts itself to every allowed CPU but the first, writes more than a reader's 16 MiB read-ahead on
each of its 4 shards past a checkpoint, and pauses replication. The standby starts, so every shard starts a
reader, and each reader fills its read-ahead and waits there. The test waits for that state on
`repl_disk_gauges()` (all 4 readers live and 64 MiB queued), not on time. It then checks that every nice-10
thread has exactly the process's CPUs and that there are 4. It unpauses and checks that the standby's log
matches. It skips, saying so, on a host with one CPU.

| Code | Result |
|---|---|
| main (28733cf) | `reader 3663 runs on CPUs 0 1 2 3, not the process's 3`, child exit 22, test fails |
| this change | `reader placement: 4 readers on the process's 3 of 4 CPUs, standby matches`; whole `test_replication` 5 of 5 passed |

## 4. R2: the Python multi-process suites

PLACEHOLDER_R2

## 5. P1: proposed slices (not started)

None of these is started. Each would be its own slice, in this order of value:

- **R1-a. Refuse or avoid a CPU another SNC process pinned.** Each shard takes a non-blocking `flock` on a
  per-CPU file (for example `/run/snc/cpu/<n>`, a runtime directory) when it pins. With no `--cpus`, the
  runtime takes the first allowed CPUs nobody holds; with `--cpus`, a CPU already held refuses the start
  (or warns: a decision for review). The kernel drops the lock when the process dies, so nothing goes
  stale. Limits to state: it sees only SNC processes that share the lock directory (not other containers or
  mount namespaces) and nothing that is not SNC.
- **R1-b. A pause that does not depend on the shard's own I/O age** (Codex's option (b) in
  `TEST_MIGRATE_ROOT_CAUSE.md` R1). It would protect other processes' completion threads, SNC or not. It
  needs measurement on the hosted 7.0 kernels first; R3, why those kernel threads go unscheduled, stays open.
- **R1-c. Say it in the deployment docs:** one node per host, or disjoint `--cpus` per process; the verified
  envelope on hosted 7.0 kernels is at most 4 pinned shard threads per CPU.
- **R2-a. Python launchers give each process its own CPUs,** as `test_migrate` does since PR #128, if section
  4 shows stacking past that envelope.
- **P2.** Measure before deciding whether `snc_server` should set `io_worker_cpus` (and to what).

Recommendation: R1-c with R1-a, since R1-a makes the product fail closed on the hazard R1 names, and R1-c
states the limit until then.

## 6. Independent QA

PLACEHOLDER_QA

## Evidence index

| Item | Where |
|---|---|
| Fix | `src/core/replication.{h,cpp}` (`leave_shard_cpu`, `reader_cpus`), `src/core/engine.cpp`, `include/snc/snc.h` |
| Regression test | `tests/test_replication.cpp` `test_reader_placement` |
| Inventory sampler | section 2 (a 50 ms `/proc` scan; not committed, its output is summarized above) |
