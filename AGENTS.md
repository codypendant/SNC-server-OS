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
11. **Failover is not migration.** A leadership change inside a
    replication group does not move slots.
12. **Placement epoch and HA term are independent version domains.**
13. **Slots map to replication groups; HA selects the active replica
    within a group.**
14. **The hot execution path consumes cached control-plane state and
    does not perform consensus.**
15. **Persistent object/protocol state is authoritative; transport
    delivery is recoverable and transient.**
16. **No inter-object or externally visible computation resumes before
    the execution domain crosses its recovery barrier.**

Invariants 11--16 come from `docs/ARCHITECTURE_AUDIT.md` (2026-10-13).

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

### 2026-10-13 11:15 UTC --- Codex architecture audit (committed by Claude)

-   **Commit reviewed:** `a835aef` on `main`
-   **Area:** repository-wide architecture
-   **What changed or was found:** `docs/ARCHITECTURE_AUDIT.md`. Nearly
    everything is **KEEP**. **RECONSIDER:** using the pair-specific
    witness state as the cluster directory. Slots should map to
    replication groups, with a placement epoch independent of the HA
    term. Six invariants added above (11--16).
-   **Next:** A design spec for replication groups and placement control
    (the ten items in the audit's section 25) before any milestone 2 code.

### 2026-10-13 11:15 UTC --- Claude (Claude Code): response to the audit

-   **Area:** cluster control plane
-   **Agreement:** I accept the audit as written. The milestone 2 plan
    I proposed (the witnesses' pair state extended into the directory)
    is withdrawn; it would have tied a slot's owner to an HA term, so a
    failover would have looked like a migration.
-   **Checked against code:** the claims match `a835aef`. Witnesses hold
    one space's term, primary, and degrade grant (`docs/REPLICATION.md`);
    `create_near` mints until the hash lands in the target slot; a resume
    pass walks every object (`docs/CLUSTER.md`, limits).
-   **One observation:** today HA and the cluster are disjoint: a
    replicated space is one pair owning all 4,096 slots, and a cluster
    node has no standby. So a replication group already exists in the
    code as "an HA pair"; the spec mostly has to name it, give it an ID,
    and let several of them share one placement map.
-   **Next:** Claude drafts `docs/CONTROL_PLANE.md` covering the audit's
    ten items, as a PR for Codex to review (through cody while Codex has
    no write access). No directory code until it is agreed.

### 2026-10-13 11:40 UTC --- Claude (Claude Code): control-plane spec

-   **Commit/PR:** `docs/CONTROL_PLANE.md` (docs only, no code)
-   **Area:** cluster control plane (audit section 25)
-   **What changed:** A proposal answering the audit's ten questions.
    Nodes have stable `u32` IDs; a replication group is one space (UUID,
    WAL lineage, term) with same-shape members; placement maps slots to
    groups under a `u64` epoch that failover never changes. Recommends
    one Paxos library in the existing witness processes running separate
    `placement` and per-group `ha/<g>` state machines. Stale maps get
    `E_MOVED {epoch, group}`; a moved slot is fenced by a durable seal in
    the source group's own data, so no serving lease is needed.
-   **Found:** mint leases are per space today, so two nodes can mint the
    same `(lease, seq)`; harmless only while slots never move. Leases
    must become cluster-wide (blocks from the placement state) before
    the first slot move.
-   **Open question / next action:** Codex to review the spec, above all
    section 10 (where placement consensus runs), section 9 (seals instead
    of leases) and the open questions at the end. No code until agreed;
    proposed first code step is groups with HA under a static map.

### 2026-10-13 11:35 UTC --- Codex review of `docs/CONTROL_PLANE.md` (relayed by cody, applied by Claude)

-   **Area:** cluster control plane
-   **Decision:** approved directionally, with three changes now in the
    spec: (1) one group per node is not a schema rule (`NodeID →
    memberships[]`; milestone 2 may restrict it operationally); (2) seals
    are kept, but serving from a stored map at startup is provisional
    until the crash orderings of `seal durable → epoch commit →
    destination activation` are proved at the slot-move milestone;
    (3) placement never copies a group's primary or term: one authority
    per fact, primaries cached only as ephemeral routing state.
-   **Accepted as proposed:** cluster-wide lease blocks, `E_MOVED`, no
    write forwarding, persisted maps, epoch in the link hello, shared
    Paxos library with separate `placement` and `ha/<group>` machines,
    groups with HA under static placement first.
-   **Next:** Replication groups with HA under the static
    map (spec section 12, step 1).

### 2026-10-13 12:30 UTC --- Claude (Claude Code): replication groups with HA, static placement

-   **Commit/PR:** `claude/project-thread-n8tebd` (control-plane spec
    section 12, step 1). Classified **REFINE**: the milestone 1 cluster and
    the existing pair replication, joined; no new consensus.
-   **What changed:** The cluster map is now slots → groups
    (`ClusterGroup {n_shards, members[]}`, `group_of`, `epoch` fixed at 1);
    `--cluster` takes members per group (`|`), nodes take `--group` and
    `--member`. A group's members replicate exactly as a pair does. Node
    links: only a primary listens; an outbound link tries the target
    group's members in turn, so it finds a promoted standby by itself; the
    hello carries crc and placement epoch. `Shard::promote` opens the
    links. `CLUSTER` lists members per group and the epoch; no primary is
    in it. `ClusterClient` keeps one client per group and, with
    `enable_sessions`, follows that group's failovers.
-   **Evidence/tests:** `test_cluster` `test_group_failover`: three groups
    of primary + sync standby, group 1's primary SIGKILLed mid-run and
    promoted over. Every acknowledged transfer is in the final balances
    (checked exactly: the only freedom is the killed primary's 0 to 12
    unreported in-flight transfers, every subset tried; the check fails
    if one acked transfer is dropped from the log), money conserved, all
    cross-group transfers settled, groups 0 and 2 acknowledged ~2,000 to
    3,000 transfers each while group 1 had no primary, the promoted member
    received follow-ups and ran its own, placement epoch 1 and the crc
    unchanged on every primary, and a session `ClusterClient` reads all
    accounts from the current primaries. 15/15 runs (3 in parallel), ASan
    clean; full ctest passes; the milestone 1 kill/restart test still
    passes.
-   **Found (changes an assumption):** "links find the primary because
    standbys do not listen" holds for a dead primary, not a cut-off one. A
    fenced old primary keeps listening and its inbound links stay up, so
    other groups keep sending it follow-ups it cannot make durable;
    transfers stall (never unsafe) until its connections drop. Options for
    the next step: a fenced or blocked primary closes its links, or the
    hello carries the group's HA term and receivers of a newer term win.
    The second puts HA state in a data-plane handshake (not in placement),
    which seems consistent with "one authority per fact" but is Codex's
    call.
-   **Not yet covered:** witnesses per group and `--rejoin` inside a
    cluster are unchanged code but not exercised by a cluster test; the
    test promotes the standby itself.
-   **Next:** Codex to review. Then the fenced-primary link question, or
    step 2 (the `placement` state machine).


### 2026-10-13 14:45 UTC --- Claude (Claude Code): a fenced primary withdraws from the data plane

-   **Commit/PR:** `claude/project-thread-n8tebd` (Codex's 14:27 request,
    before control-plane step 2). Classified **REFINE**: lifecycle only,
    no new protocol; the HA term is not in the node-link hello.
-   **What changed:** A promoted node keeps its replication listeners and
    refuses the old term there (`Receiver::fence`), so a live old primary
    learns within ~100 ms that it was replaced (`Sender::fenced`); a
    witness's `NO term ... replaced` does the same via `snc_server`
    (`Engine::withdraw`). A fenced primary, on every shard: closes its link
    listener and all node links, runs no follow-ups, answers new client ops
    `E_STANDBY` and ops waiting for the standby's ack the new status
    `E_FENCED` (in doubt; `net::Client` fails over on it); admin `STATUS`
    reports `role=f`. Only a restart (`--rejoin`) brings it back. Remote
    groups' existing member rotation finds the new primary.
-   **Evidence/tests:** `test_cluster` `test_group_failover(true)`: group 1's
    primary stays alive while its standby is promoted; it withdraws
    ~0.1 s later; its link ports refuse connections; in-process and network
    ops get `E_STANDBY`; groups 0 and 2 keep acknowledging; all transfers
    settle; every acknowledged transfer of either group-1 primary is in the
    final balances (exact subset check over the old primary's up to 12
    unacknowledged ones; runs saw both outcomes, hence "in doubt"); money
    conserved; epoch 1 and crc unchanged on all live nodes, the fenced one
    included; still fenced at the end. With `links_.reset()` removed the
    test never settles. 12/12 runs (4 in parallel), ASan clean, full ctest
    passes. `test_replication`'s restarted-old-primary check now also
    requires that it reports itself fenced.
-   **Not covered (changes no assumption):** a primary cut off from both
    the promoted member and every witness, while other groups still reach
    it, does not learn it was fenced: it blocks (acks nothing) and keeps
    its links. Option if wanted: withdraw while blocked past a bound.
-   **Next:** per Codex, control-plane step 2 (the `placement` state
    machine).

### 2026-10-13 15:10 UTC --- Claude (Claude Code): control-plane step 2a, placement in consensus

-   **Commit/PR:** `claude/project-thread-n8tebd` (spec section 12,
    step 2, first part). Classified **REFINE**: the witnesses' Paxos,
    generalised; no change to the data plane beyond `E_MOVED`.
-   **What changed:** `tools/paxos.h`: the single-decree-per-version Paxos
    from `snc_witness`, now running independent named machines (own
    versions, one state file; old state files load as `ha/0`). Each
    witness holds `ha/0` (its pair, unchanged) and `placement` (opaque to
    it: `PLACEMENT INIT` once, `PLACEMENT GET` after a majority read).
    `include/snc/placement.h`: the schema of section 3 as one line (cluster
    ID, epoch, `NodeID@host:admin`, groups with space UUID, shards and
    members `node:link:client`, slot runs); one membership per node is
    enforced as milestone 2's operational rule. `snc_ctl init|get`.
    `snc_server --control ... --node-id N`: fetches the map, finds its group
    and member, takes its client port and the group's space UUID, stores
    map and NodeID in the data dir; refuses a foreign data dir, another
    space, an unnamed node, or no map; with the control plane down starts
    from the stored map (provisional, section 9). `E_MOVED` (result
    `MovedResult {epoch, group}`) replaces `E_WRONG_NODE` for clustered
    nodes, and is checked before the overload check (found: a restarted
    node with a follow-up backlog answered a misrouted op `E_OVERLOADED`).
-   **Evidence/tests:** `test_cluster` `test_kill_restart(true)`: three
    witnesses; before init a node may not start; init chosen once (same
    map accepted, another refused), `STATE 1 <map>` on every witness;
    unknown NodeID and another node's data dir refused; three nodes start
    by NodeID (space UUID from the map), node 1 SIGKILLed and restarted
    with all witnesses killed, from its stored map; money conserved,
    settled, routing by the map, `E_MOVED` with epoch 1 and the right group.
    `test_witness.py` unchanged in substance (passes). 12/12 runs (3 in
    parallel), ASan clean, full ctest passes. Manual: `snc_ctl init` then
    two `snc_server --control` nodes start, admin `STATUS` shows `epoch=1`.
-   **Left for 2b (CONTROL_PLANE.md section 12):** cluster-wide mint lease
    blocks; one witness set running `ha/<g>` for several groups; map
    refresh on a newer epoch (untestable until slots can move).
-   **Found (not fixed):** `test_replication` picks ports up to ~37000,
    inside the ephemeral range, so under `ctest -j4` a seed port is
    occasionally taken ("cannot listen for seeds"). Pre-existing.
-   **Next:** Codex review; then 2b or slot moves (step 3), Codex's call.

### 2026-10-13 15:33 UTC --- Claude (Claude Code): control-plane step 2b

-   **Commit/PR:** `claude/project-thread-n8tebd` (Codex's 15:16 request).
    Classified **REFINE**: new machines on the existing Paxos library;
    no data-plane protocol change.
-   **Lease blocks (correctness prerequisite for slot moves):** a `leases`
    machine (body `next N`, starting at 2^32 so it is above every lease a
    space minted from its own counter). `LEASES TAKE count` -> `BLOCK lo
    hi`. Each proposed value carries a random tag: without it, two
    witnesses taking a block from the same base would propose identical
    bodies, and Paxos would report success to both (found while writing
    it). Nodes hold one 2^16 block in the superblock (new fields
    `lease_lo`, `lease_hi`; zero in old superblocks = legacy counter),
    written before minting; a new block when used up; a promotion always
    takes a fresh block first and is refused, unchanged, without one.
    Standbys take none. With a block, the old "mint above every lease seen
    from replication" bumps are skipped (a block is disjoint by
    construction).
-   **HA per group on one quorum:** `snc_witness --group G=P:ADMIN,S:ADMIN`
    (repeatable; `--primary/--standby` = group 0) runs one watcher and one
    `ha/G` machine per group. `DEGRADE/SYNCED uuid term [group]`
    (snc_server sends its group when clustered; otherwise looked up by
    space). Log lines now say the group.
-   **Evidence/tests:** `test_witness.py` multi-group: three groups on
    three witnesses plus placement and leases; group 1's primary goes
    silent and is promoted (ha/1 term 1), group 2 gets a grant, and ha/0,
    placement and leases are byte-identical before and after; no other
    standby got PROMOTE; group 1's old term is refused. New
    `test_control.py` (ctest): real snc_server nodes, two groups of
    primary + sync standby by NodeID, one quorum watching both; group 1's
    primary SIGKILLed, promoted by ha/1 to term 1 with a fresh lease
    block; ha/0, placement (version 1) and epoch 1 on every node
    unchanged; all lease blocks disjoint. `test_cluster`: the three
    control-plane nodes' account IDs use three disjoint blocks >= 2^32
    and the leases machine reads `next 2^32 + 3*65536`. Repeat: full ctest
    -j6 5/5, test_cluster 9/9 (3 in parallel), ASan clean.
-   **Test ports:** `test_replication` now takes a free run of ports below
    32768 (`free_ports`), and the Python tests pick ports in 20000-32000
    instead of from the ephemeral range.
-   **Next (per Codex):** slot migration and seal design with the crash
    ordering proof (CONTROL_PLANE.md section 9), including map refresh on
    a newer epoch.

### 2026-10-13 17:13 UTC --- Claude (Claude Code): control-plane step 3, slot moves

-   **Commit/PR:** `claude/project-thread-n8tebd` (Codex's 15:38 request).
    Classified **REFINE**: placement CAS plus group-local seal records on
    the existing Paxos library and replication; no new consensus.
-   **What changed:** `docs/MIGRATION.md` (state machine, serving rule,
    proof, crash table). A move record in `placement` advanced by CAS:
    PREPARED -> SEALED -> STAGED -> COMMITTED (only the commit raises the
    epoch); removing it before COMMITTED aborts. Move IDs are the placement
    version at PREPARE, so they never repeat. Groups keep `System.SlotState`
    (SEALED / STAGED / ACTIVE), logged and replicated like any object. A node
    serves a slot only if its map gives it to its group and the slot is not
    sealed or staged (`E_MIGRATING` otherwise). Copy ahead from durable
    state, final copy after the seal is durable, count+digest checked;
    activation once the committed map is seen; unseal only after a
    linearizable read shows the move gone. Groups and nodes refuse late
    seal/stage requests. `snc_ctl move SLOT GROUP` / `move --abort SLOT`.
    Map refresh on `E_MOVED` (`ClusterClient`) and on a newer epoch over a
    node link (held, refreshed at once).
-   **Found and fixed (changes an assumption):** "follow-ups between two
    objects arrive in order" breaks when a slot moves. The crash tests found
    money created or a credit applied twice three ways (an old owner's copy
    trailing the new owner's; a copy waiting on durability overtaken; held
    copies released late). Follow-ups now carry their emission epoch; stale
    ones are dropped and re-sent by resume, newer ones held; map adoption is
    two passes over shards. Also: a resent sequenced request answered
    `E_MOVED` by a promoted node with an old map was applied twice; the
    session replay check now runs before any moved/migrating refusal.
-   **Codex's provisional point:** starting from a stored map is now safe
    (seals, not the map, gate serving); CONTROL_PLANE.md section 9 updated.
-   **Evidence/tests:** `test_migrate`: 67 scenarios; stop after each of 7
    steps x {driver only, source/destination primary killed and promoted,
    source/destination standby killed, a witness killed, source/destination
    restarted from the stored map with all witnesses down}, aborts at every
    step, a poll-only refresh, and a move back. Checks: a probe never sees
    two serving groups; every acked transfer in the final balances (exact
    subset over in-doubt ones); money conserved; IDs unchanged, each served
    once; epoch changes only at commit; all settled; late seal/stage
    refused. 8 full runs clean, ASan clean; full ctest passes (release
    and ASan). Hot path unchanged within noise: `snc_bench` 2 shards, 30%
    transfers, 50% remote, interleaved with `main`: 1.83M vs 1.77M ops/s in
    memory, 118K vs 116K durable.
-   **Limits:** one move at a time; sealed source copies not collected; a
    seal left by a driver that died mid-abort stays until a driver runs
    again for the slot; no read forwarding; whole-slot final copy in one
    admin line.
-   **Next:** Codex review. Then read forwarding, concurrent moves, or real
    machines (step 4), Codex's and cody's call.

### 2026-10-13 18:22 UTC --- Codex review of PR #24 (relayed by cody, recorded by Claude)

-   **Commit reviewed:** `697ef2c` on `main`
-   **Area:** slot migration
-   **Decision:** accepted: the seal -> exact final copy -> staged
    destination -> placement commit -> activation ordering, the crash
    matrix, epoch-stamped follow-ups with stale-drop and resume, and
    two-pass node-wide map adoption. New invariant 17 above.
-   **Direction:** no concurrent moves yet. Next is real machines: run the
    replication, failover, cluster and migration behaviour across physical
    hosts before more distributed-control complexity. Later: replace
    whole-slot admin-line copies with a chunked stream (`BEGIN -> CHUNK* ->
    VERIFY -> STAGE`, transport kept abstract); a sealed source may unseal
    by itself only after a linearizable placement read shows the move gone
    and the slot still its group's; read forwarding optional.
-   **Recorded by Claude:** `docs/REAL_MACHINES.md` (hardware, harness,
    phases P0-P6, seven loopback assumptions to confirm or clear);
    `docs/MIGRATION.md` "Next" (the stream and the reconciliation rule,
    designs only). Found while writing the plan, from the code: node links
    set no keepalive or `TCP_USER_TIMEOUT` and have no acks, so a link to
    a powered-off host may look healthy until the kernel's retransmit
    limit (~15 min), delaying settles (not safety); clients and the
    HA/admin path accept IPv4 literals only.
-   **Next:** cody to provide hosts (3 minimum) and a way to drive them;
    Claude builds the lab harness (`bench/lab/`) meanwhile, against
    loopback, so phase P0 can start as soon as hosts exist.
