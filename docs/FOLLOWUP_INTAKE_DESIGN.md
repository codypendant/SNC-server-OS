# Follow-up intake backpressure: design note (for review)

Status: **design only, for review. Nothing here is implemented.** Revised
after an independent review (section 12).
Work order: `docs/handoff/PR82_S41_Review.txt` sections 8-14 (S4
complete; this design authorized, implementation not). The problem:
[S1_1_CORRECTIONS.md](S1_1_CORRECTIONS.md) section 3 and A15.

Code read for this note (main at 83280f9):
- `src/native/ring.h` and `src/native/runtime.cpp` (`sri_send`,
  `sri_claim`, `drain`, `publish_all`);
- `src/core/shard.cpp`: `execute`, `followups_refuse`, `admit`,
  `on_message`, `queue_emits`, `route_emit`, `run_emits`, `run_emit`,
  `send_emit`, `serve_emit`, `hold_emit`, `release_held`, `emit_result`,
  `on_link_frame`;
- `src/core/node_link.{h,cpp}`, `src/core/storage.cpp` (`WalStream`),
  `Engine::stop` and `Engine::wait_emits`, `docs/TRANSFERS.md`,
  `tests/test_s1_admission.cpp` A15.

## 0. Summary

**Recommendation: option D, credit-bounded destination staging.**

- **Rings still drain fully.** The destination keeps draining every
  inbound ring completely, as today. Nothing is ever left at the head of a
  FIFO, so no answer or control message can be trapped behind a follow-up.
- **The gate is at apply, not at drain.** A drained EMIT runs only if the
  shard's WAL has room under a hard intake cap H. Otherwise it waits, in
  order, in the shard's existing local follow-up queue (the "staging").
- **Credits bound the staging.** For each ordered shard pair (s, d), s may
  have at most W follow-ups sent to d that d has not yet taken out of
  staging. d publishes a per-source count of the follow-ups it has taken
  out. Past the window, s keeps follow-ups in its own per-destination queue,
  where its existing gate (`kEmitBacklog`) already counts them.
- **The gate also applies where work is owned.** Today the
  `kEmitBacklog` gate runs only on the shard where a client request
  arrives. A request forwarded to the owning shard is not checked there,
  so the owner's backlog can grow past the gate (section 3.5). D adds the
  same check at the owner for forwarded mutations, creates and deletes.
  They are answered with `E_OVERLOADED`, never left waiting.
- **Node links pause their reads.** A shard stops reading its inbound
  node-link sockets while it holds L or more link-origin follow-ups. TCP
  flow control then backs the sender up into its own counted queue. Node
  links carry nothing but follow-ups.
- **No new waits between shards.** Intake depends only on the shard's own
  WAL, and the WAL drains by disk completion alone. So no wait cycle
  between shards can form (B4).

The proofs cover:
- B1, the WAL bound: at most `max(H, one follow-up's worst case)`, plus
  the other non-refusable terms that S1.1 already lists;
- B3, B4 and B5;
- B6, no follow-up dropped, duplicated or refused;
- B7, recovery;
- B2 for every follow-up a client op or a resume pass originates.

**One part of B2 is not structural, and this note asks the review to
decide it (section 7).** A shard can apply an incoming follow-up whose op
emits a follow-up to a third, closed shard. The backlog of these
"derived" follow-ups is bounded by the application's protocol shape, not
by an engine gate:
- For transfers, the only follow-up type in the tree, it is bounded by
  object state (open outbox and inbox entries), not by the engine
  (section 6.2).
- A general forwarding chain could grow it in memory: one 72-byte queue
  entry per follow-up, no WAL.

The obvious engine gate for this ("close intake when my own backlog is
high") can deadlock two shards with healthy disks (section 7.1), so it
is rejected. Three options are laid out instead.

What D bounds structurally:
- received follow-ups waiting for credit to return: at most (N−1)·W per
  shard;
- follow-ups received over node links and held: at most L per shard;
- each shard's own follow-ups (local, re-routed, resume), which the gate
  counts and object state bounds.

Numeric defaults (H, W, L) are hypotheses: no value is frozen from the
dev VM.

## 1. What happens today

**Sending.** An op's `apply` emits; `queue_emits` holds a remote
follow-up until the emitting record is durable (`emit_wait_`).
`route_emit` then puts it on one of three queues, all sorted FIFO:
- `emit_local_`, for a target on this shard;
- `emit_remote_[d]`, for another shard on this node;
- `emit_node_[link]`, for another node.

`run_emits` sends `emit_remote_[d]` with `sri_claim` (not
`must_deliver`). That means the ring first, then the runtime's per-pair
overflow deque of up to 256 entries; past that the send is refused, and
the rest stay queued in order.

**The sender's gate.** `emits_in_flight() = emit_ready_count_ +
emit_wait_.size()` counts only what is still queued at the sender. A
follow-up that reached a ring or the runtime overflow has left the
count. Client `execute`, and (unless admission is legacy) create and
delete, are refused with `E_OVERLOADED` while the count is above
`kEmitBacklog` (1024). Owner-local READs are exempt.

The gate runs only on the shard where the request arrived
(`shard.cpp:177-182`, `385-389`). A mutation forwarded to its owner (`serve`
`1515`, `serve_multi` `1386`, `run` `440`) passes WAL admission B there, but
not the owner's backlog gate. So a shard whose follow-ups cannot leave
keeps taking new transfers from clients connected to other shards. This
gap exists today; D closes it (section 3.5).

**Receiving.** `drain` hands every message of a ring to `on_message`, up
to `batch_in` per pass; the ring carries every message kind in one FIFO.
For an EMIT, `serve_emit` either runs it at once (`run_emit` → `run_exec`
with `client = false`, never refused) or queues it behind a non-empty
`emit_local_` to keep the order. Either way it appends its record
whatever the WAL holds.

**The problem (A15).** While d's WAL stalls, healthy senders keep
admitting and sending. Every sent follow-up leaves their count, and d
logs it. A15 delivered 2,050 credits to one stalled shard: 50.9 MB of
WAL, 1.6× the client budget.

Two more facts this design relies on:
- **WAL drain depends on nothing but the disk.** The active WAL buffer
  (`WalStream::active_len_`) grows while a flush is in flight, and empties
  when the flush starts after the previous one completes.
- **A finding.** `active_cap_` and `active_len_` are `u32`, and
  `ensure_capacity` doubles `cap` in a `u32`. Past 2 GiB the doubling
  wraps to 0 and the loop never ends; `active_len_ + extra + kBlock` can
  wrap too (`src/core/storage.cpp:566-575`). Unbounded follow-up intake is
  one way to get there, but not the only one:
  - a standby's `Receiver::poll` appends everything it receives with
    `append_raw` and no bound (`replication.cpp:697-745`), and the primary
    sends with no window;
  - legacy admission has no WAL cap;
  - `wal_buffer_budget_bytes` is a configurable u64.

  This design does not close those paths. Section 10, Q4 proposes fixing
  the overflow as its own small change. Not changed here, because this
  slice is design only.

**Node links** (`node_link.{h,cpp}`) carry only follow-up frames, one TCP
connection per (shard, remote shard), with no acks.
- On the sending side, `room()` lets each link buffer up to `kOutCap`
  (1 MiB, 16,384 frames).
- On the receiving side, `read_in` reads everything available and passes
  each frame to `route_emit`, with no bound.
- A dropped link loses frames by design. Each new connection re-runs
  `resume_all`, and the objects re-emit what is unsettled
  (docs/TRANSFERS.md).

## 2. The FIFO problem (review section 10)

Messages from s to d share one SPSC FIFO with every other kind: EXEC,
CREATE and DELETE requests, RESP, BUF_RETURN, MULTI_REQ and MULTI_RESP.
If d stopped consuming at an EMIT, everything behind it would wait,
including the answers s's waiters need. Those waiters hold s's own
resources (the pending and waiter budgets, BUF_RETURN buffers). So a
throttle that stops the drain turns into a protocol deadlock. The design
must never leave a message unconsumed in a shared ring.

| | Mechanism | Response/control progress | Bound on unapplied follow-ups | Cost |
|---|---|---|---|---|
| A | A second SPSC ring per pair carrying only EMIT; d stops draining it while closed | Structural | Ring + overflow per pair (512); the overflow is uncounted at the sender | Changes the runtime topology (2N² rings) in both backends. A consumer that skips a non-empty ring must still sleep (`has_pending_for_consumer` sees it). Changes the order between kinds |
| B | Credits: s sends while d has granted room | d keeps draining | Only if every follow-up already in a ring is applied regardless: WAL overshoot up to N−1 windows × the largest record | One counter per pair; no topology change |
| C | Drain everything; put EMITs into a bounded staging queue at d | Structural | Needs a bound on what is in the rings. Without one, either staging is unbounded or d stops draining when it is full (the trap again) | Copies; the backlog moves to d |
| **D (recommended)** | **C + credits counted at apply.** d drains everything; EMITs wait in staging when the WAL is above H; s sends only while `sent − taken < W` | **Structural (the drain never stops)** | **Credit-bearing staging ≤ (N−1)·W, link-origin ≤ L (72 B items); WAL overshoot only the empty-buffer case** | Shard code only, plus one shared N×N counter matrix |

Why C alone is not enough: staging must be bounded, and only the sender
can stop new follow-ups from arriving without trapping the FIFO. A
sender-side window is exactly what B is. D is C with B's credits, counted
when an item leaves staging rather than when it is drained.

Why not A: A also works. But it changes the runtime in both backends, and
it doubles ring memory and wake-up checks. Its uncounted in-transit
amount per pair (ring plus overflow) is larger than D's window. D gets
the same structural separation by draining always and gating the apply.

**Order.** Every follow-up from s to d still goes through one FIFO
(`emit_remote_[d]`, then the ring, then staging), so order per source is
kept. The node-link code relies on that: a credit must not overtake its
own forget (`node_link.cpp`, `read_in`).

D adds no new kind of reordering between kinds. An EMIT can be applied
after a later RESP or request from the same sender. Today that already
happens whenever `emit_local_` is non-empty (`serve_emit`). The
implementation must check that no code relies on EMIT-before-RESP order
(section 9, V1).

## 3. Mechanism (D)

### 3.1 The intake gate (destination)

A follow-up is applied only while

    wal.active_bytes() + need_f <= H    or    wal.active_bytes() == 0

where `need_f = record_size(max over types of max_size) +
peer_record_max_`: the same worst case S1 already computes, with the
largest type, because the target's type is known only after the lookup.
The empty-buffer clause is S1's progress rule: an item larger than H
still runs, but alone. The check is made per item at the head of the
queue, immediately before `run_exec`, on the shard's own thread, and
nothing can yield between the check and the append (R3, as in S1;
`check_reserved` can assert it).

- **Where.** In `run_emits`, the queue it processes (`emit_local_`) stops
  at the first item that does not pass and keeps the rest in order.
  `serve_emit` stops running inline when the gate is closed or
  `emit_local_` is non-empty; it appends to `emit_local_`, as it already
  does in the second case. Local follow-ups (targets on this shard) and
  received ones share that queue and that gate.
- **Unlogged ops** (`forget`) need no WAL, but they keep their place in
  the FIFO. Order comes first; they wait at most until the item in front
  of them runs.
- **Order inside `run_emits`.** Today `run_emits` swaps out the whole
  vector and subtracts its size from `emit_ready_count_` before running
  it (`shard.cpp:2594-2598`). Under D, when it stops at a gated item:
  - the items it did not run go back in front of anything appended to
    `emit_local_` in the meantime;
  - `emit_ready_count_` is restored for them;
  - `serve_emit`'s `emit_local_.empty()` test sees them, so a newly
    arrived item never runs inline ahead of them.
- **H is a hard constraint, not a default:** H ≥ B + R. If H were
  smaller, clients admitted under B could refill the buffer each time a
  flush starts, and the head follow-up could starve, which breaks B5.
  Rollback is a separate switch (`intake_gate = off`), not H = 0: by the
  formula above, H = 0 means one item per empty buffer.
- **Busy and wake-ups, precisely.** A shard whose staging head is gated,
  or whose remote queues are all waiting for credit, does not report
  itself busy for that reason. Today `run_emits` returns true whenever
  `emit_ready_count_ != 0` (`2593`, `2633`), so those shards would spin. Two
  wake-ups are needed:
  - The gated destination is woken by its WAL flush completing, an I/O
    completion, which already wakes the loop. `WalStream::poll` returns
    `active_len_ > 0` (`storage.cpp:615-622`), which already keeps a shard
    with a full active buffer busy. Whether that should change is outside
    D; V4 measures it.
  - The credit-blocked sender: today nothing wakes it when `taken`
    advances. The runtime's sleep check covers rings, injected work and
    I/O only (`runtime.cpp:411-427`), with a 1 ms poll timeout as the
    fallback. D adds a flag per pair, "sender waits for credit", set by
    s. When d advances `taken` for a pair that has it set, d writes s's
    eventfd, as a ring publish does.

### 3.2 Credits (intra-node)

- `taken[s][d]`: a `u64` written only by d, padded to a cache line,
  published with release once per loop iteration (like a ring head). d
  adds one every time an item that came from s leaves its staging, by
  **any** path (section 3.4).
- `sent[d]`: private to s, the number of follow-ups it sent to d.
- s sends the head of `emit_remote_[d]` only while
  `sent[d] − taken_cache[d] < W`. It reloads `taken_cache` (acquire) only
  when the window looks exhausted, as `p_head_cache` does.
- `Emit` gains a memory-only field `u16 from` (the sending shard, from
  `Msg::src_shard`; or a node-link marker). No wire or format change: the
  `Msg` already carries `src_shard`.
- **Invariant (credit conservation):** `sent[d] − taken[s][d]` = the
  follow-ups from s that are in the ring, in the runtime overflow, or in
  d's staging. So it is at most W. A path that removes an item without
  counting it leaks one credit for good: after W leaks the pair stalls.
  F9 negative N3 tests this.
- W = 0 means no window, today's behaviour.
- `from` is cleared whenever an item is held or re-routed by this shard
  (`hold_emit`, or `route_emit` from `run_emit`), at the moment its credit
  is returned. Otherwise a later run would return the credit a second
  time.

### 3.3 Node links (cross-node, F8)

- **Pausing reads.** A shard counts the link-origin follow-ups it holds
  anywhere (staged, or forwarded on into `emit_remote_` before they leave
  for another shard). While that count is at least L, it skips `read_in`
  on every inbound link. It resumes at or below L/2: the hysteresis only
  saves syscalls, since correctness needs none. Frames stay in the kernel.
  TCP flow control fills the sender's socket, then its 1 MiB user buffer.
  `room()` then turns false, and the sender keeps further frames in
  `emit_node_[link]`, which its gate counts.
- **Bounded reads.** Today `read_in` reads until `EAGAIN`, with no limit
  on frames. Under D each call takes at most L − held frames and leaves
  any partial frame in its buffer. Without that, the first read after a
  resume could take the whole kernel buffer: tens of thousands of frames.
- **Readiness.** The links use level-triggered epoll (no `EPOLLET`), and
  the link epfd is in the shard's idle poll set (`sri_watch_fd`). A paused
  fd that stays registered for `EPOLLIN` keeps the epfd readable, so the
  idle `poll` returns at once and the shard spins. While paused, each
  in-fd is switched to `EPOLLRDHUP` only (`EPOLL_CTL_MOD`), and back on
  resume.
- **Newer-epoch frames.** These go to `emit_held_` (`on_link_frame`). They
  count toward L until they leave this shard.
- **What sits in transit, uncounted.** Up to `kOutCap` plus the kernel
  buffers per link. That is memory at the two ends; none of it is
  applied, so none of it is WAL.
- **Unchanged.** Link loss and resume, and the epoch rules
  (`on_link_frame`, held newer-epoch frames).
- **To verify** (section 9, V3): the only thing that reads in-links is
  `read_in`, and a stalled reader never makes either end drop the
  connection. There is no write timeout in `flush`; a zero window keeps
  TCP alive.

### 3.4 Every way an item leaves staging (each returns its credit)

| Path today (`run_emit` and callers) | Disposition | Credit |
|---|---|---|
| Runs (`run_exec`) | Applied | Returned |
| `withdraw()` (fenced) | Today `withdraw` sets `emits_paused_` and `run_emits` returns early (`shard.cpp:1791`, `2593`), so staged items never reach `run_emit`'s `withdrawn_` drop. Under D, `withdraw` drops every item in `emit_local_` that came from another shard. Their sources re-send them to the new primary (existing semantics) | Returned at `withdraw`, for every dropped item |
| `stale(e)` | Dropped; re-sent after map adoption (existing) | Returned |
| Newer epoch → `hold_emit` | Moves to `emit_held_` (existing) | Returned |
| Sealed or staged slot → `hold_emit`, or foreign → `route_emit` | Held, or re-routed as this shard's own backlog (counted by its gate) | Returned |
| Engine stop (process end) | Left for resume (existing: `Engine::stop` leaves queued follow-ups to the objects' resume hooks) | Not needed; the counters die with the process |

### 3.5 The owner applies the follow-up gate (closes the gap in section 1)

`serve`, `serve_multi` and `run` (the forwarded EXEC, CREATE and DELETE
paths) refuse a MUTATE, CREATE or DELETE with `E_OVERLOADED` while the
owner's `emits_in_flight` is above `kEmitBacklog`.

- The check is made before `run_exec`, the same check as at the origin
  (READs exempt, legacy as today).
- The answer goes back as an ordinary RESP or MULTI_RESP, so nothing
  waits on it (B3).
- Replay is safe: only unsequenced requests are forwarded. A sequenced
  request to another shard's object is refused with `E_NOT_OWNER` at
  the origin (`shard.cpp:207-213`), so the owner never answers a resend
  with a refusal.

With this, every client op that can emit is gated by the backlog of the
shard that will hold its follow-ups.

### 3.6 What does not change

- The client gate `kEmitBacklog` and its formula, `emits_in_flight`.
  Items waiting for credit stay in `emit_remote_` and count toward the
  sender's gate. Staged items sit in `emit_local_` and count toward the
  destination's gate, as inbound queued items do today. So while staging
  holds more than 1024, the destination refuses its own clients with
  `E_OVERLOADED`, which it should do while its WAL is over H anyway.
- S1 client admission (budget B), S1.1, S1.2, S2, S3, S4, checkpoint, WAL
  and wire formats, recovery, manifest and superblock, lifecycle,
  replication, promotion.
- `Engine::wait_emits`: staged items are in `emit_ready_count_`, so
  quiescence still waits for them.
- `Engine::stop`: its loop waits for sent = received (drained). Staged
  items are drained, so stop is not held by a closed intake.
- What does change indirectly: S2 pacing reads `emits_in_flight` against
  `kEmitBacklog` as checkpoint "pressure" (`shard.cpp:2214-2215`,
  `ckpt_pacer.h:85-86`). A sender held back by credits therefore reports
  more pressure and paces its checkpoint walk harder. That is the
  intended reading of a backlog, but S2's behaviour under D is part of
  F1's measurements.

## 4. Cap semantics (review section 11)

| Question | Answer |
|---|---|
| Destination intake signal | Local only: the shard's own `wal.active_bytes()` against H, checked per item at apply. Senders learn it only through credits that stop returning; no state is published. |
| Client budget vs internal cap | Clients are admitted under B (S1, unchanged); follow-ups under H, with H ≥ B + `need_f`. Above B clients are refused while follow-ups keep settling: follow-ups have priority, as the existing "follow-ups first" gate already intends. |
| Hysteresis / reopen | No hysteresis on the WAL gate. There is no open/close state and no message on a change, so flapping costs one compare. Links resume reads at L/2 (only to save syscalls). |
| Per shard or per node | Per shard: each shard has its own WAL stream and flush. The node bound is the sum. |
| Bytes, records or both | WAL gate in bytes (worst case per item). Credits and link staging in items (fixed 64-72 B). |
| Worst-case overshoot | WAL: none above H, except one oversized item run alone on an empty buffer (`active ≤ max(H, need_f)`). Staging: credit-bearing items none above (N−1)·W, link-origin items none above L; the shard's own and derived items are counted by its gate, not bounded (6.2). |
| Message classes drainable above the cap | All of them, always. Only the apply of follow-ups (EMIT, link frames, local follow-ups) is gated. |
| How the sender backlog becomes bounded | Credits keep unapplied items at the sender's `emit_remote_` / `emit_node_` queues, where `kEmitBacklog` refuses its clients (section 6). |
| Local follow-ups | Same queue and same gate as received ones (F7). |
| Cross-node | Same gate at apply. Bounded intake by pausing link reads (3.3). |
| Shutdown / fencing / migration / recovery | Section 8. |

## 5. Defaults (hypotheses, not frozen)

| Parameter | Proposed initial value | Basis | To be set by |
|---|---|---|---|
| H, intake cap | 2 × B (60 MiB with today's B = 30 MiB). Hard constraint H ≥ B + `need_f` (3.1) | A15 reached 50.9 MB; H must leave room for a settle wave above B | R640 preregistration (not the dev VM) |
| W, credits per pair | 64 | Pipelining needs at least one loop's worth (`batch_in` = 32), with headroom. Staging memory (N−1)·W·72 B = 32 KB at N = 8 | Measured throughput (section 9, P1) |
| L, link-origin hold | 1024 | Same order as `kEmitBacklog` | F8 |

Config fields with env overrides, the pattern S4 used.
- **Rollback:** one switch, `intake_gate = off`, with W = 0 and L = 0,
  restores today's behaviour.
- H = 0 is not "off": by the 3.1 formula it means one item per empty
  buffer, so a configuration that breaks H ≥ B + R is rejected at start.
- The owner gate (3.5) has its own switch for the same rollback.

## 6. Proofs

Notation:
- N shards on the node;
- G = `kEmitBacklog`;
- E = the most follow-ups one MUTATE op may emit. The engine does not
  limit it today; see Q2;
- R = `need_f`.

### 6.1 B1: destination WAL bound

Every append on shard d comes from one of:
- (a) client-admitted work, admitted only while `active + need ≤ B` or
  `active = 0`;
- (b) follow-up applies, admitted only under the gate in 3.1;
- (c) the other non-refusable records in S1.1 section 3 (session-expiry
  deletes, seal and unseal, migration installs bounded by
  `move_window_bytes`), unchanged and outside this slice.

Recovery replay runs before serving.

Each admission checks and appends with no yield in between. Flushes
start only in `wal_->poll`, never inside `run_exec`. A follow-up apply
appends at most its own record and one peer record (`shard.cpp:621`,
`634`), and no session record, since `client = false`. So when (a) or
(b) admits an item, `active` was at most H − need, or 0, and becomes
at most H, or the one item. Since B ≤ H, a client admission never takes
`active` above H either. Hence

    active ≤ max(H, R, client worst case) + (c)

WAL memory is the active buffer plus the flushing buffer it was swapped
with (`start_flush`), so the bound on memory is about twice this, plus the
replication sender's retained chunks.

whatever the senders do. This holds whether d's WAL is stalled or slow:
the bound does not depend on the senders' rate. A15 under D: its 50.9 MB
becomes at most H, and F1 checks that.

### 6.2 B2: sender bound

The follow-ups a shard s holds, `emits_in_flight(s)`, come from three
sources:

1. **Originated by client ops on objects s owns.** These are admitted
   only while the count is at most G: at the origin as today, and at the
   owner for forwarded work (3.5). Each op adds at most E, so the total is
   at most G + E. Items blocked by credits or by `room()` stay in this
   count; they are not forgotten. Without 3.5 this item would be false.
2. **Originated by a resume pass** (recovery, link reconnect). At most
   the objects' pending entries: for accounts, 64 per account
   (`kMaxOutbox`) plus the inbox entries. This is bounded by state, not
   by the engine; it is today's behaviour.
3. **Received follow-ups waiting at s.**
   - Credit-bearing: at most (N−1)·W.
   - Link-origin: at most L.
   - **Derived**: follow-ups that s's own applies of received items emit.
     These go into s's queues, where its gate counts them but does not
     limit them.
   - Also counted, though no credit limits them: s's own local follow-ups
     and re-routes (`release_held`, the `emit_result` fail op, the
     `resume_all`, `map_changed` and `resume_slot` passes).

In transit and uncounted:
- intra-node, at most W per pair (the credits);
- cross-node, at most `kOutCap` plus the kernel buffers per link.

Neither is WAL.

**Derived follow-ups toward a closed destination d.** The engine does
not bound these; only the application's state does.
- For transfers, every follow-up op emits at most one follow-up, back to
  the source of the item that caused it:
  - credit→settle;
  - settle→forget (or →credit on retry);
  - query→forget (or →credit);
  - bounced→none.

  So the derived items toward d are answers to items from d. Each
  corresponds to an open outbox entry of a d account (64 per account,
  `kMaxOutbox`), or to an inbox entry, plus duplicates from a resume.
- That is a state bound, not a rate bound. As S1.1 section 3 says, with
  a million accounts it is not a useful number (about 2 TB of WAL there;
  here 72 B of memory per item, so about 4.6 GB at 64 M entries).
- A rate argument ("drains as fast as it fills") does not hold in
  general. With H − B small, d applies settles at about one per flush
  while d's own clients keep originating credits under B.

Transfers are therefore bounded by object state, not structurally.
Section 7 asks the review to choose the policy. For a **forwarding** type
(A emits to B, whose op emits to C), not even state bounds it: B can
queue follow-ups for a closed C at the rate its sources are admitted.
Either way the cost is memory only. B1 still holds at C, and at B for
B's own applies.

### 6.3 B3: response and control progress

Inbound rings are drained exactly as today; `drain` and `on_message` are
unchanged. An EMIT is moved out of the ring into memory whatever the
gate says. So no RESP, BUF_RETURN, MULTI_RESP, request or CTRL message
can be behind an unconsumed EMIT.

Requests that reach a closed shard are answered by S1 admission (refused
with `E_OVERLOADED`, or served as a READ), never left waiting on intake.

Node links carry only follow-ups. Pausing their reads holds back nothing
else: admin, replication, the witness and clients use other sockets. F2
tests both cases.

### 6.4 B4: no cyclic deadlock

The waits D introduces:
- (i) a staged item at d waits for d's `active` to fall to H − R or to 0;
- (ii) an item at s waits for a credit from d, that is, for some item from
  s to leave d's staging;
- (iii) a link frame waits for d to resume reading, that is, for d's
  link-origin count to fall.

(ii) and (iii) reduce to (i). (i) depends only on d's flush completing,
which waits on the disk and never on another shard or on intake (section
1). So every chain of waits ends at a WAL flush: the wait-for graph has no
edge from one shard's intake to another's, and therefore no cycle. This
needs the credit-conservation invariant (3.2). A leak is not a cycle but
a permanent stall of one pair, so F9 tests it directly.

What the proof excludes on purpose: any intake condition on the shard's
own outbound backlog (section 7.1).

### 6.5 B5: eventual progress

When d's disk completes the flush in flight:
1. the next flush takes the active buffer, and `active` falls;
2. the head staged item passes the gate and runs;
3. its credit is published at the end of the iteration;
4. s, woken by the credit wake (3.1), sends more, and link reads resume
   below L/2.

Each staged item runs within finitely many flushes, because staging is
FIFO and every item fits under H or runs on an empty buffer. Every
accepted follow-up then settles by today's protocol (wait_emits reaches
0 in F4). This assumes the gated shard is woken by the I/O completion
(3.1).

### 6.6 B6: no discard, duplicate or client refusal

- **No discard.** Items leave staging only by the dispositions that
  `run_emit` already has (3.4); D adds none.
- **No duplicate.** No follow-up is re-sent by the engine because intake
  closed: credits stop the sender, they do not make it retry.
- **No client refusal.** Follow-ups have no client, so none of them can
  become one. Clients see `E_OVERLOADED` only from the existing gates,
  with their existing meanings.
- **Order.** Order per source is kept (section 2).

### 6.7 B7: recovery

The staged items, the credit counters and the paused reads live only in
memory. A crash loses them exactly as it loses today's ring contents and
sender queues. Recovery is today's: replay, then `resume_all`, and the
objects re-emit everything unsettled (the durable truth lives in the
objects, docs/TRANSFERS.md). At start, staging is empty and every counter
is 0.

F6 kills the process while senders are backed up and staging is full,
then checks conservation and settlement after restart.

## 7. Decision for the review: derived backlog (B2, general case)

### 7.1 Why "close intake when my own backlog is high" is rejected

Suppose A and B each close intake while their own `emits_in_flight`
exceeds some H_q.
1. A's backlog is mostly follow-ups to B; B's is mostly follow-ups to A.
2. A's backlog drains only when B applies, but B is closed. B's backlog
   drains only when A applies, but A is closed.
3. Both disks are healthy and both WALs are empty, yet nothing moves
   again.

Transfers reach this state: mutual transfers between two shards under
load, after one stall, can push both backlogs over H_q. F9 negative N5
builds it, to show the deadlock is real and that D avoids it.

### 7.2 Options

| | Policy | Bound | Cost and risk |
|---|---|---|---|
| R1 (recommended now) | Rely on the protocol shape. State an engine contract: an op that runs as a follow-up emits only to its source, or emits a bounded number of follow-ups in total per originating client op. Add a gauge for derived items held and a diagnostic hard-max counter (S1's policy for diagnostics), no gate. | Bounded by object state for transfers (6.2), not structurally; memory only (72 B per item), never WAL; diagnosed | No new mechanism. A future forwarding type must be designed against the contract. B2 then holds structurally for originated work only. |
| R2 | A backlog gate with an escape: close intake for backlog only, except for items from the shards that backlog points to. | Plausible, but unproven: an amplifying ping-pong can grow both sides. | Needs its own proof, and per-destination accounting on the hot path. |
| R3 | Shed and resume: cap each per-destination queue; past the cap, drop the newest and mark the destination "resume owed". When credits return, run the objects' resume for the affected targets. | Structural for every type | Changes the intra-node guarantee "never dropped" (B6 would rely on resume, as node links already do). Every emitting type must have a complete resume. `resume_all` costs O(objects). RECONSIDER-class. |

Recommendation: **R1** now, with the gauge, while being clear that it
leaves derived backlog bounded by state, not by the engine. Revisit R3 only if a
non-reply-shaped type is proposed. In both R2 and R3 the WAL bound B1
holds unchanged: what is at stake is sender memory.

## 8. Lifecycle interactions (review section 11, last row)

- **Synchronous I/O fallback (predates D).** With
  `sri_debug_force_sync_io`, or a full submission queue, `pwritev2` blocks
  the shard thread (`runtime.cpp:207-210`). A stalled disk then stops all
  draining. This is a B3 hole that D does not change; it is noted, not
  fixed.
- **Startup.** Emits are paused until recovery finishes
  (`pause_emits`), as today. Received items run inline today even while
  paused ("paused shards still run follow-ups that arrive"); under D they
  still pass the gate.
- **Shutdown.** `Engine::stop` pauses sending and waits for sent =
  received, that is, drained; staged items are already drained. It then
  stops the runtime, and staged or unsent items are left to the objects'
  resume, as today. A stalled WAL at stop delays only the runtime's WAL
  quiesce, as today.
- **Fencing (`withdraw`).** `withdraw` drops the staged items that came
  from other shards and returns their credits (3.4). Senders are
  therefore not stalled by a fenced shard, and their sources re-send to
  the new primary. `withdraw_flag_` spreads the fence across the node, as
  today.
- **Slot moves.** Sealed or staged slots hold the item in `emit_held_`
  (unbounded today, while the slot is sealed; outside this slice), with
  the credit returned. A foreign target is re-routed. Epoch handling
  (`stale`, `moved_at_`, newer-epoch holds) is unchanged.
  - To verify (V2): no move step waits for follow-ups to settle while
    holding something that d's flush needs. Today a stalled WAL already
    blocks such a wait, so D adds nothing, but this must be checked.
- **Recovery.** Section 6.7.
- **Standby and promotion.** A standby applies the WAL stream and runs no
  follow-ups. Promotion runs `resume_all`, whose emits pass through the
  same gate.

## 9. Test plan (review section 13)

All tests run with 8 shards where the harness allows. Each stalls a
chosen shard's WAL with `Engine::debug_hold_wal`, and reads gauges for
the counts. Those gauges count credit-bearing, link-origin, own and
derived items separately, so each bound is checked on its own class.

`debug_hold_wal` skips `wal_->poll` altogether (`shard.cpp:855`), so a
spin or CPU measurement under it would mislead. V4 uses a hook that
delays flush completion instead.

The tests size B and H so that ungated intake would exceed H by a wide
margin. A15 as it stands (50.9 MB, below H = 2B = 60 MiB) would pass
with no gate at all.

| Test | Setup | Pass criterion |
|---|---|---|
| F1 | Stall d with a small H (for example B + 4R) and enough senders that ungated intake is at least 3H; 7 healthy senders transfer to d's accounts, including transfers on sender-owned accounts forwarded from clients on other shards (3.5) | d's `active` ≤ max(H, R) for the whole stall; credit-bearing staging ≤ 7W; senders' `emits_in_flight` ≤ G + E + own + derived; forwarded mutations refused at the owner past G; S2 pacing pressure recorded |
| F2 | Stall d; fill s→d credits; then (a) s forwards a READ and a MUTATE to d, (b) a MULTI batch, (c) admin HEALTH and STATUS on d | (a) and (b) are answered (READ OK, MUTATE `E_OVERLOADED` or OK) within a bound while intake stays closed; (c) answers promptly |
| F3 | Stall every shard; mutual transfers in every pair; then release all | No deadlock: all settle; each shard's `active` ≤ max(H, R) throughout |
| F4 | Stall d with full staging; release. Variant F4b: during the stall, push at least W + 1 items per pair through `hold_emit` (seal, then unseal, the target slot) and through `stale()` (move a slot or bump the epoch), and through `withdraw` on a fenced copy | Every staged item applied exactly once and in per-source order (a test type logs (source, seq) and checks there are no gaps, duplicates or reordering); money conserved; inbox and outbox empty; `wait_emits` reaches 0. F4b: `sent − taken` returns to 0 on every pair |
| F5 | F1 and F3 setups; sample every queue | Per pair: `sent − taken` ≤ W. Per shard: credit-bearing staged ≤ (N−1)W, link-origin ≤ L; own and derived items reported against the state bound. Ring occupancy and runtime overflow recorded |
| F6 | F1 state, then the process is killed (SIGKILL, as `test_crash`) | After restart: conserved, everything settles, no duplicate credit (inbox checks), `wait_emits` reaches 0 |
| F7 | A test type emitting both local and cross-shard follow-ups into a stalled shard | Both kinds stop at the gate; bound as F1 |
| F8 | Two engines with node links (`test_cluster` harness); stall the receiving shard | Its `active` ≤ H; inbound reads pause at L; the sender's `emit_node_` grows and its gate refuses its clients; release → settles. Otherwise scope out cross-node with a follow-up item |
| F9 | Mutations | N1: no gate → F1 fails (bytes > H). N2: stop draining at an EMIT head → F2 times out. N3: no credit on the hold, stale or withdraw path → F4b leaves `sent − taken` above 0 and stalls the pair. N4: credits counted at drain, not at apply → F5 credit-bearing staging > (N−1)W. N7: `serve_emit` ignores staged items and runs new arrivals inline → F4 order check fails. N8: no owner gate (3.5) → F1 owner backlog > G + E. N5: backlog gate (7.1) → F3 deadlocks (proves the rejection). N6: link reads not paused, or not frame-limited → F8 link-origin > L |

Implementation verifications:
- **V1.** Nothing relies on EMIT being applied before a later message of
  another kind from the same sender (grep `on_message` consumers; review
  MULTI and RESP paths).
- **V2.** No wait in slot moves or checkpoints holds what a WAL flush
  needs while waiting for follow-ups (section 8).
- **V3.** Stalled link reads never cause a disconnect at either end
  (section 3.3).
- **V4.** Neither a gated shard nor a credit-blocked sender spins (CPU
  measured under the delayed-completion hook, not `debug_hold_wal`). A
  sender wakes when its credit returns, and a destination when its flush
  completes.

Performance:
- **P1.** Transfer benchmark (`test_transfer` and the S1 q4 harness) at
  W = 16, 64, 256 and with no window, run serially and interleaved as
  in S0. Report the overhead of the credit check. No regression beyond
  run-to-run noise at the chosen W.

Every slice rule still applies:
- full serial Release, ASan+UBSan and TSan runs, with zero sanitizer
  reports;
- every run reported in order;
- an independent review.

## 10. Questions for the review

1. **Q1, derived backlog policy:** R1 (recommended), R2 or R3 (section 7).
2. **Q2:** add an engine limit on follow-ups per MUTATE apply (for
   example 64, as a contract violation)? E is unbounded today, and the B2
   arithmetic needs a value. Resume passes would stay exempt; they are
   bounded by object state.
3. **Q3:** confirm the shape of the defaults: H ≥ B + R, initially 2B; W
   initially 64; L initially 1024. All are hypotheses, set later by
   preregistration.
4. **Q4:** the `u32` WAL buffer finding (section 1). It is reachable
   without follow-ups (a standby's unbounded `append_raw`, legacy
   admission, a large configured budget). Recommendation: fix it now as
   its own small reviewed change: a fatal error before the active buffer
   would pass a limit below 2 GiB, with a test. A bound on the standby's
   receive side would be separate work.
5. **Q5:** confirm the owner-side gate (3.5). It closes a B2 gap that
   exists today, and it refuses forwarded mutations at a busy owner that
   are accepted today.

## 11. Not in this slice

Any runtime, ring or drain change; S5; S6; R640; CP-1A; comparators; B2
begin-snapshot work. The implementation waits for this design's review.

## 12. Independent review of this note

A read-only reviewer checked the first draft against the code at 83280f9
and the work order.

**It confirmed:**
- B1, including `need_f`, the append sites and the no-yield path;
- B3;
- B4: WAL drain depends only on the local flush, and sync replication
  changes when d sends, not d's gate;
- the 7.1 deadlock;
- per-source order;
- the node-link facts;
- the `Emit` size;
- `Engine::stop` and `wait_emits`.

**It found the following, all corrected above:**

| Finding | Severity | Correction |
|---|---|---|
| The owner applied no backlog gate to forwarded mutations, so B2 item 1 was false | HIGH | Section 3.5 (owner gate), F1, N8 |
| "staging ≤ (N−1)W + L" counted items that carry no credit; `read_in` had no frame limit | HIGH | Bounds stated per class (0, 3.3, 4, 6.2); frame-limited reads; per-class gauges |
| N3 did not discriminate: F4 sent nothing through the hold or stale paths | HIGH | F4b |
| Spinning and wake-ups were mis-specified: no credit wake; `WalStream::poll` is busy while active > 0; the paused epfd stayed readable; `debug_hold_wal` skips the poll | MEDIUM | 3.1 busy and wake rules, 3.3 `EPOLL_CTL_MOD`, V4 hook |
| The derived-backlog argument for transfers was a rate, not a bound | MEDIUM | 6.2 restated as a state bound; R1 wording |
| The `withdrawn_` row could not be reached for staged items | MEDIUM | 3.4 and 8: drop at `withdraw` with credits returned |
| The scope of the u32 finding was wrong (standby, legacy, configured budget) | MEDIUM | Section 1, Q4 |
| The order mechanics inside `run_emits` were unspecified; `from` could return a credit twice | MEDIUM | 3.1, 3.2, N7 |
| A15's bytes are below H, so that check passes with no gate | MEDIUM | F1 sizing |
| S2 pacing reads the backlog; WAL memory is twice the active bound; H = 0 is not "off", and H ≥ B + R is needed for B5; the sync-I/O fallback; newer-epoch frames and L; creates and deletes are gated only outside legacy | LOW | 3.6, 6.1, 3.1, 8, 3.3, 1 |
