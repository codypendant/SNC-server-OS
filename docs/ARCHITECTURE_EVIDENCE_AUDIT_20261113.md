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
- I ran a clean build of `main` in a scratch directory (not `build-s6`, which holds the frozen binaries) and the full `ctest`
  (`-j4`): 47 of 49 passed. `test_pilot` failed (deterministic, known cause, section 8). `test_intake` failed **once** in that
  run and has **not failed since**: it passed 3 of 3 alone, and passed in all 8 executions of the independent QA run (below).
  Its one failure is therefore **unreproduced and undiagnosed**, not shown to be flaky. Log:
  `docs/audit-20261113-working-notes/ctest-clean-build-main-23f4a0c.log`. Passing tests are evidence for the specific scenario
  tested, not for any wider guarantee.
- **Revision 2 (after independent QA, 2026-11-13).** The reviewer held the merge pending independent QA. A fresh agent that
  had not seen this report's reasoning re-derived the findings from code and by running things; I then re-ran or re-read the
  significant ones myself. Evidence: `docs/audit-20261113-working-notes/qa/` (report, demo programs, logs). Section 0 lists
  what QA confirmed, narrowed and corrected. **One caveat on QA independence:** the agent disclosed that a document-wide
  search it ran during Q2 printed about 25 lines of this report before it stopped, so its Q2, Q3, Q4 and Q8 may carry some
  anchoring. For that reason the defects below rest on programs that I re-ran myself, not on QA's say-so alone.
- Three-way verification: author (this report), reviewers (notes A to E), and QA (`qa/QA_REPORT.md`) now all exist. I have
  personally re-verified the items marked **[V]**, including every QA-confirmed defect. The QA agent and I are the same model
  family; the reviewer (cody) decides whether that satisfies the QA requirement.

Classification vocabulary (from the assignment): **Verified**, **Partially verified**, **Implemented, unverified**,
**Planned**, **Unknown**. Where none fits, the row says so rather than stretching a label.

---

## 0. Revision 2: independent QA results and corrections

QA answered eight questions without seeing this report. Status uses the review's vocabulary: **confirmed defect**
(demonstrated or traced unambiguously), **plausible risk**, **unverified hypothesis**, **refuted**, **unknown**.
**[V]** = I re-ran or re-read it myself after QA reported it.

### 0.1 Confirmed defects

| ID | Defect | Evidence | Reach |
|---|---|---|---|
| **D1** | **Cross-shard overtaking.** A non-batchable request (create, delete, version-conditioned, args over 16 bytes) from shard A to shard B overtakes an earlier batchable EXEC from A to B that is still held in A's out-batch. | **[V]** `qa/src/order_demo.cpp`: in-process 200 of 200 overtaken with `batch_remote=true`, 0 of 200 with `batch_remote=false` (re-run by me, same result). QA also saw 100 of 100 over TCP through a non-owner shard port (`qa/src/net_order.cpp`, not re-run by me). Code: `shard.cpp:1470-1531`. | **Narrower than first written.** Sequenced requests are exempt (refused `E_NOT_OWNER`, never forwarded; `shard.cpp:230-236`). The shipped `ClusterClient` and the bench clients route to the owner shard, so they do not hit it. Exposed: in-process applications (the data-plane API) and unsequenced pipelined clients that connect to a non-owner shard port. It contradicts the stated hint that routing is "never a correctness requirement" (`include/snc/wire.h:5`). No ordering contract exists in any document. |
| **D2** | **Silent loss of acknowledged writes on mid-record corruption of the last WAL segment**, with no operator-visible signal. | **[V]** I wrote 100 acknowledged durable creates, flipped one byte in record 50 of the only segment, restarted: 49 survive, 51 acknowledged records missing, exit 0, no message, `torn_tails=1`. QA saw the same (49 of 100; 519 of 600 over three segments). Code: `storage.cpp:396-420` (`++out.torn_tails; break;`). `snc_server` never prints or exports `torn_tails` (QA grep; I confirmed it appears only in `engine.cpp` and tests). Corruption in an *earlier* segment aborts ("WAL gap", exit 134). | Any media fault (bit rot, bad sector) that damages a record in the newest segment. The loss becomes permanent because recovery then starts a new segment. |
| **D3** | **Client-identity reuse returns a false success.** A new `Client` that reuses a `client_id` restarts sequences at 1 against live sessions and receives `OK, replayed` for requests it never ran. | **[V]** `qa/src/sess_reuse.cpp` against a scratch `snc_server`: incarnation 1 applied three deposits (balance 300); incarnation 2 sent four +1000 deposits; the first three got `E_REPLAYED_OLD`, the fourth `OK, replayed=1, version=4`, and the balance stayed 300 (expected 4,300 if all had applied). `docs/CORE_CONTRACTS.md` D-5 calls reuse "out of contract". | Reachable through the public `Client::enable_session` API after any client restart that reuses an id. Severity depends on whether callers can be forced to use unique ids (server-issued ids would remove it). |
| **D4** | **No authentication, authorization or TLS on any listener; several listen on all interfaces.** | **[V]** Admin port: `ha::listen_on` (`tools/ha.h:31-41`) takes no host and binds `INADDR_ANY`; it is used at `tools/snc_server.cpp:1040`, so the data-port `--host` flag does not constrain it. I started a scratch server with the data port on `127.0.0.1` and the admin port on `0.0.0.0`, and an unauthenticated `HEALTH` request to the host's LAN address was answered. Witness port: `tools/snc_witness.cpp:677` (same helper). Seed listener: `engine.cpp:1234`. Node-link listener: `node_link.cpp:203`. Replication listener default host `0.0.0.0` (`replication.h:226`). | I did **not** run a state-changing verb. That `PROMOTE`, `MOVE`, and the witness `DEGRADE`/`NOTREADY`/`READY` and `PLACEMENT CAS` verbs are open to any peer is established by reading the handlers, not by exercising them. |

### 0.2 Plausible risks and narrowed claims

- **Duplicate application across a slot move (was R4): narrowed from "breaks" to "plausible risk".** My first draft said a
  resend that lands on the new owner would double-apply. QA found, and I **[V]** confirmed in the source, that `run_exec` checks
  the session replay at `shard.cpp:562-565` **before** the slot-serving check at `:568`, so a resend that reaches the *source*
  group is answered from its session (`E_MOVED` is only produced for a request the session shows as not applied). The shipped
  `ClusterClient` keeps one `Client` per group, each with its own unanswered queue, and never carries an unanswered request to
  another group; `test_baseline_move.py` q2 (resend while the slot moves, answers dropped) passes. **What remains:** (a) a driver
  that re-issues an in-doubt request to the destination on its own would apply it twice; (b) session expiry (default TTL 3600 s,
  evaluated only inside a checkpoint, `engine.cpp:1423-1428`) between the first attempt and the resend; (c) q2 runs at depth 1
  ("nothing in doubt"), so a depth-greater-than-1 case yields `E_REPLAYED_OLD` (outcome unknown) by design. None was demonstrated;
  QA could not run a two-group scenario and neither could I. Sessions do not move with slots (`moves_with_slot` is
  `type_id >= 256`, `shard.cpp:3578`), which is documented.
- **Power-loss, WAL EIO and WAL ENOSPC behavior: unknown.** No test covers them and neither QA nor I could exercise them here.
- **Corruption outcomes now characterized by QA (not covered by repository tests):** a flipped byte in an earlier WAL segment,
  a corrupted checkpoint part, or a corrupted manifest aborts fail-closed; a corrupted newest superblock falls back to the older
  generation; both superblocks corrupted refuses to start. These are observed behaviors, so they are good news, but no committed
  test pins them.
- **Fault-model wording:** durability evidence is process kill plus return-value injection into *checkpoint* writes only; the
  WAL path has no injection at all.

### 0.3 Corrections made to this report
1. **Listener citation (was wrong).** I cited `engine.cpp:1234` and `node_link.cpp:203` as the admin listener. Those are the
   seed and node-link listeners; the admin listener is `tools/ha.h:38` via `snc_server.cpp:1040`. The conclusion stands. Fixed
   in sections 1, 3.15 and 6.
2. **`test_intake`: "flaky under load" withdrawn.** QA saw no failure in 8 executions (5 isolated at 11.4 s, one serial `ctest`,
   one `-j4` at 17.3 s, two under artificial load at 17.6 s and 43.8 s). What is established is that it is **load-slow**. My
   single failure at 12.98 s under `-j4` is unreproduced. Fixed in sections 1 and 8 (T-3) and 9.
3. **Test totals.** My `-j4` run was 47 of 49. QA's serial run was 48 of 49 (1,701 s, 28.4 min) and its `-j4` run 48 of 49
   (459 s, 7.7 min); the only failure in both is `test_pilot`. 49 tests are registered; the one unregistered file in `tests/` is a
   helper (`witness_state.py`).
4. **`test_pilot` is deterministic:** failed 5 of 5 in isolation plus both full runs. Stale assertion, not a product defect
   (`bench/cp1a_frozen.json` has `go_record` set; the test asserts `None`; the other 23 test functions in the file pass).
5. **R4 narrowed** (above) and **R2 raised** from "hazard" to "confirmed defect" (D1) with its narrower reach stated.
6. **Items the first draft missed:** D3, the unauthenticated seed/witness/node-link ports (D4), session TTL expiry as a second
   duplicate route, `torn_tails` being invisible to operators (part of D2), and `E_REPLAYED_OLD` as the documented in-doubt
   outcome.
7. **Roadmap re-ordered** (section 9): minimal CI is now the first slice, and confirmed correctness defects precede new
   functionality, as the review requires.

### 0.4 Native architecture
SNC's persistence is its own: WAL (CRC32C, `RWF_DSYNC`), fuzzy checkpoints, manifest, A/B superblock, and recovery. QA and I
both confirm **no code path depends on PostgreSQL**; it appears only as a benchmark baseline in `bench/conventional/`. Nothing
here recommends introducing PostgreSQL. Every milestone strengthens the native components.

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

1. **Confirmed defect D2: silent loss of acknowledged writes on last-segment WAL corruption.** One flipped byte mid-record in the
   newest WAL segment drops that record and every later acknowledged record; recovery exits 0 and `snc_server` never reports
   `torn_tails` **[V]** (reproduced: 49 of 100 survive; `src/core/storage.cpp:396-420`). Power loss, WAL EIO and ENOSPC are
   untested. Section 0.1, R1.
2. **Confirmed defect D1: cross-shard overtaking.** A non-batchable request to a shard overtakes an earlier batchable EXEC from
   the same origin **[V]** (200 of 200 in process; 0 of 200 with batching off; `src/core/shard.cpp:1470-1531`). Reach is narrower
   than first written: sequenced requests are exempt and the shipped clients route to the owner. No ordering contract exists.
   Section 0.1, R2. Also confirmed: **D3**, a reused `client_id` gets a false `OK, replayed` (section 0.1).
3. **There is no event system.** The only asynchronous mechanism is the follow-up/outbox. It is at-least-once re-emission from
   durable state with hand-written idempotence per type, has no timer (re-emission only on restart, link-up, map change,
   unseal), and has a silent drop path. Section 3.13.
4. **No security boundary and no regression gate (D4).** No authentication or TLS on any port. The admin port (`tools/ha.h:38`,
   used at `tools/snc_server.cpp:1040`), the witness, seed and node-link ports bind all interfaces **[V]**, and the admin port
   answered an unauthenticated request on the LAN address while the data port was loopback-only. There is no CI **[V]**, and one
   test (`test_pilot`) is deterministically red by a stale assertion.
5. **The performance campaign has not produced a go/no-go signal, and none of its open questions blocks engineering.** The
   comparative phase is gated on a `CRITERIA.md` that does not exist **[V]**. The PR #120 recvfrom "instrument defect" is
   substantially a denominator-definition error in the preregistered check, not evidence of an observer effect (section 7.3).

**Correction to a premise in the assignment.** The brief lists "PostgreSQL persistence". **SNC has none [V].** A grep of
`CMakeLists.txt`, `src/`, `include/`, `tools/` for `postgres|libpq` finds nothing. PostgreSQL appears only as a *comparison
baseline* in `bench/conventional/`, `docs/CONVENTIONAL.md` and result CSVs. SNC persistence is its own WAL + checkpoint +
A/B-superblock on local files.

**Recommendation.** Do not run another performance experiment. A small **minimal-CI slice (M0)** first, then three milestones:
**M1** confirmed correctness defects and the durability fault model (D1, D2, D3 and corruption/EIO/power-loss tests); **M2**
retry, migration and delivery semantics plus the application surface; **M3** security and operability. Nothing new is built on
top of D1 to D3 until they are resolved or explicitly evaluated. The first slice is in section 10.

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
- **U** Op-level ordering: **confirmed defect D1** (section 0.1). Scaling beyond a few shards: quoted results are 4-vCPU VM, plus R640 loopback. The slot map is static (`slot_map.h:12`; remap only at recovery when shard count changes, `engine.cpp:1020-1023`).
- **L** A single-threaded shard loop means any long operation stalls its shard (the checkpoint begin snapshot is about 27 ms on shard 0, section 7). The cluster map is never freed: every adopted placement is pushed into `maps_` (`engine.cpp:1604-1606`). Must-deliver messages use an unbounded overflow queue (`runtime.cpp:128`).
- **N** Document an ordering contract; test the batched/unbatched interleaving; free superseded cluster maps.

### 3.5 WAL: **Partially verified**
- **R** Per-shard-stream group-commit log; a durable ack means the record is on disk.
- **I** `src/core/storage.cpp:576-640` (flush, `durable_lsn` advances only in `flush_done`, 810-826), `343-420` (recovery rules), `native/runtime.cpp:177-181` (`RWF_DSYNC`). Spare segments are renamed **without a directory fsync** **[V]** (`storage.cpp:700-706`); recovery adopts a spare by its header.
- **T** `test_crash.cpp` (SIGKILL, 4 rounds, every acked write present), `test_persistence.cpp` (torn tail, lost spare rename), `test_wal_capacity.cpp` (runs with `sync=false, direct=false`, so it says nothing about disk durability).
- **D** No loss of acknowledged durable writes across process SIGKILL on one machine and filesystem; simulated torn tail and lost rename handled.
- **U** Power loss and fsync ordering (`docs/STATUS.md:510-511`: "power loss is not tested"; `docs/FAILURE_MODEL.md` H3, S5 UNPROVEN); real partial-sector writes; WAL EIO/ENOSPC (only handler is `storage_fatal` abort, `storage.cpp:816-819`, no injection test).
- **L** **Confirmed defect D2 (reproduced).** A bad or out-of-sequence record ends the segment and increments `torn_tails` (`storage.cpp:396-420`). For the last segment this silently discards that record and every later acknowledged record; `FAILURE_MODEL` lists this as S6, a known gap. `wal_sync=false` (`storage.h:141`) still reports acks as durable. No WAL disk-space accounting.
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
- **L** Sessions are per-shard objects and do not move with slots (`moves_with_slot` is `type_id >= 256`, `shard.cpp:3578`), and the cluster client uses a separate client and sequence per group (`cluster_client.cpp:80-90`). **Revision 2:** a resend that reaches the source group is answered from its session before `E_MOVED` (`shard.cpp:564` before `:568` **[V]**), and the shipped client keeps unanswered requests in the source group's `Client`; `test_baseline_move` q2 passes at depth 1. A double apply therefore needs a driver that re-issues an in-doubt request to the destination itself, or session expiry between attempt and resend: **plausible risks, not demonstrated** (section 0.2). `docs/CORE_CONTRACTS.md:313-322` still labels the area "INFERRED, UNPROVEN".
- **N** Write the narrowed cross-group tests (destination re-issue, TTL expiry, depth greater than 1) and decide the driver contract (M2a); session-on-move only if a test shows a defect.

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
- **L** **No authentication, TLS or ACL anywhere. [V]** The admin listener binds `INADDR_ANY` through `ha::listen_on` (`tools/ha.h:38`, called at `snc_server.cpp:1040`; `--host` does not apply to it), as do the witness (`snc_witness.cpp:677`), seed (`engine.cpp:1234`) and node-link (`node_link.cpp:203`) listeners. `PROMOTE`/`MOVE` are unauthenticated plain text (`snc_server.cpp:1197-1200`; by reading, not exercised). IPv4 only. Listener and `epoll_ctl` failures abort the process; no cap on accepted connections (`conn_set.cpp:23-25,75-90`).
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
- Exactly-once beyond the session horizon, and across a slot move for a driver that re-issues an in-doubt request to the destination (the shipped client's resend path is protected; section 0.2). Separately, **D3** shows a false success on `client_id` reuse.
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
| **R1** | **Confirmed (D2):** last-segment corruption is silent loss, invisible to operators. Evidence is process kill plus checkpoint-write return injection only; no WAL I/O-error handling beyond abort | `storage.cpp:396-420` **[V]**, `816-819`; reproduced; `FAILURE_MODEL` S3/S5/S6, H1-H3 | Replication (a lost primary tail vs standby), recovery barrier, every "acknowledged" claim |
| **R2** | **Confirmed (D1):** a non-batchable request overtakes an earlier batched one; no ordering contract | `shard.cpp:1470-1531`, `engine.cpp:145`; reproduced 200/200 **[V]** | In-process applications and unsequenced pipelined clients on a non-owner port (sequenced requests are exempt; shipped clients route to the owner); version-conditioned ops |
| **R3** | Follow-ups have no timer, no first-class event, a silent drop path; idempotence is a per-type contract | `shard.cpp:3525-3546`, `1858,2085-2092,3768-3775,3812`; `account.cpp:257-285` | Any new type, cross-group transfers, migration resume |
| **R4** | **Plausible risk (narrowed):** a resend is answered from the source session before `E_MOVED` (`shard.cpp:564` before `:568` **[V]**), so the shipped client is protected; a driver re-issuing an in-doubt request to the destination, or session TTL expiry, could double-apply. **Confirmed separately (D3):** `client_id` reuse yields a false `OK` | `cluster_client.cpp:80-90`; `CORE_CONTRACTS:313-322`; `qa/QA_REPORT.md` Q3 | Migration, ClusterClient, client restart, the "no double apply" claim |
| **R5** | HA safety envelope is narrow: contact-driven fencing, no-witness degrade is timeout-only, async can be promoted stale, no checker, manual rejoin | `replication.cpp:667-669`, `shard.cpp:2252-2271`, witness has no async check **[V]** | Durability (acked writes), operations |
| **R6** | **Confirmed (D4):** no auth/TLS; admin, witness, seed and node-link ports on all interfaces; `PROMOTE`/`MOVE` open (by reading) | `tools/ha.h:38` + `snc_server.cpp:1040`, `snc_witness.cpp:677`, `engine.cpp:1234`, `node_link.cpp:203` **[V]**; `snc_server.cpp:1197` | Any non-lab deployment; HA (anyone can promote) |
| **R7** | Availability depends on abort-on-contract-violation, including on recovery of a bad image and on listener/epoll failures | `shard.cpp:57-60,1798-1802`; `conn_set.cpp:23-25,86` | Crash-loop risk after a corrupt-but-CRC-valid image |
| **R8** | Environment effects are silent: pin failure, hugepage downgrade, NUMA absent from code; the campaign relied on an external wrapper | `runtime.cpp:464`, `pages.cpp:47-61`; no `mbind` **[V]** | Every performance number's reproducibility |
| **R9** | No regression gate: no CI (no hooks, cron or timers either), one deterministic stale red test, one load-slow test, drifted docs | section 8 | Everything above (nothing keeps fixes fixed) |
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
| **T-3** | `test_intake` failed once (12.98 s) in my `ctest -j4` run; not reproduced in 8 QA executions or my 3 isolated runs; load-slow (11.4 s alone, 17.6 s under 4 busy loops, 43.8 s under 72). Cause of the single failure undiagnosed; not shown flaky | clean-build log; `qa/` logs | Diagnosis only |
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

## 9. Three prioritized engineering milestones (revision 2)

Rules applied: correctness and durability first; **minimal CI first of all** (the review rejected leaving it to the last
milestone); **no new functionality is built on top of D1 to D4 until they are resolved or their downstream effect is explicitly
evaluated**; every milestone strengthens the native object store, WAL, checkpoint and recovery, and none adds PostgreSQL. No
performance blocker is established (section 7), so no milestone is a performance milestone.

### Dependency summary

```
M1-0 minimal CI  ──►  everything else (nothing keeps a fix fixed without it)
        │
        ├─► M1-a  characterization tests (D2, corruption matrix)        no production change
        ├─► M1-b  D1 ordering: contract + fix                           needs: ordering-contract decision
        ├─► M1-c  D2 policy + torn_tails visibility                     needs: policy decision (after M1-a)
        ├─► M1-d  D3 client-identity guard                              needs: wire/session design decision
        ├─► M1-e  WAL EIO/ENOSPC injection, disk-space refusal
        └─► M1-f  power-loss run                                        needs: harness (VM or dm-log-writes)
M1 defects closed or evaluated  ──►  M2 (retry/migration/delivery semantics, second type, schema)
M3 (security, operability) can start after M1-0; its admin-bind slice should not wait for M2 (D4 is exposed today).
```

### M1. Confirmed correctness defects, durability fault model, and minimal CI

**Objective and justification.** D1 to D3 are state-consistency defects that exist today; the durability claims behind every
"acknowledged" statement rest on process-kill evidence only. This milestone makes the evidence match the claims and protects it
with a regression gate.

**Prerequisites.** None for M1-0 and M1-a. M1-b needs the ordering-contract decision; M1-c needs the D2 policy decision; M1-d
needs a client-identity design decision; M1-f needs a power-loss harness.

**Slices, in order.**

- **M1-0. Minimal CI (first).** Clean Release build; a fast deterministic `ctest` subset on every change (the C++ unit tests and
  the quick Python tests); the full suite (28.4 min serial, 7.7 min at `-j4`, from QA's per-test timings) nightly or per merge
  to `main`; an ASan/UBSan job (a stale 48/48 `build-asan` log dated 11-11 exists, nothing automated); a TSAN job (none has ever
  run); `test_pilot` resolved so the gate can be green. **Open decision:** hosted versus self-hosted runner. Many tests start
  real processes, use io_uring and `RWF_DSYNC`, and bind fixed port ranges, so a hosted runner may not suffice; the R640 host or a
  dedicated Linux VM is the likely home.
- **M1-a. Characterization tests, test-only.** Tests asserting the *current* behavior of: last-segment mid-record corruption (D2),
  earlier-segment corruption (abort), superblock A/B damage, manifest and part corruption, a batched-versus-unbatched
  interleaving (D1, asserting the current overtaking so the later fix flips it), and `client_id` reuse (D3). QA already observed all
  of these once; the tests pin them.
- **M1-b. D1: ordering.** First write the op-ordering contract (what a single origin may rely on). Then either flush the pending
  batch to a destination before any direct send to that destination, or document the weaker contract and refuse the unsafe
  combination. **Downstream effect to evaluate before merging:** batching exists for cross-shard throughput (`docs/CROSSCORE.md`
  measured 0.67x to 1.20x depending on depth), so the cost of the fix must be measured under the existing preregistration
  discipline, not assumed.
- **M1-c. D2: last-segment policy and visibility.** Decide between fail-stop and truncate. A middle option exists and avoids the
  availability cost for the ordinary crash tail: before truncating at a bad record, scan the rest of the segment for any later
  *valid* record with a higher LSN; if one exists the damage is mid-log corruption, not a torn tail, and recovery refuses with a
  named error. In every case, surface `torn_tails` in the startup log, `HEALTH` and `GAUGES`, and exit status policy.
- **M1-d. D3: client identity.** Options: server-issued client ids or an incarnation counter in the session key, so a restarted
  client cannot collide with a live session. **Downstream effects:** wire format, session records, replication of sessions, and
  migration (sessions do not move with slots). Needs a design note before code.
- **M1-e. WAL fault injection.** EIO and ENOSPC on WAL write and fsync (today only `storage_fatal` abort, untested); WAL disk-space
  accounting and a refusal path.
- **M1-f. Power loss.** A power-cut or log-writes harness (VM or device-mapper `log-writes`/`flakey`); repeated cycles.

**Scope exclusions.** No performance tuning, no new checkpoint design, no replication changes, no feature work.

**Verification strategy.** Each slice has a test that fails before and passes after; CI runs them; fault outcomes are asserted
(recover, refuse, or abort with a named reason), never merely "does not crash".

**Measurable acceptance criteria.**
1. CI green on `main` for three consecutive runs and **red on a deliberately broken branch** (the gate must be shown to fail);
   fast subset under 10 minutes; `test_pilot` green by a decision, not a skip.
2. D2: no acknowledged durable write disappears silently in any test; `torn_tails` is visible in the log and `HEALTH`/`GAUGES`.
3. D1: the contract is written and a test fails if it is violated; the measured cost of the fix is recorded against a preregistered
   threshold.
4. D3: a test in which a restarted client cannot receive a false `OK, replayed`.
5. Every storage failure listed in M1-a and M1-e has a test asserting its outcome; at least 200 power-cut cycles with zero lost
   acknowledged writes and zero unrecoverable states, or each loss explained by a tested exclusion.
6. `docs/FAILURE_MODEL.md` S3, S5, S6, H1 to H3 updated from UNPROVEN to a tested status or an explicit "not covered".

**Principal risks.** Fail-stop on mid-log corruption turns silent loss into an availability event (hence the discriminating rule);
the ordering fix may cost cross-shard throughput; the client-identity change touches the wire and session formats; power-loss
tooling on the R640 is heavy and a VM may not reproduce device behavior; TSAN may surface real races.

### M2. Retry, migration and delivery semantics, and the application surface

**Objective and justification.** Once the state-consistency defects are closed, finish the semantics a state-native platform
promises: what a client can rely on across failover and slot moves, what a follow-up guarantees, and whether anything other
than `Account` can be built on it.

**Prerequisites.** M1-0 and M1-b to M1-d resolved or explicitly evaluated. **Do not start M2's new functionality on top of an
unresolved D1 to D3.**

**Scope.**
- (a) The cross-group resend test the audit first proposed, now narrowed: exercise the driver-re-issue-to-destination and the
  TTL-expiry routes (section 0.2), and depth greater than 1 (`E_REPLAYED_OLD`). Outcome is either a test showing safety, a defect, or
  a documented contract that forbids the unsafe driver behavior. Session-on-move only if the tests show a defect.
- (b) A decision on the "event" abstraction. If follow-ups remain the only mechanism, say so and drop the word; add a timer-driven
  resume or prove every silent drop (`emit_result`, `shard.cpp:3525-3546`) is paired with a trigger.
- (c) A second, differently shaped user type through the public descriptor API; a registry freeze after start; a schema-version
  policy (today a version or name change refuses to start); a decision on how applications are loaded.
- (d) HA safety gaps from R5: refuse automatic promotion of async pairs; test a partitioned old primary serving reads; forbid or flag
  no-witness degrade in sync mode.

**Exclusions.** No new transport; no multi-object transactions or snapshot reads; no Rust, DPDK or VPP; no performance work.

**Verification.** Process-level scenarios in the `test_baseline_move` style; the second type must pass the existing crash,
failover and migration suites unchanged.

**Acceptance criteria.** (1) Every route in 0.2 is either tested safe, fixed, or excluded by a written and tested contract.
(2) Every follow-up drop path is removed or covered by a test showing eventual settlement without a restart. (3) A second type
passes the crash, failover and migration suites unchanged. (4) Async pairs are never promoted automatically, or the refusal is
loud and tested.

**Principal risks.** Session-on-move touches the migration protocol; a resume timer changes follow-up timing; the second type may
expose descriptor-API gaps.

### M3. Security and operability

**Objective and justification.** There is no trust boundary (D4) and the server stays silent about environment degradation (R8). A
deployable system needs both. The admin-bind slice should not wait for M2: the admin port is reachable on the LAN today.

**Prerequisites.** M1-0 (so changes are gated). Authentication touches the HA path and must not weaken fencing.

**Scope.** Bind admin, witness, seed and node-link listeners to a configured address (default loopback) with an explicit
flag for wider exposure; an authentication decision for `PROMOTE`, `MOVE` and the witness verbs; a connection cap; startup
self-checks that log or refuse on pin failure, hugepage downgrade, missing NUMA binding, governor and THP; pin, NUMA and page mode in
`HEALTH`; a config file and a systemd unit; wire or delete `heap_limit_bytes`.

**Exclusions.** No full TLS rollout unless the reviewer puts it in scope; no metrics backend beyond a minimal export.

**Verification.** Tests that the unauthenticated connection is refused or the lab-only posture prints a startup banner; tests of
each startup warning or refusal.

**Acceptance criteria.** (1) No listener binds all interfaces without an explicit flag. (2) `PROMOTE`/`MOVE`/witness state-changing
verbs refuse an unauthenticated peer, or the configuration is documented lab-only with a banner. (3) A server started with a
failed pin or downgraded page mode says so in its log and in `HEALTH`.

**Principal risks.** Authentication in the HA path; operators lose convenience of the open admin port.

### Recommended execution order

**M1-0 first, then M1-a and M1-e in parallel (both are test-only or additive), then M1-b, M1-c and M1-d as their decisions land,
with M1-f when a harness exists. M3's admin-bind slice may run right after M1-0, in parallel with the rest of M1. M2 starts after
D1 to D3 are resolved or evaluated.** Rationale: the CI gate protects every later change; the confirmed defects are the
state-consistency violations the review asked to prioritize; M2's semantics work would otherwise rest on a layer with known
violations.

---

## 10. Recommended immediate next work slice

**M1-0 minimal CI, then M1-a characterization tests.** Both are additive (new CI configuration, new tests); neither changes
production code.

- **M1-0** needs one decision first: how to handle `tests/test_pilot.py` under the freeze gate (`bench/pilot.py:265`,
  `tests/test_pilot.py:344`): re-choose `runner_commit`, or carve the test out of the post-freeze list. It also needs the
  hosted-versus-self-hosted runner decision.
- **M1-a** pins the current behavior of D1, D2, D3 and the corruption matrix so the later fixes visibly flip a test.

**Acceptance for the slice:** CI green three times and red on a broken branch; the M1-a tests pass on `main` asserting current
behavior; no file outside `tests/`, CI configuration and `docs/` changes.

**Requests for authorization or decision (none acted on here):**
1. Authorize M1-0 and M1-a.
2. Decide the freeze handling for `tests/test_pilot.py` (T-2) and the CI runner location.
3. Decide the ordering-contract direction (D1) and the D2 policy (fail-stop, truncate, or the discriminating rule).
4. Decide the client-identity approach (D3).
5. Authorize the T-1 capstate driver fix, or accept a documentation-only note.
6. Confirm the "event" question (M2b).
7. Commit `CRITERIA.md` before any comparative work (reviewer-owned).
