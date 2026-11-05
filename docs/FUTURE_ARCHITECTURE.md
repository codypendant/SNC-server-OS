# Future architecture: state lineage, platform and agent runtime

**Status: VISION, NOT AUTHORIZED.** The source is cody's and Codex's
"State Lineage, Enterprise Platform & Agent Runtime Vision" v2, filed
verbatim as
[`handoff/SNC_Future_Architecture_Vision_v2.docx`](handoff/SNC_Future_Architecture_Vision_v2.docx)
(text copy: [`.txt`](handoff/SNC_Future_Architecture_Vision_v2.txt)).
It authorizes no implementation: not backup or PITR, not snapshots, COW
or branches, not the Agent Runtime, Durable Events or Timers, security,
tenancy, schema migration, Rust, DPDK/VPP, or S2/S3. The falsification
roadmap ([EXPERIMENTAL_VALIDATION.md](EXPERIMENTAL_VALIDATION.md)) is
unchanged: GO, REFINE, NARROW or STOP comes first. Agent-SNC is a
separate hypothesis with its own falsification gate.

This page does two things. Section 1 states the vision in a few lines.
Section 2, the part that matters now, checks the current core against
the vision's "must not architect ourselves out of this" list. Section 3
adds ideas from Claude's review. Every claim about today's code is
marked VERIFIED (read in the source for this page) or INFERRED.

## 1. The vision in brief

- **SNC manages durable computational state** across location (core to
  site), time (current, history, snapshot, PITR, clone), ownership
  (placement, term, fencing, migration) and execution (worker, service,
  agent, model, tool).
- **The persistent state is the agent;** models are execution resources.
  Agent memory is a graph of durable objects with provenance (current
  versus historical, private versus project), with executable policy,
  dynamic skills, and reproducible activations against corpus snapshots.
- **Time and lineage:** backup distinct from replication; cheap snapshot
  roots; pinning and retention; copy-on-write clones; branches as a
  lineage DAG; content-addressed immutable segments; send/receive;
  checksums and scrub; restore that resets HA authority.
- **The addendum's central idea:** give an agent (or a migration, or an
  upgrade) a branch of production state, not production authority.
  Capabilities are scoped by lineage. The agent returns a structured
  proposal. Promotion is a controlled transaction, revalidated against
  current production, and leaves a promotion receipt. External side
  effects need their own sandbox policy.
- **Layering:** the core exposes identity, authority, durability,
  versioning, snapshot-compatible roots, hooks for namespace, security
  and resource accounting, integrity metadata, bounded work and
  recovery. Backup, branches, KMS, tenancy and audit are platform. Agent
  Runtime, MMO and TM87 workflows sit above.

## 2. Today's core against "must not architect ourselves out of this"

| Requirement | Today | Verdict |
| --- | --- | --- |
| Distinct logical time domains | WAL LSN per shard, HA term, placement epoch, mint lease and object version are separate fields with separate checks (VERIFIED: `wire.h`, `snc_witness.cpp`, `storage.h`). Two places still use wall clock: session expiry (R11) and standby read leases (matrix row 17) | **Holds**, with those two named exceptions |
| Format versioning | ObjectID carries an 8-bit format field (`object_id.h`, VERIFIED); manifest types carry `schema_ver` (VERIFIED); placement has v1 and v2 with a tested upgrade path (LEGACY_ROLE L9); the wire rejects unknown kinds with E_BAD_ARGS (X1 compat test) | **Holds**: the precedents exist |
| Schema evolution | `schema_ver` is recorded per type and per WAL record (VERIFIED: `append(..., schema_ver, ...)`); nothing reads two versions of one type | **Open, not blocked** |
| Snapshot-compatible roots | Checkpoints are immutable directories with a manifest (`seq`, per-stream `start_lsn`); seeds pin a checkpoint and the WAL from its start (`ckpt_pins`, `wal_pin`, VERIFIED in `seed.cpp`). That is already a primitive snapshot pin | **Holds** as a starting point. A checkpoint is a full copy, not a cheap root |
| A consistent cut across shards | Each shard has its own LSN space; the manifest is a vector of per-shard LSNs. Whether that vector is a consistent cut while cross-shard obligations are in flight is not established (INFERRED: not by construction) | **The real risk.** A snapshot, a backup and a branch all need a cut that respects in-flight obligations. Design it before any format freezes |
| PITR | The WAL logs the full post-operation image (spec v0.1), so replaying to an LSN is natural per shard. Retention deletes segments behind checkpoints; there is no archive | **Possible**, gated by the cut above and by WAL archival |
| Checksums and scrub | WAL records carry crc32c (VERIFIED: `last_crc`). A bad record in the middle of the last segment is still dropped silently (FAILURE_MODEL S6, open) | **Partly.** S6 must be fixed before scrub means anything |
| Derived versus authoritative state | Indexes are rebuilt at recovery (`rebuild_index`); slot state is rebuilt by `refresh_slots` (CORE_CONTRACTS I-i) | **Holds**: the classification exists implicitly; write it down |
| Restore resets authority | Rejoin moves a stale directory aside and seeds in the new term; legacy directories cannot start as a primary without adoption (LEGACY_ROLE); B2 now records NOT READY at every primary start. A directory restored under the same identity as a standby is the stated B2 residual (SYNC_READINESS 4) | **Mostly holds.** A "recovery incarnation" (the residual's fix) is exactly what restore needs |
| Namespaces, tenants, branch identity | ObjectID is format + mint lease + sequence: no tenant or lineage bits. Routing is by slot of the ID | **Open.** Branch identity cannot live in the ID without a format change; it must live in execution context, routing and capability (as the vision says). Decide that early: every request path would carry it |
| Resource accounting and bounded work | S1 bounded the final stage; B3 bounded catch-up read-ahead; `RESOURCES` exposes gauges; retention is capped per shard (F1) | **Holds** in spirit; there is no per-tenant accounting |
| Provenance | Each WAL record has an object, version and LSN; nothing records which request, session or principal caused it | **Open.** Cheap to add early (a causal ID per record), expensive to retrofit |

**Summary:** nothing in the core today rules the vision out. Two things
would become expensive if left: a defined cross-shard consistent cut
(snapshot, backup, branch, PITR all rest on it) and a causal identifier
on WAL records (provenance, audit, replay-based promotion all rest on
it). Both are design questions, not features; neither is authorized now.

## 3. Ideas from the review

These extend the vision; all are post-validation.

1. **The checkpoint as the first snapshot primitive.** Seeding already
   pins a checkpoint and its WAL tail. A named, pinned checkpoint with a
   retention owner is most of "snapshot ID plus pinning" without a new
   storage format. Cheap roots (COW, content addressing) can replace the
   full copy later behind the same name.
2. **Obligations make the cut tractable.** Cross-shard work in SNC is
   already explicit (obligations, follow-ups, settlement). A consistent
   cut can be defined as "every shard at an LSN, plus the obligations in
   flight between them", captured from the obligation records rather
   than by stopping the world. That is the same shape as a
   Chandy-Lamport snapshot, with the channels already durable.
3. **A branch is a new space UUID with a parent.** Today a space UUID is
   the identity every replication handshake, seed and witness checks. A
   branch as a child space (new UUID, recorded parent snapshot) reuses
   all of that fencing for free: a branch's primary cannot replicate
   into, seed from, or be promoted over its parent by accident. Lineage
   isolation becomes the existing space check, not a new mechanism.
4. **Promotion as replay of a session.** A branch's accepted effects can
   be expressed as the requests of one session, replayed against current
   production with version preconditions. Sessions already give
   exactly-once application (inside the R11 horizon), so promotion
   inherits deduplication; a stale precondition fails the replay rather
   than forcing a merge.
5. **Readiness as evidence.** B2 just made "proven in sync" a recorded
   fact rather than an assumption. The same pattern (a state is
   promotable only after positive, recorded proof from the current
   authority) is what a promotion receipt is. Building receipts on the
   witness Paxos machine gives them a quorum and a version.
6. **Restore drills as a scheduled test, not a document.** The test
   harness already restarts, seeds and verifies exact balances
   (`net_bench --verify`). A restore drill is a seed from a backup plus
   that verification, with the RTO measured. It could run as a
   recurring lab job once backups exist.
7. **Side effects through the outbox.** Durable outbox, WAL and resume
   are already the foundation for Durable Events (EVIDENCE R14). An
   external-effect policy (deny, redirect to a mock, gate on approval)
   is a property of the outbox's delivery stage, so sandboxing external
   effects lands in one place.

## 4. What changes now

Nothing in the current work order. The two design questions of section
2 (the consistent cut and a causal record identifier) are suggested as
items for the GO/REFINE/NARROW/STOP decision review, so that the next
format freeze does not close them by accident.
