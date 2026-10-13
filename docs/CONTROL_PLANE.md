# Control plane: replication groups and placement

Status: **accepted with three clarifications** (Claude, 2026-10-13;
reviewed by Codex the same day, see "Review outcome" at the end). It answers the ten
questions in section 25 of `docs/ARCHITECTURE_AUDIT.md`. No code
implements it yet; nothing here changes the milestone 1 data plane.

## The model in one page

```
ObjectID ─hash→ Slot ─placement map (epoch E)→ Group ─HA state (term T)→ leader Node ─slot % n→ Shard
```

- **Node**: one `snc_server` process with its data directory. It has a
  stable `NodeID` and an address. The model lets a node be a member of
  several groups (`NodeID → memberships[]`); milestone 2 runs one group
  per `snc_server` process as an operational restriction, not a rule of
  the schema.
- **Replication group**: one data store and its copies. The members hold
  the same space (one space UUID, one WAL lineage, one term). At any
  instant one member is primary (the HA authority decides which); the
  rest are standbys. This is what the code already calls an HA pair; a
  group of one member is what milestone 1 calls a node.
- **Placement**: which group owns each of the 4,096 slots. It changes
  only when a slot moves or groups join or leave, and each change bumps
  the **placement epoch**.
- **HA state**: per group, which member is primary, under which **term**,
  and whether a degrade grant is out. It changes on failover and on
  degrade; each change bumps that group's term or version, never the
  placement epoch.

Two version domains, two kinds of state machine, one consensus mechanism
(below).

## 1. NodeID

- A `u32`, assigned when the node is added to the cluster, never reused.
- Not derived from the address: a node keeps its ID when it moves to
  another host, and a new machine at an old address gets a new one.
- Persisted in the node's data directory (a small `node` file next to the
  superblock, written once), so a restarted node knows who it is without
  its command line.
- A node refuses to start in a group whose directory entry does not list
  its `NodeID`.

Today (step 1) a node is named by its place in the command-line list,
`--group G --member M`; there is no `NodeID` yet. Step 2 assigns NodeIDs,
and a converted cluster numbers its members in list order.

## 2. ReplicationGroupID

- A `u32`, assigned when the group is created, never reused.
- A group owns one space: its space UUID is the group's data identity,
  and every member's superblock carries it (as today). Term lives in the
  superblock too (as today), so it remains per group.
- Every member of a group has the **same shard count**, so replication
  stays stream to stream (`docs/REPLICATION.md`, "Same shape").
- A group is not a node: its identity is its `GroupID` and space UUID,
  never a `NodeID`. One node may hold members of several groups (a large
  server hosting several independently placed and failed-over groups);
  each membership has its own data directory and shards.
- Which nodes are members, and where they are reached, is placement
  state (section 3). Which member is primary is HA state (section 6).

## 3. Cluster metadata schema

```
Cluster
  cluster_id        u128, random at creation; every message carries it
  epoch             u64, placement epoch (section 5)
  nodes[]           NodeID → { host, admin_port, memberships[GroupID] }
  groups[]          GroupID → { space_uuid, n_shards, ha_ref,
                                members[] → { NodeID, link_port, client_port } }
  slots[4096]       GroupID
  slot_state[4096]  STABLE | (later) PREPARING | COPYING | CUTOVER, with the target group
  lease_next        u64, next unassigned block of mint leases (section 11)
```

`ha_ref` names the HA state machine instance for the group (section 10).

**One authority per fact.** `placement` holds slot → group, group
membership and addressing. `ha/<group>` holds term, current primary and
degrade state. The placement state never carries a copy of a group's
primary or term, not even as a hint. Nodes and clients cache
`group → current primary` as ephemeral routing state (section 7), learned
from the members or from `ha/<group>`, and drop it when a member refuses
them.

Size: 4,096 × 4 bytes for slots plus small tables, about 20 KiB, so every
node and client can hold all of it.

## 4. Slot → group, and on to a shard

- `slots[s]` names the owning group. Within the group, slot `s` belongs to
  shard `s % n_shards` (unchanged), on whichever member is primary.
- The initial assignment for N groups is `ClusterMap::make`'s:
  contiguous ranges proportional to shard counts.
- Moving a slot changes `slots[s]` and nothing about the objects' IDs:
  this is the payoff of the slot indirection the audit keeps.

## 5. Placement epoch

- A `u64`, +1 on every chosen change to placement (slot assignment, slot
  state, nodes or groups added or removed). Never changed by failover.
- Every node holds the latest map it has seen, with its epoch, and writes
  it to its data directory when it changes, so it restarts with it.
- **The node-link hello carries `(cluster_id, epoch)`** in place of
  today's map crc. A side that sees a higher epoch fetches the newer map
  from the control plane before taking follow-ups from that peer.
- **A follow-up for a slot the receiver's group does not own is dropped.**
  The sender learns the newer map (from the hello or a refresh) and its
  resume pass re-sends to the right group. This is invariant 15 doing the
  work: follow-ups are recoverable, so a stale route costs a retry, not
  correctness.

## 6. HA term, per group

- Unchanged in meaning: +1 at each promotion inside the group, persisted
  in each member's superblock, used for fencing exactly as today.
- Two groups' terms are unrelated numbers; a term is only ever compared
  with another term of the same group (same space UUID).
- A failover changes the group's HA state only. Clients and peers that
  hit the old primary are told so (`E_STANDBY` from a demoted, rejoined
  or fenced member, or a refused
  connection; a fenced primary closes its node links, docs/CLUSTER.md), and ask the group's members or the HA state for the new
  one. The placement epoch does not move (invariant 11, 12).

## 7. Caching on nodes and clients

- **Nodes** hold the whole map and the current primary of every group
  they send follow-ups to. They refresh the map when a hello or a
  response shows a higher epoch, and the primary of a group when its old
  primary refuses them.
- **Clients** (`net::ClusterClient`) do the same: fetch `(epoch, map,
  member addresses)` from any node (the `CLUSTER` request gains the epoch
  and the group table), keep one connection per group primary, refresh on
  `E_MOVED` and fail over within a group as `net::Client` does today.
- **No consensus on the hot path** (invariant 14): an op goes to the
  cached owner. The control plane is consulted only on a miss, at start,
  or when a peer shows a newer epoch.

## 8. Stale-map responses

- A node asked to act on a slot its group does not own answers
  **`E_MOVED { epoch, group }`** with its own current epoch and owner. It
  replaces `E_WRONG_NODE` (kept as the code for "no map at all", e.g.
  during start).
- Writes are always refused, never forwarded: the client must resend to
  the owner, which keeps a write's path (and its ack) one hop.
- Reads may be forwarded one hop to the owner, as cody chose for stale
  clients (2026-10-13). Forwarding is an optimisation and can come after
  `E_MOVED`.
- A client that sees an `E_MOVED` with an epoch older than its own keeps
  its map (the node is behind) and retries once.
- **Done with slot moves** (docs/MIGRATION.md): `E_MOVED` with a newer
  epoch makes `net::ClusterClient` fetch the map from the members and
  resend; a node that sees a newer epoch on a node link holds that
  follow-up, refreshes its map at once and runs it after. A slot this group
  owns but does not serve (sealed, or staged and not yet activated)
  answers `E_MIGRATING {epoch, group}`. Reads are not forwarded yet.

## 9. Bootstrap and persistence

- **Create**: `snc_ctl init` against the control members writes epoch 1:
  the cluster ID, the nodes, one group per primary/standby set, and the
  initial slot ranges. Nodes are then started with `--control H:P,...`
  and their `NodeID`; they fetch the map and refuse to serve until they
  have one with their own group in it.
- **Persistence**: the placement state lives in the control members' state
  files (as the witnesses' state does today); each node also keeps its last
  map in its data directory.
- **Starting from a stored map is safe** (settled at step 3,
  docs/MIGRATION.md, "Proof"). A node that cannot reach the control plane
  at restart serves from its stored map: a sealed source never serves
  whatever its map says, and a destination serves only once it holds an
  activated copy, which needs a map at the move's committed epoch. A stale
  map can make a node refuse a slot it owns, never serve one it does not.
  The crash matrix restarts a source and a destination at every step with
  all witnesses down.
- **Ownership is fenced by data, not by the map.** When a slot moves
  (later milestone), the source group writes a durable, replicated
  *seal* record for the slot before the new epoch is chosen; a sealed
  slot refuses writes whatever the node's map says. A stale map can then
  only send a request to the wrong place, never let two groups both
  accept writes for one slot. This is the placement analogue of term
  fencing, and why no node needs a lease from the control plane to serve.
- **Safety proof (done, docs/MIGRATION.md).** It was stated as: The ordering
  `source seal durable → placement epoch commit → destination activation`
  must be shown safe under a crash and restart of the source primary, the
  source standby (and a failover between them), the destination primary
  and standby, and a control-plane minority, at every transition. Two
  cases to settle: a seal written but the epoch never committed (the
  source must be able to unseal, or the move must be driven forward), and
  a destination that restarts after the epoch commit but before it has
  activated. The proof, and a crash test that kills each party at each
  step, are part of that milestone's definition of done. Both cases are
  settled there: an uncommitted seal is undone only after a linearizable
  read shows the move removed (move IDs never repeat), and a destination
  activates its staged copy whenever it sees the committed map, at start or
  later.

## 10. Placement consensus vs witness consensus

The audit's three options were: (a) the witness processes host both state
machines, (b) a new control service owns placement while witnesses stay HA
arbiters, (c) a shared consensus layer under separate state machines.

**Recommendation: (c) running in the existing witness processes**, i.e. (a)
built the way (c) describes.

- Extract the Paxos core from `tools/snc_witness.cpp` into a small
  library: a sequence of single-decree instances over a versioned value,
  with promises and accepts synced to a state file. That is what it
  already does for one state; the change is to key it by state machine.
- Run several independent state machines on it, each with its own
  version sequence and state file section:
  - `placement`: one per cluster, holding section 3's schema;
  - `ha/<group>`: one per group, holding exactly today's witness state
    (space, term, primary, grant, failed over).
- The same 3 or 5 processes serve both, so there is one thing to deploy
  and one majority to keep up. They stay logically separate: no value
  ever spans two machines, a failover never touches `placement`, and a
  slot move never touches `ha/*`.
- Why not (b): a second consensus service doubles what an operator has to
  run and keep a majority of, for state that changes rarely and is small.
  The separation the audit asks for is in the state, not the processes;
  if the processes later need splitting (load, blast radius), the library
  boundary makes that a deployment change.

## 11. Mint leases (found while writing this)

**Implemented (step 2b).** The control quorum runs a `leases` machine
(body `next N`, from 2^32 up, above any lease a space minted from its own
counter). `LEASES TAKE count` chooses `next N+count` in one Paxos round and
returns `[N, N+count)`; each value carries a random tag, so two takes from
the same base can never both win. A node (`Engine::Config::lease_blocks`)
holds one block of 2^16 in its superblock (`lease_lo`, `lease_hi`, written
before anything is minted from it), takes a new one when it is used up,
and a promotion always takes a fresh one before it changes anything (a
standby's superblock knows nothing of its primary's block). A node that
cannot reach the control plane mints from the block it holds; one whose
block is used up refuses to start, and a promotion without a block is
refused (the standby stays a standby; the witnesses ask again). Without
`lease_blocks` (no control plane) leases come from the space's own
counter, as before.

The original analysis:

An ObjectID carries a 56-bit mint lease, and today each space allocates
leases from its own superblock counter (`next_lease`). In milestone 1 two
nodes can therefore mint the same `(lease, seq)` pair. It is harmless only
because a node keeps an ID solely when it hashes into its own slots, so a
duplicate pair would land on the minter's own node.

Once slots move, that argument fails: an ID minted by group A in slot 782
before the move could be minted again by group B after it. So **leases
become cluster-wide**: a group takes blocks of leases from `lease_next` in
the placement state (one consensus round per block of, say, 2^16 leases,
recorded in the group's superblock before use). This keeps the ObjectID
format and its location independence; the lease is an allocator
namespace, never read by routing. It must land before the first slot
move. A group that cannot reach the control plane can still mint from
the block it holds.

## 12. From milestone 1 to here

1. **Groups with HA, static placement.** Each group is a primary and a
   standby with the existing replication, witnesses per group (`ha/<g>`).
   The map is still fixed at epoch 1. Clients and node links route to the
   group's primary and follow failovers. Test: three groups, kill a
   primary mid-transfer, money conserved, placement epoch unchanged.
   **Done** (docs/CLUSTER.md): the hello already carries the epoch (fixed
   at 1), and links find the primary by trying the members in turn.
2. **Placement in the control plane.** The `placement` state machine,
   `snc_ctl init`, epochs in the hello, `E_MOVED`, maps stored on nodes,
   cluster-wide leases. Still no slot moves.
   **Done in part (2a):** the witnesses' Paxos is a library
   (`tools/paxos.h`) running independent named machines; each witness set
   holds `ha/0` (its pair) and `placement`, which a failover never touches.
   `snc_ctl init` / `get`; NodeIDs (persisted in the data directory);
   nodes start from the control plane by NodeID, or from their stored map
   when it is down (provisional, section 9); `E_MOVED {epoch, group}`;
   the hello already carries the epoch (step 1). Encoding:
   `include/snc/placement.h`. Checked by `test_cluster`
   (`test_kill_restart(true)`, docs/CLUSTER.md).
   **Done (2b):** cluster-wide mint lease blocks (section 11) from a
   `leases` machine; one witness quorum running `ha/<g>` for every group
   (`snc_witness --group G=...`), next to `placement` and `leases`.
   Checked by `tests/test_witness.py` (three groups on one quorum: one
   fails over, one runs alone under a grant, nothing else moves) and
   `tests/test_control.py` (real nodes: a group's primary killed, the
   quorum promotes its standby; the other group's machine, placement and
   epoch untouched; every lease block disjoint, the promoted member's
   fresh).
   Map refresh on a newer epoch was left for step 3 (done there).
3. **Slot moves.** Seal, copy, cutover, with the `slot_state` values
   above; reads forwarded for stale clients.
   **Done** (docs/MIGRATION.md) except read forwarding: a move record in
   `placement` driven by compare-and-set (PREPARED, SEALED, STAGED,
   COMMITTED), a durable seal at the source and a staged, digest-checked
   copy at the destination, activation once the committed map is seen,
   abort with a linearized unseal; `snc_ctl move SLOT GROUP` and
   `move --abort SLOT`; map refresh on `E_MOVED` and on a newer epoch over
   a node link; follow-ups carry their emission epoch. Checked by
   `test_migrate`: 67 crash scenarios, each party killed at each step.
4. Real machines (unchanged).

Steps 1 and 2 are independent and could go in either order; step 1 first
proves invariant 11 with the least new machinery.

## Review outcome (Codex, 2026-10-13)

Approved directionally, with three changes, now applied above:

1. **Several groups per node are allowed by the model** (sections 2, 3);
   one group per process is milestone 2's operational restriction only.
2. **Seals are kept; serving from a stored map at startup is
   provisional** until the slot-move crash protocol is proved (section 9).
3. **No primary hints in placement**: one authority per fact (section 3).

Accepted as proposed: cluster-wide mint lease blocks, `E_MOVED {epoch,
group}`, no write forwarding, persisted placement maps, the epoch in the
node-link hello, one Paxos library with independent `placement` and
`ha/<group>` machines, and groups with HA under static placement as the
first step.

## Still open

- More than one move at a time; collecting a source's sealed copies;
  chunked final copies for large slots (docs/MIGRATION.md, "Limits").
- Lease block size, and whether leases are pre-split per shard.
