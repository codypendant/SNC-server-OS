# Follow-up intake backpressure: design note, revision 2 (for review)

Status: **design only, for review. Nothing here is implemented.**

History:
- **Revision 1** (PR #83): option D, reviewed independently (section 13).
- **cody's review of revision 1** (`docs/handoff/PR83_Design_Review.txt`):
  - option D is approved as the base architecture, but implementation is
    not authorized;
  - R1 ("protocol shape + gauge") is rejected, because it is not a
    structural bound;
  - follow-up execution must become request/reply-shaped as an enforced
    runtime contract, with an enforced `OpDescriptor::max_emits`;
  - the WAL u32 overflow had to be fixed first. It was: PR #84.
- **Revision 2** (this note) adds that contract (section 3.3). It also
  adds the one mechanism the contract makes possible and the structural
  bound needs: a per-origin **exchange budget** (section 3.4).

Work orders:
- `docs/handoff/PR82_S41_Review.txt` sections 8-14;
- `docs/handoff/PR83_Design_Review.txt`.

The problem: [S1_1_CORRECTIONS.md](S1_1_CORRECTIONS.md) section 3, and A15.

Code read (main at 3bcde70):
- `src/native/ring.h` and `src/native/runtime.cpp`;
- `src/core/shard.{h,cpp}` (`execute`, `followups_refuse`, `admit`,
  `serve`, `serve_multi`, `on_message`, `OpCtx::emit`, `queue_emits`,
  `route_emit`, `run_emits`, `run_emit`, `send_emit`, `serve_emit`,
  `hold_emit`, `release_held`, `emit_result`, `on_link_frame`,
  `resume_all`, `resume_slot`, `map_changed`, `withdraw`);
- `include/snc/registry.h` and `include/snc/msg.h`;
- `src/core/node_link.{h,cpp}` and `src/core/storage.cpp`;
- `Engine::stop` and `Engine::wait_emits`;
- `src/types/account.cpp`, the only type that emits;
- `docs/TRANSFERS.md`;
- `tests/test_s1_admission.cpp` A15.

## 0. Summary

Option D stays as approved (items 1-3, 6, 7). Item 4 is the contract the
review asked for. Item 5, the exchange budget X, is a **new client-visible
gate** that is not part of the approved D, and needs its own approval
(Q5). Every mechanism below acts on the apply of follow-ups, or on
admission. None stops a ring.

1. **Rings always drain.** Every inbound ring is drained completely, as
   today, so no answer or control message can be trapped behind a
   follow-up.
2. **The WAL gate is at apply.** A drained follow-up is applied only
   while the shard's WAL has room under a hard cap H, with H ≥ B + R.
   The provisional value is H = max(2B, B + R). Otherwise the follow-up
   waits, in order, in the shard's local follow-up queue (its "staging").
3. **Per-pair intake credits (W).** A sender has at most W follow-ups to
   one destination that the destination has not yet taken out of
   staging. Past that, they wait at the sender, in its own counted queue.
4. **The reply-only contract (new, enforced).** An op running as a
   follow-up may emit only to its source, and at most once. Every op
   declares `max_emits`, and the runtime enforces it. A forwarding mode
   would need its own review.
5. **The exchange budget (X, new).** The contract makes every follow-up
   part of an **exchange**: a chain between two objects with at most one
   follow-up in existence at a time. A shard admits client ops that may
   emit (`max_emits > 0`) only while it has fewer than X of its own
   exchanges unfinished. Replies never wait for this budget; only
   admission does.
6. **The owner-side gate.** The shard that owns an object applies the
   `kEmitBacklog` gate, and the X budget, to mutations, creates and
   deletes forwarded to it. It answers `E_OVERLOADED`.
7. **Bounded node-link read pausing.** A shard stops reading inbound
   node links, frame-limited, while it holds L or more follow-ups from
   links.

**Result: B2 becomes structural intra-node, given Q1.** Under the
contract, applying a follow-up never increases the number of follow-ups
in existence (conservation, 6.2). Follow-ups are created only when an
exchange starts, and client starts are bounded by X at their origin. So
every follow-up queue of an intra-node exchange, at senders and
destinations, including the "derived" ones that revision 1 left to object
state, is bounded by N·(X + E) plus resume and re-route roots (6.2).
Resume roots are structurally bounded only if resume passes are paced by
X (Q1). Exchanges that cross nodes are bounded structurally only with
D.2.

**Deadlock freedom (B4).** No follow-up in flight ever waits for X. Only
client admission waits for X, and it is refused rather than queued.
Follow-ups wait only for the destination's WAL (H) and for intake
credits (W), which also depend only on that WAL. Every wait therefore
ends at a disk flush (6.4).

**Cross-node.** A structural bound across nodes needs exchange
completions to travel back over the node links, which changes the wire
format. Revision 2 specifies that as sub-slice D.2 (3.6) and asks to
build intra-node D.1 first (Q2). Until D.2, cross-node is bounded by
link pausing and the sender gates, as in revision 1.

The defaults H, W, L and X are hypotheses. None is frozen from the
dev VM.

## 1. What happens today

**Sending.** An op's `apply` emits. `queue_emits` holds a remote
follow-up until the emitting record is durable (`emit_wait_`). Then
`route_emit` puts it on one of three FIFO queues:
- `emit_local_`, for a target on this shard;
- `emit_remote_[d]`, for another shard on this node;
- `emit_node_[link]`, for another node.

`run_emits` sends `emit_remote_[d]` with `sri_claim`, without
`must_deliver`: it goes to the ring first, then to the runtime's per-pair
overflow of up to 256 entries. Past that the send is refused, and the
rest stay queued in order.

**The sender's gate.** `emits_in_flight() = emit_ready_count_ +
emit_wait_.size()` counts only what is still queued at the shard. A sent
follow-up leaves that count.
- Client `execute` is refused with `E_OVERLOADED` while the count is
  above `kEmitBacklog` (1024). So are creates and deletes, unless
  admission is legacy.
- Owner-local READs are exempt.
- The gate runs only on the shard where the request arrives
  (`shard.cpp:177-182`, `385-389`).
- A mutation forwarded to its owner (`serve` 1515, `serve_multi` 1386,
  `run` 440) passes only WAL admission B there. This is a gap today.

**Receiving.** `drain` hands every message of a ring to `on_message`; one
FIFO carries every kind. For an EMIT, `serve_emit` either runs it at
once (`run_emit` → `run_exec`, `client = false`, never refused) or queues
it behind a non-empty `emit_local_`. Either way it appends its record
whatever the WAL holds.

**The problem (A15).** While d's WAL stalls, healthy senders keep
admitting and sending, and d logs every follow-up. A15 delivered 2,050
credits to one stalled shard: 50.9 MB of WAL, 1.6× the client budget.

**Who emits, and what.** Today only `Account` emits
(`src/types/account.cpp`). Its follow-up ops already reply only to their
source:

| Op | Runs as | Emits | To |
|---|---|---|---|
| `transfer` | client op | 0 when the target is on this shard and `local()` finds it (applied in place); otherwise 1 (`credit`), including on this shard when `local()` returns null (slot sealed or staged, object missing, wrong type), when it goes to `emit_local_` and may bounce | the target (starts an exchange) |
| `xfer_credit` | follow-up | exactly 1 (`settle`: ok, refused or retry) | the source |
| `xfer_settle` | follow-up | 1 (`forget`, or `credit` on retry), or 0 if there is no matching entry (including on a retry) | the source (`outbox[i].to == source` is checked) |
| `xfer_query` | follow-up | 1 (`forget` or `credit`), or 0 | the source |
| `xfer_forget` | follow-up, unlogged | 0 (unlogged ops may not emit) | none |
| `xfer_bounced` | follow-up (the engine's fail op) | 0 | none |
| `deposit`, `withdraw`, `set_email`, `create`, `delete`, reads | client | 0 | none |
| `resume` hook | after recovery, link-up and map change | per object: one `credit` per outbox entry, one `query` per inbox entry | any (starts exchanges) |

The engine's own fail op is also reply-shaped: when a target is missing,
`emit_result` runs `fail_op` on the source.

**Other facts relied on:**
- **WAL drain depends only on the disk.** The active buffer grows while a
  flush is in flight, and the next flush takes it.
- **The buffer is now capped.** It grows in checked u64 up to
  `max_active_bytes` (1 GiB), with a fail-stop (PR #84).
- **Node links** carry only follow-up frames: one TCP connection per
  (shard, remote shard), with no acknowledgements.
  - `room()` allows up to `kOutCap` (1 MiB) per link.
  - `read_in` reads until `EAGAIN`.
  - A dropped link loses frames by design; each new connection re-runs
    `resume_all`.

## 2. The FIFO problem and the choice of D

Messages from s to d share one SPSC FIFO with every other kind: requests,
RESP, BUF_RETURN, MULTI_REQ, MULTI_RESP. If d stopped consuming at an
EMIT, the answers behind it would be trapped. Those answers release s's
waiters, pending slots and buffers, so the throttle would become a
protocol deadlock.

| | Mechanism | Response/control progress | Bound on unapplied follow-ups | Cost |
|---|---|---|---|---|
| A | A second ring per pair for EMIT only; d stops draining it | Structural | Ring + overflow per pair (512); the overflow is uncounted at the sender | Runtime topology change in both backends; sleep checks; order between kinds changes |
| B | Credits only: s sends while d has granted room | d keeps draining | WAL overshoot up to (N−1)·W × the largest record | One counter per pair |
| C | Drain all; stage EMITs in a bounded queue at d | Structural | Needs a bound on what is in the rings: that is B | Copies |
| **D (approved)** | **C + B, with credits counted when an item leaves staging** | **Structural: the drain never stops** | **Staging ≤ (N−1)·W per shard; WAL overshoot only the empty-buffer case** | **Shard code, plus one N×N counter matrix** |

**Order.** Every follow-up from s to d goes through one FIFO
(`emit_remote_[d]`, then the ring, then staging), so per-source order is
kept.

Revision 2 keeps this. In particular, **no follow-up ever waits inside
a queue for the exchange budget** (section 7.3 shows why a queue wait
would be unsafe).

D adds no new kind of reordering between message kinds. Today an EMIT
can already run after a later RESP from the same sender whenever
`emit_local_` is non-empty. V1 still checks that nothing relies on the
order.

## 3. Mechanism

### 3.1 The WAL gate at apply (destination), unchanged from revision 1

A follow-up is applied only while

    wal.active_bytes() + R <= H    or    wal.active_bytes() == 0

where `R = need_f = record_size(max over types of max_size) +
peer_record_max_`. This is S1's worst case, using the largest type,
because the target's type is known only after the lookup.

- **H.** H ≥ B + R is a hard constraint, checked at start.
  - The provisional value is H = max(2B, B + R).
  - If H were below B + R, clients admitted under B could refill the
    buffer after each flush starts, and the head follow-up could starve
    (B5).
- **The check.** It is made per item at the head of the queue,
  immediately before `run_exec`, with no yield before the append (R3, as
  in S1). The empty-buffer clause is S1's progress rule.
- **Where.** `run_emits` stops at the first gated item. The rest of its
  batch goes back in front of anything appended since, with
  `emit_ready_count_` restored. `serve_emit` does not run inline while
  the gate is closed or `emit_local_` is non-empty.
- **Unlogged ops** keep their place in the FIFO.
- **Busy and wake-up.** A gated shard is not "busy" because of its
  staging; it wakes on the I/O completion of its WAL flush. A sender
  blocked on credit is not busy either: d writes s's eventfd when it
  advances `taken` for a pair whose "sender waits" flag is set.
- **Rollback.** The switch `intake_gate = off`, not H = 0.

### 3.2 Per-pair intake credits W (intra-node), unchanged from revision 1

- `taken[s][d]` is written only by d, padded to a cache line, and
  published once per loop iteration. d adds one each time an item from s
  leaves its staging, by any path (3.7), and also when `serve_emit` runs
  an item inline without staging it (`shard.cpp:2715`).
- s sends the head of `emit_remote_[d]` only while
  `sent[d] − taken_cache[d] < W`.
- `Emit` gains a memory-only field `u16 from`. It is cleared when the
  item is held or re-routed, at the moment its credit is returned, so the
  credit cannot be returned twice.
- **Invariant:** `sent − taken` equals the items from s that are in the
  ring, in the overflow or in d's staging, so it is at most W.
- W = 0 means no window.

### 3.3 The reply-only contract and `max_emits` (new, enforced)

**`OpDescriptor::max_emits` (u16, default 0).** It is the most follow-ups
one apply of the op may emit. `OpCtx::emit` counts emits per apply and
raises `contract_violation` (fail-stop, as other contract checks do) at
emit number `max_emits + 1`. An op that does not declare it may not emit
at all, so a new emitting type has to opt in explicitly.

**Emit only inside apply.** Today `OpCtx::emit` checks only the op kind,
so `validate` could call it. The contract allows `emit` only during
`apply` of a logged op (unlogged ops already may not emit, `shard.cpp:598`)
and during resume hooks; anything else is a `contract_violation`.

**Reply-only.** When an op runs as a follow-up (`ctx.source()` is not
null, that is, it was reached through `run_emit`):
- it may emit only to `ctx.source()`. Any other target is a
  `contract_violation` ("follow-up emitted to a target other than its
  source");
- it may emit at most once, whatever `max_emits` says. That keeps the
  exchange a chain (6.2). A follow-up op therefore declares `max_emits`
  of 0 or 1. The runtime check enforces this; a registration-time check
  needs a declared follow-up flag (Q4).
- **Forwarding and fan-out are excluded.** Emitting to a third object, or
  more than once from a follow-up, is reserved for a future, separately
  reviewed mode with its own bound (section 7.4).

**Unchanged by the contract:**
- **Client-run ops** emit up to `max_emits` follow-ups, to any target.
  Each one starts an exchange (3.4).
- **Resume hooks** (`resume_all`, `resume_slot`, `map_changed`) run with
  no op (`op_ == nullptr`, `shard.cpp:1666`, `3034`), so the `max_emits`
  check is skipped while `resuming_`. They emit to any target, with no
  per-object limit. Each emit starts an exchange;
  the hooks are bounded by object state. Q1 proposes pacing them.
- **The engine's fail op** (`emit_result`) is the reply of the exchange
  it belongs to.

**Declared maxima for `Account`** (the actual maxima, from the table in
section 1):

| Op | `max_emits` |
|---|---|
| `transfer` | 1 |
| `xfer_credit` | 1 |
| `xfer_settle` | 1 |
| `xfer_query` | 1 |
| `xfer_forget` | 0 |
| `xfer_bounced` | 0 |
| `create`, `delete`, `deposit`, `withdraw`, `set_email`, `get`, `get_balance` | 0 |

No test type emits today (grep: `src/types/account.cpp` is the only
emitter), so no other declarations are needed.

### 3.4 The exchange budget X (new)

**Exchanges.** Under 3.3, a follow-up applied as a follow-up emits 0 or 1
follow-ups, and only back to its source. So every follow-up belongs to
an exchange:
- an exchange starts with a **root**: an emit from a client-run apply,
  from a resume hook, or a re-route (below);
- it alternates between two objects;
- at any moment it has **at most one follow-up in existence** (queued,
  waiting for durability, in a ring, staged, held, or being applied);
- it **ends** when an apply of its follow-up emits nothing, or when its
  follow-up is dropped (stale, withdrawn).

**Accounting (intra-node).** Each shard o keeps two counters:
- `started_o`: roots it has emitted. Private to o.
- `ended[x][o]`: exchanges started by o that ended on shard x. Written
  only by x, cache-line padded, published once per iteration, read by o.
- `live_o = started_o − Σ_x ended[x][o]`, refreshed when o needs it,
  using a cache like `taken`.

**Which counter an ending updates.** The shard where an exchange ends
must know its origin o. Two cases:
- **Normal hop.** The exchange alternates between o and its peer p, so o
  is either the sender or the receiver of each hop. One spare `Msg` flag
  bit, `kMsgEmitOriginSender` (bit 5), says which. Bits 0-4 are in use;
  bit 4 is overloaded (`kMsgEmitRemote` on EMIT, the private
  `kMsgRespDurable` on RESP, `shard.cpp:21`), and bit 5 is free in both.
  `Emit` gains a memory-only `u16 origin`, with a reserved value
  `kOriginRemote` for items that arrived from a node link (their origin
  is on another node; see 3.6). No other field changes.
- **Re-route.** Within a node, slot owners are static
  (`owner[s] = s % n_shards`, `slot_map.h:26`). So a re-route never
  reaches a third shard on the same node: an item re-routed by
  `run_emit` (foreign target, `shard.cpp:2651`) or `release_held` either
  goes to another node or stays local. That is also why one origin bit is
  enough. A re-route that leaves the node ends the exchange at the
  re-routing shard (counted to o). Re-routes happen only on map and
  slot-state changes, so they are finite.

**Local exchanges** (o = p, the target on the same shard) are counted the
same way. They need no `Msg` bit.

**The admission rule (the only place X is waited on).** A client-run op
that may emit (`max_emits > 0`; today only `transfer`) is admitted only
while `live_o < X`, in addition to the existing gates. Ops that cannot
emit (deposit, withdraw, create, delete, reads) are not affected by X. Otherwise it is refused with
`E_OVERLOADED` and never queued. It is checked at the shard that will
apply it:
- the origin, for a local op;
- the owner, for a forwarded one (3.5).

Admitted, the op adds at most `max_emits` ≤ E roots. So
`live_o ≤ X + E` from client work.

**Roots that are not client work** never wait: resume emits and
re-routes count in `live_o` but are not refused. They are bounded by
object state and by the number of map changes. Q1 proposes pacing
resume scans by X to make this structural too.

**Nothing in flight waits for X.** A root is admitted by refusing or
accepting the client op that emits it. Once emitted, the root and all
its replies move through the queues under only the H and W rules.

### 3.5 The owner-side gate (revision 1, now also checking X)

`serve`, `serve_multi` and `run` (the forwarded EXEC, CREATE and DELETE
paths) refuse a MUTATE, CREATE or DELETE with `E_OVERLOADED` in two
cases:
- the owner's `emits_in_flight` is above `kEmitBacklog`;
- the op may emit (`max_emits > 0`) and `live_owner ≥ X`.

The check is made before `run_exec`, and the answer goes back as an
ordinary RESP or MULTI_RESP (B3). It is replay-safe: only unsequenced
requests are forwarded (`shard.cpp:207-213`).

### 3.6 Node links

**D.1, this slice. Bounded read pausing, unchanged from revision 1:**
- A shard counts the link-origin follow-ups it holds anywhere: staged,
  forwarded on before they leave, or newer-epoch frames held.
- While that count is at least L, it stops reading. Each in-fd is
  switched to `EPOLLRDHUP` only, so the level-triggered epfd does not
  spin the idle poll.
- It resumes at L/2.
- Each `read_in` call takes at most L − held frames, and keeps a
  partial frame for later.

**D.2, a separate sub-slice: cross-node exchanges.**
- An exchange whose two objects are on different nodes ends on either
  node. Its origin can count it only if the ending is reported back.
- D.2 adds an **end frame**: a `Frame` with a kind byte (in `pad[0]`)
  that carries an end count for its origin shard. Each node-link
  connection gets a new Hello version, which is a wire change.
- **Link loss:**
  - exchanges in flight on a dropped link are counted as ended at both
    ends: the connection's start and end counters reset with it;
  - every end frame carries the connection generation (incremented per
    connection), and an end frame for an older generation is ignored.
    Otherwise an exchange staged at the receiver during the drop could
    end later and be counted a second time on the new connection;
  - the `resume_all` that every reconnect already runs starts them
    again, as today.
- **Until D.2:**
  - roots sent to another node count in `live_o` and end when they
    leave this node, so X does not wait on another node;
  - replies arriving from another node are bounded by L (pausing) and
    by the sender's gates;
  - cross-node replies queued at a shard toward a slow node are not yet
    structurally bounded. This is revision 1's derived case, for
    cross-node only.
- F8 tests D.1, and states this scope.

### 3.7 Every way a follow-up leaves a queue (credit and budget)

| Path | Disposition | W credit (intra-node pair) | Exchange |
|---|---|---|---|
| Applied, emits 1 | Runs | Returned | Continues (the reply carries the origin) |
| Applied, emits 0; or refused by `validate` | Runs or is refused | Returned | **Ends** (`ended[x][o]++`) |
| Engine fail op (`E_NOT_FOUND`, `E_TYPE_MISMATCH`, `E_UNKNOWN_OP`) with a `fail_op` | `fail_op` runs on the source | Returned | Continues |
| The same errors with `fail_op == 0` (`shard.cpp:2744-2763`; e.g. settle, forget or query to a deleted target) | Dropped (existing) | Returned | **Ends** |
| Arrives while `withdrawn_`, inline from `serve_emit` (`run_emit` returns early, `shard.cpp:2638`) | Dropped (existing) | Returned | **Ends** |
| Run inline by `serve_emit` (`shard.cpp:2715`), never staged | As the rows above | Returned when it runs | As the rows above |
| `route_emit` drops: misrouted `e.remote` (2564); no links (2572, where recovery resume roots to other nodes go) | Dropped (existing) | n/a (not yet sent) | **Ends** |
| Link-down purge of `emit_node_` in `run_emits` (2610); misrouted link frame in `on_link_frame` (1700) | Dropped (existing) | n/a | **Ends** (a local root; a link-origin item has no local origin) |
| `stale(e)` | Dropped (existing) | Returned | **Ends**; `map_changed` resume starts it again |
| `withdraw` | Today staged items are never dropped (`emits_paused_` stops `run_emits`). Under D, `withdraw` drops every item in `emit_local_` that came from another shard | Returned | **Ends**; the new primary resumes |
| Newer epoch, or sealed or staged slot → `hold_emit` | Held (existing) | Returned | Continues while held (a slot seal is bounded) |
| Foreign → re-routed (to another node, 3.4) | Re-routed (existing) | Returned | **Ends** here |
| Engine stop, or crash | Left for resume | Process-wide reset | Process-wide reset |

Every row says what happens to both counters. "Ends" means any
disposition that emits nothing and leaves no item. A path that changes
neither is a leak: enough leaks would make the shard refuse emitting
client ops forever, a liveness outage. F9 (N3, N10) tests it, and the
implementation adds a debug invariant: once the node is idle, Σ `live_o`
equals the follow-ups counted in every queue (zero when quiescent).

### 3.8 What does not change

The following stay as they are:
- `kEmitBacklog`, and its formula `emits_in_flight`;
- S1 admission (B), S1.1, S1.2;
- S2, S3, S4;
- checkpoint and recovery;
- the WAL and checkpoint formats, the manifest and the superblock;
- lifecycle, replication and promotion;
- the intra-node ring wire format: one spare flag bit is used, and no
  field changes;
- `Engine::wait_emits` (staged items stay in `emit_ready_count_`);
- `Engine::stop` (sent = received is reached because drains continue).

Two things change indirectly:
- S2 pacing reads `emits_in_flight` as pressure, and credit waits raise
  it. This is measured in F1.
- Clients see `E_OVERLOADED` in one new case: the X budget is full at the
  applying shard.

## 4. Cap semantics

| Question | Answer |
|---|---|
| Destination intake signal | Local: the shard's own `active_bytes` against H, per item at apply. Senders see only credits stop returning. |
| Client budget vs internal cap | Clients are admitted under B; follow-ups under H ≥ B + R, provisionally max(2B, B + R). Above B, follow-ups still settle while clients are refused. |
| Hysteresis / reopen | None on the WAL gate: there is no state and no message, so flapping costs one compare. Links resume at L/2 to save syscalls. |
| Per shard or per node | Per shard: H, W, X and L. Node bounds are sums. |
| Bytes, records or both | The WAL gate is in bytes (worst case per item). W, X and L count items (72 B each, plus queue nodes). |
| Worst-case overshoot | WAL: none above H, except one oversized item on an empty buffer. Staging: (N−1)·W per shard. Live exchanges: X + E per origin, plus resume and re-route roots (6.2). |
| Drainable above the cap | Every message class, always. Only the apply of follow-ups is gated. |
| How the sender backlog is bounded | By conservation, total follow-ups ≤ Σ live exchanges ≤ N·(X + E) + resume roots (6.2). |
| Local follow-ups | The same queue, gate and exchange accounting. |
| Cross-node | D.1: link pausing and gates. D.2: end frames (3.6). |
| Shutdown, fencing, migration, recovery | Sections 3.7 and 8. |

## 5. Defaults (hypotheses, not frozen)

| Parameter | Provisional value | Basis | Set by |
|---|---|---|---|
| H | max(2B, B + R); hard H ≥ B + R | Room for a settle wave above B; A15 reached 50.9 MB | R640 preregistration |
| W | 64 | At least one loop's worth (`batch_in` = 32), with headroom | P1 throughput |
| X | 1024 per shard | The same order as `kEmitBacklog`; Account allows 64 open outbox entries per account | P1, F11 |
| L | 1024 | The same order as `kEmitBacklog` | F8 |

- **Configuration:** config fields, plus env overrides as in S4.
- **Rollback:** `intake_gate = off`, W = 0, `exchange_budget = off`, and
  `owner_gate = off` together restore today's behaviour. X has its own
  off switch because `live_o < 0` would refuse everything; X = 0 is
  rejected at start.
- **What stays enforced under rollback:** the contract checks (3.3). The
  only existing emitter already satisfies them.

## 6. Proofs

Notation:
- N, the shards on the node;
- G = `kEmitBacklog`;
- E = the largest `max_emits` of a client-run op (1 today);
- R = `need_f`;
- `live_o`, the started-but-not-ended exchanges of origin o.

### 6.1 B1: destination WAL bound (unchanged)

Every append on d comes from one of:
- (a) client work, admitted while `active + need ≤ B` or `active = 0`;
- (b) follow-up applies, under 3.1;
- (c) the other non-refusable records listed in S1.1 section 3 (session
  expiry, seal and unseal, migration installs bounded by
  `move_window_bytes`).

Each admission checks and appends with no yield in between. Flushes
start only in `wal_->poll`. A follow-up apply appends at most its own
record and one peer record (`shard.cpp:621`, `634`). Since B ≤ H,

    active ≤ max(H, R, client worst case) + (c)

whatever the senders do. WAL memory is at most twice that (active plus
flushing), plus the replication sender's retained chunks, and never
above `max_active_bytes` (PR #84). F1 sizes H so that ungated intake
would pass it several times over.

### 6.2 B2: sender bound (structural, new in revision 2)

**Lemma (conservation).** Under 3.3, applying a follow-up removes one
follow-up and emits at most one. So within an exchange, the number of
follow-ups never grows, and it is at most one. Proof: a root creates
the exchange with one follow-up. Each apply of that follow-up emits 0
(the exchange ends) or 1 (one again). A drop ends the exchange. A hold
or re-route moves the one follow-up, and a re-route ends this exchange
and starts another.

**Corollary (intra-node).** Every follow-up of an exchange whose two
objects are on this node is counted by exactly one live exchange. So, for
any shard s, counting only such follow-ups,

    emits_in_flight(s) + staged(s) + in transit to or from s
        ≤ Σ_o live_o
        ≤ N·(X + E) + Σ_o (resume roots_o + re-route roots_o)

Follow-ups of exchanges that cross nodes (link-origin items and their
replies in `emit_node_`) are in no local `live_o` until D.2; they are
bounded by L, by `kEmitBacklog` and by link pausing, as in revision 1
(3.6).

**Resume roots.** Each resume pass emits at most one root per outbox or
inbox entry (object state). But passes are not deduplicated: every link
flap and map change re-runs `resume_all` (`shard.cpp:1730`, `2990`), so
without pacing the bound is object state times outstanding passes. With
Q1's pacing, a pass waits while `live_o ≥ X`, so resume adds at most X
per shard, and the whole intra-node bound becomes structural.

**Re-route roots.** These are at most one per follow-up held or in
flight at a map or slot-state change.

**Revision 1's open case is covered.** That case was derived follow-ups
at x toward a closed d. They are follow-ups of live exchanges, so the
same bound holds. No rate argument is needed.
- The case reviewer M2 described: d admits clients faster than x's
  replies to d drain. d stops admitting once `live_d` reaches X.
- Every reply that x holds toward d belongs to an exchange started by d
  or by x, so it is counted in `live_d` or `live_x`.

**Per-class bounds that hold at the same time:**
- staged items per pair, at most W (3.2);
- link-origin items, at most L (3.6);
- the sender's own unsent queue: its client admission still refuses
  above G, as today.

### 6.3 B3: response and control progress (unchanged)

Rings drain fully, so nothing is ever left behind an EMIT. Requests are
answered by admission: refused, or served as READs. The X refusal is an
ordinary answer. Node links carry only follow-ups.

### 6.4 B4: no cyclic deadlock

Every wait that D introduces:
- (i) a staged follow-up at d waits for d's `active` to fall to H − R,
  or to 0;
- (ii) a follow-up at s waits for a W credit from d, that is, for an
  item from s to leave d's staging. This depends only on (i) at d;
- (iii) a link frame waits for the reader to resume, that is, for its
  held link-origin items to leave. This also depends only on (i);
- (iv) **client admission** waits for `live_o < X`. It does not wait:
  it is refused. Exchanges end only through applies or drops, which
  depend only on (i)-(iii).

No follow-up in flight waits on (iv), or on anything that (iv) holds.
(i) depends only on the local flush completing, which depends only on
the disk (B4 in revision 1, confirmed by its review). So every wait
chain ends at a flush, and the wait-for graph is acyclic.

What is excluded on purpose, each a deadlock (section 7):
- an intake condition on the shard's own backlog (7.1);
- holding a credit until a reply is sent (7.2);
- a root waiting in a queue for the budget (7.3).

### 6.5 B5: eventual progress

When d's disk completes a flush:
1. `active` falls;
2. the staged head passes the gate and runs;
3. the W credit is published, and the waiting sender is woken by eventfd;
4. link reads resume below L/2.

Every exchange's next hop therefore runs in finite time:
- staging is FIFO, and every item fits under H or runs alone;
- no hop waits for X;
- held items are released by the slot-state change that holds them
  (existing).

Every exchange therefore ends, or continues, in finite time. When client
load stops, every exchange ends: transfers end in at most `forget`, or
loop on `retry` only while the target's inbox is full. That is
application behaviour, unchanged.

### 6.6 B6: no discard, duplicate or client refusal

- **No discard.** The dispositions in 3.7 are the existing ones, plus the
  `withdraw` drop. A fenced primary's follow-ups are re-sent by the new
  primary's resume, as today.
- **No duplicate.** No new resend.
- **No refusal of a follow-up.** Follow-ups have no client. Clients see
  `E_OVERLOADED` from the existing gates and from the X budget. Both
  come before apply, so nothing is applied twice, and a forwarded
  refusal is replay-safe.
- **Order.** Per-source FIFO is kept, and no follow-up waits for X. A
  reply can never overtake an earlier root on the same pair of objects
  (7.3).

### 6.7 B7: recovery (unchanged)

Staging, credits, exchange counters and paused reads exist only in
memory. A crash loses them as it loses today's ring contents. Recovery
is replay, then `resume_all`, as today. Every counter starts at 0, and
the resume roots start exchanges.

## 7. Rejected alternatives

### 7.1 Closing intake on the shard's own backlog (revision 1, unchanged)

A and B close intake when their own backlog exceeds a threshold. A's
backlog is mostly to B, and B's mostly to A. Neither drains, even with
healthy disks.

### 7.2 Holding the intake credit until the reply is sent

This would make each W credit cover a whole round trip.
- x holds W items from d, each waiting for its reply to d to be sent,
  which needs x→d credits.
- d holds W items from x, each waiting likewise.
- Both windows are full, and nothing moves.

### 7.3 Per-pair exchange credits taken by roots in the sender's queue

This would bound live exchanges per pair by letting a root wait at the
head of `emit_remote_[p]` for a credit.
- **The deadlock.** A root waits for one of W exchanges to end. One of
  those exchanges has its reply queued behind the root, so it cannot
  end.
- **Why the obvious fix fails.** Letting replies overtake waiting roots
  breaks per-source order. On a duplicate exchange that loses money:
  1. A's resume hook queues a duplicate `credit(S)`, which waits.
  2. Meanwhile A applies `settle(S)` and emits `forget(S)`, which
     overtakes the waiting credit.
  3. B forgets S, then receives the duplicate credit, and credits a
     second time.

  So roots may never wait inside a queue. X is enforced at admission,
  where a refusal costs nothing (3.4).

### 7.4 Forwarding and fan-out follow-ups

These are excluded by the contract.
- A forwarding mode, where a follow-up emits to a third object, breaks
  the two-object chain.
- Fan-out, where a follow-up emits more than once, breaks conservation.

Either would need its own bound: for example, the forwarding shard
starting a new root that is charged to its own X budget, with
admission-time refusal impossible because there is no client. That
needs separate design and review, as cody's review states.

### 7.5 R1, R2, R3 (revision 1)

- **R1** ("protocol shape + gauge") is rejected by the review.
- **R2** (backlog gate with an escape) is unproven.
- **R3** (shed and resume) changes the intra-node "never dropped"
  guarantee.

The contract plus X replaces all three.

## 8. Lifecycle interactions

- **Synchronous I/O fallback (predates D).** Blocking `pwritev2` stops
  all draining on a stalled disk. Noted, not changed.
- **Startup.** Emits are paused until recovery finishes, then
  `resume_all` starts exchanges, which count in `live_o` (Q1: paced).
- **Shutdown.** `Engine::stop` waits for sent = received. Staged and
  unsent items are left to resume, and the counters end with the
  process.
- **Fencing.** `withdraw` drops the staged items from other shards and
  returns their W credits and exchange endings (3.7).
- **Slot moves.** Held items keep their exchange live while held. A
  re-route ends the exchange and starts a new root. Stale items end
  theirs, and `map_changed`'s resume starts new ones. V2 checks that no
  move step waits on follow-ups.
- **Standby and promotion.** A standby runs no follow-ups. Promotion's
  `resume_all` starts exchanges.

## 9. Test plan

General rules:
- 8 shards where the harness allows;
- the WAL stalled by `Engine::debug_hold_wal`, except where V4 needs a
  delayed-completion hook;
- gauges per class: staged per pair, link-origin, `live_o`,
  `started_o`, ended, own queue;
- B and H sized so that ungated intake would exceed H several times
  over.

| Test | Setup | Pass criterion |
|---|---|---|
| F1 | Stall d (H = B + 4R); 7 senders transfer to d's accounts, including forwarded transfers on sender-owned accounts | d's `active` ≤ max(H, R) throughout; staging ≤ 7W; every shard's queues ≤ the 6.2 bound; forwarded mutations refused past G or X; S2 pressure recorded |
| F2 | Stall d; fill the s→d credits; then (a) a forwarded READ and MUTATE, (b) MULTI, (c) admin HEALTH and STATUS | All answered within a bound while intake stays closed |
| F3 | Stall every shard; mutual transfers on every pair; release | No deadlock; all settle; `active` ≤ max(H, R) throughout |
| F4 | Full staging; release. F4b: push at least W + 1 and X + 1 items through hold, stale, re-route and withdraw | Exactly once, in per-source order (a test type logs (source, seq)); conserved; `wait_emits` reaches 0. F4b: `sent − taken` = 0 and `live_o` = 0 at the end |
| F5 | F1 and F3; sample every queue | Per pair ≤ W; per shard, staged ≤ (N−1)W and link-origin ≤ L; Σ items ≤ Σ `live_o` ≤ N(X + E) + resume roots |
| F6 | F1 state, then SIGKILL | After restart: conserved, settled, no double credit |
| F7 | Local and cross-shard exchanges into a stalled shard | Both gated; both counted |
| F8 | Two engines over node links (D.1); stall the receiver | `active` ≤ H; reads pause at L; the sender's gate refuses; settles. The D.2 scope is stated |
| F10 (new) | Contract death tests (fork, as `test_lifecycle` L11): a follow-up emitting to a non-source target; a follow-up emitting twice; an op emitting past `max_emits`; an undeclared op emitting | SIGABRT with the contract message, for each |
| F11 (new) | The rate-mismatch case (reviewer M2): d's clients transfer to x's accounts as fast as possible while x's replies to d are throttled (H = B + R, a slow flush hook on d) | x's queue toward d ≤ `live_d` + `live_x` ≤ 2(X + E); d's clients see `E_OVERLOADED` from X; everything settles |
| F9 | Mutations | N1 no WAL gate → F1. N2 stop draining at an EMIT → F2. N3 no credit returned on hold, stale or withdraw → F4b. N4 credits counted at drain → F5. N5 backlog gate → F3 deadlocks. N6 link reads not paused or not limited → F8. N7 `serve_emit` ignores staging → F4 order. N8 no owner gate → F1. **N9 no X → F11 queue > 2(X + E). N10 ending not counted on one path → F4b `live_o` ≠ 0. N11 contract check removed → F10. N12 roots wait in the queue for credits (7.3) → a duplicate-credit order test (F4c) or a deadlock** |

Implementation verifications:
- **V1:** nothing relies on EMIT-before-RESP order.
- **V2:** no move or checkpoint wait holds what a flush needs.
- **V3:** stalled link reads never cause a disconnect.
- **V4:** no spin, with the right wake-ups.
- **V5 (new):** every emit site is classified as root, reply or re-route
  (`OpCtx::emit` under client apply, follow-up apply, or `resuming_`;
  `emit_result`; `route_emit` from `run_emit` and `release_held`). The
  `Msg` origin bit is set on every `send_emit`.

Performance:
- **P1:** transfer benchmarks at W in {16, 64, 256} and X in {256, 1024,
  4096}, plus no window. Reports the overhead of the credit and budget
  checks.

Every slice rule applies:
- full serial Release, ASan+UBSan and TSan runs, with zero sanitizer
  reports;
- every run reported;
- an independent review.

## 10. Questions for the review

1. **Q1, resume pacing.** Resume roots (recovery, link-up, map change)
   are bounded today only by object state. Should the implementation
   page `resume_all` and `resume_slot`, scanning a slice of objects per
   loop iteration and pausing the scan while `live_o ≥ X`?
   - It would make B2 structural for resume too.
   - It changes when "recovered and serving" emits finish, not when
     serving starts.
   - Recommendation: yes, in D.1.
2. **Q2, split.** Build D.1 first (intra-node: H, W, X, the contract,
   the owner gate, link pausing), then D.2 (cross-node end frames, a
   wire change) as its own reviewed sub-slice before S6? Recommendation:
   yes.
3. **Q3, defaults.** H = max(2B, B + R) with H ≥ B + R hard; W = 64;
   X = 1024; L = 1024. All are hypotheses.
4. **Q4, registration check.** Should `add_op` reject `max_emits > 1` on
   any op that may run as a follow-up? This needs a declared
   `followup = true` flag on the op, because today that is known only
   inside `validate`. Recommendation: add the flag. Account's follow-up
   ops set it, and `run_emit` refuses (contract violation) to run an op
   without it.

5. **Q5, approve X.** The exchange budget X is a new client-visible
   gate, beyond the approved D: emitting client ops (today `transfer`)
   see `E_OVERLOADED` while the applying shard has X or more of its own
   exchanges unfinished. It is what makes B2 structural (6.2). Approve
   it, or keep D without X and accept that B2 stays bounded only by the
   sender gate and rates, as revision 1 stated?
   Recommendation: approve X.

## 11. Not in this slice

Implementation of any part of D; S5; S6; R640; CP-1A; comparators; B2
begin-snapshot work.

## 12. Changes from revision 1

| Revision 1 | Revision 2 |
|---|---|
| R1: derived backlog bounded by protocol shape plus a gauge | Rejected by the review. Replaced by the reply-only contract (3.3) and the exchange budget X (3.4). B2 is structural (6.2) |
| E unbounded (Q2) | Enforced `OpDescriptor::max_emits`; Account declares its actual maxima |
| H = 2B | H = max(2B, B + R); H ≥ B + R is hard |
| Cross-node bounded by pausing only | D.1 pausing; D.2 end frames specified (3.6) |
| The u32 WAL finding (Q4) | Fixed in PR #84 |
| Rejected: backlog gate | Also rejected: deferred credits (7.2) and queued roots (7.3), with the money-duplication order hazard |

## 13. Independent reviews

### 13.1 Revision 2

A read-only reviewer checked revision 2 against main at 3bcde70 and
cody's review.
- **Confirmed:** the flag bits; the Account emit table (apart from the
  corrections below); how `cur_source_` is set (non-null exactly for
  follow-ups, including the fail op); the line references; the
  conservation lemma intra-node; no wait cycle in 6.4 (X is
  refusal-only; W, L and pausing bottom out in the WAL flush; the settle
  retry loop is application behaviour bounded per hop by H); every point
  of cody's review is covered, with no implementation claimed.
- **Found and corrected here:**
  - 1 HIGH: the 3.7 table missed drop paths (the withdrawn inline drop,
    fail op without `fail_op`, `route_emit` and link drops), any of which
    would leak `live_o`. Rows added, "ends" defined, an idle invariant
    added;
  - 6 MEDIUM: the B2 claim overstated (now intra-node and conditional on
    Q1; resume passes are not deduplicated); X is a new gate needing its
    own approval (Q5); X gated non-emitting ops (now only
    `max_emits > 0`); the re-route description (owners are static within
    a node); D.2 link-loss double counting (generation tags); table
    facts (`transfer` on a null `local()`, settle with no entry);
  - 4 LOW: resume hooks have no op; emit outside apply; X = 0 ambiguous;
    the inline credit-return path; the bit 4 overload.

### 13.2 Revision 1 (summary)

A read-only reviewer checked revision 1 against the code at 83280f9 and
the work order.
- **Confirmed:** B1, B3, the B4 drain argument, the backlog-gate
  deadlock, per-source order and the node-link facts.
- **Found:**
  - 3 HIGH: no owner gate; staging bounds stated wrongly; N3 did not
    discriminate;
  - 6 MEDIUM: spin and wake-ups; the derived-backlog rate argument; the
    withdraw path; the u32 scope; `run_emits` order mechanics; A15's
    sizing;
  - several LOW.
- All were corrected in revision 1 before the review. Revision 2 keeps
  those corrections (3.1, 3.2, 3.5, 3.6, 3.7, 9).
