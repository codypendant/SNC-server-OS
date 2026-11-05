# Cluster: one keyspace across replication groups

A fixed slot map across **replication groups**, with transfers between
accounts in different groups, and failover inside a group
(`docs/CONTROL_PLANE.md`, section 12 step 1; milestone 1 had one node per
group). Every node is the same shard-per-core engine; what is new is that
an object's owner may be another group, on another machine.

## The map

The 4,096 slots are split across the groups in contiguous ranges, each
group's share proportional to its shard count (`ClusterMap::make`,
`include/snc/cluster.h`). A group is one space (one UUID, one WAL
lineage, one term) held by its members: one primary and its standbys,
replicating exactly as a lone pair does (docs/REPLICATION.md), all with the
group's shard count. Within the group the local slot map is unchanged:
slot `s` belongs to shard `s % n_shards`. Every node is started with the
same group list:

```
snc_server --shards 4 --port 7000 --data-dir /d0 --replica-of 10.0.0.2:7300 --space-uuid U0 \
  --cluster 10.0.0.1:7100:4:7000|10.0.0.2:7100:4:7000,10.0.0.3:7100:4:7000|10.0.0.4:7100:4:7000 \
  --group 0 --member 0
```

Groups are separated by `,`, a group's members by `|`, each member
`host:link_port:shards:client_port`; the client port may be left out, but
then clients cannot be routed to that member. The node above is member 0
(primary) of group 0; member 1 is started with `--standby 7300 --member 1`
and the same list.

(`Engine::Config::cluster`, `group_id` and `member` in the library.) The
map is fixed while running: **placement epoch 1**. Its crc covers the
assignment and shard counts, not the members or addresses, so adding a
standby or failing over changes neither. A node refuses follow-ups from a
peer whose crc or epoch differs. Which member is primary is not in the map
at all: it is the group's HA state (its term, its witnesses), and links
and clients find it by asking the members.

## Starting from the control plane

Instead of `--cluster` on every node, the map can be written once to the
control plane and fetched (docs/CONTROL_PLANE.md, step 2):

```
snc_witness --port 9000 --state w0 --id 0 --peers H0:9000,H1:9000,H2:9000 --new-state 1   # x3, first start only
snc_ctl --control H0:9000,H1:9000,H2:9000 init \
    --nodes 1@HA:7100,2@HB:7100,3@HC:7100 \
    --groups 4:1/7200/7000+2/7200/7000,4:3/7200/7000
snc_server --control H0:9000,H1:9000,H2:9000 --node-id 1 --data-dir d1 ...
```

A witness's first start creates its state file (`--new-state 1`); every
later start runs on that file and refuses to start without it. A witness
whose state file is lost or restored from a copy must not be restarted as
itself (docs/PAXOS_BALLOTS.md).

- `snc_ctl init` writes placement epoch 1: a random cluster ID, the nodes
  (`NodeID@host:admin`), one group per list entry (`SHARDS:` then members
  `NODE/LINK/CLIENT` joined by `+`, each group with a random space UUID)
  and the slots in contiguous ranges. It is chosen once by the witnesses'
  `placement` machine; another map is refused, the same one is accepted.
  `snc_ctl get` prints it.
- `snc_server --control ... --node-id N` asks the control members for the
  map (one answers after reading what a majority holds), finds its group
  and member by NodeID, takes its client port and the group's space UUID
  from the map, and keeps the map and its NodeID in its data directory
  (`placement`, `node`). It refuses to start in a data directory of
  another node or another space, if the map does not name it, or if the
  control plane has no map yet.
- **Control plane down at start:** the node starts from its stored map
  and says so. Safe with slot moves too: seals decide what a node serves,
  not its map alone (docs/MIGRATION.md).
- **Slots move** between groups (`snc_ctl move SLOT GROUP`), which raises
  the epoch; nodes adopt newer maps from the control plane (polled, or at
  once when a node link or a client shows a newer epoch). The protocol, its
  proof and its tests are in docs/MIGRATION.md.
- Which member is primary is not in the map; `--standby` and
  `--replica-of` still decide it, and the witnesses after that. One
  witness quorum can watch every group (`snc_witness --group
  G=PRIMARY:ADMIN,STANDBY:ADMIN`, repeated), each on its own `ha/G`
  machine; `tests/test_control.py` runs two groups this way and fails one
  over.
- Mint leases come in cluster-wide blocks from the same quorum
  (docs/CONTROL_PLANE.md section 11), so an ObjectID minted by one group
  can never be minted by another, which slot moves will rely on. Admin
  `STATUS` reports the block held (`leases=LO-HI`) and the epoch.

## Ops

- **Creates stay local.** A group mints IDs only in its own slots, so a
  create never crosses the network. `create_near` an object of another
  group is refused.
- **Ops on another group's object** are refused with `E_MOVED`, whose
  result (`MovedResult`) carries the refusing node's placement epoch and
  the owning group by its map (docs/CONTROL_PLANE.md section 8). The
  client must send them to the owner. A refusal does not depend on load
  (it comes before the follow-up backlog check). Forwarding reads for
  out-of-date clients comes with slot moves, when a map can be stale.
- **Clients route by the map.** A `CLUSTER` request (wire kind 5) returns
  the map, its crc and epoch, and every member's address and client port.
  `net::ClusterClient` fetches it from any node and keeps one `Client` per
  group, connected to whichever member answers as primary. With sessions
  (`enable_sessions`) each group's client also follows that group's
  failovers and resends safely (docs/RETRIES.md); the map it holds stays
  valid throughout.
- **Index lookups** search only the group they are sent to.

## Follow-ups between nodes

A transfer to an account in another group works as one between shards
(docs/TRANSFERS.md): the debit commits on the source, and the credit,
settle and forget are follow-ups. Follow-ups for another group's objects
travel over **node links** (`src/core/node_link.{h,cpp}`):

- Shard `i` of a group's primary listens on its member's `link_port + i`
  and keeps one TCP connection to every shard of every other group. A
  standby does not listen and opens no links. A follow-up goes straight
  to the shard that owns its target. Frames are 64 bytes (source, target,
  op, fail op, up to 16 bytes of args) after a 24-byte hello naming the
  sender's group and shard, its map crc and placement epoch.
- **An outbound link tries the group's members in turn** until one
  accepts: the primary, since standbys do not listen. After a failover it
  finds the promoted member by itself, without any change to the map.
- **A follow-up to another group waits until its source's record is
  durable**, which for a sync primary means on its standby too, even when
  this node is otherwise free to run it earlier, so no group acts on a
  debit that could still be lost, by a crash or by a failover.
- **No acks, no log.** A follow-up whose link is down is dropped, and so is
  whatever a connection held when it dropped. Every time a connection comes
  up, in either direction, the shard runs its objects' resume hooks again:
  outbox entries re-send their credit, inbox entries ask their sender
  (`query`). Transfer follow-ups are idempotent, so repeats are harmless.
  For this to finish every exchange, `query` now re-sends the credit when
  the sender still holds the entry (the destination's settle was lost
  while the sender did not restart).
- **Order per link.** Frames from one shard to another arrive in the order
  sent, which the transfer protocol needs (a credit must never overtake its
  own forget). A receiver that gets a new connection from a shard closes
  the older one first, discarding what it has not read, so a reconnect
  cannot reorder frames.
- **Every follow-up carries the epoch it was emitted at.** One newer than
  the receiver's map is held until the receiver adopts that map; one
  emitted before its source's or target's slot last moved is dropped (the
  sender re-sends what is unsettled when it adopts the new map). Without
  this a move let an old copy of a credit arrive after its exchange was
  forgotten (docs/MIGRATION.md, "Follow-ups across a move").

## Failover inside a group

A group's primary fails over as a lone pair does (docs/REPLICATION.md):
its witnesses (one `snc_witness` set per group, the group's `ha/<g>`
state) or an operator promote its standby. What the cluster adds:

- **The promoted member opens the group's links** once every shard is
  primary (follow-ups are held until then, as after recovery). The other
  groups' links, which kept trying the members in turn, connect to it;
  every link coming up runs the resume pass on both ends, so the credits,
  settles and forgets lost with the old primary's links are sent again.
- **Nothing about placement changes.** The map, its crc and its epoch are
  the same before, during and after; a client holding the map routes by
  it throughout and only its group's connection moves.
- **The other groups keep working.** Their transfers to the failed
  group's accounts still commit (the debit is local); the credits wait in
  their outboxes until the group has a primary again.

## A fenced primary withdraws

A failed-over primary that is still running (cut off from its standby,
or simply replaced while alive) used to keep its node links: the other
groups stayed connected to it and sent it follow-ups it could never make
durable, so cross-group transfers stalled until its connections dropped.
Now a primary that learns it was replaced leaves the data plane
(`Shard::withdraw`, `Engine::withdraw`):

- **How it learns.** The promoted member keeps its replication listeners
  and refuses the old term there (`kRefusedTerm`); the old primary's
  sender reconnects within 100 ms of the promotion and is refused. A
  witness answering `NO term ... was replaced` does the same through
  `snc_server`. Either is final: one shard learning it withdraws every
  shard.
- **What it does.** Closes its link listener and every node link, in and
  out; runs no follow-ups (they are recoverable: their sources re-send them
  to the new primary); answers new client ops `E_STANDBY` and ops that were
  waiting for its standby's ack `E_FENCED` (in doubt: the promoted standby
  may hold them). Its admin port stays and reports `role=f`.
- **What the others do.** Nothing new: their links lose the old primary,
  try the next member, find the promoted one and run the resume pass, as
  after a crash. The HA term is not added to the link hello (one
  authority per fact: it stays in replication and the witnesses).

## What is checked

`tests/test_cluster.cpp`, failover (`test_group_failover`): three groups
of two members (2 shards each, sync replication that never degrades), 120
accounts per group, each primary running transfers to accounts in all
groups. Group 1's primary is SIGKILLed mid-run and its standby promoted;
it then runs transfers of its own. Checked:

- every transfer any primary acknowledged, the killed one's included, is
  in the final balances, and nothing else is: each account equals its
  start plus the acknowledged transfers plus some subset of the killed
  primary's few transfers still in flight (each committed or not, never
  acknowledged); the test tries every subset and fails if none fits;
- money is conserved over all 360 accounts, and no outbox or inbox entry
  is left (every cross-group transfer settled);
- groups 0 and 2 kept acknowledging transfers while group 1 had no
  primary;
- the promoted member received follow-ups from the other groups and
  acknowledged transfers of its own;
- the placement epoch is 1 and the map crc unchanged on every primary at
  the end, and a `ClusterClient` with sessions, given the same map, reads
  all 360 accounts from each group's current primary.

The same test runs a second time with group 1's primary **left running**
(`test_group_failover(true)`): its standby is promoted under it. Checked
in addition:

- the old primary learns it was fenced (about 0.1 s after the promotion)
  and withdraws: its link ports refuse connections, in-process ops and a
  network client get `E_STANDBY`, and it still reports itself fenced at
  the end;
- groups 0 and 2 keep acknowledging transfers after the fence, the other
  groups' links reach the promoted member, and everything settles;
- every transfer either primary of group 1 acknowledged is in the final
  balances; the old primary's unacknowledged ones (`E_FENCED` or never
  answered, up to 12) each committed or not, and runs have seen both, which
  is why `E_FENCED` means in doubt rather than refused;
- the placement epoch and map crc are unchanged on all four live nodes,
  the fenced one included.

With withdrawal disabled (links left open) the same test never settles.
12 runs, four in parallel, passed; one under ASan.

A typical run: about 10,000 transfers acknowledged, 3,000 to 5,000 of
them by groups 0 and 2 in the 0.4 s group 1 had no primary, promotion in
about 3 ms, 0 to 12 transfers in flight at the kill. 15 runs of three in
parallel passed, and one under ASan.

Without replication (`test_kill_restart`, milestone 1): three
single-member groups (2 shards each) with 120 accounts per group. Each runs random transfers from its accounts to any
account in the cluster. Node 1 is SIGKILLed partway through, while the
others keep sending to it, and restarted. Once nothing is in flight:

- money is conserved over all 360 accounts;
- no outbox or inbox entry is left on any node;
- every account matches the transfers acknowledged, apart from the
  handful the killed node acknowledged in the instant before the kill,
  which its log (written after each ack) can miss;
- a `ClusterClient` connected to one node reads all 360 accounts, each
  from its owner (none refused, none forwarded between shards), and a
  read sent to the wrong node gets `E_MOVED` with epoch 1 and the right
  group.

The same test runs again with the map from the control plane
(`test_kill_restart(true)`, next section): three witnesses hold it,
nodes start by NodeID, and node 1 restarts while every witness is down.

A typical run: about 20,000 transfers acknowledged, 30,000 follow-ups
sent between nodes per node, several thousand dropped while node 1 was
down and recovered by the resume passes.

## Limits (this milestone)

- With `--cluster` (no control plane) the map is fixed and every node must
  be given the same list. Slots move only under `--control`, one move at a
  time (docs/MIGRATION.md, "Limits").
- **A fenced primary withdraws only once it hears so** (above): from the
  promoted member or a witness. One cut off from both, while the other
  groups still reach it, blocks (acknowledges nothing, as before) but
  keeps its links, and their follow-ups to its group wait until its
  connections drop. Safe, a stall. If it matters, the next step is
  withdrawing while blocked (sync, standby gone past a bound), which needs
  no new protocol.
- The cluster test promotes the standby itself; witnesses per group are
  the same `snc_witness` as for a pair, not yet exercised in a cluster
  test, and neither is `--rejoin` of the old primary as a standby.
- A resume pass walks every object on the shard. Cheap at this scale; at
  millions of objects per shard it should walk only objects with entries.
- A peer that vanishes without closing its connections (power loss, a
  partition) is noticed only when TCP gives up; follow-ups sent to it
  meanwhile are lost until the link comes back and resumes.
- Loopback only so far: numbers across real machines are milestone 4.
