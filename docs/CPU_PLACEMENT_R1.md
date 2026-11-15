# R1: CPU placement with several SNC processes on one host (2026-11-15)

Status: **review, plus one defect fix (D1)**. The stacking itself (P1) and a second defect found in QA (D2)
are not changed here; the slices proposed for them in section 5 wait for review.

Scope: the native runtime, which is the product. The Seastar backend (`src/seastar/runtime.cc`, built only
for `snc_bench_seastar`, off by default) is out of scope; note that it passes `--cpuset 0..n-1` to Seastar
when no CPUs are given, the same class of mistake as D1.

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
   process puts shard *i* on the (*i* mod *k*)-th of the *k* CPUs it is allowed, and no process knows of the
   others. Two `snc_server --shards 2` processes under `taskset -c 2,3` both put shard 0 on CPU 2 and shard 1
   on CPU 3 (**observed**). An explicit `--cpus` that overlaps another process's is not detected either. Inside the
   deployment model (one node per host, one shard per core, `docs/BENCHMARKS.md`) this does not arise.
4. **P2, related, measured only.** Each shard's io_uring workers inherit the shard's single CPU
   (observed on kernel 6.18; io-wq's affinity rules have changed across kernels and the hosted 7.0 kernels
   were not checked), because `snc_server` never sets `RuntimeConfig::io_worker_cpus` (it has no flag; only
   `snc_bench --io-cpus` does). In the PR #128
   failures the starved threads were the kernel's per-CPU `kworker/N:M-dio` and `ksoftirqd/N`, not
   `iou-wrk`, so no failure is attributed to P2. Recorded so a fix for P1 does not overlook it.
5. **D2, a second defect, found in QA; fixed afterwards in its own slice, D2-a (section 7).** With neither `--shards` nor
   `--cpus`, `snc_server` runs `std::thread::hardware_concurrency()` shards (`tools/snc_server.cpp`,
   before D2-a): the host's online CPU count, not the process's set. Under `taskset -c 2,3` on this 4-CPU host
   one process runs 4 shards on 2 CPUs; in a 2-CPU cpuset on a 64-CPU host it would run 64 shards on 2
   CPUs (32 per CPU, far past the 4-per-CPU envelope). **Confirmed by reading**, then observed (section 7).
6. **R2 (the Python multi-process suites) measured:** see section 4.

No effect on safety was found; every effect found is liveness or performance. That is **inferred, not
shown**: starvation stretches pauses, and time-based mechanisms (standby read leases, witness failover,
`T_block`) assume pauses are bounded. This review did not test them under unbounded pauses.

## 1. Classification

| Item | Class | Why |
|---|---|---|
| D1 catch-up reader placement | **REFINE** (fixed) | It contradicted its own comment ("not on the shard's core ... behind the shards"): it widened past the process's CPUs |
| P1 default placement with several processes | **KEEP** for one node per host; **limitation** outside it | Correct inside the deployment model; outside it, nothing prevents stacking (section 5 proposes slices) |
| D2 default shard count from the host's CPU count | **REFINE** (D2-a, section 7) | Same count-is-not-a-set mistake as D1, in `snc_server` |
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
| Segment preparer, placement watcher, seed server | `Engine` on the thread calling `start()` | the process's set | source (`Engine::recover`, `start`); sampler |
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

**Fix.** `Engine::start_` records the CPUs of the calling thread before it starts the runtime
(`Engine::process_cpus_`). The shard workers are created by that call and inherit that set, so it is the
set the runtime places shards from. (A first version captured it in the constructor; QA 1 noted that
construction and start may be on different threads, so it moved.) It goes to
each shard's `Sender::Config::reader_cpus`, and the reader calls `repl::leave_shard_cpu(cpus)`, which sets
exactly that set (empty: the main thread's set). Nice 10 is unchanged. If the set cannot be applied (a
cpuset shrunk since start, or a host with more than `CPU_SETSIZE` = 1024 CPUs), the reader stays on its
shard's CPU and now says so on stderr. Not changed: with `--cpus` and no taskset the process's set
includes the shards' CPUs, so the reader may still share one with a shard, at nice 10, as on main.

**Regression test** (`tests/test_replication.cpp` `test_reader_placement`, deterministic). The forked
primary restricts itself to every allowed CPU but the first, writes more than a reader's 16 MiB read-ahead on
each of its 4 shards past a checkpoint, and pauses replication. The standby starts, so every shard starts a
reader, and each reader fills its read-ahead and waits there. The test waits for that state on
`repl_disk_gauges()` (all 4 readers live and 64 MiB queued), not on time. It then checks that every nice-10
thread has exactly the process's CPUs and that there are 4. It unpauses and checks that the standby's log
matches. It skips, saying so, on a host with fewer than 3 CPUs (with 2, the process's set would be the
shards' one CPU and a reader that never moved would pass) or when run under `nice` (readers are found by
nice 10).

| Code | Result |
|---|---|
| main (28733cf) | `reader 3663 runs on CPUs 0 1 2 3, not the process's 3` (3 CPUs: 1-3), child exit 22, test fails; reproduced by QA 1 |
| this change | `reader placement: 4 readers on the process's 3 of 4 CPUs, standby matches`; whole `test_replication` passed 5 of 5 (first version), 3 of 3 (final), 4 of 4 by QA 1 |
| end to end, `snc_server`, QA 3 | main: `sched_setaffinity(..., [0 1 2 3])` under `taskset -c 2,3`; fix: `[2 3]`, and `[1 3]` under `taskset -c 1,3`. Through a promotion (standby B under `taskset -c 0,2` promoted, then catching up a new standby): main `[0 1 2 3]`, fix `[0 2]`. The standby caught up in every arm |
| end to end, final code | `taskset -c 1,3`: `sched_setaffinity(..., [1 3])`, nice 10, both shards caught up, no "cannot leave" line |

## 4. R2: the Python multi-process suites

Method: the 50 ms `/proc` sampler over 12 suites that start `snc_server` clusters (`test_witness`,
`test_async_failover`, `test_control`, `test_health`, `test_fail_closed`, `test_s1`, `test_baseline_retry`,
`test_baseline_move`, `test_xnode_proc`, `test_baseline_ha`, `test_ckpt_failover`, `test_index_wire`; all
passed, 1037 s, local 4 vCPUs). Counted per sample: pinned shard threads of every `snc_server` from this
build on each CPU (io_uring workers not counted). 18,046 samples had at least one pinned shard.

| Most pinned shard threads on one CPU, in a sample | Samples |
|---|---|
| 1 | 3,022 |
| 2 | 8,990 |
| 3 | 2,289 |
| 4 | 3,721 |
| 5 | 1 |
| 6 | 23 |

Per CPU, the most ever seen: CPU 0: 6, CPU 1: 6, CPU 2: 2, CPU 3: 1. So the suites stack on the low CPUs, as
P1 predicts, and leave CPUs 2 and 3 almost unused. They stay within the 4-per-CPU envelope except for 24
samples (about 1.2 s in 15 minutes) at 5 or 6. The sampler did not record which suite those came from. None
of these suites failed here, and their hosted stability is not re-measured (as R2 said). This supports
R2-a as a hardening, not as the fix of an observed failure.

## 5. P1: proposed slices (not started)

None of these is started. Each would be its own slice, in this order of value:

- **D2-a. Default shard count from the process's CPU set** (`sched_getaffinity` count), not the host's.
  A separate defect slice with its own regression test.
- **R1-a. Refuse or avoid a CPU another SNC process pinned.** Each shard takes a non-blocking `flock` on a
  per-CPU file when it pins, opened `O_CLOEXEC` so no child inherits it. With no `--cpus`, the runtime takes
  the first allowed CPUs nobody holds; with `--cpus`, a CPU already held refuses the start (or warns). To
  decide in review: what happens when no free CPU is left without `--cpus` (refuse, or stack as today; the
  multi-process tests stack today, so refusing needs R2-a first), and the lock directory (`/run/snc` needs
  root; a fallback such as `$XDG_RUNTIME_DIR` means processes using different directories miss each other).
  The kernel drops the lock when the last holder of the file description exits. Limits: it sees only
  processes that open the same lock files (containers see each other only if they bind-mount the same
  directory), nothing that is not SNC, and covers pinned shards only, not readers, `iou-wrk` or the kernel's
  completion threads, which are what starved in PR #128.
- **R1-b. A pause that does not depend on the shard's own I/O age** (Codex's option (b) in
  `TEST_MIGRATE_ROOT_CAUSE.md` R1). It aims to protect other processes' completion threads, SNC or not. It
  needs measurement on the hosted 7.0 kernels first; R3, why those kernel threads go unscheduled, stays open.
- **R1-c. Say it in the deployment docs:** one node per host, or disjoint `--cpus` per process; the verified
  envelope on hosted 7.0 kernels is at most 4 pinned shard threads per CPU, measured under the q10 and
  `test_migrate` test workloads, not a production load.
- **R2-a. Python launchers give each process its own CPUs,** as `test_migrate` does since PR #128, if section
  4 shows stacking past that envelope.
- **P2.** Measure before deciding whether `snc_server` should set `io_worker_cpus` (and to what).

Recommendation: D2-a first (a defect, small), then R1-c with R1-a, since R1-a makes the product fail closed on the hazard R1 names, and R1-c
states the limit until then.

## 6. Independent QA

Three independent reviews of the first version (c4dc492). None found a blocker.

| Reviewer | Scope | Result |
|---|---|---|
| QA 1 | code and test: every Sender path, determinism, failure on main | Fix correct on every path that builds a Sender; the test waits on state, not time; reproduced the failure on main and 4 passes on the branch |
| QA 2 | this document against the source: every thread creation | Inventory confirmed: no thread but the reader is started from a shard thread; found D2 and the Seastar gap |
| QA 3 | end to end with `snc_server` and strace, before and after, including a promotion | Reader on all host CPUs on main, on exactly the taskset CPUs with the fix, in all 4 arms |

| Finding | Reviewer | Resolution |
|---|---|---|
| Default shard count from the host's CPU count (D2) | QA 2 | Recorded as D2, slice D2-a proposed; not fixed here (one defect per slice) |
| Seastar backend not in the inventory; it also passes `--cpuset 0..n-1` | QA 2 | Scope stated at the top; not changed |
| "No safety property depends on placement" was stated as fact | QA 2 | Reworded as inferred, with the time-based mechanisms named |
| R1-a's limits imprecise (flock follows the file, `O_CLOEXEC`, `/run` needs root, exhaustion rule, kernel threads not covered) | QA 2 | R1-a rewritten |
| Captured on the constructing thread, but shards inherit from the thread calling `start()` | QA 1 | Capture moved to `Engine::start_` |
| Test passes a reader that never moved on a 2-CPU host | QA 1, QA 2 | Test needs 3 CPUs, else skips saying so |
| Test fails spuriously under `nice -n 10` | QA 1 | Test skips, saying so, unless at nice 0 |
| Reader ignores a failed affinity call | QA 3 | Now logged on stderr |
| More than 1024 CPUs: the reader stays on its shard's CPU | QA 1, QA 3 | Stated in section 3; the runtime's own pinning has the same limit |
| P2 wording ("nothing sets it"), io-wq rule observed on 6.18 only, envelope's workload, "aims to", failure string | QA 2 | Reworded |
| `--cpus` without taskset: reader may share a shard's CPU at nice 10 | QA 3 | Stated in section 3; unchanged from main |
| Vector copy per reader start | QA 1 | Kept: once per catch-up |

After the changes: `test_replication` passed 3 of 3 on the final code, and the end-to-end strace check was
repeated (section 3).

## 7. D2-a: the default shard count (2026-11-15, after cody's PR #133 review)

Assigned by cody's PR #133 review (`docs/handoff/PR133_CPU_Placement_Review_Approval.md`) as its own slice.

**Root cause.** With neither `--shards` nor `--cpus`, `tools/snc_server.cpp` set the shard count to
`std::thread::hardware_concurrency()`, the host's online CPU count (glibc 2.39 here). The runtime then
places those shards round the process's allowed CPUs (`Impl::run`), so a process limited to *k* of *n* CPUs
ran *n* shards on *k* CPUs, *n/k* per CPU. **Observed** before the fix: under one CPU of 4,
`snc_server shards=4`.

**Fix.** The default is now the size of the process's affinity set (`sched_getaffinity` on the main thread,
which no shard has pinned yet). `--shards` and `--cpus` behave as before. If the set cannot be read, the
old count is used and a line on stderr says so. Nothing else changed.

**Regression test** (`tests/test_default_shards.py`, deterministic, about 3 s). Each case starts the server
in memory under a given CPU set, reads its startup line and stops it.

| Case | Before (28733cf + test) | After |
|---|---|---|
| S1: one CPU of 4 | `shards=4`, fails (also reproduced by QA 1 at ed47b61) | `shards=1` |
| S2: two CPUs that are not neighbours | (not reached) | `shards=2` |
| S3: unrestricted | `shards=4` | `shards=4` (unchanged) |
| S4: `--shards 3` or `--cpus` of 2, under one CPU | unchanged | unchanged |

S1 and S4 need 2 CPUs and S2 needs 3; a case that cannot discriminate is skipped, saying so.

**What this changes for an operator.** Only processes started without `--shards` and `--cpus` under a
limited CPU set (taskset or a cgroup cpuset) see a different count; on an unrestricted host the count is as
before. A CPU *quota* (`docker --cpus`, Kubernetes CPU limits, cgroup `cpu.max`) leaves the affinity set at
the whole host, so such a container still gets the host count: this slice does not cover quotas.

- **Pass `--shards` explicitly in deployments.** Every launcher in this repository (tests, `bench/lab/lab.py`,
  the pilot, the bench scripts, the README example) already passes `--shards` or `--cpus` (QA 2 checked each);
  `docs/CLUSTER.md`'s `--control` example elides its other flags.
- **A standalone node** restarted with its data directory under a set of a different size now gets a
  different count, and recovery remaps once, as when a host's CPU count changes. That restart routes every
  record on the control thread, so it is slower on large data. **Observed:** a directory made with 4
  shards, restarted under `taskset -c 3`, logged `recovery: checkpoint before serving (shard count changed)`
  and served with `shards=1`.
- **A primary and standby** must have the same count. Two such processes under sets of different sizes used
  to agree (both at the host count); now the standby is refused, and the refusal is easy to miss
  (**observed by QA 3**: primary unrestricted with 4 shards, standby under `taskset -c 2,3` with 2):
  - only the primary logs, once a second per shard: `standby refused shard N (another space, shard count or
    slot map)`, which names neither the failed check nor the two counts; shards past the standby's count log
    only `standby unreachable`; the standby logs nothing;
  - a sync primary without a witness then degrades and keeps acknowledging writes with one copy (139k in
    QA 3's run; STATUS `live=0 degraded=4`), as it does for any lost standby;
  - a `--seed-from` standby seeds the primary's checkpoint, remaps it to its own count and is then refused,
    so reseeding does not help. Restarting the standby with matching `--shards` fixed every case, and the
    promoted standby matched the primary.
- **`--cluster` and `--control` nodes** must match the shard count their group has in the placement map.
  A node that relied on the default, restarted under a smaller set, now fails at start. It aborts
  (`std::terminate`) rather than exiting cleanly, because `snc_server` constructs the `Engine` outside the
  `try` that turns refused starts into exit 1 (`tools/snc_server.cpp`). That abort is a pre-existing defect
  this default makes easier to reach; see D3 and D4 below.
- **Clients** that outlive a server restarted with another count fail to reconnect (`EPROTO`, "another shard
  count", `src/net/client.cpp`) and must be recreated. `snc_load` then does not record its in-flight
  transfers as in doubt, so its check fails (24 accounts in QA 3's run; the total was conserved). QA 3
  showed the same on the code before D2-a with an explicit `--shards 4` then `--shards 2`: pre-existing,
  now reachable by restarting under another CPU set.
- Not changed: an allowed set larger than `kMaxShards` (256) aborts the start, as the host count did.

**Found in QA, not fixed here (each its own slice, for review):**

- **D3.** In `--cluster` and `--control` mode the shard count is recorded in the group's map, but the default
  ignores it. The default there should come from the map, or a mismatch should be refused with a message.
- **D4.** A configuration the `Engine` constructor refuses (`std::invalid_argument`) aborts `snc_server`
  instead of exiting 1 with the reason, since the constructor runs outside the `try` around `start()`.
- **D5.** A shard-count mismatch between primary and standby deserves its own refusal, naming both counts and
  logged on both sides (today the standby folds it into "another space" and logs nothing), or a standby
  could refuse at start. A `--seed-from` standby could default to the seed's count.
- **Noted for investigation (QA 2):** after a shard-count remap, a standby resumes without the WAL tail CRC
  check (`tail_crc` is filled only on the non-remap path). Not shown reachable with diverged data.

**Independent QA of D2-a** (three reviews of 251d3fe; none found a blocker).

| Reviewer | Scope | Result |
|---|---|---|
| QA 1 | code and test | Ordering, fallback and casts correct; test fails on ed47b61 (`S1: one CPU (3) gave shards=4`), passes 4 of 4 |
| QA 2 | claims and deployment impact: every launcher, remap, replication, cluster mode | Every launcher passes `--shards` or `--cpus`; found the cluster/control abort (D3, D4), the client and HA effects, the quota limit, the remap tail-CRC note |
| QA 3 | end to end, before and after: startup matrix, remap 4→1→2→4 after SIGINT and SIGKILL, replication, seeding, live clients | Counts as designed; no data lost in any remap or replication case (check OK over all runs); found the quiet refusal and the single-copy degrade (D5) |

| Finding | Reviewer | Resolution |
|---|---|---|
| Cluster/control nodes now abort when the default differs from the map | QA 1, QA 2 | Stated above; D3 and D4 proposed |
| Primary/standby mismatch is quiet; a sync primary degrades to one copy | QA 3 | Stated above; D5 proposed |
| Clients fail across a count change; `snc_load` loses in-doubt transfers | QA 2, QA 3 | Stated above (pre-existing) |
| "a container" overclaimed: a CPU quota does not narrow the set | QA 2, QA 3 | Reworded in code, test and doc |
| Test could hang past its 30 s deadline; stdout not drained | QA 1 | Reads with a selector deadline; `communicate()` drains |
| Test passes vacuously on a 1-CPU host | QA 1, QA 3 | Exits 77, `SKIP_RETURN_CODE` |
| Silent fallback to the host count | QA 3 | Logged on stderr |
| `--cpus` tested twice | QA 1 | Simplified to if/else |
| Remap slower on large data; restart under any other size, not only smaller | QA 2 | Stated above |
| Tail CRC not checked after a remap | QA 2 | Noted for investigation |
| Sets over 256 CPUs abort with no hint; `bench/baseline_server` default; port race in test | QA 1, QA 3 | Not changed (pre-existing, bench-only, usual pattern) |

## Evidence index

| Item | Where |
|---|---|
| Fix | `src/core/replication.{h,cpp}` (`leave_shard_cpu`, `reader_cpus`), `src/core/engine.cpp`, `include/snc/snc.h` |
| Regression test | `tests/test_replication.cpp` `test_reader_placement` |
| D2-a fix and test | `tools/snc_server.cpp` (default `n_shards`), `tests/test_default_shards.py` |
| Inventory sampler | section 2 (a 50 ms `/proc` scan; not committed, its output is summarized above) |
