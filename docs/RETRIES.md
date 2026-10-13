# Safe client retries across a failover

Status: built (engine sessions, wire flag, failover-aware `net::Client`,
session expiry); see "As built". It follows automatic failover
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
  Requests that fail (`E_APP`, `E_VERSION_CONFLICT`, `E_NOT_FOUND`) change
  nothing and are not logged; a retry runs them again, which can apply at
  most once. Every sequence number up to `last_seq` was decided: applied,
  or failed with no effect.
- **Duplicates.** A request with `seq <= last_seq` is not run again:
  - When `seq == last_seq`, the owner answers with the recorded status and
    version, marked `replayed`. It waits for that record to be durable, as
    for any write.
  - When `seq < last_seq`, it answers `E_REPLAYED_OLD`: decided earlier
    (applied, or failed with no effect), with the outcome not kept.
  - A gap (`seq > last_seq + 1`) is normal: failed requests leave gaps, and
    so do requests lost unlogged in a failover. The owner runs the request.
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

## As built

- `ExecOptions::client_id` and `seq`; `ExecResult::replayed`; statuses
  `E_REPLAYED_OLD` and `E_NOT_OWNER`. Sessions are `System.Session`
  objects (type 1) with a shard-local index on `client_id`
  (`src/core/session.{h,cpp}`); MUTATE ops and creates are sequenced,
  reads are not (a retried read runs again).
- Wire: `kReqSequenced` (the body starts with `{client_id, seq}`) and
  `kRespReplayed`.
- `net::Client::add_endpoint` and `enable_session`: on a failed
  connection, or an `E_STANDBY` answer, it tries the next endpoint until
  one answers as primary (a probe that a standby answers `E_STANDBY`), then
  resends every unanswered request in order. `net_bench --session ID
  --endpoint H:P --verify`.
- Session expiry: `Engine::Config::session_ttl_s` (default 3600). A
  primary deletes sessions untouched that long just before each checkpoint;
  the deletes replicate like any other. Tested with a 1 s TTL.
- Request timeout: `enable_session(id, failover_ms, request_timeout_ms)`
  (default 5000 ms; `net_bench --request-timeout-ms`). A node that leaves a
  request unanswered that long counts as failed, like a broken connection.
  This covers a primary that is alive but cut off: fenced, it acknowledges
  nothing and keeps its connections open. The same timeout bounds each
  blocking step of reconnecting, so a hung node cannot stall the client's
  search for the new primary. A false timeout (a slow but healthy primary)
  costs a reconnect and a resend, never a double apply.

## Results

`bench/run_retries.py`, `bench/results/retries.csv`. The run_failover.py
layout (primary on cores 0-1 with a sync standby on core 2 and
`snc_witness`); one `net_bench` thread on core 3, deposits only, 100K
accounts, 256 in flight, 8 s, `--verify` reading every account at the end.
3 runs each.

| Case | Deposits/s (median) | Checked |
| --- | --- | --- |
| No session | 230K | every account exact |
| Session (sequenced) | 220K (0.93× of the run before it) | every account exact |
| Session, primary SIGKILLed 3 s in | 268K | 1 failover; 298-396 requests answered from sessions or as already applied; every account exact |

- **No deposit was lost or applied twice** across the three failovers:
  about 2.1M deposits each, every account at its start plus exactly what
  the client sent it.
- **Sequencing costs about 7%** here: one 64-byte session record logged
  with each deposit, and a session lookup.
- The failover runs are faster because after the kill the promoted node
  runs without a standby. The witness promoted it about 0.52 s after the
  kill; the client was back as soon as the probe saw a primary.
- `tests/test_replication.cpp`: duplicates, older sequences, a misrouted
  request and a repeated create on one engine, kept across a checkpoint and
  restart; and 30K pipelined sequenced deposits with the primary
  SIGKILLed, all resent to the promoted standby, every account exact
  (with a different client ID for the resend, the same test finds accounts
  credited twice).

## Costs and limits

- **WAL bytes.** A sequenced write logs one more small record, the session
  image (about 64 bytes next to a 250-byte account). Unsequenced requests
  pay nothing.
- **Memory.** There is one session object per (client, shard) the client
  wrote to. Sessions idle for longer than a configured time
  (`session_ttl_s`, default one hour) are deleted before a checkpoint. A client
  returning after that has its old sequences forgotten, and must start a new
  `client_id`.
- **Replayed results are status and version only.** An op whose result
  carries data (a transfer's balance) returns it on the first answer only;
  a replay returns the status. Storing the result would cost its bytes in
  every session record.
- **A shard-count change at restart** moves sessions with their slots,
  like any object. The index finds them on their new owner.

## Tests still worth adding

- Kill mid-retry: the new primary killed again during the resends (needs a
  second witness configuration).
- Sessions across seeding: a standby seeded from a checkpoint holding
  sessions, then promoted, still replays.
