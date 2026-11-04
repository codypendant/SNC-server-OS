# SNC core contracts (deliverable A, with F, G, H and I)

Status: **DRAFT for review** (core stabilization work order, 2026-11-04).
This document says what SNC *means*. Where the code does something
different, the code is not automatically right: the gap is marked and
listed in [STABILIZATION_REPORT.md](STABILIZATION_REPORT.md) section L.

## 0. How to read this document

Requirement keywords are as in RFC 2119: **MUST**, **MUST NOT**,
**SHOULD**, **SHOULD NOT**, **MAY**.

Every normative statement carries its source in brackets:

| Tag | Source |
| --- | --- |
| `[INV n]` | an accepted invariant in AGENTS.md |
| `[ACCEPTED: ...]` | a decision recorded by cody or Codex (the work order, a review) |
| `[AS-BUILT]` | what the code does today, with a citation |
| `[PROPOSED]` | a contract suggested here, not yet accepted. **It binds nothing until accepted.** |

Status markers:

| Marker | Meaning |
| --- | --- |
| **UNDECIDED** | no contract yet |
| **AS-BUILT — CONTRACT UNDECIDED** | behavior exists, but nobody has decided it is the contract |
| **UNPROVEN** | the contract is stated, but no test or run shows it holds |

Evidence levels (EL0 code inference ... EL6 formal) are those of
[STABILIZATION_REPORT.md](STABILIZATION_REPORT.md). Fault classes (P1,
N2, C7, ...) are those of [FAILURE_MODEL.md](FAILURE_MODEL.md).

## 1. Identities

| Identity | Definition | Survives | Reuse | Source |
| --- | --- | --- | --- | --- |
| **ObjectID** | 128 bits: a mint lease (high) and a sequence within it (low); location-free | restart, failover, migration (moves with its slot), restore | **never** reused, deleted or not | [INV 2], [AS-BUILT] `include/snc/object_id.h` `Minter`, C12; lease blocks from the control plane are disjoint (`test_control.py`) |
| **Space** | 128-bit UUID of one data set; every node, stream, seed and witness HA machine carries it | everything; a node with another space is refused | n/a | [AS-BUILT] superblock, replication handshake (`kRefusedSpace`), witness `uuid` |
| **NodeID** | a process's identity in the control plane (`snc_ctl init --nodes`), stored in `DIR/node` | restart and `--rejoin` (it comes from the required `--node-id` flag; `DIR/node` is rewritten when missing, `control.h:194`) | not reused | [AS-BUILT] `tools/control.h` `resolve`; `--rejoin` loses the stored placement file instead (L-12) |
| **Replication group** | the members that hold the same slots, one primary at a time | membership is fixed at `init` in v0.x | n/a | [INV 13] |
| **Placement epoch** | version of the slot → group map; strictly increasing | restart (stored map, then refreshed); independent of HA | n/a | [INV 12], [INV 17] |
| **HA term** | version of a group's primary; incremented by promotion, stored in the superblock and the witness HA machine | restart; failover bumps it | never reused within a group | [INV 12], [AS-BUILT] `Engine::promote` |
| **Move** | one slot migration; ID = the placement version that created it; increases | driver death, node crash (all steps resume) | never reused | [AS-BUILT] `tools/migrate.h` |
| **Client** | today a 64-bit `client_id` the caller supplies (`Client::enable_session(u64)`, `client.cpp:98`; `ClusterClient::enable_sessions`); the library generates none; no server-issued identity | as long as its sessions exist | **AS-BUILT — CONTRACT UNDECIDED**: nothing stops reuse or collision | [AS-BUILT] `src/net/client.cpp`, `src/net/cluster_client.cpp:167` |
| **Session** | dedupe evidence for one (client, owner shard): last sequence and its answer | restart, failover (in the WAL group, replicated), checkpoint; **not** TTL expiry; **not** slot migration | n/a | section 7; G in section 10 |

**Upgrade.** **UNDECIDED.** Durable formats are versioned (`format` = 1,
magics) but have no compatibility matrix; wire protocol v0 has no version
field (EVIDENCE R12). [ACCEPTED: work order Part XXIV] Each durable format MUST be specified in
bytes, independently of any language's layout, before a second
implementation exists. [PROPOSED] An identity MUST NOT change meaning
across an upgrade.

**Restore** (from backup): **UNDECIDED**; there is no backup or restore
facility.

## 2. Ownership

| Role | Definition | Rights | Source |
| --- | --- | --- | --- |
| **Placement owner** of slot S | the group the current placement (epoch e) assigns S | its primary MAY serve S once S is ACTIVE there | [INV 13], [INV 17] |
| **Serving owner** of S | the one primary that runs ops on S now: placement owner, current term, slot ACTIVE (not SEALED or STAGED), not withdrawn | the only node that MAY apply a mutation to an object of S | [INV 3], [AS-BUILT] `Shard::serving` |
| **Shard owner** | within a node, the shard that `SlotMap` gives S's hash; all ops on S run on that thread | executes; others forward or refuse (`E_NOT_OWNER` for sequenced) | [AS-BUILT] `shard.cpp` `execute` |
| **Primary** | group member with the current term, not withdrawn | serves, ships WAL, emits follow-ups | [AS-BUILT] |
| **Standby** | member receiving the primary's WAL | [INV 3] MUST NOT apply client mutations. As built it refuses them (`E_STANDBY`) and serves reads under a valid lease (section 5) | [AS-BUILT] `shard.cpp:300-313` |
| **Fenced former primary** | a primary that learned of a newer term (or was refused by the standby) | [INV 3] MUST NOT serve or acknowledge. As built it answers `E_STANDBY`/`E_FENCED` and returns only by restart/rejoin | [AS-BUILT] `Engine::withdraw`; FAILURE_MODEL C3 |

Rules:

- At most one serving owner per slot at any instant [INV 3]. Evidence:
  probe of `test_migrate` and P5, never two (EL3-EL4).
- **Failover is not migration** [INV 11]. A failover changes the term,
  never the placement. A migration changes the placement, never the
  term.
- **Stale placement** [INV 3, INV 17]: a node MUST NOT serve a slot its
  adopted map does not give its group. As built:
  - It answers `E_MOVED` (with its epoch) for slots of other groups.
  - It answers `E_MIGRATING` for its own SEALED or STAGED slots.
  - It adopts a newer epoch node-wide before replaying for it [INV 17].
- **Stale term** [PROPOSED; follows from INV 3]: a node with an older
  term MUST NOT acknowledge any write after a newer term is chosen. As
  built, this rests on two mechanisms, and is **violated for async
  pairs** (FAILURE_MODEL C3 exception and C7; STABILIZATION_REPORT L-1).
  - The standby refuses the old term's stream.
  - The old primary withdraws when it learns of the new term.
- **Recovery:** a node owns nothing until its recovery barrier (section
  12).

## 3. Mutation stages

| Stage | Meaning | As built | Observable to the client? |
| --- | --- | --- | --- |
| RECEIVED | bytes of a request parsed by the owner's shard | `NetServer` | no |
| ACCEPTED | passed every check (role, session, serving, existence, type, args, version, `validate`); a refusal before this changes nothing | `run_exec` | the refusal status |
| APPLIED | the object's new state in memory; later ops on this shard see it | `apply` | only with `allow_undurable` reads, or APPLIED-mode ack |
| LOCALLY COMMITTED | the after-image (plus session record and `local()` peer) appended to the shard's WAL buffer as one atomic group | `kRecGroupNext` | no |
| LOCAL_DURABLE | that group written with `RWF_DSYNC` on this node | `WalStream::flush_done` | DURABLE ack without a standby, or async, or degraded |
| REPLICATED | the standby has received the group | sender | no |
| HA_DURABLE | the group is durable on every member a covered failover could promote: sync, not degraded → the standby's fsync ack | `Shard::durable()` with `repl_hold_` | DURABLE ack in sync mode |
| ACKNOWLEDGED | the client got OK | `complete_local` after the mode's stage | yes |
| SETTLED | every durable obligation the mutation created has been applied at its target (e.g. the credit of a transfer) | follow-up applied and its outbox entry cleared | no direct signal; read the target |

Notes:

- The standby can hold records that the primary later loses.
  `WalStream::start_flush` ships records to the standby **before** the
  local fsync (`storage.cpp:618,653`). A promoted standby can therefore
  hold a write the old primary never acknowledged. **AS-BUILT — CONTRACT
  UNDECIDED**; [PROPOSED] allowed, because the write was not
  acknowledged (its client sees IN-DOUBT, section 6).
- Acknowledgement and settlement are separate. An OK never means SETTLED
  for a multi-object op that crosses shards or groups [ACCEPTED: work
  order Part III].

## 4. Durability classes

| Class | Definition | Survives |
| --- | --- | --- |
| **APPLIED** | in-memory change only | nothing; lost on crash |
| **LOCAL_DURABLE** | crossed this node's persistence boundary (`RWF_DSYNC`) | process crash (P1); host crash (H1, **UNPROVEN** at host level); **not** loss of this node or its disk; not power loss without honest storage (H3, UNPROVEN) |
| **HA_DURABLE** | crossed the persistence boundary that the automatic-failover contract requires: durable on the primary and on every member a witness may promote | any covered single-member failure: P1; H1 and S4 **UNPROVEN** |
| **SETTLED** | all durable distributed obligations of the operation completed | — |

Verified against the code and tests (work order Part III):

| Claim | Verdict | Evidence |
| --- | --- | --- |
| sync replicated DURABLE OK → HA_DURABLE | **holds** while not degraded. While degraded the OK is LOCAL_DURABLE, and the witnesses refuse promotion until SYNCED, so no stale promotion happens | EL2-EL4 (`test_replication`, `test_witness.py`, Vultr P3); `Shard::durable` |
| async replicated DURABLE OK → LOCAL_DURABLE, not necessarily HA_DURABLE | **holds, and is a KNOWN ISSUE**: the witness still auto-promotes, so the OK is not HA_DURABLE but is treated as if it were | `test_async_failover` (EL2) |
| cross-group transfer OK → durable debit + durable obligation, not SETTLED | **holds** | `test_transfer`, `test_cluster`, P4 (26K unsettled during a cut, all settled after) |
| a group whose standby was lost (fenced old primary, never rejoined) | every OK is LOCAL_DURABLE, with no fault reported (`role=n`) | MEASURED, STABILIZATION_REPORT D |

[PROPOSED] Acknowledgement durability (the class an OK guarantees) and
settlement (whether its obligations completed) MUST be reported
separately. An OK MUST NOT be described as more than its class. A node
that acknowledges at a lower class than its configured mode (async,
degraded, standby lost) SHOULD say so in `STATUS` and in its
acknowledgements (section 6).


## 5. Read consistency (work order Part IV)

There is one read API: an `EXEC` of an op whose kind is `READ`, on one
object. There is no multi-object read, snapshot or version-pinned read
(`include/snc/snc.h:45-60`). All statements below are [AS-BUILT] at EL0
(code reading) unless a test is named.

| Read | Sees | Answered when | Contract |
| --- | --- | --- | --- |
| Owner, default | the applied in-memory state, which can include writes not yet durable | only once the object's last LSN is `durable()` (`shard.cpp:362-364`, `complete_local`); in sync mode that includes the standby's ack | **AS-BUILT — CONTRACT UNDECIDED**. In effect: the answer never reveals a state that could be lost by a covered failover, because it waits for the same point an acknowledgement would. A read that was answered is at least as new as every write acknowledged before it started (single object), **while this node is still the primary**; see the cut-off row below |
| Owner, `allow_undurable` (`kReqAllowUndurable`) | the applied state | immediately | **AS-BUILT**: may show a state that a crash or failover later loses. Documented as "reads: do not wait" (`snc.h:26`) |
| Standby, under a read lease | the standby's installed state (whole WAL groups only, `replication.cpp:640`) | once locally durable on the standby; `E_STALE` without a valid lease, `E_STANDBY` for any non-READ op (`shard.cpp:300-313`) | **AS-BUILT — CONTRACT UNDECIDED**. The lease is granted only in sync, non-degraded mode, and the primary may not degrade while a lease it granted is unexpired (`shard.cpp:1501-1515`). The lease uses each side's own clock and a 0.9 factor: **its safety depends on bounded clock rate drift (UNPROVEN)**; with session TTL, one of the two clock dependencies listed in section 14 |
| Secondary index (`index_lookup`, in-process only; not on the wire) | per shard, objects whose current state is durable and whose slot is served | after a fan-out to every shard | **AS-BUILT — CONTRACT UNDECIDED**: not a snapshot across shards; an object with a write in flight is **hidden** (even though its older durable state matched); a key change in flight hides it under both keys; order is arbitrary and changes at recovery; `max_results` truncation picks an arbitrary subset; a partial failure returns partial results with an error status |
| Slot sealed or staged (migration) | nothing | refused `E_MIGRATING` (own slot) or `E_MOVED` (foreign) | [AS-BUILT] `refuse_unserved` (`shard.cpp:78-91`, checked in `run_exec`); **AS-BUILT — CONTRACT UNDECIDED** for reads |
| Old primary cut off, before it learns of the new term | its own state, which the new primary may already have overwritten | a default read of an object whose last write is already durable answers **at once** (`durable(last_lsn)` is already true, `shard.cpp:362-364`); only objects with a write in flight wait. It learns the new term only through the standby's refusal or a `NO term` reply from the witnesses, both of which need connectivity | **KNOWN GAP (INFERRED, EL0): stale reads are possible during a partition.** This violates R-2 below; a primary-side read lease (the mirror of the standby's) would close it. After it learns: new reads `E_STANDBY`, waiting reads `E_FENCED` |

Cross-object reads:

- A same-shard multi-object op is one atomic WAL group, so readers see
  both objects change together once durable (`TRANSFERS.md:132-143`).
- A cross-shard or cross-group transfer is acknowledged at the debit;
  the credit lands later. A reader can see the debit without the credit
  (section 13). This is a stated property, not a defect.

[PROPOSED] contract for v0.2:

- R-1. A default owner read MUST NOT return a state that a covered
  failover could lose (it waits for the acknowledgement point of the
  group's mode, section 6). This is what the code does; the proposal is
  to make it the contract.
- R-2. A read answered `OK` MUST reflect every write to that object
  acknowledged before the read was sent (single-object
  read-your-acknowledged-writes). Across objects nothing is promised.
- R-3. `allow_undurable` and standby reads MUST be named as weaker in the
  API, never as the default.
- R-4. The standby read lease MUST state its clock assumption (maximum
  rate drift) or be replaced by a mechanism that does not depend on
  clocks. **UNDECIDED.**
- R-5. Index results are hints: the caller MUST re-read the objects it
  acts on. Whether a durable older match should be returned instead of
  hidden is **UNDECIDED**.

## 6. Acknowledgement semantics (deliverable H)

### 6.1 The matrix

One row per configured mode; the columns are the stages of section 3.
"Ack" is the point the client gets `OK` for a `DURABLE` mutation (the
default). A `Durability::APPLIED` mutation is acknowledged at APPLIED in
every mode (`shard.cpp:420`) and is lost by any crash. Creates ignore
`APPLIED` and always wait for durability (`shard.cpp:485`).

| | Local only (no standby) | Sync, healthy | Sync, degraded (grant on record) | Async | Group with lost standby (`role=n` after a failover) |
| --- | --- | --- | --- | --- | --- |
| Receive → accept → apply | owner shard, in order | same | same | same | same |
| Visibility to other ops on the shard | at APPLIED | at APPLIED | at APPLIED | at APPLIED | at APPLIED |
| Visibility to a default read's *answer* | at LOCAL_DURABLE | at HA_DURABLE | at LOCAL_DURABLE | at LOCAL_DURABLE | at LOCAL_DURABLE |
| WAL write | group commit, `RWF_DSYNC` | same; shipped to the standby **before** the local fsync | same, shipping continues if connected | same | same, no shipping |
| Standby receipt / durability | n/a | received, then fsynced, then acked by LSN | best effort | best effort, not waited for | n/a |
| **Ack point** | LOCAL_DURABLE | **HA_DURABLE** (primary durable + standby acked) | LOCAL_DURABLE | LOCAL_DURABLE | LOCAL_DURABLE |
| Survives primary process crash + restart | yes (P1) | yes | yes | yes | yes |
| Survives loss of the primary's host or disk | no | **yes** | no | no (KNOWN ISSUE: and the witness promotes anyway, section 9) | no, and **nothing reports it** |
| Automatic failover permitted | n/a (no standby) | yes | **no**, until the primary says SYNCED | AS-BUILT yes; [PROPOSED] **no** (section 9) | n/a |
| Loss on covered single failure | none | none | none, because no promotion happens; the cost is availability | every write not yet on the standby (`test_async_failover`: 500/500) | n/a |
| Resend after timeout | legal: sequenced resend is deduped (section 7) | legal | legal | legal on the same primary; **after an async failover the session is as old as the standby, so a resend of a lost write applies again: correct, but an in-doubt answer was given as OK** | legal |
| Obligations created | durable at the ack (outbox in the same WAL group) | HA_DURABLE at the ack | LOCAL_DURABLE | LOCAL_DURABLE | LOCAL_DURABLE |

Two more classes, which the work order lists beside the modes:

| Class | Ack point | Survives | Retry | Obligations |
| --- | --- | --- | --- | --- |
| **APPLIED** (`Durability::APPLIED` on a mutation; any mode) | at APPLIED, before the WAL (`shard.cpp:420`) | nothing: any crash loses it; a failover loses it unless the record was shipped and installed | sequenced: deduplicated at once against the in-memory session (`replay_if_seen`), whose replayed answer waits for the session's LSN; a crash before that LSN is durable loses both, and a resend applies once more (correct: the first was lost) | created, but durable only when the WAL group is |
| **SETTLED** (a cross-shard or cross-group operation) | not acknowledged: there is no signal (O-5) | as the obligations' class | n/a | none left |

Special answers, and what a client may legally do next (AS-BUILT
meaning; the "next action" column is [PROPOSED] as the client contract):

| Answer | Meaning | Applied? | Legal next action |
| --- | --- | --- | --- |
| `OK` | applied, and durable at the mode's ack point | yes | none; for a cross-shard op, read the target to learn SETTLED |
| `OK` + `replayed` | a resend of the session's last sequence; the original was applied and is durable at the ack point (`replay_if_seen` waits for the session record's LSN) | yes, once | as `OK`. No result payload is returned on a replay |
| `E_REPLAYED_OLD` | the sequence is older than the session's last. **Ambiguous**: the older request may have been applied, or refused (`E_OVERLOADED`, `E_MIGRATING`) and its answer lost. The client cannot tell which | unknown → IN-DOUBT | reconcile by reading state; MUST NOT resend under a new sequence blindly |
| `E_FENCED` | the server lost its role while the request waited | unknown → IN-DOUBT (the write may have reached the standby, section 3) | resend the **same** sequence to the new primary (`net::Client` does); its session decides |
| `E_MOVED` | the slot belongs to another group (epoch in the answer) | no | refresh the map; resend. **The shipped `ClusterClient` resends to the new group with that group's next sequence, i.e. as new work**: an earlier send that was applied but unanswered applies again (section 7, D-3) |
| `E_MIGRATING` | the slot is sealed or staged here | no | back off; resend the same sequence to the same group |
| `E_OVERLOADED` | backlog or ring full | no | back off; resend the same sequence |
| `E_STANDBY` | this node is not the primary, or is fenced; refused **before** the duplicate check (`shard.cpp:97-98, 296-307`). INFERRED safe: the client resends the same sequence to the primary, which holds the session | no | fail over; resend the same sequence |
| `E_NOT_OWNER` | sequenced request to a shard that does not own the object; refused **before** the duplicate check (`shard.cpp:131-138, 174-178`). INFERRED safe, as for `E_STANDBY` | no | resend the same sequence to the owner shard |
| `E_STALE` | standby read without a valid lease | no (reads only) | read from the primary, or retry |
| `E_APP` | `validate` refused it; nothing logged, session not advanced | no | application decision; a new request needs a new sequence |
| `E_NO_MEMORY` on the cross-shard path (`fill_resp`, `shard.cpp:1135-1141`) | the op **ran**, but allocating its out-of-line result failed. Only unsequenced requests take this path | **yes**, contrary to what the status suggests (STABILIZATION_REPORT L-8) | treat as IN-DOUBT (H-3) |
| No answer (timeout, reset) | IN-DOUBT | unknown | resend the same sequence (on a reset, `net::Client` fails over and resends; on a timeout with no progress for `request_timeout_ms` it does the same) |

### 6.2 Proposed rules

- H-1 [PROPOSED]. Every `OK` MUST be at least as durable as the class
  the group's configured mode promises, or the node MUST report that it
  is acknowledging at a lower class: in `STATUS`, and (open question)
  in a response flag. Today a group whose standby was lost acknowledges
  LOCAL_DURABLE with nothing reported (STABILIZATION_REPORT D).
- H-2 [PROPOSED]. As built, `net::Client` resolves most in-doubt cases
  itself: on `E_STANDBY`, `E_FENCED`, a reset or a request timeout it
  fails over and resends every unanswered request with its original
  sequence (`client.cpp:271-276, 299-312`). That is correct only while the
  session holds (sync, healthy, not expired). IN-DOUBT MUST be a
  first-class client outcome:
  `E_REPLAYED_OLD`, `E_FENCED` and an expired session (section 10) all
  map to it, and the client library MUST NOT turn an in-doubt into `OK`
  or into a refusal.
- H-3 [PROPOSED]. A status that says "refused" MUST mean "not applied".
  `fill_resp`'s `E_NO_MEMORY` after the op ran violates this and MUST
  become a distinct status or be checked before the op runs.
- H-4 [ACCEPTED: work order Part III]. Acknowledgement and settlement are
  reported separately; `OK` never means SETTLED.

## 7. Retry and deduplication (work order Part V)

As built (`shard.cpp:628-690`, `RETRIES.md`, EL1-EL3: `test_net`,
`test_replication` 30K resends, `test_resend_refusals`, `test_cluster`):

- **Scope.** A session is keyed by (`client_id`, owner shard). Its record
  is an ordinary object (`System.Session`, 48 bytes), so it is in the
  WAL, replicated, checkpointed and recovered with the data it protects.
- **Atomicity.** The session record is in the same WAL group as the
  mutation (peer, session, target closes the group), so "applied" and
  "recorded as applied" are durable together. Failed ops log nothing and
  do not advance the session.
- **Order.** `replay_if_seen` runs before every refusal that invites a
  resend [INV 18], and before `E_MOVED` for creates.
- **Answers.** `seq > last`: run, including after a gap (**AS-BUILT —
  CONTRACT UNDECIDED**: gaps are accepted silently; reads leave gaps by
  design). `seq == last`: recorded answer (always
  `OK` with the ID and version, no result payload). `seq < last`:
  `E_REPLAYED_OLD`.
- **Not deduplicated:** reads (they run again, harmlessly, and leave gaps
  in `last_seq`); deletes (not sequenced; a resend can answer
  `E_NOT_FOUND`); MUTATE ops flagged `unlogged`; all unsequenced
  requests, including cross-shard forwarded ones.
- **Horizon.** The session lasts until TTL expiry (section 10). Expiry
  runs only in `Engine::checkpoint`. `snc_server` checkpoints periodically
  only with `--checkpoint-s` (default never); recovery also checkpoints
  before the barrier after a shard-count change, orphaned streams or a
  fresh directory (`engine.cpp:525-529`), which expires sessions then.
  Otherwise, on a default server, sessions never expire and grow without
  bound.
- **Migration.** Sessions are system objects (`type_id < 256`) and do not
  move with a slot. A resend to the source group after a move is
  deduplicated there before `E_MOVED` [INV 18]; a resend that goes
  to the new owner is not. INFERRED, **UNPROVEN**: `ClusterClient` keeps
  one `client_id` for every group but a separate `Client`, with its own
  per-shard sequence from 1, per group (`cluster_client.cpp:84-88`), so it
  resends after `E_MOVED` as new work there and an applied-but-unanswered
  request applies twice. A hand-built resend of the *old* sequence is
  worse: it is compared with the destination's unrelated session for the
  same `client_id`, and could get a replayed `OK` for a different op.
- **Shard-count change** routes session records by their own ID's slot,
  not by the client's shard; the client refuses a node with another
  shard count (`client.cpp:148-152`), which is the only guard, and it is
  undocumented (contradiction L).

[PROPOSED] contract:

- D-1. A sequenced mutation MUST be applied at most once for as long as
  its session is ACTIVE (section 10), across crash, restart, failover
  (sync, healthy), checkpoint and recovery. Evidence today: EL3-EL4.
- D-2. A server MUST NOT run a sequenced mutation whose session it
  cannot prove ACTIVE. Today it runs it (session absent = new). This is
  the R11 defect; section 10 is the fix.
- D-3. Resend after a slot move: the request MUST be either deduplicated
  or refused as IN-DOUBT. **UNDECIDED** between moving sessions with the
  slots they touched, or refusing sequenced requests whose session the
  destination does not hold.
- D-4. Every MUTATE that can be sequenced MUST be deduplicated, or the
  op registration MUST forbid sequencing it (today `unlogged` ops are
  silently not deduplicated).
- D-5. Client identity: `client_id` MUST be server-issued once sessions
  have incarnations (section 10). Until then, a reused or colliding
  `client_id` is out of contract (FAILURE_MODEL).

## 8. Promotion eligibility (work order Part XV)

As built, the witness promotes a standby when the primary has been
silent for `failover_ms` and the group's HA state is not `degraded`, and
the standby answers `STATUS` with role `s` and the group's space and
term (`tools/snc_witness.cpp`). The witness has no LSNs, no mode, no
placement, no migration state. `Engine::promote` has no lag check.
Admin `PROMOTE` has no term guard.

[PROPOSED] A replica is **PROMOTION_ELIGIBLE** only if every row holds:

| # | Condition | Holds today? |
| --- | --- | --- |
| E1 | The group's mode permits automatic promotion (sync; not async, section 9) | **no**: the witness does not know the mode |
| E2 | The HA state is not degraded (no DEGRADE grant on record) | yes (EL2-EL4: `test_witness.py`, Vultr P3) |
| E3 | The candidate's term equals the decided term, and the promotion is chosen by a witness majority for term+1 | witness path only (EL2-EL4); admin `PROMOTE` has no term guard |
| E4 | The candidate's space UUID equals the group's | yes (EL0, `snc_witness.cpp` STATUS check) |
| E5 | The candidate's durable LSN, per shard, is at least the primary's last acknowledged LSN. In sync healthy mode this follows from E1+E2, because the primary acknowledges nothing the standby has not acked; the witness compares no LSNs | by construction for sync (EL2-EL4); never checked explicitly |
| E6 | The candidate has finished catch-up (it was in sync, not seeding or catching up from disk) | INFERRED (EL0) yes. Initial seed: the seed fetch runs inside `Engine::recover` (`engine.cpp:327-337`) and the admin thread that answers `STATUS` starts only after `start` returns (`snc_server.cpp:355, 455`), so a seeding node never answers role=s and cannot be promoted. Catch-up of a connected standby: the primary is not in sync, so it degraded or blocked. Not tested |
| E7 | The candidate is not diverged (no records from a term it did not follow) | yes: the term handshake refuses an older term's stream |
| E8 | The candidate passed its own recovery barrier (section 12) | INFERRED (EL0) yes, implicitly: the admin port (`HEALTH`, `STATUS`) opens only after `Engine::start` returns (`snc_server.cpp`, kept by the admin loop) |
| E9 | Placement: the candidate's group still owns the slots it will serve; a move in progress is resumable by the new primary | **UNPROVEN**: promotion ignores placement; `test_migrate` covers driver and node crashes, not a failover during a move |
| E10 | The decision has a witness quorum | yes (Paxos; EL2 `test_witness.py` with one witness down) |
| E11 | The candidate obtained a fresh mint lease block (with the control plane) before serving | with `--control` (`lease_blocks` set): yes (EL0 `Engine::promote` step 0; EL2 `test_control.py`). Without it: leases above `max(next_lease, max_seen + 1)` (`engine.cpp:589`) |

- PE-1 [PROPOSED, from work order Part XV]. A live replica MUST NOT be
  promoted automatically merely because it is alive and the primary is
  silent. E1 is the missing row.
- PE-2 [PROPOSED]. Operator promotion (`PROMOTE`) MUST carry the term it
  expects and fail on any other, and MUST require an explicit
  acknowledgement of possible loss when E1, E5 or E6 does not hold.
- PE-3 [ACCEPTED, IMPLEMENTED: review of PR #36; HEALTH_DECOUPLING.md
  section 10]. Silence MUST be measured on a health path that does not
  depend on shard threads finishing long work (section 11). As built:
  the witness polls `HEALTH`, which waits on no shard. This holds only
  when **every** witness of the group polls `HEALTH`: one witness on
  `STATUS` (old, or `--health-verb status`) still makes a busy primary
  look dead and proposes the promotion for the whole group (EL3,
  `test_health.py` T10).
- PE-4 [ACCEPTED: review of PR #36]. **Liveness is not promotion
  eligibility.** A `HEALTH` answer establishes only that the process and
  its admin path respond. It establishes none of: every shard healthy,
  useful progress, replication current, HA_DURABLE, safe to promote,
  safe to serve, eligible. `HEALTH` MUST NOT make eligibility weaker than
  before; as built the standby is still checked with `STATUS` before a
  promotion is proposed (EL3, `test_health.py` T7).

## 9. R08: async replication contract (deliverable F)

**Problem (MEASURED, EL2).** `test_async_failover`: an async primary
acknowledged 500 writes, was killed, the witness promoted the async
standby, and all 500 acknowledged writes were missing. The witness cannot
tell async from sync: an async primary never asks DEGRADE, so the HA
state stays "not degraded" and the standby looks eligible.

**[PROPOSED] contract (the work order's default):**

- F-1. An async standby is a **warm replica**: it receives the WAL, it
  can be promoted by an operator, and it is never promoted
  automatically.
- F-2. An async primary's acknowledgement is LOCAL_DURABLE and MUST be
  described as such (section 6).
- F-3. Operator promotion of an async standby MUST require an explicit
  acknowledgement that acknowledged writes may be lost, and SHOULD report
  the standby's last received LSN against the primary's last known
  acknowledged LSN when the primary is reachable.
- F-4. After a forced promotion, every session on the promoted node is
  as old as the standby. Requests resent by clients may apply again
  (correct: they were lost) and earlier `OK`s may be lost. The operator
  acknowledgement covers both.
- F-5 (future, not in v0.2). A bounded-lag mode: the primary refuses or
  degrades when the standby lags by more than N records or T of WAL,
  with N and T in the witness state, so an automatic promotion loses at
  most that bound. **UNDECIDED.**

**Minimal witness information.** The witness HA state today is {version,
uuid, term, degraded, primary}. Two options:

| Option | Change | Compatibility | Cost |
| --- | --- | --- | --- |
| F-a (recommended) | Add `mode` (sync / async) to the group's HA state, set at `snc_ctl init` and changed only by a decided command; the witness refuses to propose a promotion for an async group | HA state format change. The parser reads fields in order and would ignore an appended field (`snc_witness.cpp:90`), so the new state MUST carry a format version that an old witness rejects, or old witnesses must be upgraded first; an old state reads as sync, which is today's behavior | small: one field, one check in `suspect` |
| F-b | An async primary registers as permanently degraded (asks DEGRADE at start) | no format change | overloads "degraded" (which today means "the standby may be behind *temporarily*"), and a restart of the primary before it asks leaves a window; rejected |

**Tests that would prove it** (none written; implementation not
authorized):

1. `test_async_failover` inverted: the witness MUST NOT promote; the
   group is unavailable until an operator acts; 0 acknowledged writes
   lost.
2. Forced promotion without the acknowledgement flag is refused; with it,
   succeeds and reports the LSN gap.
3. Sync control unchanged (`test_witness.py`, P3 kill).
4. Mixed old and new witnesses: an old witness must reject the new
   state's format version (or the rollout must upgrade every witness
   first); test that an old witness refuses to act on a new-format state.

## 10. R11: session incarnation (deliverable G)

**Problem (MEASURED, EL2).** `test_session_ttl`: a request was applied and
answered `OK`; its session expired; a resend of the same sequence found
no session, ran as new work, and applied twice (`test_replication.cpp:
1171-1189`). The realistic case, a resend of a request whose answer was
lost, takes the same path (INFERRED). The cause is structural: "no session
found" is indistinguishable from "session expired", and expiry uses the
wall clock.

**[PROPOSED] contract:**

States: `ABSENT → ACTIVE → EXPIRED | CLOSED`.

- G-1. **OPEN_SESSION.** The client asks the owner shard to open a
  session. The server creates a session object and returns its
  **SessionID**, an ObjectID (never reused [INV 2]), once the creation is
  durable at the group's ack point. The client's later sequenced
  requests carry the SessionID and a sequence, not a self-chosen
  `client_id`.
- G-2. **ACTIVE.** A sequenced mutation is deduplicated against the
  session exactly as today (section 7).
- G-3. **An old session MUST NOT execute.** A request naming a SessionID
  that the shard does not hold as ACTIVE is refused with a new status
  (`E_SESSION_EXPIRED`), never run. The shard needs no tombstones to
  decide this: a SessionID it does not hold, minted under a lease this
  space has issued (lease ≤ the superblock's high-water mark), is
  expired or closed; one above it is invalid. Lease blocks interleave across groups
  (`snc_witness.cpp:207-231`), so a foreign group's SessionID below the
  mark would be classed "expired" rather than "invalid": still refused,
  a misclassification, not a safety hole. INFERRED, **UNPROVEN**: this
  holds across restart, failover and checkpoint because session objects
  and the local lease high-water mark are durable and replicated. Under
  `--control`, leases come from the control plane's lease blocks, so the
  high-water mark MUST be the control plane's (the highest block issued
  to the group), not the promoted node's superblock, which may not cover
  leases its old primary minted.
- G-4. **No clock in safety.** Expiry MAY use any policy (time,
  count, memory pressure). Because G-3 refuses, an early or late expiry
  changes only liveness (the client must reopen), never safety. Clock
  jumps become harmless.
- G-5. **After `E_SESSION_EXPIRED`** every unanswered request of that
  session is IN-DOUBT (section 6). The client opens a new session and
  reconciles by reading state, or by an application idempotency key; the
  library MUST NOT resend in-doubt requests on the new session
  automatically.
- G-6. **CLOSED.** A client MAY close its session; the shard deletes it
  after its last answer is durable.
- G-7. **Scope.** One session per (client, owner shard), as today. The
  client library opens them lazily. Migration (D-3) and shard-count
  change are **UNDECIDED** and must be decided with G: as built, session
  objects never move with their slot (`moves_with_slot` skips
  `type_id < 256`, `shard.cpp:2005`); the simplest safe rule is that a request whose
  session is not held is refused (G-3), which costs availability after a
  move, never safety.

**Compatibility.** The wire header carries `client_id` and `seq` today.
G replaces the first with a 128-bit SessionID: a wire version bump, and
the R12 versioning problem (no version field) must be solved first or
together. Old clients would be refused by a new server, not silently
deduplicated less.

**Tests that would prove it:**

1. `test_session_ttl` inverted: after expiry, the resend gets
   `E_SESSION_EXPIRED` and the balance moves once.
2. Clock jump forward (fake clock): same outcome.
3. Restart and failover between expiry and resend: same outcome.
4. Forged SessionID above the lease high-water mark: invalid, refused.
5. Existing dedupe suites unchanged (`test_net`, `test_replication`,
   `test_resend_refusals`, `test_cluster`).

## 11. Bounded work (work order Part XI)

**Measured (STABILIZATION_REPORT D).** One `stage_slot` task held a
shard thread for 3.7-5.0 s; `STATUS` waits on every shard thread with no
timeout; the witness gave up after 100 ms per poll and promoted after
1 s. The engine's health signal is "every shard thread is idle enough to
answer", which conflates busy with dead.

As-built unbounded units of work on a shard thread (EL0, inventory in
STABILIZATION_REPORT J): `stage_slot` (final), `export_slot`, `seal`,
`slot_report`, `resume_all`, `activate_staged`, `expire_sessions`,
`begin_checkpoint` (ID snapshot), `stats` (counts objects), map rehash,
`rebuild_index` (before serving), AUDIT. Index erase is linear in the
posting list.

[PROPOSED] rules:

- B-1 [ACCEPTED: work order Part XI]. A single unit of work MUST NOT
  monopolize a shard thread past a stated budget. The budget is to be
  measured before it is fixed (Part XI); none is proposed here. The same
  rule applies to the admin thread, the witness's request handling, and
  checkpoint on the main thread (STABILIZATION_REPORT J). Work proportional to
  the slot, the map or a posting list MUST be chunked and resumable, with
  its progress in durable state where it must survive a crash (the
  migration steps already are).
- B-2 [ACCEPTED, IMPLEMENTED: HEALTH_DECOUPLING.md section 10].
  Liveness health (what a witness polls) MUST NOT wait on shard threads
  finishing work. It reads a per-shard progress counter (each loop
  iteration publishes it) and reports a shard as *stalled* (`stall_ms`)
  separately from the process being silent.
- B-3 [ACCEPTED: review of PR #36, option B]. **Shard progress is
  diagnostic state, not authority state**, until the work it measures is
  bounded (B-1). A stall detector reports "the loop has not iterated for
  T" (`HEALTH` `stall_ms`) separately from "STATUS was slow". The
  witness promotes only on silence of the health path. A shard stall
  alone, or a main-loop stall alone, MUST NOT trigger automatic
  promotion, withdrawal or a grant. The trade-off is accepted as a
  temporary availability limitation: a primary whose shard thread is
  wedged forever is alive and useless, and stays primary until an
  operator acts (HEALTH_DECOUPLING.md HF1). Automatic wedge handling MAY
  be reconsidered with an evidence-based threshold once the relevant work
  is bounded and a progress contract exists; a threshold chosen to exceed
  today's measurements is not one. Self-withdrawal on a stall is rejected
  (HEALTH_DECOUPLING.md 3.5, option C).
- B-4. Changing `failover_ms`, poll timeouts or TTLs MUST NOT be used to
  hide B-1..B-3 [ACCEPTED: work order].

## 12. Recovery barriers (deliverable I)

As built (`Engine::start`, `engine.cpp:189-195`; `Shard::finish_recovery`;
EL0 except where named):

| Step | When | Before the barrier? |
| --- | --- | --- |
| Superblock read; space UUID; mint leases (`next_lease += n`, superblock written) | start | yes |
| Checkpoint load, WAL replay per stream (or rerouted on shard-count change) | `recover` | yes |
| After a shard-count change, orphaned streams or a fresh directory: a full checkpoint (expires sessions; waits on `durable(max_lsn)`) | `recover` step 8 (`engine.cpp:525-529`) | yes |
| Per shard: `rebuild_index`, `refresh_slots`, `start_serving` / `resume_all` (follow-ups **queued**, held), WAL opened for append, `start_sender` (**replication shipping starts**), `activate_staged`, `start_links` (**node links connect**) | `finish_recovery` | yes |
| `release_emits` (held follow-ups dispatched), then `serving = true` | `Engine::start` end | **barrier** |
| Space-UUID check against the control plane; placement watcher | after `start` | **after** |

Findings:

- I-a. Follow-ups are held until every shard has recovered (the code
  comment explains the lost-credit race it prevents). This is the
  barrier invariant 16 asks for, for inter-object computation. **Holds**
  (EL2: `test_transfer` restart with transfers in flight; EL3:
  `test_cluster`).
- I-b. **Externally visible I/O starts before the barrier**: replication
  shipping (`start_sender`), node links (`start_links`), and
  `activate_staged` writes. INFERRED: shipping recovered records to a
  standby is safe (they are durable locally). Node links carry only
  follow-up frames (`node_link.h:25-30`); `on_link_frame` queues them and
  `run_emits` returns while paused (`shard.cpp:1316-1346, 1815`); client
  I/O is gated on `serving` (`engine.cpp:76-78`). So no request is
  accepted before the barrier (EL0).
- I-c. The space-UUID check against the control plane runs after the
  node starts serving: a node pointed at the wrong data directory
  serves it briefly. **KNOWN ISSUE** (EL0).
- I-d. A restart as primary does not bump the HA term. INFERRED safe
  (the term changes only by promotion, and the witness still holds the
  same term), recorded because the work order asks.
- I-e. Session TTL is applied at recovery only when step 8 checkpoints
  (shard-count change, orphans, fresh directory); otherwise at a later
  checkpoint.
- I-e2. **A restarted sync primary counts nothing as durable until its
  standby reconnects** (`Sender::acked_` starts at 0, `replication.h:172`;
  `durable()` needs it while `repl_hold_` is set). Until then default
  reads, replays, cross-node follow-ups and checkpoints block. INFERRED
  (EL0): with `--witness`, the standby down, and a shard-count change,
  the step-8 checkpoint waits inside `Engine::start`, before
  `snc_server`'s main loop exists to ask for DEGRADE: the restart can
  hang. INTERACTION_MATRIX row 13 at startup.
- I-f. The index is rebuilt, not validated.
- I-g. Promotion has a follow-up barrier (`pause_emits`, then
  `release_emits` after every shard is primary) but **no client
  barrier**: `run_on_all` promotes shards in parallel and they finish at
  different times, and the network poll is already enabled on a standby,
  so one shard can serve before another is promoted.
  INFERRED safe for single-shard ops (each shard serves only after its
  own promotion), **UNPROVEN** for anything that reads across shards
  (index lookup).
- I-h. In-memory mode (no data directory) has no barrier: it serves at
  `start`.
- I-i. Migration seals at recovery: slot state is rebuilt by
  `refresh_slots` before serving, so a sealed slot stays refused
  (EL2-EL3, `test_resend_refusals` restart while sealed, `test_migrate`
  crash points).
- I-j. Standby catch-up after recovery: the recovered primary starts
  shipping from the standby's acknowledged LSN, or from disk, before the
  barrier (I-b); a standby whose gap was dropped must be reseeded
  (EL2, `test_replication` outage tests).

Invariant 17 (placement epoch adopted node-wide before replay for it):
`update_placement` runs `run_on_all` twice (adopt, then resume), so every
shard has the new epoch before any shard resumes for it. **Holds**
(EL2-EL3, `test_migrate`, PR #24 review).

[PROPOSED] contract:

- I-1. A node MUST NOT accept a client or node request, or dispatch a
  follow-up, before its barrier: WAL replayed, every shard's index and
  slot state rebuilt, its identity (space, group, term, epoch) verified
  against the control plane when it has one.
- I-2. Shipping recovered, locally durable WAL to a standby before the
  barrier would be an exception to invariant 16, which forbids any
  externally visible computation before it. [PROPOSED] Accept it as an
  explicit, documented exception (an invariant amendment, for Codex), or
  move `start_sender` after the barrier. STABILIZATION_REPORT L-14.
- I-3. The space-UUID check MUST move before the barrier (I-c).
- I-4. Promotion MUST have the same barrier as recovery for anything
  that spans shards.

## 13. Durable obligations (work order Part XVII)

As built, every follow-up (the credit of a transfer, a settle reply) is
an entry in the source object's outbox, written in the same WAL group as
the mutation that created it, so the obligation is exactly as durable as
the acknowledgement (EL2-EL4: `test_transfer`, `test_cluster`, Vultr P4: a
26K backlog during a cut settled after it). Delivery is transient
[INV 15]: dropped messages are re-emitted from outboxes.

Obligations are re-emitted only on events: restart, node link up, map
change, unseal, activate. **There is no timer.** The inventory found
several drops, each relying on one of those events to re-emit. Stale,
misrouted and link-down drops are counted (`emits_stale`,
`node_emits_misrouted`, `node_emits_dropped`); two are silent: an
`emit_result` status outside the expected set (`shard.cpp:1990`) and the
withdrawn-shard return (`shard.cpp:1860`). INFERRED: an intra-node follow-up dropped
for an unexpected status can sit until the next restart. `kSettleRetry`
(target inbox full) re-emits immediately with no backoff.

[PROPOSED] contract:

- O-1 [ACCEPTED: work order Part XVII]. An obligation MUST be durable
  atomically with the state transition that creates it, or the
  transition MUST NOT commit. As built this holds: the outbox entry is in
  the same WAL group (EL2-EL4).
- O-2. Every obligation MUST eventually be applied exactly once once its
  target is reachable, without depending on an unrelated event (a
  restart or a link flap). Today: **not guaranteed** (no timer).
- O-3. Every drop of an obligation message MUST be counted and visible.
  Today two are silent, and the counted ones are not in `STATUS`.
- O-4. Retries of an obligation SHOULD back off.
- O-5. SETTLED SHOULD be observable for an operation (today only by
  reading the target).

## 14. Architecture principles carried forward

- **Locality** [INV 4, 5]: cross-shard, cross-node and cross-group costs
  stay explicit in the API (`ExecResult` forwarded flag; transfers
  settle asynchronously across shards). Work order Part XXIX asks for a
  locality placeholder: affinity-aware placement is **UNDECIDED** and
  out of v0.2.
- **Transaction boundary** (Part XXX): the atomic unit is one WAL group
  on one shard. Multi-object atomicity across shards or groups is not
  offered; it is built from obligations (section 13). Anything wider is
  **UNDECIDED** and out of v0.2.
- **Clocks** [ACCEPTED: work order Part VII for sessions; PROPOSED in
  general]: a safety property MUST NOT depend on
  synchronized clocks. Gaps today, to remove or bound: session TTL (section 10, G-4),
  the standby read lease (section 5, R-4). Paxos ballots read the wall
  clock (`tools/paxos.h:394-402`; the proposer's counter is not
  persisted), but INFERRED (EL0) safe: acceptors persist their promise
  before answering and a PREPARE needs a ballot strictly above it, so a
  reused ballot cannot gather a second majority in the same instance. A
  clock jump costs liveness only.
- **Security** (Part XXVII): R05 and R13 are missing. Every client,
  admin, witness, node-link and replication port is unauthenticated: a
  **prototype limitation**, and the current isolation is **not production
  security**. Room to preserve, **UNDECIDED**: principal identity, tenant
  identity, capability-based authorization, project/security domains,
  secret brokering, executable policy, module containment,
  audit/provenance. G's server-issued SessionID is the natural carrier
  of a principal.
- **Multi-tenant resource isolation** (Part XXVIII): not implemented.
  Future accounting is hierarchical: cluster → tenant → project →
  team/role → entity/agent → execution, over CPU, memory, storage, WAL
  bandwidth, network, activation rate, runnable entities, cross-shard
  and cross-node work. [PROPOSED] Admission and backpressure (section
  11, STABILIZATION_REPORT J) MUST be expressed as per-principal
  budgets that can nest, never as a single global counter that cannot
  be split later (today `kEmitBacklog` is per shard and global to all
  clients).
- **Rust and other implementations** [ACCEPTED: Codex, PR #33 review]:
  no rewrite now. Before any second implementation, every durable format
  and wire message MUST be specified in bytes (section 1, Upgrade).
- **Platform boundary** [INV 1, 6, 10]: Linux, io_uring, the native
  runtime and the networking OS remain replaceable mechanisms behind
  SNC's own abstractions.
