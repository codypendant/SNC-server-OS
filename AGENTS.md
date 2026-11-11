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
17. **A placement epoch is adopted node-wide before state-driven replay
    for that epoch begins.**
18. **A duplicate is resolved before any refusal that invites a resend.**
    For a sequenced mutation, the owner answers from the client's session
    before returning any status that would lead the client to send the
    request again as new work (today `E_MOVED`, `E_MIGRATING`,
    `E_OVERLOADED`). See `docs/EVIDENCE.md` section 1.

Invariants 11--16 come from `docs/ARCHITECTURE_AUDIT.md` (2026-10-13);
17 from Codex's review of slot moves (2026-10-13 18:22 UTC; the handoff
log cited it but it was missing from this list until 2026-11-04, and its
wording here is taken from that review as relayed, since no earlier text
of it exists in the repo); 18 from the real-machine partition finding (2026-11-04).

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

### 2026-10-13 18:50 UTC --- Claude (Claude Code): real-machine lab harness

-   **Commit/PR:** `claude/project-thread-n8tebd` (Codex's 18:22
    direction). Classified **REFINE**: test tooling plus two operator
    knobs; no protocol change.
-   **What changed:** `bench/lab/lab.py` runs the plan's phases (P0-P5,
    soak) on an inventory of hosts over SSH, or on loopback
    (`bench/lab/loopback.toml`) with the same code; `snc_load` (create,
    transfer load with per-interval latency, and `test_migrate`'s balance
    check); `snc_server` admin `AUDIT [PATH]`; `snc_ctl move --stop-after
    STEP --chunk N`; timestamped driver logs. A node started with
    `--control` now listens for clients on every interface (it listened on
    loopback only, though the map advertises its host).
-   **Evidence (loopback, `bench/results/lab-20261013/`):** failover under
    load, kill and SIGSTOP: promoted ~1.0 s, hung old primary withdrew in
    0.12 s after SIGCONT, every acknowledged transfer present. Slot move
    of 2,012 objects under load and back: never two serving groups, 0.18 s
    with none serving, epoch 3 on every node, check OK. `test_migrate`,
    `test_control`, `test_cluster` pass.
-   **Found (changes an assumption):** a move's duration is dominated by
    the copy ahead of the seal, 8 objects per durable round trip: 5.7 of
    6.0 s for 2,012 objects; 0.56 s total with 256 per request. The client
    window (seal to activation, ~0.25 s) does not depend on it. On real
    hosts each round trip adds RTT, so this is the first number P5 should
    measure; it supports the chunked stream.
-   **Next:** cody brings up 3+ hosts (Ubuntu Server 24.04; setup in
    `docs/REAL_MACHINES.md`) and a Claude Code session on one of them runs
    P0-P6. Codex to review the plan and harness meanwhile.


### 2026-11-04 UTC --- Claude (Claude Code): real-machine run on Vultr

-   **Commit/PR:** `claude/project-thread-n8tebd` (this PR). Classified
    **REFINE**: harness/test fixes and results only; no protocol or
    data-plane change.
-   **Area:** real-machine lab, phases P0-P6 (`docs/REAL_MACHINES.md`).
-   **Hosts:** three Vultr VPSs in Dallas, 2 vCPU / 3.8 GB / one NIC each,
    **public IPs** (no private networking). ufw opened for tcp 21000-21600
    between the hosts; clients/driver and witness 2 on c, a group's primary
    and standby never shared. `bench/lab/vultr.toml`,
    `bench/results/lab-20261104/`.
-   **What changed (harness/tests, loopback-only assumptions):** (1)
    `tests/test_runtime.cpp`'s 60 s deadline assumed >= 4 cores; 4 shards on
    2 cores take ~102 s, so ctest failed. Now scales by
    `ceil(shards/cpus)`; unchanged on >= 4 cores. (2) P0's `iperf3` used
    its default port 5201, outside the firewall-opened range, and hung
    ~130 s; now `base_port+500` with `--connect-timeout`. (3) `check()`
    preserves `.delta`/`.unknown`/`ids` on a failed check for post-mortem.
-   **RTT effects:** all three hosts in one datacentre, RTT ~0.34 ms, so
    steady-state throughput barely differs from loopback and no loopback
    timeout was too tight. netem 1/5/20 ms degrades throughput gently
    (9.7K -> 7.7K ops/s) and never trips `failover_ms` 1000.
-   **Reconnect / failover behaviour (assumption changes):** a silent cut
    (partition/oneway) has no RST, so promotion waits out timeouts and
    takes ~8 s vs ~1.5 s for kill/stop -- the dominant real-vs-loopback
    failover difference. Sync replication is **0.98-1.05x** of non-sync
    durable (loopback 0.54-0.60x): across hosts the standby has its own
    cores and the RTT overlaps the fsync. P4's 10 s cross-group cut built a
    ~26K-entry follow-up backlog that drained 8.1 s after heal.
-   **Migration throughput:** move time is N/chunk durable round trips plus
    RTT each: 1K/10K move in 4.8 s/43.5 s at chunk 8, 0.58 s/2.9 s at chunk
    256. **The 100K-object whole-slot move at chunk 8 does not complete** --
    after ~197 s the driver returned `STUCK no control member answered` and
    left the slot sealed, not activated (retry refused `BUSY`); chunk 256
    does 100K in 11.3 s. The client-visible E_MIGRATING window stays
    0.1-0.9 s regardless of slot size. Direct evidence for the chunked
    stream (`docs/MIGRATION.md` "Next"). The P5 crash matrix (source/dest
    primary killed at sealed/committed) all pass; soak 20 min / 30 rounds
    of kill/stop/move passed, money conserved.
-   **Open question / next action:** an intermittent (~1 in ~24) balance
    check failure under the `partition` fault only -- net 0 (money
    conserved) but ~32 accounts off with fewer in-doubt recorded than
    needed. Either a rare lost/double-applied acknowledged transfer
    (candidate: a request's account and its session object on different
    shards, replication cut at different points -> replay re-applies) or an
    `snc_load` in-doubt under-count. `check()` now preserves inputs to
    catch the next one. **Failover under partition is not proven until this
    is classified.** Then: the 1M-object reconnect and node-link isolation
    need the 5-host layout (3 hosts cannot cut one node link without
    failing over both groups).

### 2026-11-04 03:10 UTC --- Claude (Claude Code): the partition double-apply

-   **Area:** client sessions (docs/RETRIES.md) under failover. Classified
    **REFINE** (an ordering fix, no protocol change).
-   **Found:** the Vultr run's open item (balances off under `partition`,
    net 0) is a real bug, not an `snc_load` miscount. `Shard::execute`
    answered `E_OVERLOADED` (follow-up backlog over 1,024) before the
    session replay check. After a promotion the backlog is large while
    clients resend; a resend of an already replicated transfer got
    `E_OVERLOADED`, and clients resend that as a new request, so it ran
    twice. Same shape as the `E_MOVED` double-apply fixed in PR #24.
-   **Fixed:** a sequenced mutation checks its session before the
    overload refusal. Rule now stated in RETRIES.md: a duplicate gets its
    answer before any refusal a new request would get.
-   **Evidence:** `test_transfer` `test_resend_overloaded` fails without
    the fix, passes with it (also under ASan); full ctest passes.
-   **Open:** not re-run on real hosts (servers torn down); a confirmation
    is ~24 `p3 --fault partition` rounds on three fresh hosts.


### 2026-11-04 04:30 UTC --- Claude (Claude Code): partition re-run with the fix

-   **Commit/PR:** `claude/p3-partition-confirmation`; ran `74209a8`.
    Docs and results only; no code change.
-   **Area:** client sessions under failover; real-machine lab.
-   **Found:** **24 of 24** `p3 --fault partition` rounds passed on three
    fresh Vultr hosts: no double-apply, every old primary fenced, promote
    7.66-8.41 s, 489-4,247 `E_OVERLOADED` per round.
-   **Limits (keep these when citing it):** at the old rate of about 1 in
    24, 0 failures in 24 has a ~36% chance without any fix, and bounds the
    rate only below ~12% (95%). `snc_load` does not count refused resends
    separately, and the one failing round had more refusals (10,942) than
    any round here. The fix rests on the cause and
    `test_resend_overloaded`; this run is consistent with it, not proof.
-   **Harness (changes an assumption):** `--seed` does nothing for `p3`,
    only `soak` reads it. `p3` passes or fails on the balance check alone,
    and only logs an old primary that does not withdraw.
-   **Setup:** `vultr_setup.sh` had not finished on c: the inventory still
    held the old addresses and nothing was deployed. Cause not determined.
    I applied its last two steps by hand; the new addresses are not
    committed in `vultr.toml`.
-   **Evidence:** `bench/results/lab-20261104/p3-040835` to `p3-042527`;
    `docs/REAL_MACHINES.md`, "Partition re-run with the fix".
-   **Next action:** for a stronger result, count refused resends in
    `snc_load`, or run the pre-fix build the same way to measure its rate.

### 2026-11-04 05:40 UTC --- Claude (Claude Code): contracts and evidence pass

-   **Commit/PR:** `claude/project-thread-n8tebd` (this PR). Classified
    **KEEP** (no engine change): docs, plus a `test_cluster` robustness
    fix. Its live-fence progress check sampled a fixed 300 ms window and
    failed under load (2 of 6 runs beside `test_migrate`). It now waits up
    to 10 s and prints the gap (0.00-0.23 s in 18 loaded runs). A failed
    CHECK no longer leaves forked members holding ctest's pipe, which had
    turned the failure into a 12-minute hang.
-   **Area:** retry contract, acknowledgement stages (R08), obligation
    foundations (R14), failure detection, migration, backlog metrics.
-   **What changed:** `docs/EVIDENCE.md` (new); `docs/STATUS.md` real-machine
    row, replication cost across machines, test count; invariants 17 and 18
    here.
-   **Found:** no new violation of the retry contract in the execute or
    admission path. Gaps: sessions across a shard-count change rely on
    `net::Client` refusing a changed shard count (undocumented); deletes are
    not sequenced; `E_MOVED`/`E_MIGRATING` ordering has no deterministic
    test; a witness failover of an `--async-replica` pair can drop acked
    writes (inferred from code, untested and undocumented). Invariant 17 was
    cited in this log but missing from the list.
-   **Not determined:** which stage of the detection chain adds the ~6 s a
    silent partition costs over a kill. The logs were not kept; no
    timeout should change before lab experiment L0.
-   **R01-R14:** mapped in `docs/EVIDENCE.md` section 0 against the
    Contracts and Validation Handoff v1.0 (now in `docs/handoff/`). None is
    STRONG. R08 is implemented with insufficient evidence. R01, R03, R04,
    R06, R07, R10, R11, R12 and R14 are partial. R02, R05, R09 and R13 are
    missing. R11 has a correctness gap: dedupe evidence expires after the
    session TTL, and a resend after that runs again.
-   **Next action:** L0 plus contract tests, no behavior change
    (`docs/EVIDENCE.md` section 11).


### 2026-11-04 06:20 UTC --- Claude (Claude Code): verification slice after Codex's review of PR #33

-   **Classification:** **KEEP** (no engine change). Characterization tests,
    one benchmark-client bug fix, docs.
-   **Area:** R08 (async failover), R11 (retry horizon), invariant 18
    (`E_MOVED`/`E_MIGRATING`), migration scale evidence.
-   **What changed:** `tests/test_async_failover.py` and
    `tests/test_resend_refusals.cpp` (new ctests); `test_session_ttl` now
    asserts the balance; `bench/snc_load.cpp` dangling-reference fix;
    `docs/EVIDENCE.md` section 12; `docs/STATUS.md`.
-   **Found:**
    -   R08 confirmed: a witness promotes an `--async-replica` standby that
        lacks acknowledged writes (500 of 500 acked creates missing on the
        promoted node). Sync control: not promoted under a degrade grant.
    -   R11 confirmed: a resend after session expiry applies a second time
        (balance start+2).
    -   Invariant 18 holds for `E_MIGRATING` and `E_MOVED`, also after a
        restart. An older seq gets `E_REPLAYED_OLD`, never a refusal.
    -   The "100K" P5 slots held ~31K because `snc_load create --in-slot`
        kept a reference into a vector it grew (capacity 131,072 after
        100,000 creates, hence ~31K). Fixed. A real 100K slot moved on
        loopback in 57 s, and the move caused a false failover of the
        destination primary during the final copy (inferred: one long
        stage step stalls the shard past `failover_ms`; not verified).
-   **Contract choices** for R08 and R11 are listed in `docs/EVIDENCE.md`
    section 12 with a recommendation each. Not decided; nothing changed.
-   **Not run:** ASan/TSan (the sanitizer build directories are stale);
    real-machine 100K rerun.
-   **Next action:** Codex decides the R08 and R11 contracts; then the
    bounded final copy (section 4) or L0, as Codex prefers.

### 2026-11-04 UTC --- Claude (Claude Code): core stabilization work order (characterize and specify)

-   **Classification:** **KEEP** for the engine (no semantic change).
    Instrumentation, bench and test fixes, and specifications only, as the
    work order authorizes. One operator-tool message fix is flagged for
    review (below).
-   **What changed:**
    -   `SNC_TRACE=1` stopgap tracing (`include/snc/trace.h`; shard loop,
        admin, witness, migration, stage phases), `bench/lab/cpu_sample.py`,
        `bench/lab/trace_merge.py`, and `bench/stage_stall` (an in-process
        repro of the stall).
    -   `tests/test_cluster.cpp`: a data race in the test's own counter
        (TSan) fixed with an atomic.
    -   `tools/migrate.h`: the driver's `STUCK` message no longer carries a
        stale reason, and names the deadline and move state. **For review:
        it is an operator tool's output, at the edge of the authorization.**
    -   New docs: `CORE_CONTRACTS.md` (A, F, G, H, I), `FAILURE_MODEL.md`
        (B), `INTERACTION_MATRIX.md` (C, M), `TRACING.md` (E),
        `STABILIZATION_REPORT.md` (D, J, K, L, N, P, Part X, sanitizers,
        review and QA findings). EVIDENCE.md and STATUS.md reconciled.
-   **Found:**
    -   The 100K false failover is explained and measured: the final stage
        runs as one shard task (3.7-5.0 s, 91% re-indexing a posting list
        shared by every account), `STATUS` waits on every shard thread,
        and the witness treats a 100 ms poll timeout as silence. The
        promoted node is left with no replica, acknowledging
        LOCAL_DURABLE with nothing reported.
    -   Sanitizers: ASan+UBSan 16/16; TSan 16/16 after the test fix.
    -   Review found two gaps not known before (INFERRED, EL0): a cut-off
        old primary serves stale default reads; WAL corruption in the
        middle of the last segment silently drops acknowledged records.
        And a possible restart hang (recovery checkpoint waiting for a lost
        standby under a witness).
-   **Not changed:** R08, R11, timeouts, `failover_ms`, TTLs.
-   **Next action:** Codex reviews the contracts (CORE_CONTRACTS sections
    5-14 are PROPOSED unless tagged otherwise). Recommended next slice:
    R08 option F-a (async groups never auto-promote). Not started.

### 2026-11-04 UTC --- Claude (Claude Code): HA health-path decoupling (option B)

-   **Classification:** **REFINE** of the witness's liveness signal and the
    admin port, as approved in the review of PR #36 (option B and the four
    decisions). No durable-format, client wire or Paxos-state change.
-   **What changed:** `HEALTH` admin verb (published state only, shard and
    main-loop progress as diagnostics); per-shard loop counter
    (`NativeRuntime::progress`) and a 10 ms sampler; `Engine::replicating()`
    for `role` without a shard task; a non-blocking admin loop
    (`tools/admin_loop.h`: `HEALTH` inline, `MOVE` handed off early, one
    worker for the other verbs, queue 16, half-read 256, counters);
    `DEBUG STALL` behind `--debug-admin`; witness polls `HEALTH` with
    `STATUS` fallback and `--health-verb status` rollback; `--rejoin`
    uses `HEALTH`. Docs: HEALTH_DECOUPLING section 10, CORE_CONTRACTS
    PE-3, PE-4, B-2, B-3 (option B), FAILURE_MODEL P2 and W1,
    INTERACTION_MATRIX row 11, REPLICATION, TRACING, EVIDENCE, STATUS.
-   **Deviations from the proposal, for review:** the standby is still
    checked with `STATUS` before promotion (no `stall_ms` eligibility rule,
    so eligibility is exactly as before); `progress()` is on
    `NativeRuntime`, not `sri.h`; non-`MOVE` admin lines are capped at
    64 KiB; the main loop wakes every 100 ms so `main_ms` is meaningful.
-   **Evidence:** `tests/test_health.py` T1-T8, T10, T10b, T11 pass in
    RelWithDebInfo, ASan+UBSan and TSan (full suites 17/17 each, server
    children clean); T12 lab P5 100K move with no promotion; T9 no
    regression visible (host noise 8-16%, larger than the 2% bar).
-   **Not changed:** `failover_ms`, `poll_ms`, migration staging, the index,
    R08, R11, trace rings.
-   **Next action:** review of this slice. Bounded/resumable migration
    staging only after that review.

### 2026-11-04 UTC --- Claude (Claude Code): bounded/resumable migration staging (design only)

-   **Classification:** design for review; no code changed. Proposes
    **REFINE** of CORE_CONTRACTS B-1 (progress durable only as the staged
    records themselves, no manifest) and **RECONSIDER** of MIGRATION.md's
    accepted "Chunked migration stream" (no scratch area).
-   **What:** `docs/MIGRATION_STAGING.md`, sections 1-17 as the work order
    asked. Three independent steps: S1 the destination's final stage as a
    chain of bounded units with local fencing guards (node-local, no wire or
    format change; the proposed next slice); S2 converging copy ahead
    (driver only); S3 streamed transfer with versioned verbs. Unit bound is
    shard-thread time plus a byte cap, defaults to be set from a latency
    sweep.
-   **Evidence:** independent review against the code (17 findings, all
    applied). Baseline from `bench/results/lab-20261104/p5-131933`:
    E_MIGRATING window about 6.75 s for 100K objects (17,874 changed), of
    which the final stage held the destination shard 4,772.7 ms (index
    4,347.5 ms); about 2.2 s of window is O(slot) work even with zero
    changes (the move back).
-   **Adjacent defects reported, not fixed:** D1 a moved slot can be served
    from a partial copy after a lossy promotion (async or admin `PROMOTE`;
    R08/E3 class), and D7 its source-side analogue; D2 the lab's window metric undercounts about 40x; D3
    every STAGE reply runs an O(map) `slot_report`; D4 a driver timeout loops
    O(map) HAVE/EXPORT and two drivers can queue duplicate final stages; D5 slot size limit from the 256 MiB line
    cap; D6 the probe's `INFO` polling loads every node.
-   **Not changed:** HEALTH and option B (stall stays diagnostic only);
    the migration stall itself, which is **not fixed**; index, R08, R11.
-   **Next action:** review of MIGRATION_STAGING.md (decisions 1-5 in its
    section 0). No implementation before that.

### 2026-11-04 UTC --- Claude (Claude Code): fail-closed migration serving, D1/D7 (design only)

-   **Classification:** design for review; no code changed. **REFINE** of
    MIGRATION.md's activation rule and safety proof (its HA-durability
    assumption made explicit) and of the promotion path (it reads placement).
-   **What:** `docs/MIGRATION_FAIL_CLOSED.md`, the 19 sections of the PR #38
    review. Serving becomes a positive proof: the placement records, per
    slot, the move that last gave it to its group (`arrived`, placement v2);
    a node serves only with local evidence of exactly that move. Plus a
    start barrier for any start that may hold a replica prefix (linearizable
    placement read, re-seal, `ROLE m`), and a commit gate in the driver.
-   **Found:** D1b (stale STAGED of an aborted move activated: activation
    ignores move IDs), D1c (earlier-era ACTIVE served), D1d (earlier-era
    SEALED reopened by UNSEAL), and the offline promotion path (a standby's
    directory restarted without `--standby`) that bypasses `Engine::promote`.
    All INFERRED from code; F1/F2 reproduce them first in the implementation
    slice.
-   **Also, per the PR #38 review:** MIGRATION_STAGING.md marked accepted
    with modifications (budgets are sweep points, `W_window` UNDECIDED);
    MIGRATION.md's scratch-area stream marked SUPERSEDED (kept); the
    0.24 s / 0.18 s probe window figures marked invalid in EVIDENCE and
    REAL_MACHINES; CORE_CONTRACTS B-1 refined (output as progress).
-   **Evidence:** independent review against the code (17 findings: 4
    blocking, all applied).
-   **Not changed:** R08, R11, S1, any code.
-   **Next action:** review of MIGRATION_FAIL_CLOSED.md (decisions 1-5 in
    its section 0). Then implementation only if approved.

### 2026-11-04 UTC --- Claude (Claude Code): fail-closed migration serving, D1/D7 (implementation)

-   **Classification:** implementation of the slice the PR #39 review
    authorized. **REFINE** of MIGRATION.md (exact activation, serving
    proof, start barrier, commit gate), CORE_CONTRACTS (MS-1 to MS-4 in
    section 2; E9 now covered for migration state) and FAILURE_MODEL (new
    row C7a). No change to R08, R11 or S1-S3; no repair, GC, new client
    status or trace rings.
-   **Order kept:** F1/F2 (and D1b-D1d) reproduced on the old code first,
    exactly as predicted (`bench/results/failclosed-20261104/`); then
    placement v2, exact activation and the cached proof, DIR/role, the
    barrier and re-seal, the gate and aborts, visibility and backfill.
-   **What:** see MIGRATION_FAIL_CLOSED.md section 20 (code map,
    differences from the design, measurements, manual v2 rollback,
    residual risks).
-   **Evidence:** `test_fail_closed` (F1-F11, F13, F15, F17, F18, role
    crash order, backfill, stuck, prepared, f10_reseal, f7_prepared, size;
    compat and f7b against an old build), `test_proof` (C, D, E, F12),
    `test_placement` (v2 text rules); independent source review and QA
    against the four documents (findings fixed: MIGRATION_FAIL_CLOSED.md
    section 20). Sanitizer and full-suite results: section 20 "Runs".
-   **Next action:** review of this slice. Do NOT start S1 before it.

### 2026-11-04 UTC --- Claude (Claude Code): legacy role upgrade hardening

-   **Classification:** the slice the review of PR #40 (item 17) ordered
    before S1. **REFINE** of MIGRATION_FAIL_CLOSED.md (residual 6 closed;
    section 20's in-place-promotion wording made exact, item 8) and
    CORE_CONTRACTS MS-3. No change to S1-S3, R08, R11, the driver's retry
    rule, STUCK or rollback.
-   **What:** [docs/LEGACY_ROLE.md](docs/LEGACY_ROLE.md). A directory with
    no role file and no `kSbRoleTracked` is refused as a primary; it starts
    with `--standby` (role and tracking durable before replica service) or
    once with `--adopt-role primary`: refused without the control plane or
    if a group member is silent, serving/promoting or at a newer term;
    then the start barrier; tracking only after role `primary`. Option B
    (infer the role) rejected: nothing on disk distinguishes an old
    standby. No compatibility mode: the upgrade is the cutover.
-   **Evidence:** `test_fail_closed` `legacy_primary`, `legacy_standby`,
    `legacy_stale` (L1-L8), `compat` rewritten against the older build
    (L9); runs, independent review and QA in LEGACY_ROLE.md section 7.
-   **Next action:** review of this slice. Do NOT start S1 before it.

### 2026-11-04 UTC --- Claude (Claude Code): reference architecture research lane

-   **Classification:** KEEP. Documents only; no code or contract change.
-   **What:** cody's research program (Styx, Restate, Orleans,
    SpacetimeDB, DBOS) added as `docs/research/` with a review, a
    teardown template, and SNC CURRENT mapped per template question.
-   **Review notes:** S1 is node-local and need not wait for the Styx
    review; S2/S3 designs should. Use CORE_CONTRACTS' stage names.
    Benchmark DBOS and SpacetimeDB through the CONVENTIONAL.md harness,
    with transfers. Pin versions and check licenses per review.
-   **Next action:** none until a lane is started; the Core order is
    unchanged (legacy-role review, then S1).

### 2026-11-04 UTC --- Claude (Claude Code): experimental validation milestone and scope freeze

-   **Classification:** KEEP. Roadmap and documents only. There is no
    code or contract change; the one new script records the machine.
-   **What:** cody's validation roadmap is filed as
    `docs/EXPERIMENTAL_VALIDATION.md`; the source text is in
    `docs/handoff/`. STATUS.md names the milestone: planned after S1
    and the required correctness fixes.
    - **Major platform expansion after S1 is conditional** on the
      milestone's GO / REFINE / NARROW / STOP decision. Documents,
      Events/Timers, WaitSets, Activation, the Agent Runtime, a Rust
      rewrite, DPDK/VPP and the rest are frozen until then. Exceptions:
      work a validation workload needs, and fixes for defects that would
      invalidate an experiment.
    - The research lane now states that it feeds the comparative
      workload design.
    - Added `bench/manifest.sh`, `bench/manifests/` (the development
      VM's manifest; the main local server and the remote-site host are
      still to capture), and rules for raw results under `bench/results/`.
-   **Review notes:**
    - Failure criteria are committed per campaign
      (`bench/results/<campaign>/CRITERIA.md`) before its comparative
      runs. No thresholds are proposed; they are for cody and Codex at
      the Experimental Baseline.
    - Loopback numbers from the 4-vCPU VM are regression checks only:
      multi-process runs there are CPU-oversubscribed.
    - Gaps: there are no SpacetimeDB, Orleans, Restate or DBOS
      harnesses, and no drivers for workloads B and C (section 5 table).
-   **Next action:** S1 continues unchanged and returns for review. The
    milestone starts after that review and the correctness fixes it
    requires.

### 2026-11-04 UTC --- Claude (Claude Code): S1 bounded destination staging (implementation)

-   **Classification:** REFINE. Migration staging now executes in bounded
    units, with no wire, format, placement or durable-state change.
-   **What:**
    - Every STAGE, final or not, and its reply report run as a volatile
      chain of units on B's shard. Each unit is at most
      `--move-unit-us` (1 ms) or `--move-unit-bytes` (1 MiB) plus one
      object.
    - Guards run between units: fence, token, placement, mutation and
      rehash.
    - A window (8 MiB) bounds the not-yet-durable records.
    - The driver resends after `E_OVERLOADED` or a timeout, with a
      `MOVE ROLE` probe, instead of HAVE/EXPORT loops.
    - `--move-units off` is the one-task rollback.
    - `DEBUG MOVE-STOP` gives deterministic stop and crash points.
-   **Evidence:** MIGRATION_STAGING.md section 18, EVIDENCE.md section
    14, `bench/results/s1-20261104/`.
    - At 100K, B's longest unit falls from 2979 ms to 3.8 ms.
    - The costs: total move +45%; `E_MIGRATING` +37% (shared) and +10%
      (unique).
    - The shared-key index cost (about 0.2 ms per put) is a separate
      primitive; it was not optimized.
    - Tests: `test_stage_chain`, `test_s1` (S1-1..S1-18), plus
      `test_fail_closed` and `test_migrate` unchanged.
    - Full suites: RelWithDebInfo 22/22. ASan+UBSan and TSan 21/22 at
      first: `test_s1` health hit the driver's 60 s deadline (its copy
      ahead of 10,000 objects at `--chunk 4` is 3-10x slower under
      sanitizers). It now uses 2,000 objects in sanitizer builds, and
      `test_s1` then passed in all three builds, with no sanitizer
      reports.
-   **Departures from section 17:**
    - non-final stages are chained (no `E_BAD_ARGS` cap);
    - the D3 report is chained, with its content unchanged;
    - the driver resend also covers non-final stages;
    - 3 runs per matrix cell, not 5.
-   **Residuals kept:** the legacy swapped-labels case; no-control-plane
    adoption is only an operator assertion; no WAL-position heuristics.
-   **Next action:** wait for cody/Codex review. S2, S3, R08 and R11 are
    not started. The experimental validation milestone follows this
    review and any correctness fixes it requires.

### 2026-11-04 UTC --- Claude (Claude Code): experimental baseline readiness review (design and triage only)

-   **Classification:** KEEP. Documents only; no code or contract change.
-   **What:**
    - `docs/EXPERIMENTAL_BASELINE.md`, after cody's approval of PRs #43
      and #44 (`docs/handoff/PR44_S1_and_Experimental_Baseline_Handoff.txt`):
      - candidate baseline `fd0ba42`;
      - profiles A (LOCAL_DURABLE), B (SYNC HA), C (memory ceiling) and
        an optional D (async, manual failover), each with its OK,
        survival, replication, failover, retry, read and migration
        contract;
      - a known-issue triage per profile;
      - pre-baseline tests (T-Q1..T-Q10, T-H);
      - the R08 and R11 decisions;
      - the hardware plan (main and remote Dell R640s, VPS fabric);
      - pre-registered GO/REFINE/NARROW/STOP rules;
      - the freeze requirements;
      - the claims the baseline may not make.
    - INTERACTION_MATRIX: row 20 is TESTED (PRs #40 and #44); rows 3,
      22 and 26 are refined.
    - EXPERIMENTAL_VALIDATION and the manifests README carry the R640
      hosts.
-   **Proposed decisions:**
    - **R08:** EXCLUSION. Profile B is sync only; async only as Profile D
      without witnesses. F-a is deferred.
    - **R11:** not a blocker. The shipped client resends in-doubt
      requests only within about 35 s of a failure, against a 3600 s
      TTL (left at its default). Test T-Q1 must prove that horizon, and
      long-idle retries are excluded from claims.
-   **Found:**
    - No secondary-index query on the wire (X1, a workload-A gap).
      Recommended: a small wire kind, as its own slice.
    - Checkpoint × lost standby (Q3) and blocked sync × memory (Q4)
      become blockers for Profile B failure runs if their tests
      reproduce the hazard.
-   **Next action:** WAIT for cody/Codex review of the document. Implement
    nothing (tests, X1, harness guards) and start no campaign until it
    is accepted.

### 2026-11-05 UTC --- Claude (Claude Code): baseline readiness review corrections (PR #45 review)

-   **Classification:** KEEP. Documents only.
-   **What:** cody's review of PR #45 is applied to
    `docs/EXPERIMENTAL_BASELINE.md`; the source is
    `docs/handoff/PR45_Baseline_Review.txt`.
    - **T-Q1..T-Q10 and T-H are mandatory**, with sharper pass
      conditions:
      - T-Q1: largest resend age per path; R11 is a BLOCKER if any path
        nears expiry.
      - T-Q2: no duplicate obligation.
      - T-Q3: DEGRADE blocked by durability is a Profile B BLOCKER.
      - T-Q4: resource growth measured as time series.
      - T-H: one profile checker that rejects undeclared deviations.
    - **X1 approved:** a minimal index-query wire kind.
    - **Reporting:** Profile B intervals are tagged (healthy, degraded,
      failover).
    - **Network:** RTT p50/p95/p99/max.
    - **Pilot:** N is a guard, not a confidence interval.
    - **Materiality has two paths:** 2x strong win, or a multi-dimensional
      win with thresholds fixed from the pilot.
    - **GO** inventories complexity as a cost.
    - **REFINE** is capped at two rescue rounds.
    - **Claims and work order:** the claims list is extended, and the
      11-step order is recorded.
-   **Next action:** implement the pre-baseline tests and guards, then
    X1. Blockers found are characterized and returned, not fixed.

### 2026-11-05 UTC --- Claude (Claude Code): pre-baseline readiness tests (T-Q1..T-Q5, T-Q10, T-H)

-   **Classification:** KEEP (tests, harness and instrumentation). Two
    RECONSIDER items are raised for review, not acted on: B1, B2 and B3.
-   **What:** the pre-baseline tests of `docs/EXPERIMENTAL_BASELINE.md`
    section 5, run as real processes on the development VM. Results and
    evidence: `docs/BASELINE_READINESS.md` and
    `bench/results/baseline-readiness-20261105/`.
    - **Pass:** T-Q1 (seven resend paths; the oldest resend on a failure
      path was 10.2 s), T-Q2 (lost answers across a slot move, replayed by
      the source, exact), T-Q4 (growth bounded while blocked; it drains
      at the grant), T-Q10 (5 move steps, checkpoint then restart) and T-H.
    - **BLOCKER B1 (T-Q3, matrix row 13):** a checkpoint in flight when
      the standby dies holds the main thread on durability that needs the
      standby, so DEGRADE is never asked. Reproduced 1 of 3, with a gdb
      stack. Proposed fix: the checkpoint waits for local WAL durability
      only.
    - **BLOCKER B2 (T-Q5, matrix rows 5 and 12):** after a failover, an
      old primary rejoining within `--replica-timeout-ms` is promoted by
      the witnesses while still catching up, and every acknowledged write
      is lost. Reproduced 3 of 3. Fails closed when the rejoin is later.
      Proposed fix: a promotion records the term as degraded, and the new
      primary owes SYNCED.
    - **BLOCKER B3 (T-Q5 under TSan, matrix row 12):** `stop_disk()`
      cancels the WAL catch-up reader, but the reader only stops waiting
      for queue space and goes on queuing its whole remaining range. Each
      reconnect during catch-up adds another copy: 2 GiB peak in release
      (control 283 MiB), OOM kill under TSan. Proposed fix: the reader
      stops on `cancel`.
    - **Suites:** release 26/26; ASan 26/26 and TSan (q4) after the q4
      memory criterion allowed for sanitizer shadow memory; TSan
      `q5_kill_kill` stays red until B3 is fixed.
    - **Findings:**
      - F1: retention fills to `--replica-retain-mib` per shard after a
        grant.
      - F2: section 7's 35 s resend bound fails while a primary is
        blocked by a control-plane outage; a restated bound is proposed.
-   **Instrumentation:**
    - the `RESOURCES` admin verb and the `ShardStats` gauges;
    - `net::Client` resend age;
    - `DEBUG DROP-ANSWERS` (`--debug-admin` only).

    Behaviour is unchanged when they are unused.
-   **Tests:** new ctest entries for the passing scenarios; the blocker
    scenarios run by name until their fixes.
-   **Next action:** WAIT for cody and Codex to review B1, B2, B3, F1 and F2.
    X1 (the index-query wire kind) goes ahead as its own slice. No
    blocker fix starts without review.

### 2026-11-05 UTC --- Claude (Claude Code): X1, the INDEX wire kind

-   **Classification:** KEEP (validation-enabling slice, approved as
    option (b) in `docs/EXPERIMENTAL_BASELINE.md` section 4).
-   **What:** wire kind `INDEX = 6` (`include/snc/wire.h`,
    `src/net/server.cpp`, `docs/ROUTING.md`). It calls the unchanged
    `index_lookup`, so the filters are today's: durable only, served
    slots only, not a snapshot. No index redesign and no other protocol
    change.
    - A standby answers E_STANDBY.
    - An error from any shard is answered with no IDs.
    - The sequenced flag and a non-zero `reserved` are refused.
-   **Tests:**
    - `test_net` covers bounds, refusals, a 40-byte key sent out of line,
      and equality with the in-process lookup.
    - `tests/test_index_wire.py` covers `durable` (an applied create hidden
      until its standby acknowledges it), `serving` (each account listed
      once across a slot move) and `compat` (an old server answers
      E_BAD_ARGS and keeps the connection). `compat` needs
      `SNC_OLD_BUILD`; it was run against `dea7ba5`.
    - An independent review found nothing blocking. Its test gaps and doc
      notes are addressed.
-   **Suites:** release 27/27, ASan 27/27, TSan 26/27. The TSan failure is
    `q5_kill_kill`, BLOCKER B3 (`docs/BASELINE_READINESS.md` 3b), which
    is unchanged by X1.
-   **Next action:** WAIT for cody and Codex to review B1, B2, B3, F1, F2
    and X1. No blocker fix, freeze, manifest or campaign work starts
    without review.

### 2026-11-05 UTC --- Claude (Claude Code): B3, B1, B2 blocker fixes (work order PR47_PR48_Blocker_Fix_Handoff)

-   **Classification:** REFINE (fixes inside the existing design). B2
    stays narrow: no witness state or Paxos format change, and no R08
    async policy.
-   **What:** `docs/BASELINE_READINESS.md` section 10 has the full record.
    - **B3.** A cancelled catch-up reader stops reading, allocating and
      queueing, and stops scanning (`read_stream_range` takes a stop
      predicate). Gauges are in `RESOURCES`.
    - **B3 (TSan).** The ack timeout runs only while the standby owes an
      ack for a frame it has begun to get. Neither the primary's own
      reader nor its pacing counts. This also fixes F5 (catch-up rate floor).
    - **B1.** A checkpoint waits only for local WAL durability. The client
      ack contract is unchanged.
    - **B2.** Readiness is recorded and proven: `degraded` means NOT
      READY, and only SYNCED from the current term clears it
      (`docs/SYNC_READINESS.md`).
      - Protocol additions: seed status 3 "retry", and the witness reply
        "NO not learned yet".
      - The receiver answers the handshake only once its position is
        durable.
    - **F2/R11.** The contract is stated in EXPERIMENTAL_BASELINE section
      7, and `net_bench --retry-claim-ttl-s` enforces it per run.
-   **Tests:**
    - New: `test_sync_readiness` (work-order tests 1 to 10),
      `b3_stalled_catchup`, `r11_guard`, `q5_catchup_race`, and the f19
      log order.
    - Existing tests wait for READY before killing a primary.
    - Final suites: release, ASan+UBSan and TSan each 28/28, zero
      sanitizer reports.
    - Evidence for every T-Q scenario is in
      `bench/results/blocker-fixes-20261105/rel/`.
    - An independent source review found two B2 holes, both fixed.
-   **Open:** a delayed same-term SYNCED can land after a later DEGRADE
    grant (proposed `OK v=N` and `SYNCED ... after=N`; not coded, needs
    a witness protocol change). F6: no back-pressure in degraded mode. F1
    is to be measured on the R640.
-   **Also on this branch:** cody's and Codex's future architecture vision
    v2 is filed with a review (`docs/FUTURE_ARCHITECTURE.md`). It
    authorizes nothing.
-   **Next action:** WAIT for cody and Codex to review. No freeze, no
    benchmark campaign, and none of the not-authorized items (R08 async
    auto-failover, session incarnation, S2/S3, index redesign) start
    without review.

### 2026-11-05 UTC --- Claude (Claude Code): delayed-SYNCED causal order (work order PR49_Review_Handoff)

-   **Classification:** REFINE. A request/reply protocol change inside the
    existing witness HA state: no new durable witness or Paxos format, no
    per-group state machine redesign. Paxos only gains "propose at a fresh
    version" and "report the version chosen".
-   **What:** `docs/SYNC_READINESS.md` section 7 (rule and the ten
    questions), `docs/BASELINE_READINESS.md` section 11 (record).
    - Every NOT READY (`DEGRADE`, new `NOTREADY`) is chosen at a fresh
      version and answered `OK v=N`.
    - `READY uuid term group after=N` is chosen only if the group is still
      NOT READY at exactly version N; otherwise `NO not ready since v=M`.
      `SYNCED` is refused. READY reads linearizably first.
    - The primary keeps the last NOT READY version it was answered and
      never accepts an older one.
    - `snc_witness --debug 1` adds `DEBUG HOLD/RELEASE/HELD/SKIP-COMMIT`
      for the forced races; off by default.
-   **Evidence:** `bench/results/ready-order-20261105/`: pre-fix
    characterization (READY restored over a grant, 2,000/2,000 accounts
    lost), V1 to V10 and regressions on 5afa86b, full suites.
-   **Reviews:** the first independent review found an adoption hole
    (a fresh NOT READY answering an adopted version), fixed. The second
    found a stale-read fallback, fixed, and reported F7 (liveness) and F8
    (ballots not persisted, pre-existing), not fixed.
-   **Compatibility:** new nodes with old witnesses never become READY;
    old nodes with new witnesses get DEGRADE but never READY. Fails safe
    (unavailable for failover). Upgrade witnesses with the nodes.
-   **Next action:** WAIT for cody and Codex to review. Then the final
    experimental baseline freeze review. No comparative benchmarks; none
    of R08, F6, F1, session incarnation, S2/S3 or the vision items start.

### 2026-11-05 UTC --- Claude (Claude Code): F8 Paxos ballot characterization (work order PR50_Review_Handoff)

-   **Classification:** KEEP (Paxos as built), plus a new finding F9 for
    review. No protocol or state format change. Test hooks only:
    `snc_witness --debug 1` gains `DEBUG CLOCK-OFFSET-MS`, `SET-COUNTER`,
    `NEXT-BALLOT` and `PROPOSE debug/...`, and
    `--debug-clock-offset-ms`.
-   **What:** `docs/PAXOS_BALLOTS.md` covers ballot construction,
    persistence, scope, the proof, the tests, F9 and the designs.
-   **Found:**
    - F8: a restarted witness reuses ballots (C1), but each acceptor
      promises an (instance, ballot) at most once, durably, so no second
      value is chosen (C2a, C2b, C2l). Proposed outcome A.
    - F9: an acceptor that loses its state (state file deleted or
      restored, restarted in place) lets two values be chosen in one
      instance (C4a, C4b). Proposed outcome B: exclusion E1, guards G1
      (`--new-state`) and G2 (harness).
    - The real fix for F9 is a recovering-acceptor protocol that needs
      a new durable state line. It is design only and returned before
      any format change.
-   **Evidence:** `bench/results/paxos-ballots-20261105/`.
-   **Review:** an independent review found no counterexample to the
    proof. It corrected the F9 recovery design's argument (read the
    maximum from a majority of the *other* witnesses). It reported F10
    (all-digit machine names in a state file load as `ha/0`), F11
    (duplicate `--peers` not rejected) and an ignored directory fsync
    failure. All are documented and none fixed.
-   **Next action:** WAIT for cody's and Codex's review of the F8/F9
    disposition. No freeze, no benchmarks, no format change.

### 2026-11-05 UTC --- Claude (Claude Code): witness hardening before the baseline freeze (work order PR51_Review_Handoff)

-   **Classification:** REFINE (witness start-up, configuration checks
    and persistence error handling). No protocol, wire or state-file
    format change. KEEP Paxos as built (F8 outcome A approved).
-   **What:** G1 (`snc_witness --new-state 1` for first starts only;
    missing state refused), G2 (campaign guard in `bench/profile_check.py`
    and `bench/lab/lab.py`: first-start rules, checks at campaign
    boundaries of state file, directory identity, run nonce and
    per-machine Paxos marks; a violation stops and invalidates the run;
    not a continuous monitor), F10 (legacy files only by shape; strict
    parsing; READ creates nothing), F11 (normalized unique peers, `--id`
    names this machine's own endpoint), every save step fatal including
    the directory's open and fsync (`DEBUG FAIL-PERSIST` test hook).
-   **Tests:** `tests/test_witness_hardening.py` (H1-H9, H12),
    `tests/test_profile_check.py` (H10), existing suites for H11, H13,
    H14. Every witness launch in tests and `bench/` passes
    `--new-state 1` on first start (`tests/witness_state.py`).
-   **Docs:** PAXOS_BALLOTS section 9 (dispositions, C2x assumption,
    invalid histories, backup/restore semantics, hypervisor snapshot
    warning, F9 not solved for enterprise), EXPERIMENTAL_BASELINE
    Profile B witness requirements and claims, BASELINE_READINESS
    section 12, STATUS, CLUSTER.md.
-   **Evidence:** `bench/results/witness-hardening-20261105/`.
-   **Next action:** WAIT for the FINAL EXPERIMENTAL BASELINE FREEZE
    REVIEW. No recovering-acceptor work, no benchmarks, no freeze.

### 2026-11-05 UTC --- Claude (Claude Code): PR #52 review corrections (work order PR52_Review_Handoff)

-   **Classification:** REFINE (campaign harness and tests only). No
    production code change.
-   **What:**
    - G2 now checks every witness's state at the lab's campaign
      boundaries: before each fault, after each heal, after each load
      phase, before the final check, and at the end. Each check advances
      the recorded marks.
    - A violation stops the run at that boundary and invalidates it.
    - It is documented as a point check, not a continuous monitor.
-   **Tests:** `tests/test_lab_witness_guard.py` (loopback Profile B p3:
    clean, and a violation that stops the run before its fault), and
    `witness_state_boundaries` in `test_profile_check`.
-   **Found:**
    - Old test directories in `/tmp` filled the disk during a full suite,
      and b3 failed. `test_transfer`'s crash test leaked about 250 MB a
      run; it now cleans up.
    - b3 now waits for READY before its failover.
-   **Evidence:** a clean full Release run, 32 / 32, with
    `SNC_OLD_BUILD` set correctly from the start
    (`bench/results/witness-hardening-20261105/ctest-release-clean.log`);
    the earlier runs are kept in `history/`.
-   **Next action:** WAIT for the FINAL EXPERIMENTAL BASELINE FREEZE
    REVIEW. No benchmarks, no freeze.

### 2026-11-06 UTC --- Claude (Claude Code): campaign preparation, step 1 (work order PR53_Review_Handoff)

-   **Classification:** KEEP. Docs and benchmark tooling only.
-   **What:**
    - Status documents reconciled: PRs #49 to #53 approved; the
      correctness gate is clear; the full Experimental Baseline is not
      frozen.
    - `bench/manifest.sh` now records every item of the work order's
      manifest list: NUMA memory, huge pages, frequency and idle
      states, kernel knobs, BIOS and DIMMs (root), per-disk write
      cache and scheduler, `nvme list`, RSS, channels, IRQ affinity,
      the build environment, and a topology section filled in by hand.
    - `docs/CAMPAIGN_PREP.md` holds the R640 manifest procedure, the
      first-wave gap inventory (G1 to G13), the minimal plan CP-1 and
      decision D1 (SpacetimeDB in the first wave or not).
-   **Not done:** the main R640's manifest. It must be measured on the
    host, which this environment cannot reach; cody runs the script.
-   **Next action:** WAIT for the R640 manifest, the D1 decision and
    authorization of CP-1. No pilot, no comparative benchmarks.

### 2026-11-06 UTC --- Claude (Claude Code): campaign plan corrections (work order PR54_Review_Handoff)

-   **Classification:** KEEP. Documentation only.
-   **What:** `docs/CAMPAIGN_PREP.md` corrected:
    - The pilot is SNC-only; no competitor numbers come before
      `CRITERIA.md`.
    - CP-1 is split into CP-1A (the SNC pilot harness) and CP-1B (the
      comparative harnesses, built after the pilot).
    - SpacetimeDB stays in workload A's comparative set. Its harness is
      required before `CRITERIA.md`, not before the pilot.
    - Added: resource metric boundaries, NUMA-aware allocation, the
      Redis index decision (A, B or C) and a count-only complexity
      inventory.
-   **Open point for review:** EXPERIMENTAL_BASELINE 10.1 computes N
    from both systems' pilot CVs. With an SNC-only pilot, `CRITERIA.md`
    must pre-register how a competitor's CV enters N (proposal in
    CAMPAIGN_PREP section 2).
-   **Next action:** STOP until the R640 is available for its untouched
    manifest. CP-1A is not authorized yet; no pilot, no comparative
    benchmarks.

### 2026-11-06 UTC --- Claude (Claude Code): N and p99 methodology reconciled (work order PR55_Review_Handoff)

-   **Classification:** REFINE (documentation only).
-   **What:**
    - EXPERIMENTAL_BASELINE 10.1 and 10.2, and CAMPAIGN_PREP, now
      agree: the pilot is SNC-only.
    - N = max(3 × max(CV_throughput_SNC, CV_p99_SNC), N_MIN).
    - Competitor noise is governed by a pre-registered V_MAX: a noisy
      cell is invalid and is rerun, and N never moves.
    - The p99 objective comes from an SNC-only load sweep, by a knee
      rule declared before the sweep.
    - Every final value is set in `CRITERIA.md` before any competitor
      run. No value was chosen.
-   **Next action:** STOP until the R640 is booted and its untouched
    manifest captured. No code, no CP-1A, no pilot.

### 2026-11-06 UTC --- Claude (Claude Code): PR #56 review filed (handoff/PR56_Review_Handoff.txt)

-   **Classification:** REFINE (wording only).
-   **What:** PR #56 approved.
    - A competitor CV at or below V_MAX now reads "the variance criterion
      is satisfied", not "the cell is valid". Other invalidity reasons
      are listed.
    - The knee rule must be an exact algorithm, defined before the R640
      sweep.
-   **Next action:** STOP until the R640 untouched manifest. Then run
    the manifest as root before any tuning, fill the network topology,
    review the allocation, and only then is CP-1A authorized.

### 2026-11-06 UTC --- Claude (Claude Code): main R640 untouched manifest and topology review

-   **Classification:** KEEP (evidence). The allocation is PROPOSED only.
-   **What:**
    - `bench/manifests/r640-main-20261106.txt` was captured on the host
      by a local Claude Code session at 58c638f, before any tuning, and
      relayed by cody.
    - docs/R640_TOPOLOGY.md records what the host is: 2 × Gold 6150,
      SMT on, even/odd NUMA split, 320 GB (10 × 32 GB DDR4-2400), no
      NVMe. Data sits on the BOSS SATA card with the OS. The NICs are
      X550 10GBASE-T, but eno1 is linked at 1 GbE on a shared LAN.
      There is no OS cpufreq driver (the BIOS controls frequency).
    - It also lists the gaps R-G1 to R-G4, a proposed node-0 pilot
      allocation, and decisions for cody (storage, BIOS profile,
      network).
-   **Next action:** review the allocation and the decisions. CP-1A is
    NOT authorized.

### 2026-11-06 UTC --- Claude (Claude Code): R640 manifest b (data SSD, 10 GbE)

-   **Classification:** KEEP (evidence).
-   **What:**
    - cody added a dedicated Samsung 3.84 TB 12 Gb/s SAS SSD on the
      SAS3008 HBA (node 0). It is ext4 with default options at
      /srv/snc-data, WCE=1, fua=1.
    - eno1 is now on a 10 GbE switch and links at 10 Gb/s.
    - `bench/manifests/r640-main-20261106b.txt` was re-captured, with no
      tuning. R-G4 is closed: all storage and NIC controllers are on
      node 0. R640_TOPOLOGY's storage decision is settled.
-   **Next action:** review the allocation. Open gaps: R-G1 (BIOS
    profile), R-G2 (DIMM slots). The pilot stays loopback-only (one
    host). CP-1A is NOT authorized.


### 2026-11-06 UTC --- Claude (Claude Code): CP-1A corrected per the PR #61 review

-   **Classification:** REFINE (benchmark tooling and pilot method);
    KEEP (manifest c, storage).
-   **What:** review filed as `docs/handoff/PR61_Review_Handoff.txt`. On PR #61's branch:
    - `net_bench --rate R` is the open loop (`bench/rate_schedule.h`):
      - an exact periodic schedule per thread;
      - latency from the intended send time;
      - owed work waits for a slot and is never dropped;
      - it reports offered, achieved, backlog, lateness and drain.
    - `--mix` gives exact shares of total ops. The draft's nested flags
      had given read 59.85%, deposit 25.65%, transfer 9.50% and index
      5.00%.
    - The load resends a refused create under the same email number.
    - Each kind's latency and the total are reported. The histogram's
      bounds are documented.
    - `bench/pilot.py` is rewritten around `bench/pilot_plan.json`:
      calibration, duration check, the open-loop grid, the exact sustained
      / peak / objective / knee rule, fixed and capacity reps, and the
      shard sweep.
    - The allocations are explicit: PILOT-L is the baseline (all node 0,
      disjoint cores), PILOT-X is cross-socket.
    - Loopback RX and TX are reported apart. Memory is measured after
      settling (RSS and PSS); the peak is separate.
    - A real run refuses a dirty tree, a missing manifest, a governor
      other than performance, or numa_balancing other than 0.
    - `docs/PILOT_PLAN.md` holds the exact plan (PROPOSED).
    - The stopped R640 run is renamed
      `bench/results/INVALID-pilot-r640-20261106-stopped/`.
    - `manifest.sh` prints one line per DIMM slot.
-   **Evidence:**
    - `tests/test_rate_schedule.cpp` checks the schedule arithmetic.
    - `tests/test_net_bench_rate.py` stops the server for 400 ms: open
      loop p99 ~370 ms, closed loop at depth 1 p99 ~65 µs. It also covers
      owed work across the window's end and the undrained case.
    - `tests/test_pilot.py` pins the rule's boundaries.
    - A `--quick` pilot ran end to end on the dev VM (not evidence).
    - Suite logs are in `bench/results/cp1a-devvm/`.
-   **Next action:** the R640 sets governor=performance and captures
    manifest d. cody/Codex review the plan's PROPOSED values (grid, f,
    windows, reps, CV_thr source, total vs per-kind p99). The real pilot
    is NOT authorized.

### 2026-11-06 UTC --- Claude (Claude Code): R640 manifest d

-   **Classification:** KEEP (evidence).
-   **What:** the R640 session set the governor to `performance` and
    captured `bench/manifests/r640-main-20261106d.txt` at 5bcaca2. It
    ran nothing else.
    - All 72 CPUs show `performance`.
    - R-G2 is closed: A1-A5 and B1-B5 are populated, so 5 of 6
      channels per socket (inferred from the slot layout).
    - `numa_balancing` read 1, most likely after a reboot (inferred). The
      runner refuses unless it is 0.
    - R640_TOPOLOGY section 2c records all of this.
-   **Next action:** manifest d ACCEPTED (handoff/PR61_Addendum_ManifestD.txt); its SHA-256 is now
    in pilot_plan.json. Then review PILOT_PLAN. The real pilot is NOT
    authorized.

### 2026-11-06 UTC --- Claude (Claude Code): PR #61 current-head review, C1-C3

-   **Classification:** REFINE (pilot method; review corrections only).
-   **What:** the review is filed as `docs/handoff/PR61_Final_Review_Handoff.txt`.
    It approves the corrected CP-1A, f = 0.70, the grid, the windows,
    the CV sources, manifest d and the evidence. Corrections:
    - **C1:** `net_bench` reports `scheduled_in_window`, the ops due in
      the measured window. The 1% backlog limit is now a share of that,
      not of warm-up plus window.
    - **C2:** total-operation p99 is the primary objective. Per-kind
      p99/p99.9 are diagnostics (PILOT_PLAN, pilot_plan.json).
    - **C3:** a `client_limited_suspect` boundary point (the lowest
      unsustained one) makes the sweep INVALID and aborts the pilot. Above
      the boundary the flag is diagnostic only.
-   **Evidence:** test_pilot (both C3 cases, R_1, errors, the 1 ms edge,
    the C1 boundary) and test_net_bench_rate (`scheduled_in_window`).
    Release, ASan+UBSan and TSan logs are in `bench/results/cp1a-devvm/`.
-   **Next action:** PR #61 may merge once clean. Merging does NOT
    authorize the pilot. The next review is GO or not for the real
    SNC-only R640 pilot.

### 2026-11-07 UTC --- Claude (Claude Code): CP-1A R640 pilot abort, for review

-   **Classification:** KEEP (evidence); no code change.
-   **What:** the real SNC-only R640 pilot ran and aborted under the
    approved rule (S = R_1 = 218,323; R_2 = 436,646 refused 3/3). The
    R640 diagnosis (branches `r640-pilot-cp1a`, `r640-diag-telemetry`,
    not merged) ties the refusals to the checkpoint's unpaced `fdatasync`
    on the WAL's SSD. The telemetry proof stopped on G0/G2. Summary and
    questions: `docs/handoff/CP1A_Pilot_Abort_For_Review.md`.
-   **Next action:** Codex review (step 1, G0/G2, causal cells,
    classification, telemetry branch). No R640 run until then.

### 2026-11-07 UTC --- Claude (Claude Code): PR #62 review filed

-   **Classification:** REFINE (diagnostic method); no code change.
-   **What:** Codex's review is `docs/handoff/PR62_Codex_Review.txt`.
    - Step 1 (2,183,233 w30, `--ids` on `/dev/shm`, 3 reps) approved.
    - G0 drops its zero-refusal clause; G2 becomes a refusal-fraction
      tolerance against B (median +0.01%, any rep at most max(B) + 0.02%).
      Both must be committed before new proof data. G1/G3/G4 unchanged.
    - Causal cells only after step 1 is resolved and revised G0-G4 pass.
    - Mechanism is strongly supported. Two design-gap candidates stay open:
      read admission coupled to durability backpressure, and an unpaced
      checkpoint burst on the WAL's device. Do not fix yet; do not raise
      kEmitBacklog.
    - `r640-diag-telemetry` stays a diagnostic branch, off main.
-   **Next action:** step 1 on the R640, then follow the review's order.
    Return before any SNC optimization or full CP-1A rerun.

### 2026-11-07 UTC --- Claude (Claude Code): step 1 result, benchmark scratch isolation

-   **Classification:** REFINE (benchmark harness; BENCHMARK DEFECT fixed).
-   **What:** R640 step 1 (preregistered at 7977258, result 2a79e80 on
    `r640-diag-telemetry`): with `--ids` on `/dev/shm`, 3 of 3 runs at
    2,183,233 w30 had 0 refusals, `late_over_1ms` 0 and no storage event.
    Under Codex's rule that is ELIMINATED, so a BENCHMARK DEFECT: the
    pilot wrote the 80 MB ids file buffered onto the data SSD just before
    each run. Caveat: the 3 reps ran back to back, while the old B-w30 reps
    each followed a closed-loop rep.
    - `bench/pilot.py` now needs `--scratch DIR` for a real pilot and
      refuses unless scratch and out are on no physical disk of the data
      directory (partitions, dm/md members, tmpfs via swap; unknown is
      refused). It records the paths and disks.
    - `tests/test_pilot.py`: a fake sysfs covers partitions, dm, tmpfs
      with and without swap, btrfs, loop and missing devices.
    - PILOT_PLAN section 8 states the rule as PROPOSED.
-   **Evidence:** test_pilot passes; a `--quick` run recorded scratch on
    memory and out sharing the data disk (dev VM); a real-mode run refused
    with both isolation problems listed.
-   **Next action:** Codex reviews the fix and the preregistration of the
    corrected harness (order step 2). The B/T proof driver (`diag2.py`, on
    the R640 branch) must apply the same rule before the proof reruns.
    No R640 run until then.

### 2026-11-07 UTC --- Claude (Claude Code): corrected diagnostic harness frozen for review

-   **Classification:** REFINE (benchmark and diagnostic harness).
-   **What:** per `docs/handoff/PR64_Reconciliation_Review.txt`
    (PR #63 approved, PR #64 accepted, benchmark defect confirmed):
    - PILOT_PLAN section 8: scratch isolation is APPROVED and preregistered.
    - main was merged INTO `r640-diag-telemetry` (the branch itself stays
      off main), which gained
      `bench/results/diag3-r640-20261107-corrected/`: `diag3.py` (both arms
      use one isolated `--scratch`; B and T differ only by telemetry),
      PREREG.md (G0 without the zero-refusal clause, plus identity; G2
      refusal-fraction tolerance; G1/G3/G4 unchanged; causal cells only if
      all pass), `test_diag3.py`, VALIDATION.md.
    - The original pilot and the diag2 proof are labelled historical.
-   **Evidence:** test_pilot and test_diag3 pass; quick-run isolation
    metadata and fail-closed refusals are logged (diag3 `validation/`).
    No production C++ change; no R640 load.
-   **Frozen:** `r640-diag-telemetry` at 69d612fa9b4b4ba697575bd001df5b340da22993.
-   **Next action:** review of the frozen harness. Then the corrected B/T
    proof; causal cells follow only if G0-G4 pass. No full pilot,
    competitors, Profile C or engine optimization.

### 2026-11-07 UTC --- Claude (Claude Code): corrected proof and causal cells, for review

-   **Classification:** KEEP (evidence); engine design gap PROPOSED.
-   **What:** Codex gave GO for the corrected B/T proof from 69d612f. The
    R640 ran it once (evidence c88cb70 on `r640-diag-telemetry`; report
    and post-hoc phase analysis at c9537dd,
    `bench/results/diag3-r640-20261107-corrected/CAUSAL_REPORT.md`).
    - G0-G4 pass. With scratch isolated, the 2,183,233 w30 cell has 0
      refusals and 0 late ops in both arms. B closed median 4,338,624.
    - Causal cells (T): 436,646 with a checkpoint refused in 3/3 reps
      (2,267-9,455), all during the checkpoint's ~1.2 s fdatasync (WAL
      await 0.05 -> ~12 ms). Without a checkpoint: 0 refusals and 0 late
      ops in 3/3. At 2,183,233 the walk refuses too. No refusal outside
      a checkpoint.
    - Inferred: a clean CP-1A rerun on this engine would again put S at
      R_1.
-   **Next action:** review the report and the classification (Claude
    proposes a REFINE design gap: an unpaced checkpoint, with reads
    refused by the EXEC gate). Return before any engine optimization or
    pilot rerun.

### 2026-11-07 UTC --- Claude (Claude Code): checkpoint/backpressure design, for review

-   **Classification:** REFINE (design only; no code).
-   **What:** `docs/handoff/PR66_Checkpoint_Design_Review.txt` accepted the
    causal proof and classified checkpoint-induced refusal as an ENGINE
    DESIGN GAP. `docs/CHECKPOINT_BACKPRESSURE_DESIGN.md` answers it:
    as-built snapshot/recovery semantics and invariants I1-I5; both
    measured bottlenecks; options A-F compared; preferred A + B1 + C2 + D
    (E switchable, B2 later, F deferred); proof sketch; resource bounds
    (two are unbounded today: `waiters_`, WAL active buffer); adaptive
    AIMD pacing on SNC signals; tests T1-T13; R640 regression; slices
    S0-S6 with switches.
-   **Next action:** design review. No engine implementation, CP-1A
    rerun, competitors or Profile C before it.


### 2026-11-07 UTC --- Claude (Claude Code): checkpoint design revision (DESIGN-R), for review

-   **Classification:** REFINE (design only; no code).
-   **What:** `docs/handoff/PR67_Design_Review.txt` accepted the PR #67
    design as direction with required corrections.
    `docs/CHECKPOINT_BACKPRESSURE_DESIGN.md` revision 2 applies them:
    I1-I5 become the checkpoint correctness contract CC-1..CC-5 with test
    anchors; C2 withdrawn as the chosen mechanism (C1/C2/C3 compared,
    choice at S3-D); numeric budgets and thresholds marked HYPOTHESIS;
    reserve-before-mutate rule traced through `run_exec`; scheduling
    priority mapped onto the as-built loop; controller cut to two driving
    signals with hysteresis and deterministic `step()`; target completion
    time separate from hard maximum age; HEALTH/STATS fields; revised
    R640 bar; tests T14-T18; S0 scope.
-   **Correction:** revision 1 said the WAL is not `O_DIRECT`. Wrong:
    `EngineConfig::wal_direct` defaults true (`snc.h:309`); the
    `false` at `snc.h:202` is a STATS field. The WAL is `O_DIRECT` +
    `RWF_DSYNC` unless the filesystem refuses. R640 evidence does not
    record it; S0 adds it to STATS.
-   **Next action:** DESIGN-R review, then S0 only. No S1-S5, R640 load,
    CP-1A, competitors or Profile C.

### 2026-11-07 UTC --- Claude (Claude Code): S0 observability, for review

-   **Classification:** KEEP (observability only; no behaviour change).
-   **What:** `docs/handoff/PR68_DesignR_Review.txt` approved DESIGN-R and
    authorized S0 only. Added per-shard published gauges
    (`include/snc/gauges.h`: seqlock block written by the shard thread at
    most every 50 µs; readers never submit shard tasks), node checkpoint
    gauges, refusal-cause counters (`refused_followup`, `refused_ring`;
    waiter and WAL-buffer causes stay 0 until S1), waiter, emit and WAL
    buffer current and peak values, actual `wal_direct`, WAL flush EWMA,
    checkpoint phase, durations and bytes, and committed start LSN. These
    are exposed through the new inline admin verb `GAUGES [shard]` and new
    HEALTH keys appended at the end. CC-1..CC-5 were added to
    `docs/CORE_CONTRACTS.md` section 13A. Field semantics are in
    `docs/CHECKPOINT_BACKPRESSURE_DESIGN.md` 11.1.
-   **Evidence:**
    - `test_gauges` (S0-T1..T7) and `test_gauges_admin.py` (S0-T8) pass.
    - Suites, as run (corrected wording per the PR #69 review):
      - Release: first full run 36/37; the fixed `test_gauges` passed
        when rerun on its own.
      - ASan+UBSan: one clean full run, 37/37.
      - TSan: full `-j2` run 34/37 with 0 reports; the three HA timing
        tests then passed one at a time.
      - These are not clean first-pass 37/37 runs. The clean final
        validation is S0.1's (`bench/results/s01-validation-devvm-20261107/`).
      - Raw history: `bench/results/s0-overhead-devvm-20261107/`.
    - The independent review found no behaviour change; three findings
      were fixed.
    - The dev-VM overhead check shows no perturbation beyond about ±7%
      noise.
    - The R640 was not used.
-   **Next action:** S0 review. Do not start S1 automatically.

### 2026-11-07 UTC --- Claude (Claude Code): S0.1 seqlock memory order, for review

-   **Classification:** KEEP (the S0 architecture) / REFINE (the
    publication memory order). Narrow fix only: the field list, publish
    cadence, budgets, checkpoint behaviour and controller are unchanged,
    there is no performance tuning, and the R640 was not used.
-   **What:** `docs/handoff/PR69_S0_Review.txt` approved S0 and blocked
    on the seqlock. `ShardGauges` in `include/snc/gauges.h` now uses the
    conventional acquire/release pattern:
    - Writer: odd sequence (release), field stores (release), even
      sequence (release).
    - Reader: first sequence (acquire), retry if odd, field loads
      (acquire), second sequence (acquire); accept only if the two match.
    - The contract and a step-by-step proof are in the header and in
      `docs/CHECKPOINT_BACKPRESSURE_DESIGN.md` 11.1. No `seq_cst`.
    - The S0 evidence wording was corrected in this log's S0 entry and in
      `bench/results/s0-overhead-devvm-20261107/README.md`.
-   **Evidence** (`bench/results/s01-validation-devvm-20261107/`, final
    code 1be6d4a, each a single complete serial `ctest -j1` run):
    - Release: 37/37.
    - ASan+UBSan: 37/37, 0 reports.
    - TSan: 37/37, 0 reports, serial from the start.
    - TSan `test_gauges` alone: T7 has 0 torn snapshots.
    - Independent review of the memory-order contract: APPROVE. Its
      comment-wording findings were applied.
    - Two earlier runs failed and are kept in the raw history: Release
      `-j2` (`test_lab_witness_guard`) and ASan run 1 (`test_ready_order`).
      Both are pre-existing harness races, traced in that README, with
      proposed patches. Neither is fixed here.
-   **Next action:** S0.1 review. S1 is not authorized. No R640, CP-1A or
    competitors.

### 2026-11-07 UTC --- Claude (Claude Code): HA test-harness race fixes, for review

-   **Classification:** KEEP (test-only; no engine, server or witness
    change).
-   **What:** `docs/handoff/PR70_S01_Review.txt` approved S0.1 (S0 is
    complete) and required the two known harness races fixed before S1.
    - `bench/lab/lab.py`: p3 now uses `Lab.wait_primary` (bounded, exactly
      one claimant, `None`-safe, raises with the last roles) instead of
      one `primary()` sample.
    - `tests/test_ready_order.py`: c2 waits (bounded, 30 s) for the new
      process's first READY line after READY is chosen. The grounding rule
      is unchanged and self-tested.
    - `test_lab_witness_guard` gains a deterministic `primary` case.
-   **Evidence** (`bench/results/harness-races-devvm-20261107/`):
    - Each test passes repeatedly: Release ×10, ASan+UBSan ×5, TSan ×5,
      with 0 sanitizer reports.
    - 3 stressed parallel rounds with CPU burners pass.
    - Both races are reproduced deterministically and handled.
    - c2 still fails when its READY line is absent or stale.
    - The full Release suite (`ctest -j1`) passes 37/37.
-   **Next action:** review. S1 design planning may proceed; S1 behaviour
    waits for this review. No R640, CP-1A, competitors or Profile C.

### 2026-11-07 UTC --- Claude (Claude Code): S1 resource-specific admission, for review

-   **Classification:** REFINE (admission). The checkpoint, recovery, WAL
    format and scheduler loop order are unchanged, and so are
    `kEmitBacklog` = 1024 and `E_OVERLOADED`. CC-1..CC-5 are not changed.
-   **What:** `docs/handoff/PR71_Harness_Review.txt` authorized S1.
    `docs/S1_ADMISSION.md` has the admission matrix, the
    reserve-before-mutate proof (R1-R6), the budget derivation, the drain
    argument and the known limits.
    - Owner-local READs that need no waiter skip the follow-up gate.
      Forwarded READs keep it.
    - Creates and deletes are now gated (before S1 they were not).
    - Client mutations, creates and deletes reserve after validation and
      before the first change. The WAL check is the object, plus the
      local peer, plus the session's worst case. The waiter check
      follows it.
    - The budgets are 32 MiB of waiters and 30 MiB of WAL buffer per
      shard. They are provisional, to be revisited before S6.
    - Follow-ups, standby apply, recovery, migration and session expiry
      are never refused.
    - `refused_waiter` and `refused_wal_buffer` now count.
    - `--admission legacy` restores the old gate.
-   **Tests:** `test_s1_admission` A1-A9. The required section-12 items
    are mapped in `docs/S1_ADMISSION.md` section 6.
-   **Review:** two independent source reviews.
    - The first found two major issues (gated session expiry, a vacuous
      A6) and several minor ones. All were fixed.
    - The second APPROVED. Its minor findings are recorded in
      `docs/S1_ADMISSION.md` sections 9 and 10, notably a sub-size-class
      reservation shortfall to fix before S6.
-   **Evidence** (`bench/results/s1-validation-devvm-20261107/`):
    - Release 38/38, ASan+UBSan 38/38 and TSan 38/38, each a complete
      serial run with 0 sanitizer reports.
    - One earlier ASan run was 37/38: `test_baseline_ha` `q4_normal`
      read 270 held answers in its single end sample, against a window
      of 256. It is analysed there; a harness change is proposed but not
      made.
-   **Also filed:** `docs/STATE_NATIVE_COMPUTING_LINEAGE.md`, research
    only. It authorizes nothing.
-   **Next action:** review. No S2, R640 load, CP-1A, competitors or
    Profile C.

### 2026-11-07 UTC --- Claude (Claude Code): S1.1 corrections, for review

-   **Classification:** REFINE (admission). Loop order, checkpoint,
    recovery, WAL format, `kEmitBacklog` = 1024, `E_OVERLOADED` and
    CC-1..CC-5 are unchanged. No protocol field changed.
-   **What:** `docs/handoff/PR72_S1_Review.txt` blockers 1-5, detailed in
    `docs/S1_1_CORRECTIONS.md`.
    - Reservation: option B. `type.max_size` is now a hard cap at
      resize. Before, a full-inbox account could grow past max_size
      within its allocator capacity and the next restart aborted in
      install; that was a real recovery bug, now fixed.
    - Remote reads: a per-origin pending budget (refused before a slot
      exists, `refused_pending`) and owner-side admission of every
      answer-wait not already reserved (`refused_waiter`). Global bound
      in section 2; no deadlock (answers never wait on the origin).
    - Non-refusable follow-up WAL growth is not finitely bounded by the
      client budget. A15 measures 1.6x the budget with two senders; the
      next bound (follow-up intake backpressure) needs a loop change and
      is named for a later slice, not built.
    - A6 tightened to exact counts; q4 uses a preregistered gauge-based
      drain rule with a synthetic self-test (`h1_measurement`).
    - Defaults provisional; 1 GB/s labelled hypothetical.
-   **Tests:** A10-A15 new, A6 rewritten, H1 in ctest. Negative checks
    (cap reverted, owner check removed, pending budget removed) fail as
    expected; logs in the evidence dir.
-   **Review:** one independent source review, APPROVE. Its medium
    finding (drain progress summed over shards) and three low ones were
    fixed before the headline runs; the rest are documented.
-   **Evidence:** `bench/results/s11-validation-devvm-20261107/`.
-   **Next action:** review. No S2, R640 load, CP-1A or competitors.

### 2026-11-07 UTC --- Claude (Claude Code): S1.2 replay waiters, for review

-   **Classification:** REFINE (admission). Loop order, checkpoint,
    recovery, WAL format, wire format (no new field or status),
    `kEmitBacklog` = 1024 and CC-1..CC-5 are unchanged.
-   **What:** `docs/handoff/PR73_S11_Review.txt`; details in
    `docs/S1_2_REPLAY.md`.
    - `replay_if_seen` no longer exempts replays. Every replay answer
      passes `admit_answer_wait`. With no waiter room, an undurable
      replay is `E_OVERLOADED` with `replayed` set (`refused_waiter`,
      plus the new `refused_replay`). Nothing is re-applied.
    - **Client contract change:** a plain `E_OVERLOADED` for a duplicate
      would double-apply with the shipped callers, which reissue under a
      new sequence. `net::Client` therefore resends `E_OVERLOADED` +
      `kRespReplayed` with the same sequence, with backoff, and never
      hands it over. A refusal lasting past the request timeout fails
      over. `docs/RETRIES.md` records the rule. cody chose this
      option (2026-11-07).
    - `waiter_reserved` has one meaning; every assignment is audited.
    - Follow-up intake backpressure is recorded as a required pre-S6
      slice.
-   **Tests:** A16-A21. Negative checks: the exemption restored fails
    A16/A18/A20/A21; the client retry disabled fails A21.
-   **Review:** one independent source review, APPROVE (server proven).
    Its client liveness findings were fixed before the headline runs.
-   **Evidence:** `bench/results/s12-validation-devvm-20261107/`.
-   **Next action:** review. No S2, R640 load, CP-1A or competitors.

### 2026-11-08 UTC --- Claude (Claude Code): S2 paced checkpoint walk, for review

-   **Classification:** REFINE (checkpoint scheduling). CC-1..CC-5
    unchanged (mapping in `docs/S2_PACING.md` section 6). Loop order,
    begin snapshot, writeback, seal, commit, reclaim, recovery, WAL and
    wire formats, admission and `kEmitBacklog` = 1024 are unchanged.
-   **What:** `docs/handoff/PR74_S12_Review.txt` (S1 approved and complete;
    S2 authorized, walk pacing only). Details in `docs/S2_PACING.md`.
    - The walk is time-budgeted per loop iteration: batches of 16 IDs,
      the clock read after each, at least one batch per iteration.
      `--ckpt-pacing off` restores 256 IDs per iteration.
    - A pure controller (`src/core/ckpt_pacer.h`) sets the budget within
      [floor, ceiling] from the WAL flush EWMA against its baseline and
      the follow-up backlog, with hysteresis, and catch-up toward a
      target. It reports past-target, warn and degraded against a hard
      maximum age, without abandoning or committing early.
    - 17 gauges; HEALTH `ckpt_state`, `ckpt_past_target`, `ckpt_catchup`.
    - Defaults are provisional hypotheses. The walk budgets come from a
      dev-VM measurement (0.6-0.8 us per ID); the target and maximum age
      are placeholders until campaign data.
-   **Not done (by the work order):** loop reordering (DESIGN-R 6.3 item 4
    deferred), the writeback window (S3), sync tokens (S4), follow-up
    intake backpressure (pre-S6).
-   **Tests:** P1-P8 (`tests/test_s2_pacing.cpp`).
-   **Evidence:** `bench/results/s2-validation-devvm-20261108/`.
-   **Next action:** review. No S3-D, R640 load, CP-1A or competitors.

### 2026-11-08 UTC --- Claude (Claude Code): S3-D writeback mechanism, for review

-   **Classification:** REFINE (checkpoint I/O), design only. No engine
    code changed.
-   **What:** `docs/handoff/PR75_S2_Review.txt` (S2 approved and complete;
    S3-D authorized). `docs/S3D_WRITEBACK.md` proposes C3: buffered part
    writes with `RWF_DSYNC` per 1 MiB chunk, a window W of chunks in
    flight that back-pressures the walk, and the final `fdatasync` kept.
    C2 (`O_DIRECT`) is the named fallback if the R640 characterization
    shows C3's per-chunk syncs hurt the WAL; C1 is rejected.
    - Dev-VM bench (`bench/ckpt_writeback_char.cpp`): the legacy final
      sync stalls the WAL (p50 0.1-0.6 s for 0.4-1.2 s); all three
      candidates keep dirty bytes at a few MiB and the final sync at about
      a millisecond; during the paced write, C3 had the steadiest WAL p99
      (1.4-2.0 ms over 9 runs; C1 1.5-40, C2 2.6-177).
    - Values (W, chunk size) stay hypotheses.
-   **Evidence:** `bench/results/s3d-devvm-20261108/` (51 runs, 0 failed).
-   **Next action:** review. No S3 implementation, R640 load, CP-1A or
    competitors.

### 2026-11-08 UTC --- Claude (Claude Code): S3 bounded checkpoint writeback (C3), for review

-   **Classification:** REFINE (checkpoint I/O). CC-1..CC-5 unchanged
    (code-level mapping in `docs/S3_WRITEBACK.md` section 7). Part format,
    recovery, WAL, commit order, admission and `kEmitBacklog` unchanged.
-   **What:** `docs/handoff/PR76_S3D_Review.txt` (C3 approved with
    conditions). Part chunks are written with `RWF_DSYNC`; at most W
    (`--ckpt-window`, default 4, a hypothesis) in flight per shard; a full
    window stops the walk; each write owns its buffer and frees it on its
    own completion; the last chunk keeps the walk in WALK until submitted;
    short writes are fatal; synchronous-fallback writes are counted
    (`sri_write` returns whether it queued). Final `fdatasync` kept.
    `--ckpt-io legacy` restores the pre-S3 I/O. 17 gauges; HEALTH
    `ckpt_io`, `ckpt_sync_writes`.
-   **Not done (by the work order):** S4, C2, follow-up intake
    backpressure, B2, R640 load.
-   **Tests:** T1-T11, T13 (`tests/test_s3_writeback.cpp`); T12 = the
    suite (dsync default) and the suite under `SNC_CKPT_IO=legacy`.
-   **Evidence:** `bench/results/s3-validation-devvm-20261108/`.
-   **Next action:** review. No S4, R640 load, CP-1A or competitors.

### 2026-11-08 UTC --- Claude (Claude Code): S3.1 engine lifecycle under checkpoint I/O, for review

-   **Classification:** REFINE (engine lifecycle). CC-1..CC-5, part
    format, recovery, WAL, admission and `kEmitBacklog` unchanged.
-   **What:** `docs/handoff/PR77_S3_Review.txt` (S3 core approved; teardown
    use-after-free a blocker). Traced: the use-after-free was the static
    `EngineState` freed at process exit under live shard threads; shard
    teardown already waited for checkpoint I/O; `stop()` during
    `checkpoint()` abandoned the waiter. Fix: `checkpoint()` holds a
    lifecycle lock that `stop()` takes first (checkpoint finishes, then
    stop); the Engine owns its state. Test hook for the final sync.
-   **Tests:** S3-T14..T18 (`tests/test_s3_writeback.cpp`); five negative
    mutations including the runtime drain.
-   **Evidence:** `bench/results/s31-validation-devvm-20261108/`;
    `docs/S3_WRITEBACK.md` section 10.
-   **Open:** other Engine entry points that submit to shards are not
    serialized with `stop()` (out of S3.1 scope, for review).
-   **Next action:** review. No S4, R640 load, CP-1A, competitors or
    follow-up intake slice.

### 2026-11-08 UTC --- Claude (Claude Code): S3.2 Engine lifecycle gate, for review

-   **Classification:** REFINE (engine lifecycle). Checkpoint semantics,
    persistence format, admission, placement semantics and `kEmitBacklog`
    unchanged; no S4, no follow-up intake.
-   **What:** `docs/handoff/PR78_S31_Review.txt` (S3.1 approved; every other
    shard-submitting Engine method could race `stop()`). One counting
    permit (`Engine::Op`) taken by each category-2 method; `stop()` moves to
    STOPPING, admits nothing new and waits for the count to reach zero;
    explicit states CONSTRUCTED, STARTING, RUNNING, STOPPING, STOPPED,
    FAILED; restart refused; nesting tracked per thread without recursive
    locks; `require_op()` aborts on any shard submission outside a permit.
-   **Review:** an independent read-only review found 9 items; 5 fixed
    (FAILED state, cross-engine nesting, stop from a shard thread, the
    negative mutation's catch reason, L3's sleep), the rest accepted or
    left for the reviewer (`runtime()` stays ungated; a pre-existing
    `cfg_.standby` race in promotion). Table: `docs/ENGINE_LIFECYCLE.md`
    section 8.
-   **Tests:** L1-L11 (`tests/test_lifecycle.cpp`); eight negative
    mutations.
-   **Evidence:** `bench/results/s32-validation-devvm-20261108/`.
-   **Next action:** review. Decision needed: gate `runtime()` or accept
    the exemption. No S4, R640 load, CP-1A, competitors or follow-up
    intake slice.

### 2026-11-08 UTC --- Claude (Claude Code): S3.3 data-plane lease and durable control, for review

-   **Classification:** REFINE (engine lifecycle and control state).
    Checkpoint and promotion steps, superblock format, seed protocol,
    admission, placement semantics and `kEmitBacklog` unchanged.
-   **What:** `docs/handoff/PR79_S32_Review.txt` (S3.2 approved; blockers:
    the raw `runtime()` handle, promote vs checkpoint).
    - `runtime()` is removed. `Engine::DataPlane`, a counted lease that
      `stop()` waits for, is now the only outside path to shard work.
      `RuntimeView` gives read-only facts. Callers were migrated
      mechanically.
    - `control_mu_` covers the whole of `checkpoint()` and `promote()`, so
      the two linearize.
    - The runtime role is an atomic; `cfg_` is no longer mutated.
    - `term()` and `lease_block()` read a published view.
    - `CheckpointReport` gains `standby` and `term`.
-   **Review:** independent review, no blockers. Four should-fix items
    fixed or mitigated, six notes handled
    (`docs/ENGINE_LIFECYCLE.md` section 11).
-   **Tests:** R1-R3, C1-C4 (`tests/test_lifecycle.cpp`); five negative
    mutations.
-   **Evidence:** `bench/results/s33-validation-devvm-20261108/`.
-   **Next action:** review; S3 closes if clean. No S4, R640 load,
    CP-1A, competitors or follow-up intake slice.

### 2026-11-09 UTC --- Claude (Claude Code): S4 staggered final checkpoint sync, for review

-   **Classification:** REFINE (checkpoint final-sync scheduling).
    What is written, chunk size, window, `RWF_DSYNC`, phases, formats,
    recovery, manifest and superblock, commit rule, S1, S2, lifecycle,
    replication, promotion and `kEmitBacklog` unchanged.
-   **What:** `docs/handoff/PR80_S33_Review_S4_Authorization.txt` (S3
    complete; S4 authorized, only S4).
    - A ready part takes a ticket from the node's `SyncTokens` and syncs
      once its ticket is inside the window of K (`ticket < releases + K`);
      until then its step returns and the shard keeps serving.
    - The sync's completion returns the token first, before the fatal
      path.
    - `Config::ckpt_sync_tokens` / `--ckpt-sync-tokens`, default 0 =
      unlimited (the S3 path). K is a hypothesis; none is chosen.
    - Gauges (`ckpt_sync_state` 0/1/2, token waits, ticket), node counts
      (`Engine::sync_tokens()`), HEALTH `ckpt_sync_*`.
-   **Review:** independent review found no engine bug; a test race,
    overclaiming comments and env parsing fixed
    (`docs/S4_SYNC_TOKENS.md`).
-   **Tests:** S4-T1..T9, TC (`tests/test_s4_sync_tokens.cpp`); six
    negative mutations (T10).
-   **Evidence:** `bench/results/s4-validation-devvm-20261109/`.
-   **Next action:** review. No S5/S6, R640, CP-1A, comparators or
    follow-up intake slice.

### 2026-11-09 UTC --- Claude (Claude Code): S4.1 admin reply delivery and fairness wording, for review

-   **Classification:** REFINE (admin loop output; S4 documentation).
    No engine, token, checkpoint, format, recovery, lifecycle or
    replication change.
-   **What:** `docs/handoff/PR81_S4_Review.txt` (S4 core approved; S4.1
    required).
    - Admin replies now finish on POLLOUT. They are bounded by a write
      deadline and `kOutMax`, and a cut is visible as a missing newline.
      HEALTH no longer waits on a slow GAUGES reader.
    - The "granted in ticket order" claim is corrected to ordered
      eligibility plus eventual acquisition.
    - `waiting_peak` is clarified.
-   **Tests:** A1-A4 (`tests/test_admin_loop.cpp`); S4 T1-T9 unchanged
    and green.
-   **Evidence:** `bench/results/s41-validation-devvm-20261109/` (README):
    serial Release 43/43, ASan+UBSan 43/43 (run2; run1 cut by a task time
    limit, kept as invalid), TSan 43/43, zero sanitizer reports; negatives
    4 of 4 caught. The independent review's findings (A1 under TSan and
    four small ones) were fixed before these runs.
-   **Next action:** review. Then the follow-up intake slice if
    authorized. No S5/S6, R640, CP-1A or comparators.


### 2026-11-09 UTC --- Claude (Claude Code): follow-up intake backpressure design note, for review

-   **Classification:** design only (no code). Proposes a REFINE of the
    follow-up apply path; RECONSIDER options are listed, not chosen.
-   **What:** `docs/handoff/PR82_S41_Review.txt` approved PR #82, so S4
    is COMPLETE, and it authorized the design only.
    `docs/FOLLOWUP_INTAKE_DESIGN.md` recommends option D:
    - rings always drain;
    - the apply of follow-ups is gated on the shard's own WAL active
      bytes against a cap H;
    - per-pair credits (W) are counted when an item leaves staging;
    - node-link reads pause at L held items.
    It gives proofs B1-B7, tests F1-F9 and the negatives.
-   **Open for review:**
    - Q1, the derived-backlog policy (R1 recommended; a gate on the
      shard's own backlog deadlocks, section 7.1);
    - Q2, a limit on emits per op;
    - Q3, the defaults;
    - Q4, a `u32` WAL active-buffer doubling that wraps past 2 GiB
      (`storage.cpp:566-575`; also reachable through a standby's
      unbounded `append_raw`; proposed as its own small fix);
    - Q5, an owner-side follow-up gate for forwarded mutations (closes a
      B2 gap that exists today).
-   **Review:** an independent review found 3 HIGH, 6 MEDIUM and LOW
    items; all are corrected in the note (section 12).
-   **Next action:** review. No runtime, ring or drain change until then;
    no S5/S6, R640, CP-1A or comparators.

### 2026-11-09 UTC --- Claude (Claude Code): WalStream active-buffer overflow fix

-   **Classification:** REFINE (a storage fail-stop; formats unchanged).
-   **What:** this is the first step of `docs/handoff/PR83_Design_Review.txt`
    (option D approved as the base architecture; R1 rejected).
    `WalStream::ensure_capacity` doubled a u32 capacity, which wrapped
    past 2 GiB. Now:
    - growth is checked in u64 (`grown_capacity`);
    - `WalConfig::max_active_bytes` is explicit (default and hard maximum
      1 GiB);
    - `append` and `append_raw` fail stop before any state change.
-   **Tests:** `test_wal_capacity` C1-C5; negatives 5 of 5. Serial
    Release, ASan+UBSan and TSan all 44/44, with zero reports
    (`bench/results/walcap-validation-devvm-20261109/`).
-   **Known limit:** a standby has no receive-side bound; it now fails
    stop at 1 GiB.
-   **Next action:** FOLLOWUP_INTAKE_DESIGN rev2, then review.

### 2026-11-09 UTC --- Claude (Claude Code): follow-up intake design rev2, for review

-   **Classification:** design only (no code). REFINE of option D, as
    approved in `docs/handoff/PR83_Design_Review.txt`.
-   **What:** `docs/FOLLOWUP_INTAKE_DESIGN.md` revision 2:
    - R1 is dropped, as the review rejected it;
    - the reply-only contract is specified as enforced: a follow-up may
      emit only to its source, and at most once;
    - `OpDescriptor::max_emits` is enforced (default 0), with Account's
      actual maxima declared;
    - an origin **exchange budget X**, checked only at client admission
      (refused, never queued), makes the sender bound B2 structural
      intra-node by conservation (section 6.2);
    - D is kept: drain always, H = max(2B, B+R) with H >= B+R hard, W,
      the owner gate (now also X) and link pausing L;
    - cross-node end frames are specified as sub-slice D.2, a wire
      change.
-   **Open for review:** Q1 resume pacing by X; Q2 build D.1 then D.2;
    Q3 defaults (all hypotheses); Q4 a declared follow-up flag on ops;
    Q5 approve X, a new client-visible gate beyond the approved D.
-   **Review:** an independent review found 1 HIGH (missing drop paths
    in the exit table, which would leak the budget), 6 MEDIUM and 4 LOW;
    all are corrected in the note (section 13.1).
-   **Next action:** review. No implementation of D; no S5, S6, R640,
    CP-1A, comparators or B2 snapshot work.

### 2026-11-09 UTC --- Claude (Claude Code): follow-up intake design rev2.1 (review conditions)

-   **Classification:** design only. REFINE.
-   **What:** cody approved rev2 with conditions
    (`docs/handoff/PR85_Design_Review.txt`; Q1-Q5 yes, Q3 as hypotheses).
    `docs/FOLLOWUP_INTAKE_DESIGN.md` section 3.9 adds them:
    - a per-invocation emit reservation in `validate`
      (`OpCtx::reserve_emits`), which X checks after validation and
      before mutation. Static `max_emits` stays the hard ceiling, and
      `transfer` reserves 0 when `local(to)` succeeds;
    - a coalesced, paged resume: one scan per shard plus owed state,
      paged under X;
    - B2 updated (no resume term), and tests F12-F15.
-   **Next action:** implement D.1, then return it for review. D.2 is a
    separate slice, required before S6.

### 2026-11-09 UTC --- Claude (Claude Code): follow-up intake D.1 (intra-node), implementation

-   **Classification:** implementation of the approved design. KEEP
    (option D, reply-only contract, X, owner gate); REFINE (as-built
    details in `docs/FOLLOWUP_INTAKE_DESIGN.md` 3.9.6).
-   **What:** the H gate per item at apply, the per-pair window W
    returned when an item leaves staging, the exchange budget X checked on
    the invocation's reservation after validate (sequenced replays
    answered before it), the owner gate, link reads paused at L, a
    coalesced paged resume under X, and the contract checks (`max_emits`,
    the `followup` flag, reply-only, `max_resume_emits`).
    `tests/test_intake.cpp`: F1-F8, F10-F15 and G1 (owner gate).
    Older backlog-gate tests run with `exchange_gate` off; A6 with H and
    W off (3.9.6 says why).
-   **Defect found and fixed on site:** link reads removed a connection
    from epoll whenever the per-poll budget ran out, even when not paused,
    and never re-added it; `test_cluster` stalled. Now removed only while
    paused.
-   **Defect found by the independent review and fixed:** the first
    resume pass after start sent re-emits of objects changed before the
    pass as if durable (lsn 0), so a crash could keep a credit and lose
    its debit. Each re-emit now waits for the WAL position when its hook
    ran (F16, negative N15). Also: a link frame to a non-followup op is
    dropped instead of aborting the node (checked at the sender instead);
    the design's claim that `quiesced()` counts a resume scan corrected
    (it does not; counting it could hang a stop).
-   **Not met in D.1 (D.2 scope):** a sender is not bounded across nodes
    (its exchange ends at the link). P1 performance not run.
-   **Test defect found by TSan and fixed:** F11 compared two shards'
    stats read in the wrong order (x before d), so d could end exchanges
    between the reads; reading d first is sound. The failed CHECK then
    ran exit() with engine threads live, which crashed a shard and hung
    TSan; test_intake now uses _Exit on a failed check and a 2400 s ctest
    timeout.
-   **Evidence:** `bench/results/intake-d1-validation-devvm-20261109/`
    (README lists every run in order). Headline: Release run 4 45/45,
    ASan+UBSan run 1 45/45 with 0 reports, TSan run 3 45/45 with 0
    warnings, negatives 12/12. Release runs 1-3, TSan runs 1-2 kept with
    their causes (disk allowance; witness "primary up" logging race,
    proposed as a separate fix; F11; a container restart).
-   **Next action:** WAIT for the D.1 review. No D.2, S5, S6, R640,
    CP-1A, comparators or B2 snapshot work.

### 2026-11-10 UTC --- Claude (Claude Code): D.1 closure PR (PR #87 review)

-   **Review:** the D.1 core was APPROVED and D.1's completion was held
    on three items (`docs/handoff/PR87_D1_Review.txt`). Classification:
    KEEP (D.1 as approved); REFINE (operability, a witness logging fix).
-   **Witness fix (defect found during D.1 validation, fixed on site):**
    `snc_witness` printed "primary up" only from its own proposal. A
    witness that learned the chosen space from a peer's COMMIT first
    printed nothing. It now prints the line once, from whichever path
    sees the space served first. The regression test is `test_health`
    T12 (the witness's node paths cut, then healed); it is
    deterministic: the old witness fails it 3/3, the fixed one passes
    3/3.
-   **Operability:** 33 `intake_*` gauges per shard on the admin
    `GAUGES` line. They give the effective H, R, W, X and L and each
    gate's on/off, plus the exchange, staging, window, link-hold, gate
    and resume state and counters. They are published from existing
    counters and change no decision. Tests: `test_intake` F12, F8 and
    G2; `test_gauges_admin` T8d.
-   **P1:** preregistered before the run, on the dev VM, against
    rollback. 110/110 runs were valid. The default (W 64, X 1024) is
    within rollback's range. X = 256 is about 5% below on uniform
    transfers and refuses 4-5% of cross-shard transfers. No default
    changed (`bench/results/intake-d1-closure-devvm-20261110/p1/REPORT.md`).
-   **Also fixed:** `test_transfer` leaked three /tmp dirs per pass
    (PR #88). The new gauge test read the seqlock once and failed
    under TSan (test only; it now reads until consistent).
-   **Evidence:** `bench/results/intake-d1-closure-devvm-20261110/`
    (README lists every run).
-   **Next action:** WAIT for the closure review. No D.2, S5, S6, R640,
    CP-1A, comparators or B2 snapshot work.

### 2026-11-10 UTC --- Claude (Claude Code): PR #89 review filed; Agent-SNC doc filed

-   **Review:** PR #89 APPROVED; follow-up intake D.1 COMPLETE; D.2
    (cross-node exchange-end accounting, wire change) authorized as the
    next separate reviewed slice, with its acceptance list
    (`docs/handoff/PR89_D1_Closure_Review.md`, verbatim).
-   **Filed:** cody's Agent-SNC v0.1 architecture synthesis, verbatim
    (`docs/handoff/AGENT_SNC_ARCHITECTURE_V0_1.md`). Design input only:
    it authorizes no Agent-SNC, Event Fabric or Rust work.
-   **Next action:** D.2. Still forbidden until D.2 is approved: S6, the
    R640 campaign, a CP-1A rerun, comparators, B2 begin-snapshot work,
    and S5 unless separately justified by evidence.

### 2026-11-10 UTC --- Claude (Claude Code): D.2 design, for review

-   **What:** `docs/CROSS_NODE_EXCHANGE_DESIGN.md`. It is design only, as
    cody chose ("design first"). No D.2 code until it is approved.
-   **Mechanism:** an exchange stays counted at its origin until it ends
    on either node. Data frames keep 64 bytes and gain two origin bits.
    The peer counts what it holds per origin and sends coalesced ENDs.
    After any connection change, the peer's SYNC reports what it
    physically holds and what it received on the origin's current
    connection, and the origin overwrites its count with the exact value.
    Heartbeats and silence detection turn a partition into a drop, and
    release frees a dead peer's count. A pushing shard adopts uncounted
    items. Wire v2 is fail-closed.
-   **KEEP / REFINE / RECONSIDER:**
    -   KEEP the D.1 mechanisms.
    -   REFINE the section 3.6 D.2 sketch: SYNC replaces generations,
        because of flap amplification and restart collisions (2.3).
    -   RECONSIDER nothing.
-   **Acceptance mapping:** sections 4-6, tests X1-X12 (with X7b),
    negatives ND1-ND14.
-   **Pre-review:** an adversarial agent review of the first draft found
    7 issues. They are all fixed and listed in section 12:
    -   F1: a leak on a foreign-link purge;
    -   F2: a fenced primary's second incarnation, now counted per
        incarnation;
    -   F3: silence detection versus read pausing;
    -   F4: nonce resets;
    -   F5: double-counting wording;
    -   F6: a one-way failure blocking release;
    -   F7: bounds.
-   **Questions:** section 10. They cover the version policy, release,
    adoption above X, SYNC instead of generations, and the timer defaults.
-   **Next action:** WAIT for the cody/Codex design review.

### 2026-11-10 UTC --- Claude (Claude Code): PR #91 review filed; D.2 design rev2, for review

-   **Review:** PR #91 got DESIGN DIRECTION APPROVED, IMPLEMENTATION HOLD.
    It is filed verbatim as `docs/handoff/PR91_D2_Design_Review.md`.
    There were four blockers, plus explicit release and adoption terms in
    B2.
-   **Rev2:** `docs/CROSS_NODE_EXCHANGE_DESIGN.md` is rewritten.
    -   Out connections stay UNBOUND until an exact SYNC from the
        receiving instance names their nonce.
    -   Invariant M gives one live links instance per (group, shard,
        member), from listener-before-links and a data-dir lock. So
        entries per link are at most the member count, and an entry
        retires only on its own member's new incarnation.
    -   State is per entry, END debt is never combined, and every field
        has a defined transition.
    -   Release is a quarantine: it resets the Out connection, sends no
        counted DATA until re-binding, and keeps a non-increasing debt.
    -   Adoption is replaced by drop-to-resume, so `live_s < X +
        max(E, Rr)` holds exactly.
    -   B2: physical ≤ live + D, with D ≤ Q_o(X + max), independent of
        churn.
    -   Tests now run X1-X18, with negatives ND1-ND20. Section 13 maps
        every review item.
-   **KEEP / REFINE / RECONSIDER:**
    -   KEEP the approved direction: origin counting, END, SYNC, 64-byte
        DATA, fail-closed, no `TCP_USER_TIMEOUT`.
    -   REFINE binding, release, incarnations and the proof.
    -   RECONSIDER adoption, which is removed.
-   **Next action:** WAIT for the rev2 review. No D.2 code.

### 2026-11-10 UTC --- Claude (Claude Code): PR #92 review filed; D.2 design rev3, for review

-   **Review:** PR #92 got DESIGN REV2 SUBSTANTIALLY APPROVED WITH TWO
    CORRECTNESS BLOCKERS, IMPLEMENTATION HOLD. It is filed verbatim as
    `docs/handoff/PR92_D2_Design_Rev2_Review.md`.
-   **Rev3 (narrow):** `docs/CROSS_NODE_EXCHANGE_DESIGN.md`.
    -   HelloAck proves identity only (IDENTIFIED). A link is BOUND only
        after both the HelloAck and the exact SYNC naming the current
        connection (nonce and `conn_seq`, `pushed = 0`), in either order.
        No counted DATA before BOUND.
    -   FINAL is removed. Only a SYNC naming the current connection
        applies; all others are ignored and counted. No history of old
        connections is kept. Lost items drop out at the next binding, or
        by quarantine, which gains a not-bound trigger.
    -   On silent retirement, the old sockets are fenced (removed from
        epoll and reset) before the entry retires and the new
        incarnation is accepted.
    -   `blocked_since_` survives the links closing, so quarantine comes
        at most `T_hb + T_release` after the block, matching X19.
    -   An independent adversarial check of the draft found nine issues
        (SYNC/binding cycle, SYNC routing and stream position, field
        names, 3.3, fencing in every link state, blocked-bit delivery,
        incarnation order); all fixed, listed in section 12.
    -   Tests X19b, X24-X27 and negatives ND27-ND35 added; X3/X4 check the
        accounting equation at every exact SYNC. Section 14 maps every
        review item.
-   **KEEP / REFINE / RECONSIDER:**
    -   KEEP the rev2 architecture as reviewed.
    -   REFINE the reconciliation state machine, fencing and timing.
    -   RECONSIDER the FINAL SYNC, which is removed.
-   **Next action:** WAIT for the rev3 review. No D.2 code.

### 2026-11-10 UTC --- Claude (Claude Code): PR #93 review filed; D.2 design rev4, for review

-   **Review:** PR #93 got REV3 CORE PROTOCOL APPROVED, IMPLEMENTATION
    HOLD ON TWO NARROW CORRECTNESS ITEMS. It is filed verbatim as
    `docs/handoff/PR93_D2_Design_Rev3_Review.md`. Q9 (remove FINAL) and
    Q10 (not-bound quarantine) were approved.
-   **Rev4 (narrow):** `docs/CROSS_NODE_EXCHANGE_DESIGN.md`.
    -   SYNC accounting barrier: the SYNC is appended FIFO after every
        byte already in `Out::buf`, whose frames have already done their
        accounting at `push`. It preempts only work not yet pushed, and
        its fields are read at the append. HEARTBEAT follows the same
        rule.
    -   The incarnation is a persistent per-member generation
        (`links_generation`, with CRC). It is incremented and made
        durable under the data-dir lock before the listener or any Out
        connection opens. A bad file fails closed, and no clock or
        randomness is involved. A restored data directory needs
        `--links-generation-min`.
    -   X28/ND36 and X29/ND37 added. X3/X4 record `count_`, `held_for_`,
        `pushed` and `rx` at the barrier and at the apply. Section 15
        maps the review.
-   **KEEP / REFINE / RECONSIDER:**
    -   KEEP everything approved through rev3.
    -   REFINE the SYNC stream position.
    -   RECONSIDER the clock-based incarnation, which is replaced.
-   **Next action:** WAIT for the rev4 review. No D.2 code.

### 2026-11-10 UTC --- Claude (Claude Code): PR #94 review filed; D.2 implementation authorized

-   **Review:** PR #94 (design rev4) APPROVED; D.2 implementation
    AUTHORIZED. Filed verbatim as
    `docs/handoff/PR94_D2_Design_Rev4_Review.md`.
    `--links-generation-min` is approved with fail-closed semantics.
-   **Next action:** implement D.2 as one PR, in the design's section 9
    commit order, each commit green. Run X1-X29 (with X19b), ND1-ND37,
    and the Release, ASan+UBSan and TSan serial suites, keeping every
    attempt. Then return it for independent review. S5, S6, R640, CP-1A,
    comparators and B2 begin-snapshot stay forbidden.


### 2026-11-10 UTC --- Claude (Claude Code): D.2 implemented, for independent review

-   **What:** D.2 cross-node exchange accounting, implemented to design
    rev4 in the section 9 commit order (steps 1-10). Later commits fix
    what validation found. Evidence:
    `bench/results/xnode-d2-validation-devvm-20261110/README.md`.
-   **Results:**
    -   Release, ASan+UBSan and TSan complete serial suites pass 47 of 47
        at beda8a2. 66d5145 changes only X4, and test_xnode passes there
        in all three builds.
    -   X1-X32 with X19b, plus the process tests x6, x7, x7b and x9, pass.
    -   Negatives: 36 of 39 caught at beda8a2. ND10 and ND24 hit
        unreachable branches. ND7 was missed by chance; X4 now forces the
        case, and ND7 is caught 3 of 3.
    -   P2 shows no measurable cost.
-   **Product defects found and fixed on site:**
    -   A rejoining links instance never bound.
    -   The blocked bit could arrive late under traffic.
    -   A single stale heartbeat could reset a healthy Out connection
        (X15, then X32; ND38).
    -   The step 9 gauge publish was slower than its 50 us interval under
        TSan, which blocked slot moves (bisected to a69da05).
-   **Disclosures:**
    -   The step 10 commits from 8ef83d3 to 6489cec are not green in every
        build. The fixes are later commits; history was not rewritten,
        because 8ef83d3 was already pushed.
    -   A pre-D.2 data directory needs `--links-generation-min 1` once.
-   **KEEP / REFINE / RECONSIDER:**
    -   KEEP the rev4 protocol as approved.
    -   REFINE the meaning of "holds for T_dead" in design 2.10: it must
        be shown by fresh heartbeats.
-   **Next action:** WAIT for independent review of the D.2 PR. Do not
    merge it, and do not call D.2 complete until it is approved. S5, S6,
    R640, CP-1A, comparators and B2 stay forbidden.

### 2026-11-11 UTC --- Claude (Claude Code): PR #95 review 1 fixed, returned for review

-   **What:** the four items of review 1
    (`docs/handoff/PR95_D2_Implementation_Review.md`) are fixed on site,
    without rewriting history. Final head 2bccdbe. Engine code is the same
    from b77937e. Evidence:
    `bench/results/xnode-d2-review-fixes-devvm-20261110/README.md`, with
    every run in order.
-   **Fixes:**
    1.  The explicit generation file is flocked (X33, process x33).
    2.  The blocked bit is retried on both edges (X34).
    3.  END v2 layout: n in source.hi, target incarnation in target.lo
        (X35; design 2.2 byte table).
    4.  ND10/ND24 are closed by proofs I1/I2 (design 2.9.1). Their
        branches are invariant checks, and the negatives are premise
        mutations. X12c was added because ND24 went uncaught in attempt 1.
    -   New negatives: ND39, ND40, ND41.
-   **Results at 2bccdbe:**
    -   Release, ASan+UBSan and TSan serial suites: 47 of 47 each.
    -   Complete test_xnode: 33 of 33. Process x6, x7, x7b, x33 and x9 pass.
    -   Negatives: 42 of 42.
    -   F8 passes on and off in all three builds.
    -   P2: on −1.0% vs off, inside the noise between rounds.
-   **Test defects fixed along the way:**
    -   q4 summed gauges published up to 84 ms apart.
    -   X4 relied on chance.
    -   test_intake F8/G1 did not allow legitimate S1 WAL-budget refusals.
-   **Environment:** low disk (6.8 GB free) failed b3 on main too. cody
    approved deleting about 17 GB of leaked test dirs, and gave a standing
    OK to delete test scratch after every run.
-   **Disclosures:** two failures have no root cause established: paxos c2b
    in Release run 2, and one F8 links-up timeout in 1,000 runs. Both
    passed on rerun, and both are recorded.
-   **KEEP / REFINE / RECONSIDER:**
    -   KEEP the rev4 protocol.
    -   REFINE: the negatives must be checked against a test that leaves the
        mutated state in place (as X12c does), not only by
        whether they apply.
-   **Next action:** WAIT for independent review of PR #95. Do not merge
    it, and do not call D.2 complete. S5, S6, R640, CP-1A, comparators,
    B2, Agent-SNC, Rust and DPDK/VPP stay forbidden.

### 2026-11-11 UTC --- Claude (Claude Code): PR #95 APPROVED, D.2 COMPLETE, merged

-   **What:** the final independent review
    (`docs/handoff/PR95_D2_Implementation_Final_Review.md`) approved
    PR #95. D.2 is COMPLETE. Validated at 2bccdbe; product code b77937e.
    Evidence: `bench/results/xnode-d2-review-fixes-devvm-20261110/`.
-   **Observations carried forward:** one paxos c2b deadline failure and
    one F8 links-up timeout, neither with a root cause. If either
    recurs, investigate it on site.
-   **Hardware:** the R640 now has all six memory channels per socket
    populated. Manifest `bench/manifests/r640-main-20261106d.txt` is no
    longer valid for new evidence. The old CP-1A results stay valid for
    the old configuration.
-   **Next action:**
    1.  Recapture the R640 manifest on merged main (governor
        performance, numa_balancing 0, dmidecode showing 6 of 6 channels
        per socket).
    2.  Update `docs/R640_TOPOLOGY.md`.
    3.  Return both for review.
    -   Not authorized: S6, CP-1A, comparators, baseline freeze,
        Agent-SNC, Rust and DPDK/VPP.

### 2026-11-11 UTC --- Claude (Claude Code): R640 manifest e, manifest-baseline closure

-   **What:**
    -   PR #95 is merged and D.2 is COMPLETE.
    -   The R640's RAM is now 12 × 32 GB DDR4-2400, in A1-A6 and B1-B6:
        six of six channels per socket, 384 GB nominal, 376 GiB usable.
    -   Manifest e (`bench/manifests/r640-main-20261111.txt`, sha256
        `768056bb...c27b6`) was captured on the host at d0d3bb7. Its
        review APPROVED the capture.
-   **Changed (branch r640-manifest-20261111):**
    -   `docs/R640_TOPOLOGY.md` 2d: manifest e supersedes d for all future
        evidence.
    -   `bench/pilot_plan.json`: only the manifest pin
        (`approved_manifest_sha256`, `_approved_manifest`).
    -   `docs/PILOT_PLAN.md` 2: the host is manifest e, and the section
        says why changing the pin is permitted.
    -   No method, workload, allocation, rate, threshold, seed or metric
        changed.
    -   Manifest d and the old CP-1A results stay as historical evidence.
-   **Next action:** return the closure for review. Do NOT run CP-1A, S6,
    comparators, Agent-SNC, Rust or DPDK/VPP until they are explicitly
    authorized.

### 2026-11-11 UTC --- Claude (Claude Code): PR #97 approved, manifest-baseline gate closed

-   **What:** cody's final review of PR #97 (head f9c2151) APPROVED it:
    manifest e is the R640 hardware baseline and the closure is COMPLETE.
    Filed verbatim as `docs/handoff/PR97_Manifest_Closure_Final_Review.md`.
-   **Next authorized task (same file):** R640 S2 pacing re-derivation and
    S6 preregistration. Freeze the post-merge main SHA, verify the host and
    the /srv/snc-data filesystem identity, run `bench/ckpt_walk_cost` on the
    R640, derive pacing values numerically, write the S6 preregistration
    (reference = same-SHA no-checkpoint median), then stop for review/GO.
-   **Still not authorized:** S6 data, CP-1A rerun, comparators, CRITERIA
    freeze, S5/ioprio, B2, Agent-SNC, Rust, DPDK/VPP.

### 2026-11-11 15:10 UTC --- Claude (Claude Code): R640 pacing re-derivation and S6 preregistration (for review)

-   **Commit/PR:** branch `r640-s2-pacing-s6-prereg`. Evidence and docs only;
    no product code changed.
-   **Code identity:** merged post-PR97 main `0dbd4c7`, RelWithDebInfo
    build-s6; `ckpt_walk_cost` SHA-256 `4707dac1…5d2d2`.
-   **Host and filesystem:**
    - Governor and NUMA balancing reapplied; RAM as manifest e.
    - `/srv/snc-data` is the same ext4 filesystem manifest d measured:
      UUID `3bf857c8…`, created 2026-11-06 06:00:03, before manifest d.
    - No lazy init (all 28,616 groups `ITABLE_ZEROED`), no idle I/O.
    - What removed the 2.2 GB is not determined.
-   **Found:**
    - `ckpt_walk_cost` alone cannot measure the walk: the S3 dsync window
      stops steps, so `walk_us` is I/O wait. On one CPU, the step maxima
      are preemption (about 1 ms in every mode).
    - bpftrace uprobes on the unchanged binary give a legacy 256-ID step of
      171.6 us p50 (about 670 ns per ID, mostly `encode_record`) and a
      batch of 16 of 13.3 us p50 (16.4 p99).
    - The R640 values are min 20, max 65 and ceiling 155 us, batch 16,
      step 5 us. The dev-VM max and ceiling would let a step reach 0.68x
      and 1.55x of the legacy step.
    - **Blocker:** `snc_server` has no walk-budget flags. The proposed
      fix is a flags-only change, separately reviewed (S6_PREREG section 2).
-   **Next action:** review `docs/S6_PREREG.md` (section 9 lists the open
    decisions: the flags change, the max age, the pre-S1 build, the runner),
    then an explicit GO. Not done: S6 data, CP-1A, comparators, CRITERIA
    freeze.

### 2026-11-11 16:20 UTC --- Claude (Claude Code): S6 pacing CLI plumbing (PR #98 review section 7)

-   **Commit/PR:** branch `s6-pacing-cli`, on merged main `7700123`.
    Classified **REFINE** (tool flags only).
-   **What changed:**
    - `snc_server` gains `--ckpt-walk-batch`, `--ckpt-walk-min-us`,
      `--ckpt-walk-max-us`, `--ckpt-walk-ceiling-us` and
      `--ckpt-walk-step-us` over the existing `Engine::Config` fields.
    - Strict parsing; times 1 us to 60 s; `min <= max <= ceiling` on the
      effective values; every refusal exits 2.
    - It prints `ckpt_pacing: ...` (the effective values) at startup.
    - Defaults are unchanged, and nothing changed under `src/` or
      `include/`. `ckpt_walk_cost` is byte-identical to the characterized
      binary.
-   **Found:** before this, an inconsistent pacing config could only reach
    the Engine from code. With flags it would have reached
    `Engine::Engine`'s `invalid_argument` uncaught (abort); the negative
    checks show that path.
-   **Evidence/tests:**
    - `tests/test_ckpt_cli.py` C1-C5;
    - 5/5 negative mutations caught;
    - RelWithDebInfo 48/48;
    - ASan+UBSan 48/48 with 0 reports;
    - all in `bench/results/s6-pacing-cli-20261111/`.
-   **Next action:** independent review and merge. Then PR #98 section 8:
    `bench/s6.py` with rule and VOID tests, the dev-machine check that the
    current `net_bench` drives the `389b623` server, and the final
    `S6_PREREG.md` freeze. **S6 NOT GO.**

### 2026-11-11 UTC --- Claude (Claude Code): PR #99 approved, S6 pacing CLI complete

-   **What:** the final review of PR #99 (head 8d40447) APPROVED it and
    authorized the merge (fbf9ca9). S6 pacing CLI plumbing is COMPLETE.
    The TSan waiver is accepted for this parser-only slice only. Filed
    verbatim as `docs/handoff/PR99_S6_Pacing_CLI_Final_Review.md` and
    posted on PR #99.
-   **Next action (authorized):** the S6 runner and final preregistration
    as one separate PR (`bench/s6.py`, its rule and VOID tests, the
    dev-machine check that the current `net_bench` drives the `389b623`
    server, the `docs/S6_PREREG.md` freeze), returned for review.
    **S6 still NOT GO.**

### 2026-11-11 17:30 UTC --- Claude (Claude Code): S6 runner and final preregistration (for review, NOT GO)

-   **Commit/PR:** branch `s6-runner`, on main `91278d7`. Python, JSON and
    docs only; no C++ change.
-   **What:**
    - `bench/s6.py` runs the preregistered cells (N, P, H, C, C0 × 3,
      rep-major), with diag3's discipline and pilot.py's isolation.
    - It samples GAUGES and HEALTH every 100 ms (sampler on CPU 0, disk
      observer on CPU 36).
    - It judges by the exact rules: PASS/FAIL/VOID, one rerun only for
      window or harness VOIDs.
    - C0 runs the 389b623 server driven by the current net_bench.
    - `bench/s6_frozen.json` holds the binary hashes. A real run refuses
      until `runner_commit` (this PR's merge) and `go_record` are set in a
      docs-only freeze commit, and refuses any code change after that
      commit.
    - `docs/S6_PREREG.md` is final.
-   **Evidence/tests** (`bench/results/s6-runner-20261111/`):
    - `tests/test_s6.py` pins every boundary, VOID and rerun rule;
    - 19/19 mutations caught;
    - dry run: preflight ok;
    - real mode refused before any process;
    - compatibility: the current net_bench drove the 389b623 server on the
      Profile-A shape (errors 0, checkpoints done, exit 0, absent gauges
      reported absent). It ran on the R640, shortened, on the boot disk:
      not evidence.
-   **Found:**
    - Stale `__pycache__` bytecode can make mutation tests run the wrong
      code. Run them with `PYTHONDONTWRITEBYTECODE=1`.
    - The pre-S1 server refuses some INDEX ops under load (no S1
      admission). That is a valid status, and it does not count against
      C0's rule 7.
-   **Next action:** independent review. Then the docs-only freeze (merge
    SHA, GO record), then the explicit S6 GO. **S6 NOT GO.**

### 2026-11-11 18:40 UTC --- Claude (Claude Code): PR #101 review 1 closure items 1-4

-   **Commit/PR:** PR #101 (`s6-runner`), code at `3fa31ef`. Python and
    docs only.
-   **What:**
    1. Rule 7 requires the measured net_bench to exit 0 (a result, never a
       rerun).
    2. The sampler skips missed deadlines (`next_deadline`); no catch-up
       bursts.
    3. C and C0 must prove a checkpoint began inside the measured run;
       otherwise a window VOID, under the existing rerun policy.
    4. Storage identity on the resolved disk: mq-deadline, write back,
       fua=1, ext4, the UUID and all of rw, relatime, stripe=2.

    S6_PREREG.md is updated to match.
-   **Evidence:**
    - `bench/results/s6-runner-20261111/review1/` and `negative-review1/`;
    - K1, S1, W1, D1 added;
    - 28/28 mutations caught;
    - dry run: preflight ok;
    - real mode refused before any process.
-   **Next action:** independent re-review. Not merged. No freeze or GO
    record. **S6 NOT GO.**

### 2026-11-11 UTC --- Claude (Claude Code): PR #101 approved and merged; S6 freeze closure

-   **What:** the final review of PR #101 (head 68c3fe4) APPROVED the S6
    runner and preregistration. Merged as
    `63440b12288f9aae36c5309fa21b759d0c9a36fd`. Filed verbatim as
    `docs/handoff/PR101_S6_Runner_Final_Review.md`.
-   **Freeze closure (docs/identity only):** `bench/s6_frozen.json`
    `runner_commit` = that merge SHA; `docs/S6_PREREG.md` 1.2/1.3 record
    it. `go_record` stays null. No runner, test, C++, binary, pacing,
    threshold, cell, sampling, allocation or workload change.
-   **Next action:** final independent freeze verification, then an
    explicit S6 GO. **S6 NOT GO.**

### 2026-11-11 UTC --- Claude (Claude Code): S6 GO filed

-   **What:** PR #102 (freeze closure) APPROVED and merged (1567507). The
    S6 freeze is VERIFIED and an explicit S6 GO is issued, filed verbatim
    as `docs/handoff/S6_R640_GO_20261111.md`. `bench/s6_frozen.json`
    `go_record` points to it. S6_PREREG 1.3 now describes the three-step
    freeze. No runner, test, C++, binary, parameter or rule change.
-   **Next action:** the S6 run itself on the R640, once, with the frozen
    runner; then commit the results and open the S6 result-review PR, and
    stop. No parameter changes before the results. CP-1A, comparators,
    CRITERIA, S5, B2, Agent-SNC, Rust and DPDK/VPP stay unauthorized.

### 2026-11-11 18:50 UTC --- Claude (Claude Code): S6 R640 regression run (once, as frozen): FAIL

-   **Commit/PR:** branch `s6-result-r640-20261111`. Run at HEAD `e2aab62`
    under `docs/handoff/S6_R640_GO_20261111.md`. Results only; nothing
    tuned or edited.
-   **Result** (`verdict.json`): **FAIL**. Preflight clean, postflight
    identical, 15 reps, no VOID, no rerun. Every P rep fails rule 4 only:
    total p99.9 14.2-14.7 ms (limit 10 ms; N below 1 ms); max about 49 ms
    in each. Rules 1-3 and 5-7 pass: 0 refusals, lateness 0.025%, p99 at
    most 1.36x the N median, emits_peak at most 329, checkpoint
    1.49-1.56 s, correctness. Capacity guard holds (0.991).
-   **Reported, not judged:**
    - The about-49 ms P max is near the unbounded one-step begin snapshot.
      This is a hypothesis, not tested.
    - C refuses 38-40 M ops per closed-loop rep (`refused_followup`), while
      C0 refuses at most about 1 k, at equal completed throughput.
    - C0's final sync takes about 1.2 s; the current build's about 0.3 ms.
-   **Next action:** independent S6 result review. No tuning, rerun or
    CP-1A before it.

### 2026-11-11 19:15 UTC --- Claude (Claude Code): S6 write-window characterization (W1/W2), for review

-   **Commit/PR:** branch `s6-wchar-r640-20261111`. Preregistration and
    driver at `c13153f`, before data. Results only; no product code.
-   **Run:** `wchar.py` (imports the frozen `bench/s6.py`), 6 reps.
    Preflight and postflight identity clean; no VOID or rerun; 0
    refusals or errors.
-   **Result vs W4 (the S6 P reps):**
    - W1: chunk write EWMA 3.2-7.8 ms (vs 9.7-35), WAL EWMA peak 4.4-4.6 ms
      (vs 6.9-7.4), 0 s above WAL-high (vs 0.8-1.1), deposit/transfer
      p99.9 12.1-12.6 ms (vs 17.3-17.8), total p99.9 9.70 ms in all reps.
    - W2: write latency about halved, but WAL pressure and tail near W4.
    - Checkpoint about 1.5 s everywhere.
    - Neither reaches N (mutation p99.9 about 1.2 ms). Max about 49 ms is
      unchanged and unexplained.
    - `wal_flush_max_us` (about 47 ms) is a since-start mark present in N
      too, so it is not a cause.
-   **Next action:** independent review chooses REFINE #1 or another
    path. No W chosen; S6 not rerun.

### 2026-11-11 20:00 UTC --- Claude (Claude Code): S6 REFINE round 1 (W=1): PASS, for review

-   **Commit/PR:** branch `s6-refine1-r640-20261111`. Preregistration,
    overlay and tests at `3acad8c`, before data. No product code.
-   **Run:** P1/H1/C1 = S6 P/H/C plus `--ckpt-window 1`, 3 reps each, once
    (19:40-19:51 UTC). S6 N and C0 reused read-only. Preflight and
    postflight clean; no VOID or rerun.
-   **Result:** **PASS**.
    - All three P1 reps pass all seven original rules: p99.9 9,961,472 ns
      (limit 10,000,000; a thin, bucket-edge margin), p99 at most 950,272
      (limit 958,464), 0 refusals, lateness 0.025%, checkpoint about
      1.55 s.
    - Capacity: median C1 4,353,886, against at least 3,815,353.
    - H1 is about equal to H (no pathology).
-   **Still open:** the about-49 ms max; mutation p99.9 about 12 ms vs N's
    1.2; the begin snapshot.
-   **Next action:** independent review. No CP-1A, default change or
    adaptive window.

### 2026-11-11 20:40 UTC --- Claude (Claude Code): CP-1A configuration closure (for review)

-   **Commit/PR:** branch `cp1a-config-closure`. Python, JSON and docs
    only; no benchmark run, no product change.
-   **Why:** PR #106's review found that `bench/pilot.py` passed only
    `--checkpoint-s 60`, so CP-1A would have run the compiled defaults
    (W=4: the S6 failure).
-   **What:**
    - `pilot_plan.json` gains `profiles.A.checkpoint` (checkpoint 60 s,
      pacing on, 16/20/65/155/5, target 30 s, max age 60 s, dsync,
      window 1, tokens 0). The top-level `checkpoint_s` is folded in. New
      plan SHA-256 `568e9874…`; nothing pinned the old one.
    - `pilot.py` `server_args`/`checkpoint_args` emit all twelve flags
      from the plan, and refuse missing or unknown keys.
    - `profile_check.py` `PROFILE_REQUIRED["A"]`: each flag exact, or
      refused, with no deviation route. C unchanged; B and D untouched.
-   **Evidence** (`bench/results/cp1a-config-closure-20261111/`):
    - tests for the command, Profile C, plan provenance and 13
      wrong-value cases plus 12 missing-flag cases;
    - 9/9 mutations caught;
    - the real `snc_server` started with the generated flags and printed
      exactly the approved values;
    - the Python gate: 7 tests pass.
-   **Next action:** independent review. Then freeze HEAD and the plan
    hash, then an explicit CP-1A GO. No CP-1A before that.

### 2026-11-11 21:00 UTC --- Claude (Claude Code): CP-1A freeze interlock (for review)

-   **Commit/PR:** branch `cp1a-freeze-interlock` (`850ad0a` plus docs).
    Changes `bench/pilot.py` and tests, and adds `bench/cp1a_frozen.json`;
    no plan, guard, workload or C++ change; no pilot run.
-   **What:** a real pilot refuses, before building or starting anything,
    unless:
    - `runner_commit` is set and an ancestor of HEAD, with only docs,
      AGENTS.md, the freeze file and results changed since;
    - the plan SHA is `568e9874…` and the manifest SHA `768056bb…`;
    - a filed GO record exists.

    `--quick` is unchecked and never evidence. Actual identities are
    recorded.
-   **Design note:** `runner_commit` is null, not PR #107's merge.
    This PR changes `pilot.py`, so the runner must be this PR's merge,
    recorded in a coordination-only commit (the S6 pattern).
-   **Evidence:**
    - interlock tests against a real temporary git repo;
    - 10/10 mutations caught;
    - gate 6/6;
    - real-mode preflight refused before any process, for exactly the
      unset runner and GO.
-   **Next action:** independent review. Then runner_commit, the GO
    record and go_record, verification, and the full CP-1A. No CP-1A
    before that.

### 2026-11-11 UTC --- Claude (Claude Code): PR #108 merged; CP-1A freeze closure

-   **What:** the final review of PR #108 (head d79f5a3) APPROVED the
    CP-1A freeze interlock, merged as `4250b32aa5e1fdfa34f15b56f8e124b4d3cc73e8`. Filed verbatim as
    `docs/handoff/PR108_CP1A_Freeze_Interlock_Final_Review.md`.
-   **Freeze closure (identity/coordination only):** `bench/cp1a_frozen.json`
    `runner_commit` = that merge SHA. The plan and manifest hashes are
    unchanged; `go_record` stays null. No runner, plan, guard, test, C++,
    workload, allocation or methodology change.
-   **Next action:** independent freeze verification, then the explicit
    CP-1A GO. **CP-1A NOT GO.**
