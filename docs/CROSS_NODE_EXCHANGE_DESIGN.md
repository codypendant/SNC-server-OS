# Follow-up intake D.2: cross-node exchange accounting (design rev4, for review)

**Status: design rev4, for review. No D.2 code until it is approved.**

- Rev1 was PR #91. Its review ruled: DESIGN DIRECTION APPROVED, IMPLEMENTATION
  HOLD (`docs/handoff/PR91_D2_Design_Review.md`, verbatim).
- Rev2 was PR #92. Its review ruled: SUBSTANTIALLY APPROVED WITH TWO
  CORRECTNESS BLOCKERS, IMPLEMENTATION HOLD
  (`docs/handoff/PR92_D2_Design_Rev2_Review.md`, verbatim).
- Rev3 is narrow. It changes only the reconciliation state machine
  (IDENTIFIED vs BOUND, current-connection SYNC, no FINAL), fencing on
  silent retirement, and the blocked-primary timing, with their tests.
  The rest of rev2 stands as reviewed. Section 14 maps each rev2 review
  item; section 13 still maps the rev1 items.
- Rev3 was PR #93. Its review ruled: REV3 CORE PROTOCOL APPROVED,
  IMPLEMENTATION HOLD ON TWO NARROW CORRECTNESS ITEMS
  (`docs/handoff/PR93_D2_Design_Rev3_Review.md`, verbatim).
- Rev4 changes only those two items: the SYNC's position in the byte
  stream (an accounting barrier, 2.5), and the incarnation, now a
  persistent monotonic generation (2.1). It adds X28, X29, ND36 and
  ND37, and fixes the wording those affect. Section 15 maps the review.
- D.2 builds on D.1 as approved and merged
  (`docs/FOLLOWUP_INTAKE_DESIGN.md`, 3.1-3.9), which stays unchanged.

**Still forbidden until D.2 is implemented and approved:** S6, the R640
campaign, a CP-1A rerun, comparators, B2 begin-snapshot work, and S5 unless
separately justified.

## 0. Summary

**The gap (D.1).** An exchange ends at its origin as soon as its follow-up
leaves the node (`shard.cpp:3051-3054`). Link arrivals are counted by no
shard (`shard.cpp:1825`). So X does not bound cross-node work: replies can
pile up at a peer toward a slow node.

**Rev2 mechanism:**

1. **Origin counting.** A cross-node exchange stays live at its origin
   until it actually ends. That is approved from rev1.
2. **Entries per peer member.** Accounting state is kept per remote
   *instance*, an entry. An instance is identified by member index and an
   incarnation, a persistent per-member generation that only increases
   (2.1, rev4). Invariant M (2.1) gives at most one live instance
   per (group, shard, member). So entries per link are at most the
   group's member count, a structural constant.
3. **Bound Out connections.** An Out connection carries no counted DATA
   until it is BOUND. The receiving instance first proves it saw that
   exact connection, with a HelloAck naming the connection's nonce,
   written back on the same socket. That only identifies it; the exact
   binding SYNC (item 5) is also needed (2.4). The binding is never inferred from a reverse
   Hello. A link that is up but unbound is treated as down for queueing.
4. **END, coalesced per entry.** Debts of two instances are never mixed.
5. **SYNC for exact reconciliation.** Approved from rev1. Rev3 scopes it
   to the current connection only. HelloAck proves identity, and an exact
   current SYNC, sent before any counted DATA, makes the link BOUND. No
   history of old connections is kept (2.4, 2.5).
6. **Release by quarantine.** A peer instance is quarantined when its
   inbound side has been down, or it has reported itself blocked, for
   `T_release`:
   - its Out connection is reset;
   - no counted DATA goes to it until it is bound again;
   - its count stops gating X and becomes an explicit, non-increasing
     debt `D` (2.8).

   Rev3 adds a third trigger: an entry that has not been bound for
   `T_release` (2.8).

   A blocked primary also leaves the links after `T_block` (2.12).
7. **No adoption.** Rev1 let a shard count work it did not admit. Rev2
   instead drops an item whose counting instance cannot follow it. Every
   such drop coincides with an event that already runs `resume_all` on
   the ends that own the state. The resume pass recreates the work as
   roots, counted under X (2.9).
8. **Liveness without churn.** Heartbeats carry the sender's view of the
   reverse connection. A paused or backlogged connection is never closed
   for silence. There is no `TCP_USER_TIMEOUT` (approved from rev1).
9. **Wire v2, fail-closed.** Approved, with its effects stated honestly
   (2.11).

**Result (section 5).** For each origin shard o:

    physical items of o's exchanges  ≤  live_o + D_o + (o's share of U)
    live_o  <  X + max(E, Rr)                         (D.1, unchanged: no adoption)
    D_o     ≤  Q_o · (X + max(E, Rr))
    U_p     ≤  Q_p · (X + max(E, Rr))                 (dead, not-yet-retired instances' items at p)

Q_s is the number of (peer shard, member) pairs that s is linked to. It is
fixed by the cluster map.

- D_o never grows while quarantined. It is repaid at re-binding, or
  retired with the instance.
- U never grows. It goes when each peer sees the member's next instance.

Summed over the cluster, `Σ_s (1 + 2·Q_s)(X + max(E, Rr))`. That is
structural and independent of churn, timers and flap counts.

**Also new in rev2 after an independent check (section 12):**

- a link that is up but unbound is treated as down, so it never holds X;
- a blocked primary leaves the links after `T_block`;
- the reverse link steers to the live member.

## 1. What the code does today (verified)

| Fact | Where |
|---|---|
| Links are one-way. Each shard has one Out connection to every shard of every other group, and accepts In connections. The peer never sends on an Out connection. | `node_link.h:1-13`, `node_link.cpp:357-362` |
| Hello: 24 bytes, magic `SNCL`; wrong magic or crc refused. | `node_link.cpp:29-36`, `269-275` |
| A new Hello from the same (group, shard) closes the older In connection before any of its frames are read. | `node_link.cpp:284-289` |
| `drop_out` closes the socket (graceful close: kernel-buffered data may still be delivered) and clears the user buffer; the next attempt tries the next member. | `node_link.cpp:172-185` |
| Every connect, either direction, runs `resume_all`. | `node_link.cpp:279`, `355`; `shard.cpp:1852-1860` |
| Frames are 64 bytes; `pad[3]` is zero today. | `node_link.h:24-32`; `shard.cpp:3042` |
| Slot to shard within a group is fixed (`slot % n_shards`), so owners within a node are fixed. | `cluster.h:66`; `shard.cpp:3116-3118` |
| A link item not in this group by our map is dropped as misrouted, never forwarded across nodes. | `shard.cpp:2955-2964` |
| Leaving through a link ends the exchange at push; a down link's queue is purged and its exchanges end. | `shard.cpp:3026-3058` |
| Link items count against L until they run; their replies are new, unheld items. Replies wait for durability in `emit_wait_`. | `shard.cpp:1825-1827`, `3098`; `shard.h:670` |
| The listener binds without `SO_REUSEPORT`, so one process per port. A failed bind retries, while Out links connect regardless. | `node_link.cpp:108-127` |
| Withdraw destroys the links instance (listener, all In and Out) and pauses emits: queued follow-ups never leave. | `shard.cpp:1914-1920` |
| A fenced primary cut off from its standby and witnesses keeps its links. | `CLUSTER.md`, Limits |

**Consequences used below**

- An exchange between a (owned by A.o) and b (owned by B.p) travels only
  on o's links to p and p's links to o, until a slot moves between groups.
- A link item never waits for another link: it is dropped as misrouted or
  run by its fixed in-node owner.

## 2. Design

### 2.1 Instances and invariant M

- **Instance.** A links instance is one `NodeLinks` object. Its Hello
  carries `(group, shard, member, incarnation)`.
- **Incarnation = a persistent generation (rev4; PR #93 review item 2).**
  `incarnation` is a u64 that strictly increases across every links
  instance the member ever creates. It never depends on a clock or on
  randomness. The connection nonce stays random, for connection identity
  only.

  The engine keeps `links_incarnation_generation` in its data directory,
  in the file `links_generation`: magic, version, u64 generation and a
  CRC32C. Allocating one, while holding the data-dir lock below:

  1. read the file and verify its CRC;
  2. increment it, checked: overflow refuses to start;
  3. write it to `links_generation.tmp`, `fsync` the file, `rename` it
     over `links_generation`, and `fsync` the directory;
  4. only then return the value. It is used on the wire only after
     step 3 has completed.

  So it allocates before the listener opens and before any Out
  connection, at engine start, and again for every links instance
  it creates later: after a withdraw, a promotion, or a blocked primary
  rejoining (2.12 A). Allocations are
  serialized by one engine-wide mutex. One counter serves every shard of
  the engine, so each (group, shard, member) sees strictly increasing
  values. It costs one `fsync` pair per links instance, never per frame.

  **Crash consistency.**
  - The `rename` is atomic, so after a crash the file holds either the old
    value or the new one.
  - The old value can come back only if the crash came before step 3's
    directory `fsync` finished. In that case the new value was never put
    on the wire, so reusing the old one's successor is safe.
  - A file that is missing, truncated, or fails its CRC, in a data
    directory that already has WAL or checkpoint state, refuses to start.
    It is never treated as 0, and an older generation is never reused.
    Only a fresh, empty data directory starts at 1.

  **No data directory** (in-memory engines). Cross-node links refuse to
  start unless the engine is given `--links-generation-file <path>`,
  handled the same way and under its own `flock`. In-process tests use a
  test-local monotonic allocator shared by every engine of the test.
  `CLOCK_REALTIME` is never used.

  **Restoring an older data directory** can roll the generation back.
  Peers then refuse the member's Hellos (2.12 C), loudly and counted, and
  never silently accept a lower generation. This is a recovery exclusion
  with one explicit operator mechanism: start with
  `--links-generation-min G`, which stores `max(stored, G)` before the
  first allocation. G must exceed the highest generation the peers have
  seen for that member. Peers show that value in GAUGES and in the
  refusal log line.
- **New rule: no links before the listener.** An instance opens no Out
  connection until its listener is bound. Today Out links connect even
  while the bind is retrying (`node_link.cpp:116-123`).
- **New rule: a data-dir lock.** The engine takes `flock(LOCK_EX |
  LOCK_NB)` on its data directory at open, and fails to start if it is
  held.

**Invariant M.** At most one links instance is live per (group, shard,
member). Proof:

1. On one host, the member's link port admits one listening socket: there
   is no `SO_REUSEPORT`. The data-dir lock admits one engine per data
   directory.
2. An instance sends a Hello only after it holds the listener.
3. An instance releases the listener only when it is destroyed. That
   happens on withdraw (which also stops its emits), standby or stop.
4. So a second live instance for the same member would need the port and
   the lock while the first holds them, which is impossible.

**Deployment assumption.** Across hosts, M assumes that a member's address
names the host it runs on (`CLUSTER.md`). Running member m's data on
another host is outside the model, as it already is for HA.

**Corollary M1.** A Hello from member m with an incarnation different
from the one last seen from m means the older instance is destroyed. Its
sockets are closed and its emits stopped or gone. Its entry can be
**retired** at once (2.7).

**Corollary M2.** Entries per link ≤ `members(group)`. That is 2 for a
pair. This is the structural bound review item 4 asked for. There is no
"third incarnation releases the oldest" rule any more.

### 2.2 Wire format v2

**Hello: 48 bytes**

```
off  size  field
  0     4  magic = 0x324e4353 ("SNC2")
  4     2  group
  6     2  shard
  8     4  crc (cluster shape)
 12     4  version = 2
 16     8  epoch          (informational, as today)
 24     8  conn           (this connection's nonce: 64 random bits per connection)
 32     8  incarnation    (this links instance, 2.1)
 40     2  member         (this process's member index in its group)
 42     1  flags          (bit 0: accounting on, 2.12 D)
 43     1  zero
 44     4  conn_seq       (1, 2, 3, ... per (instance, link): orders this instance's connections, 2.5)
```

**Frame: 64 bytes, as today.** `pad[0]` is the kind, `pad[1]` the flags,
and `pad[2]` = 0.

| Kind | Meaning and fields |
|---|---|
| 0 DATA | As today. Flags: bit 0 COUNTED_BY_SENDER (the sender's exchange; the sender counts it), bit 1 COUNTED_BY_RECEIVER (a reply in the receiver's exchange). Exactly one bit is set with accounting on; neither is set in rollback (2.11). |
| 1 END | `op` = n ≥ 1; `source.hi` = the target incarnation, whose items ended |
| 2 SYNC | `source.hi` = `conn` named (the receiver's current Out connection, as the sender sees it); `op` = its `conn_seq`; `source.lo` = R; `target.hi` = k; `target.lo` = the target incarnation |
| 3 HEARTBEAT | `source.hi` = `conn` of the sender's current In connection from the receiver's instance (0 = none); `source.lo` = R on it; `op` bit 0 = the sender's reads are paused |

**HelloAck: 32 bytes**, written only by the accepting side, back on the
accepted socket (2.4). Fields: magic `SNCA`, conn echoed, the acceptor's
incarnation, member.

**Hello flags.** Bit 0 of the flags byte is accounting on. A mismatch is
refused like a version mismatch (2.12 D).

**HEARTBEAT** `op` bit 1 = the sender is blocked (2.12).

**Receiver checks.** A receiver applies an END or SYNC only if its target
incarnation equals the receiver's own incarnation. Otherwise it is
ignored and counted (`xnode_ignored_frames`).

**The sending instance** of any frame is the one named in the Hello of the
In connection it arrived on. That is the only reliable identity: the
process at the other end wrote it.

### 2.3 Per-link state

State is kept at shard s, per link l (one remote shard), per entry e (one
remote instance, at most `members` of them).

**As the origin, toward entry e:**

| Field | Meaning |
|---|---|
| `count_[l][e]` | s's counted items pushed to e and not yet ended or back |
| `gates_[l][e]` | `count_` is part of `live_s = started − ended` (now signed) only while this is true. It is false while QUARANTINED, and that `count_` is the debt D (2.8). Never added twice. |
| `cur_[l]` (per link, not per entry) | s's current Out connection: nonce, `conn_seq`, link state (UNBOUND, IDENTIFIED, SYNCED, BOUND), the entry it names (from the HelloAck or the binding SYNC), and `pushed`, the counted frames pushed on it. Created at connect, and cleared as a whole when the connection drops (rev1 F4). **No record of older connections is kept** (rev2 review item 1). |
| `state_[l][e]` | ACTIVE, QUARANTINED or RETIRED. Binding is a property of the link (`cur_[l]`), not of the entry. |

**As the peer, for origin entry e:**

| Field | Meaning |
|---|---|
| `held_for_[l][e]` | Items of e's exchanges now at s |
| `end_owed_[l][e]` | Coalesced ENDs owed to e |
| `in_conn_[l][e]`, `in_seq_[l][e]`, `rx_[l][e]` | The nonce and `conn_seq` of e's current In connection, and the counted frames received on it. Cleared when that connection closes. |
| `sync_owed_[l][e]` | A SYNC is owed to e |

**Link-level state:**

| Field | Meaning |
|---|---|
| `bound_[l]` | Shorthand, derived and not stored: `cur_[l].entry` when `cur_[l].state` is BOUND, else none |

**Per item.** Every Emit gets `peer = (link, entry slot, entry serial)`,
memory only. It is the counting instance on the other end, if the item
belongs to a remote origin. The serial bumps when a slot is reused, so a
stale tag never matches.

### 2.4 Binding an Out connection to an instance (review item 1)

**Mechanism: a Hello-ACK on the same socket.** The review allowed this
alternative. The rev2 draft bound through a SYNC on the reverse link, and
an independent check found that this can be impossible: the reverse link
picks its member on its own, and may be attached to a different instance
(section 12, rev2 G2). So the receiving end now answers on the accepted
socket itself.

1. s's Out connection to p connects, with Hello nonce c and the next
   `conn_seq` q. The link is UNBOUND, so no counted DATA is pushed.
2. p reads s's Hello and accepts it (2.12 says when p refuses). p then
   writes one 32-byte HelloAck back on that same accepted socket:

   ```
   u32 magic "SNCA", u32 zero, u64 conn = c, u64 incarnation (p's), u16 member (p's), u16 zero[3]
   ```

   It is the only thing ever written in that direction. Otherwise the
   link stays one-way. p also owes s a current SYNC naming (c, q), on
   p's own Out connection to s (2.5).
3. **IDENTIFIED.** s's Out side reads the HelloAck. Today a readable Out
   socket means closed (`node_link.cpp:357-362`). In v2 it reads up to 32
   bytes once, then treats anything further as a protocol error, and a
   hang-up as today. If `conn == c`, `cur_[l]` becomes IDENTIFIED with
   entry e, the entry for (p's member, p's incarnation). It is created if
   new, or it retires an older incarnation of that member (2.7). Still no
   counted DATA flows.
4. **SYNCED.** s reads, on an In connection whose Hello names e, a SYNC
   targeted at s that names (c, q). Since nothing counted was pushed on c,
   `pushed = 0`, and the SYNC sets `count_[l][e] = k` exactly (2.5).
   Still no counted DATA flows.

   Steps 3 and 4 may happen **in either order**. The HelloAck travels on
   s's Out socket and the SYNC on an In socket, two TCP connections, so
   s may read the SYNC first. A SYNC naming (c, q) cannot be stale: c is
   the current connection's random nonce, which only the instance that
   accepted c has seen. So s applies it at once, and records its sender
   as the entry `cur_[l]` names. The later HelloAck must name that same
   instance. If it names another, which invariant M rules out, s resets
   the Out connection and counts `xnode_bind_mismatch`. Ignoring the
   early SYNC instead would leave the link unbound until quarantine,
   since p sends no second one unprompted.
5. **BOUND,** once both the HelloAck and the binding SYNC have been read,
   naming the same entry e, and e's `blocked_since_` is not set (2.12 A).
   Then:
   - `cur_[l]` becomes BOUND;
   - `gates_` becomes true, which ends a quarantine (2.8);
   - **`resume_all` runs**, re-sending what was purged while unbound.

   Counted DATA may flow from now on.
6. Nothing else identifies or binds. A reverse Hello, a heartbeat or a
   SYNC that names another connection never changes `cur_[l]`.

**Why this is proof.**

- **Identity.** The HelloAck arrives on the same TCP connection that
  carried c, written by the process that accepted it. That process is
  the instance it names.
- **Accounting readiness.** The SYNC comes from that same instance,
  because the reverse In connection's Hello says so. It names c, which
  that instance alone has seen. It is exact because nothing counted has
  been pushed on c yet.

There is no inference from any other connection.

**Why one exact SYNC at binding covers all history.** k is everything e
physically holds of s's, whichever older connection brought it. Items
lost on any older connection are not in k, and nothing is in flight on c.
So `count = k` is exact for the whole entry, however many older
connections came and went, and no per-connection history is needed. Rapid
forward flapping creates no records: each drop clears `cur_[l]`, and the
count keeps a conservative over-count until the next binding sets it
exactly.

**Between a drop and the next binding,** `count_` still includes items
lost on the dropped wire. It over-counts, so admission is held back by at
most that count. That lasts until the next binding, or until quarantine
at `T_release` stops the count from gating.

**While a link is not BOUND (review G1).** UNBOUND, IDENTIFIED or SYNCED, the
link is treated as **down for queueing**:

- `run_emits` purges `emit_node_[l]` with `end_exchange`, as D.1 does for
  a down link;
- `route_emit` queues nothing there.

So a link that never binds holds nothing in `live_s`. That covers a
one-way failure (2.8), a peer that never acknowledges, and a dead reverse
path. Admission at s is unaffected beyond D.1. The binding runs
`resume_all`, which re-sends what was purged.

### 2.5 SYNC: exact reconciliation, current connection only (rev2 review item 1)

**p sends a SYNC to entry e.** `sync_owed_[l][e]` is set:

1. on reading a new Hello from e (step 2 of 2.4);
2. when p's own Out connection to s becomes IDENTIFIED with e, if e has a
   current In connection at p.

**Where it goes.** p pushes it only on an Out connection whose `cur_`
names e, in any state: IDENTIFIED, SYNCED or BOUND. If p's Out link is
attached to another member's instance, or not yet identified, the flag
and `end_owed_[l][e]` wait. They are never cleared by a SYNC that e
cannot receive (review of rev3 draft, finding 2). Member steering
(2.12 B) moves p's Out link to e's member when e is the one live.

**HEARTBEAT flows in every state** of p's Out connection, as in rev2.
**SYNC flows in every state once it is identified.** Only counted DATA
and END wait for BOUND. Otherwise each
direction's binding would wait on the other's SYNC, and neither would
ever bind (finding 1).

**Position in the stream: an accounting barrier (rev4; PR #93 review
item 1).** Rev3 let the SYNC go ahead of frames still in user space.
That was wrong. A frame takes its accounting step when `push` appends it
to `Out::buf` (`node_link.cpp:136-140`), well before `flush` hands it to
the kernel (`node_link.cpp:186-200`). So a counted reply sitting in
`Out::buf` is already out of `held_for_`. A SYNC placed ahead of it
would make s apply the reply after `count = k`: a double decrement.

The rule has two layers:

| Layer | What is in it | SYNC |
|---|---|---|
| Kernel socket buffer and `Out::buf` | Encoded frames, accounting already done | Never overtaken. The SYNC is **appended FIFO** after every byte already in `Out::buf`. |
| `emit_node_[l]` and owed END counters at the shard | Work not yet pushed, accounting not yet done | May be preempted: the owed SYNC is pushed before more of this is pushed. |

Its fields `(in_conn, in_seq, rx, held_for, e's incarnation)` are read
at the moment it is appended, and `end_owed_[l][e]` is cleared at that
same moment. Then:

- every counted reply or END whose effect k already reflects was
  appended earlier, so it is ahead of the SYNC in the byte stream;
- anything appended after the SYNC takes its accounting step after k was
  read, so k still includes it.

There is no prepend or priority insertion into `Out::buf`. The same
holds for every control frame, HEARTBEAT included (2.12 A). The SYNC
waits, as a flag, for room in `Out::buf`.

**FINAL is removed.** Rev2 had a FINAL SYNC for closed connections, and
its arithmetic was wrong. It added the `pushed − R` frames that a closed
connection never delivered, so it kept lost work as live (rev2 review
item 1). The binding SYNC of 2.4 makes it unnecessary: lost work drops
out of the count at the next binding, and quarantine covers a peer that
never comes back.

**s applies SYNC(conn, seq, R, k) from instance f, targeted at s, only
when all of these hold:**

- `conn` and `seq` equal `cur_[l]`'s nonce and `conn_seq`;
- f is the entry `cur_[l]` names, or `cur_[l]` names none yet (UNBOUND
  awaiting both, step 4 of 2.4: this SYNC then names f).

Then:

    count_[l][f] = k + (cur_[l].pushed − R)

For the binding SYNC, `pushed = 0`, so `count = k`. **Every other SYNC is
ignored** and counted as `xnode_syncs_stale`. That includes one naming an
older connection, an older seq, another instance or another link. So a
SYNC can never raise or overwrite state from a superseded connection.

While `gates_` is true, every change to `count_` changes `ended_own_` by
the opposite amount, so `live_s` stays `started − ended`. While it is
false (quarantine), only the debt changes.

**Ordering of control frames from one instance.** p's SYNCs, ENDs and
counted replies to s reach s in p's emission order. Within one of p's Out
connections, TCP keeps the order. Across p's reconnects, s closes p's
older In connection (same member) on reading the newer Hello, before
reading any frame of the newer one (`node_link.cpp:284-289`, per member
in v2). After that, no frame of the older connection can be read.

So a frame that p emitted before a SYNC is applied before it, and one
emitted after is applied after. Nonces give identity and `conn_seq`
gives order. Neither is used for the other job (the review's point).

**Exactness argument.** A SYNC follows, in p's stream, every END and
counted reply p emitted before computing k, so s has applied those
first. Every item s counted for f is then in exactly one of three
places:

1. at f: in k;
2. lost on a dropped wire, or in a purged queue at f: it is neither in k
   nor in flight, so it drops out;
3. in flight on the current connection c: `pushed − R` of them, still
   to be received and counted at f.

The SYNC names c, so these are exactly the frames on c, and older
connections contribute nothing in flight.

### 2.6 Lifecycle of each item, and END (review item 3)

**Push from s onto link l:**

| Item | Rule |
|---|---|
| s's own exchange (`origin == id_`) | Only if `bound_[l] = e`: flag COUNTED_BY_SENDER, `count_[l][e]++`, `cur_[l].pushed++`. Unbound links queue nothing (2.4). |
| A reply whose `peer` tag names entry e on l, with `bound_[l] = e` and the serial current | Flag COUNTED_BY_RECEIVER, `held_for_[l][e]--`. |
| A reply whose tag does not match l's bound entry (another link after a slot move, or another instance of the same link) | **Drop** (2.9): `held_for_[tag]--`, `end_owed_[tag]++`. |
| An exchange of a local shard x ≠ s (a re-route after a slot move) | **Drop**: `end_exchange(x)`, as D.1 does at push. |
| A tagged reply whose target slot moved into s's own node (an in-node hand-off to shard q) | **Drop** (2.9), and END on the tag (review G6). It is never handed on untagged. |
| Uncounted (`origin = kNoShard`, no tag) | Cannot occur with accounting on: every arrival is tagged or local. Debug abort, gauge in release. |

**Receipt at s, on an In connection from instance f (entry e of link
l).** This is applied before the misroute check, which today returns
before the Emit is built.

| Frame | Rule |
|---|---|
| DATA, COUNTED_BY_SENDER | `rx_[l][e]++` and `held_for_[l][e]++`, and the Emit is tagged e. Its replies inherit the tag through `cur_peer_`. If it is then dropped at receipt (misrouted, stale), that is an ordinary disposition and owes an END (review G9). |
| DATA, COUNTED_BY_RECEIVER | s's own exchange returns: `count_[l][e]--`, whatever the state. The Emit is then counted locally (`origin = id_`) as in D.1. If `count_` is already 0, drop it and count `xnode_end_underflow`. |
| END(n) for s | `count_[l][e] −= n`. A negative result is clamped, counted in `xnode_end_underflow` (must stay 0), and aborts in debug. |

**Every disposition of a tagged item, other than its counted reply, owes
an END on its own tag (review item 3, rev1 F1).** That covers:

- a drop: stale, misrouted, withdrawn, a tag mismatch;
- a run that emits nothing;
- a purge of any link's queue;
- an in-node hand-off after a slot move.

In every case: `held_for_[tag]--` and `end_owed_[tag]++`. `end_owed_` is
per entry, so two instances' debts are never combined.

**END delivery.** `end_owed_[l][e]` is pushed as one END(n, target = e)
only when `bound_[l] = e`. While l is bound to another entry, or
unbound, it waits as a counter: no memory growth, and no wait on
anything. A SYNC to e clears it (2.5).

### 2.7 Incarnation transitions at the peer (review item 3)

The peer's state is per entry, which is option A of the review. Every
field's transition is defined below.

**Entry RETIRED.** Member m presented a new incarnation, in a Hello or a
HelloAck (corollary M1), and the older incarnation's In connection is
closed or silent (2.12 C). That covers a QUARANTINED entry too. In both cases the old instance is destroyed. In one step
(single shard thread):

| Field | Transition |
|---|---|
| `held_for_[l][e]` | Set to 0. Every item tagged e is **purged eagerly** in one pass over `emit_local_`, `emit_remote_`, `emit_node_`, `emit_wait_` and the held-emit list. These are dropped with no END (their instance is gone). |
| `end_owed_[l][e]` | Discarded, never sent: no instance can receive it, and it is never merged into another entry. |
| `in_conn_`, `in_seq_`, `rx_`, `sync_owed_` | Cleared |
| Queued DATA carrying the tag | Purged with the items above |
| Replies waiting for durability (`emit_wait_`) | Purged with the items above |
| `count_[l][e]` (s as origin toward e) | Set to 0, with `ended_own_` adjusted if `gates_`: e's memory is gone, and so are the items. |
| `cur_[l]` names e, in **any** state (IDENTIFIED, SYNCED or BOUND) | Reset the Out connection (`SO_LINGER{1,0}`), remove it from epoll, and clear `cur_[l]` (review of rev3 draft, finding 6) |
| A later HelloAck naming e's incarnation | Refused like a Hello naming it (2.12 C): the Out connection is reset |

The slot is freed, its serial bumped, and the new incarnation takes a slot
as a new entry. The work behind every purged item was durable state at
its two objects, and the new instance's connect ran `resume_all` at both
ends. So the purged follow-ups are re-sent as roots under X.

**Two live members.** Two instances of different members of the same
shard (old fenced primary and promoted primary) are two entries. They
coexist: each has its own In connection, its own counts and its own END
debt.

- The In-replacement rule becomes per (group, shard, member): a new Hello
  closes an older In connection only from the same member. Today it is
  per (group, shard). Each member's frames stay in the order that member
  sent them, which is what the rule protects.
- The two members' Out connections no longer knock each other off at o,
  so the rev1 ping-pong is gone (question 10.6).
- **Ordering (review G10).** The rule protects one sender's order: a
  credit must not overtake its own forget (`TRANSFERS.md`). That order is
  within one instance's stream, and per-member replacement keeps it.
  Between two members there was never an order to keep. Today's
  per-(group, shard) replacement already interleaves the two members'
  streams at every reconnect, and it loses each one's undelivered tail.
  Per-member streams are at least as ordered: each stays FIFO, and the
  receiver sees no more interleaving than the ping-pong produced. Both
  members emit only idempotent transfer follow-ups for objects they both
  hold. The fenced one's follow-ups are bounded by `T_block` (2.12 A).

**Invariant P (peer side).** For every live entry e:

    held_for_[l][e] = |items at s tagged (l, e, serial)|

Every increment is at a tagged receipt. Every decrement is at exactly one
disposition of one such item: its counted push, an END-owing disposition
or the retirement purge. ENDs owed are per entry.

### 2.8 Release by quarantine (review item 2)

**One counter per entry.** The rev2 draft split it into `remote_` and
`debt_`, and an independent check found states where items sat in both
or neither (section 12, G4). So each entry has one counter `count_[l][e]`
and a flag `gates_`:

- `live_s` includes `count_` only while `gates_` is true;
- the debt D is Σ `count_` over entries whose `gates_` is false;
- every SYNC, END and counted-reply rule in 2.5 and 2.6 acts on `count_`,
  whatever the flag;
- flipping the flag moves `count_` into or out of `live_s`, with
  `ended_own_` adjusted (signed).

**Trigger.** Any of these holds:

1. s's In side from entry e has been down for `T_release`, with no open
   In connection from e;
2. `now − blocked_since_[l][e] ≥ T_release`. `blocked_since_` is set at
   the first HEARTBEAT from e with the blocked bit. It is cleared only by
   a HEARTBEAT from e without the bit, or by e's retirement. It is **not**
   reset when e's connections close, or when e leaves the links (2.12 A):
   an instance that blocked and then left keeps its original deadline
   (rev2 review item 3).
3. e has a nonzero `count_` but has not been s's BOUND entry on link l
   for `T_release`. For example, s's Out connection failed over to
   another member while e stays connected and unblocked.

   No SYNC can ever apply to e again unless it is bound, so items lost on
   its last wire would otherwise gate X forever. Quarantine is safe here:
   no counted DATA goes to an entry that is not bound anyway.

**Steps.** s performs them atomically, before the released count stops
gating:

1. `state_ = QUARANTINED`, and `gates_ = false`.
2. If `cur_[l]` names e, in any state:
   - reset the Out connection (`SO_LINGER{1,0}`), which discards s's
     unsent send queue. Frames already in flight, or in e's receive queue,
     may still be counted at e; they are in `count_`;
   - clear `cur_[l]`.

   The link is UNBOUND, so it is down for queueing (2.4) and its queue is
   purged with `end_exchange`.
3. Admission at s reopens under X. Roots for p are not queued while the
   link is unbound.

**Leaving quarantine.**

| Event | Effect |
|---|---|
| A binding to e (2.4): the HelloAck and the exact current SYNC over e's live In connection, in either order (both directions work by construction, 3.3) | `count_ = k` (exact, when the SYNC is read), `gates_ = true` at BOUND. One of the two alone leaves the link not BOUND, still down for queueing. |
| Retirement of e (2.7) | `count_ = 0` (`gates_` was false, so `live_s` does not change) |

**While quarantined,** ENDs and counted replies from e still act on
`count_`. The only SYNC that can apply is the binding SYNC of a new
connection, which leads to BOUND and so ends the quarantine. A counted reply decrements it and becomes s's local item,
which enters `live_s` (+1, while the debt falls by 1).

**Why the debt cannot grow.**

- While e is QUARANTINED, no counted DATA can reach it. That would need a
  binding, and a binding makes the state BOUND.
- `count_` therefore only falls, or is corrected downward by exact SYNCs.
- The only SYNC that can apply while quarantined is a binding SYNC
  (pushed = 0), which sets `count_ = k`. It cannot raise the count:
  every item e holds of s's was counted by s, and its END or counted
  reply has not been sent, so `count_ ≥ k` whenever the SYNC is read.
  ENDs and replies only lower it. So no control frame can raise the
  count.

So D per entry is non-increasing while quarantined. Each further
quarantine needs a binding first, and a binding resets nothing upward.
**A one-way partition cannot accumulate X per release.**

### 2.9 Drop-to-resume instead of adoption (review item 5)

Rev1 adopted items whose counting shard could not follow them. That put
`live_s` above X by an amount that churn could grow. Rev2 drops those
items instead. Every drop is one of the dispositions D.1 already has: a
follow-up lost with its link is re-sent by resume. Each drop happens
only at an event that runs `resume_all` on the shards owning the two
objects:

| Drop | Resume trigger |
|---|---|
| Tag mismatch at push (slot moved, or another instance bound) | Map change, or the binding (which runs `resume_all`, 2.4) |
| In-node hand-off of a tagged reply (slot moved into this node) | Map change |
| Purge of an UNBOUND link's queue (2.4) | The binding |
| Re-route of a local exchange to another node | Map change |
| Counted reply with no count | The connect that delivered it |
| Arrival for a RETIRED or replaced entry | The new instance's connect |
| Retirement purge | The new instance's connect |

**The resume pass** re-sends from durable object state (outbox and inbox
entries). Its roots are paged under X, as in D.1 3.9.2. Transfer
follow-ups are idempotent (`TRANSFERS.md`), so a drop costs latency,
never correctness.

**Bound.** No follow-up is ever counted by a shard that did not admit
its exchange's root. So `live_s < X + max(E, Rr)` holds exactly as in D.1,
with no adoption term.

### 2.10 Liveness: heartbeats and silence (rev1 F3, approved)

- **Heartbeat.** Each Out connection idle for `T_hb` pushes a HEARTBEAT.
  It carries the sender's view of its In connection from the receiver:
  conn, R and paused.
- **In side.** An In connection is closed as silent after `T_dead` with no
  bytes read, but only while this shard's reads are not paused and the
  socket has no unread bytes (`FIONREAD` = 0). A paused or backlogged
  connection is backpressure, not death.
- **Out side.** There is no `TCP_USER_TIMEOUT`. s resets its Out
  connection when either of these holds for `T_dead`:
  - the peer's heartbeats name another conn, or none: the peer does not
    see this connection;
  - the peer is not paused, but its R does not advance while frames are
    outstanding.

  With no reverse connection at all, quarantine (2.8) applies.

**Timer constraints.** The engine refuses to start unless
`T_hb < T_dead < T_release`, with `T_dead ≥ 3·T_hb`, and
`T_block ≥ T_dead`.

Defaults, as hypotheses with overrides, all shown in GAUGES:

| Timer | Default |
|---|---|
| `T_hb` | 100 ms |
| `T_dead` | 1 s |
| `T_release` | 3 s |
| `T_block` | 2 s |

No safety argument in this note depends on the values. They decide only
how soon a failure is detected and released.

### 2.11 Version policy (Q1 approved: fail-closed) and rollback

**Version refusal**

- A v1 node reads `SNC2` as "not a node link" and closes.
- A v2 node reading `SNCL` refuses with:

  > node link: refused group G shard S: wire v1, this node speaks v2
  > (upgrade every node)

  and counts it (`link_version_refusals`).
- No dual-stack.

**Effect on a mixed cluster** (corrected wording, review item 6). Client
request and response transport does not use node links, so it is not
refused. Applications can still see the effect:

- Cross-node follow-ups cannot cross between v1 and v2 nodes, so they do
  not settle. A transfer to an account on a non-upgraded node stays
  pending: debited, not yet credited.
- Its follow-ups wait. Those queued for a down link are purged and
  resumed, so admission is not held by them (`live_o` does not fill).
- They settle after the upgrade, when links connect and `resume_all`
  runs.
- A long mixed window is therefore a settlement outage for cross-node
  transfers. It is not a correctness issue: nothing is lost or applied
  twice.

X9 tests exactly this.

**Rollback.** `SNC_XNODE_ACCOUNTING=off` keeps the v2 wire, heartbeats and
silence, and restores D.1 counting: end at push, no flags, no END and no
SYNC. Binding is not required in rollback. `SNC_FOLLOWUP_INTAKE=off`
implies it.

### 2.12 Blocked primaries, member steering and enforcing M at the peer (review G2, G3, G8)

An independent check of the rev2 draft showed two availability failures,
both from the documented fenced-primary limit (`CLUSTER.md`, Limits):

- a primary cut off from its standby and witnesses keeps its links;
- it never makes anything durable;
- the peers' Out connections stay attached to it.

**A. A blocked primary leaves the links (new, small HA change).**

- A primary that has been **blocked** for `T_block` withdraws from the
  node links: it destroys its links instance, listener included. Blocked
  means it is not acknowledging durable writes because its standby is
  gone (sync profile).

  The code has no such state today; `CLUSTER.md`:270-276 names it only as
  a limit. D.2 defines it: sync replication is on, and either the standby
  connection is down, or the oldest waiter for a standby ack is older than
  `T_dead`. It is read on the shard thread from existing waiter state.
  `T_block` is then only the delay before leaving the links, not part of
  the definition, so it is not counted twice (review of rev3 draft,
  finding 7).
- It recreates them, as a **new instance** with a new incarnation, when it
  is unblocked.
- Its HEARTBEAT carries a blocked bit from the moment it blocks. A
  change of the bit sends a HEARTBEAT **at once** on every identified Out
  connection, not only after `T_hb` of idleness. It is appended FIFO to
  `Out::buf`, ahead only of work not yet pushed (the barrier of 2.5).
  `Out::buf` is capped (`kOutCap`), so it reaches the peer behind at most
  that many bytes. So peers start
  `blocked_since_` one delivery delay after the block.
- **No binding to a blocked instance.** s never makes a link BOUND to an
  entry whose `blocked_since_` is set. A reconnect that lands on the
  blocked primary again stays at most SYNCED, so the count cannot gate
  again, and quarantine cannot cycle, before it leaves. They quarantine it at `blocked_since_ +
  T_release`, whether it is still linked or has already left (2.8).

`CLUSTER.md` names this as the next step ("withdrawing while blocked ...
needs no new protocol"). Data and clients are unchanged: it still blocks
writes as today.

**The window, from the written state machine (rev2 review item 3).**
Take t0 as the moment the primary blocks.

| Time | Event |
|---|---|
| t0 + δ (δ = one delivery delay) | Peers see the blocked bit and set `blocked_since_` |
| t0 + `T_block` | The primary leaves the links. Peers' Out connections fail over to the promoted member, and their In side from the old instance goes down. Its `blocked_since_` is kept |
| t0 + δ + `T_release` | Peers quarantine the old entry, so its count stops gating X |

So the old instance's count can hold a peer's admission for at most
`δ + T_release` after it blocks. X19 allows δ ≤ `T_hb`.

**If the bit cannot be delivered** because the Out connection to a peer
has no room (that peer is not reading), the peer learns nothing until
the primary leaves at t0 + `T_block`. Its In side is then down, so the
In-down trigger quarantines at most t0 + `T_block` + `T_release`. X19b
checks this. The promoted member's own entry is
separate (2.7). It starts gating only from its own binding, with an
exact count.

A primary that blocks while it cannot reach a peer at all is covered by
the In-down trigger: at most `T_dead + T_release` after its last frame.
X19 checks the first deadline, X19b the full-buffer one, and X7b the
unreachable one.

**B. Member steering.** p's Out link to group A shard o normally tries
members in turn (`node_link.cpp:172-185`). In v2 it also drops and moves
on when, for `T_dead`, both hold:

- p has an open, live In connection from another member of that shard;
- p has none from the member its Out link is attached to.

So the reverse path follows the live instance, and ENDs and SYNCs reach
it.

**C. Enforcing M at the peer (review G8).**

- **Only a newer incarnation is accepted.** p keeps, per member, the
  highest incarnation it has seen (`xnode_highest_incarnation`, in
  GAUGES). A Hello or HelloAck naming a lower one is refused and counted
  in `xnode_m_conflicts`. It can only be a delayed connection from a
  retired instance, or a restored data directory (2.1). One u64 per
  member replaces any list of retired incarnations, and a delayed old
  Hello can never retire the live entry. The generation is persistent and
  strictly increasing (2.1), so a legitimate new instance is always
  higher, whatever the clock does.
- **When a newer incarnation is accepted.** p accepts a Hello or HelloAck
  naming member m with a higher incarnation only once the older
  incarnation's In connection from m has closed, or has been silent for
  `T_dead`. Until then the new connection is refused and retried.
- **Fencing before retirement (rev2 review item 2; part of M1).** When the
  older incarnation's transport is not yet observed closed, p fences it
  before anything else, in this order and on the shard thread:
  1. it removes the older In socket from epoll, resets it
     (`SO_LINGER{1,0}`, then `close`), and destroys the In object, which
     discards frames already read into user space, as `close_in` does
     today (`node_link.cpp:230-234`). Bytes still buffered or in flight
     on it can no longer be read, because no reader exists;
  2. it resets any Out connection whose `cur_[l]` names the older entry,
     in **any** state (IDENTIFIED, SYNCED or BOUND), removes it from
     epoll and clears `cur_[l]` (review of rev3 draft, finding 6);
  3. only then does it retire the entry (2.7): purge, zero, discard END
     debt;
  4. only then does it accept the new incarnation's connection.

  A retired entry therefore has no socket left that could deliver a
  frame. Any later frame claiming that incarnation would need a new
  connection, and its Hello or HelloAck is refused, because its
  incarnation is below the member's highest.
- **The same fencing applies to observed closes,** where steps 1 and 2 are
  no-ops for sockets already gone.
- **Restarts.** A crashed process's connections close at once. A host
  that died hard is detected by silence. So this delays a restart by at
  most `T_dead`.
- **If M is violated anyway** (two live instances of one member, for
  example in separate network namespaces on one host), each new Hello is
  refused while the other lives, so the two never retire each other.
  `xnode_m_conflicts` counts the refusals and the engine logs them
  loudly.
- **Engines with no data directory.** They are in-memory test engines,
  with no lock. They rely on the port, and on this check.

**D. One accounting setting per cluster (review G7).** The Hello carries
an accounting bit. A mismatch is refused like a version mismatch, so
`SNC_XNODE_ACCOUNTING` is cluster-wide.

## 3. Lifecycle cases

### 3.1 s's Out connection to p drops (s → p wire lost)

1. s clears `cur_[l]`, keeping no record of the old connection. The
   count stays conservatively and still includes what was lost on the
   wire. The link is not bound, so its queue is purged (2.4).
2. s reconnects with a new nonce and the next `conn_seq`.
3. p closes the old In connection, either on reading the new Hello or on
   the hang-up. It writes the HelloAck, and sends a current SYNC naming
   the new connection. Its `rx` is 0 and k is what p holds.
4. s reads the HelloAck and the SYNC, in either order (2.4); the SYNC
   sets `count = k`, and both together make the link BOUND. The lost
   items drop out exactly here. Then it runs `resume_all`.
5. However many times steps 1-3 repeat before a SYNC gets through
   (X24), only the SYNC naming the current connection can apply. Every
   earlier one is ignored, so no lost generation is resurrected.

### 3.2 p's Out connection to s drops (p → s wire, its queue and ENDs lost)

- p purges its queue toward s. Every tagged item owes an END, and the
  counters stay.
- s sees its In connection from p close. Its count stays (conservative).
- p reconnects. Once s's HelloAck identifies that connection with s's
  entry, p's first frame is a SYNC naming s's current Out connection,
  which is exact.

### 3.3 One-way partition: s → p up, p → s dead (review item 2)

- After `T_release`, s quarantines p (2.8). It resets its Out connection,
  and the count stops gating X.
- s's Out connection reconnects. p accepts it and writes the HelloAck,
  which travels on the s → p connection's own reverse path. A "p → s
  dead" failure may or may not carry it:
  - (a) if the ack is lost, s's link stays UNBOUND;
  - (b) if the ack arrives (only p's own Out connection is down), the
    link is IDENTIFIED, but the binding SYNC can only travel on p → s,
    which is dead. So it is never SYNCED, and never BOUND.
- In both variants s never reaches BOUND. Its queue for p is purged
  while unbound, no counted DATA reaches p, and the debt stays fixed.
  Admission at s is not held, which is the D.1 behaviour.
- So quarantine is left only when both directions work, by construction
  of the binding (2.4). No extra rule is needed.

X14 checks both variants.

### 3.4 Both directions, then heal

1. Silence detects the loss, then quarantine applies.
2. On heal, the binding needs both directions: the HelloAck goes s → p,
   and the SYNC comes p → s.
3. The binding SYNC sets the count exactly, and `gates_` turns true.

### 3.5 Crash and restart of o (same member)

- The new instance draws a new incarnation. Its connections can be
  accepted once the old process's connections are closed: at once after
  a crash, or after `T_dead` of silence for a dead host (2.12 C).
- p reads its Hello (member m, new incarnation). Corollary M1 applies,
  so p retires o's old entry: eager purge, END debt discarded, counts
  zeroed.
- New o binds with p's HelloAck and runs `resume_all`. The purged
  follow-ups are re-sent as roots under X.

### 3.6 Failover of p's group; the old primary fenced but running

- o's Out connection drops and reconnects to member m′, which is p′. The
  HelloAck and the current SYNC from p′ bind it, with an exact k.
- o's entry for old p keeps its count, but it is not bound, so no DATA
  reaches it. Old p's In connection to o survives, because the
  replacement rule is now per member. Its ENDs and counted replies
  decrement the old entry. Its SYNCs are ignored, since they do not name
  o's current connection.
- The old entry stops gating at the earliest of three deadlines (2.8):
  - blocked, at `blocked_since + T_release`;
  - In down, at `T_release`;
  - not bound, at `T_release`.

  After `T_block`, old p leaves the links (2.12 A). The old entry retires
  when its member's next instance connects, after fencing (2.12 C).
- p's Out connection to A.o is steered to the member whose In connection
  is live (2.12 B), so the ENDs and SYNCs for p′'s exchanges reach o's
  live instance.

Nothing from old p can touch p′'s entry. X7b, X13, X15, X19 and X20 check
this.

### 3.7 A slot moves to another group mid-exchange

- p's reply to a is now routed to C.z, and its tag is o's entry on o's
  link. That is a tag mismatch, so p drops it and owes o an END.
- The map change ran `resume_all` at a's new owner, which re-sends what
  is unsettled.
- An item arriving at p for a slot that is no longer in its group is
  dropped as misrouted, and owes an END.
- If the slot moved into p's own node, the reply is not handed on
  untagged: p drops it and owes the END (2.6).

### 3.8 Map epochs, withdraw, standby, stop

- **Map epochs.** Unchanged. A held newer-epoch frame stays tagged and
  counted.
- **Withdraw.** It destroys the instance:
  - its queued follow-ups never leave (`shard.cpp:1920`);
  - the peers' entries for it go quiet, then quarantine, then retire
    when its member's next instance connects.
- **Standby.** No links.
- **`quiesced()`.** It does not wait for remote ENDs, so a clean stop
  waits for no other node.

## 4. No double end, no leak (acceptance items 2 and 3)

### 4.1 Invariant O (origin side)

For each origin entry e at s:

    count_[l][e] ≥ |s's counted items pushed to e: on the wire, at e, or on the wire back|

It is equal after an exact SYNC. `gates_` decides only whether the count is
in `live_s` (ACTIVE, bound or not) or in the debt (QUARANTINED). The count itself is one
number, and it is never split (review G4). It is zeroed only at
retirement, when the instance and its items are gone.

**Increments.** Only at a counted push to the bound entry (2.4, 2.6). The
HelloAck proves that the bound entry is the instance receiving the
frames.

**Decrements.** Only by:

1. an END from e targeted at s. p emits at most one END per tagged item,
   at its single non-reply disposition, and that includes drops at
   receipt. ENDs are kept per entry, and are cleared at a SYNC that
   already excludes them;
2. a counted reply from e. p emits it at most once per item, and never
   for an item that owed an END: the two are disjoint branches of one
   disposition;
3. an exact SYNC, which sets the true value (2.5);
4. retirement.

Frames are attributed by the Hello of the connection they arrive on, so
nothing from instance f can touch entry e ≠ f.

**No double end.** Each item is decremented at most once, or removed by
an exact SYNC that does not count it.

### 4.2 No leak

**Every item that disappears without an END or a reply** was lost on a
wire or in a purged queue at the peer. Both are connection events:

- the sender's next binding applies an exact current SYNC, `count = k`,
  before any counted DATA;
- if no binding comes, the entry is quarantined: In down, blocked, or
  not bound for `T_release`.

The binding SYNC removes the lost item exactly. Drops at receipt owe ENDs
(2.6), so they are not left to a SYNC (review G9).

**A peer that never sends a SYNC** is quarantined, so its count leaves
`live` and admission is not held. That count is cleared when the instance
retires. If the instance comes back, the count is corrected by its SYNCs.

### 4.3 Checked at every sample, and at quiescence

- **At every sample:** `count_ ≥ 0` and `xnode_end_underflow` = 0.
  `xnode_ignored_frames` and `xnode_m_conflicts` are 0 except in tests
  that inject them.
- **At quiescence:** every `count_`, `held_for_`, `end_owed_` and `live`
  is 0.

## 5. B2: the cluster-wide structural bound (review item 5)

### 5.1 Conservation and admission

The lemma of `FOLLOWUP_INTAKE_DESIGN.md` 6.2 holds: each exchange has at
most one item, and a drop ends the exchange.

A shard creates an exchange only by:

- client admission while `live_s < X`;
- a resume root, paged while `live_s < X`.

No shard ever counts an exchange it did not create (2.9). So:

    live_s < X + max(E, Rr)        at every shard, at all times.

### 5.2 Where each physical item is accounted

Take an exchange created by o. Its one item is in exactly one of these
states:

| State | Accounted in |
|---|---|
| Local at o | o's local part of `live_o` (D.1) |
| Pushed to a BOUND entry e (wire, at e, or wire back) | `count_[l][e]`, gating, so in `live_o` |
| At a QUARANTINED entry e | `count_[l][e]`, not gating: debt D_o |
| At a peer p2, tagged with an incarnation of o that is dead but that p2 has not yet retired (it has not yet seen the member's new instance) | Nowhere: the term U below (review G5) |
| An END or reply in flight | Still counted until it lands: an over-count |

The rev2 draft claimed that an item at a retired entry "does not exist".
That holds only once every peer has retired it, and retirement is local
to each peer. So the U term is added.

### 5.3 The terms are structurally bounded

**Debt D.**

- At quarantine, an entry's count is at most `live_o` < X + max(E, Rr).
- While quarantined, the count never rises above what that instance
  still holds of o's. No DATA is sent to it, and leaving quarantine needs
  both directions to work (3.3).
- At most `members(group)` entries per link exist (M2).

So:

    D_o ≤ Q_o · (X + max(E, Rr)),     Q_o = Σ over peer shards p of members(group(p))

**Unretired dead incarnations U.**

- Per peer shard p2 and per (remote shard, member), at most one dead
  incarnation can remain unretired. Any later instance of that member
  retires it as soon as p2 sees its Hello or HelloAck.
- A later instance that never reaches p2 never had items at p2.
- The items tagged with it are at most p2's `held_for_` for it, which is
  at most what that incarnation counted: < X + max(E, Rr).
- They never grow, because the dead instance sends nothing more.

So:

    U_p2 ≤ Q_p2 · (X + max(E, Rr))

**Cluster-wide:**

    Σ physical items of all exchanges  ≤  Σ_s (1 + 2·Q_s) · (X + max(E, Rr))

This is independent of flaps, releases, restarts, failovers, slot moves
and timer values. Those change *which* entries hold D or U, never how
many, nor any entry's ceiling.

The constant is loose. In practice D and U sit at unreachable or dead
instances.

### 5.4 Per-queue consequences (stalled destination, item 7)

**When p stalls (slow flush, or no reads):**

- **p's held link items:** ≤ L, and p's reads pause (D.1).
- **o's Out buffer:** ≤ `kOutCap`.
- **o's `emit_node_[l]`:** o's own items plus replies to p's exchanges,
  so ≤ `live_o + live_p` < 2(X + max(E, Rr)).
- **o's clients:** they get `E_OVERLOADED` once `live_o` reaches X.

**p's queue toward a slow o:** ≤ `live_o + live_p` by the same argument.

**Control state per link:** per entry, one END counter, one SYNC flag and
a few words; per link, one heartbeat timer. None of it grows with load.

## 6. The other acceptance items

### 6.1 Recovery and resume (item 5)

- Counters are memory only.
- Recovery is replay, then `resume_all`, then links. The peers retire the
  old instance (3.5).
- Resume roots stay paged under X.
- The bound does not depend on counts surviving a crash. A crashed
  instance's items are purged at its peers, and its state is re-sent
  from durable objects.

### 6.2 Compatibility and version (item 6)

Covered in 2.11, with the honest availability statement.

### 6.3 Response and control progress (item 8)

- Rings drain fully.
- Client request, response and admin traffic never uses node links.
- The intended client-visible effects are `E_OVERLOADED` from X, and
  delayed cross-node settlement while a link is unbound, quarantined or
  refused.

### 6.4 No cyclic deadlock (item 4)

The waits are those of `FOLLOWUP_INTAKE_DESIGN.md` 6.4, (i) to (iv),
plus these:

- **(v) A frame waits for Out room.** Out room depends on the peer
  reading, and the peer reads once its held link items are below L/2.
  Those drain through the peer's own WAL, because a link item never
  waits for another link (section 1).
- **(vi) Nothing waits for binding.** An unbound link is down for
  queueing (2.4), so its items are purged and ended, and are re-sent by
  the resume at binding. The HelloAck itself is written by the peer's
  accept path, which waits for nothing.
- **(vii) END waits for room, or for its entry to be bound. SYNC waits
  only for room on an identified connection, and HEARTBEAT only for
  room (2.5).**
  Identification needs only the peer's accept path, so there is no
  cycle.
  Nothing waits for them: a missing END keeps admission refused (iv),
  which is a refusal, not a wait.

Every wait chain still ends at a local flush. An unbound or quarantined
link delays only cross-node settlement over it. It never holds admission
(review G1).

### 6.5 Sanitizers (item 10)

Release, ASan+UBSan and TSan full serial suites, plus P2. Every run is
reported in order.

## 7. Gauges

Per shard. Per-entry state is summed, plus a count of entries by state.

| Kind | Gauges |
|---|---|
| Configuration | `xnode_accounting`, `xnode_hb_ms`, `xnode_dead_ms`, `xnode_release_ms`, `xnode_block_ms` |
| State now | `xnode_remote_live`, `xnode_debt`, `xnode_held_for_peers`, `xnode_end_owed`, `xnode_links_unbound`, `xnode_entries_bound`, `xnode_entries_quarantined`, `xnode_blocked` (this shard has left the links while blocked) |
| Counters | `xnode_bindings`, `xnode_syncs_sent`, `xnode_syncs_exact`, `xnode_syncs_stale` (ignored: not the current connection), `xnode_identified`, `xnode_bind_mismatch`, `xnode_fenced`, `xnode_sync_correction` (Σ \|change\|), `xnode_ends_sent`, `xnode_ends_received`, `xnode_quarantines`, `xnode_retirements`, `xnode_retire_purged`, `xnode_drops_to_resume`, `xnode_ignored_frames`, `xnode_end_underflow` (must stay 0), `xnode_heartbeats_sent`, `xnode_silence_closes`, `xnode_out_resets`, `xnode_unbound_purged`, `xnode_steers`, `xnode_m_conflicts`, `xnode_block_leaves`, `link_version_refusals`, `link_accounting_refusals` |

`xnode_remote_live` is Σ `count_` over gating entries, and `xnode_debt` is the same over quarantined entries. `intake_exchanges_live` is `started − ended`. It includes the first and excludes the second.

## 8. Tests

### 8.1 Positive tests

The tests use two or three engines in one process over loopback, with a
fault proxy where noted. X6, X7, X7b and X9 use `snc_server` processes.
"Physical" means Σ over peers of `held_for_`, plus the local queues, as
read from GAUGES.

| Test | Setup | Pass criterion |
|---|---|---|
| X1 stalled remote | B's flush stalls; A's clients run cross-node transfers flat out | A's `emit_node_` ≤ `live_A + live_B`; `live_A` < X + max(E, Rr); Out buffer ≤ `kOutCap`; B's held ≤ L; A's clients see `E_OVERLOADED`. Unstall: all settle, money conserved, counts 0 |
| X2 rate mismatch | F11 across nodes | B's queue toward A ≤ `live_A + live_B` at every sample; settles |
| X3 forward drop | The proxy cuts o → p repeatedly | Per cycle a HelloAck, then a binding SYNC; no counted DATA between them; at each binding, a test hook records p's `held_for_` and `rx` at the barrier where the SYNC is appended, and o's `count_` and `cur_.pushed` where it is applied; they agree exactly (`count_ = k`, `pushed = 0`); no underflow; counts 0 at quiescence |
| X4 return drop | The proxy cuts p → o repeatedly | The same, plus: each current SYNC sets `count_ = k + pushed − R`, with `held_for_`, `rx`, `count_` and `pushed` recorded at the barrier and at the apply, and checked against the proxy's own count of delivered counted frames, before and after the SYNC |
| X5 blackhole and heal | The proxy stops forwarding both ways, then heals | Silence close (unpaused only); quarantine after `T_release`; debt fixed then repaid at binding; settles |
| X6 crash of o | kill -9 A, restart | B retires the old entry and purges it; new binding k = 0; settles; money conserved |
| X7 failover of B | Lab pair with witnesses; kill B's primary | Binds to p′ with k = 0; the old entry quarantines, then retires on restart; settles |
| X7b fenced peer alive | As X7, but the old primary is cut off from its standby and witnesses, not killed | The two entries coexist; nothing from the old primary changes p′'s entry; no underflow; settles after it withdraws |
| X8 slot move | Move a's slot A → C under load (three engines) | Drops to resume counted; ENDs owed are sent; counts 0 at quiescence |
| X9 mixed version | A runs v1 (`SNC_OLD_BUILD`), B runs v2 | Both refuse, with the message and gauge. Clients are served; cross-node transfers stay pending; `live` does not fill; after A is upgraded all settle, nothing lost or doubled |
| X10 rollback | `SNC_XNODE_ACCOUNTING=off` | D.1 behaviour; `xnode_*` counts stay 0; F8 passes |
| X11 backpressure, not churn | X1 with B's flush stalled for 5 × `T_dead` | No silence close and no Out reset while B is paused |
| X12 foreign purge and hand-off | Drop p's link to C while it holds o-tagged replies; move a's slot into B | o's count returns to 0 (ENDs on the tag) |
| **X13 reverse-Hello mismatch** (review) | A proxy pins o's Out connection to old p, while old and new p's reverse connections to o alternate | Counts are attributed only to the instance whose HelloAck bound the connection; nothing from the other instance changes that entry; no underflow; counts exact at quiescence |
| **X14 one-way partition, repeated releases** (review) | o → p passes; p → o blackholed, in two variants: (a) all of p → o, HelloAck included; (b) only p's own Out connection. Full client load for 5 × `T_release` | Physical items at p of o's exchanges never exceed the value at the first quarantine; debt non-increasing; no re-binding while the In side is down; no counted DATA pushed while unbound; o's intra-node clients served throughout (`live_o` not held); after heal, exact and settled |
| **X15 incarnation alternation** (review) | Restart o's member repeatedly (k = 10), with a fenced second member alive | Entries per link ≤ members at every sample; retirements purge; counts 0 at quiescence; no underflow |
| **X16 END debt at a switch** (review) | p holds ENDs owed to o1 when o1′ (a new incarnation of the same member) connects, and others to member 2 | o1's debt is discarded, never sent to o1′; member 2's debt is sent only on a binding to member 2; o1′'s count is exact |
| **X17 churn cycles** (review) | 20 cycles of slot moves, releases, reconnects and restarts under load | At every sample: physical ≤ Σ_s (1 + 2·Q_s)(X + max(E, Rr)); `live_s` < X + max(E, Rr) at every shard; settles |
| X18 timer constraints | Start with `T_dead ≤ T_hb`, or `T_release ≤ T_dead` | The engine refuses to start, with a message |
| X19 blocked primary leaves links (rev2 check G3; deadline per rev2 review item 3) | Cut B's primary off from its standby and witnesses, while A still reaches it | The blocked bit reaches A within `T_hb`, sent at once, even under steady END traffic; no binding to the blocked entry (no re-gating cycle); it leaves the links at `T_block` after the block; `blocked_since_` survives its leaving; A quarantines the old entry by `blocked_since + T_release` (≤ `T_hb + T_release` after the block); A's Out connections move to the promoted member, which binds with an exact k; on unblock it rejoins as a new instance |
| X19b blocked primary, bit undeliverable | As X19, but A pauses its reads from B's primary first, so B's Out buffer is full | A quarantines the old entry by `T_block + T_release` after the block, through the In-down trigger |
| X20 member steering (G2) | Old and new members both accept; pin p's Out connection to the old one while only the new one's In connection is live | p's Out connection moves to the live member within `T_dead`; ENDs reach it; counts settle |
| X21 M conflict (G8) | Two in-memory engines claiming the same member | The second's Hellos are refused while the first is live; `xnode_m_conflicts` > 0; neither retires the other; no underflow |
| X22 unbound link does not hold X (G1) | A peer accepts but never sends a HelloAck (test hook) | Cross-node roots are purged while unbound; `live_s` stays below X from intra-node work; once acks resume, the binding runs `resume_all` and all settles |
| X23 accounting mismatch (G7) | One engine with `SNC_XNODE_ACCOUNTING=off` | Links refused both ways, with message and gauge |
| **X24 repeated forward flaps before reverse reconciliation** (rev2 review) | The proxy delays p → o control frames; o → p is flapped at least 5 times under load, so several connections close before any SYNC reaches o; then the delay is lifted | SYNCs naming earlier connections are ignored (`xnode_syncs_stale` ≥ the flap count); none raises `count_`; the first applied SYNC names the current connection and sets `count_ = held_for_` at p; no counted DATA before it; no underflow; counts 0 at quiescence |
| **X25 delayed old frames after silent replacement** (rev2 review) | Old instance of member m: connection held open but silent past `T_dead` (proxy stall), with DATA, END and SYNC frames queued in the proxy; then a new instance of m connects; the proxy then releases the old frames | The old In socket is reset and removed from epoll before the retirement; the old Out connection (if bound) is reset; the old entry is purged exactly once (`xnode_retire_purged` once); no released old frame is read; the new entry starts at 0 and binds with an exact k; no underflow; no uncounted item |
| **X28 SYNC barrier with queued accounted frames** (PR #93 review) | Make p's Out socket to o non-writable (the proxy stops reading), so frames pile up in `Out::buf`; get at least one counted reply and one END encoded there; then make o reconnect, so p owes a binding SYNC; then restore output | The queued reply and END are observed at o before the SYNC; at the SYNC, `count_` equals the accounted state recorded at p's barrier; no later frame underflows; `xnode_end_underflow` 0; counts 0 at quiescence |
| **X29 monotonic incarnation without clocks** (PR #93 review) | Restarts of one member: two forced into the same wall-clock millisecond (a faked clock); the wall clock stepped back between restarts; stepped forward then corrected back; 20 rapid restarts; plus a crash injected at each step of the generation update, and a corrupt `links_generation` file | Every new incarnation is above the previous one; delayed old Hellos (the proxy replays them) are refused; the new member binds without waiting for wall time; no entry is retired by an older incarnation; a corrupt file in a non-empty data directory refuses to start; `--links-generation-min` recovers a restored directory |
| X26 not-bound quarantine | Fail o's Out connection over to member 2 while member 1 stays connected and unblocked, with items lost on its last wire | Member 1's entry stops gating `T_release` after it was last bound; its ENDs still decrement it; no underflow |
| X27 SYNC before HelloAck | The proxy delays the HelloAck on o's Out socket until after p's binding SYNC reaches o, on each reconnect | o binds on the HelloAck's arrival, with `count_ = k` from the earlier SYNC; no counted DATA before both; `xnode_bind_mismatch` 0 |

### 8.2 Negatives

One mutation per build, with `negative/run_negatives.sh`. The named test
must fail.

| Mutation | Caught by |
|---|---|
| ND1 end the exchange at push (D.1) | X1, X2 |
| ND2 omit the END on one disposition (misrouted) | X8, X3 |
| ND3 apply a SYNC from an instance other than the identified one | X13 |
| ND4 no current SYNC on a new Hello | X3 (never binds: no settlement) |
| ND5 a counted reply does not decrement | X1 (never 0) |
| ND6 both an END and a counted reply for one item | X3 (underflow) |
| ND7 keep `end_owed_` when pushing a SYNC | X4 (double end) |
| ND8 no silence close | X5 |
| ND9 release on Out-side loss with the In side up | X5 (underflow when replies arrive) |
| ND10 adopt instead of drop on a re-route (rev1) | X17 (`live_s` above X + max) |
| ND11 no END for a purge of a foreign link | X12 |
| ND12 no incarnation check on END or SYNC | X7b, X13 |
| ND13 silence close while paused | X11 |
| ND14 carry `pushed` across a reconnect (a per-entry counter instead of `cur_[l].pushed`) | X3 (binding SYNC gives `k + old pushed`; nonzero at quiescence) |
| **ND15 bind from the reverse Hello, or from a reverse SYNC** (review) | X13, X20 |
| **ND16 release without quarantine (no Out reset; DATA continues)** (review) | X14 |
| **ND17 one `end_owed_` scalar per link, not per entry** (review) | X16 |
| **ND18 no eager retirement purge** | X15 (stale items pushed later: underflow, or count nonzero) |
| **ND19 retire an entry when another member appears, not on the same member (the rev1 "third incarnation" rule)** | X7b (underflow) |
| **ND20 In replacement per (group, shard), not per member** | X7b, X13 (oscillation: counts never settle) |
| ND21 queue on an unbound link (no purge) | X22 (`live_s` reaches X) |
| ND22 no `resume_all` at binding | X22 (purged work never settles) |
| ND23 accept a new incarnation while the old one's In connection is live | X21 (mutual retirement; underflow) |
| ND24 hand off a tagged reply in-node untagged | X12 (uncounted item: debug abort or count nonzero) |
| ND25 leave quarantine on a HelloAck alone (BOUND without the binding SYNC) | X14b (counted DATA after the ack; debt grows across releases) |
| ND26 two split counters (`remote_` and `debt_`) with the rev2-draft SYNC rule | X5 (count stuck after a partial SYNC during quarantine) |
| **ND27 the rev2 FINAL rule: keep the closed connection's record and apply a SYNC naming it as `count = k + (pushed − R)`** (rev2 review) | X24, X3 (the count keeps lost frames; nonzero at quiescence) |
| **ND28 apply a SYNC naming an older connection (stale)** (rev2 review) | X24 (`count_` raised after a newer binding; underflow later) |
| **ND29 counted DATA after the HelloAck, before the binding SYNC** (rev2 review) | X3, X24 (binding SYNC with `pushed` ≠ 0; the count is off by the frames in flight) |
| **ND30 accept a new incarnation after silence without fencing the old socket** (rev2 review) | X25 (an old frame reaches the new entry or a retired one: underflow, or an uncounted item) |
| ND31 reset `blocked_since_` when the old instance's links close | X19 (quarantine later than `T_hb + T_release`) |
| ND34 send the blocked bit only in idle heartbeats | X19 (bit late under END traffic) |
| ND35 allow BOUND to an entry with `blocked_since_` set | X19 (count gates again; repeated quarantines before `T_block`) |
| **ND36 insert the SYNC ahead of frames already encoded in `Out::buf`** (PR #93 review) | X28, deterministically (double decrement: underflow) |
| **ND37 incarnation from `CLOCK_REALTIME` milliseconds plus random low bits, as rev3 wrote it** (PR #93 review) | X29 (same-millisecond or clock-step restart refused as stale) |
| ND32 no not-bound quarantine trigger | X26 (member 1's count gates forever) |
| ND33 ignore a binding SYNC read before its HelloAck | X27 (the link never binds; quarantined at `T_release`) |

### 8.3 Existing tests

Every D.1 test runs unchanged. F8 now also checks that the sender's gate
refuses across nodes.

### 8.4 P2 (dev VM, not tuning)

Cross-node transfers on two engines, accounting on against rollback. It
reports the per-frame overhead, the heartbeat rate, the binding latency
after a connect, and the drops to resume.

## 9. Implementation plan (after approval)

The plan is one PR, in this commit order, each one green:

1. Wire v2 (48-byte Hello with flags, HelloAck, frame kinds, version and
   accounting refusal), with behaviour unchanged.
2. Invariant M: listener before links, data-dir lock, M enforcement at the
   peer, In replacement per member.
3. Heartbeats (with the blocked bit), silence, member steering.
4. Entries, HelloAck binding, unbound-as-down, resume at binding, and
   SYNC.
5. END, dispositions and retirement.
6. Quarantine.
7. Drop to resume.
8. Blocked primary leaves the links.
9. Gauges.
10. Tests and negatives.
11. Evidence.

**Files:**

- `node_link.{h,cpp}`;
- `shard.{h,cpp}`;
- `engine.cpp`: the lock, the timer checks, and the blocked-primary hook
  with replication;
- the tests;
- docs: this note, `FOLLOWUP_INTAKE_DESIGN.md` 3.6, and `CLUSTER.md` (the
  wire, the per-member rule and the blocked-primary limit, updated).

## 10. Questions for the review

1. **Drop to resume instead of adoption (2.9).** It makes `live_s < X +
   max(E, Rr)` exact, at the cost of resume latency for follow-ups in
   flight at a slot move, an instance retirement or a binding. Approve?
2. **Invariant M (2.1, 2.12 C).** Listener before links, a data-dir lock,
   and refusing a member's new incarnation while its old one is live,
   with the deployment assumption across hosts. Is that acceptable as the
   one-instance-per-member basis?
3. **The B2 constant (5.3).** Is the loose bound Σ_s (1 + 2·Q_s)(X + max)
   acceptable? A tighter bound would need debt to hold admission, and
   that is the availability problem release exists to solve.
4. **Quarantine (2.8).** It resets the Out connection, stops gating, and
   is left only with a HelloAck *and* a live In connection. Approve as the
   corrected release?
5. **Timers (2.10).** Are the enforced ordering constraints enough?
6. **In replacement per member (2.7).** It is a behaviour change outside
   accounting, argued against `TRANSFERS.md` ordering. Approve?
7. **HelloAck (2.4).** It writes back once on the accepted socket, so
   links are no longer strictly one-way. Approve? (The review offered it
   as an option.)
8. **Blocked primary leaves the links (2.12 A).** It is a small HA change
   that `CLUSTER.md` already names as the next step, and it bounds the
   fenced-alive window. Is it in scope for D.2? The alternative is to
   accept that window as unbounded and documented.

**Rev3 questions.**

9. **FINAL removed (2.5).** The review offered either a FINAL with
   explicit semantics or none. Rev3 has none: only the current
   connection's SYNC applies, and lost items drop out at the next
   binding, or by quarantine. Approve?
10. **Not-bound quarantine (2.8, trigger 3).** Without FINAL, an entry
    that stays connected but is no longer s's bound entry could keep lost
    items forever. Rev3 quarantines it after `T_release`. Approve?

**Rev4 question.**

11. **Operator recovery for a restored data directory (2.1).** Rev4
    refuses a lower generation and offers `--links-generation-min G` as
    the only override. Is that the right recovery mechanism, or should a
    restored directory be excluded outright for v0.1?

## 11. Not in this slice

S5; S6; R640; CP-1A; comparators; B2 begin-snapshot work. Any change to
the reply-only contract or to X semantics.

## 12. Independent reviews

**Rev1, before submission.** An adversarial agent review found seven
issues (F1-F7), fixed in rev1 as merged. Rev2 keeps those fixes, except
where noted.

| Finding | Status in rev2 |
|---|---|
| F1 (END on a foreign purge) | Kept (2.6) |
| F2 (two incarnations) | Mechanism replaced (2.1, 2.4, 2.7) |
| F3 (silence vs pausing) | Kept (2.10) |
| F4 (nonce resets) | Kept (2.3) |
| F5 (signed live) | Kept (2.3) |
| F6 (one-way release) | Corrected by quarantine (2.8) |
| F7 (bounds) | Superseded by section 5 |

**Rev2, before submission.** A fresh adversarial review of the rev2
draft, by a separate agent, found ten issues (G1-G10). It judged the
following sound:

- the binding proof (attribution);
- SYNC exactness in the current, final and binding cases;
- per-entry END debt;
- M2;
- that debt cannot grow while quarantined;
- no wait cycle;
- most drop-to-resume claims;
- the section 1 citations.

| Finding | Severity | Fix in this rev2 |
|---|---|---|
| G1: a link that is up but never binds kept its queue, counted in `live_s`, so X filled and the shard refused every client: a regression against D.1 | High | Unbound means down for queueing: purge and end; `resume_all` at binding (2.4). X22, ND21, ND22 |
| G2: binding through a SYNC on the reverse link can be impossible when that link is attached to another member (a fenced old one) | High | HelloAck on the same socket (2.4); member steering of the reverse link (2.12 B). X20, ND15 |
| G3: a fenced primary that stays connected, but never makes anything durable, is never quarantined, and its count holds X | High | Blocked bit in HEARTBEAT, which triggers quarantine; a blocked primary leaves the links after `T_block` (2.8, 2.12 A). X19 |
| G4 (rev3: the FINAL SYNC it mentions is removed): the `remote_`/`debt_` split let items sit in both counters, or neither, after a partial or FINAL SYNC during quarantine | Medium | One counter plus a gating flag (2.3, 2.8). ND26 |
| G5: "a retired item does not exist" is false while another peer has not yet retired that incarnation | Medium | Term U, bounded per (peer, member) (5.2, 5.3) |
| G6: an in-node hand-off produced an untagged, uncounted item | Medium | Drop to resume, with END (2.6, 2.9). ND24 |
| G7: mixed accounting settings on the same v2 wire | Low | Accounting bit in Hello; refused (2.12 D). X23 |
| G8: invariant M was weaker than stated (no lock for in-memory engines; network namespaces), and two live instances would retire each other | Low | The peer refuses a new incarnation while the old one is live (2.12 C). X21, ND23 |
| G9 (rev3: FINAL removed, so its first half no longer applies): RST wording; FINAL = 0 needed in the exact-current case; a drop at receipt leaked until some later SYNC | Low | Corrected (2.5, 2.6, 2.8); drops at receipt owe ENDs |
| G10: the per-member In replacement was not argued against `TRANSFERS.md` ordering | Low | Argued (2.7) |

## 13. PR #91 review: required corrections and where they are

| Review item | Rev2 |
|---|---|
| 1 BLOCKER: Out connection not bound to the peer instance | 2.4: UNBOUND until a HelloAck on the same socket names this conn and the acceptor's incarnation (the review's alternative; the reverse-SYNC route was found unreliable, G2); never from a reverse Hello. X13, X20, ND15 |
| 2 BLOCKER: release while o → p carries DATA | 2.8: quarantine resets the Out connection, stops gating, and allows no counted DATA until a binding with both directions live; the debt is explicit and non-increasing; unbound links queue nothing. 3.3, X14 (two variants), ND16, ND25 |
| 3 BLOCKER: incarnation transitions | 2.3 and 2.6-2.7: per-entry state; every field's transition at retirement; END debt never combined; invariant P. X16, ND17, ND18 |
| 4 BLOCKER: one orphan, third incarnation | Removed. Invariant M and corollaries M1 and M2: entries ≤ members, retire only on the same member's new incarnation. X15, ND19 |
| 5 B2 proof: release and adoption terms | Adoption removed (2.9). Section 5: physical ≤ Σ live + D + U, with D, U ≤ Q(X + max) each, independent of churn. X17, ND10 |
| 6 Hello size | 2.2: 48 bytes, laid out by offset |
| 6 Mixed-version wording | 2.11: client transport is unaffected, but cross-node settlement and availability can be. X9 |
| 6 Timer constraints | 2.10: enforced ordering; safety does not depend on the values. X18 |
| Section 10 answers | Q1 fail-closed: kept. Q4 SYNC: kept. Q5 timers: kept as hypotheses. Q2, Q3 and Q6: re-asked as 10.4, 10.1 and 10.6 |

## 14. PR #92 review: required corrections and where they are

| Review item | Rev3 |
|---|---|
| 1 BLOCKER (state sequence): HelloAck identity used as accounting-ready binding | 2.4: HelloAck makes the link IDENTIFIED only. BOUND needs both it and the exact SYNC naming the current conn and `conn_seq` with `pushed = 0`, which sets `count_ = k`, in either order; then `resume_all`. No counted DATA before BOUND. X3, X24, X27, ND29, ND33 |
| 1 (state sequence): exact current SYNC before counted DATA | 2.4, 2.5, 3.1. ND4, ND29 |
| 1 BLOCKER (FINAL): its arithmetic added lost `pushed − R` | 2.5: FINAL removed. Only a SYNC naming `cur_[l]` applies: `count = k + (cur.pushed − R)`. Lost items drop out at the next binding (3.1, 4.2) or by quarantine, including the new not-bound trigger (2.8). X24, X26, ND27, ND32 |
| 1 (related): stale control-frame ordering and supersession | 2.2 (`conn_seq` in Hello and SYNC), 2.3 (`cur_[l]` cleared at drop, no history), 2.5 (non-current SYNCs ignored, `xnode_syncs_stale`). X24, ND28 |
| 2 BLOCKER: retirement on silence without fencing | 2.12 C: remove the old In socket from epoll and reset it, reset any Out connection identified with or bound to the old entry, retire, then accept the new incarnation. 2.7 row for `in_conn_`, `in_seq_`, `rx_`, `sync_owed_`. X25, ND30 |
| 3 Blocked-primary timing and X19 | 2.8 trigger 2: `blocked_since_` survives the links closing. 2.12 A timing table: quarantine ≤ `T_hb + T_release` after the block. X19, ND31 |
| 1-3 tests: X24, X25, stronger X3/X4, negatives | 8.1: X3, X4 check the accounting equation at every exact SYNC; X24-X27 and X19b added. 8.2: ND27-ND35 |

**Rev3, before submission.** A fresh adversarial review of the rev3
draft, by a separate agent, found nine issues. All are fixed in this
revision:

| Finding | Severity | Fix |
|---|---|---|
| 1 SYNC waiting for its own link to bind: each direction's binding waits on the other | High | SYNC flows on any identified connection, HEARTBEAT always; only counted DATA and END wait for BOUND (2.5, 6.4 vii) |
| 2 A SYNC sent on a connection attached to another member is lost, with its flags cleared | Medium | Sent only on a connection identified with its target; flags wait (2.5) |
| 3 "Ahead of other frames" could overtake buffered ENDs | Medium | Ahead only of the user-space queue; fields read at encode (2.5). **Superseded in rev4:** that still overtook accounted frames in `Out::buf` (PR #93 review item 1); now an accounting barrier |
| 4 Per-entry `pushed_` against `cur_[l].pushed`; overlapping state names | Medium | One `cur_[l]`; `bound_` derived; entry state ACTIVE/QUARANTINED/RETIRED (2.3, 2.6). ND14 |
| 5 3.3 still described rev2 binding on a HelloAck alone | Medium | 3.3 rewritten: never BOUND in either variant. ND25 |
| 6 Retirement left a SYNCED or IDENTIFIED `cur_` naming the old entry | Medium | Reset in any state; a HelloAck for a retired incarnation refused; In backlog discarded (2.7, 2.8, 2.12 C) |
| 7 Blocked bit only in idle heartbeats; rebind cycle; `T_block` counted twice | Medium | Heartbeat at once on change; no binding to a blocked entry; blocked defined with `T_dead` (2.12 A). X19, X19b, ND34, ND35 |
| 8 Incarnations unordered; a delayed old Hello could retire the live entry | Low | Incarnation ordered by start time; only a higher one accepted (2.1, 2.12 C). **Superseded in rev4:** the clock is not monotonic (PR #93 review item 2); now a persistent generation |
| 9 Labels, ordering and leftover rev2 wording | Low | Corrected |

## 15. PR #93 review: required corrections and where they are

| Review item | Rev4 |
|---|---|
| 1 BLOCKER: the binding SYNC could overtake accounted frames in `Out::buf` | 2.5: an accounting barrier. The SYNC is appended FIFO after every byte in `Out::buf`, and preempts only work not yet pushed; k, `rx` and `end_owed_` are read at the append. Same for HEARTBEAT (2.12 A). X28, ND36 |
| 2 BLOCKER: `CLOCK_REALTIME` plus random is not ordered | 2.1: a persistent per-member generation in `links_generation`, incremented and made durable under the data-dir lock before the listener or any Out connection opens; crash consistency, fail-closed on a bad file, no data directory, restored data directory (`--links-generation-min`). 2.12 C: only a higher generation is accepted. X29, ND37 |
| 6 Accounting evidence at the barrier | X3, X4: `count_`, `held_for_`, `pushed` and `rx` recorded where the SYNC is appended and where it is applied |
| Q9, Q10 approved; rev1/rev2 answers unchanged | Not reopened |

