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
  sets the timeout to 0. Either way the choice is explicit, not silent. A
  standby that hangs with its socket open (stopped, or wedged) counts as
  lost once it acks nothing new for the same timeout while records wait.
  With a witness (below), continuing alone needs the witness's leave.
- **Primary lost.** An operator (or, later, an arbiter) promotes the
  standby: in place with `Engine::promote()` (`snc_server`: SIGUSR1), or by
  stopping it and starting it as a primary on its data directory. Every
  write acknowledged in sync mode is in its WAL. Follow-ups in flight
  (transfers' credits and settles) resume from the objects' resume hooks,
  as after any crash (docs/TRANSFERS.md). `snc_witness` can decide when
  (below).
- **Fencing by term.** Each superblock carries a term; promotion bumps it,
  the handshake carries it, and a standby refuses a primary of an older
  term. An old primary that comes back therefore cannot get its writes
  acknowledged by any standby of the new primary. With sync replication
  and `replica_timeout_ms = 0` it then acknowledges no durable write at
  all; with a nonzero timeout and no witness it would degrade and carry on
  alone, so such a deployment sets the timeout to 0, keeps a replaced
  primary down, or runs a witness. Reads wait for durability by default and stall the same way; a read
  that skips that wait can still be served by such a primary.

## Automatic failover

`snc_witness` is a third process that decides two things, and records
each decision (written and synced) before anyone acts on it:

- **Whether the primary may continue without its standby.** A sync
  primary started with `--witness` (`Engine::Config::replica_degrade_gated`)
  that passes its timeout asks the witness (`DEGRADE uuid term`) instead
  of degrading on its own. The witness grants it only to the term it
  considers current. When the standby is back in sync the primary says so
  (`SYNCED`), closing the grant.
- **When to promote the standby.** It polls both nodes' admin ports
  (`snc_server --admin-port`) with `HEALTH`, which the node answers
  without waiting on its shard threads, so a long task on a shard does
  not read as death ([HEALTH_DECOUPLING.md](HEALTH_DECOUPLING.md); a node
  without `HEALTH` is polled with `STATUS`). If the primary has not
  answered for `--failover-ms`, and the standby answers `STATUS` (every
  shard of it, within `--poll-ms`) as a standby in the same space and
  term, and the primary holds no grant, it records the new term and
  promotes the standby (`PROMOTE`). With a grant out it does not: the
  standby may lack acknowledged writes, so it waits for the primary. An
  answer to `HEALTH` proves the process is alive, nothing more: a primary
  whose shard is stuck but whose process answers is not failed over.

Together these keep one rule: **a write is acknowledged only if the node
that will serve next has it.** Either the standby acked it (sync), or the
witness let the primary run alone and then will not promote the standby.
A primary that is cut off rather than dead (from the witness, or from
everything) is fenced twice: its standby, once promoted, refuses its
term; and the witness refuses to let it continue alone. It acknowledges
nothing durable.

**A fenced primary withdraws.** A promoted node keeps its replication
listeners, which from then on refuse every primary, with "term replaced"
for an older one. An old primary still running that reconnects there (or
that a witness answers `NO term ... was replaced`) knows it is fenced for
good and leaves the data plane: it closes its node links (docs/CLUSTER.md),
runs no more follow-ups, answers new ops `E_STANDBY`, and answers ops that
were waiting for its standby's ack `E_FENCED`: in doubt, since the promoted
standby may hold them (a session client's resend is safe, and
`net::Client` fails over on either). Its admin port stays up and reports
`role=f`; only a restart (e.g. `--rejoin`) brings it back, as a standby.

The witnesses also hold the cluster's placement map (docs/CLUSTER.md,
"Starting from the control plane"): a second state machine (`placement`)
on the same Paxos library (`tools/paxos.h`), next to the pair's (`ha/0`).
Each machine has its own versions; neither ever changes the other. A
witness started without `--primary`/`--standby` serves placement only.
State files from before the split are read as `ha/0`.

One quorum can serve every group of a cluster: `--group G=PRIMARY:ADMIN,
STANDBY:ADMIN` (repeated) watches each group's pair on its own machine,
`ha/G`, with its own terms and grants; `--primary`/`--standby` are group 0.
A clustered primary names its group in `DEGRADE` and `SYNCED`. The quorum
also hands out cluster-wide mint lease blocks (`leases`,
docs/CONTROL_PLANE.md section 11).

### Replicated witnesses

One witness is a single point of failure for availability: while it is
down, failover waits, and so does a primary past its timeout. Started with
`--id I --peers H:P,H:P,H:P` (every witness, this one at index I), N
witnesses (3 or 5) keep one decision state between them, and any minority
of them may be down.

- **One state, changed by Paxos.** The state (space, term, grant, failed
  over) has a version. Every change (learning the primary, a grant, the
  standby back in sync, a promotion) is the value of one single-decree
  Paxos instance, the one for the next version: a majority of witnesses
  promises a ballot, then accepts the value. A proposer that finds a value
  already accepted in that instance proposes that value instead, then
  retries its own change on the newer state. Promises and accepts are
  synced to each witness's state file before they are answered.
- **A grant and a promotion exclude each other.** Each is a change to the
  same version of the state, checked against it (a promotion needs no
  grant out; a grant needs the term not replaced). Only one value is chosen
  per version, so a primary can never be granted while the standby is
  promoted over it, whichever witnesses each request reached.
- **Any witness serves.** The primary sends `DEGRADE` and `SYNCED` to the
  first witness on its `--witness` list that answers; that witness runs the
  Paxos round and answers `OK` only once the change is chosen. Every
  witness watches the nodes and may propose the promotion; whichever
  witnesses learn it promote the standby (`PROMOTE` is idempotent).
- **A lost answer is safe.** A grant chosen without the primary hearing
  `OK` only makes the witnesses more careful (they will not promote); the
  primary asks again and gets `OK` for the grant already chosen. After
  running alone, the primary closes its gate before telling the witnesses
  its standby is back, and keeps telling them until one confirms.

### Failing over again

The witnesses watch two nodes, not two roles: `--primary` and `--standby`
name them as they start, and the decision state records which node holds
the current term. After a failover:

1. The promoted node runs alone. Started with `--replica-of` the other
   node and `--witness` (flags a standby ignores until it is promoted), it
   is a sync primary without a standby, so it asks the witnesses to run
   alone and they grant it for its new term. While the grant is out they
   promote nothing.
2. The old primary is restarted as its standby, seeded from it. With
   `--rejoin STANDBY_PORT,PEER:ADMIN,PEER:SEED` this happens on its own
   when the node is restarted with its usual command line: before serving,
   it asks the other node, finds it primary of the same space, moves its
   own data directory aside (`DIR.stale-TIME`, kept for inspection, never
   deleted) and starts as that node's standby, seeded from it. The seed
   carries the new term, so the new primary's records are accepted.
3. Once that standby is back in sync, the primary reports `SYNCED`, the
   grant is closed, and the witnesses will promote the old primary if the
   new one fails.

No witness is restarted and no configuration changes. A supervisor that
restarts a failed process (systemd's `Restart=always`, for example) is all
that is needed for the pair to heal after a failover.

Limits of this version:

- **Two nodes.** The witnesses watch one pair; a third node (a second
  standby) is not supported.
- **Rejoin covers the node started as primary.** A node started with
  `--standby` that was promoted and then failed comes back as a standby
  with its old data; if that data diverged (writes it acknowledged alone),
  the new primary refuses it and it has to be reseeded by hand.
- **Each rejoin keeps a full copy aside.** Old `.stale-` directories are
  left for an operator to remove.
- **A majority of witnesses must be up** for failover or for a primary
  past its timeout to continue alone: availability needs them, safety does
  not. With three, one may be down. A single witness (no `--peers`) runs
  the same protocol with itself as the majority. Their state files survive
  restarts.
- **Membership is fixed** by the command line; changing it needs all
  witnesses restarted with the new list and the same state.
- **Failure detection is the witness's view.** A primary the witness
  cannot reach but clients can is promoted over; the fencing above makes
  that safe for durable writes, at the cost of those clients' availability
  until they move to the new primary. A `net::Client` with endpoints and
  a session moves itself and resends safely (docs/RETRIES.md).
- Reads that skip the durability wait can still be served by a fenced
  primary.

## Reads on the standby

A standby started with `standby_read_lease_ms` (`snc_server
--standby-reads MS`) serves READ ops; writes, creates, deletes and index
lookups stay refused (`E_STANDBY`). It serves them only under a **read
lease** from a sync primary, and answers `E_STALE` without one.

- Each shard's standby asks for a lease with its acks, at least four times
  per lease, stamping the request with its own clock. A primary that holds
  client acks for this standby (sync, not degraded) grants it: it echoes
  the stamp, and it will not continue without the standby until a lease
  length after it received the request. The standby counts from its stamp
  (earlier than the primary's receipt) and keeps a 10% margin for the two
  clocks' rates. Clocks need not agree, only run at about the same rate.
- **The guarantee:** a read on the standby sees every write the primary
  acknowledged before the read began. In sync mode the primary acks a
  write only after the standby has it durably; the lease keeps the primary
  from degrading (acking writes the standby lacks) while the standby may
  still be serving reads.
- **The cost:** a sync primary that loses a leasing standby waits for the
  later of its `replica_timeout_ms` and the lease before continuing alone.
  An async primary grants no leases, so its standby serves no reads.
- Leases are per shard, so a standby shard whose link is down refuses
  reads while its other shards serve.

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
- **Promotion in place** (`Engine::promote()`): each shard stops taking
  records and waits until every installed record is durable; the engine
  then takes leases above every ID the old primary minted, bumps the term
  and the generation in a durable superblock, resumes follow-ups, and
  starts shipping to `replica_host` if one is configured (`standby_port`
  keeps the port it listened on apart from the one it ships to). The
  secondary index is already current: a standby maintains it as it
  installs. Seeds carry the term, so a standby seeded from the new primary
  starts in its term.
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
  timeout; a standby promoted in place after its sync primary is SIGKILLed
  under transfers and deposits (every acked deposit kept, transfers
  conserved, term 1, new IDs unique, index keys changed on the primary
  found on the promoted node), then the old primary restarted with
  timeout 0: its deposit is not acknowledged and a standby seeded from the
  promoted node refuses it.

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
  seed was empty and everything came from WAL files.

**Rate-limited catch-up** (`replica_catchup_bytes_per_s`, `snc_server
--catchup-mib-s`): a token bucket on what each shard sends from WAL files
(at most 10 ms of burst). `bench/results/seeding_rate.csv`, same setup,
5 runs alternating with a no-standby run; client throughput as a ratio
to the no-standby run next to it.

| Catch-up limit per shard | Client ops/s vs none (median, range) | Caught up after (median) |
| --- | --- | --- |
| none | 0.55× (0.47-0.81) | 11.9 s |
| 30 MiB/s | 0.63× (0.30-1.32) | 13.2 s |
| 15 MiB/s | 0.80× (0.36-1.16) | 23.7 s |

- **15 MiB/s per shard halves the clients' loss for twice the join
  time.** 30 MiB/s barely limits anything: unlimited catch-up already
  ran at about 30 MiB/s per shard here.
- **These runs are noisy.** The no-standby runs alone ranged from 317K to
  631K ops/s in this set, from the shared virtual disk, so the ratios
  carry wide ranges; the medians are the claim, and only roughly.
- The limit applies to WAL files only. Catch-up from memory is not
  limited: it is at most `replica_retain_bytes` and is what the standby
  needs next to go live.
- The limit must exceed the shard's WAL write rate, or the standby never
  catches up. A test checks the rate holds and catch-up completes
  (`test_late(SLOW)`).

### Promotion

`bench/run_promotion.py`, `bench/results/promotion.csv`. Primary on cores
0-1, sync replication (timeout 0) to a standby whose two shards share core
2; one `net_bench` thread on core 3 creates 1M accounts and runs 50% reads
for 5 s. Then the primary is SIGKILLed and the standby gets SIGUSR1.
3 runs.

| | Median | Range |
| --- | --- | --- |
| Client ops/s on the primary before | 632K | 621-660K |
| Promotion (signal to serving) | 0.052 s | 0.050-0.072 s |
| Client ops/s on the promoted node, no standby | 957K | 921K-1.01M |

- **Promotion takes about 50 ms for 1M accounts**, measured inside the
  engine and from outside (signal to the "promoted" line; the two agree
  within a millisecond). Most of it is the resume-hook pass over every
  object.
- **The standby keeps its index live** as it installs records (old key
  out, new key in, only when the key changes), so promotion does not
  rebuild it. Rebuilding took about 0.6 s here (0.55-0.64 s in the first
  version of this benchmark), 92% of the promotion. The primary's
  throughput with the live index, 632K, is within this VM's run-to-run
  spread of the 663K measured with it rebuilt at promotion.
- The promoted node runs without a standby until one is seeded from it.

### Automatic failover

`bench/run_failover.py`, `bench/results/failover.csv`: primary on cores
0-1 (sync, 500 ms timeout, `--witness`), standby on core 2, witnesses with
`--failover-ms 500 --poll-ms 50`, one `net_bench` thread on core 3, 100K
accounts. Each case is checked, 3 runs of each case in each witness
configuration (one witness; three; three with one killed once the primary
is up, the one the primary lists first). All 36 passed.

| Case | Checked |
| --- | --- |
| Primary SIGKILLed | the witnesses promote; the new primary serves |
| Standby stopped under load | the primary is granted and continues; resumed, it reports in sync; stopped again, a second grant; then the primary is killed and the witnesses do not promote the stale standby |
| Primary stopped (cut off) | the witnesses promote; the old primary, resumed, acknowledges no create from a new client and is refused; the new primary serves |
| Two failovers in a row | the primary is killed and the standby promoted; it runs alone under a grant and creates 20K accounts; the old primary is restarted with its original command line and rejoins (`--rejoin`: data moved aside, seeded from the new primary); once in sync, the new primary is killed and the old one promoted again; all 20K accounts are there |

Medians, in seconds:

| Witnesses | Kill to promoted | Decision and promotion | Client stall before a grant | Stop to promoted | Rejoin to in sync | Second failover |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 0.51 | 0.016 | 0.54 | 0.55 | 0.71 | 0.51 |
| 3 | 0.49 | 0.015 | 0.53 | 0.54 | 0.97 | 0.52 |
| 3, one down | 0.48 | 0.017 | 0.54 | 0.54 | 0.76 | 0.54 |

- **Failover takes about half a second, almost all of it detection**
  (`--failover-ms`). Deciding by Paxos among three witnesses and promoting
  takes 15-30 ms. That includes each witness syncing its state file twice
  per decision.
- **The second failover is as fast as the first.** A failed primary
  rejoins as a standby and is back in sync in under a second (seeded from
  the new primary's checkpoint, then its WAL); from then on it can take
  over again, with no witness restarted.
- **With one of three witnesses down, nothing changes but a few
  milliseconds**: the primary's requests skip the dead witness (connection
  refused), and the other two are a majority.
- With two of three down there is no majority. The primary is not
  promoted over, and a primary past its timeout waits (checked by hand
  with `--down 2 --cases kill`: no failover).
- `tests/test_witness.py` (in ctest) races the two decisions against
  three witnesses with fake nodes. Each trial cuts the primary off from
  the witnesses' polling while it asks one witness, picked at random, to
  run alone. In 100 trials there were 68 grants and 32 promotions, and
  never both. Every live witness ended on the same state, including one
  trial with a witness down. Run as three independent witnesses (each its
  own majority), the same test finds a grant and a promotion together.
- Client throughput in these runs (130K-440K ops/s) varies with the
  shared disk; these runs measure correctness and timing, not throughput.

### Reads on the standby

`bench/run_standby_reads.py`, `bench/results/standby_reads.csv`: primary on
cores 0-1, sync replication to a standby with 200 ms leases whose two shards
share core 2; 1M accounts. A writer (deposits) and a reader (get_balance),
one `net_bench` thread each on core 3, 512 in flight, 8 s, 3 runs (medians).

| Reader on | Writer ops/s | Reader ops/s | Stale answers |
| --- | --- | --- | --- |
| Primary (2 cores) | 264K | 1.63M | |
| Standby (1 core) | 250K | 1.13M | 0 |

- **The standby serves 1.13M reads/s on one core** while applying the
  primary's 250K writes/s, with no stale answers: leases were renewed
  throughout.
- On this 4-vCPU VM the standby has half the primary's cores, so moving the
  reads there lowers the total. Where the standby has its own machine, its
  reads add to the primary's capacity instead of sharing it.
- Moving the reads off the primary barely changes the writer (264K vs
  250K, within run-to-run spread).
- `tests/test_replication.cpp` checks the guarantee: 3000 deposits, each
  read on the standby right after the primary acknowledged it, all seen;
  the primary stopped, reads turn `E_STALE` within 0.2 s; and a 100 ms
  timeout waits out a 1.5 s lease before the primary continues alone.

## Open questions for later rounds

- Fixed witness membership; one standby per primary; a promoted node
  whose data diverged is reseeded by hand.
- Index lookups on the standby (they gather across shards, each of which
  would need its lease).
- Standbys with a different shard count (records routed by slot rather
  than by stream).
- Choosing the catch-up limit automatically from the WAL write rate and
  the clients' load.
