# Control plane: replication groups and placement

Status: **proposal for review** (Claude, 2026-10-13). It answers the ten
questions in section 25 of `docs/ARCHITECTURE_AUDIT.md`. No code
implements it yet; nothing here changes the milestone 1 data plane.

## The model in one page

```
ObjectID ─hash→ Slot ─placement map (epoch E)→ Group ─HA state (term T)→ leader Node ─slot % n→ Shard
```

- **Node**: one `snc_server` process with its data directory. It has a
  stable `NodeID` and an address. It is a member of exactly one group.
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

Today `--node I` is an index into the command-line list (`u8`). That index
becomes the initial `NodeID` when a milestone 1 cluster is converted.

## 2. ReplicationGroupID

- A `u32`, assigned when the group is created, never reused.
- A group owns one space: its space UUID is the group's data identity,
  and every member's superblock carries it (as today). Term lives in the
  superblock too (as today), so it remains per group.
- Every member of a group has the **same shard count**, so replication
  stays stream to stream (`docs/REPLICATION.md`, "Same shape").
- Membership changes (adding a standby, replacing a dead one) are HA
  changes of that group, not placement changes.

## 3. Cluster metadata schema

```
Cluster
  cluster_id        u128, random at creation; every message carries it
  epoch             u64, placement epoch (section 5)
  nodes[]           NodeID → { host, link_port, client_port, admin_port, group }
  groups[]          GroupID → { space_uuid, n_shards, members[NodeID], ha_ref }
  slots[4096]       GroupID
  slot_state[4096]  STABLE | (later) PREPARING | COPYING | CUTOVER, with the target group
  lease_next        u64, next unassigned block of mint leases (section 11)
```

`ha_ref` names the HA state machine instance for the group (section 10);
the placement record does not copy the group's term or primary.

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
  hit the old primary are told so (`E_STANDBY` from a demoted or
  rejoined member, or a refused
  connection), and ask the group's members or the HA state for the new
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

## 9. Bootstrap and persistence

- **Create**: `snc_ctl init` against the control members writes epoch 1:
  the cluster ID, the nodes, one group per primary/standby set, and the
  initial slot ranges. Nodes are then started with `--control H:P,...`
  and their `NodeID`; they fetch the map and refuse to serve until they
  have one with their own group in it.
- **Persistence**: the placement state lives in the control members' state
  files (as the witnesses' state does today); each node also keeps its last
  map in its data directory. A node that cannot reach the control plane
  at restart serves with its stored map: safe, because a slot that has
  moved away is sealed in the node's own data (next point).
- **Ownership is fenced by data, not by the map.** When a slot moves
  (later milestone), the source group writes a durable, replicated
  *seal* record for the slot before the new epoch is chosen; a sealed
  slot refuses writes whatever the node's map says. A stale map can then
  only send a request to the wrong place, never let two groups both
  accept writes for one slot. This is the placement analogue of term
  fencing, and why no node needs a lease from the control plane to serve.

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
2. **Placement in the control plane.** The `placement` state machine,
   `snc_ctl init`, epochs in the hello, `E_MOVED`, maps stored on nodes,
   cluster-wide leases. Still no slot moves.
3. **Slot moves.** Seal, copy, cutover, with the `slot_state` values
   above; reads forwarded for stale clients.
4. Real machines (unchanged).

Steps 1 and 2 are independent and could go in either order; step 1 first
proves invariant 11 with the least new machinery.

## Open questions for review

- Is "one group per node" right, or should a node host several groups
  (one per shard set) so that load can be balanced without moving whole
  nodes? This spec keeps one, as the simplest step from today's code.
- Should a node fetch the map at every start even with a stored one, and
  refuse to serve while the control plane is unreachable? This spec says
  no, relying on seals.
- Lease block size and whether leases should be pre-split per shard.
- Whether the `placement` machine should record each group's current
  primary as a hint (for clients), accepting that it may lag the `ha/*`
  machine.
