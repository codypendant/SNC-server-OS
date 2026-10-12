# Two-object ops: transfers between accounts

v0.1 ops change one object. The first thing a real application asks for
that SNC cannot do is move value between two objects, such as a transfer
between accounts, which PostgreSQL does in one transaction. The handoff
defers multi-object and distributed transactions, so this design adds the
smallest mechanism that makes a transfer correct across shards and
crashes, without locks, two-phase commit, or a new log format.

## Semantics

`execute(A, Account.transfer, {to: B, amount})`:

- **Validated and committed at the source.** The op checks A's balance
  and debits A. The client's completion fires when that debit is durable
  (or applied, with `Durability::APPLIED`). From then on the transfer is
  committed: the credit to B will happen exactly once, even across
  crashes, restarts and shard-count changes.
- **Credited asynchronously.** B is credited by a follow-up op on B's
  owner. Until it lands, the amount is *in flight*: a reader of B can
  see the old balance. Money is never created or lost; the sum of
  balances plus in-flight amounts is constant.
- **Refunded if the credit cannot apply.** If B does not exist, is not
  an account, or would overflow, A is refunded.
- **Not serializable across objects.** There is no isolation between a
  transfer and other ops on A or B, beyond each op being atomic on its
  object. This is the saga/outbox model, not a transaction.

## Mechanism

### Engine: durable follow-up ops (`OpCtx::emit`)

An op's `apply` may call `ctx.emit(target, op, args, fail_op)` with up to
16 bytes of args. The engine runs `op` on `target` as an ordinary exec on
the target's owner; inside it, `ctx.source()` names the object that
emitted it (null for a client call, which is how follow-up ops refuse
clients). Results go nowhere. If the target is missing, the wrong type,
or lacks the op, the engine runs `fail_op` on the source object instead,
with the same args and `source()` set to the target.

When it is sent:

- **Remote target:** once the emitting op's WAL record is durable (at
  once in memory mode). If B were credited before A's debit was durable,
  a crash could keep the credit and lose the debit.
- **Local target:** at once, from the poll loop (never inline, so
  handlers cannot recurse). The shard's WAL is replayed as a prefix, so
  a follow-up record can never survive a crash that loses the record
  that caused it.

Remote follow-ups travel as one-way `EMIT` messages that fit the 64-byte
ring slot (source in the header fields a response would use, args
inline), with no response and no buffer to return. They queue per
destination shard and are never dropped for a full ring. Follow-ups are
not logged: whatever must survive a crash lives in the objects (below).
A type can declare a `resume` hook that the engine runs on every object
once recovery finishes; it re-emits whatever the object says is still
pending.

Backpressure: while a shard has more than 1024 follow-ups queued or
waiting for durability, client `execute` on it returns `E_OVERLOADED`
without running. `Engine::wait_emits()` waits until every follow-up has
run (sent equals received, nothing queued, two quiet rounds), and
`Engine::stop()` stops sending new follow-ups and waits for those
already in the rings, leaving the rest in the objects for `resume`.

### Account: outbox and inbox entries in the payload

The account payload keeps its 200-byte `AccountV1` prefix, followed by
two short lists the transfer ops maintain:

- **outbox** entries `{seq, to, amount}`: debits whose credit is not yet
  confirmed. `seq` is A's version at the debit, unique per account.
- **inbox** entries `{from, seq, ok}`: credits seen here whose sender
  has not yet confirmed it stopped resending, and whether the credit
  applied (`ok = 0` records a refused credit, so a resend is refused
  again rather than applied).

Because they are part of the object, the entries are in every WAL image
and checkpoint, move with the object on a shard-count change, and need
no recovery code of their own.

### The exchange

| Step | Runs on | Does | Emits |
| --- | --- | --- | --- |
| `transfer` | A | balance ≥ amount: debit, add outbox entry | `credit` to B (fail: `bounced` to A) |
| `credit` | B | if inbox has (A, seq): nothing new; else credit (refuse on overflow), add inbox entry; inbox full: no change | `settle` to A: ok, refused, or retry |
| `settle` | A | only from the entry's `to`. ok: remove entry; refused: refund, remove entry; retry: resend `credit` | `forget` to B (not on retry) |
| `bounced` | A | B does not exist or is not an account: refund, remove entry | none |
| `forget` | B | remove inbox entry (A, seq) | none |
| `query` | A | if outbox lacks seq | `forget` to B |

Each step is idempotent, so any message can be repeated. Each emit
waits for its own record to be durable, so no step's effect is visible
to the next until it would survive a crash.

`resume` after recovery: every outbox entry re-emits `credit`; every
inbox entry emits `query` to its sender. Together these finish any
exchange a crash interrupted:

- crash before B's credit was durable: A's outbox re-sends it;
- crash after the credit but before A settled: B sees the duplicate
  `credit` in its inbox and re-sends `settle`;
- crash after A settled but before B forgot: B's `query` finds no
  outbox entry, so A sends `forget`.

An inbox entry is only removed after A's outbox entry is durably gone,
so B can never see a resent credit it has forgotten.

`forget` is not logged (`OpDescriptor::unlogged`): it changes neither
the balance nor the version, and its effect reaches the WAL with B's
next logged change or checkpoint. If a crash loses it, the inbox entry
comes back and B's resume sends `query`, which ends in `forget` again.

### Limits

- At most 64 outbound transfers in flight per account (`transfer`
  rejects with `kAppBusy` past that). Inbox entries are bounded by the
  senders' outboxes.
- An account with entries cannot be deleted.
- Three WAL records and three messages per transfer when A and B are on
  different shards (transfer, credit and settle records; credit, settle
  and forget messages). When they share a shard the transfer is
  one op with one atomic group of two records (below).
- Duplicate messages arise only from `resume` after a restart; in normal
  running each follow-up is sent once.

## Same-shard transfers: one op, one atomic WAL group

When the shard that owns A also owns B, `transfer` changes both in one
op and emits nothing. `OpCtx::local(id, type)` hands an op a second
object its shard owns; when apply uses it, the engine bumps that object's
version and logs its image just before the op's own record, flagged
`kRecGroupNext`. Recovery installs the records of a group only once it
reads the record that closes it, so a group cut off by a torn tail is
dropped whole: never a credit without its debit. The client is answered
when the closing record is durable, and B's `last_lsn` points at it, so
reads of B wait for the whole group too. Groups never span WAL segments
because one op's records are flushed together.

Follow-up targets arriving in a ring batch are prefetched like client
requests, and `local()` checks the slot map before probing the map, so a
destination on another shard costs no cache miss here.

The op may change the second object's payload in place but not its
length or index key (checked in debug builds), so it needs no
reservation or index work. A destination that would overflow is
rejected up front (`kAppOverflow`) instead of refunded.

## Results

4-vCPU VM, 1M accounts, 1-cent transfers, mean of 3 runs. Raw numbers:
`bench/results/matrix_transfer.csv`, `net_matrix_transfer.csv`,
`conventional_transfer.csv`. Random pairs: with 4 shards a quarter of
pairs share a shard (in process); with 2 server shards, half (TCP).

| Setup | SNC | Locked baseline | Redis | PostgreSQL |
| --- | --- | --- | --- | --- |
| In process, memory, 100% transfers | 3.64M/s | 6.27M/s | | |
| In process, memory, 20% transfers + 40% reads + 40% deposits | 8.44M/s | 9.73M/s | | |
| In process, durable | 527K/s | 569K/s (4 logs) | | |
| TCP, 2 server cores, memory | 1.15M/s (p99 0.16 ms) | 1.65M/s | 129K/s | 33K/s (pipelined) |
| TCP, 2 server cores, durable | 427K/s (p99 2.6 ms) | 504K/s (2 logs) | 59K/s | 28K/s (pipelined), 15K/s (1 per round trip) |

Placement decides it. `snc_bench --xfer-local P` sends P% of transfers
to an account on the source's shard and the rest to other shards (in
process, 100% transfers; the baseline does not care where accounts live):

| Same-shard pairs | 0% | 50% | 90% | 100% | Baseline |
| --- | --- | --- | --- | --- | --- |
| Memory | 2.76M/s | 4.28M/s | 7.03M/s | 9.50M/s | 6.27M/s |
| Durable | 516K/s | 561K/s | 579K/s | 558K/s | 569K/s |

Correctness: `test_transfer` passes in release, ASan/UBSan and TSan
builds: exact balances after transfers settle in memory and durable,
across a clean stop with transfers in flight, a 4→3→4 shard-count change,
a refund for a missing destination, all-local transfers on one shard
(including a restart after the closing record of the last group is
corrupted, which must drop both sides), and conservation after 4 SIGKILL
rounds with up to a few hundred exchanges in flight at recovery.

What this shows:

- **Across shards, SNC loses to the locked baseline in memory**: 2.3x
  slower when no pair shares a shard. Durable, it is within 10%: both
  are bound by writing the log. The baseline does every transfer
  as one step on one thread: lock two stripes, change both rows, append
  one log record per side. A cross-shard SNC transfer is four ops on two
  cores, three WAL records (full object images) and three ring messages. With uniform keys the
  baseline's stripe locks almost never contend (0.13% of ops), so its
  locks cost nothing and SNC's extra steps are pure overhead.
- **Within a shard, SNC wins in memory**: 9.50M against 6.27M (1.5x).
  Durable it ties (both write two records per transfer and are bound by
  the disk). The break-even in memory is around 75% same-shard pairs.
- **So SNC's answer to multi-object work is placement**, as the handoff
  expects: objects that are changed together belong on one shard.
  `create_near(near, type, args)` mints an ID in the same slot as `near`
  (by skipping IDs until one hashes there, about 4096 per create), so
  the two share an owner under every slot map, including after a
  shard-count change. A customer's accounts created near each other
  transfer among themselves in one op; `test_transfer` checks this
  before and after a 4→3 shard change.
- **Against real databases SNC stays far ahead** even with random
  pairs: 9x Redis and 35x PostgreSQL in memory, 7x Redis and 15x
  PostgreSQL durable, on the same two server cores. The baseline is not
  a database: it has no recovery for its logs.
- **The semantics differ across shards.** The baseline (and PostgreSQL,
  Redis) make both sides visible at once. A cross-shard SNC credit lands
  later; a reader of B can briefly see the old balance. Same-shard
  transfers are atomic.

What would cut the cross-shard cost further:

1. **Placement** with `create_near` (above), where the application knows
   which objects belong together.
2. **Batch follow-ups per destination.** Each follow-up is its own ring
   message and exec; several credits to one shard could share one.

## Measurement plan

- Correctness: balance sums conserved after transfers settle, in memory
  and durable; restart mid-flight; SIGKILL mid-flight (`test_crash`
  extended); a shard-count change with transfers in flight.
- Throughput and latency: transfers between random accounts in
  `snc_bench`, against the baseline (lock both stripes in address order,
  one log record for both objects), PostgreSQL (one transaction, two
  `UPDATE`s with a balance check) and Redis (one Lua script).
