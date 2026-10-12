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

### Limits

- At most 64 outbound transfers in flight per account (`transfer`
  rejects with `kAppBusy` past that). Inbox entries are bounded by the
  senders' outboxes.
- An account with entries cannot be deleted.
- Four records and three messages per transfer when A and B are on
  different shards (transfer, credit, settle, forget records; credit,
  settle, forget messages). When they share a shard the follow-ups skip
  the rings and the durability wait but still take their own records;
  doing both sides in one step is a later optimization.
- Duplicate messages arise only from `resume` after a restart; in normal
  running each follow-up is sent once.

## Measurement plan

- Correctness: balance sums conserved after transfers settle, in memory
  and durable; restart mid-flight; SIGKILL mid-flight (`test_crash`
  extended); a shard-count change with transfers in flight.
- Throughput and latency: transfers between random accounts in
  `snc_bench`, against the baseline (lock both stripes in address order,
  one log record for both objects), PostgreSQL (one transaction, two
  `UPDATE`s with a balance check) and Redis (one Lua script).
