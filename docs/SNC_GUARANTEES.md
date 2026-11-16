# SNC Guarantees Specification (proposed)

- **Document:** SNC-AFR-001 Phase 2 ("Guarantees and Architectural Contracts", Codex's Milestone 2), primary output (cody's amendment 2: "one document stating what SNC
  promises, so differential testing judges against the spec and not C++ behavior").
- **Status: PROPOSED. Nothing in this document is accepted.** Each clause binds nothing until cody accepts it,
  after Codex's review. Clauses already accepted elsewhere are marked **[ACCEPTED: source]** and are restated,
  not re-decided.
- **Baseline:** `main` at `2fb029c` (PR #138 merged; C++ reference code as of `63cf6d7`). Every file:line citation refers to this commit.
- **Builds on:** `docs/CORE_CONTRACTS.md` (DRAFT), `docs/FAILURE_MODEL.md` (G1-G6 PROPOSED),
  `docs/AFR_001_INVENTORY.md` (Phase 1 findings AFR-F-01 to 07). Where this document and CORE_CONTRACTS differ, this
  document is the newer proposal and says so; on acceptance it supersedes the overlapping CORE_CONTRACTS clauses.
- **Authority once accepted:** SNC-ADR-RUST-001 makes "the specification" authoritative over both the C++ reference
  and the Rust implementation. Accepted clauses of this document would be that specification. The v0.1 design
  (`docs/spec/SNC_v0.1_Object_Execution_Specification.md`) is historical input only.

## 0. How to read this document

**Keywords** are RFC 2119 (MUST, MUST NOT, SHOULD, MAY).

**Counts.** 64 guarantees (AFR-G-01 to 64), 32 rulings (AFR-R-01 to 32), 15 decisions (AFR-D-01 to 15).

**Clause IDs.** `AFR-G-nn` is a guarantee; `AFR-R-nn` is a proposed ruling on a known gap; `AFR-D-nn` is a decision
that must be made before Rust crate design. Older identifiers are cited with their source document
(`CORE_CONTRACTS R-2`, `FAILURE_MODEL G1`, `audit-D2` = `ARCHITECTURE_EVIDENCE_AUDIT_20261113.md` D2, `INV n` =
`AGENTS.md` invariant n).

**C++ status** of each clause, as the reference behaves at the baseline:

| Status | Meaning |
|---|---|
| **HOLDS** | the reference satisfies the clause, with test or experiment evidence (cited) |
| **HOLDS (code)** | the reference satisfies it by code reading only; no test pins it |
| **PARTIAL** | holds only in named configurations or cases |
| **VIOLATED** | the reference is shown not to satisfy it (cited experiment or test; "(code reading)" or "(inferred)" marks weaker evidence) |
| **ABSENT** | the mechanism the clause needs does not exist |
| **UNDECIDED** | the clause itself needs an owner decision (an `AFR-D`) before it can be stated |

**Differential-testing class** (amendment 2; assignment section 4.L) says how a C++/Rust comparison treats behavior
under the clause:

| Class | Meaning for the harness |
|---|---|
| `EQ` | required semantic equivalence: C++ and Rust must agree, and both must satisfy the clause |
| `ND` | allowed nondeterminism: outcomes may differ within what the clause permits (interleaving, timing, which of two legal answers) |
| `KD` | known C++ defect: the oracle is the clause, not C++; a C++ divergence is expected and recorded, a Rust divergence fails |
| `INV` | invalid state: either implementation reaching it fails the run |
| `DUR` | durability violation: loss of an acknowledged write; fails the run |
| `CON` | consistency violation (ordering, duplicate application, two serving owners, stale read where forbidden); fails the run |
| `PERF` | performance regression; judged against preregistered thresholds, never against correctness |

**Documented limitation versus violation** (Codex, Handoff 1 review). A behavior the repository already documents
as a limitation or exclusion is not a violation of an accepted guarantee, because no guarantee is accepted yet
(AFR-F-07). Section 13 rules each one explicitly: DEFECT (the clause stands and the reference is wrong), ACCEPTED
BEHAVIOR (the clause is weakened or scoped to admit it), or UNRESOLVED (evidence or intent insufficient).

---

## 1. System model and scope

### 1.1 Entities

A **space** is one data set (128-bit UUID). It is divided into 4,096 **slots**; the **placement map** assigns each
slot to one **replication group** under a **placement epoch**. A group has one **primary** per **HA term** and
replicas. Inside a node, each slot belongs to one **shard** (one pinned thread). An **object** has a stable
**ObjectID**, a type, a version and a payload; it lives in its slot. A **client** issues **requests** (EXEC,
CREATE, DELETE, reads) and receives **answers**. **Witnesses** decide HA and placement state by consensus.

### 1.2 Fault model (scope of every guarantee)

Guarantees hold under the faults listed as **covered**. Faults listed as **out of contract** may cause any
outcome, but the system SHOULD detect them and MUST NOT report success it cannot back (AFR-G-14).

| Fault | Covered? | Notes |
|---|---|---|
| Process crash (SIGKILL), clean restart | covered | FAILURE_MODEL P1, P3 |
| Process stall (SIGSTOP, starvation, long task) | covered | P2 |
| Host crash, power loss with storage that honours `RWF_DSYNC`/`fdatasync` | covered (target) | H1, H3: UNPROVEN in the reference; no harness exists |
| Single storage-media damage (bit flip, lost or zeroed sector, misdirected write) on one node | **covered for detection** (proposed): the node MUST NOT lose acknowledged data silently; it fails closed (AFR-G-14) | new in this document; today VIOLATED (AFR-F-01) |
| Storage I/O error (EIO, ENOSPC) | covered: fail-stop | AFR-G-16 |
| Network partition, message loss, delay, reorder, duplication | covered | N-classes; safety MUST NOT depend on timing (AFR-G-46) |
| Clock step or drift | covered for safety | only explicitly bounded rate-drift assumptions are allowed (AFR-G-46) |
| Loss of a minority of witnesses | covered | Paxos majority |
| Rollback or restore of a witness's persisted state from an older copy | **out of contract** (proposed), must be detectable in production (AFR-R-19) | Paxos F9 |
| Simultaneous loss of every replica of a group, or of a witness majority | out of contract | |
| A peer that is not part of the cluster, or a compromised member | **covered for authentication and authorization** (proposed, AFR-G-40 to 44); a compromised authenticated member is out of contract | today ABSENT (AFR-F-02) |
| Buggy client (reused identity, mismatched retry) | covered for safety: refused, never mis-answered (AFR-G-21) | today VIOLATED (audit-D3) |

### 1.3 Configurations

A guarantee may depend on the group's configuration. The **reference configuration** is a group of one primary and
one synchronous replica on separate hosts, three or five witnesses, and clients using sessions (FAILURE_MODEL
baseline). Other configurations (no replica, async replica, no witnesses) MUST either satisfy the same safety
clauses or be refused at start-up unless the operator explicitly accepts the named weaker guarantee (AFR-G-31). **In-memory mode** (no data directory) is outside every durability and recovery clause: it has no WAL, no barrier and no durability.

---

## 2. Identity (deep: format-level, expensive to reverse)

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-01 | An ObjectID is 128 bits, encodes no location (CPU, NUMA node, disk, server), and is never reused within a space, whether or not the object was deleted. | INV 2; CORE_CONTRACTS 1 | PARTIAL | lease committed to the superblock before minting (`engine.cpp:1060-1072`); disjoint control-plane blocks (`test_control.py`); duplicate mint aborts only on collision with a live object (`shard.cpp:732-735`); never-reuse depends on G-02 (silent superblock fallback) | `EQ` |
| AFR-G-02 | Mint leases MUST never be issued twice, including after a superblock copy is lost or a node is restored. | new (speculative S-a, `A_core.md` 9) | PARTIAL | the superblock falls back silently to the older A/B copy, which holds a lower `next_lease` (`storage.cpp:210-226`, `:236`); rebase covers only leases of objects seen in loaded or replayed records (`engine.cpp:1210-1227`, `shard.cpp:1782`) | `KD` until AFR-R-23 is resolved |
| AFR-G-03 | The slot of an ObjectID is a frozen function of its bytes, specified byte-for-byte with pinned test vectors, and independent of any library version. | CORE_CONTRACTS Part XXIV (format specification); "frozen" stated only in code comments (`object_id.h:46`, `slot_map.h:12`) | PARTIAL | XXH3-64 from the system library (`CMakeLists.txt:13`, `object_id.cpp:15`), top 12 bits (`slot_map.h:14`); the only test compares the hash with itself (`test_object_id.cpp:53`) | `EQ` (on pinned vectors) |
| AFR-G-04 | The space UUID, HA term and placement epoch are independent version domains; the term and epoch only increase; a failover never changes the placement and a migration never changes the term. | INV 11, 12, 13 | PARTIAL | holds in operation: strictly newer epoch (`engine.cpp:1634`); promotion adopts the control-plane placement and bumps only the term (`engine.cpp:1340-1426`, `:1371`, `:1402`); `test_migrate`. Not under storage damage: the silent superblock fallback can revert a term or epoch (AFR-R-23) | `EQ` |
| AFR-G-05 | Every identity a protocol uses to decide authority or deduplication (space, group, term, epoch, session) is checked before any state is changed on its behalf, and a value an unauthenticated party can learn MUST NOT by itself grant authority. | new (AFR-F-02 item 6) | VIOLATED (code reading) | HEALTH publishes the space uuid and term (`C_interfaces.md` 4.2), which with shard count and slot CRC are the only replication handshake checks (`replication.cpp:676-691`) | `KD` |

| AFR-G-55 | A node restart does not by itself change the HA term; a node that restarts resumes at its durable term and MUST then satisfy G-32 before serving. | CORE_CONTRACTS I-d (INFERRED safe) | HOLDS (code) | | `EQ` |
| AFR-G-56 | A change of a node's shard count never changes which session record governs a client's requests; a client or server that cannot guarantee this refuses with a named error. | new (contradiction L, `A_core.md`) | PARTIAL | session records route by their own ID's slot; only guard is the client refusing a node with a different shard count (`client.cpp:148-152`), undocumented | `KD` |

---

## 3. Objects, operations and atomicity

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-06 | The atomic unit of change is one operation on one owner shard: its target object, at most one same-shard peer object (`OpCtx::local`), the session record and the obligations it creates, made durable as one indivisible WAL group. Recovery applies a group entirely or not at all. Operations registered `unlogged` are outside this clause: they change memory only and are never durable. | CORE_CONTRACTS 14 (transaction boundary) | HOLDS | `kRecGroupNext` (`wal_format.h:55-58`); incomplete groups dropped (`storage.cpp:416-434`); groups never span segments (`storage.cpp:651-688`); `unlogged` path (`shard.cpp:666-671`); `test_transfer` | `EQ` |
| AFR-G-07 | No multi-object atomicity is offered across shards or groups; such work is built from durable obligations (AFR-G-09). An answer of OK for such an operation means the source transition is acknowledged, never that its obligations are settled. | CORE_CONTRACTS 3, H-4 [ACCEPTED: work order Part III] | HOLDS | `docs/TRANSFERS.md`; `test_transfer`, `test_cluster` | `EQ` |
| AFR-G-08 | A refusal means "not applied": any status other than OK or OK-replayed MUST guarantee the operation had no effect, or the status MUST be IN-DOUBT (or carry an explicit applied marker). Validation MUST have no side effects. | CORE_CONTRACTS H-3 | PARTIAL | validate is hash-checked in debug builds only (`shard.cpp:514-526`); `E_NO_MEMORY` after the op ran on the cross-shard path (CORE_CONTRACTS 6.1, STABILIZATION_REPORT L-8); a replay refused at admission answers `E_OVERLOADED` with `replayed=1` for an applied op (`shard.cpp:330-335`), which conforms only if `replayed=1` is specified as the applied marker | `KD` |
| AFR-G-09 | Obligations (follow-ups), three separate properties: **(a) durable creation**: an obligation is durable atomically with the transition that creates it. **(b) At-least-once delivery**: it is re-delivered until its target has applied it, without depending on an unrelated event (restart, link flap, map change); duplicates are possible; every dropped delivery is counted and visible. **(c) Idempotent processing**: the target applies a duplicate delivery with no further effect. **Exactly-once observable effect is not claimed**: it would follow from (a) to (c) only with a deduplication and crash-recovery proof, which does not exist. | CORE_CONTRACTS O-1 [ACCEPTED], O-2 (which says "exactly once"; this clause narrows it), O-3; FAILURE_MODEL G4 | (a) HOLDS; (b) PARTIAL; (c) HOLDS (code) for the account type only; exactly-once UNPROVEN | (a) outbox in the same WAL group (`test_transfer`, `test_cluster`); (b) re-emission only on events, no timer, two silent drop paths (CORE_CONTRACTS 13); (c) idempotence is per application type (account inbox and outbox, "each step is idempotent", `docs/TRANSFERS.md:94`), not an engine guarantee; duplicates arise from `resume` after a restart | (a) `EQ`; (b) `KD`; (c) `EQ` per type |
| AFR-G-10 | An object's type and schema version are recorded durably; a node MUST refuse to start on data whose type or schema it cannot interpret, with a named error, and MUST NOT silently reinterpret it. A schema upgrade path is UNDECIDED (AFR-D-09). | CORE_CONTRACTS 1; `A_core.md` 3 | PARTIAL | manifest catalogue check refuses a changed schema or name at start (`engine.cpp:1040-1044`), but only when a checkpoint exists; replay checks the type (`shard.cpp:1789-1795`) and aborts on an unknown one, but copies `schema_ver` without comparing it (`:1818`) | `EQ` |

| AFR-G-57 | Every logged mutation increases the object's version by exactly one; a request carrying `expected_version` that does not match is refused (`E_VERSION_CONFLICT`) with no effect; redo is idempotent by version. | new | HOLDS (code) | `shard.cpp:673`, `:1791` | `EQ` |

---

## 4. Acknowledgement and durability (deep)

Durability classes and stages are as defined in CORE_CONTRACTS sections 3 and 4 (APPLIED, LOCAL_DURABLE,
HA_DURABLE, SETTLED). This section states what an acknowledgement promises.

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-11 | An answer distinguishes the **requested** class (per request, bounded by the group's configured mode) from the **achieved** class (the class the WAL group actually reached when the answer was sent). An OK guarantees exactly the achieved class: LOCAL_DURABLE means durable on the answering node; HA_DURABLE means durable on the primary and acknowledged by every automatically promotable replica; APPLIED (`Durability::APPLIED`, never the default) promises nothing across a crash. Creates always wait for at least LOCAL_DURABLE. In the reference configuration a default OK is HA_DURABLE. | CORE_CONTRACTS 6.1 | PARTIAL (holds for sync, healthy; the achieved class is not reported, G-12; async below HA_DURABLE, G-31) | `shard.h:555-559`; `test_replication`, `test_witness.py`, Vultr P3 | `EQ` |
| AFR-G-12 | **Degradation is represented, never silent.** When the achieved class is below the requested class (lost or lagging replica, degraded mode, async), the node either refuses or waits (per configuration), or answers OK with the achieved class stated; and its status surface reports the current achievable class. How the achieved class travels in the answer is part of the wire format and is decided under AFR-D-14 and AFR-D-06; until then the status surface is the only required channel. | CORE_CONTRACTS H-1 | VIOLATED | a group with a lost standby acknowledges LOCAL_DURABLE with nothing reported (STABILIZATION_REPORT D) | `KD` |
| AFR-G-13 | An acknowledged write at class C survives every covered fault for class C. In particular an HA_DURABLE write survives the loss of the primary's host or disk. | FAILURE_MODEL G2 | PARTIAL | holds for process kill (EL3-EL5); host crash and power loss UNPROVEN; storage damage VIOLATED (AFR-G-14) | `DUR` |
| AFR-G-14 | **No silent loss**, in two parts. **(a) Detection**: recovery MUST NOT truncate, delete, overwrite or reuse durable log content it finds damaged or cannot interpret; it refuses to start (or to serve the affected shard) with a named, operator-visible error. **(b) Boundary**: recovery restores every write acknowledged at LOCAL_DURABLE or above on that node, or refuses. (b) needs a durable acknowledgement boundary (AFR-D-02), because damage that removes whole flushes (missing or zeroed newest flushes), a superblock rolled back to its older copy, or a reused LSN can leave nothing to detect. An unacknowledged torn tail MAY be discarded only when the node can prove it lies beyond that boundary. Satisfying (a) is not compliance with G-14. | new; cody's PR #121 ruling on audit-D2 ("fail-stop on detected corruption that could conceal acknowledged data") | (a) VIOLATED; (b) ABSENT (no acknowledgement boundary exists) | AFR-F-01: mid-record damage, first-record damage (segment unlinked, `storage.cpp:526`), zeroed block, unknown record type all lead to exit 0 with acknowledged writes missing | `DUR` |
| AFR-G-15 | A log position (LSN) identifies one record for the life of a stream. After any recovery, no LSN that was ever durable is reassigned to different content. | new (AFR-F-01 item 5) | VIOLATED | QA 1 and reviewer A: new writes reuse the lost LSNs | `INV` |
| AFR-G-16 | A storage I/O error (EIO, short write, ENOSPC) on the WAL, a checkpoint, the manifest or the superblock stops the node (fail-stop) before any further acknowledgement; it is never retried silently or ignored. | FAILURE_MODEL S3 (the v0.1 storage spec's C7, not FAILURE_MODEL C7); `A_core.md` 6.4 | HOLDS (code) | `storage.cpp:814-819`, `storage_fatal` aborts (`:103-106`); checkpoint EIO tested (`test_s3_writeback` T6, T18; `test_s4_sync_tokens` T6), so FAILURE_MODEL S3's "no test injects EIO" is out of date; WAL EIO has no injection test | `EQ` |
| AFR-G-17 | The checkpoint correctness contract CC-1 to CC-5 holds. | [ACCEPTED: Codex, PR #68 review] CORE_CONTRACTS 13A | HOLDS | anchors T1-T18, `test_s2_pacing`, `test_s3_writeback` | `EQ` |
| AFR-G-18 | Recovery serves nothing before its barrier: log replayed, every shard's index and slot state rebuilt, and the node's identity (space, group, term, epoch) verified against the control plane when it has one. Externally visible effects before the barrier are limited to an explicit, documented list. A promotion applies the same barrier across all shards. | CORE_CONTRACTS I-1, I-2, I-3; INV 16; FAILURE_MODEL G5 | PARTIAL | follow-ups held until the barrier (I-a HOLDS); replication shipping and node links start before it (I-b); space-uuid check after it (I-c); promotion has no cross-shard client barrier (I-g); in-memory mode has no barrier (I-h, out of scope, section 1.3) | `KD` for I-c, I-g |

| AFR-G-58 | Writes the old primary never acknowledged may survive on the replica (records ship before the local fsync) and become visible after failover. A client MUST treat every unanswered request as IN-DOUBT (G-24); the spec never promises that an unanswered write is absent. | CORE_CONTRACTS 3 | HOLDS (documented behavior) | | `ND` |
| AFR-G-59 | Other operations on the shard may observe a write once it is APPLIED, before it is durable; an operation whose answer depends on such a write MUST NOT be acknowledged before that write's acknowledgement point. | CORE_CONTRACTS 6.1 (implied) | HOLDS (code) for default reads (G-27); UNTESTED for mutations that read another object's undurable state | | `CON` |

---

## 5. Ordering (deep)

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-19 | **Per-origin FIFO execution.** Requests from one origin to one owner shard execute in the order the origin issued them. An origin is one in-process caller (a shard thread's application code) or one client connection. Answers MAY arrive in a different order. | new; contract missing (audit-D1); README "routing is never a correctness requirement" | VIOLATED (in-process and unsequenced TCP via a non-owner port) | AFR-F-06: QA 1 20/20 per case; `shard.cpp:1473-1531` | `CON` |
| AFR-G-20 | No ordering is promised between different origins, between different owner shards, or between a request and an obligation it created, except what the operation's own causality implies. | CORE_CONTRACTS 5 (cross-object reads) | HOLDS (code) (nothing is promised) | | `ND` |

The scope of AFR-G-19 is itself a decision (AFR-D-04): per (origin, shard) is the cheapest rule that makes routing
an optimization; per (origin, object) is weaker and cheaper still.

---

## 6. Retry and deduplication (deep)

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-21 | A sequenced mutation is applied at most once while its session is ACTIVE, across crash, restart, checkpoint, recovery and covered failover. The dedup record is bound to the request: a request that reuses a seen sequence number with a different target, operation or arguments is refused, never answered as a replay. | CORE_CONTRACTS D-1; FAILURE_MODEL G3; new binding (AFR-F-03) | PARTIAL | at-most-once HOLDS within the horizon (EL3-EL4: `test_replication` 30K resends); binding VIOLATED (audit-D3; QA 1 P5) | `CON` |
| AFR-G-22 | A server MUST NOT run a sequenced mutation whose session it cannot prove ACTIVE; an expired or unknown session is refused with a distinct status. Expiry is a liveness policy only and never depends on wall-clock time for safety. | CORE_CONTRACTS D-2, G-3, G-4 | VIOLATED | `test_session_ttl` (`tests/test_replication.cpp:1285`); QA 1 P7 (clock step, balance 2,103 to 3,103); expiry by `::time(nullptr)` only inside checkpoints (`engine.cpp:1452-1457`) | `CON` |
| AFR-G-23 | Client identity is issued or qualified by the server (a session identity or an incarnation), so a restarted client cannot inherit another incarnation's dedup state. | CORE_CONTRACTS D-5, G-1 | ABSENT | `client_id` is caller-chosen (`client.cpp:98`) | `KD` |
| AFR-G-24 | IN-DOUBT is a first-class outcome: `E_REPLAYED_OLD`, `E_FENCED`, an expired session, a lost answer and `E_NOT_FOUND` on a resent delete all map to it; a client library never converts IN-DOUBT into OK or into a refusal, and never resends an in-doubt request under a new identity automatically. | CORE_CONTRACTS H-2, G-5 | PARTIAL | `net::Client` resends with the original sequence (correct while the session holds); `ClusterClient` resends after `E_MOVED` as new work (CORE_CONTRACTS 7) | `KD` for the cross-group case |
| AFR-G-25 | Every operation that can be sequenced is deduplicated, or its registration forbids sequencing; deletes are either sequenced or documented as non-idempotent with IN-DOUBT answers. | CORE_CONTRACTS D-4 | VIOLATED for `unlogged` ops; documented for deletes | a sequenced `unlogged` op runs every time and never records its session (`shard.cpp:666-671`; registry `registry.h:33-38`), and sequencing it is not forbidden; deletes are never sequenced (`shard.cpp:403`); both listed under "Not deduplicated" in CORE_CONTRACTS 7 | `KD` for unlogged; `ND` for deletes (ruling AFR-R-08) |
| AFR-G-26 | Retry across a slot move: a resend after `E_MOVED` is deduplicated or refused as IN-DOUBT, never applied twice. | CORE_CONTRACTS D-3 | VIOLATED (inferred, code reading only) | `ClusterClient` resends after `E_MOVED` as new work in the destination's sequence space (CORE_CONTRACTS 7, INFERRED); the mechanism is AFR-D-08 | `KD` |

| AFR-G-60 | A duplicate is resolved before any refusal that invites a resend (`E_MOVED`, `E_MIGRATING`, `E_OVERLOADED`). Refusals issued before the dedup check (`E_STANDBY`, `E_NOT_OWNER`) are safe only because the client resends the same sequence to the session holder; this dependency is part of the clause. | [ACCEPTED] INV 18 | HOLDS | `test_replication` resend tests | `EQ` |
| AFR-G-61 | A sequence number greater than the last seen runs even after a gap. Whether gaps are allowed is UNDECIDED (CORE_CONTRACTS 7, "contract undecided"); ruled under AFR-D-05. | CORE_CONTRACTS 7 | UNDECIDED | | `ND` |
| AFR-G-62 | A replay answer promises only the original status and version: a replayed OK carries no result payload, and a replay refused at admission keeps `replayed=1` (the op was applied). | CORE_CONTRACTS 7; `shard.cpp:330-335` | HOLDS (code) | | `EQ` |

---

## 7. Reads and consistency (deep)

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-27 | A default read MUST NOT return a state that a covered fault could later lose (it waits for the acknowledgement point of the group's mode). | CORE_CONTRACTS R-1 (which says "covered failover"; this clause widens it to every covered fault) | PARTIAL | the read waits for the object's LSN (`shard.cpp:627`, `complete_local` `:461-466`); in async mode that point is LOCAL_DURABLE and automatic failover loses it (`test_async_failover`, FAILURE_MODEL C7) | `EQ` |
| AFR-G-28 | **Read-your-acknowledged-writes, single object.** A default read answered OK reflects every write to that object acknowledged (by any node of the group) before the read was sent. This requires that the answering node holds valid authority at the time it answers (AFR-G-33). | CORE_CONTRACTS R-2 | VIOLATED (partition) | a cut-off primary answers immediately (QA 2 N7: about 330 stale reads); documented as a known gap (CORE_CONTRACTS 5; EXPERIMENTAL_BASELINE Q7) | `CON` (ruling AFR-R-09) |
| AFR-G-29 | Weaker reads (`allow_undurable`, standby reads, index lookups) are named as weaker in the API and are never the default; index results are hints the caller re-reads. | CORE_CONTRACTS R-3, R-5 | PARTIAL | `allow_undurable` is a request flag (`snc.h:26`); standby reads are a server option (`standby_read_lease_ms`, `snc.h:417-420`) with no request flag, and a sessionless client connected to a lease-enabled standby gets unmarked OK reads (only session clients skip standbys, `client.cpp:128-146`); index skips non-durable states | `ND` |
| AFR-G-30 | A standby read under a valid lease reflects every write acknowledged before the read was sent, because the primary may not degrade or acknowledge without that standby while the lease is live. The clock assumption (maximum rate drift) is stated and checked at configuration time, or the mechanism does not depend on clocks. | CORE_CONTRACTS R-4, 5 | PARTIAL | 0.9 factor on the standby's own echoed timestamp, rate drift < 10% assumed (`replication.cpp:742-746`); primary refuses to degrade while a lease is live (`shard.cpp:1501-1515`); a forged lease frame extends the lease (AFR-F-02 item 5) | `KD` |

No multi-object snapshot, no cross-shard read consistency and no linearizable index are offered (CORE_CONTRACTS 5).

---

## 8. Authority, failover and fencing (deep)

These clauses state the single property that AFR-F-04 found missing: **authority is proven, bounded in time and
durable at its end**, not believed.

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-31 | **One serving owner.** At any instant at most one node may apply mutations to, or answer default reads of, an object: never two groups serving one slot, never two primaries of one group serving. A configuration that cannot guarantee this (async with automatic failover, sync without witnesses and a non-zero degrade timeout) MUST be refused at start-up unless the operator explicitly accepts the named weaker guarantee. | INV 3; FAILURE_MODEL G1 (PROPOSED); CORE_CONTRACTS 2 (stale term) | PARTIAL | sync with witnesses: HOLDS for writes (EL2-EL4), but a cut-off primary keeps answering reads, gated only by `withdrawn_` (`shard.cpp:542-545`; `B_distributed.md` 4a, 5.2); sync without witnesses: VIOLATED (QA 2 N6, two acknowledging primaries, totals 53,161 vs 52,928); async: acknowledged writes lost on automatic failover (`test_async_failover`), two acknowledging primaries by code reading (QA 2 N6(a), `B_distributed.md` 5.1) | `CON` |
| AFR-G-32 | A node that has learned of a newer term (refusal by a replica, a witness answer, or a promotion it observed) MUST NOT serve or acknowledge at the older term again, including after a restart. The fact is recorded durably before the node answers anything else. | CORE_CONTRACTS 2; new durability requirement (AFR-F-04) | VIOLATED | withdrawal is an in-memory flag (`engine.cpp:1968-1970`); after restart the node is primary at its old term (QA 2 N6) | `CON` |
| AFR-G-33 | **Bounded authority.** A primary may answer default reads and acknowledge writes only while it holds authority that it can prove for a bounded interval (for example a lease granted by the replica set or the witnesses, renewed before expiry). A newer term MUST NOT be able to serve until every lease of the older term has expired or been revoked. | new (AFR-F-04); the mirror of the standby lease suggested in CORE_CONTRACTS 5 | ABSENT | no primary-side lease (`B_distributed.md` 4a) | `CON` |
| AFR-G-34 | Status surfaces report authority only as far as the node can prove it: a node whose authority is unconfirmed reports that, not `role=p`. | new (O1, AFR-F-04) | VIOLATED | O1 (`HEALTH_BASELINE.md`), narrowed by reviewer B 4e | `KD` |
| AFR-G-35 | Promotion eligibility E1-E11 holds; automatic promotion happens only for a group whose mode permits it; operator promotion carries the term it expects and fails on any other, and requires an explicit acknowledgement when E1, E5 or E6 does not hold. | CORE_CONTRACTS 8 (PE-1 to PE-4; PE-3, PE-4 [ACCEPTED]) | PARTIAL | E1 ABSENT (witness has no mode); `PROMOTE` has no term guard (`snc_server.cpp:1315-1319`) | `KD` |
| AFR-G-36 | Liveness health is not promotion eligibility; a shard stall alone never triggers promotion, withdrawal or a grant. | [ACCEPTED: review of PR #36] CORE_CONTRACTS B-2, B-3, PE-4 | PARTIAL | holds when every witness polls HEALTH (`test_health.py` T1, T3, T7, T12); a witness on `--health-verb status` still promotes on a stall (T2, T10; FAILURE_MODEL P2 "KNOWN ISSUE") | `EQ` |

---

## 9. Placement and migration

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-37 | Migration serving proof MS-1 to MS-4: a node serves a slot only when the placement gives it to its group and its local slot state proves the arrival; staged state activates only at the matching epoch; a promoted or ex-standby directory reads placement linearizably and re-seals before serving. | CORE_CONTRACTS 2 (MS-1 to MS-4) [AS-BUILT]; `docs/MIGRATION_FAIL_CLOSED.md` | HOLDS (except a split-brained source, and stale reads from a cut-off source primary for a slot already committed away; AFR-G-31) | `test_fail_closed`, `test_migrate` 67 crash points | `EQ` |
| AFR-G-38 | A placement epoch is adopted node-wide before any state-driven replay for that epoch begins. | [ACCEPTED] INV 17 | HOLDS | `engine.cpp:1640-1642`; `test_migrate` | `EQ` |
| AFR-G-39 | The control plane validates every placement value it decides (well-formed, consistent with membership); an invalid value is refused, never chosen. | new (`B_distributed.md` 6, speculative S5) | ABSENT | the witness treats the body as opaque and accepts any CAS body (`snc_witness.cpp:209-225`) | `KD` |

---

## 10. Trust and security (deep for protocol identity; principle level otherwise)

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-40 | **Authentication.** Every network endpoint authenticates its peer (a cluster member, a client or an operator principal) before acting on any request; an unauthenticated request changes no state and discloses nothing beyond what G-41 allows. | new (AFR-F-02); CORE_CONTRACTS 14 (security, UNDECIDED) | ABSENT | no authentication on any port (audit-D4); QA 2 N1-N4 | `KD` |
| AFR-G-64 | **Cluster identity.** Every control, consensus, replication, seed and node-link message is bound to the cluster (space) identity and the sender's member identity, and a message that fails the check changes no state. This is separate from G-40: identity binding without authentication only stops mistakes, not attackers. | new (AFR-F-02) | PARTIAL (checks exist, unauthenticated) | identity checks use values learnable from HEALTH: replication Hello (`replication.cpp:675-685`), node-link cluster-shape CRC (`node_link.cpp:961`), witness HA verbs (uuid, term). Forged consensus: a `COMMIT` is persisted as chosen (`paxos.h:142-152`), a `PREPARE` base value is adopted (`:157-166`), machine names are invented on demand (`acc_[m]`, `:140`), and one poisoned witness spreads through `read_latest` (QA 2 N3). A Hello-matching peer can inject WAL records into a standby (`replication.cpp:749-756`) | `KD` |
| AFR-G-41 | State-changing management operations (promotion, slot moves, placement changes, debug faults) and data-disclosing operations (seed transfer, `MOVE HAVE`, `MOVE EXPORT`, audit output) require an authorized principal; only non-sensitive status is separable and may be exposed more widely. | new | ABSENT | `PROMOTE`, `MOVE SEAL/STAGE`, witness `DEGRADE/READY/PLACEMENT CAS` open to any peer; the seed port serves a full checkpoint to an all-zero uuid; `MOVE HAVE`/`EXPORT` return payloads (AFR-F-02 item 6) | `KD` |
| AFR-G-42 | No management operation writes to a caller-chosen file-system path. | new (AFR-F-02 item 2) | VIOLATED | `AUDIT <PATH>` (`snc_server.cpp:204-226`); being removed in the separate port-exposure thread | `KD` |
| AFR-G-43 | Every listener binds only the addresses it is configured for; the default is loopback; wider exposure needs an explicit flag. | cody's PR #121 review (D4) | VIOLATED | admin, witness, seed and node-link bind `INADDR_ANY` (`ha.h:97-110`, `engine.cpp:1267`, `node_link.cpp:203`, `snc_witness.cpp:678`); the replication receiver defaults to `0.0.0.0` (`replication.h:232`); `--control` forces the data port to `0.0.0.0` (`snc_server.cpp:596`); being fixed in the separate port-exposure thread. Loopback binding reduces exposure in the interim; it is not a production security solution and satisfies none of G-40, G-41, G-63 or G-64 | `KD` |
| AFR-G-44 | State-changing management actions are recorded in a durable, tamper-evident audit trail with principal, time, target and outcome. | assignment domain G | ABSENT | | n/a |
| AFR-G-63 | Object data leaves a node only to an authenticated member or an authorized principal; no protocol answers an unauthenticated peer with object payloads, checkpoints or WAL. | new (QA of this document) | VIOLATED (code reading) | seed to an all-zero uuid; `MOVE HAVE`/`EXPORT`; replication shipping to any Hello-matching peer | `KD` |

The four concerns stay separate: authentication (G-40), authorization (G-41, G-63), cluster identity (G-64), and resource exhaustion (G-45, section 11), which must hold even against authenticated peers. The concrete mechanism (mutual TLS, shared cluster keys, capability tokens) is a decision (AFR-D-07). The clauses
above are what any mechanism must deliver.

---

## 11. Clocks, resources and liveness

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-45 | Every buffer or queue that a network peer or client can grow is bounded; overload yields a retryable refusal or a bounded wait, never unbounded memory growth or a crash. | CORE_CONTRACTS 11, 14 (resource isolation) | VIOLATED | the standby receive buffer doubles without bound before and after the Hello (`replication.cpp:711`; AFR-F-05); one detached thread per witness connection (`snc_witness.cpp:695-701`); internal `must_deliver` overflow deques unbounded (`runtime.cpp:135, 176`) | `INV` |
| AFR-G-46 | **No safety property depends on synchronized clocks or on timing.** Timeouts affect liveness only; an explicitly stated bound on clock **rate** drift is the only clock assumption allowed, and only for leases (AFR-G-30, AFR-G-33). | CORE_CONTRACTS 14 [ACCEPTED for sessions: work order Part VII] | VIOLATED | session expiry (AFR-G-22); async failover safety depends on `failover_ms` (`B_distributed.md` 5.3) | `CON` |
| AFR-G-47 | No unit of work monopolizes a shard thread beyond a measured budget; work proportional to data size is chunked and resumable. | [ACCEPTED: work order Part XI] CORE_CONTRACTS B-1 | PARTIAL | implemented for migration staging; still unbounded: seal, export, activation, checkpoint snapshot, `rebuild_index` | `PERF` |
| AFR-G-48 | After a covered fault heals, service resumes without operator action, within a stated bound. | FAILURE_MODEL G6 | PARTIAL | a fenced old primary needs `--rejoin` (P1); a wedged shard needs an operator (option B, accepted) | `ND` |

---

## 12. Formats, protocols, management and observability

### 12.1 Formats and protocols (deep: every row is expensive to reverse)

| ID | Guarantee | Source | C++ status | Evidence | Diff |
|---|---|---|---|---|---|
| AFR-G-49 | Every durable format and every wire message is specified in bytes, independently of any language's memory layout, before a second implementation exists. | [ACCEPTED: work order Part XXIV; Codex PR #33 review] CORE_CONTRACTS 1, 14 | PARTIAL | WAL and checkpoint layouts are documented with byte offsets in comments (`wal_format.h:16-74`), but they are C++ structs copied with `memcpy` under static asserts (`types.h:16-17`); no language-independent specification exists; wire messages have none | `EQ` (golden corpus) |
| AFR-G-50 | Every durable format and every protocol carries an explicit version. A reader that meets an unknown version or record kind refuses (fails closed) and names it; it never treats it as damage-to-be-skipped or as end of data. | new (AFR-F-01 item 4) | VIOLATED | durable formats carry `kFormatVersion` (`wal_format.h:10`), but an unknown WAL `rec_type` truncates silently (QA 1 P3), and a superblock copy with an unknown `format` is skipped as damaged so the older copy wins (`storage.cpp:219-222`); the client wire has no version field (`C_interfaces.md` 4.1); replication and seed version by magic only | `KD` |
| AFR-G-51 | Compatibility policy: a stated range of mixed versions (UNDECIDED, AFR-D-06) interoperates; outside it, peers refuse each other with a named error at the handshake. | CORE_CONTRACTS 1 (Upgrade, UNDECIDED) | ABSENT (node links only, v2 refusal) | Hello `node_link.cpp:29-66`; refusal `:952-969`, `:1059-1066` | `EQ` |

### 12.2 Management and observability (light pass, amendment 1)

| ID | Guarantee | Source | C++ status | Evidence |
|---|---|---|---|---|
| AFR-G-52 | One authoritative management API defines management semantics; every CLI is a client of it and implements no protocol logic of its own. | assignment domain F | ABSENT | `snc_ctl` runs the slot-move protocol itself; four separate HEALTH/STATUS parsers (`C_interfaces.md` 6) |
| AFR-G-53 | Management answers are machine-readable with stable error categories; exit codes are documented and stable. | assignment domain F | ABSENT | free-text `NO <English>`; undocumented, inconsistent exit codes; uncaught exceptions exit 134 |
| AFR-G-54 | Any condition that weakens a guarantee is visible without invasive debugging: acknowledging below class, a discarded log tail, a lost replica, failed pinning or page-mode downgrade, an unreachable control plane. | CORE_CONTRACTS H-1; audit M3 | VIOLATED | `torn_tails` never surfaced; lost standby unreported |

---

## 13. Rulings on the known gaps (AFR-R)

Format (Codex, Handoff 1 review item 3). **Owner disposition** is blank: it is cody's to fill. "Evidence" uses the
strength scale: experiment (E), test (T), code reading (C), documented only (D). **Documented today** says whether the repository already records the behavior as a limitation or known issue. A DEFECT ruling on a documented limitation means: if the clause it cites is accepted, the limitation becomes a known violation of that clause; until then it violates nothing accepted (Codex, Handoff 1 review).

| ID | Gap (older ID, source) | Documented today | Affected | Trigger | Evidence | Proposed ruling | Impact / severity | Rust-readiness impact | Smallest fix or experiment | Owner |
|---|---|---|---|---|---|---|---|---|---|---|
| AFR-R-01 | Silent loss on newest-segment damage (audit-D2, AFR-F-01 items 1-3) | yes (audit-D2; STATUS known gaps), narrower than found | G-13, G-14; INV 8 | one damaged or zeroed block in the newest segment | E (reviewer A, QA 1) | **DEFECT** | durability, critical | blocks WAL format design (AFR-D-02) | widened audit-D2 slice (AFR-S-1); experiments E-01, E-02 | |
| AFR-R-02 | Unknown record type read as a torn tail (AFR-F-01 item 4) | no | G-50 | a log written by a newer binary, or one bit in `rec_type` | E (QA 1 P3) | **DEFECT** | durability and upgrade, high | defines record-kind versioning | fold into AFR-S-1 | |
| AFR-R-03 | LSN reuse after truncation (AFR-F-01 item 5) | no | G-15 | any truncation of acknowledged records | E | **DEFECT** | integrity, high | log identity rule | fold into AFR-S-1 | |
| AFR-R-04 | Cross-shard overtaking (audit-D1, AFR-F-06) | yes (audit-D1) | G-19 | batching on, mixed batchable and non-batchable requests to one shard | E (audit, QA 1 20/20) | **DEFECT** (scope per AFR-D-04; flushing the batch before a direct send satisfies both per-shard and per-object scope, so the fix holds unless AFR-D-04 chooses "none") | consistency, high for in-process apps | dispatch design | audit-D1 slice: flush the destination's batch before a direct send; measure the cost (CROSSCORE) | |
| AFR-R-05 | Dedup entry not bound to the request (audit-D3) | yes (audit-D3) | G-21, G-23 | client restart reusing `client_id`, or a buggy client | E (audit, reviewer A, QA 1 P5) | **DEFECT** | false success, high | session record and wire format (AFR-D-05) | none before AFR-D-05 and AFR-D-06 are decided: a request fingerprint changes the durable session record. A provisional C++ change needs explicit authorization and a migration analysis (existing data directories, replication between versions, rollback) | |
| AFR-R-06 | Session expiry by wall clock inside checkpoints (R11, audit 0.2) | yes (FAILURE_MODEL W2; R11) | G-22, G-46 | clock step, long outage, checkpoint after TTL | E (QA 1 P7), T (`test_session_ttl`) | **DEFECT** | double apply, high | session lifecycle (CORE_CONTRACTS 10) | session incarnation (G-1 to G-7) | |
| AFR-R-07 | Failed sequenced ops not recorded; a later success makes the failed seq answer `E_REPLAYED_OLD` | yes (`RETRIES.md:42-45`) | G-21, G-24 | resend after an `E_APP`-class failure | E (QA 1 P6), D (`RETRIES.md:42-45`) | **ACCEPTED BEHAVIOR**: failed ops change nothing, so re-running them is safe; `E_REPLAYED_OLD` is specified as IN-DOUBT | none beyond the stated ambiguity | none | document in the client contract | |
| AFR-R-08 | Deletes never sequenced | yes (CORE_CONTRACTS 7) | G-25 | resend of a delete | E (QA 1 P6), D (`shard.cpp:403`) | **ACCEPTED BEHAVIOR** for the reference: a resent delete answers `E_NOT_FOUND`, which G-24 classes IN-DOUBT, never a double effect, because an ObjectID is never reused (G-01); production SHOULD sequence deletes | low | small | none now | |
| AFR-R-09 | Stale reads from a cut-off primary (CORE_CONTRACTS 5; EXPERIMENTAL_BASELINE Q7) | yes (CORE_CONTRACTS 5) | G-28, G-33; INV 3 | partition of the primary from its replica and witnesses | E (QA 2 N7), D | **DEFECT** against G-28 (documented limitation in the reference until AFR-D-01 is decided) | consistency, high | central: authority model (AFR-D-01) | E: netns partition test (reviewer B S2); fix only after AFR-D-01 | |
| AFR-R-10 | Async standby promoted automatically (R08) | yes (R08 characterization) | G-11, G-27, G-31, G-35 | async group, primary silent `failover_ms` | T (`test_async_failover` asserts acknowledged writes missing after promotion, `:244-246`) | **DEFECT** (F-1: an async replica is warm, never auto-promoted) | durability and split brain, critical for async deployments | witness state gets a mode (CORE_CONTRACTS 9, F-a) | add `mode` to the HA state; refuse auto-promotion | |
| AFR-R-11 | Sync primary degrades alone without witnesses (gap g) | yes (`REPLICATION.md:104-111`, operator responsibility) | G-31 | sync, no `--witness`, timeout > 0 (default 2000 ms) | E (QA 2 N6) | **DEFECT**: refuse this configuration at start unless an explicit flag accepts it | split brain, high | configuration model | start-up refusal plus flag | |
| AFR-R-12 | Fencing not durable (AFR-F-04) | no | G-32 | restart of a fenced primary that cannot reach the new primary or a witness | E (QA 2 N6: two acknowledging primaries, without witnesses, so overlapping R-11; with witnesses the restarted node withdraws on its first `NO term` and the residue is stale reads, R-09) | **DEFECT** | split brain, high | authority model (AFR-D-01) | persist the highest known term before answering; a narrow C++ slice is possible | |
| AFR-R-13 | `PROMOTE` has no term guard (PE-2) | yes (CORE_CONTRACTS PE-2) | G-35, G-41 | any peer, or an operator mistake | E (QA 2 N2) | **DEFECT** | split brain, high | management API | `PROMOTE <term>` refused on mismatch | |
| AFR-R-14 | O1: resumed old primary reports `role=p` (HEALTH_BASELINE O1) | yes (HEALTH_BASELINE O1, tracked) | G-34 | stall then resume; partition | E (reviewer B 4e, narrowed) | **DEFECT** (subsumed by AFR-D-01; status reporting follows authority) | observability, medium | status semantics | none until AFR-D-01 | |
| AFR-R-15 | Standby receive unbounded, pre-Hello (AFR-F-05) | yes (STATUS known gaps) | G-45 | any peer connecting; or a standby slower than its primary under sustained load | C (reviewer B, QA 2 N5) | **DEFECT** | availability, high | flow control in the replication protocol | AFR-S-3 | |
| AFR-R-16 | F6: degraded mode has no back-pressure (BASELINE_READINESS) | yes (F6) | G-48 | standby slower than the write rate | D, T | **ACCEPTED BEHAVIOR** for the reference: the group stays NOT READY (unavailable for failover, never unsafe); production SHOULD offer bounded-lag back-pressure (CORE_CONTRACTS F-5) | availability, medium | replication flow control | none now | |
| AFR-R-17 | F7: readiness latency, one shared proposal queue (SYNC_READINESS 7.5); withdrawn primary NOTREADY every 50 ms (QA 2 N8, narrowed) | yes (F7); NOTREADY loop no | G-48 | stopped witness; many groups | C, T | **ACCEPTED BEHAVIOR** for the reference (liveness only); **DEFECT** for the NOTREADY loop | liveness, medium | consensus service design | stop NOTREADY once fenced | |
| AFR-R-18 | Paxos F8: ballots after a clock step | yes (F8 outcome A approved) | G-46 | clock step back | T (`test_paxos_ballots`), E (C1-C3) | **ACCEPTED BEHAVIOR**: safe by strict promise comparison; liveness cost only (outcome A approved, `PAXOS_BALLOTS.md:300-302`) | none | none: no ballot persistence needed | none | |
| AFR-R-19 | Paxos F9: witness acceptor state restored or rolled back | yes (F9 outcome B approved for the baseline) | G-31; fault model 1.2 | operator restores or deletes a witness state file | E (C4a restored, C4b deleted; `PAXOS_BALLOTS.md:21`) | **UNRESOLVED** for production: outcome B (excluded history plus guard) is approved for the baseline; proposed OUT OF CONTRACT with mandatory detection in production | safety under operator error, high | consensus persistence design (AFR-D-13) | review the candidate design in `PAXOS_BALLOTS.md` section 7 | |
| AFR-R-20 | Sessions do not move with slots; cross-group resend (CORE_CONTRACTS D-3) | yes (CORE_CONTRACTS D-3) | G-26 | in-doubt request across a slot move | C, D | **UNRESOLVED** (AFR-D-08) | duplicate application, medium | session and migration design | E: two-group resend test (audit M2-a) | |
| AFR-R-21 | Unauthenticated ports; forged `COMMIT` and `PREPARE`; receiver takeover and WAL injection into a standby; data disclosure (seed, `MOVE EXPORT`); `AUDIT PATH` (audit-D4, AFR-F-02) | yes ("prototype limitation", `CORE_CONTRACTS.md:756-759`; C2x non-Byzantine proposer, `PAXOS_BALLOTS.md:303`) | G-40 to G-44, G-63, G-64 | any network peer | E (QA 2 N1-N4) | **DEFECT** against the proposed G-40 to G-44 and G-63; a documented exclusion today | security, critical | trust model and every protocol (AFR-D-07) | interim: separate port-exposure thread; full fix after AFR-D-07 | |
| AFR-R-22 | test_xnode X16 flake (CI_FAILURE_DIAGNOSIS) | yes (CI_FAILURE_DIAGNOSIS) | test estate | full CI under load | E (local reproduction 5/20 and 2/55, CI_FAILURE_DIAGNOSIS), not at the current head | **UNRESOLVED** until E-03 reproduces it at the current head | CI trust, medium | none | E-03 | |
| AFR-R-23 | Superblock silently falls back to the older copy (speculative S-a) | no | G-02, G-04, G-32 | damage to the newest superblock copy; or a newest copy with an unknown `format` (`storage.cpp:219-222`, see AFR-R-32) | C | **UNRESOLVED**: experiment first | lease and term regression, potentially high | superblock design | E: corrupt the newest copy after a promotion and a lease take; check term and leases | |
| AFR-R-24 | Older-segment damage: abort with a misleading message; checkpoint-covered segments never read (QA 1 P4) | no | G-14, G-54 | damage in an older segment | E | **DEFECT** (diagnostic only; fail-closed outcome is correct) | operability, low | none | name the damaged file | |
| AFR-R-25 | `E_NO_MEMORY` after the op ran (STABILIZATION_REPORT L-8) | yes (STABILIZATION_REPORT L-8) | G-08 | allocation failure on a cross-shard result | C, D | **DEFECT** | false refusal, medium | error model | distinct IN-DOUBT status | |
| AFR-R-26 | Space-uuid check after the barrier (CORE_CONTRACTS I-c) | yes (CORE_CONTRACTS I-c KNOWN ISSUE) | G-18 | wrong data directory under `--control` | C, D | **DEFECT** | serves the wrong data briefly, medium | start-up sequence | move the check before the barrier | |
| AFR-R-27 | Obligations re-emitted only on events; two silent drop paths (CORE_CONTRACTS O-2, O-3) | yes (CORE_CONTRACTS O-2, O-3) | G-09 | an unexpected status on an intra-node follow-up | C, D | **DEFECT** | settlement liveness, medium | obligation scheduler | a re-emission timer and counters | |
| AFR-R-28 | Lost standby acknowledges LOCAL_DURABLE unreported (H-1) | yes (FAILURE_MODEL P1 "unreported") | G-12, G-54 | failover without `--rejoin` | E (STABILIZATION_REPORT D) | **DEFECT** | silent weakening, high | status model | report the class in STATUS and the answer | |
| AFR-R-29 | Standby read lease depends on clock rate drift; lease frame forgeable | partly (CORE_CONTRACTS 5 states the drift assumption) | G-30, G-40 | drift above 10%; forged frame | C | **UNRESOLVED** for drift (needs a stated bound, AFR-D-01); **DEFECT** for forgery (AFR-R-21) | stale standby reads, medium | lease design | none now | |
| AFR-R-30 | No pinned hash vectors; system xxHash decides placement (AFR-G-03) | no | G-03, G-49 | a different xxHash build | C | **DEFECT** (test gap) | silent remap risk, medium | format specification | add pinned vectors | |
| AFR-R-31 | `PLACEMENT SYNC` (a read) persists a Paxos promise on every witness, can run `ACCEPT`/`COMMIT`, and can starve writers (QA 2 N9) | yes (QA 2 N9, inventory only) | G-46, G-48 | frequent placement reads | C (`snc_witness.cpp:204-207`, `paxos.h:176-180`, `:291-303`) | **DEFECT** (liveness; reads should not need write quorum rounds) | liveness, medium | consensus service read path (AFR-D-13) | none now | |
| AFR-R-32 | Superblock copy with an unknown `format` skipped as damaged; the older copy wins | no | G-50, G-02, G-04 | a superblock written by a newer binary, or damage to the `format` field | C (`storage.cpp:219-222`) | **DEFECT** (fail closed on an unknown version) | upgrade and identity regression, medium | format versioning (AFR-D-06) | experiment with AFR-R-23 | |

### 13.1 Reference treatment and verification strategy

Codex's planning decision (relayed by cody, 2026-11-16): not every C++ defect must be fixed before Rust starts.
Every critical defect must instead be understood, classified, tied to an accepted guarantee and given a
verification strategy. Each ruling therefore also says how the C++ reference is treated:

- **FIX-IN-C++**: the defect compromises C++ as the testing reference (it makes the reference's answers wrong or
  unstable in scenarios the differential harness must cover), so it gets its own slice before the harness relies on
  that area.
- **KNOWN-VIOLATION**: documented; the harness either excludes the triggering scenario for C++ or applies the `KD`
  rule (clause is the oracle); Rust MUST NOT reproduce it.
- **EXPERIMENT FIRST**: the facts are not settled; no treatment until the named experiment runs.

| Ruling | Proposed treatment | Why | Verification strategy |
|---|---|---|---|
| AFR-R-01, 02, 03 (WAL damage, unknown record, LSN reuse) | **FIX-IN-C++**, minimal form (one slice, AFR-S-1): fail closed on detected damage or an unknown record, never reuse an LSN; no format change. This satisfies G-14(a) only; G-14(b) waits for AFR-D-02, and the harness keeps whole-flush loss and superblock rollback as `KD` until then | storage damage is a covered fault like a partition, so a `KD` treatment would be possible. The difference is that a partition defect is visible to a history checker, while silent truncation leaves a recovered state the harness cannot tell from a correct one; and the fail-closed slice is small, format-neutral and needed whatever AFR-D-02 decides. AFR-R-24 (misleading message) rides along with no oracle reason | storage-damage fault injection (bit flip, zeroed block, first record, unknown type) over every segment position; checker: acknowledged-set subset of recovered-set, or named refusal |
| AFR-R-04 (cross-shard overtaking) | **FIX-IN-C++** (audit-D1 slice) | in-process workloads are the core of SNC; a reference that reorders one origin's requests cannot judge ordering | history checker for per-origin FIFO (G-19) on mixed batchable and direct sends |
| AFR-R-05 (dedup not bound to the request) | KNOWN-VIOLATION until AFR-D-05 and AFR-D-06 are decided; then **FIX-IN-C++** | a false "OK, replayed" corrupts the retry oracle, but the fix changes a durable format, which must not be frozen early (Codex, PR #139 review item 2). Meanwhile the harness never reuses a client identity across incarnations, and mismatched-retry scenarios run as `KD` | fingerprint-mismatch tests; retry histories checked for at-most-once and no false replay |
| AFR-R-30 (no pinned hash vectors) | **FIX-IN-C++** (test-only) | placement equivalence between implementations needs fixed vectors | golden vectors shared by both implementations |
| AFR-R-06 (session TTL by wall clock) | KNOWN-VIOLATION | the fix is the session redesign (AFR-D-05); the harness avoids clock steps and outages beyond the TTL for C++ | clock-step injection, judged against G-22 for Rust only |
| AFR-R-09, 12, 14, 29 (authority) | KNOWN-VIOLATION | the fix is the authority model (AFR-D-01); partition scenarios run as `KD` | netns partition tests with a linearizability checker (single object) |
| AFR-R-10, 11 (async auto-promotion; sync without witnesses) | KNOWN-VIOLATION; harness uses only the reference configuration | unsafe configurations are excluded, not tested for equivalence | start-up refusal tests (Rust), configuration matrix |
| AFR-R-13 (unguarded `PROMOTE`) | KNOWN-VIOLATION | harness drives promotion only through guarded paths | stale-term promotion refused (Rust) |
| AFR-R-21 (unauthenticated ports) | KNOWN-VIOLATION, interim loopback binding by the port-exposure thread | harness runs on an isolated network; not an oracle problem | adversarial-peer tests (forged `COMMIT`, oversized frames) against Rust; memory bound check |
| AFR-R-15 (unbounded standby receive) | KNOWN-VIOLATION for forged peers; **EXPERIMENT FIRST** for the benign trigger | if harness load can drive a slow standby to exhaustion, C++ becomes an unstable reference under load and the slice (AFR-S-3) moves to FIX-IN-C++ | measure standby memory under the harness's heaviest load profile |
| AFR-R-28 (lost standby acknowledges LOCAL_DURABLE unreported) | **FIX-IN-C++** (report the achievable class in the status surface only; no client-wire change) | every failover without `--rejoin` triggers it, so every later fault scenario would judge C++ at an unreported weaker class. The answer-level field waits for AFR-D-14 and AFR-D-06, so no wire format is frozen | the harness reads the status surface and compares it with the clause |
| AFR-R-25, 26, 27 (false refusal, uuid check order, obligation timer) | KNOWN-VIOLATION | narrow scenarios the harness can exclude or mark `KD` | targeted tests per clause |
| AFR-R-31, 32 (placement reads take write rounds; unknown superblock format) | KNOWN-VIOLATION; R-32 experiment with R-23 | liveness, and upgrade scenarios the harness does not yet run | |
| AFR-R-07, 08, 16, 17 (loop excepted), 18 | ACCEPTED BEHAVIOR: part of the clause | | tested as `ND` or `EQ` |
| AFR-R-17 (NOTREADY loop) | KNOWN-VIOLATION | liveness only | |
| AFR-R-19, 20 | UNRESOLVED; scenario excluded | needs a design decision | after AFR-D-13 (F9 design, `PAXOS_BALLOTS.md` section 7) and AFR-D-08 |
| AFR-R-22, 23 | EXPERIMENT FIRST | | E-03; superblock corruption experiment |

The FIX-IN-C++ set is deliberately small: four slices now (WAL fail-closed, audit-D1, pinned vectors, class reporting in status), and audit-D3 after its format decisions. All are narrow and on paths the harness must trust. None is started; each needs its own authorization. Everything
else stays a documented known violation until the decision that fixes it is made.

### 13.2 What is ready to stabilize, and what is not

The purpose of this phase is to separate decisions that are ready to freeze from those that are not (Codex,
planning note). Proposed split:

| Ready to stabilize now (evidence holds, low regret) | Not ready (decision or evidence missing) |
|---|---|
| ObjectID shape and no-location rule (G-01); independent version domains (G-04) as rules (their storage weakness is AFR-R-23) | Authority and leases (AFR-D-01) |
| Atomic unit = one op on one owner shard; obligations for everything wider (G-06, G-07) | End-of-log and WAL framing (AFR-D-02) |
| LOCAL_DURABLE as an acknowledgement point; APPLIED as an explicit weaker class | Replication group model and membership (AFR-D-03, AFR-D-11), and with it HA_DURABLE ("every automatically promotable replica" depends on the group model and on E1 mode, AFR-R-10) |
| Checkpoint contract CC-1 to CC-5 (G-17) | Session identity and sessions across moves (AFR-D-05, AFR-D-08) |
| Migration serving proof MS-1 to MS-4 and epoch adoption (G-37, G-38) | Byte formats, versioning, compatibility (AFR-D-06; G-49 to G-51) |
| Liveness health separate from promotion eligibility (G-36) | Trust model (AFR-D-07) |
| Fail-closed and no-silent-loss principles (G-14, G-16, G-50) as principles; their mechanism is not ready | Schema evolution (AFR-D-09) |
| | Ordering scope and default read consistency (AFR-D-04, AFR-D-10): small, decidable now, but not yet decided |
| | Slot hash freeze, witness durable state, status taxonomy, configuration safety (AFR-D-12 to AFR-D-15) |

Nothing in the right column should be frozen to move faster; nothing in the left column needs Rust to wait.

---

## 14. Decisions required before Rust crate design (AFR-D)

These are the expensive-to-reverse choices. Each needs cody's decision (with Codex's review) before the clause it
unlocks can be accepted. The recommendation is mine; none is accepted.

| ID | Decision | Unlocks | Options | Recommendation and reason |
|---|---|---|---|---|
| AFR-D-01 | **Authority model** | G-28, G-31 to G-34, G-37, G-46; R-09, R-12, R-14, R-29 | (a) leader leases; (b) quorum-confirmed authority (a quorum round per read or per batch, no clock assumption) | **No recommendation yet.** A unified model is required first (section 14.1); both options are then compared against the same invariants. |
| AFR-D-02 | **End-of-log rule** (how recovery knows how far acknowledgements went) | G-14, G-15; R-01 to R-03 | (a) a durable acknowledged-LSN watermark; (b) self-describing flush frames (per-flush header and trailer with LSN range and CRC) that let recovery tell a torn flush from damage before it; (c) both | **No recommendation until measured.** (b) alone cannot tell a wholly lost or zeroed newest flush from an unwritten tail, which is exactly the R-01 trigger, unless a replica holds a copy; in a no-replica configuration G-14 then needs (a). The cost of (a) (an extra synchronous write or a piggy-backed one) and of (b) are not measured anywhere in the repository. Proposed: an experiment measuring both on the R640 data disk, then decide; (c) is the likely answer. |
| AFR-D-03 | **Replication group model** | G-31, G-35; membership | (a) keep primary/standby pairs with witnesses; (b) N-replica groups with majority acknowledgement | **No recommendation yet.** Today's two sources of truth (pair HA state, N-member placement, `B_distributed.md` 0) are a problem to solve, not a reason to pick (b). A comparison is required first (section 14.2). |
| AFR-D-04 | **Ordering scope** | G-19; R-04 | per (origin, shard); per (origin, object); none | per (origin, shard): it makes routing purely an optimization, which the README already claims. |
| AFR-D-05 | **Client and session identity** | G-21 to G-24; R-05, R-06 | server-issued SessionID (CORE_CONTRACTS G-1); client id plus incarnation | server-issued SessionID: no clock in safety, carries a principal later (AFR-D-07). |
| AFR-D-06 | **Format and protocol versioning and compatibility** | G-49 to G-51; R-02 | strict same-version clusters; N/N-1 rolling upgrade; negotiated features | N/N-1 with explicit versions on every format and handshake; specify bytes first (AFR-G-49). |
| AFR-D-07 | **Trust model** | G-40 to G-44; R-21 | mutual TLS with a cluster CA; shared cluster key plus message authentication; capability tokens for management on top of either | mutual TLS for transport identity, plus a cluster id in every consensus and replication message; capabilities for management principals. |
| AFR-D-08 | **Sessions and slot migration** | G-26; R-20 | move sessions with the slots they touched; refuse sequenced requests whose session the destination does not hold (IN-DOUBT) | refuse as IN-DOUBT (CORE_CONTRACTS G-7): safe, simple, costs only availability after a move. |
| AFR-D-09 | **Schema evolution** | G-10 | refuse on change (today); versioned types with upgrade functions run at recovery; lazy per-object upgrade | versioned types with an explicit upgrade function per version step; never implicit reinterpretation. |
| AFR-D-10 | **Default read consistency** | G-28, G-29 | linearizable single-object default (needs AFR-D-01); durable-but-possibly-stale default with a stronger opt-in | linearizable single-object default, so applications never reconstruct consistency themselves (assignment domain A). |
| AFR-D-11 | **Membership change** | G-31, G-39 | static (today); reconfiguration through the consensus service | consensus-driven reconfiguration, specified before the control-plane crate; static is acceptable for the first Rust milestone if stated. |
| AFR-D-12 | **Slot hash freeze** | G-03; R-30 | freeze XXH3-64 top 12 bits as specified bytes; choose a different, fully specified function before any persisted placement exists in production | freeze the current function with pinned vectors and a vendored implementation: persisted placement already depends on it, and nothing measured argues for a change. |
| AFR-D-13 | **Durable consensus state and member replacement** | R-19, R-31; G-31 | keep outcome B (excluded history plus guard); adopt the `PAXOS_BALLOTS.md` section 7 design (new durable witness state); move to a different consensus design for the Rust control plane | review the section 7 design before the control-plane crate; outcome B is not acceptable for production. |
| AFR-D-14 | **Client status and IN-DOUBT taxonomy on the wire** | G-08, G-24, G-53, G-62; R-25 | keep today's status codes and document them; a versioned status enum with explicit categories (OK, refused-no-effect, in-doubt, retry-elsewhere) | versioned categories: every client library and the differential harness need the same classification, and it is part of the wire format (AFR-D-06). |
| AFR-D-15 | **Configuration-safety model** | G-31; R-10, R-11 | allow every mode with documentation (today); refuse weaker modes unless an explicit flag names the weaker guarantee | refuse with an explicit flag; may be folded into AFR-D-03 if the group model removes the unsafe modes. |

### 14.1 What AFR-D-01 must specify before a choice

One model covers every path by which a node decides it may serve: primary reads, primary writes, durable fencing,
promotion, restart, slot migration (source and destination) and recovery after a partition heals. For each path it
states:

1. **The safety invariants every option is judged against:** G-28 (read your acknowledged writes), G-31 (one serving
   owner), G-32 (newer term is durable and final), G-34 (status follows authority), G-37 (migration serving proof).
2. **How authority is obtained, renewed, ended and proven**, and what is persisted before a node acts on it.
3. **Stale-authority rejection:** what a node holding older authority sees (a refusal naming the newer term or
   epoch), on every path, including after a restart that lost memory.
4. **Clock assumptions:** none for (b); for (a) a stated maximum rate drift, how it is checked, and what happens
   when it is exceeded (safety must fail closed, not open).
5. **Cost:** round trips on the read and write paths, measured, not estimated (INV 14).

The same scenarios (partition, stall and resume, restart while fenced, promotion during migration) are run as
thought experiments against both options, and the result is written up before cody decides.

### 14.2 What AFR-D-03 must specify before a choice

For both the existing pair model and an N-replica model:

1. **Quorum intersection:** which sets must intersect (acknowledgement and promotion, promotion and promotion) and why.
2. **Data commit:** when a write counts as committed, and what a replica may hold that is not committed.
3. **Witness responsibilities:** what witnesses decide (term, placement, membership) and what they never hold (data).
4. **Promotion eligibility:** E1 to E11 restated for the model, including how the new primary is shown to hold every
   committed write.
5. **Membership transitions:** adding, removing and replacing a replica or witness without a window of two quorums.
6. **Complexity and guarantees compared:** failures tolerated per replica count, write latency, recovery work, and
   the code and test surface each needs.

---

## 15. Differential-testing treatment

The harness compares C++ and Rust executions **against this document**, not against each other alone.

1. A clause marked `EQ` with C++ status HOLDS is a **required equivalence**: both implementations must agree with
   each other and with the clause.
2. A clause marked `KD` or a ruling of DEFECT: the **clause** is the oracle. C++ divergence is recorded as an
   expected known defect with its AFR-R ID; Rust divergence fails.
3. `ND` clauses define the allowed outcome set (for example, which of several legal answers, or interleavings); the
   harness checks membership in the set, not equality.
4. `DUR`, `CON` and `INV` failures fail a run in either implementation, regardless of what the other did.
5. `PERF` is judged only against preregistered thresholds (AGENTS.md change discipline), never as correctness.
6. A ruling of ACCEPTED BEHAVIOR becomes part of the clause (the allowed outcome set) and is tested as `ND` or `EQ`.
7. A ruling of UNRESOLVED keeps the affected scenario out of the equivalence suite until it is ruled, and the
   suite lists it as excluded.

8. **Every accepted guarantee has a named oracle** before it counts as covered. The oracle is a check that
   needs no knowledge of either implementation's internals:

   | Clauses | Oracle |
   |---|---|
   | Identity (G-01 to G-05, G-55, G-56) | pinned vectors; an ObjectID-uniqueness check over the full history, across restarts |
   | Atomicity and obligations (G-06 to G-10, G-57) | a state invariant per workload (for example the conserved total of transfers) checked after every fault; every obligation eventually observed applied |
   | Durability (G-11 to G-18, G-58, G-59) | the acknowledged set (with achieved class) is a subset of the recovered set after each covered fault, or a named refusal; the reported class matches |
   | Ordering (G-19, G-20) | a per-origin FIFO check on the recorded history |
   | Dedup and retry (G-21 to G-26, G-60 to G-62) | at most one effect per (session, seq); no OK-replayed for a mismatched request; IN-DOUBT classification of every unanswered request |
   | Reads (G-27 to G-30) | a single-object linearizability check (with the weaker read modes checked against their stated bound) |
   | Authority and placement (G-31 to G-39) | an operation history with each request's invocation and completion interval, the answering node's term and epoch, and explicit authority evidence (which lease or confirmation the answer relied on); a linearizability check over that history. Overlapping successful answers with timestamps only flag obvious split brain: timestamps alone cannot establish linearizability or prove two leaders did not overlap (Codex, PR #139 final review) |
   | Security and resources (G-40 to G-45, G-63, G-64) | adversarial-peer scenarios: no state change, no disclosure, bounded memory |
   | Formats and management (G-49 to G-54) | golden corpus round trips; refusal of unknown versions; documented exit codes |
   | Liveness (G-46 to G-48) | preregistered time bounds after a fault heals; measured, never used as a correctness verdict |

9. **Coverage reports show four outcomes, never two:** PASS, FAIL, KNOWN-VIOLATION (ran, and violated as a ruled
   defect) and EXCLUDED (not run, with the ruling that excludes it). A KNOWN-VIOLATION or EXCLUDED scenario is never
   counted as passing, and the report lists them on every run.

Prerequisites the repository lacks today (`D_verification_docs.md` 8): byte-level format and wire specifications
(AFR-G-49), a golden corpus of WAL, checkpoint and wire samples, a history checker for G-19, G-21 and G-28, a
fault-injection layer for storage damage and I/O errors, and test drivers that do not depend on C++ log text.

---

## 16. What acceptance of this document would and would not do

- It would make the accepted clauses the specification that SNC-ADR-RUST-001 calls authoritative, and supersede the
  overlapping PROPOSED clauses of CORE_CONTRACTS and FAILURE_MODEL.
- Three decisions stay separate, for each clause:
  1. **Accepting the contract**: the clause becomes part of the specification.
  2. **Proving the C++ reference satisfies it**: its status reaches HOLDS with a named oracle (section 15), or it is
     ruled a KNOWN-VIOLATION with a visible coverage entry.
  3. **Authorizing Rust implementation**: a later, separate decision (Milestone 3, readiness review), which this
     document does not make.
  Accepting a clause implies neither of the others.
- It would not authorize any implementation: each DEFECT ruling still needs its own slice, and Rust work still
  waits for the readiness review (ADR Timing clause).
- Owner dispositions in section 13 and decisions in section 14 can be accepted one at a time.

---

## 17. How this document was verified

- Drafted by the lead reviewer from the Phase 1 inventory, CORE_CONTRACTS, FAILURE_MODEL, the reviewer and QA notes
  in `docs/afr-001-working-notes/`, and the source at the baseline.
- Two independent verifiers, neither given the drafting notes, checked every clause, ruling and decision against the
  source and the cited documents. One covered sections 2 to 7, the other sections 8 to 14.
  - They found that the `engine.cpp` and `snc_server.cpp` line numbers were stale. Those numbers came from the
    Phase 1 baseline `24006ee`, before PR #136 added lines to both files. Every citation was re-pointed to the
    baseline and the shifted lines were spot-checked.
  - They downgraded seven statuses to PARTIAL or VIOLATED: G-01, G-04, G-25, G-26, G-27, G-29 and G-31. They also
    downgraded G-36 to PARTIAL. G-49 went the other way, from ABSENT up to PARTIAL.
  - They fixed sources and evidence: G-16 cited the wrong C7, and R-10, R-18, R-19 and R-22 had their evidence
    corrected.
  - They questioned three 13.1 treatments. As a result the reason for R-01 to R-03 was rewritten, R-24's oracle
    claim was removed, R-28 moved to FIX-IN-C++, and R-15 now needs an experiment first.
  - They withdrew the unmeasured cost claim in AFR-D-02.
  - They added nine clauses (G-55 to G-63), two rulings (R-31, R-32), four decisions (D-12 to D-15) and the
    "documented today" column.
- Passing tests are not taken as proof: every HOLDS cites the test or experiment, and every HOLDS (code) is marked as
  code reading only.
- Codex's independent review via cody is still to come. Nothing here is accepted until it does.

### Revision 1 (Codex's PR #139 review, `docs/handoff/PR139_AFR001_Phase2_Codex_Review.md`)

A focused revision, with no redesign. Each of Codex's nine points maps to one change:

1. G-14 is split into (a) detection and (b) the acknowledgement boundary. The fail-closed slice is stated as
   meeting (a) only.
2. R-05: no durable fingerprint is written before AFR-D-05 and AFR-D-06; R-05 is a known violation until then.
3. AFR-D-01: the recommendation is withdrawn. Section 14.1 sets out what a unified authority model must specify.
4. AFR-D-03: the recommendation is withdrawn. Section 14.2 sets out what the comparison of the two group models
   must cover.
5. G-09 is split into durable creation, at-least-once delivery and idempotent processing. Exactly-once is not
   claimed.
6. G-11 and G-12 now cover the requested versus the achieved class, and how degradation is represented. R-28
   reports through the status surface only.
7. Authentication (G-40), cluster identity (new G-64), authorization and resource exhaustion are now separate
   clauses. Loopback binding is stated as an interim measure.
8. Section 15 gives an oracle per clause group and four coverage outcomes.
9. Section 16 separates accepting a clause, proving the C++ reference meets it, and authorizing Rust.

Every owner disposition remains pending.

