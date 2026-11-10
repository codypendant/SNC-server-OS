# Follow-up intake D.2: cross-node exchange accounting (design, for review)

**Status: design only, for review.** No D.2 code is written until cody and
Codex approve this note (cody's choice, 2026-11-10: "design first"). It is
the slice that the PR #89 review authorized
(`docs/handoff/PR89_D1_Closure_Review.md`, section 6). It builds on D.1 as
approved and merged (`docs/FOLLOWUP_INTAKE_DESIGN.md`, sections 3.1-3.9).
It replaces the D.2 sketch in that note's section 3.6, as explained in 2.3
below.

Still forbidden until D.2 is approved: S6, the R640 campaign, a CP-1A rerun,
comparators, B2 begin-snapshot work, and S5 unless separately justified.

## 0. Summary

**What is wrong today (D.1).** An exchange ends at its origin as soon as its
follow-up leaves the node (`shard.cpp:3051-3054`). Items that arrive over a
link belong to no shard's exchange count (`on_link_frame`, `shard.cpp:1825`).
So X does not bound cross-node work. A shard can keep admitting
cross-node roots while the replies pile up at the peer toward a slow node.
That is the "derived" case, which D.1 closed only within a node
(`FOLLOWUP_INTAKE_DESIGN.md` 3.6, "Until D.2").

**What D.2 does.**

1. **Origin counting across nodes.** An exchange stays counted in its
   origin's `live_o` until it ends, on either node. It no longer ends when
   it leaves the node.
2. **Two origin bits per data frame**, in the existing padding. "Counted by
   the sender" or "counted by the receiver". The frame stays 64 bytes.
3. **The peer keeps one counter per origin shard:** how many of that
   origin's items it holds. When one ends there, the peer owes the origin
   an **END**. When it leaves as a reply to its origin, it is still
   counted at the origin, and the origin takes it back when the reply
   arrives.
4. **ENDs are coalesced** into one counter per link and sent as one frame
   when there is room.
5. **SYNC, not generations.** After any connection change, the peer
   reports how many of the origin's items it physically holds. It also
   reports how many it received on the origin's current connection. The
   origin then overwrites its count with the exact value. Everything lost
   on a dropped connection simply disappears from the count, and nothing
   is ended twice.
6. **Link liveness.** Idle links send heartbeats, which carry the
   sender's view of the reverse connection. A silent inbound connection
   is closed, but never while reads are paused for backpressure. An
   outbound connection that the peer reports it cannot see is dropped.
   That turns a silent partition into a detected drop.
7. **Release of a dead peer.** When the inbound connection from a peer
   has been down for `T_release`, the origin stops counting that peer's
   items.
   Otherwise a dead node would hold X forever. The next SYNC restores the
   exact count.
8. **Incarnations.** Counts are kept per peer incarnation, so a fenced
   primary that still holds its links cannot touch the promoted one's
   counts.
9. **Adoption.** A frame pushed onto a link is always counted by someone.
   If its exchange has no counting shard on either end (a re-route after a
   slot move, or a reply after a release), the pushing shard adopts it. It
   counts it without waiting for X.
10. **A new wire version** (Hello magic `SNC2`), refused fail-closed by
   both v1 and v2.

**Result.**

- **B2 becomes structural cluster-wide.** Every item of a cross-node
  exchange is counted by one live shard. Σ items ≤ Σ over every shard in
  the cluster of (X + max(E, Rr)), plus a bounded adoption term and a
  bounded transient after a release (section 5).
- **No new wait.** ENDs and SYNCs are counters that never wait for
  anything. No follow-up waits for an END. B4 still ends every wait chain
  at a local flush (section 6.4).

## 1. What the code does today (verified)

| Fact | Where |
|---|---|
| Links are one-way. Each shard has one Out connection to every shard of every other group, and accepts In connections. The peer never sends on an Out connection. | `node_link.h:1-13`, `node_link.cpp:357-362` |
| Hello is 24 bytes: magic `SNCL`, group, shard, shape crc, pad, epoch. A wrong magic or crc is refused and closed. | `node_link.cpp:29-36`, `269-275` |
| A new Hello from the same (group, shard) closes the older In connection before any of the new connection's frames are read. | `node_link.cpp:284-289` |
| `drop_out` closes the socket and clears the unsent buffer. The next attempt tries the group's next member, which is how a failover is followed. | `node_link.cpp:172-185` |
| Any connect, either direction, sets `connected_`. That runs `resume_all`. | `node_link.cpp:279`, `355`; `shard.cpp:1852-1860` |
| Frames are 64 bytes. `pad[3]` is always zero today. | `node_link.h:24-32`; `shard.cpp:3042` (`Frame f{}`) |
| Within a group, slot to shard is fixed: `slot % n_shards`. Owners within a node are fixed. | `cluster.h:66`; `shard.cpp:3116-3118` |
| A link item whose target is not in this group by our map is dropped as misrouted. It is never forwarded to another node. | `shard.cpp:2955-2964` |
| An item leaving through a link ends its exchange at push. A queue for a link that is down is purged, and its exchanges end. | `shard.cpp:3026-3058` |
| Link items are counted against L until they run, leave the shard or are dropped. A reply they cause is a new item, not held. | `shard.cpp:1825-1827`, `3098` |

**Two consequences D.2 relies on.**

- **An exchange between a and b uses only one pair of connections.** Say a
  is owned by A.o and b by B.p. Owners are fixed within a node, and a slot
  moves only between groups. So every item to b goes o → p, on o's Out link
  to p, and every item to a goes p → o, on p's Out link to o. The only way
  to leave that pair is a slot moving to another group, which 3.6 handles.
- **No link item ever waits for another link.** A link item is never
  forwarded across nodes. Within the node it goes only to its fixed owner,
  which is the shard that read it. So it drains through the local WAL
  alone (section 6.4).

## 2. Design

### 2.1 Counting rule

Every follow-up item has at most one **counting shard**, the origin of its
exchange.

- **Local exchanges.** Unchanged from D.1 (`started_`, `ended_own_`,
  `ended_at_`).
- **Cross-node exchanges.** The origin shard o keeps, per out-link l to
  peer shard p:

  - `remote_[l]`: o's items that are at p's side of the pair. That means
    pushed onto l and not yet back at o or ended. It covers the wire o → p,
    everything p holds, and the wire p → o.
  - `pushed_[l]`: counted frames pushed on the current Out connection.
    Reset when the connection changes.
  - `conn_[l]`: the current Out connection's 64-bit nonce, sent in its
    Hello.

  `live_o` stays `started − ended`, now signed (`exchanges_live`
  aborts today if ended exceeds started; that check becomes the
  `xnode_end_underflow` gauge plus a debug abort). `remote_[l]` is the
  part of `live_o` that is on p's side. It is not added again. A push
  moves an exchange from the local part into `remote_` and changes no
  D.1 counter. An END or a SYNC changes `remote_` and `ended_own_`
  together, so X and the gauges see the cross-node exchanges exactly
  once.
- **Peer incarnations (review F2).** o keys this state by the
  incarnation of p that its Out connection reaches, `inc_out_[l]`,
  taken from the peer's Hello on the reverse In connection (2.2).
  Section 2.7 says what happens when two incarnations of p are linked
  at once.

- **At the peer.** Shard p keeps, per origin shard o on another node
  (indexed by p's link to o):

  - `held_for_[l]`: o's items now at p. That is link items flagged
    COUNTED_BY_SENDER, held, staged or running, and the replies they
    produce until those are pushed;
  - `end_owed_[l]`: ENDs not yet pushed;
  - `rx_[l]`: counted frames received on o's current In connection, and
    `in_conn_[l]`, its nonce, from the Hello. Both are zeroed when that
    connection closes (review F4).

  Every item p holds for o also records o's incarnation, from the Hello
  of the In connection that brought it (2.7).

### 2.2 Wire format v2

**Hello, 32 bytes:**

```
u32 magic = 0x324e4353 ("SNC2")
u16 group, shard
u32 crc
u32 version = 2
u64 epoch              // informational, as today
u64 conn               // this connection's nonce: 64 random bits per connection
u64 incarnation        // 64 random bits drawn once per process start
u16 member             // this process's member index in its group
u16 pad[3]
```

The Hello is 48 bytes. Nonces and incarnations come from `getrandom`.
o zeroes `conn_` and `pushed_` together when its Out connection drops,
and sets both at the next connect (review F4). A stale nonce from
another process therefore matches with probability 2^-64. A match would
mis-set one count until the next exact SYNC.

**Frame, 64 bytes as today.** The layout is unchanged, and `pad[3]`
becomes:

- `pad[0]` = kind: 0 DATA, 1 END, 2 SYNC, 3 HEARTBEAT;
- `pad[1]` = DATA flags:
  - bit 0 COUNTED_BY_SENDER: an item of the sending shard's exchange;
  - bit 1 COUNTED_BY_RECEIVER: a reply in the receiving shard's
    exchange;
  - neither: uncounted (rollback, or never sent with accounting on:
    a push with no counting shard adopts, 2.4);
- `pad[2]` = 0.

**Non-data frames** reuse fields that DATA uses, and leave the rest zero:

| Kind | Fields |
|---|---|
| END | `op` = n, the count, at least 1; `source.hi` = the origin incarnation those items came from |
| SYNC | `source.hi` = the nonce of o's In connection that p's count refers to (0 if none); `source.lo` = `rx`; `target.hi` = `held_for` (k); `target.lo` = o's incarnation it refers to |
| HEARTBEAT | `source.hi` = `in_conn_` and `source.lo` = `rx_` for the receiver's link; `op` bit 0 = this side's reads are paused (2.6) |

v1 sent zero padding, so a v1 DATA frame would decode as an uncounted v2
DATA frame. Versions are still not mixed (2.8).

### 2.3 Why SYNC rather than per-session generations

The section 3.6 sketch, and the first draft of this note, used generations.
It reset the count to 0 on every drop and tagged ENDs with a generation, so
that stale ENDs could be ignored. Two defects decided against it:

1. **Flapping amplifies.** After a reset, o admits up to X new
   exchanges, while the old ones may still be at p and still producing
   replies toward o. Each flap adds up to X more. The bound becomes
   X × the number of flaps.
2. **Incarnations.** After o restarts, generations start again. A stale
   END from the previous process could match a new generation and end an
   exchange twice, unless generations are made globally unique.

SYNC counts what physically exists. It needs no generation in data frames,
and it is exact across restarts, failovers and flaps (sections 3 and 4).

### 2.4 Item lifecycle and messages

**Push from o onto link l (o → p):**

| Item | Flag | Accounting |
|---|---|---|
| o's own exchange (`origin == id_`) | COUNTED_BY_SENDER | It moves from local to remote: the exchange stays live, `remote_[l]++`, `pushed_[l]++`. `end_exchange` is not called. |
| A reply in p's exchange (`origin_link == l`) | COUNTED_BY_RECEIVER | `held_for_[l]--` (it goes back to its origin). |
| A reply in another remote origin's exchange (`origin_link == m ≠ l`, after a slot move) | COUNTED_BY_SENDER, adopted | END owed on m (`held_for_[m]--`, `end_owed_[m]++`). o adopts it: `started_++` with no X check, then `remote_[l]++`, `pushed_[l]++`. |
| An exchange of a local shard x ≠ o (a re-route after a slot move) | COUNTED_BY_SENDER, adopted | `end_exchange(x)` as in D.1. Then o adopts it, as above. |
| Uncounted (`origin == kNoShard`, no link) | COUNTED_BY_SENDER, adopted | o adopts it, as above. |

**Receipt at p, on In connection from o (that is, link l):**

| Frame | Accounting |
|---|---|
| DATA, COUNTED_BY_SENDER | `held_for_[l]++`, `rx_[l]++`. The Emit records `origin_link = l`, and so does every reply it produces, through `cur_origin_link_`. |
| DATA, COUNTED_BY_RECEIVER | p's own exchange is back: `remote_[l]--`. The Emit gets `origin = id_` and is counted locally as in D.1. |
| DATA, neither | Uncounted, as every link item is today. |
| END(n) | `remote_[l] -= n`, and `ended_own_ += n` |
| SYNC | Section 2.5 |
| HEARTBEAT | Nothing (it only refreshes liveness) |

**The end of o's item at p.** The item is dropped (stale, misrouted, or
withdrawn), or it runs and emits nothing, or the slot moves. Then
`held_for_[l]--` and `end_owed_[l]++`. Its reply instead leaves p by the
push rules above.

**ENDs.** `end_owed_[l]` is pushed as one END(n) frame on p's Out link to o
whenever there is room. It is coalesced: one counter per link, however
many ENDs are owed. When p's Out link to o drops, `end_owed_[l]` is
cleared. The next SYNC covers those items.

**Every disposition at p owes an END (review F1).** p may remove an item
with `origin_link = m` without pushing it as a counted reply on m. That
covers:

- a drop: stale, misrouted, withdrawn;
- a run that emits nothing;
- the purge of a down link's queue, whatever link that is;
- an in-node hand-off.

In every one of these cases p does `held_for_[m]--` and
`end_owed_[m]++`. If m itself is down, `end_owed_[m]` is cleared with it
and the SYNC that opens m's next connection covers the item. One rule,
no "o learns by SYNC" exceptions.

- **A purge on a link other than m** (the reply's target slot moved to
  another group, and that group's link dropped) therefore still ends the
  item at o.
- **An in-node hand-off.** The reply's target slot moved into p's own
  node (owner q ≠ p). p owes the END on m and adopts the item
  (`started_++`, `origin = id_`) before putting it on q's ring. Within
  the node, it is an ordinary D.1 exchange of p.
- **o's own queued items** (`origin == id_`) in a purged queue never
  reached `remote_`, which rises only at push. The purge only calls
  `end_exchange(id_)`, as in D.1 (review F5).

**Where receipt accounting happens.** In `on_link_frame` it is applied
before the misroute check, which returns before the Emit is built today.
That covers `rx_++`, `held_for_++` (and the END owed if the frame is then
dropped), and the `remote_` decrement for a COUNTED_BY_RECEIVER frame.

### 2.5 SYNC: exact reconciliation

**When p sends a SYNC to o**, on its Out link to o:

1. as the first frame of every new Out connection to o;
2. whenever p reads a new Hello from o on its In side, if the Out link is
   up. Otherwise rule 1 covers it on connect.

Each SYNC is owed as a flag. It is pushed ahead of p's next frame to o
when there is room. At that moment `end_owed_[l]` is cleared, because k
already excludes those items. The SYNC carries
`(in_conn_[l], rx_[l], held_for_[l])`, read when it enters the stream.

**What o does when it reads SYNC(N, R, k) from p:**

| Case | Action |
|---|---|
| **Exact:** `N == conn_[l]` | `remote_[l] = k + (pushed_[l] − R)`. Adjust `ended_own_` by the difference; it is signed, so `live_o` stays `started − ended`. |
| **Partial:** N ≠ `conn_[l]` (p has not yet read o's newest Hello, so it has received nothing on it) | `remote_[l] = max(remote_[l], k + pushed_[l])`. It only raises, and the exact SYNC of rule 2 follows. |

**Why the exact case is exact.** The SYNC sits in p's Out stream after
every END and reply p sent before computing k. So o has applied those
before it reads the SYNC, and k excludes them. Every item o has counted
is in exactly one of three places:

1. at p: in k;
2. lost: on a dropped wire, or in a purged queue at p. It is in neither
   k nor in flight, so it disappears;
3. in flight on the current connection: `pushed − R`. A frame from an
   older connection cannot reach p after p has read the newer Hello,
   because p closes the old In first (`node_link.cpp:284-289`).

Replies already on the wire p → o, ahead of the SYNC, are applied before
it, and p excluded them from k when it pushed them. Replies behind the
SYNC were in k. ENDs work the same way.

**Without a SYNC, o never decreases a count on a drop.** o's count stays
an upper bound (conservative) until the exact SYNC. ENDs and replies keep
decrementing for items that really ended or came back. A lost wire
therefore never under-counts. It over-counts briefly, until the
connection comes back and p sends the SYNC, one round trip after the
Hello.

### 2.6 Liveness: heartbeats, silence, release

**Heartbeats.** Every Out link that has been idle for `T_hb` pushes a
HEARTBEAT. A link that is sending data needs none.

**Silence, without fighting read pausing (review F3).** D.1 pauses a
shard's reads at L: a paused In connection is watched for hang-ups only
(`node_link.cpp:215-227`). Its heartbeats are not read, so silence cannot
be judged by arrivals. And Linux aborts a zero-window connection under
`TCP_USER_TIMEOUT` even when its window probes are answered, which would
turn backpressure into churn. So:

- **In side.** An In connection is closed as silent only after `T_dead`
  with no bytes read, while this shard's reads are not paused and the
  socket has no unread bytes (`FIONREAD` = 0). A paused connection is
  never closed for silence: its hang-up is still reported.
- **Out side.** No `TCP_USER_TIMEOUT`. o judges its Out connection to p
  from p's heartbeats on the reverse connection, which carry p's
  `in_conn_`, `rx_` and paused bit. The Out link is dropped and
  reconnected when either holds:
  - for `T_dead`, p's heartbeats name another nonce or none: p does not
    see this connection;
  - for `T_dead`, p is not paused while `rx_` does not advance with
    frames outstanding: blackholed.

  With no reverse connection at all, the release rule below applies.

A paused peer is alive: it is backpressure, so nothing is dropped.

**Release (review F6).** It applies when o's In connection from p has been
down for `T_release`, whatever o's Out link is doing. It is safe with
the Out link up: the next In connection from p begins with p's SYNC
(rule 1), so no END or counted reply can reach o before that SYNC
re-establishes the count. Waiting for both directions would let a
one-way failure (o → p up, p → o never connecting) hold `remote_` at X
forever. And X is per shard, so o would then refuse every emitting client
op, intra-node ones included.

- o sets `remote_[l] = 0` (adjusting `ended_own_`), and counts a release.
- The peer is unreachable, so its items there occupy no memory on a live
  node. Nothing new reaches p: o's queue to a down link is purged, as in
  D.1.
- The next SYNC from p, after the connection is back, restores the exact
  count of whatever p still holds.

By the time o releases, p's Out link to o is down, so p's queue toward o
has been purged. p can still hold o's link items (≤ L), and replies waiting
for durability in `emit_wait_`. Those are not counted against L
(`shard.cpp:3098`, `queue_emits`), and a purge does not touch them. So the
transient after a release is at most `held_for_` at p, which is at most
X + max(E, Rr) + adopted_o: what o counted toward p before. It is
bounded, it is counted again at the next SYNC, and it does not grow with
flaps (review F7, section 5.3).

**Defaults** (hypotheses, not frozen, for the same reason as H, W, X and
L): `T_hb` = 100 ms, `T_dead` = 1 s, `T_release` = 3 s. Each has an
environment override, and each is in GAUGES.

### 2.7 Two incarnations of one peer (review F2)

A fenced primary that is cut off from its standby and witnesses keeps its
links (`CLUSTER.md`, Limits). So o can be linked to both the old p and the
promoted p′ at once. Their Out connections replace each other's In
connection at o every retry. Counting is therefore per incarnation.

- **o's main count.** `remote_[l]` counts items pushed to `inc_out_[l]`,
  the incarnation o's Out connection reaches.
- **When o's Out reconnects to a different incarnation** (a failover),
  the old count moves to a per-link orphan entry `(inc, n)`. It is kept
  (conservative):
  - it is decremented by ENDs and counted replies that arrive on an In
    connection whose Hello names that incarnation;
  - it is dropped when no In connection from that incarnation has
    existed for `T_release` (dead, or cut off).

  At most one orphan entry is kept per link. A third incarnation's
  arrival releases the oldest, as a release does.
- **o ignores END and SYNC frames that do not match.** A frame is
  applied to `remote_[l]` only if both hold:
  - the In connection it arrived on is from `inc_out_[l]`;
  - for a SYNC, the incarnation it names is o's own.

  Otherwise it goes to the matching orphan entry, or is ignored. Ignoring
  only over-counts, until the next exact SYNC or release.
- **A counted reply from a non-matching incarnation.** A
  COUNTED_BY_RECEIVER reply from an incarnation that has neither a
  `remote_` nor an orphan entry still arrives as o's exchange. o adopts
  it on arrival (`started_++`, `origin = id_`). It is counted, never
  decremented.
- **At p, items of an older incarnation of o.** When p reads a Hello
  from o with a new incarnation, the new o counts nothing p holds:
  its SYNC k is for the new incarnation only. So p adopts every item it
  holds for an older incarnation of o (`started_++` each, `origin = id_`).
  Their replies then go COUNTED_BY_SENDER, and p counts them as its own.
  This replaces the 3.4 rule in the first draft, where the new o counted
  them.
- **A reply goes COUNTED_BY_RECEIVER only to the matching incarnation.**
  If the incarnation on p's Out link to o differs from the one the item
  came from, p ends nothing (that o is gone or fenced) and adopts the
  reply instead.

Over-counts here last at most `T_release` after a cut-off incarnation
stops reconnecting. While it keeps reconnecting, its group's follow-ups
stall, as documented today.

### 2.8 Version policy: fail-closed (Q1)

**What each side does**

- A v1 node reads `SNC2` as "not a node link" and closes. The v2 Out link
  retries every 100 ms, as for any refused connection.
- A v2 node reading `SNCL` refuses it with:

  > node link: refused group G shard S: wire v1, this node speaks v2
  > (upgrade every node)

  The refusals are counted (`link_version_refusals`).

**Effect on a mixed cluster**

- Nothing crosses the wire in a mixed cluster, so v1 and v2 counting
  never mix.
- Cross-node follow-ups stop between upgraded and non-upgraded nodes.
  They are durable object state, and every connect runs `resume_all`, so
  they are re-sent once both ends speak v2.
- Clients are not affected: node links carry only follow-ups.
- Rolling upgrade costs cross-node settlement latency for the window,
  and no correctness.

**The alternative is dual-stack** (a v2 node speaks v1 to v1 peers, with
D.1 counting on those links). It needs a negotiation that this protocol
cannot express cheaply:

- Out connections never receive, so a v2 Out link can learn the peer's
  version only by trying v2 and falling back when refused.
- Each such link would quietly lose the structural bound.

It is offered as Q1. Fail-closed is recommended for v0.1: every lab and
campaign node is built from one frozen SHA.

### 2.9 Rollback

`SNC_XNODE_ACCOUNTING=off` keeps the v2 wire, heartbeats and silence
closes, and restores D.1 counting: end at push, items uncounted at the
peer. Frames are sent with no origin bits, and no END or SYNC is sent.

`SNC_FOLLOWUP_INTAKE=off` implies it.

## 3. Lifecycle cases

### 3.1 o's Out link to p drops (o → p wire lost)

- o keeps `remote_[l]` (conservative). o's queued items for l are purged
  as in D.1: those are o's own and never left, so they end.
- o reconnects with a new nonce, and `pushed_ = 0`.
- p reads the new Hello, closes the old In, and sends SYNC(new, 0, k).
- o applies it exactly: `remote_ = k + pushed_`.
- What was in flight on the old wire is gone from the count.
- The reconnect also runs `resume_all` on both sides, which re-sends
  whatever is unsettled as new roots, counted under X.

### 3.2 p's Out link to o drops (p → o wire, p's queue and ENDs lost)

- p purges its queue toward o: `held_for_` falls by o's replies in it,
  and `end_owed_[l]` is cleared.
- o sees its In connection from p close. Its count stays conservative.
- p reconnects, and the first frame is SYNC(N, R, k).
- If N is o's current Out nonce, the SYNC is exact.

### 3.3 Both directions (partition, then heal)

The cases above, detected by silence within about `T_dead`. After
`T_release`, o releases. On heal, the first SYNC is exact or partial:

- **Exact:** the count is right.
- **Partial:** `max(0, k + pushed_)`. That is exact too after a release,
  because nothing from o's new connection has reached p before p reads its
  Hello. p reads the Hello first and sends the exact SYNC right after.

### 3.4 Crash and recovery of o

- o's counters are memory only. The new process starts at 0, as in D.1.
- Recovery runs replay, then `resume_all`, then links.
- p reads the new Hello, with a new incarnation. p's In connection from
  the old process has closed, or is closed by the replacement rule.
- p adopts every item it holds for the old incarnation (2.7). They are
  counted at p from then on, and their replies go COUNTED_BY_SENDER.
- p sends an exact SYNC with k = 0 for the new incarnation.
- Nothing is counted twice, and nothing is uncounted. An earlier draft
  had the new o count the old items through k. The review (F2) showed
  that this mixes two live incarnations when a fenced o is still
  running, so the survivor adopts instead.

### 3.5 Failover of p's group

- o's Out link drops and reconnects to the new primary p′
  (`node_link.cpp:172-185`).
- p′ holds nothing of o's: its memory is fresh. o moves its count for
  old p into the orphan entry (2.7), and counts p′ from `pushed_`, which
  is exact at p′'s first SYNC (k = 0).
- If old p is dead, no In connection from it reappears. The orphan entry
  goes after `T_release`.
- If old p is fenced but running, its ENDs and counted replies still
  decrement the orphan entry, and nothing it sends touches p′'s count.
- o's resume, run on connect, re-sends what is unsettled.

### 3.6 A slot moves to another group mid-exchange

Take a's slot moving from A to C, while o's item to b is at p.

- p's reply to a is now routed to C.z. Its `origin_link` is o's link and
  not the link it is pushed on. So p owes o an END, and adopts the reply
  (COUNTED_BY_SENDER).
- At C.z it is p's exchange. Its counts are on the pair (p, z).
- Symmetrically, an item that o re-routes after a move is ended at its
  local origin and adopted (2.4).
- An item arriving at p for a slot that is no longer p's group is
  dropped as misrouted, as today, and p owes o an END.
- Adoption is bounded per move (section 5).

### 3.7 Map epoch differences on links

Unchanged:

- a newer-epoch frame is held, and `held_for_` counts it;
- an older-epoch misroute is dropped, which owes an END.

Neither waits for another link.

### 3.8 Withdraw, standby and stop

- **Withdraw** drops the shard's follow-ups and ENDs them: `held_for_--`
  and `end_owed_++`. The Out links of a withdrawn shard close with it,
  and the peers re-sync with the new primary (3.5).
- **A standby** has no links.
- **`quiesced()`** does not wait for remote ENDs, for the same reason it
  does not count a paged resume (`FOLLOWUP_INTAKE_DESIGN.md` 3.9.2). A
  clean stop waits for no other node.

## 4. No double end, no leak (acceptance items 2 and 3)

### 4.1 Invariant I (at o, per link l)

Between exact SYNCs:

    remote_[l] ≥ (o's counted items on the wire o→p, held at p, or on the wire p→o)

At an exact SYNC it becomes equal.

**Proof sketch.**

- `remote_` rises only at push, by 1 per counted push. Every pushed item
  is in one of the three places until it is lost, ends at p or returns.
- It falls only by:
  - END(n): p ends n items it held, once each. `held_for_` falls before
    `end_owed_` rises. An END is sent at most once, or cleared with the
    link and covered by SYNC;
  - a COUNTED_BY_RECEIVER reply arriving, once per item. At p, the item
    left `held_for_` at that push;
  - an exact SYNC, which sets the exact value;
  - a release, the only deliberate under-count (5.3).
- p emits at most one of {END, counted reply} per item: they are
  disjoint branches of the item's single disposition (2.4).
- An exact SYNC is ordered after everything p emitted before computing
  k (2.5).

So no item is decremented twice, and every item is either decremented
once, or removed by an exact SYNC that does not count it. **No double
end.**

### 4.2 No leak

A lost item is removed by the next exact SYNC. An exact SYNC follows:

- every reconnect in either direction, by rule 1 or rule 2;
- every heal;
- every failover.

If the peer never comes back, release removes the items after
`T_release`. No count can survive both of those. **No leak.**

### 4.3 What the tests check

At quiescence:

- every `remote_` and `held_for_` is 0;
- every `live_o` is 0.

At every sample:

- `remote_ ≥ 0`;
- the underflow gauge (an END or reply that would take `remote_` below
  0) stays 0.

## 5. B2: the structural cross-node bound (acceptance item 1)

### 5.1 Conservation still holds

An item that crosses a link is still the exchange's one follow-up (lemma
of `FOLLOWUP_INTAKE_DESIGN.md` 6.2). The reply-only contract is
unchanged.

### 5.2 Every item has a counting shard

Every item of a cross-node exchange is counted by exactly one live
shard, the origin, outside three transients:

- an END or reply in flight: counted until it arrives (an over-count);
- an adoption: counted by the adopter from the push;
- after a release, before the SYNC (5.3).

So, cluster-wide:

    Σ items of all exchanges  ≤  Σ_s live_s
    live_s  <  X + max(E, Rr) + adopted_s

The first term is client and resume roots, admitted only while
`live_s < X`, as in D.1 (3.9.1, 3.9.2). `adopted_s` is exchanges s took
over in one of four cases:

- a slot move;
- an in-node hand-off;
- an incarnation change of a peer;
- a counted reply from an unknown incarnation (2.4, 2.7).

Orphan entries are over-counts, never under-counts.

### 5.3 The adoption and release terms

- **Slot moves.** Adoptions happen only when an item's counting shard is
  not on the pair it travels: after a slot move between groups, or for
  an uncounted reply after a release. Per move, they are at most the
  items in flight for that slot at the time.
- **Incarnation changes.** Adoptions there are at most what p held for
  the old incarnation (≤ X + max(E, Rr) + adopted of that o, since it was
  counted there).
- **Adoptions cannot multiply.** An adopted exchange is a counted
  exchange like any other, so it delays new admission at s.
- **Release.** The transient after a release is at most `held_for_` at p,
  which is what o counted toward p before: at most
  X + max(E, Rr) + adopted_o (2.6). Corrected from "≤ L" in the first
  draft: replies waiting for durability are not held link items (review
  F7). Those items are counted again at the next SYNC. Nothing new reaches
  p while the In side is down long enough to release, so repeated
  releases do not add up.

### 5.4 The per-queue consequence (acceptance item 7: stalled destination)

Take p stalled: a slow flush, or no reads.

- **At p:** its held link items stay ≤ L, and its reads pause (D.1).
- **o's Out buffer:** ≤ `kOutCap` (1 MiB), as today.
- **o's queue `emit_node_[l]`:** o's own and adopted items, counted in
  `live_o`, plus replies to p's exchanges, counted in `live_p`. So it is
  ≤ `live_o + live_p` (corrected, review F7).
- **o's clients:** they get `E_OVERLOADED` once `live_o` reaches X.
  Without D.2 they keep being admitted.
- **p's queue toward a slow o:** replies to o's items (counted in
  `remote_` at o, so o stops admitting), plus p's own (counted in
  `live_p`). So it is ≤ `live_o + live_p`, the cross-node analogue of
  F11.
- **Control state per link:** one END counter, one SYNC flag and one
  heartbeat timer, whatever the load.

## 6. The other acceptance items

### 6.1 Recovery and resume (item 5)

- Counters are memory only.
- Recovery is replay, `resume_all`, then links, as today. Then SYNC
  (3.4).
- Resume roots are still paged under X. Counts do not have to be exact
  for the rebuilt state to be correct: only the bound depends on them,
  and the bound is restored by the first exact SYNC.

### 6.2 Compatibility and version (item 6)

Section 2.8. A mixed cluster refuses links in both directions, and says
why. Nothing is lost: follow-ups are re-sent after the upgrade.

### 6.3 Response and control progress (item 8)

- Rings still drain completely.
- Node links still carry only follow-ups, and now accounting frames.
  Client requests, responses and admin never use them.
- The new client-visible effect is the intended one: `E_OVERLOADED` from
  X when cross-node work is outstanding.

### 6.4 No cyclic deadlock (item 4)

The waits are those of `FOLLOWUP_INTAKE_DESIGN.md` 6.4, (i) to (iv),
plus:

- **(v) a frame waits for room on its Out link.** That is the peer
  reading, which is (iii) at the peer: its held link items falling below
  L/2. Those drain only through the peer's own WAL (i), because a link
  item never waits for another link (section 1).
- **(vi) END, SYNC and HEARTBEAT** are counters and flags that wait only
  for (v). Nothing waits for them:
  - an END only lowers `live_o`;
  - a missing END only keeps client admission refused (iv), which is a
    refusal, not a wait.

**The case to rule out.** Two nodes, each with its reads paused and its
queue toward the other full.

- A's held link items drain by A's WAL whatever B does, and likewise for
  B. So both resume reading, and both queues move.
- The mutual-pause case flagged in the D.1 review is ruled out by the
  code: a link item is never forwarded to another node (misrouted drop,
  `shard.cpp:2955-2964`). Its in-node owner is fixed (`cluster.h:66`).

**ENDs behind DATA in the same stream.** They are delayed only by (v).
A delayed END delays admission, never a follow-up. Every wait chain still
ends at a local flush.

### 6.5 Sanitizers (item 10)

Release, ASan+UBSan and TSan full serial suites, as for D.1, with every
run reported in order.

## 7. Gauges (added to GAUGES)

Per shard:

| Kind | Gauges |
|---|---|
| Configuration | `xnode_accounting`, `xnode_hb_ms`, `xnode_dead_ms`, `xnode_release_ms` |
| Counts now | `xnode_remote_live` (Σ `remote_`), `xnode_held_for_peers` (Σ `held_for_`), `xnode_end_owed` |
| Counters | `xnode_ends_sent`, `xnode_ends_received` (frames and items), `xnode_syncs_sent`, `xnode_syncs_exact`, `xnode_syncs_partial`, `xnode_sync_correction` (Σ \|change\|), `xnode_releases`, `xnode_orphan_live`, `xnode_orphans_dropped`, `xnode_adopted`, `xnode_ignored_frames` (END/SYNC from a non-matching incarnation), `xnode_end_underflow` (must stay 0), `xnode_heartbeats_sent`, `xnode_silence_closes`, `link_version_refusals` |

`intake_exchanges_live` is `started − ended` and so already includes `xnode_remote_live`; the gauge is published, not added.

## 8. Tests

### 8.1 Positive tests

Two engines in one process over loopback, as F8, unless noted.

| Test | Setup | Pass criterion |
|---|---|---|
| X1 stalled remote | B's flush hook stalls; A's clients run cross-node transfers flat out | A's `emit_node_` ≤ `live_A + live_B` and `live_A` < X + max(E, Rr) + adopted; A's Out buffer ≤ `kOutCap`; B's held link items ≤ L; A's clients see `E_OVERLOADED`. After unstall: all settle, money conserved, every count 0 |
| X2 rate mismatch | F11 across nodes: A's clients transfer to B; B's replies to A throttled (slow flush on A) | B's queue toward A ≤ `live_A + live_B` at every sample; settles |
| X3 Out drop mid-flight | A proxy between A.o and B.p cuts o → p repeatedly under load | No `end_underflow`; `remote_ ≥ 0`; an exact SYNC after each reconnect; at quiescence every count 0; money conserved |
| X4 return-path drop | The same, cutting p → o (B's queue and ENDs lost) | The same |
| X5 blackhole and heal | The proxy stops forwarding without closing, one direction then both | Silence close within `T_dead` of an unpaused, silent In; release after `T_release` with the In side down (also when only p → o is cut); on heal the SYNC restores the count; settles |
| X6 crash of o | `snc_server` processes; kill -9 A with items at B; restart | B adopts the old incarnation's items; the new A's SYNC is exact with k = 0; settles; money conserved; no underflow |
| X7 failover of B | Lab pair with witnesses; kill B's primary | o's old count goes to an orphan entry and is dropped after `T_release`; p′'s count is exact at its first SYNC; settles |
| X7b fenced peer still running | As X7, but B's old primary is cut off from its standby and witnesses, not killed (its links stay, `CLUSTER.md` Limits) | No frame from the old incarnation changes p′'s count; `xnode_ignored_frames` or orphan decrements counted; no underflow; settles after the old primary withdraws |
| X11 backpressure is not churn | X1 with B's flush stalled for 5 × `T_dead` | No link is dropped while B's reads are paused (`xnode_silence_closes` and Out drops stay 0) |
| X12 in-node hand-off and foreign purge | Move a's slot into B's node while p's reply is queued; separately, drop p's link to C while it holds a reply with o's `origin_link` | o's count returns to 0 at quiescence (the END on m is sent) |
| X8 slot move mid-exchange | Move a's slot from A to C under load (three engines) | Adoption counted; no underflow; every count 0 at quiescence |
| X9 version | An A v1 binary (`SNC_OLD_BUILD`) and a B v2 binary | Both refuse, with the message and gauge; no follow-up lost; after upgrading A, everything settles |
| X10 rollback | `SNC_XNODE_ACCOUNTING=off` | D.1 behaviour: no END or SYNC sent, `xnode_*` counts stay 0, F8 passes |

### 8.2 Negatives

Each is a mutation, one build each, run with `negative/run_negatives.sh`.
The named test must fail.

| Mutation | Caught by |
|---|---|
| ND1: end the exchange at push (D.1 counting) | X1 (A's queue bound), X2 |
| ND2: p forgets an END on one drop path (misrouted) | X8 or X3 (count never returns to 0) |
| ND3: treat a partial SYNC as exact | X3, X4 (underflow or nonzero at quiescence) |
| ND4: no SYNC on a new In Hello (rule 2) | X3 (count stays high: leak) |
| ND5: a COUNTED_BY_RECEIVER reply does not decrement `remote_` | X1 or X10-style quiescence (never 0) |
| ND6: both END and the counted reply for one item (double end) | X3 (`end_underflow` > 0) |
| ND7: do not clear `end_owed_` when pushing a SYNC | X4 (double end after re-sync) |
| ND8: no silence close | X5 (never released) |
| ND9: release on Out-side loss alone (In from p still up) | X5 (underflow when the replies arrive) |
| ND10: no adoption on a re-route | X8 (an uncounted item reaches the far side; uncounted gauge or bound check) |
| ND11: purge of a link other than m owes no END (the first draft's rule) | X12 (o's count never returns to 0) |
| ND12: no incarnation check on END/SYNC/counted reply | X7b (underflow, or p′'s count changed by old p) |
| ND13: silence close while paused (or `TCP_USER_TIMEOUT` on Out) | X11 (links dropped under backpressure) |
| ND14: zero `pushed_` but keep `conn_` at drop | X3 (count off by R after a stale-nonce SYNC; nonzero at quiescence) |

### 8.3 Existing tests

Every D.1 test runs unchanged (intra-node), plus F8, now with the sender
gate refusing across nodes. That was "not met in D.1" in
`FOLLOWUP_INTAKE_DESIGN.md` 3.9.5.

### 8.4 P2 regression check (dev VM, not tuning)

Two engines on loopback, cross-node transfers, accounting on against
rollback. It reports the per-frame overhead and the heartbeat traffic.

## 9. Implementation plan (after approval)

The plan is one PR, in this commit order:

1. wire v2 (Hello, frame kinds, version refusal) with no behaviour change;
2. heartbeats and silence;
3. accounting, SYNC and release;
4. adoption;
5. gauges;
6. tests and negatives;
7. evidence.

Each commit builds and passes the suite.

**Files:**

- `node_link.{h,cpp}`: Hello v2, frame kinds, nonce, heartbeat, silence,
  user timeout, accessors for In connection identity and connect events
  per link;
- `shard.{h,cpp}`: Emit `origin_link`; per-link `remote_`, `pushed_`,
  `conn_`, `held_for_`, `end_owed_`, `rx_`, `in_conn_`, SYNC owed; push
  and receipt rules; purge; release; gauges;
- tests;
- docs: this note, `FOLLOWUP_INTAKE_DESIGN.md` 3.6 and `CLUSTER.md`
  (the wire).

## 10. Questions for the review

1. **Version policy.** Fail-closed (recommended), or dual-stack with
   fallback, which loses the bound on v1 links?
2. **Release.** Release a peer's count after its inbound side has been
   down for `T_release` (recommended: otherwise one dead or one-way-cut
   node can hold X and refuse every emitting client op at o)? Or never
   release (strict, at the cost of availability)?
3. **Adoption.** Accept that follow-ups may push `live_s` above X by
   adoption, since they never wait for X, with the term bounded per slot
   move and release (5.3)?
4. **SYNC overwrite.** Accept SYNC as the reconciliation mechanism,
   instead of per-item generations (2.3)?
5. **Defaults.** `T_hb` 100 ms, `T_dead` 1 s and `T_release` 3 s, as
   hypotheses with overrides, to be characterized on target hardware?

6. **Incarnations.** Accept per-incarnation counting, with orphan entries
   and adoption by the survivor (2.7), as the answer to the documented
   case of a fenced primary that keeps its links? The alternative is to
   first make a blocked primary withdraw (the `CLUSTER.md` "next step"),
   which is outside this slice.

## 11. Not in this slice

S5; S6; R640; CP-1A; comparators; B2 begin-snapshot work. Any change to the
reply-only contract or to X semantics.

## 12. Independent review before submission

An adversarial review of the first draft, made by a separate agent on the
note and the cited code, found seven issues. All are fixed in this
revision. The review also judged several parts sound:

- the exact-SYNC argument for one incarnation with unique nonces;
- the no-cycle argument (6.4);
- the version policy and rollback;
- the code citations in section 1.

| Finding | Severity | Fix |
|---|---|---|
| F1: a purge on a link other than the origin's, or an in-node hand-off, removed an item without an END; o leaked it, since no SYNC follows on an unchanged pair | High | Every disposition owes an END on the origin's link; a hand-off adopts (2.4); X12, ND11 |
| F2: a fenced primary keeps its links (`CLUSTER.md` Limits), so two incarnations of p reach o at once and mix counts (double end) | High | Hello carries an incarnation and a member index; counts, ENDs, SYNCs and counted replies are per incarnation; orphan entries; the survivor adopts (2.7); X7b, ND12 |
| F3: silence detection and `TCP_USER_TIMEOUT` would drop links that D.1 paused for backpressure, turning it into churn | High | No silence close while paused or with unread bytes; no `TCP_USER_TIMEOUT`; the Out side is judged from the peer's heartbeats (2.6); X11, ND13 |
| F4: the nonce and the counter resets were unspecified; a reused nonce, or `pushed_` reset without `conn_`, gives an under-count | Medium | Random 64-bit nonces; `conn_` and `pushed_` reset together; `in_conn_` and `rx_` zeroed on close (2.1, 2.2); ND14 |
| F5: `live_o` "adds" `remote_` (double count), and the purge decremented `remote_` for items never in it | Medium | `remote_` is a part of signed `started − ended`; the purge only calls `end_exchange(id_)` (2.1, 2.4) |
| F6: requiring both directions down to release lets a one-way failure hold X forever | Medium | Release when the In side has been down for `T_release`. It is safe because the next In connection starts with SYNC (2.6) |
| F7: the release transient is not ≤ L (replies waiting for durability), and o's queue also holds replies to p's exchanges; X1's bound and one cross-reference were wrong | Low-Medium | Bounds corrected (2.6, 5.3, 5.4, X1) |
