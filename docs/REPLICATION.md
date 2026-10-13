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
  Round 2 adds `src/core/seed.{h,cpp}`, `read_stream_range` in storage,
  `Engine::Config` `replica_retain_disk_bytes`, `seed_port` and
  `seed_from`, and `snc_server` `--seed-port`, `--seed-from`,
  `--replica-retain-mib`, `--checkpoint-s` and `--report-ms` (a line of
  replication state every N ms).
- **Catch-up: memory, then WAL files** (round 2). Each primary shard keeps
  the records it has flushed and the standby has not acked, up to
  `replica_retain_bytes` (1 GiB by default). On reconnect the standby's
  `next_lsn` picks the resume point, mid-flush if need be. Anything older
  than memory holds is read back from the primary's WAL segments by a
  helper thread (off the shard's core, at lower priority) and sent first;
  the link then continues from memory. Records dropped from memory while
  the standby is live are sent from disk the same way.
- **Retention.** A primary checkpoint keeps the WAL segments its standby
  may still need (from the standby's last acked LSN), up to
  `replica_retain_disk_bytes` per shard (16 GiB by default) beyond what the
  checkpoint itself keeps. Past that budget the oldest go, and a standby
  that needs them is reported `repl_lost`: it must be reseeded.
- **Seeding** (round 2). A primary started with `seed_port`
  (`snc_server --seed-port P`) serves its active checkpoint's files. A
  standby started on an empty directory with `seed_from`
  (`--seed-from host:P`) fetches them during recovery, syncs them, and only
  then writes a superblock naming that checkpoint, so a crash mid-seed
  leaves no space and the next start fetches again. It then recovers from
  the checkpoint like any engine and connects at the checkpoint's LSNs;
  the primary sends the rest from its WAL files and memory. While a seed
  is sent, its checkpoint is not deleted, and the WAL from its LSNs is kept
  until the standby is past them (or for an hour). A space can also start
  new on both sides (`--space-uuid`), or from a copy of a stopped primary's
  directory. The handshake refuses another space, shard count or slot
  map.
- **Divergence.** A primary that crashes after shipping records it had not
  made durable comes back without them and reuses their LSNs. Its standby
  then holds records the primary does not. At each handshake the standby
  sends the crc of its last record; the primary compares it with its own
  record at that LSN (from memory or disk) before acking or sending
  anything, and a standby that is ahead or different is reported
  `repl_lost` with the LSN where they part. It must be reseeded.
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
  conserved, new IDs unique); a late standby catching up from memory, from
  WAL files after a checkpoint, and from a seed when the WAL it needs is
  gone; one without a seed reported lost; a standby killed while its
  primary writes and checkpoints catching up from WAL files on its return;
  a standby ahead of, or different from, a primary restored from an older
  copy refused; a sync primary with no standby degrading after its
  timeout.

## Results

4-vCPU VM, one disk, loopback TCP, 1M accounts, every ack durable. The
standby writes its own WAL with fsync on the same disk.

**In process** (`bench/run_replication.sh`, `bench/results/replication.csv`;
`snc_bench`, 2 shards on cores 0-1, depth 256; the standby on cores 2-3;
3 runs, medians):

| Workload | No standby | Async standby | Sync standby |
| --- | --- | --- | --- |
| 50% reads | 1.49M ops/s, p99 1.0 ms | 0.93M, p99 1.8 ms | 0.81M, p99 2.1 ms |
| Deposits only | 557K, p99 1.6 ms | 484K, p99 2.0 ms | 327K, p99 2.6 ms |

**Against Redis and PostgreSQL** (`bench/conventional/run_replication.py`,
`bench/results/replication_stack.csv`; servers on cores 0-1, replica on
core 2, the same 50/50 workload as docs/CONVENTIONAL.md; SNC through
`snc_server` and one `net_bench` thread on core 3):

| System | No replica | Async replica | Sync replica |
| --- | --- | --- | --- |
| SNC | 1.06M ops/s | 868K (0.82×) | 640K (0.60×) |
| Redis, AOF always on both | 129K | 104K (0.80×) | not available |
| PostgreSQL, pipelined | 34K | 38K (1.10×) | 29K (0.84×) |

With a synchronous standby SNC does about 22× PostgreSQL with synchronous
streaming replication, and about 6× Redis with its (asynchronous)
replica.

What the numbers say:

1. **Sync costs 40-46% here, and most of it is not waiting.** Sync adds
   the standby's fsync and a loopback round trip to each group commit;
   p50 moves from 0.2 to 0.4 ms. But async, which waits for nothing,
   already costs 13-38%. On loopback the kernel charges TCP receive work
   to the sending core, the standby's shards take cores that the
   primary's io_uring workers used, and both WALs share one disk. A
   standby on its own machine with its own disk removes all three. This VM
   cannot show that, so these costs are an upper bound.
2. **The faster the primary, the more the standby costs in relative
   terms.** The 50/50 mix keeps 0.54× in process with sync, deposits-only
   keeps 0.59×: the fixed per-record costs above weigh more at 1.5M ops/s
   than at 557K.
3. **PostgreSQL's async replica is faster than none**: repeatable over 3
   runs, likely the restart that enabling `wal_level = replica` needs
   (fresh shared buffers and a checkpoint) rather than replication itself.
   Its sync cost (0.84× of none) is smaller in relative terms because
   each commit already waits on its own fsync with 8 connections; SNC's
   group commit is far shorter, so a round trip weighs more.
4. **Redis has no synchronous mode** for a script: `WAIT` cannot run
   inside `EVALSHA`. A Redis deposit acked with a replica can still be
   lost with the primary.
5. **A standby must not share cores with a spinning process.** A first
   run put the standby on the load generator's cores: sync throughput
   swung between 0.3K and 350K ops/s, with stalls of several seconds,
   because the standby's io_uring workers starved. The standby needs its
   cores (or a machine) to itself, like a primary.

### Seeding and catch-up

`bench/run_seeding.py`, `bench/results/seeding.csv`. Primary on cores 0-1
with a checkpoint every 4 s and 64 MiB of WAL in memory per shard; one
`net_bench` thread on core 3 (1M accounts, 50% reads, 12 s); a new
standby on core 2 seeding from the primary. 3 runs, medians.

| Case | Client ops/s | Standby caught up after | Seed | From WAL files |
| --- | --- | --- | --- | --- |
| No standby | 713K | | | |
| Standby joins 2 s into the run | 406K (0.57×) | 10.4 s | 0-244 MiB | 667 MiB |
| Standby joins after the run | | 4.8 s | 244 MiB | 246 MiB |

- **A new standby is caught up in about 5 s when the primary is idle**:
  the 244 MiB checkpoint of 1M accounts plus 250 MiB of WAL written
  since it, about 100 MiB/s over loopback on this disk.
- **Under load it takes about 10 s and costs the clients 43% over the
  run**: the standby has to catch up with WAL that keeps growing, the
  primary's shards send it all over loopback TCP (the kernel charges that
  to their cores), and the reader shares the one disk with both WALs.
  Where the run's latest checkpoint was still the initial empty one, the
  seed was empty and everything came from WAL files. Rate-limiting
  catch-up would trade a longer join for a smaller hit; it is not built.

## Open questions for later rounds

- Automatic failover needs an outside arbiter (or three nodes) to avoid
  split brain. That belongs with the distributed directory.
- Serving reads from the standby (with bounded staleness) is cheap to add
  once apply is live, but it changes the read API's promises.
- Standbys with a different shard count (records routed by slot rather
  than by stream).
- Rate-limiting catch-up so a joining standby costs the primary's clients
  less (see the seeding results).
