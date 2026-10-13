# Safe client retries across a failover (design draft)

Status: proposed, not built. It follows automatic failover
(docs/REPLICATION.md), which promotes a standby but leaves clients pointed
at the dead primary, and leaves them unsure what happened to the requests
they had in flight.

## The problem

A client with requests in flight when its primary dies cannot tell which
of them were applied. Each was in one of three states:

1. never logged: lost with the primary;
2. logged on the standby but not acknowledged to the client;
3. acknowledged.

Resending all of them to the new primary repeats every request in state 2.
A deposit is applied twice. Reads are harmless to repeat; creates, deposits,
transfers and other mutations are not.

## Proposal: per-client sequences, remembered in the WAL

The approach is the one Kafka's idempotent producer uses, fitted to SNC's
shards.

- **Sessions.** A client that wants safe retries picks a random 64-bit
  `client_id`. It numbers its mutating requests to each owner shard
  1, 2, 3 and so on (`ExecOptions::client_id`, `seq`), in the order it
  sends them on that shard's connection. Reads carry no sequence.
- **The owner remembers the last sequence.** Each shard keeps a session
  per client: `{client_id, last_seq, last_status, last_version}`. It is
  stored as a small system object of a `Session` type, found by a
  shard-local index on `client_id`. Storing it this way means sessions get
  the same treatment as any object, with no new machinery: WAL, checkpoint,
  replication to the standby (with its live index), recovery and
  promotion.
- **Logged with the op.** A sequenced request updates its session in the
  same atomic WAL group as the object it changes, using the same-shard
  group transfers already use (`OpCtx::local`, `kRecGroupNext`). A standby
  therefore holds a write exactly when it holds the session's record of it.
  Requests that fail validation (`E_APP`, `E_VERSION_CONFLICT`) also
  advance the session: they are logged as a session-only record. That way
  every sequence number up to `last_seq` has been decided.
- **Duplicates.** A request with `seq <= last_seq` is not run again:
  - When `seq == last_seq`, the owner answers with the recorded status and
    version, marked `replayed`. It waits for that record to be durable, as
    for any write.
  - When `seq < last_seq`, it answers `E_REPLAYED_OLD`. The client already
    moved past that request, so it has its answer or will get it.
  - A gap (`seq > last_seq + 1`) means requests between them were lost. They
    can only have been lost unlogged, and only in a failover, because one
    connection delivers in order. The owner accepts the request and records
    the gap.
- **Ordering.** A sequence is per (client, owner shard), so a client must
  route sequenced requests to the owner. The owner-aware client already
  does: forwarding through another shard could reorder them. A misrouted
  sequenced request is refused, not forwarded.

## Client side

`net::Client` takes a list of endpoints (primary, standby). When a
connection fails, or a node answers `E_STANDBY`:

1. It tries the other endpoint until one serves, following the witness's
   promotion.
2. It resends every unanswered sequenced request in its original order,
   with its original sequence number.
3. It resends unanswered reads as they are.

The application sees each request answered once, with `replayed` set when
the answer came from a session record.

## Costs and limits

- **WAL bytes.** A sequenced write logs one more small record, the session
  image (about 64 bytes next to a 250-byte account). Unsequenced requests
  pay nothing.
- **Memory.** There is one session object per (client, shard) the client
  wrote to. Sessions idle for longer than a configured time
  (`session_ttl_s`, default one hour) are deleted at checkpoint. A client
  returning after that has its old sequences forgotten, and must start a new
  `client_id`.
- **Replayed results are status and version only.** An op whose result
  carries data (a transfer's balance) returns it on the first answer only;
  a replay returns the status. Storing the result would cost its bytes in
  every session record.
- **A shard-count change at restart** moves sessions with their slots,
  like any object. The index finds them on their new owner.

## Tests it would need

- **Retries after a SIGKILL:** with 1000 sequenced deposits in flight, the
  primary is killed and the standby promoted. The client resends all
  unanswered deposits, and each account ends at the start balance plus
  exactly the number of deposits sent.
- **Kill mid-retry:** the same, with the new primary killed again during the
  retries (a second witness configuration).
- **Session lifecycle:** sessions survive a checkpoint, recovery, seeding
  and promotion, and expire after their TTL.
- **Benchmark:** throughput with sequenced versus unsequenced deposits.
