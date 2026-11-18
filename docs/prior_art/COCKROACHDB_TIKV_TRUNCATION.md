# CockroachDB and TiKV raft-engine: log truncation, applied state, recovery

Pins: **CockroachDB `v24.3.0`** and **tikv/raft-engine tag `0.4.2`** (the tag has no `v`; CHANGELOG dates it 2024-04-16). All sources are raw.githubusercontent.com at these pins. Labels: FACT-SRC, DOC, INF, HYP.

**UNVERIFIED:** GitHub issue and PR pages (#78412, #99273, #36262, #93248, #40213) returned 403 from both api.github.com and github.com. What follows about them is only what the code comments say. No separated-raft-storage design doc was read.

## A. CockroachDB v24.3.0

### A1. Truncation index: `kvserver/raft_log_queue.go` `computeTruncateDecision`
1. Clamps:
   - The index starts at LastIndex and is clamped to commit: FACT-SRC `decision.ProtectIndex(decision.CommitIndex, ...)`.
   - For each follower: RecentActive and StateProbe protects FirstIndex, so the truncation becomes a no-op. RecentActive otherwise protects `Match`. A follower that is not recently active is protected only `if !input.LogTooLarge()`.
   - The pending snapshot index is protected: FACT-SRC `ProtectIndex(input.PendingSnapshotIndex, truncatableIndexChosenViaPendingSnap)`.
   - Asserted bounds: `FirstIndex <= NewFirstIndex <= min(LastIndex, CommitIndex)`.
   - The queue truncates only if `n >= RaftLogQueueStaleThreshold` (100) or the log size is at least 64 KiB.
2. Failure mode it addresses: needless snapshots for live followers. The log size stays bounded because the code is "willing and able to cut off followers as long as a quorum has acked" (FACT-SRC).
3. Weakness: the log size estimate is in-memory. FACT-SRC: "a freshly started node will believe its Raft log to be zero-sized". The decision is advisory only.
4. Ordering:
   - The leader proposes `TruncateLogRequest{Index, ExpectedFirstIndex}` through Raft. It applies at a higher index than anything it removes. FACT-SRC: "By the time the truncation itself is handled, the state machine will have applied all entries the truncation could possibly affect."
   - Each replica treats the index as advisory and ignores one that moves it backwards (`if suggestedTruncatedState.Index <= currentTruncatedState.Index { return false, nil }`).
5. Snapshot pins (`replica_raft.go addSnapshotLogTruncationConstraint`):
   - An **in-memory** map `r.mu.snapshotLogTruncationConstraints[snapUUID]` holds the RaftAppliedIndex at snapshot start. The entry is removed when the OutgoingSnapshot closes.
   - It is not persisted. INF: a crash aborts the snapshot anyway, so this is safe.
6. SNC O-FLOOR (INF): `keep_from`, seed pins and the disk budget map onto Match, snapshot pins and LogTooLarge. In CRDB these pins are volatile inputs, and only the resulting floor is durable. SNC can keep pins volatile if `retain_from` is durable no later than the delete.

### A2. Persisting truncation
1. Default: **strongly coupled**. FACT-SRC: `"kv.raft_log.loosely_coupled_truncation.enabled" ... false`, with the comment "Due to ... issues/78412 we have changed the default to false."
2. Strongly coupled path (`replica_app_batch.go` to `handleTruncatedStateBelowRaftPreApply`):
   - The log keys are cleared and `RaftTruncatedState` is put in the **same Pebble batch** as the applied writes and `RangeAppliedState`. FACT-SRC: "performed atomically with the raft command application so that the TruncatedState index is always consistent with the state of the Raft log itself."
   - The commit is unsynced unless sideloaded files go: FACT-SRC `sync := b.changeRemovesReplica || b.changeTruncatesSideloadedFiles`, because sideloaded files "are removed ... not atomically with it".
   - INF: the single-engine WAL ordering makes the unsynced commit safe.
3. Loosely coupled path (`raft_log_truncator.go`):
   - At apply time the truncation is only queued, with at most two pending.
   - On the Pebble flush callback `durabilityAdvancedCallback`, the truncator reads `RangeAppliedState` via `NewReader(storage.GuaranteedDurability)` and enacts only `trunc.Index <= as.RaftAppliedIndex`.
   - The truncation is then committed with `Commit(false)`. FACT-SRC: "Loss of a truncation means we have more of the suffix of the raft log, which does not affect correctness."
4. The general rule (INF): **the floor may advance only after the state it depends on is durable. A lost floor advance is harmless.** The code names the separated-engine case explicitly. FACT-SRC (TODO(sep-raft-log)): "we must either sync here unconditionally upon log truncation (which would be expensive), or apply the side effects ... asynchronously when sure that the state machine engine has synced".
5. SNC O-FLOOR (INF):
   - SNC's checkpoint and its segments are in separate durability domains, which is CRDB's separated case.
   - Required order: the covering checkpoint is durable, then the manifest with `retain_from` is synced along with its directory, then the deletes. D06_BOUNDARY "compute before publishing" already has this order.
   - The FL check must tolerate "segments below `retain_from` left over" (a lost floor advance). It must never tolerate "segments missing above it".

### A3. Committed / applied state
1. `RangeAppliedState` holds RaftAppliedIndex, LeaseAppliedIndex, the applied term, stats and the closed timestamp.
   - It is written into every apply batch (`addAppliedStateKeyToBatch`).
   - The commit is unsynced. FACT-SRC: "if the batch ends up not being durably committed then the entries in this batch will be applied again upon startup."
2. Entries plus HardState (`logstore/logstore.go`):
   - They go in one batch. FACT-SRC: "If that ever changes, we must write and sync the Entries before the HardState."
   - The batch is synced before the responses go out. This is the ack boundary. FACT-SRC: "these syncs happen before the MsgStorageAppend's responses are delivered back to the RawNode."
   - A non-blocking sync path exists. It is disabled when overwriting entries, so sideloaded files are purged only after their replacements are durable.
3. Recovery and reconciliation:
   - `kvstorage.LoadAndReconcileReplicas` checks only identity: a ReplicaID exists and the descriptor contains the store and ID.
   - `LoadedReplicaState.check` rejects a non-zero Commit on an uninitialized replica. FACT-SRC: "we have observed this in the wild. See #40213."
   - The raft library fail-stops on inconsistency. `raft.go loadState` gives "state.commit %d is out of range". `log.go` gives "tocommit(%d) is out of range [lastIndex(%d)]. Was the raft log corrupted, truncated, or lost?"
4. Detects: Commit above LastIndex, and applied or applying out of range.
5. Does NOT detect (INF): a lost synced tail that was removed together with its HardState. The batch is atomic, so the log looks consistently shorter; this is the D-02 pattern. CRDB relies on the quorum to cover it. It also does not detect a rollback of the whole engine (R-23).
6. Separated raft log (#99273): UNVERIFIED. At v24.3.0 there is still one engine (`TODOEngine()`), with `TODO(sep-raft-log)` markers.
7. Implications (INF):
   - R2′: CRDB never truncates the log by a computation at recovery. It reloads atomically and lets the leader overwrite conflicting suffixes. SNC's computed truncation should be modelled as that kind of overwrite, never going below the checkpoint or applied point.
   - D-02: CRDB's only guard is `Commit <= LastIndex`, which is weaker than SNC G-14(b).

### A4. Snapshot catch-up and replica GC
1. Catch-up below the truncated index:
   - FACT-SRC `pkg/raft/raft.go maybeSendAppend`: "The log probably got truncated at >= pr.Next ... Send a snapshot instead." It requires `pr.RecentActive`.
   - The snapshot is the leader's state at `RaftAppliedIndex`.
2. `applySnapshot` / `writeUnreplicatedSST`:
   - It clears the whole unreplicated RangeID keyspace. One ingested SST then carries the HardState, the RaftReplicaID, `RaftTruncatedState{Index: meta.Index, Term: meta.Term}` and the data.
   - It then asserts `RaftAppliedIndex == Metadata.Index`, or `log.Fatalf`.
   - Floor and applied index are set atomically, with an empty log.
3. `replica_gc_queue.go`:
   - It does a meta RangeLookup and never GCs a current member. FACT-SRC: deleting a member that lags "would just force the use of a snapshot".
   - Removal uses `NextReplicaID` and a tombstone, synced (`replica_destroy.go`).
   - An admitted window: "(1) state machine syncs, (2) OS crashes before (3) sideloaded was able to sync the files removal."
4. SNC R2′ and reseed (INF):
   - A reseed must set floor = applied = seed start atomically. So a byte-for-byte manifest copy must rewrite `retain_from`, as D06_BOUNDARY proposes.
   - The tombstone plus `NextReplicaID` is a monotonic anti-resurrection guard, which bears on R-23 within one store only.

### A5. Durability bugs and performance
- The bugs referenced in code are #40213, #78412, #36262 and #93248. Their content is UNVERIFIED.
- No performance data was found in the files read.

## B. tikv/raft-engine 0.4.2

### B1. Persistent structures
- Append and rewrite queue files, named by `seq`.
- In format V2 the seq is the checksum signature. FACT-SRC: `Some(self.id.seq as u32)`, and "`signature` ***XOR*** `original checksum`". This rejects stale recycled or misplaced content.
- A LogBatch is `header {u56 len | u8 compression | u64 item offset}`, then an entries block plus crc32, then an item batch plus crc32. It is atomic and checksummed.
- **No durable first-index metadata.** `Engine::first_index` reads `memtable.read().first_index()`, which is rebuilt on replay from the entries and the `Command::Compact{index}` records.

### B2. Purge ordering (`engine.rs`, `purge.rs`, `pipe.rs`)
1. `compact_to` appends `Command::Compact` **unsynced**: FACT-SRC `self.write(&mut log_batch, false)`.
2. `purge_expired_files` runs these steps in order:
   1. Write the tombstones to the rewrite queue with sync. FACT-SRC: "AFTER acquiring `append_queue_barrier`, or deletion marks might be lost after restart."
   2. Rewrite the live old entries, with a final `rewrite_impl(..., true)`. The memtable is repointed only after the append and sync.
   3. Compute the purge point: the minimum over the memtables of `min_file_seq`, capped by the `PurgeHook` barrier. The barrier is a per-file refcount of writes not yet applied to the memtable.
   4. `purge_to` deletes or recycles the oldest files first, with no dir fsync.
3. Rotation: the old file is truncated and synced on close. The new header is synced, then the directory: "File header must be persisted." INF: every non-active file is fully durable.
4. INF: purge never touches the active file (the watermark stops at `active_file - 1`). So losing an unsynced Compact only *keeps more* history. This is the same rule as CRDB.

### B3. Recovery (`file_pipe_log/pipe_builder.rs`, `memtable.rs`)
1. `scan_dir` sorts the files by seq, with **no contiguity check and no check for a missing first file**.
2. Replay into the append memtable uses `allow_hole=false`, which panics with "memtable {} has a hole". A missing *middle* file is caught for any region whose entries span it. A missing *oldest* file looks exactly like compaction (INF).
3. RecoveryMode (the default is `TolerateTailCorruption`):
   - **AbsoluteConsistency**: any broken header, batch or entries block returns Err.
   - **TolerateTailCorruption**:
     - A broken header in the last file gives `truncate(0)`.
     - A broken batch in the last file gives `truncate(valid_offset)` plus `sync`.
     - Nuance, FACT-SRC: a broken entries block in the final item of **any** file is truncated. The `if_chain` has no `is_last_file` test.
   - **TolerateAnyCorruption**: every file is opened ReadWrite, and any broken batch truncates that file's suffix.
4. Detects: torn or corrupt batches, stale recycled bytes, a lost middle file for spanning regions, and an append that overwrites compacted entries.
5. Does NOT detect (INF):
   - A lost synced tail that ends on a batch boundary (D-02).
   - Loss of the oldest file.
   - A rollback of the whole directory (R-23).
   - Tail truncation itself is only a `warn!`; the caller is not told.
6. Bugs (DOC, CHANGELOG):
   - 0.4.0: "Fix data loss caused by aborted rewrite operation" and "un-persisted log batch is mistakenly recovered".
   - 0.3.0: "Avoid leaving fractured write after failure by reseeking the file writer".
   - The PR numbers are UNVERIFIED.
7. Performance (DOC):
   - TiKV calls `purge_expired_files` "every 10 seconds by default".
   - 0.4.2 (#347) periodically flushes unsynced rewrite bytes "to avoid I/O jitters".
   - No benchmark figures.

### B4. Implications for SNC (INF)
- O-FLOOR: a floor kept as a log record, plus deletion based on references, avoids the manifest-vs-delete race but cannot detect a missing oldest file. SNC's FL check is stronger. Keep it, with the manifest durable before the delete.
- R2′: raft-engine truncates at recovery only for corruption. Protocol truncation is an overwrite through new appends (`unsafe_truncate_back`). SNC's computed truncation should be a durable, idempotent record, not a recovery-mode side effect.
- D-02 and R-23: neither system has a durable ack boundary or rollback detection independent of the log. Both rely on the quorum. There is no prior art here for G-14(b).
