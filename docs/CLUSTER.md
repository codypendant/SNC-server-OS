# Cluster: one space across nodes

Milestone 1 of the multi-machine design: a fixed slot map across nodes,
with transfers between accounts on different nodes. Every node is the same
shard-per-core engine; what is new is that an object's owner may be on
another machine.

## The map

The 4,096 slots are split across the nodes in contiguous ranges, each
node's share proportional to its shard count (`ClusterMap::make`,
`include/snc/cluster.h`). Within a node the local slot map is unchanged:
slot `s` belongs to shard `s % n_shards`. Every node is started with the
same node list:

```
snc_server --shards 4 --port 7000 --data-dir /d0 \
  --cluster 10.0.0.1:7100:4:7000,10.0.0.2:7100:4:7000,10.0.0.3:7100:8:7000 --node 0
```

Each entry is `host:link_port:shards:client_port`; the client port may be
left out, but then clients cannot be routed to that node.

(`Engine::Config::cluster` and `node_id` in the library.) The map is fixed
while running. Its crc covers the assignment and shard counts, not the
addresses; a node refuses follow-ups from a peer whose crc differs.

## Ops

- **Creates stay local.** A node mints IDs only in its own slots, so a
  create never crosses the network. `create_near` an object of another
  node is refused.
- **Ops on another node's object** are refused with `E_WRONG_NODE`. The
  client must send them to the owner. Forwarding reads for out-of-date
  clients comes with the directory (milestone 2), when the map can change.
- **Clients route by the map.** A `CLUSTER` request (wire kind 5) returns
  the map, its crc and every node's address and client port.
  `net::ClusterClient` fetches it from any node, keeps one `Client` per
  node, and sends each request to the owning node and, on it, the owning
  shard.
- **Index lookups** search only the node they are sent to.

## Follow-ups between nodes

A transfer to an account on another node works as one between shards
(docs/TRANSFERS.md): the debit commits on the source, and the credit,
settle and forget are follow-ups. Follow-ups for another node's objects
travel over **node links** (`src/core/node_link.{h,cpp}`):

- Shard `i` of each node listens on `link_port + i` and keeps one TCP
  connection to every shard of every other node. A follow-up goes straight
  to the shard that owns its target. Frames are 64 bytes (source, target,
  op, fail op, up to 16 bytes of args) after a 16-byte hello naming the
  sender and its map crc.
- **A follow-up to another node waits until its source's record is
  durable**, even when this node is otherwise free to run it earlier, so
  no node acts on a debit that could still be lost.
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

## What is checked

`tests/test_cluster.cpp` runs three node processes (2 shards each) with
120 accounts per node. Each runs random transfers from its accounts to any
account in the cluster. Node 1 is SIGKILLed partway through, while the
others keep sending to it, and restarted. Once nothing is in flight:

- money is conserved over all 360 accounts;
- no outbox or inbox entry is left on any node;
- every account matches the transfers acknowledged, apart from the
  handful the killed node acknowledged in the instant before the kill,
  which its log (written after each ack) can miss;
- a `ClusterClient` connected to one node reads all 360 accounts, each
  from its owner (none refused, none forwarded between shards), and a
  read sent to the wrong node gets `E_WRONG_NODE`.

A typical run: about 20,000 transfers acknowledged, 30,000 follow-ups
sent between nodes per node, several thousand dropped while node 1 was
down and recovered by the resume passes.

## Limits (this milestone)

- The map is fixed: no slot moves, and every node must be given the same
  list (nothing persists it yet; a node restarted with another map is
  refused by its peers' crc check, not by its own data).
- Not combined with replication: a cluster node has no standby yet
  (milestone 2 makes each node a replication group; see
  `docs/ARCHITECTURE_AUDIT.md`).
- A resume pass walks every object on the shard. Cheap at this scale; at
  millions of objects per shard it should walk only objects with entries.
- A peer that vanishes without closing its connections (power loss, a
  partition) is noticed only when TCP gives up; follow-ups sent to it
  meanwhile are lost until the link comes back and resumes.
- Loopback only so far: numbers across real machines are milestone 4.
