# Track B prerequisites: source research (2026-11-22)

One Opus research pass, read-only, after P-B. It settles or narrows the UNRESOLVED prerequisites in
`docs/DIRECTION_COMPARISON_PLAN.md` 4.1 to 4.3 from primary sources at pinned versions. Nothing was run. The report
follows verbatim. Where it corrects the plan, the plan carries an erratum note in 4.1.

## Direction-comparison prerequisites (Track B): source research

I read source only. Nothing was compiled or run, and no repository file was written. Pins: xfstests mirror `kdave/xfstests@d07dca72`; Linux `v6.8`; Garnet `ef3c20df65aa` (the same pin as the sources doc); dotnet/runtime `v10.0.0` (Garnet targets net10.0, per its `Directory.Build.props` and `global.json`); Restate `v1.8.1` (`20efb1b8`); PostgreSQL `REL_18_1`; docs-restate `1e57259`; Debezium `v3.3.0.Final`. postgresql.org and microservices.io were unreachable from this container, so PostgreSQL docs are cited from the repo's sgml.

### 1. dm-log-writes and replay-log

- **Where it lives.** FACT: `src/log-writes/{replay-log.c,log-writes.c,log-writes.h,Makefile}`. The Makefile includes `../../include/builddefs` (Makefile:3-4), so building in-tree needs xfstests' autoconf build.
- **Standalone build.** FACT: the sources include only libc headers plus `<linux/fs.h>`, `<linux/types.h>` and `<linux/byteorder/*>`. INF: `gcc -O2 -o replay-log replay-log.c log-writes.c` should work with just `linux-libc-dev`. I did not compile it.
- **Ubuntu 24.04 packaging.** FACT: packages.ubuntu.com (noble) finds no package named `xfstests` or `fstests`, and no package containing a file named `replay-log`.
- **`--end-mark` and `--limit` include the stop entry.** FACT: the loop at replay-log.c:363 replays an entry first, then breaks on `run_limit && num_entries == run_limit` or `should_stop(...)` (388-390). So `--limit N` replays exactly N entries, and `--end-mark M` replays through the mark entry. A mark has no data and writes nothing (log-writes.c:280, `if (!size) return 0`). `should_stop` matches the mark label (replay-log.c:83-100). `--find --end-mark` prints `entry@sector` (330-335). xfstests itself runs `--find` and then `--replay <dev> --end-mark` (common/dmlogwrites:144-149).
- **Log flags.** FACT: `LOG_FLUSH_FLAG` 1<<0, `LOG_FUA_FLAG` 1<<1, `DISCARD` 1<<2, `MARK` 1<<3, `METADATA` 1<<4 (log-writes.h:24-28). The kernel sets FLUSH from `REQ_PREFLUSH` and FUA from `REQ_FUA` (drivers/md/dm-log-writes.c:652-697).
- **Ordering in the log (this matters).** FACT: a plain write that completes goes onto `unflushed_blocks`. Those writes are logged only when a later flush completes; the flush map splices them in (715-719), then end_io appends them followed by the flush entry (777-780). A FUA write is appended when it completes (781-783). A mark goes straight onto `logging_blocks` (log_mark, ~602-607). The header comment says writes are logged "only after they have been flushed" (36-37).
  - INF: every entry before mark M is a write covered by a completed flush, or a completed FUA write. So `--end-mark M`, replayed from log start, already gives the "volatile cache lost at M" image. There is no need to locate "the last flush before the mark".
  - INF: do not use `--start-mark mkfs`. The mkfs writes precede that mark.
- **FUA to post-flush when fua=0.** FACT: `blk_flush_policy` adds `REQ_FSEQ_POSTFLUSH` when the queue has WC set, lacks FUA, and the request has `REQ_FUA` (block/blk-flush.c:103-118). That is blk-mq, which loop uses. For bio-based queues, `submit_bio_noacct` strips PREFLUSH/FUA only when WC=0 (block/blk-core.c:759-770).
- **dm-log-writes advertises FUA itself, so the plan's wording is wrong.** FACT: the target sets `flush_supported = true` (dm-log-writes.c:571-572). `dm_table_supports_flush` returns true for any target with `flush_supported`, whatever flag is asked (drivers/md/dm-table.c:1803-1824). So `blk_queue_write_cache(q, wc=true, fua=true)` (1987-1992).
  - INF: the filesystem sees fua=1 on the dm device and sends REQ_FUA. The log records `LOG_FUA_FLAG` entries. The FUA-to-flush conversion happens below, at the loop device, and the log does not see it.
  - So "a flush is the only durability point in the log" (plan section 4) is false: FUA entries are durability points too. Replay-to-mark stays correct, because FUA writes are logged at completion.
  - UNVERIFIED for kernels after 6.8; the queue_limits rework may differ.
- **Loop device settings.** FACT: loop calls `blk_queue_write_cache(lo_queue, true, false)` when the backing file has fsync (drivers/block/loop.c:1075-1076). That sets `HW_WC` and `WC` and clears `FUA` (block/blk-settings.c:834-845). So the default is already "write back" with fua 0.
  - `write_cache` is writable; "write back" is accepted only if `HW_WC` is set (block/blk-sysfs.c:444-459).
  - `fua` is read-only, mode 0444 (blk-sysfs.c:520). Loop does not support FUA. A flush becomes `vfs_fsync` of the backing file (loop.c:326-328, 474-475).

**SETTLED**, with two corrections to the plan's text:
- Nothing needs setting on the loop device. Verify `write back` and `0` and put them in the manifest.
- The log contains FLUSH and FUA entries, and `--end-mark` replay (inclusive, from entry 0) is the right check.

**UNRESOLVED:** only the standalone `gcc` build and a blkparse or `replay-log -v` look at the entry flags. A 10-minute VM test settles both: mkfs, fsync, mark, then `replay-log --log L -v`.

### 2. Garnet

- **FileStream gives O_SYNC on Linux.** FACT: .NET 10 maps `FileOptions.WriteThrough` to `OpenFlags.O_SYNC` (SafeFileHandle.Unix.cs:301-305), and that becomes native `O_SYNC` (src/native/libs/System.Native/pal_io.c:310-311). .NET 9 is the same (SafeFileHandle.Unix.cs:313-315).
  - FACT: `--device-type FileStream` creates `ManagedLocalStorageDevice` (Tsavorite Devices.cs:53), whose write handle uses `WriteThrough|Asynchronous` (ManagedLocalStorageDevice.cs:435-440).
  - INF: on Linux, O_SYNC includes O_DSYNC. Garnet's comment in LinuxFileExtensions.cs:19-21 says this path ends up with "neither O_DIRECT nor O_DSYNC". The .NET source contradicts that for O_DSYNC.
  - FACT: `0x20000000` (no-buffering) is not in pal_io's allowed set (pal_io.c:294), so there is no O_DIRECT. INF: .NET's managed code may drop it before then.
- **Native device does no flush or fsync.** FACT: a grep of the whole `libs/storage/Tsavorite/cc` tree finds no fsync, fdatasync, O_SYNC, O_DSYNC or RWF_. Open adds only `O_DIRECT` (file_linux.cc:323-328 for libaio, 673-678 for io_uring). Neither TsavoriteLog nor the device code calls fsync.
  - INF: with `--aof-commit-wait`, a Native-device commit is O_DIRECT completion only. The device's volatile cache is not flushed, so the Native configuration should fail the dm-log-writes test.
- **AOF device type is not separate.** FACT: the AOF device comes from the same `GetInitializedDeviceFactory`/`DeviceFactoryCreator` as the main log (GarnetServerOptions.cs:1276-1285, 1362, 1755-1770), and so does the AOF commit-metadata manager (1667-1671). The only AOF-specific device option is `--aof-null-device` (Options.cs:554).
  - So FileStream's O_SYNC also applies to the main log and checkpoints. The plan's "UNVERIFIED" on this becomes FACT, and the cost is labelled "favours SNC".
- **Readers can see writes not yet committed to the AOF.** FACT: the value is written in place first. The AOF record is appended afterwards in `PostUpsertOperation`/`PostRMWOperation` (MainStore/UpsertMethods.cs:118-132, RMWMethods.cs:1840-1842).
  - FACT: the commit wait happens only in `Send()`, after the commands have executed (RespServerSession.cs:1566-1571). It is armed only for commands that are not `IsAofIndependent()` (RespCommand.cs:1279-1294).
  - INF: a GET in another session reads the in-memory value with no wait, before that AOF commit.

**SETTLED** (all four sub-questions). Residual UNVERIFIED: whether `RUNTXP` transactions follow the same Send-time wait. Check `TxnManager` commit or the custom-procedure path.

### 3. Restate 1.8.1

- **Ingress reply follows durable log commit (single-node, replication 1).**
  - FACT: the default provider is `Replicated` (types/config/bifrost.rs:152).
  - FACT: the log-server writes with `set_sync(true)`, or `!rocksdb_disable_wal_fsync` (default false), with the WAL on by default (log-server/rocksdb_logstore/writer.rs:498-507; config/log_server.rs:445-446). It advances the tail watch after the batch commits (writer.rs:553-557; loglet_worker.rs:556-557, "advances ... after durable commit").
  - FACT: the sequencer marks a node committed on `Stored Ok`. It moves `known_global_tail` only when the write quorum holds (sequencer/appender.rs:442-447, 478-483).
  - FACT: the partition processor's reader is bounded by `known_global_tail`, with `move_beyond_global_tail=false` (replicated_loglet/loglet.rs:289-301). It applies records, commits, and only then calls `handle_actions` (worker/partition/mod.rs:548-557, 651-705).
  - FACT: `RpcReply::Output` is built only in the state machine (state_machine/mod.rs:4472). It is delivered by `Action::ReplyRpc`, which takes the waiter from `awaiting_rpc_actions` (leadership/leader_state.rs:1031-1033).
  - INF: the ingress response for an invocation is sent only after the record carrying its output has been fsynced on a quorum (1 node).
  - UNVERIFIED: how the ingress-http handler side registers the request with the partition-processor RPC. I did not trace it.
- **Shared handlers are journaled invocations and read only durable state.** FACT: docs list "Concurrent execution of shared handlers (read-only)" (foundations/services.mdx:15, 217). The invoker loads eager state from the partition store (invoker_storage_reader.rs via `ReadStateTable`; service_protocol_runner_v4.rs:317-325).
  - FACT: state is written to the partition store only when a `SetState` journal command is applied from the log (state_machine/entries/set_state_command.rs:43). The partition store's own WAL is off (partition-store/partition_store.rs:885, 898, 1092). It is rebuilt from the log.
  - INF: a shared handler runs as a full invocation, with an invoke record plus output and end records on the log, so it costs log appends and fsync. It sees only state from committed records, possibly from an exclusive handler still in flight.
  - INF: it is not cheaper on the log path than an exclusive handler. It only avoids the per-key lock.
- **SQL introspection reads only durable state.** FACT: the `state` table scans `PartitionStore` (storage-query-datafusion/src/state/table.rs:15-17, 70).
  - INF: it reads only applied state, which is a prefix of the durable log. It is durable but may lag, and it appends nothing to the log.

**SETTLED (INF strength)** for reply-after-fsync and for which reads see only durable state. **UNRESOLVED:** what each read costs. Settle in bring-up by timing SQL `SELECT ... FROM state WHERE service_key=...` against a shared-handler call, and by including both in the dm-log-writes probe check.

### 4. PostgreSQL 18

FACT, in src/backend/access/transam/xact.c:
- `RecordTransactionCommit` (1315) writes the commit record (1442).
- If `synchronous_commit > OFF` (or a forced sync, or rels to drop), it calls `XLogFlush(XactLastRecEnd)` (1498-1502), then `TransactionIdCommitTree` (1507-1508). Otherwise it uses the async variant (1531).
- `SyncRepWaitForLSN` follows (1557).
- `CommitTransaction` calls `ProcArrayEndTransaction` only after `RecordTransactionCommit` returns (2365, then 2389).

INF: new snapshots treat the xid as running until the ProcArray removal, so other sessions see the commit only after the local WAL flush. With no synchronous standby the sync-rep wait is a no-op (syncrep.c not checked).

`commit_delay`:
- FACT: in `XLogFlush`, the process that gets `WALWriteLock` sleeps `CommitDelay` µs if fsync is on and `MinimumActiveBackends(CommitSiblings)` holds, then flushes as far as WAL has been inserted (xlog.c:2882-2885).
- FACT: the docs say it "adds a time delay before a WAL flush is initiated", "no delays are performed if fsync is disabled", and the default is 0 (config.sgml:3572ff).

**SETTLED.** The plan's "PostgreSQL visibility after local WAL flush: UNVERIFIED" can become FACT (source ordering) plus INF (snapshot rule).

### 5. PostgreSQL transactional outbox

- FACT: Debezium describes the outbox pattern as avoiding "inconsistencies between a service's internal state ... and state in events". Its event id can be used "to remove duplicate messages" (debezium `documentation/modules/ROOT/pages/transformations/outbox-event-router.adoc`:19, 143). This is the log-tailing (CDC) relay variant.
- UNVERIFIED (not fetched): Chris Richardson's https://microservices.io/patterns/data/transactional-outbox.html. Its text was not checked.
- FACT: `SKIP LOCKED` "can be used to avoid lock contention with multiple consumers accessing a queue-like table" (doc/src/sgml/ref/select.sgml:1603-1610).
- FACT: NOTIFY events "are not delivered until and unless the transaction is committed" (notify.sgml:79-91). It works as a wake-up hint, not a durable queue.
- Usual worker settings (INF): poll interval or LISTEN/NOTIFY with a fallback poll, batch size, worker count, delete or mark-sent, and an idempotency key at the consumer.

Minimal design:
1. The debit transaction does `UPDATE acct ... ; INSERT INTO outbox(id bigserial, dst, amt, xfer_id) ; NOTIFY outbox;` in one commit.
2. The worker runs `LISTEN outbox` and also polls every P ms.
3. Each loop, in one transaction: `DELETE FROM outbox WHERE id IN (SELECT id FROM outbox ORDER BY id LIMIT B FOR UPDATE SKIP LOCKED) RETURNING *`.
4. In the same transaction it applies each credit with `INSERT INTO applied(xfer_id) ON CONFLICT DO NOTHING` and updates the target only when inserted, then commits (exactly-once inside one database).
5. The tuning budget is P, B and the worker count W.

**SETTLED.**

### What changes in the plan
- Section 4's dm-log-writes text needs the FUA correction.
- These move from UNVERIFIED to FACT:
  - FileStream gives O_SYNC on all Garnet logs.
  - Native does no fsync.
  - Garnet readers see writes not yet committed to the AOF.
  - PostgreSQL makes a commit visible only after the local WAL flush.
- Still open:
  - building `replay-log` on the VM;
  - the cost of Restate's durable reads;
  - the Restate ingress-side RPC registration;
  - the `RUNTXP` commit-wait path.

Source clones are in `(scratch, not kept) src/`, and fetched docs are in `(scratch, not kept) raw/`.
