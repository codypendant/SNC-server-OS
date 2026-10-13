# Replication

cody picked replication as the step after cross-core batching (2026-10-13).
The goal is that losing the machine a space runs on loses no acknowledged
write. Today a durable acknowledgement means the write is on the local disk
(spec C2), and nothing survives that disk.

This note was written before the code; "As built" at the end says where
v1 differs. It follows the handoff's
"Later" list, where replication comes before a distributed directory and a
cluster object space. So this round is one primary and one standby, not a
cluster.

## Model

- **Roles.** One **primary** serves clients. One **standby** keeps a full
  copy of the space and serves nothing. Both run the same `Engine`; the
  standby's is started in replica mode.
- **Same shape.** The standby has the same shard count and slot map. Shard
  *i* on the standby owns the same slots as shard *i* on the primary, so
  replication is stream to stream: primary WAL stream *i* to standby shard
  *i*. A standby with a different shard count is out of scope.
- **What travels is the WAL.** Records are full post-op object images
  (spec §12) with their LSNs, types and group flags, byte for byte as the
  primary wrote them. Nothing is re-executed on the standby, so it needs no
  handlers to stay correct, only `install`, the same function recovery
  uses.

## Data path

```
primary shard i                              standby shard i
  op -> WAL append (LSN n)                     
  group commit: write to local disk  ──┐
                and send the same      │  TCP, port base+i
                records to the standby └────────────────> read records
                                                          install() each image
                                                          append to own WAL, same LSN
                                                          flush; when durable:
  replicated_lsn = n  <─────────────────────────────────  ack(n)
  durable completion when
  local_lsn >= n and replicated_lsn >= n
```

1. **Send at flush start.** When a shard's WAL stream starts a flush, it
   hands the same record bytes to the replication link, so the local
   write and the network send overlap. An op's records are always flushed
   together, so an atomic group (`kRecGroupNext`) never splits across
   sends.
2. **Standby apply.** The standby shard checks each record (CRC, LSN
   sequence) and installs it under the version rule. Then it appends it to
   its own WAL stream with the **same LSN and flags**. A new
   `WalStream::append_at` refuses an LSN other than the next one. The two
   logs are identical record for record. So the standby can checkpoint,
   truncate and recover on its own, exactly like a primary.
3. **Ack.** When the standby's WAL makes LSN *n* durable, it sends
   `ack(n)` back on the same connection.
4. **Durability rule.** Each primary shard tracks `replicated_lsn`. In
   **sync** mode, the default whenever a standby is configured, the point
   where a durable completion fires becomes `min(local durable,
   replicated)`. The existing `durable(lsn)` check and the deferred-waiter
   queue change in one place. Reads wait on the same rule, as they already
   do for local durability. In **async** mode, completions use local
   durability only and the standby's lag is reported in `ShardStats`.

## Catch-up and retention

- **Handshake.** On connect, the standby shard sends `(space uuid, stream,
  next_lsn)`, the first LSN it does not have. The primary refuses a
  different space or shard count.
- **From disk, then live.** If `next_lsn` is behind what the primary has
  sent live, the primary reads the gap from its own WAL files
  (`scan_stream`, which already serves recovery). This happens on a helper
  thread, not the shard. Live flushes queue in memory meanwhile, and the
  link switches to live once the gap is sent. While catching up, sync mode
  holds completions; a timeout policy turns that into a degraded async
  state rather than a stall (see below).
- **Retention.** A checkpoint on the primary deletes WAL segments the
  checkpoint covers. With a standby attached, it also keeps any segment
  holding LSNs the standby has not acked, so catch-up can always read from
  disk. A standby that has been away so long that its gap is gone has to
  be reseeded.
- **Seeding.** v1 seeds a standby by copying a stopped primary's data
  directory, or by starting both on a new space. Shipping a checkpoint
  over the link is the follow-up that removes the copy step.

## Failure handling

- **Standby lost.** In sync mode the primary stops completing durable
  writes and reports why. After `replica_timeout_ms` (configurable,
  default 2 s) it continues in async mode, keeps WAL for catch-up, and
  says so in its stats and log. A deployment that prefers to stall forever
  sets the timeout to 0. Either way the choice is explicit, not silent.
- **Primary lost.** Promotion is manual in v1. Stop the standby and start
  it as a primary on its data directory; recovery runs as after a crash.
  Every write acknowledged in sync mode is in its WAL. Follow-ups in
  flight (transfers' credits and settles) resume from the objects' resume
  hooks, as after any crash (docs/TRANSFERS.md).
- **No fencing in v1.** Nothing stops an old primary that comes back from
  serving clients next to the promoted standby. Until a fencing token or
  lease exists, promotion assumes the old primary is down. The doc and
  `snc_server` say so.

## What v1 will deliver

1. Engine config: `replica_listen` (standby) and `replica_of`
   (primary), `replica_sync` (default true), `replica_timeout_ms`.
2. `snc_server --replica-of host:port` and `--standby --listen-port P`.
3. Tests:
   - Records on the standby match the primary's byte for byte.
   - A sync-acknowledged write survives SIGKILL of the primary followed by
     promotion of the standby, under transfers. Money is conserved and
     every acknowledged deposit is present.
   - The standby catches up after a disconnect.
   - Retention keeps unacked segments through a checkpoint.
4. Benchmarks: durable throughput and p99 with sync replication, with
   async, and without a standby, over loopback. Then the same against
   Redis with a replica and PostgreSQL with synchronous streaming
   replication.

## As built (v1)

- **Code.** `src/core/replication.{h,cpp}` (Sender and Receiver, framing,
  non-blocking sockets polled from each shard's loop), the hooks in
  `Shard` (`on_wal_flush`, `apply_replicated`, `poll_replication`, the
  `durable()` rule) and `WalStream::append_raw` plus an `on_flush` hook.
  `Engine::Config` gains `replica_host/port/sync/timeout_ms/retain_bytes`,
  `standby`, `standby_host` and `init_uuid`; `snc_server` gains
  `--replica-of`, `--async-replica`, `--replica-timeout-ms`, `--standby`,
  `--standby-host` and `--space-uuid`, and prints the space UUID.
- **Catch-up is from memory, not disk.** Each primary shard keeps the
  records it has flushed and the standby has not acked, up to
  `replica_retain_bytes` (1 GiB by default). On reconnect the standby's
  `next_lsn` picks the resume point, mid-flush if need be. A standby that
  needs records already dropped is reported `repl_lost` and must be
  reseeded; the primary never connects to it again. Reading the gap from
  WAL files is the follow-up that lifts the memory bound. Checkpoints do
  not wait for the standby.
- **Seeding** is a new space on both sides: create the standby first with
  the primary's UUID (`--space-uuid`, or `init_uuid`), or copy a stopped
  primary's directory. The handshake refuses another space, shard count or
  slot map.
- **Stop** waits for a connected standby to ack everything, so a clean
  stop leaves the two logs equal.
- **Mint leases.** A promoted standby holds IDs minted under its primary's
  leases, which its own superblock never handed out. Recovery tracks the
  highest lease it installs and mints above it; checkpoints persist it.
- **Standby checkpoints** may truncate records it received, as on any
  engine; the two logs then agree where both have a record.
- **Tests** (`tests/test_replication.cpp`): equal logs and objects after a
  clean run; SIGKILL of a sync primary under transfers and deposits, then
  promotion, three rounds (every acked deposit present, transfers
  conserved, new IDs unique); a late standby catching up under live load;
  a standby past the retention budget reported lost; a sync primary with
  no standby degrading after its timeout.

## Open questions for later rounds

- Automatic failover needs an outside arbiter (or three nodes) to avoid
  split brain. That belongs with the distributed directory.
- Serving reads from the standby (with bounded staleness) is cheap to add
  once apply is live, but it changes the read API's promises.
- Seeding over the link (checkpoint shipping), and standbys with a
  different shard count (records routed by slot rather than by stream).
