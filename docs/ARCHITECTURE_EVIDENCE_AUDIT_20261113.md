# SNC Server OS: architecture and evidence audit (PR #121)

**Baseline:** `main` at `23f4a0c` (PR #120 merged). **Date:** 2026-11-13 UTC.
**Assignment:** `docs/handoff/PR120_Capacity_State_Result_Review_and_PR121_Audit_Assignment.md`.
**Nature:** audit only. No production code, test, benchmark or bench-config file was changed; no experiment was run.
Fixes that are outside this documentation scope are *documented and listed for authorization*, not applied.

## How this audit was done, and how far to trust it

- Five read-only reviewers each took a scope (object model and scheduling; durability and recovery; replication, cluster and
  events; runtime, memory and operations; experimental evidence). Their working notes are in
  `docs/audit-20261113-working-notes/` (A to E). They are **agent-produced**. A claim in them is only as strong as the
  file:line it cites.
- I (Claude, the author of this report) **re-verified directly** the items marked **[V]** below, by reading the cited code or
  running a command. Items without **[V]** are **reviewer-reported from reading the cited lines; not independently re-checked by
  me.** Treat them as leads with evidence, not as established facts.
- I ran a clean build of `main` in a scratch directory (not `build-s6`, which holds the frozen binaries) and the full `ctest`:
  **47 of 49 passed.** `test_pilot` fails (known cause, see section 8). `test_intake` failed once under `ctest -j4` and
  **passed 3 of 3 when run alone**; its cause is undiagnosed (the first failure's log was overwritten). Log:
  `docs/audit-20261113-working-notes/ctest-clean-build-main-23f4a0c.log`. Passing tests are evidence for the specific scenario
  tested, not for any wider guarantee.
- The project's three-way verification (author, reviewer, QA) is **not** satisfied by this audit. The reviewers and I are
  the same model family, and my checks were spot checks. QA verification of the **[V]** items and of the milestone
  recommendations is still needed.

Classification vocabulary (from the assignment): **Verified**, **Partially verified**, **Implemented, unverified**,
**Planned**, **Unknown**. Where none fits, the row says so rather than stretching a label.

---

## 1. Executive summary

**What exists.** SNC is a single-process, shard-per-core, in-memory object store with a per-shard WAL plus fuzzy checkpoints
(`src/core/`), a native pinned-thread runtime (`src/native/`), a TCP front end with sessions and resend (`src/net/`), per-group
primary/standby replication with Paxos-witness-driven failover (`src/core/replication.cpp`, `tools/snc_witness.cpp`), a
multi-group placement map with a fail-closed slot-migration protocol, and published gauges. One application type (`Account`)
is implemented. About 18.7 k lines of C++ in `src/` and `include/` (the server and tools are additional), and 49 registered tests.

**What is demonstrated.** Under `SIGKILL`/`SIGSTOP` on one loopback machine (and one 3-VPS lab run on 2026-11-04):
acknowledged durable writes survive restart; synchronous-pair failover loses no acknowledged write; an old primary is fenced
when it contacts a member; a 67-scenario migration failure matrix never showed two serving owners; transfers conserve
balances. On the R640, Profile A serves an objective rate of 654,569 ops/s at p99 about 0.93 to 1.0 ms with closed-loop
capacity about 4.0 to 4.3 M ops/s.

**What is not demonstrated.** Power-loss durability; WAL I/O-error and disk-full behavior; any ordering guarantee between
requests; a defined "event" abstraction; exactly-once beyond a session horizon or across a slot move; HA safety under real
network faults or on more than one host after D.1/D.2/S1; NUMA-local placement enforced by code; any second application type;
any schema evolution; any security boundary; any comparative result against a matched competitor.

**The five findings that matter most**

1. **Durability evidence is crash-of-process evidence only.** Power loss is explicitly untested, WAL EIO/ENOSPC has no injection
   test, and a corrupted record in the *last* WAL segment is dropped together with every later acknowledged record and counted
   only as `torn_tails` **[V]** (`src/core/storage.cpp:396-420`). Section 6, R1.
2. **Request ordering is unspecified and one reorder path exists in code.** A batchable cross-shard EXEC is held until the
   end-of-iteration flush while a non-batchable request (version-conditioned, large args, create, delete) to the same shard
   goes straight to the ring and can overtake it **[V]** (`src/core/shard.cpp:1470-1531`). No test and no document covers it.
   Section 6, R2.
3. **There is no event system.** The only asynchronous mechanism is the follow-up/outbox. It is at-least-once re-emission from
   durable state with hand-written idempotence per type, has no timer (re-emission only on restart, link-up, map change,
   unseal), and has a silent drop path. Section 3.13.
4. **No security boundary and no regression gate.** No authentication or TLS on any port; the admin port (`PROMOTE`, `MOVE`)
   binds `INADDR_ANY` **[V]** (`src/core/engine.cpp:1234`, `src/core/node_link.cpp:203`). There is no CI **[V]**, one test
   is red by a stale assertion, and one is flaky under load.
5. **The performance campaign has not produced a go/no-go signal, and none of its open questions blocks engineering.** The
   comparative phase is gated on a `CRITERIA.md` that does not exist **[V]**. The PR #120 recvfrom "instrument defect" is
   substantially a denominator-definition error in the preregistered check, not evidence of an observer effect (section 7.3).

**Correction to a premise in the assignment.** The brief lists "PostgreSQL persistence". **SNC has none [V].** A grep of
`CMakeLists.txt`, `src/`, `include/`, `tools/` for `postgres|libpq` finds nothing. PostgreSQL appears only as a *comparison
baseline* in `bench/conventional/`, `docs/CONVENTIONAL.md` and result CSVs. SNC persistence is its own WAL + checkpoint +
A/B-superblock on local files.

**Recommendation.** Do not run another performance experiment. Three milestones, in this order: **M1** durability fault-model
closure; **M2** delivery, ordering and application-surface closure; **M3** operability and regression baseline (CI, security
posture, startup self-reporting). A test-only first slice for M1 is in section 10.

---

## 2. Current architecture inventory

| Area | What is in the repository | Where |
|---|---|---|
| Object identity | 128-bit ObjectID (8-bit format, 56-bit lease, 64-bit seq); XXH3-64 hash; 4096 slots; owner `slot % n_shards` | `include/snc/object_id.h`, `slot_map.h`, `src/core/object_id.cpp` |
| Execution | Compiled-in types and ops; two-phase op (validate then apply); `OpCtx::local()` same-shard peer; follow-up `emit` | `src/core/shard.cpp:540-711`, `registry.cpp`, `include/snc/snc.h` |
| Scheduling | One pinned pthread per shard, full mesh of SPSC rings with overflow deques, cross-shard batching up to 32 | `src/native/runtime.cpp`, `src/core/shard.cpp:1456-1640` |
| Durability | Per-stream WAL (CRC32C, group commit, `RWF_DSYNC`), fuzzy per-shard checkpoint, manifest, A/B superblock | `src/core/storage.cpp`, `engine.cpp:1395-1549`, `shard.cpp:2318-2560` |
| Checkpoint control | S1 admission, S2 pacing, S3 DSYNC write window, S4 sync tokens | `shard.cpp:282-300`, `ckpt_pacer.h`, `sync_tokens.h` |
| Replication / HA | Primary to standby WAL stream, term fencing, sync/async, witness (3/5 Paxos) with READY rule | `src/core/replication.cpp`, `tools/snc_witness.cpp`, `tools/paxos.h` |
| Cluster / placement | Slot to replication-group map with epoch, node links (wire v2), links-generation, cluster client | `src/core/cluster.cpp`, `placement.cpp`, `node_link.cpp`, `src/net/cluster_client.cpp` |
| Migration | Seal, staged digest-checked copy, epoch raised at commit, abort; fail-closed serving proof | `shard.cpp:3915-4170`, `tools/migrate.h` |
| Network | epoll `ConnSet` per shard, wire v0 (48-byte headers, little-endian), sessions, resend, failover client | `src/net/` |
| Observability | Seqlock gauge blocks; `HEALTH`/`GAUGES`/`STATUS`/`RESOURCES` admin verbs; `SNC_TRACE` stderr stopgap | `include/snc/gauges.h`, `tools/admin_loop.h`, `tools/snc_server.cpp:1050-1200` |
| Application | One type, `Account` (create, delete, deposit, transfer, index) | `src/types/account.cpp` |
| Alternate runtime | Seastar backend, benchmark-only link target | `src/seastar/runtime.cc`, `CMakeLists.txt:38-48` |
| Not present **[V]** | PostgreSQL code, CI (`.github`, `scripts/`), `CRITERIA.md`, systemd/Docker/install targets, NUMA binding calls, auth/TLS | greps recorded in section 3 |

Intent versus implementation differ in these places (detail in section 3): "events" (no such concept; follow-ups only),
"transactions" (a saga, per `docs/TRANSFERS.md:26-27`), "NUMA locality" (recorded, never enforced), "Seastar interchangeable"
(compiles and benchmarks, but has no tests), "frozen hash for persisted placement" (no pinned test vector).

---

## 3. Subsystem evidence matrix

Fields per row: **R** intended responsibility, **I** implementation, **T** tests and evidence, **D** demonstrated,
**U** unverified, **L** limits and risks, **N** next action. Citations are file:line on `23f4a0c`. **[V]** = I re-checked.

### 3.1 ObjectID, slot indirection, LocalMap: **Verified** (for the behaviors tested)
- **R** Stable, location-free identity (invariant 2); constant-time owner lookup.
- **I** `include/snc/object_id.h:20-60`, `src/core/object_id.cpp:11`, `include/snc/slot_map.h:15,26`, `include/snc/local_map.h:104-125`.
- **T** `tests/test_object_id.cpp` (10k unique IDs, shard balance within 3% over 400k), `test_local_map.cpp` (1M random ops against `std::map`), `test_execute.cpp`.
- **D** Uniqueness within one minter, even slot distribution, LocalMap correctness against a reference model.
- **U** The hash is "frozen" for persisted placement, but **[V]** `tests/test_object_id.cpp:53` only compares two constructions of the same ID; **no golden hash value is pinned**. Cross-process lease disjointness lives in the control plane.
- **L** Shard lease is `lease_base + shard_id` (`engine.cpp:66`), so the minting shard leaks into the ID (soft tension with invariant 2; nothing routes on it today). LocalMap growth rehashes in one step (`local_map.h:12-15`). A duplicate mint aborts (`shard.cpp:731-734`).
- **N** Pin a golden hash vector; add a lint that nothing routes on lease or seq.

### 3.2 Registry, types, execute path: **Partially verified**
- **R** Compiled-in type and op descriptors; validate-then-apply.
- **I** `src/core/registry.cpp:17-57`, `shard.cpp:540-711`, `src/types/account.cpp`.
- **T** `test_execute.cpp`, `test_intake.cpp` F10. All against `Account`.
- **D** Status paths, conditional ops, exact balances after N deposits across 4 shards.
- **U** Any second type; "registry read-only after start" is not enforced (`registry.cpp:78-81` is a mutable process-wide static); apply failing after validate is a contract enforced only by debug checks and abort.
- **L** `Registry::type()` is a linear scan called per op (`registry.cpp:61-66`, `shard.cpp:632`). Contract violations abort the process in release (`shard.cpp:57-60`). Recovery aborts on any object image that fails `type->check` (`shard.cpp:1798-1802`), so a CRC-valid but bad image would crash-loop a node. No schema migration (`engine.cpp:1011`).
- **N** Add a second toy type through the public descriptor API; add a registry freeze; dense type lookup.

### 3.3 Lifecycle and transaction boundaries: **Partially verified**
- **R** Create, mutate, delete lifecycle; atomic units.
- **I** The only atomic unit is one object per op, plus an optional same-shard peer, implemented as one WAL group chained by `kRecGroupNext` (`shard.cpp:690,1241`; `storage.cpp:417-435`). Cross-shard and cross-group work is a **saga with an outbox, not a transaction** (`docs/TRANSFERS.md:26-27`). Mutations are APPLIED in memory first and acknowledged on durability; creates always wait for durability (`shard.cpp:782`).
- **T** `test_transfer.cpp` (conservation after SIGKILL, torn-group drop, remap), `test_execute.cpp`, `test_intake.cpp` F6.
- **D** All-or-nothing same-shard group on recovery; conservation across restart in the listed scenarios.
- **U** Cross-shard atomicity does not exist (a reader can see the debit before the credit; `docs/CORE_CONTRACTS.md` section 5). No multi-object, snapshot or version-pinned read. Local follow-ups skip the durability wait on a WAL-prefix argument (`shard.cpp:2961-2966`) that is reasoned, not isolated by a test.
- **L** Session records store wall-clock `::time(nullptr)` (`shard.cpp:1236`), so replay is not deterministic for TTL. Sessions do not move with slots.
- **N** State "saga, not transaction" wherever "two-object ops" is listed; add a test that an apply violating its validate contract is caught.

### 3.4 Scheduling, workers, cross-core operations: **Partially verified**
- **R** Shard-per-core, share-nothing, pinned workers, SPSC rings (invariants 3 and 4).
- **I** `src/native/runtime.cpp:119-170,440-500`; `shard.cpp:170-244` (entry), `1456-1640` (batching); `engine.cpp:145` (flush per loop iteration).
- **T** `test_runtime.cpp` (per-pair FIFO, no loss with forced overflow), `test_execute.cpp` (75% remote), `test_intake.cpp`, `test_lifecycle.cpp`. `test_gauges.cpp:215` is the only test touching `batch_remote=false`.
- **D** Ring-level FIFO and no loss; owner-thread execution by construction.
- **U** Op-level ordering (see R2). Scaling beyond a few shards: quoted results are 4-vCPU VM, plus R640 loopback. The slot map is static (`slot_map.h:12`; remap only at recovery when shard count changes, `engine.cpp:1020-1023`).
- **L** A single-threaded shard loop means any long operation stalls its shard (the checkpoint begin snapshot is about 27 ms on shard 0, section 7). The cluster map is never freed: every adopted placement is pushed into `maps_` (`engine.cpp:1604-1606`). Must-deliver messages use an unbounded overflow queue (`runtime.cpp:128`).
- **N** Document an ordering contract; test the batched/unbatched interleaving; free superseded cluster maps.

### 3.5 WAL: **Partially verified**
- **R** Per-shard-stream group-commit log; a durable ack means the record is on disk.
- **I** `src/core/storage.cpp:576-640` (flush, `durable_lsn` advances only in `flush_done`, 810-826), `343-420` (recovery rules), `native/runtime.cpp:177-181` (`RWF_DSYNC`). Spare segments are renamed **without a directory fsync** **[V]** (`storage.cpp:700-706`); recovery adopts a spare by its header.
- **T** `test_crash.cpp` (SIGKILL, 4 rounds, every acked write present), `test_persistence.cpp` (torn tail, lost spare rename), `test_wal_capacity.cpp` (runs with `sync=false, direct=false`, so it says nothing about disk durability).
- **D** No loss of acknowledged durable writes across process SIGKILL on one machine and filesystem; simulated torn tail and lost rename handled.
- **U** Power loss and fsync ordering (`docs/STATUS.md:510-511`: "power loss is not tested"; `docs/FAILURE_MODEL.md` H3, S5 UNPROVEN); real partial-sector writes; WAL EIO/ENOSPC (only handler is `storage_fatal` abort, `storage.cpp:816-819`, no injection test).
- **L** **[V]** A bad or out-of-sequence record ends the segment and increments `torn_tails` (`storage.cpp:396-420`). For the last segment this silently discards that record and every later acknowledged record; `FAILURE_MODEL` lists this as S6, a known gap. `wal_sync=false` (`storage.h:141`) still reports acks as durable. No WAL disk-space accounting.
- **N** M1 (section 9).

### 3.6 Checkpoint, recovery, superblock: **Partially verified** (strongest evidence in the repository)
- **R** Bound recovery time; commit order protects the previous checkpoint.
- **I** `engine.cpp:1395-1549`: fsync checkpoint directory; write manifest (tmp, fdatasync, rename, fsync directory; `storage.cpp:299-320`); write the alternate A/B superblock (`storage.cpp:238-251`); only then delete the old checkpoint and covered WAL. Corrupt parts or manifest are `storage_fatal` (`storage.cpp:422-450`).
- **T** S3 T6/T7/T18 (`test_s3_writeback.cpp:622-756,1047-1075`), S4 T5/T6 (`test_s4_sync_tokens.cpp:669-760`), S2 crash positions (`test_s2_pacing.cpp:714`), `test_persistence.cpp` B/C/D. SIGKILL at in-flight, partial, complete and before-commit points; injected `-EIO` and short write through debug hooks.
- **D** A crash at any checkpoint step recovers from the correct generation (SIGKILL model); checkpoint I/O failure aborts before commit.
- **U** No test corrupts or tears the superblock, manifest or a part; `read_superblock` CRC and generation selection (`storage.cpp:200-218`) have no direct test. Failures are injected as return values, not real EIO.
- **L** Checkpoint failure aborts the process; there is no degraded-serving path. Clean stop takes no checkpoint (restart replays the WAL; `engine.cpp:218-243`). Recovery reads the whole segment holding `start_lsn` (no seek index).
- **N** M1.

### 3.7 Admission S1, pacing S2, writeback S3, sync tokens S4: **Partially verified**
- **S2 controller logic: Verified** as a pure function (`ckpt_pacer.h`, `test_s2_pacing.cpp` synthetic traces). Any durability or latency *benefit* is **Implemented, unverified**; the design calls every number a hypothesis (`docs/S2_PACING.md` section 5).
- **S1** refuses `E_OVERLOADED` past the WAL buffer or waiter budget (`shard.cpp:282-300`; `test_s1_admission.cpp`). It protects RAM, not disk: disk exhaustion is not handled.
- **S3** memory bound, window bound and out-of-order completion tested; real-disk benefit not shown by tests (the campaign measured it, section 7).
- **S4** bound, fairness and crash cases tested; the default `K=0` (unlimited) means the shipped default does not use the mechanism.

### 3.8 "PostgreSQL persistence": **not present**
Verified absent **[V]**. See the executive summary. Recommendation: remove it from the vocabulary of the program, or state explicitly that
it is a baseline only. If a PostgreSQL-backed persistence option was intended, it is not even a documented plan in this tree.

### 3.9 Replication, term fencing: **Partially verified**
- **R** One primary streams per-shard WAL to a standby; terms fence an old primary.
- **I** `src/core/replication.cpp:667-669` (receiver refuses older term), `468-470` (sender fenced), `shard.cpp:920-922,2143-2170` (withdraw), `2252-2271` (sync degrade, witness-gated only if `--witness`).
- **T** `test_replication.cpp` (real child processes, 42 kill/stop sites), `test_cluster.cpp`, `test_async_failover.py`, `test_health.py`, lab run 2026-11-04.
- **D** Sync-pair failover under SIGKILL/SIGSTOP loses no acked write; old primary fenced on contact; async pair *does* lose acked writes (500 of 500 in a characterization test that asserts the current behavior).
- **U** Power loss; packet loss, jitter, flapping; clock-drift safety of the standby read lease; stale reads from a cut-off old primary (`docs/INTERACTION_MATRIX.md:52`, UNTESTED).
- **L** Fencing is contact-driven. Without `--witness` the degrade gate is null and degrade is timeout-only (`shard.cpp:2256-2257`), a split-brain-capable window by design. **[V]** the witness has no async check (`grep -c async tools/snc_witness.cpp` returns 0), so an async standby can be promoted stale (R08). A fenced primary needs a manual `--rejoin`.
- **N** Refuse automatic promotion of async pairs loudly; test partitioned-old-primary reads; forbid or flag no-witness degrade in sync mode.

### 3.10 Witness and Paxos HA authority: **Partially verified** (narrow claims Verified)
- **I** `tools/snc_witness.cpp`, `tools/paxos.h`; READY rule (`docs/SYNC_READINESS.md`); HEALTH decoupled from shard threads (`snc_server.cpp:1147`).
- **T** `test_witness.py` (random cuts; never both DEGRADE-OK and PROMOTE; uses **fake** primary/standby admin ports), `test_ready_order.py` (real servers, delayed-READY race), `test_witness_hardening.py`, `test_paxos_ballots.py`, `test_sync_readiness.py`.
- **U** No model checker or Jepsen-style history checker found (Unknown whether one exists elsewhere). Paxos ballots after a clock step-back (F8). Loss of acceptor state (F9, "not solved for enterprise"). Two carried flakes (a Paxos `c2b` deadline and an F8 links-up timeout) are unexplained (`docs/STATUS.md` about line 118-121).
- **L** A live but wedged shard is never failed over (FAILURE_MODEL P2, known limitation).

### 3.11 Multi-group cluster, placement, node links: **Partially verified**
- **I** `cluster.cpp`, `placement.cpp`, `node_link.cpp` (wire v2), `links_generation.cpp`, `net/cluster_client.cpp`.
- **T** `test_cluster.cpp` (real processes), `test_xnode.cpp` (engines **in one process** behind a fault-injecting proxy, a simulation of network faults), `test_xnode_proc.py` (real processes, four scenarios), `test_fail_closed.py`, `test_control.py`. **`test_placement.cpp` covers text encoding and witness CAS only** (header lines 1-16); it is not evidence for routing, ownership or migration.
- **U** More than two members per group; dynamic membership; multi-host beyond the one 3-VPS lab, which predates D.1, D.2, S1 and HEALTH, so whether its results still hold on `main` is Unknown.

### 3.12 Migration and transfers: **Partially verified overall; Verified for the tested matrix**
- **I** `shard.cpp:3915-4170`, `tools/migrate.h`, stage chain, serving proof.
- **T** `test_migrate.cpp`: 67 scenarios (8 failure kinds x 7 stop points + 11), real node processes plus three witnesses, SIGKILL of source/destination primary/standby/witness, a double-ownership probe, conservation check. Also `test_stage_chain.cpp` (in-process), `test_fail_closed.py`, `test_baseline_move.py`.
- **D** The probe never saw two serving owners; acknowledged transfers exact; unproven slots fail closed.
- **U** The probe is sampled, so no overlap is an observation, not a proof. Repair of a failed-closed slot is not built. Witness-majority loss mid-move (FAILURE_MODEL C6).
- **L** Sessions are per-shard objects and do not move with slots; the cluster client uses a separate client and sequence per group (`cluster_client.cpp:80-90`). A resend of an applied-but-unanswered request that lands on the new owner after a map refresh would be a new request, hence a double apply. `docs/CORE_CONTRACTS.md:313-322` labels this "INFERRED, UNPROVEN"; `test_baseline_move` q2 proves only the resend-reaches-source branch.
- **N** Write and run the cross-group resend test, or carry the session record on move (M2).

### 3.13 Event delivery, acknowledgement, ordering: **Implemented, unverified** (as a general guarantee)
- **There is no first-class event concept.** Only the follow-up exists: `OpCtx::emit`, `NodeLinks::Frame` (`node_link.h`), and per-object outbox/inbox state in the `Account` type (`src/types/account.cpp:140-345`). No publish/subscribe, event log, event IDs or per-subscriber ordering.
- **Delivery:** at-least-once re-emission from durable object state. Exactly-once *effect* exists only because each op type is hand-written idempotent (`account.cpp:257-285`); the engine does not enforce that for other types. There are no transport acks; END/SYNC frames (`node_link.h:13-17`) are accounting, not delivery confirmations.
- **Re-emission has no timer.** It runs on start, link bind, map change and unseal (`shard.cpp:1858,2085-2092,3768-3775,3812`). `emit_result` (`shard.cpp:3525-3546`) handles only three failure statuses and otherwise falls through, leaving state unsettled until the next trigger. `CORE_CONTRACTS` O-2 states this ("Today: not guaranteed").
- **Ordering:** per-TCP-link FIFO only. Re-emission can reorder or duplicate. Op-level ordering has the hazard in R2.
- **Resend and refusal:** invariant 18 (resolve duplicates before a resend-inviting refusal) holds at the places checked (`shard.cpp:177-221,430-436,558-566`; `test_resend_refusals.cpp`, in-process only). Whether the cross-shard `owner_refuses` paths (`shard.cpp:1585-1594,1716-1722`) come after the replay check is **Unknown**. Session expiry is wall-clock and checkpoint-only, so a duplicate applies again after expiry (R11, known).
- **N** Decide whether an event abstraction is intended; if not, stop using the word. Add a timer-driven resume or prove every drop is paired with a trigger.

### 3.14 Runtime and SRI: **Partially verified**; Seastar: **Implemented, unverified**
- **I** One pthread per shard, `sched_setaffinity`, full ring mesh, eventfd/poll idle, per-shard io_uring (`runtime.cpp:440-500`). The Seastar backend (`src/seastar/runtime.cc`) is built only with `-DSNC_SEASTAR=ON` and only `snc_bench_seastar` links it. `snc_server` and every pilot tool use native only.
- **T** `test_runtime.cpp` (prints but does not assert pinning, `:103`). **No ctest covers Seastar**; it was benchmarked (`matrix_seastar.csv`, 4-vCPU VM).
- **U** Seastar functional correctness; `sri_progress` is unsupported on Seastar, so HEALTH decoupling is native-only.
- **N** Add a backend conformance test, or mark Seastar benchmark-only in `docs/STATUS.md`.

### 3.15 Network front end and admin/control: **Partially verified**
- **I** `src/net/`, `tools/admin_loop.h`, `tools/snc_server.cpp:1050-1200`.
- **T** `test_net.cpp`, `test_admin_loop.cpp`, `test_gauges_admin.py`, many process-level tests.
- **D** Owner forwarding, session replay, HEALTH/GAUGES answering while a shard is stalled.
- **L** **No authentication, TLS or ACL anywhere. [V]** The admin and node-link listeners bind `INADDR_ANY` (`engine.cpp:1234`, `node_link.cpp:203`), and `PROMOTE`/`MOVE` are unauthenticated plain text (`snc_server.cpp:1197-1200`). IPv4 only. Listener and `epoll_ctl` failures abort the process; no cap on accepted connections (`conn_set.cpp:23-25,75-90`).
- **N** Bind to a configured address; document the trust boundary; connection cap; malformed-frame fuzz test.

### 3.16 Application integration: **Implemented, unverified** (as a developer surface)
- **I** The only application-facing API is the in-process C++ API (`include/snc/snc.h`, `registry.h`). Types are compiled in; `snc_server` hardwires `account::register_type()` (`tools/snc_server.cpp:545`). No plugin loader, IDL or SDK; clients speak raw op ids and byte arguments.
- **T** Every test uses `Account`. No test registers a second type.
- **L** A schema version or name change refuses to start (`engine.cpp:1011-1015`); only additive new types are accepted. No upgrade hook.
- **N** M2.

### 3.17 Memory, NUMA, affinity, hugepages: **Partially verified**; NUMA enforcement in code: **Planned (not implemented)**
- **I** Pin-then-first-touch; per-shard mimalloc arena; `map_pages` with HUGETLB to THP to 4 KiB fallback (`src/core/pages.cpp:25-65`).
- **Verified in this review [V]:** no `mbind`, `set_mempolicy`, `numa_alloc` or `numa_bind` exists in `src/`, `include/` or `tools/`. `numa_node` is recorded (`runtime.cpp:469`) and never consulted; the allocator receives `numa_node=-1` (`allocator.cpp:25`). Locality in the R640 procedure comes from an external `numactl` wrapper (`bench/pilot.py:512`) and `numa_balancing=0`.
- **L** A failed pin sets `topo.pinned=false` with no log (`runtime.cpp:464`) and the server never reports it. The hugepage downgrade is silent, and `test_pages.cpp:20` accepts the fallback, so the HUGETLB path is not proven exercised. `heap_limit_bytes` (per-shard cap, `E_NO_MEMORY`) is never set by `snc_server`, any bench or any test: **dead in every shipped path**.
- **N** Report pin state and page mode at startup and in HEALTH; wire or delete the memory limit; if NUMA locality is to be a product claim, add enforcement and a `numa_maps` check.

### 3.18 Observability: **Partially verified**
- **I** Seqlock gauge blocks (`include/snc/gauges.h`), admin verbs, `SNC_TRACE=1` stderr stopgap. The ring-buffer tracing design is "not started" (`docs/TRACING.md:162`). No metrics endpoint, structured log or log levels.
- **T** `test_gauges.cpp`, `test_gauges_admin.py` (responsiveness under a stalled shard), `test_admin_loop.cpp`.
- **U** Cost of tracing when disabled is "INFERRED (not measured)"; pin, NUMA and page-mode state are not in the gauges.

### 3.19 Failure handling and operational lifecycle: **Partially verified**
- **I** About 50 command-line flags, no config file; SIGINT/SIGTERM orderly stop; SIGUSR1 promotes a standby; SIGHUP is not handled (`snc_server.cpp:739-745,1295-1330,1480-1490`). Many fatal paths are `std::abort()`.
- **Absent [V]:** CI, systemd unit, Dockerfile, packaging, `install()` target, config file, metrics export, startup self-checks, backup/restore tooling, disk-space alarms. TSAN is documented in the README, but no `build-tsan` exists, so whether TSAN has ever run is Unknown.

---

## 4. Verified architectural guarantees

Each is bounded by the model stated. None extends beyond it.

1. **Process-kill durability, single machine.** Every acknowledged `DURABLE` write survives `SIGKILL` and restart (`test_crash.cpp`, `test_persistence.cpp`).
2. **Checkpoint commit ordering under process kill.** A kill at any checkpoint step recovers from the correct generation; checkpoint I/O failure injected as a return value aborts before commit (S3/S4 tests).
3. **Atomic same-shard group.** A torn WAL group is dropped whole on recovery (`test_transfer.cpp`, `test_persistence.cpp`).
4. **Sync-pair failover without acknowledged-write loss** under SIGKILL/SIGSTOP of a primary (real processes, loopback), and fencing of an old primary when it contacts a member.
5. **Witness safety under random cuts:** never both DEGRADE-OK and PROMOTE (`test_witness.py`, with fake node ports); delayed-READY promotion refused (`test_ready_order.py`, real servers).
6. **Fail-closed migration, 67-scenario matrix:** the sampled probe never saw two serving owners; transfers conserved.
7. **Request replay by session** where the duplicate reaches the owner shard that holds the session (single group).
8. **Control-plane responsiveness:** HEALTH and GAUGES answer while a shard is stalled (`test_gauges_admin.py` T8c).
9. **Object lookup correctness and distribution** (section 3.1).

## 5. Unverified assumptions and guarantees

- Durability under power loss, fsync reordering, real torn sectors, WAL EIO/ENOSPC, bit rot (last segment), superblock/manifest damage.
- Any request ordering guarantee between a client and a shard, or across shards (R2).
- Delivery completeness of follow-ups without a restart or link event (no timer).
- Exactly-once across a slot move, across groups, or beyond the session horizon.
- HA behavior under packet loss, jitter, flapping, clock steps, partition with a serving old primary, async promotion.
- Any multi-host result on current `main`.
- NUMA-local placement, as a code guarantee; huge-page use, as asserted behavior.
- Seastar correctness; behavior with a second application type; schema evolution.
- Security of any listener.
- Scaling beyond a handful of shards, or a hot-key distribution (single-owner ceiling).
- Any comparative advantage over a matched competitor (section 7).

---

## 6. Cross-subsystem risks and dependencies

| ID | Risk | Evidence | Couples |
|---|---|---|---|
| **R1** | Durability evidence is process-kill only; last-segment corruption is silent loss; no WAL I/O-error handling beyond abort | `storage.cpp:396-420` **[V]**, `816-819`; `FAILURE_MODEL` S3/S5/S6, H1-H3 | Replication (a lost primary tail vs standby), recovery barrier, every "acknowledged" claim |
| **R2** | Ordering unspecified; batched and unbatched cross-shard requests from one origin can reorder | `shard.cpp:1470-1531` **[V]**, `engine.cpp:145` | Client retry/session logic, version-conditioned ops, any application that issues dependent ops without awaiting completion |
| **R3** | Follow-ups have no timer, no first-class event, a silent drop path; idempotence is a per-type contract | `shard.cpp:3525-3546`, `1858,2085-2092,3768-3775,3812`; `account.cpp:257-285` | Any new type, cross-group transfers, migration resume |
| **R4** | Exactly-once breaks at a slot move or across groups because sessions are per-shard objects | `cluster_client.cpp:80-90`; `CORE_CONTRACTS:313-322` | Migration, ClusterClient, the "no double apply" claim |
| **R5** | HA safety envelope is narrow: contact-driven fencing, no-witness degrade is timeout-only, async can be promoted stale, no checker, manual rejoin | `replication.cpp:667-669`, `shard.cpp:2252-2271`, witness has no async check **[V]** | Durability (acked writes), operations |
| **R6** | No trust boundary: no auth/TLS; admin `PROMOTE`/`MOVE` on `INADDR_ANY` | `engine.cpp:1234`, `node_link.cpp:203` **[V]**, `snc_server.cpp:1197` | Any non-lab deployment; HA (anyone can promote) |
| **R7** | Availability depends on abort-on-contract-violation, including on recovery of a bad image and on listener/epoll failures | `shard.cpp:57-60,1798-1802`; `conn_set.cpp:23-25,86` | Crash-loop risk after a corrupt-but-CRC-valid image |
| **R8** | Environment effects are silent: pin failure, hugepage downgrade, NUMA absent from code; the campaign relied on an external wrapper | `runtime.cpp:464`, `pages.cpp:47-61`; no `mbind` **[V]** | Every performance number's reproducibility |
| **R9** | No regression gate: no CI, one stale red test, one load-sensitive test, drifted docs | section 8 | Everything above (nothing keeps fixes fixed) |
| **R10** | Application surface is one compiled-in type with no schema evolution | `snc_server.cpp:545`, `engine.cpp:1011-1015` | Adoption; every claim about "persistent application objects" |
| **R11** | Scaling assumptions rest on small, unlike systems: 4-vCPU VM and one R640 on loopback; static slot map; per-shard single thread; cluster-map growth; whole-segment recovery read | various | Performance claims, long-running operation |

**Hidden coupling worth stating.** Repairing the stale `test_pilot` assertion touches `tests/test_pilot.py`, a path the pilot's
freeze gate forbids changing after `runner_commit` (`bench/pilot.py:265`, and `tests/test_pilot.py:344` asserts that very
refusal). Fixing the test therefore requires a decision about the freeze (re-choose `runner_commit`, or carve out the test),
not just an edit.

**Violations of established invariants.** None found in code except the soft tension in 3.1 (lease embeds the minting shard)
and the unmet intent of invariant 4 (locality "fundamental", but enforced only by an external wrapper).

---

## 7. Experimental evidence assessment

All campaign numbers below were re-derived by the evidence reviewer from raw files (`pilot.json`, `verdict.json`,
`characterization.json`), not copied from `STATUS.md`; no numeric discrepancy was found. Wording issues are in section 8.

### 7.1 Observations (what was measured)
- **S6** (R640): FAIL on rule 4 only (p99.9 14.2 to 14.7 ms against 10 ms); p99 and all other rules pass; capacity guard 0.9914.
- **Write-window characterization and REFINE #1:** W=1 lowers write latency; REFINE #1 passes all rules, with every rep at p99.9 = 9,961,472 ns, **0.39% below the limit, within one histogram bucket**. One rescue round remains.
- **CP-1A:** Profile A stable peak 1,090,948; objective 654,569 at p99 933,888 ns; closed-loop capacity 4,031,898. Profile C stable peak 5,775,584.
- **Duration validation:** mechanical pass at 654,569 but session p99 about 1.58x CP-1A.
- **Storage investigation:** one `fstrim` about 6 h before CP-1A, about 3.8 TB written since; SSD healthy; no cause shown.
- **A2-a:** STOP, triggered by the *supplementary* closed-loop throughput modality rule; the primary p99 gate passed.
- **Capacity-state (PR #120):** 25 of 25 runs; all 15 capacity runs large-batch (frames/recv 15.4 to 15.9), capacity 4.26 to 4.34 M ops/s; one state only, stop.

### 7.2 Validated conclusions
1. S6 failed as preregistered on p99.9 only; W=1 passes with a thin margin and is the approved Profile A checkpoint configuration.
2. CP-1A is complete and valid. Profile A's refusal-limited peak is about 1.09 M; the objective is 654,569 ops/s at p99 about 0.93 to 1.0 ms; the mechanism (checkpoint/backpressure design gap) was accepted.
3. Open-loop p99 at the objective rate is reproducible in a trimmed and settled state (A2-a between-session CV 1.4%).
4. Saturated closed-loop capacity is state-dependent (about 8% bimodal in A2-a). Its cause is unknown. It is not a decision input for Path A, which uses the objective-rate quantity.
5. No comparative campaign data exist. `CRITERIA.md` **[V]** does not exist; Profile B was not measured on the R640; every R640 run is TCP loopback, which `EXPERIMENTAL_BASELINE.md:695` forbids as campaign comparative evidence.

### 7.3 The PR #120 recvfrom ratio (0.81 to 0.82): the reviewer's hypothesis, tested
The reviewer expected a measurement-window mismatch. It is confirmed in kind and substantially in size.
- `capstate.py:454-458` divides a 60 s perf count by `req / fr`, where both come from the server log's single end-of-run line, which is built from **whole-process-life counters** (`tools/snc_server.cpp:1511-1515`, `src/net/conn_set.cpp:105-106,140`). The process life includes the load phase (5,000,000 creates) and a priming run.
- **[V]** Arithmetic from `A-capstate-k1-C1`: total requests 302,652,435 at 15.7 frames/recv gives 19.28 M expected recv calls for the process life; the perf window counted 15.49 M, ratio 0.8165. Back-solving, the non-window recv calls are about 3.5 to 3.7 M for the observed range 0.81 to 0.82, and 5,000,000 load-phase requests at about 1.4 frames/recv would account for 3.57 M. The match is close.
- **Not verified:** the 1.4 frames/recv figure for the load phase is borrowed from the anchor-run batching and was not measured; no windowed server counters exist and perf did not record the load phase. So this is "consistent with, quantitatively", not proven.
- **Consequences.** (a) The "instrument defect" flag is mostly a denominator error in the preregistered check and is not evidence of an observer effect; PR #120 should not be read as one. (b) The same whole-life `frames/recv` feeds the batching classifier (boundary 15.0) and the A2-a 13.2-13.9 versus 15.6-15.8 separation, so classification is consistent across experiments (same blend), but the absolute frames/recv *inside the window* is higher than the logged value and the 15.0 boundary is a property of the blended metric. (c) The observer-effect question (does the perf/sar instrumentation select the large-batch state?) remains unresolved, and this check gives no evidence either way.
- **Recorded as a separate instrumentation issue**, as directed (section 8, item T-1).

### 7.4 Hypotheses (unproven)
- Time since trim, cumulative writes or run order explain the duration session's 1.47 ms p99 (three confounded).
- The instrumentation selects the large-batch state.
- The checkpoint begin snapshot (about 27 ms on shard 0, max latency about 49 ms, invariant to W) is a p99.9 liability for a Path B comparison.

### 7.5 Measurement limitations
sysstat 10-minute interval, disk only; no BMC log, no historical frequency or C-state data, `turbostat` unusable; the 350 MB of `sar.sa` files are not committed (hashes only), so sar-derived columns cannot be re-derived from the repository; loopback only, one R640, one SSD with a background medium scan, one OS image; histogram quantization (REFINE #1 and the 240 s duration cell each sit within one bucket of their limit); K is small and results are single-day.

### 7.6 Unresolved questions
Why the duration session read 1.47 ms; whether closed-loop capacity enters any `CRITERIA` threshold (if it does not, the bimodality is a method nuisance); whether N_raw 0.125 (A2-a) versus 0.058 (CP-1A) matters for M_B; whether the checkpoint stall is a Path B liability.

### 7.7 New experiments
**None is recommended.** S6, A2-a, the capacity-state run and the duration run stay closed by their stopping rules. The recvfrom question is answerable offline. The one consequential architectural uncertainty is the preregistered comparative phase (real NIC, Profile B, matched durable competitors), which is already planned and gated on `CRITERIA.md` being committed first with N, M_B and the objective-rate p99 fixed, and with capacity either excluded or taken from non-bimodal quantities. That is a documentation and decision task for the reviewer before it is an engineering one.

**Pre-campaign dev-VM comparisons** (`docs/CONVENTIONAL.md`, `BENCHMARKS.md`, `SEASTAR.md`) match their CSV files numerically but are not like-for-like and, by the project's own rule, not campaign evidence. The "50x PostgreSQL" figure is arithmetically correct (2.38 M versus 45.7 K) and compares SNC to an untuned PostgreSQL with different load generators; the same data shows near parity in memory with the in-process thread-pool baseline over the network (2.38 M versus 2.35 M) and a worse durable p99. Under Path A (at least 2x), none of the matched in-process comparisons is a 2x win.

---

## 8. Technical debt and instrumentation issues

| ID | Item | Evidence | Authorization needed |
|---|---|---|---|
| **T-1** | **Instrumentation: capstate validity check uses whole-process-life denominators against a window numerator.** Fix: windowed server counters or subtract the load phase. | `capstate.py:454-458`; section 7.3 | Yes (touches `bench/results/`, which the freeze gate allows; but a driver change after a closed experiment needs a decision). Not applied. |
| **T-2** | `test_pilot` red since PR #110: asserts `go_record is None`, but `bench/cp1a_frozen.json:6` now sets it. **[V]** | `tests/test_pilot.py:366`; fix collides with the freeze gate (`bench/pilot.py:265`, `test_pilot.py:344`) | Yes: a freeze decision, not just an edit |
| **T-3** | `test_intake` failed once under `ctest -j4`, passed 3 of 3 alone; cause undiagnosed | clean-build log | Diagnosis only |
| **T-4** | Two carried unexplained flakes (Paxos `c2b` deadline, F8 links-up timeout) | `docs/STATUS.md` about 118-121 | Diagnosis only |
| **T-5** | No CI, no TSAN run on record | no `.github`/`scripts` **[V]** | Yes (new files) |
| **T-6** | `heap_limit_bytes` is dead code in all shipped paths | `native_runtime.h:23`, `runtime.cpp:472` | Wire or delete |
| **T-7** | Cluster map never freed | `engine.cpp:1604-1606` | Production change |
| **T-8** | `Registry::type()` linear on the hot path; mutable global registry | `registry.cpp:61-66,78-81` | Production change |
| **T-9** | Stale docs: `STATUS.md:538-541` versus `561-575` contradict on migration staging; `STATUS.md:459` "no acks"; `CORE_CONTRACTS.md` is marked DRAFT yet cited as authority and its shard.cpp line references are stale (refuse_unserved is now `shard.cpp:154-168`, standby gate `171-173`); `EXPERIMENTAL_VALIDATION.md` header still says "PLANNED"; `cp1a_frozen.json` `_status` still says "NOT FROZEN, NO GO"; the STATUS capstate line omits the denominator finding | reviewers C, D, E | Docs only; I did not edit them because several are reviewer-owned |
| **T-10** | `sar.sa` binaries (350 MB) not committed | `bench/results/a2a-capacity-state-r640/SHA256SUMS` | Storage decision |
| **T-11** | No golden hash vector for the persisted-placement hash | `test_object_id.cpp:53` **[V]** | Test only |
| **T-12** | Earlier audit follow-through (`docs/ARCHITECTURE_AUDIT.md`, 2026-10-13): invariants 11-18, group/epoch/term split, `E_MOVED` and the migration protocol were done; the **persistent pending-work index was silently not done [V]** (resume still walks every object, paced); transport abstraction not built (the audit made it conditional) | reviewer C | Decision |

---

## 9. Three prioritized engineering milestones

Prioritization rule from the assignment: correctness, integration and completeness over optimization, unless a performance blocker
is established. **No performance blocker is established** (section 7).

### M1. Durability fault-model closure
- **Objective and justification.** Everything downstream (replication, migration, "acknowledged") rests on durability claims whose only evidence is process kill. R1 is the highest-severity gap and includes one silent-data-loss path.
- **Prerequisites.** None. Test-only first step needs no production change.
- **Scope.** WAL write/fsync EIO and ENOSPC injection; last-segment mid-record corruption (first *characterize*, then decide fail-stop versus truncate with a durable signal); superblock, manifest and part damage (torn, zeroed, bit-flipped) with A/B fallback; a power-loss run on a device-mapper log-writes or flakey target, or a VM power-cut harness; WAL disk-space accounting and refusal; documentation of `durable` under `wal_sync=false`.
- **Exclusions.** No performance tuning; no new checkpoint design; no replication changes.
- **Expected work.** Test harness around existing debug hooks plus one injection point for the WAL path; later, a small `storage.cpp` change if the reviewer chooses fail-stop on mid-log corruption.
- **Verification.** Each injected fault produces a defined outcome (recover, refuse, or abort with a named reason) asserted by a test; the power-loss run replays N cycles.
- **Acceptance criteria.** (1) No test path in which an acknowledged durable write disappears silently; (2) every storage failure has a test asserting its outcome; (3) at least 200 power-cut cycles with zero lost acknowledged writes and zero unrecoverable states, or each loss is explained by a documented, tested exclusion; (4) `FAILURE_MODEL.md` S3, S5, S6, H1-H3 updated from UNPROVEN to a tested status or an explicit "not covered".
- **Risks.** Power-loss tooling on the R640 is heavy; a VM may not reproduce real device behavior; fail-stop on mid-log corruption can turn a silent loss into an availability event (a deliberate trade-off to decide).

### M2. Delivery, ordering and application-surface closure
- **Objective and justification.** The platform's value proposition is application objects with correct semantics. Today the semantics are unspecified (ordering), implicit (idempotence per type) or unproven (cross-group resend), and only one type exists.
- **Prerequisites.** M1 may proceed in parallel; the ordering test needs no prerequisite. Schema work should follow the ordering contract.
- **Scope.** (a) A written op-ordering contract and a test of batched versus unbatched interleaving (R2); (b) the cross-group resend test, or carry the session record on slot move (R4); (c) a decision on the "event" abstraction, and for follow-ups either a timer-driven resume or a proof that every drop is paired with a trigger (R3); (d) a second user type through the public descriptor API, a registry freeze flag, and a schema-version policy; (e) a decision on how applications are loaded (linked in versus loaded).
- **Exclusions.** No new transport; no multi-object transactions or snapshot reads; no Rust, DPDK or VPP; no performance work.
- **Expected work.** Mostly tests and documentation first; production changes (flush the pending batch before a direct send to the same destination, or define the weaker contract; a resume timer) only after authorization.
- **Verification.** Property or randomized interleaving test for ordering; process-level cross-group resend scenario in `test_baseline_move` style; a second type exercised by the existing crash, replication and migration suites.
- **Acceptance criteria.** (1) The ordering contract is stated and a test fails if it is violated; (2) a resend after a slot move to the new owner applies exactly once, in a test; (3) every follow-up drop path is either removed or covered by a test showing eventual settlement without a restart; (4) a second, differently shaped type passes the crash, failover and migration suites unchanged.
- **Risks.** Closing the batching reorder may cost cross-shard throughput (CROSSCORE showed 0.67x to 1.20x sensitivity to depth), which would have to be measured under the existing preregistration discipline, not assumed; the session-on-move design touches the migration protocol.

### M3. Operability and regression baseline
- **Objective and justification.** There is no regression protection (R9) and no trust boundary (R6), and the server stays silent about environment degradation (R8). A deployable system needs these before further features.
- **Prerequisites.** A decision on the freeze interlock for T-2. Otherwise independent.
- **Scope.** CI that builds and runs `ctest` plus an ASan/UBSan run and a TSAN run; resolve `test_pilot` (T-2) and diagnose `test_intake` (T-3); startup self-checks that log or refuse on pin failure, page-mode downgrade, missing NUMA binding, governor and THP; bind admin and node-link listeners to a configured address; an authentication decision for `PROMOTE`/`MOVE`; connection cap; a config file and systemd unit; pin/NUMA/page-mode in HEALTH.
- **Exclusions.** No full TLS rollout unless the reviewer decides it is in scope; no metrics backend beyond a minimal export.
- **Expected work.** New CI configuration, small server changes for self-reporting and binding, documentation.
- **Verification.** CI green on `main` and on a deliberately broken branch (the gate must fail); tests asserting startup warnings or refusals.
- **Acceptance criteria.** (1) `ctest` passes in CI on three consecutive runs, with ASan/UBSan and TSAN clean or each report triaged; (2) no listener binds `INADDR_ANY` without an explicit flag; (3) `PROMOTE`/`MOVE` refuse an unauthenticated peer, or the trust boundary is documented as a lab-only configuration with a startup banner; (4) a server started with a failed pin or downgraded page mode says so in its log and in HEALTH.
- **Risks.** TSAN may surface real races in the shard/ring design; authentication touches the HA path and must not weaken fencing.

### Recommended execution order
**M1 and the test-only half of M2 first and in parallel; M3's CI slice early** (it is cheap and protects the rest), with the T-2 freeze decision requested now. If the reviewer wants one sequence: **M1, then M3, then M2**, because M3's CI makes M2's production changes safe to land, and M2's ordering decision can be made by the reviewer meanwhile.

---

## 10. Recommended immediate next work slice

**M1-a: WAL and checkpoint fault-injection characterization, test-only.**
- Add tests (no change to `src/`) that (1) inject EIO and ENOSPC into the WAL write path via the existing debug-hook style and assert the outcome, (2) flip a byte mid-record in the *last* WAL segment and assert the *current* behavior (silent drop of later acknowledged records), in the manner of `test_async_failover` characterizing a known-bad behavior, and (3) zero or tear the superblock, manifest and one checkpoint part and assert the fallback or abort.
- **Why this one.** It is the highest-severity gap (R1), needs no production change, converts "unknown" into named, tested outcomes, and gives the reviewer the data to decide fail-stop versus truncate.
- **Acceptance.** All new tests pass on `main` by asserting the current behavior; the three outcomes are recorded in `docs/FAILURE_MODEL.md`; no source file outside `tests/` and `docs/` changes.
- **Needs authorization** because it adds tests (not allowed under this audit's scope).

**Requests for authorization or decision (none acted on here):**
1. Authorize M1-a.
2. Decide the freeze handling for `tests/test_pilot.py` (T-2) so the red test can be repaired.
3. Authorize the T-1 driver fix or accept a documentation-only note.
4. Confirm the "event" abstraction question (M2c) and the op-ordering contract direction (M2a).
5. Commit `CRITERIA.md` before any comparative work (reviewer-owned).
