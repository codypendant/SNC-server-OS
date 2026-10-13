# AGENTS.md --- SNC Agent Coordination

## Purpose

Shared coordination contract for AI agents working on **SNC
(State-Native Compute)**. Read this file, `README.md`, and
`docs/STATUS.md` before architectural changes. Detailed evidence belongs
in source, tests, benchmarks, subsystem docs, commits, and PRs.

## Project Thesis

SNC tests whether persistent application objects can be directly
executable state: stable ObjectIDs, per-core ownership, computation
dispatched toward authoritative state, and integrated
persistence/recovery. Performance advantages are hypotheses until
measured.

## Architecture Invariants

1.  **Linux is the machine substrate.** Do not recreate Linux facilities
    without measured cause.
2.  **Object identity is independent of location.** ObjectIDs must not
    encode physical addresses, CPUs, NUMA nodes, disk locations, PCI
    addresses, or servers.
3.  **One authoritative owner exists per object at an instant.** Direct
    mutation occurs on the owning shard.
4.  **Locality is fundamental.** Prefer ingress → worker → shard →
    NUMA-local state alignment.
5.  **Cross-locality costs remain explicit.** Do not hide cross-shard,
    cross-NUMA, or future cross-node costs.
6.  **Borrow mechanisms; preserve SNC abstractions.** Seastar, DPDK,
    VPP, io_uring, allocators, and storage libraries are replaceable
    implementation choices.
7.  **Native runtime is currently the default.** Keep Seastar as a
    measured comparison; see `docs/SEASTAR.md`.
8.  **Correctness before benchmark wins.** Durability, recovery,
    fencing, replication safety, and failure behavior require tests.
9.  **Benchmark competent alternatives.** Do not construct intentionally
    weak baselines.
10. **The networking OS remains separate.** Future integration belongs
    behind a clean transport/locality boundary rather than coupling the
    object core to F5, x86, ARM, DPDK, or VPP.

## Current Implementation State

SNC has moved beyond the original v0.1 checklist. `docs/STATUS.md` is
authoritative.

Implemented areas include: - stable 128-bit ObjectIDs and relocatable
objects - shard-per-core ownership and pinned native workers - local and
cross-shard execution - shard-local indexing - WAL, group commit,
checkpoints, crash recovery, and shard-count remap recovery - locality,
huge-page, cross-core, skew, transfer, and conventional-stack
benchmarks - owner-aware routed TCP front end - replication, seeding,
promotion, fencing, standby reads, retry/session behavior - witness
failover and replicated Paxos witnesses - repeated failover and
automatic rejoin of a failed original primary

Do not infer that the general distributed-object problem is solved. Read
`docs/REPLICATION.md` for current limits.

## Networking Direction

The long-term networking model is influenced by TMOS/F5
ownership-and-locality principles:

``` text
NIC → RX queue → physical core → SNC worker
    → owning object shard → NUMA-local object
    → operation → TX
```

RSS, DPDK, and VPP are candidates, not mandatory architecture. RSS
steers flows rather than ObjectIDs, so object-aware ingress may require
second-stage dispatch or an SNC protocol/header exposing routing
information early. Establish baselines and measure each optimization.

## Separate Networking-OS Project

A separate networking OS is being developed from F5/TMOS research,
intended to evolve toward F5, x86, and potentially ARM backends.

``` text
SNC object/state runtime
        │
transport/locality interface
        │
portable networking OS
        │
F5 │ x86/DPDK │ ARM/DPU
```

Neither project should depend on the other's unfinished internals.

## Change Discipline

Before a significant change: 1. Identify the invariant or measured
bottleneck addressed. 2. Read relevant implementation and benchmark
documentation. 3. Classify the change as **KEEP**, **REFINE**, or
**RECONSIDER**. 4. Add/update correctness tests. 5. Benchmark
performance-sensitive changes against the current implementation. 6.
Update `docs/STATUS.md` and relevant subsystem docs. 7. Add a concise
handoff entry below when another agent's assumptions change.

Avoid large architectural rewrites without rationale, evidence, and a
migration path.

## Architecture Review Priorities

Before another major subsystem, review: - ObjectID/object
representation - ownership, placement, and shard mapping - execution and
cross-shard dispatch - memory allocation and NUMA behavior -
WAL/checkpoint/recovery contract - replication/fencing/failover
semantics - TCP routing and future DPDK/networking-OS boundary -
transition from two-node HA to a general distributed object model

The current HA implementation must not accidentally become the permanent
distributed-object architecture without deliberate review.

## Agent Handoff Protocol

For meaningful cross-agent work record: - **Date/time (UTC)** -
**Agent** - **Commit/PR** - **Area** - **What changed or was found** -
**Evidence/tests** - **Open question / next action**

Keep entries concise. Detailed reasoning belongs in docs, PRs, commits,
or source comments.

## Agent Handoff Log

### 2026-10-13 --- ChatGPT architecture review

-   **Commit reviewed:** `86b26f6` on `main`
-   **Area:** repository-wide architecture
-   **Finding:** Original SNC v0.1 is substantially implemented;
    development has advanced into HA/distributed concerns.
-   **Evidence:** `docs/STATUS.md`, `docs/BENCHMARKS.md`,
    `docs/SEASTAR.md`, `docs/REPLICATION.md`, source tree, and recent
    merged PRs.
-   **Decision:** Do not restart v0.1 implementation. Reconcile the
    implemented architecture with the intended long-term
    state-native/locality model before adding major new subsystems.
-   **Next:** Perform a **KEEP / REFINE / RECONSIDER** architecture
    audit, especially ownership, placement, persistence, replication,
    and the future TMOS-inspired DPDK/networking-OS boundary.

### 2026-10-13 06:25 UTC --- Claude (Claude Code)

-   **Commit/PR:** PR #15 (`claude/project-thread-n8tebd`)
-   **Area:** multi-node milestone 1; transfers
-   **What changed:** A fixed slot map across nodes (`docs/CLUSTER.md`):
    each node owns a contiguous share of slots, refuses ops on others'
    objects (`E_WRONG_NODE`), and sends transfer follow-ups over node
    links (TCP, no acks; every reconnect re-runs resume hooks). A
    `CLUSTER` request and `net::ClusterClient` route clients to owners.
    `Account.xfer_query` now re-sends the credit when the sender still
    holds the entry (a settle lost on a link). Classified **REFINE**:
    built on the existing follow-up and resume mechanism; the map is
    static and nodes have no standby yet.
-   **Found and fixed:** `test_transfer`'s SIGKILL rounds lost money
    under CPU load (7 of 24 runs with four copies in parallel, on
    `main` too). Cause: during recovery (and promotion) one shard could
    start running follow-ups while another was still finishing; a
    credit then ran on a shard whose WAL was not open yet, was treated
    as durable without being logged, and its settle removed the
    sender's outbox entry. Now follow-ups are held until every shard has
    recovered or been promoted, and the network front end serves only
    after recovery. 0 of 40 parallel runs fail since. The crash test
    also checks that the recovered state conserves money before
    anything resumes (`Config::hold_emits`).
-   **Evidence/tests:** `tests/test_cluster.cpp` (three node processes,
    one SIGKILLed and restarted mid-run: money conserved, nothing in
    flight); full ctest passes.
-   **Next:** The KEEP / REFINE / RECONSIDER audit as `docs/ARCHITECTURE_AUDIT.md`, folding in the
    multi-node design, before milestone 2 (directory in the witnesses).
