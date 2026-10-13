<!-- Written by Codex (ChatGPT) and committed by Claude as relayed by cody on 2026-10-13. Claude's response is in the AGENTS.md handoff log. -->

# SNC Architecture Audit

**Audit baseline:** `a835aef` on `main`\
**Date:** 2026-10-13\
**Scope:** Architecture reconciliation before multi-node milestone 2\
**Status:** Recommended design direction

## Executive Decision

The current SNC architecture is fundamentally sound. Keep the object
identity, slot indirection, shard-per-core execution model, native
runtime, persistence/recovery model, replication machinery, and
state-driven cross-node follow-up mechanism.

The main decision before cluster milestone 2 is:

> **KEEP the witness consensus mechanism as HA authority for replication
> groups. RECONSIDER using the current pair-specific witness state
> machine as the general cluster directory. Introduce an explicit
> cluster placement model mapping stable slots to replication groups,
> with placement epoch independent of HA term.**

The existing witness/Paxos implementation answers **which replica may
act as primary**. The cluster directory answers **which replication
group owns a slot**. These authorities are related but logically
distinct. They may eventually share a consensus substrate without
becoming one state machine.

## 1. Classification

-   **KEEP** --- aligned with SNC's long-term model and supported by
    current evidence.
-   **REFINE** --- directionally correct; implementation/interface
    should evolve.
-   **RECONSIDER** --- risks hardening the wrong abstraction boundary.

## 2. Summary Matrix

  -------------------------------------------------------------------------
  Area                      Verdict                 Rationale
  ------------------------- ----------------------- -----------------------
  128-bit ObjectID          **KEEP**                Stable identity
                                                    independent of physical
                                                    location

  4,096-slot indirection    **KEEP**                Strong
                                                    virtual-placement
                                                    boundary

  Shard-per-core ownership  **KEEP**                Core locality
                                                    mechanism; benchmark
                                                    evidence supports it

  Native runtime + SPSC     **KEEP**                Measured against
  rings                                             Seastar and justified

  Seastar backend           **KEEP as comparison**  Preserves runtime
                                                    abstraction/reference

  Per-shard local map       **KEEP**                Correct
                                                    ownership/locality
                                                    model

  WAL/checkpoint/recovery   **KEEP**                Coherent persistence
                                                    contract with crash
                                                    testing

  Recovery/promotion        **KEEP / formalize**    Necessary lifecycle
  barrier                                           invariant

  `create_near()` semantics **KEEP**                Placement affinity is
                                                    central to multi-object
                                                    performance

  `create_near()`           **REFINE later**        \~4096 attempts
  mint-by-hash                                      expected per target
                                                    slot

  Fixed cluster slot map    **KEEP as milestone**   Correct bootstrap
                                                    model, not final
                                                    directory

  Owner-aware ClusterClient **KEEP**                Routes computation
                                                    toward state

  TCP node links            **REFINE**              Good prototype;
                                                    transport should remain
                                                    replaceable

  Durable/idempotent        **KEEP concept**        Persistent state, not
  follow-ups                                        network delivery, is
                                                    source of truth

  Full-object resume scan   **REFINE**              Needs pending-work
                                                    index at scale

  Cross-node transfers      **KEEP semantics**      Valid distributed
                                                    state-native execution
                                                    proof

  Primary/standby           **KEEP**                Good durability/failure
  replication                                       primitive

  Term fencing              **KEEP**                Required safety
                                                    mechanism

  Paxos witnesses           **KEEP for HA           Correct
                            authority**             promotion/degrade
                                                    arbitration

  Witness state as cluster  **RECONSIDER**          Conflates HA leadership
  directory                                         with
                                                    placement/membership

  Static membership         **REFINE later**        Fine for milestone;
                                                    eventually versioned

  Node-local secondary      **REFINE later**        Global indexing needs
  index                                             separate design
  -------------------------------------------------------------------------

## 3. Object Identity --- KEEP

Current ObjectID is 128 bits: format + mint lease + sequence. Placement
is derived from `snc_hash64(ObjectID)`.

It does **not** encode node, shard, CPU, NUMA node, memory address, or
disk location. That is the correct invariant.

**Decision:** keep the format unless strong evidence requires revision.
Do not encode placement into ObjectID for easier routing.

## 4. Slot Indirection --- KEEP

Current placement begins with 4,096 stable virtual slots:

``` text
ObjectID → XXH3-64 → Slot 0..4095 → Placement → Node → Shard
```

This is a strong architectural choice. It allows many objects to move
logically through metadata changes rather than identity changes.

**Decision:** keep the 4,096-slot abstraction. Revisit the exact count
only if future scale measurements justify it.

## 5. Shard-Per-Core Execution --- KEEP

``` text
Physical Core N → Pinned Worker N → Shard N → Owned Objects
```

Only the owner directly mutates an object. Benchmarks show owner-local
operations are strong and cross-core operations impose measurable cost.
Placement is therefore architectural.

**Decision:** keep. Future networking and placement should reinforce
this model.

## 6. Runtime --- KEEP

The native runtime has been measured against Seastar. Native wins tested
in-memory workloads; Seastar's inter-core path is costly; SNC rings
recover much of that gap.

**Decision:** keep native as default, retain the runtime abstraction and
Seastar backend as comparison/reference.

## 7. Memory and NUMA --- KEEP / MEASURE

Huge-page results correctly show that huge pages are not universally
advantageous.

**Decision:** keep NUMA/locality first-class. Continue measuring
worker + object + map + hot index + arena colocation rather than making
page-size mechanisms doctrine.

## 8. Persistence and Recovery --- KEEP

Current model:

``` text
operation → object after-image → per-shard WAL
→ group commit/io_uring → checkpoint → recovery
```

PR #16 exposed an important lifecycle bug: one recovered shard could
resume follow-ups while another shard's WAL was not ready, allowing a
follow-up to be treated as durable without being logged.

The fix establishes a stronger invariant:

> **No externally visible or inter-object computation may resume until
> all shards participating in the recovered execution domain have
> reached recovery readiness.**

``` text
shards recover/promote
        ↓
RECOVERY BARRIER
        ↓
enable emissions
        ↓
enable network
        ↓
READY
```

**Decision:** keep persistence architecture and formalize this lifecycle
invariant.

## 9. `create_near()` --- KEEP SEMANTICS / REFINE LATER

Placement benchmarks show that objects modified together benefit
strongly from colocation. `create_near()` preserves that affinity across
shard-map changes by targeting the same slot.

Current implementation mints IDs until the hash lands in the target slot
(\~4096 attempts expected).

**Decision:** keep the semantics. Do not change ObjectID format yet.
Optimize minting later only if identity/location separation remains
intact.

## 10. Multi-Node Milestone 1 --- KEEP

The current fixed-map cluster is an appropriate bootstrap model:

``` text
4096 slots
   ├── Node A → shards
   ├── Node B → shards
   └── Node C → shards
```

Creates stay local; foreign operations return `E_WRONG_NODE`; clients
route using the map; cross-node follow-ups target owning shards; map CRC
rejects incompatible peers; crash/restart tests exercise distributed
transfers.

**Decision:** keep as milestone implementation, not final control plane.

## 11. Cross-Node Follow-Ups --- KEEP CONCEPT / REFINE SCALE

Current node links intentionally do not provide a durable message queue:

``` text
durable source state → send follow-up → may be lost
→ reconnect/restart → resume durable protocol state → re-emit idempotently
```

This is strongly aligned with SNC:

> **Persistent object state is the source of truth; network delivery is
> transient.**

The scaling problem is that resume may walk every object. Future
direction should use a persistent pending-work index so only objects
with unfinished protocol state are revisited.

**Decision:** keep state-driven/idempotent resume. Refine resume lookup
later. Do not add a heavyweight durable messaging subsystem without
evidence.

## 12. TCP Node Links --- REFINE

Direct shard-to-shard TCP links are appropriate for correctness testing
but should not become SNC semantics.

Future conceptual boundary:

``` text
send_followup(destination, frame)
poll_transport()
peer_state()
```

Possible implementations: TCP, DPDK, the separate networking OS, RDMA,
or others.

**Decision:** introduce the transport abstraction when a second
transport actually needs it; do not rewrite working TCP solely for
abstraction purity.

## 13. Replication --- KEEP

Replication streams WAL records to a standby, which installs
authoritative object images rather than re-executing application
handlers.

**Decision:** keep. Replication should become a property of a
replication group rather than global placement.

## 14. Term Fencing --- KEEP

Terms protect against an old primary continuing to acknowledge durable
writes after replacement.

**Decision:** keep. Terms belong to HA/replication-group authority and
must not double as cluster placement versions.

## 15. Paxos Witnesses --- KEEP FOR HA AUTHORITY

Witness state is fundamentally:

``` text
Space UUID
Term
Primary node
Degraded grant
Version
```

The witness cluster arbitrates promotion, degraded-primary permission,
and exclusion between those decisions.

**Decision:** keep the Paxos witness mechanism for HA decisions.

## 16. Witnesses as General Cluster Directory --- RECONSIDER

The proposed milestone 2 direction risks conflating two state machines.

### HA question

``` text
Which replica is allowed to serve as primary?
```

State includes replication group, term, leader, degrade grant, replica
health.

### Placement question

``` text
Which replication group owns slot X?
```

State includes cluster membership, placement epoch, slot assignments,
migration state, addresses/capabilities.

These change for different reasons and at different rates.

**Decision:** do not directly extend the current pair-specific witness
state into the cluster directory. The same consensus technology may
later underpin both, but keep their logical state separate.

## 17. Introduce Replication Groups

Long-term mapping should evolve from:

``` text
Slot → Node
```

to:

``` text
Slot → Replication Group
```

Example:

``` text
RG17
  Members: Node B, Node F
  Primary: Node B
  Standby: Node F
  HA Term: 42

Slots 500–749 → RG17
```

If Node B fails, Node F can become primary while the slot placement
remains unchanged.

> **Failover is not migration.**

## 18. Separate Placement Epoch from HA Term

Use independent version domains.

**Placement Epoch** changes when slot ownership/membership/migration
changes.

**HA Term** changes when leadership changes within a replication group.

Required invariant:

> A failover inside a replication group must not require a
> placement-epoch change unless placement itself changed.

## 19. Proposed Control-Plane Model

Define before implementing dynamic directory behavior:

``` text
Cluster
 ├── ClusterID
 ├── PlacementEpoch
 ├── Nodes[]
 │    ├── NodeID
 │    ├── addresses
 │    └── capabilities
 ├── ReplicaGroups[]
 │    ├── GroupID
 │    ├── members[]
 │    └── HA authority/state reference
 └── Slots[4096]
      └── ReplicaGroupID
```

Routing hierarchy:

``` text
ObjectID
  ↓
Slot
  ↓
Placement Directory
  ↓
Replication Group
  ↓
HA Term / Current Leader
  ↓
Node
  ↓
Shard
  ↓
Core
  ↓
Object
```

## 20. Data Plane vs Control Plane

### Data plane

Hot path:

``` text
ObjectID → cached placement → owner node/shard → execute
```

Includes client routing, shard dispatch, object execution, node
follow-ups, replication data streams, and future DPDK/networking-OS
transport.

### Control plane

Cold path: membership, placement, migration, HA authority,
configuration.

**Required principle:** the hot path must not require a consensus round.
Nodes/clients normally operate from cached, versioned placement state.

## 21. Future Migration

Slot indirection enables:

``` text
Slot 782: RG3 → RG8
```

A future migration protocol should have explicit states such as
`STABLE`, `PREPARING`, `COPYING`, and `CUTOVER`.

Do not implement dynamic slot movement until replication-group identity,
placement epochs, and stale-routing behavior are specified.

## 22. Stale Routing

`E_WRONG_NODE` is correct for the fixed-map milestone. With dynamic
placement, responses should eventually expose enough version/owner
information for clients to refresh efficiently.

Potential conceptual response:

``` text
E_MOVED
placement_epoch = 19
owner_group = RG8
```

Exact wire semantics are future work.

## 23. Secondary Indexes

Current indexes are shard/node-local. That is acceptable now.

Global/distributed indexing should be designed separately because its
consistency and placement semantics differ from ObjectID routing.

**Decision:** do not make global indexing a prerequisite for the
control-plane milestone.

## 24. Networking-OS / DPDK Boundary

Long-term fast path remains:

``` text
NIC → RX queue → physical core → SNC worker
→ owning shard → NUMA-local object → operation → TX
```

The distributed control plane should resolve the upper routing
hierarchy; the future networking layer should optimize delivery to the
resolved node/shard/core.

The separate networking OS remains modular:

``` text
SNC
  ↓
transport/locality interface
  ↓
networking OS
  ↓
F5 | x86/DPDK | ARM/DPU
```

Do not couple cluster semantics to a specific packet-processing backend.

## 25. Recommended Next Step Before Milestone 2

Do **not** immediately implement "directory in witnesses."

First write a small design/specification for:

1.  `NodeID`
2.  `ReplicationGroupID`
3.  cluster metadata schema
4.  `Slot → ReplicationGroupID`
5.  placement epoch semantics
6.  per-group HA term semantics
7.  client/node map caching
8.  stale-map response behavior
9.  bootstrap/persistence of cluster metadata
10. relationship between placement consensus and existing witness
    consensus

Then decide whether:

-   existing witness processes host both logically separate state
    machines;
-   a new cluster-control service owns placement while witnesses remain
    HA arbiters; or
-   both use a shared generalized consensus layer with separate
    replicated state machines.

The architecture should decide this before code hardens the boundary.

## 26. Immediate KEEP / REFINE / RECONSIDER Actions

### KEEP

-   ObjectID format and location independence
-   4,096 virtual slots
-   shard-per-core ownership
-   native runtime and SPSC rings
-   per-shard maps
-   WAL/checkpoint/recovery
-   recovery barrier
-   placement-affinity semantics
-   state-driven idempotent follow-ups
-   replication
-   term fencing
-   Paxos witnesses for HA

### REFINE

-   `create_near()` minting efficiency
-   pending-work/resume indexing
-   transport abstraction when a second transport arrives
-   static membership/map into a versioned placement model
-   distributed/global index strategy later

### RECONSIDER BEFORE IMPLEMENTATION

-   using the current pair-specific witness state machine directly as
    the cluster directory
-   treating failover and placement change as the same event/version
-   binding the distributed object model permanently to the current
    two-node HA shape

## 27. Recommended Architectural Invariants to Add to AGENTS.md

1.  **Failover is not migration.**
2.  **Placement epoch and HA term are independent version domains.**
3.  **Slots map to replication groups; HA selects the active replica
    within a group.**
4.  **The hot execution path consumes cached control-plane state and
    does not perform consensus.**
5.  **Persistent object/protocol state is authoritative; transport
    delivery is recoverable and transient.**
6.  **No inter-object or externally visible computation resumes before
    the execution domain crosses its recovery barrier.**

## 28. Handoff to the Next Agent

Before cluster milestone 2, implement **design**, not directory code.

The next artifact should specify the replication-group and
placement-control model. Preserve the working milestone-1 data plane
while doing so.

The most important architectural constraint is:

> **Do not let a convenient existing Paxos implementation determine
> SNC's long-term control-plane boundaries. Use consensus where needed,
> but model HA leadership and cluster placement as separate concerns.**
