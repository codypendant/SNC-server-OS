# etcd: consistent index, snapshot/WAL ordering, WAL tail (research notes)

**Sources (pinned).** Module zips from proxy.golang.org (these are the released tag contents): `go.etcd.io/etcd/server/v3` @ v3.5.0, v3.5.2, v3.5.3, v3.5.6, v3.6.0; `tests/v3`, `etcdutl/v3`, `client/pkg/v3` @ v3.6.0; `go.etcd.io/raft/v3` @ v3.6.0 (the version etcd v3.6.0's go.mod uses). **Unpinned DOC:** `CHANGELOG/CHANGELOG-3.5.md`, `CHANGELOG-3.6.md` and `Documentation/postmortems/v3.5-data-inconsistency.md` were fetched from raw.githubusercontent `main` on 2026-11-18, because the tag paths returned 404. github.com and api.github.com returned 403, so no issue or PR page could be read.
**UNVERIFIED:** PR numbers #13844, #13854 and #13903 (no source names them). The changelog names **#13908** instead.

## 1. v3.5.0–v3.5.2 consistent-index atomicity bug (#13766)

1. **Path, tag.** Buggy at v3.5.2: `etcdserver/server.go applyEntryNormal`, `etcdserver/server.go backendHooks.OnPreCommitUnsafe`, `mvcc/backend/batch_tx.go batchTxBuffered.commit`. Fixed in v3.5.3, and the fix carries into v3.6.0 (`storage/hooks.go`, `server.go:2553 getTxPostLockInsideApplyHook`).
2. **Failure mode.**
   - DOC (postmortem): "Code refactor in v3.5.0 resulted in consistent index not being saved atomically". It was introduced by PR #12855 (backend hooks) and released in v3.5.0.
   - FACT-SRC (v3.5.2): apply first set the shared in-memory CI, `s.consistIndex.SetConsistentIndex(e.Index, e.Term)`. Then every batch commit persisted it through the hook, `bh.indexer.UnsafeSave(tx)` (in `OnPreCommitUnsafe`). Commits also fire from the periodic loop (`defaultBatchInterval = 100 * time.Millisecond`, `b.batchTx.Commit()`).
   - DOC: a crash after CI=N was persisted but before N's writes meant "etcd would skip executing changes from failed apply workflow".
3. **Persistent structures.** bbolt `meta` bucket keys `consistent_index` and `term`. FACT-SRC (v3.5.0 `buckets/bucket.go`): `MetaTermKeyName = []byte("term")`. So the term was already persisted in v3.5.0; v3.6 did not add it.
4. **Ordering of the fix (FACT-SRC, v3.5.3).**
   - Apply now stages the index only: `s.consistIndex.SetConsistentApplyingIndex(e.Index, e.Term)`. The comment says it is "not ready to be persisted yet".
   - It is promoted inside the apply's own write lock, `batchTx.LockInsideApply()` → `t.backend.txPostLockInsideApplyHook()` → `if applyingIdx > s.consistIndex.UnsafeConsistentIndex() { s.consistIndex.SetConsistentIndex(...) }`.
   - Periodic commit takes the same lock (`Commit(){ t.lock(); t.commit(false) ...}`).
   - INF: so CI=N can reach bbolt only in the same bbolt transaction as entry N's writes, or a later one.
   - For entries that never take the lock (no v3 txn), CI is set directly in a `defer` ("The txPostLock callback will not get called in this case").
   - Outside-apply callers must use `LockOutsideApply()`. With ETCD_VERIFY set, the stack checks `ValidateCalledInsideApply` / `ValidateCalledOutSideApply` enforce this ("Called outside of APPLY!").
   - Related fix, also v3.5.3 (FACT-SRC, CHANGELOG #13933): `s.consistIndex.SetBackend(newbe)` moved before lessor recovery. Otherwise "the new consistent_index value coming from snapshot is overwritten by the old value".
5. **Detects.** The bug was never detected locally. DOC: "For single member cluster it is totally undetectable. There is no mechanism or tool for verifying that state database matches WAL." In multi-member clusters only the `HashKV` corrupt check can catch it, and it "might fail causing the check to pass".
6. **Does NOT detect.** The fix prevents the hazard; it does not detect it. A local metadata CI that runs ahead of the applied data is still invisible on restart.
7. **Performance data.** None found. Only the batching constants: 100 ms interval, `defaultBatchLimit = 10000`.
8. **SNC.**
   - INF: this is exactly SNC's single-metadata-record risk. A progress pointer persisted by a background flush that does not hold the apply's write lock can claim work that is not durable.
   - Rule: persist "applied through N" only atomically with, or after, N's effects.
   - The fix needed a lock discipline plus runtime stack assertions; DOC: "took almost 2 weeks and multiple tries".

## 2. Snapshot / applied index / WAL durability coordination (v3.6.0)

1. **Path.** `etcdserver/raft.go raftNode.start` (Ready loop); `storage/storage.go SaveSnap/Release`; `etcdserver/server.go snapshot()`; `etcdserver/bootstrap.go recoverSnapshot`, `openWALFromSnapshot`; `storage/backend.go RecoverSnapshotBackend`; `etcdserver/api/snap/snapshotter.go LoadNewestAvailable`; `server/verify/verify.go`.
2. **Ready ordering (FACT-SRC).**
   1. Committed entries go to apply (`r.applyc <- ap`) first.
   2. The leader sends `r.transport.Send(...)` before its own disk write ("leader can write to its disk in parallel", raft thesis 10.2.1).
   3. If there is a snapshot, `SaveSnap` runs before `Save` ("Must save the snapshot file and WAL snapshot entry before saving any other entries or hardstate").
   4. `r.storage.Save(rd.HardState, rd.Entries)`. `WAL.Save` writes the entries, then the state, and fsyncs iff `raft.MustSync(...)`.
   5. With a snapshot: `Sync()` → `notifyc` → `ApplySnapshot` → `Release`. Without one: the follower sends its messages only after Save (`raftBeforeFollowerSend`).
   - INF: followers therefore ack only after fsync.
   - INF: apply runs in parallel with the WAL write, so bbolt CI can run ahead of the local WAL after a crash. DOC (CHANGELOG main): "Fix `--force-new-cluster` permanently bricking a member whose backend consistent index is ahead of its WAL" (#22688, v3.5.36 TBC; #22686, v3.6).
3. **Single-node lost write (#14370; DOC: CHANGELOG v3.5.5 "Durability API guarantee broken in single node cluster", #14424).**
   - FACT-SRC v3.5.6 `raft.go`: added `shouldWaitWALSync(rd)`, with the comment "it might run into a situation of data loss when the etcd crashes immediately after responding to the client and before the boltDB and WAL successfully save".
   - v3.6.0: helper absent; raft v3.6.0 `appendEntry` self-acks "after these entries have been written to stable storage" (FACT-SRC). INF: the guard moved into raft.
4. **Snapshot write (FACT-SRC, `storage.go`).**
   - Snap file, then WAL snapshot record ("prevents a WAL snapshot entry from having no corresponding snapshot file").
   - Before creating the snapshot, `s.KV().Commit()` runs ("This guarantees that Backend's consistent_index is >= index of last snapshot"). A verify-only check then runs: `consistent_index(%d) is older than snapshot index`.
5. **Retention.**
   - `ReleaseLockTo(index)` "releases the locks, which has smaller index than the given index except the largest one among them".
   - The purge loop (`client/pkg/fileutil/purge.go`) does `TryLockFile` and `break`s on a locked file before `os.Remove`. So the purge cannot delete WAL segments still locked by the WAL. FACT-SRC; that lock acts as the deletion floor.
6. **Restart (FACT-SRC).**
   - `wal.ValidSnapshotEntries` collects WAL snapshot records and keeps those with `s.Index <= state.Commit`.
   - `LoadNewestAvailable` then picks the newest snap file whose (Term, Index) matches one of them. Unreadable snap files are renamed `.broken` and skipped.
   - `RecoverSnapshotBackend` replaces the db with `snap.db` if `snapshot.Metadata.Index > consistentIndex`. If CI < snapshot index and CI≠0, bootstrap fails: `database file ... does not match with snapshot`.
   - Replay skips entries with index ≤ CI (v3.5.3 `if e.Index > index { ... ApplyBoth }`).
7. **Verify package** (`server/verify`, present in v3.5.3 and v3.6.0).
   - Checks `backend.ConsistentIndex ... must be <= WAL.HardState.commit`, the same for term, and `>= last snapshot index`. `ExactIndex` is for backups.
   - Runs only at `Close()` / etcdutl restore and only if env `ETCD_VERIFY` is set (FACT-SRC: `envVerify = "ETCD_VERIFY"`). INF: a test gate, not a production startup check. Corrupt-check flags: DOC only, source not read.
8. **SNC.**
   - etcd's restart chooses a checkpoint only if a durable log record names it *and* it is ≤ the durable commit. Orphan checkpoint files are tolerated. A data store behind the checkpoint is a hard error. A data store ahead of the log is tolerated, which is the source of #22688.
   - For O-FLOOR: segment deletion is gated by a lock released only after the snapshot record is durable. It is not gated by a value in a manifest.

## 3. WAL tail handling (v3.6.0 `storage/wal`)

1. **Path.** `decoder.go decodeRecord/isTornEntry`, `wal.go ReadAll`, `repair.go Repair`, `encoder.go`, `wal.go cut`.
2. **Framing.** 8-byte length with padding bits, then a protobuf Record with a CRC.
   - FACT-SRC: the CRC is *cumulative*. Encoder: `e.crc.Write(rec.Data); rec.Crc = e.crc.Sum32()`. Each segment starts (`cut`) with a `CrcType` record carrying `prevCrc`, which chains CRCs across files. A mismatch returns `ErrCRCMismatch`.
3. **Tail cases (FACT-SRC).**
   - A length of `0` or EOF is treated as "hit end of file or preallocated space": the reader moves to the next file. In the last file this is clean `io.EOF`.
   - A record whose length exceeds the remaining file gives `ErrUnexpectedEOF`.
   - A CRC or unmarshal failure where some sector-sized chunk is all zero (`isTornEntry`, last file only) gives `ErrUnexpectedEOF`. Otherwise it is a hard CRC error.
   - In write mode, after a clean EOF, `ReadAll` zeroes the rest of the file: "since the records were never fully synced ... it's safe to zero them".
   - Index gaps are caught: `ErrSliceOutOfRange` ("prevent panic: runtime error: slice bounds out of range").
4. **Repair.** Bootstrap repairs only `ErrUnexpectedEOF`, and only once ("we can only repair ErrUnexpectedEOF and we never repair twice"). `Repair` copies the last file to `.broken`, `Truncate(lastOffset)`, then `Fsync`. Any other error is fatal.
5. **Detects.** Torn last record, mid-log bit flips (CRC), cross-segment splicing (chained CRC), and gaps in entry indexes.
6. **Does NOT detect.**
   - INF: a lost *acknowledged* tail is indistinguishable from an unwritten one. A zeroed tail after the last good record reads as clean EOF. A zeroed suffix of the last file plus a torn record is "repaired" by truncation.
   - No durable watermark of the last fsynced offset or index is kept outside the WAL. `HardState.Commit` lives inside the same WAL.
   - INF: a zeroed region in a *non-last* segment ends it early; the next segment's chained CrcType record or index continuity should then fail.
   - etcd relies on raft peers to re-supply a lost tail; single-node has no backstop.
7. **Performance.** None. `warnSyncDuration = time.Second` is a log threshold only.
8. **SNC (D-02).**
   - etcd does not solve D-02 locally. It accepts a truncated tail and depends on quorum replication.
   - The chained CRC plus a per-segment starting-CRC record detects *splices*, but not *truncation*.
   - To detect a lost acknowledged tail, SNC needs something etcd lacks: a durable watermark written after the flush (outside the log), or a self-describing flush frame that a later frame or the metadata references. etcd's design is evidence that log-internal CRCs alone are insufficient.

## 4. Tests protecting the behaviour (tests/v3 @ v3.6.0)

- **Track record** (`tests/robustness/README.md`): #13766 reproduces via `make test-robustness-issue13766` (`makefile.mk`: `--count 100` against a v3.5.2 failpoint build); #14370 "Single node cluster can loose a write on crash" against v3.5.4; #14685 crash during defrag.
- **Scenarios** (`robustness/scenarios/scenarios.go`, FACT-SRC):
  - `Issue13766`: `failpoint.KillFailpoint`, `HighTrafficProfile`, `WithSnapshotCount(100)`.
  - `Issue14370`: `RaftBeforeSavePanic`, `WithClusterSize(1)`.
- **gofail crash points** (`robustness/failpoint/gofail.go`; markers `// gofail: var X struct{}`): `raftBefore/AfterSave`, `raftBefore/AfterSaveSnap`, `raftAfterWALRelease`, `commitBefore/AfterPreCommitHook`, `beforeCommit`, `defrag*`, `compact*`. `walBeforeSync` / `walAfterSync` are also in the source.
- **Checker:** `robustness/validate/operations.go`: `porcupine.CheckOperationsVerbose(model.NonDeterministicModel, ...)`.
- **Gap.**
  - INF: the failpoints are process kills and panics. No zeroed-sector or fsync-lie injection seen, so media tail loss (AFR-G-14) is untested.
  - DOC (postmortem): reproduced only because "/tmp mounted to standard disk".

## Implications for SNC (summary)

- **Metadata atomicity and checkpoint recovery:** see §1.8 and §2.8.
- **R-23:** etcd has no A/B metadata. Its snapshot choice cross-checks the snapshot files against WAL records, and its CI cross-checks against the snapshot index. That gives two independent witnesses. SNC can do the same: validate the chosen A/B copy against a monotonic value held elsewhere (log or checkpoint). Otherwise rollback to an older valid copy is undetectable, the same class as etcd's single-node "totally undetectable" case.
- **D-02:** etcd provides no local solution. Treat etcd as a counter-example, not a design to copy.
