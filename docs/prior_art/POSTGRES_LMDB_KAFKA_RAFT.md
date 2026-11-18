# Prior art for D-02 / R-23 / O-FLOOR: PostgreSQL, LMDB, Kafka, Raft, CTRL

**Pins (fetched 2026-11-18):** PostgreSQL `REL_17_2` (release notes from `REL_11_0`, `REL_11_22`); LMDB `LMDB_0.9.31` (github LMDB/lmdb, `libraries/liblmdb`); Kafka `3.8.0`; Raft dissertation `ongardie/dissertation` master @`0c10ea1b` (2016-05-24; untagged); etcd-io/raft `v3.6.0`.
**Blocked (403):** raft.github.io, usenix.org, wisc.edu, cwiki.apache.org, arXiv, ACM, Semantic Scholar via curl. KIP-101/279 and the CTRL paper were read through WebFetch, where a helper model extracts short quotes. Those are tagged "(WF)" and are **not byte-verified**.
**Labels:** FACT-SRC (pinned source, quoted), DOC, PAPER, INF, HYP.

## 1. PostgreSQL 17

### 1a. pg_control: one copy, CRC, sector atomicity
- **Where:** `pg_control.h` `ControlFileData`; `controldata_utils.c` `update_controlfile`/`get_controlfile_by_exact_path`; `xlog.c` `ReadControlFile`/`WriteControlFile`/`UpdateControlFile`.
- **Structures:** `global/pg_control`, 8192 B, zero-padded. FACT-SRC: `/* CRC of all above ... MUST BE LAST! */`; `#define PG_CONTROL_MAX_SAFE_SIZE 512`; static assert `"pg_control is too large for atomic disk writes"`; the comment says updates must be "atomic writes ... can't be more than one disk sector".
- **Write:** `UpdateControlFile` → `update_controlfile(DataDir, ControlFile, true)`: overwrite in place, then `pg_fsync`. "All errors issue a PANIC" (FACT-SRC). No temp+rename and no generation field (FACT-SRC: none in the struct; `time` is a wall-clock stamp).
- **Detects:** a torn or corrupt file. `ereport(FATAL, ... "incorrect checksum in control file")` (FACT-SRC). **No second copy and no fallback.** The frontend's 10x retry ("same bad CRC twice in a row") only covers reading while the server writes (FACT-SRC).
- **Does NOT detect:** a lost write that leaves an older, CRC-valid file. Nothing marks a copy as current (INF).
- **Perf:** none.

### 1b. Checkpoint authority / O-FLOOR order
- **Order in `CreateCheckPoint` (FACT-SRC, line order):** `CheckPointGuts(checkPoint.redo, flags)` → `XLogInsert(RM_XLOG_ID, ...)` → `XLogFlush(recptr)` → `ControlFile->checkPoint = ProcLastRecPtr` → `UpdateControlFile()` → `KeepLogSeg` → `RemoveOldXlogFiles`. WAL is deleted only after the durable authority points past it.
- **Selection (`xlogrecovery.c` `InitWalRecovery`):** `backup_label`, if present, overrides pg_control. FACT-SRC FATAL "could not locate required checkpoint record", with the hint "removing ... backup_label will result in a corrupt cluster if restoring from a backup". Otherwise `CheckPointLoc = ControlFile->checkPoint`; if that is unreadable, `PANIC "could not locate a valid checkpoint record"`. Comment: "We used to attempt to go back to a secondary checkpoint record here ... We now just fail" (FACT-SRC).
- DOC (release-11, commit `4b0d28de0` "Remove secondary checkpoint"): "Retain WAL data for only a single checkpoint ... Previously, WAL was retained for two checkpoints."
- **Rolled-back pg_control:** if the older checkpoint's WAL still exists, replay starts earlier, which is benign (page-LSN idempotent redo). If that WAL was recycled, the page checks (1d) fail → PANIC. Either way it fails closed or is harmless (INF).

### 1c. minRecoveryPoint = candidate (a), archive mode only
- FACT-SRC: it is "updated to the latest replayed LSN whenever we flush a data change during archive recovery ... we mustn't start up until we reach X again. **Zero when not doing archive recovery.**" It is advanced by `UpdateMinRecoveryPoint` → `UpdateControlFile`.
- FACT-SRC (`xlog.c`): `EndOfLog < LocalMinRecoveryPoint` → FATAL "WAL ends before consistent recovery point" / "WAL ends before end of online backup". This is **only under `if (ArchiveRecoveryRequested || ControlFile->backupEndRequired)`**.
- Does NOT apply to local crash recovery.

### 1d. End of WAL; stale segments; lost tail
- Crash recovery reads at `emode = LOG`, so the first invalid record = end of WAL (FACT-SRC: `ReadRecord(xlogprefetcher, LOG, ...)` in `xlogrecovery.c`, lines 1725 and 1832 at REL_17_2; `ReadRecord` returning NULL ends replay. `emode_for_corrupt_record` only lowers repeated messages to DEBUG1; corrected after review).
- Page/record checks (FACT-SRC `xlogreader.c`):
  - `xlp_sysid` → "WAL file is from different database system";
  - `xlp_pageaddr != recptr` → "unexpected pageaddr" ("typically fails when an old WAL segment is recycled");
  - `xlp_tli` → "out-of-sequence timeline ID";
  - `xl_prev` → "incorrect prev-link";
  - record CRC.
- Contrecord: when the end of WAL cuts a multi-page record, `missingContrecPtr` is set and `CreateOverwriteContrecordRecord` writes `XLOG_OVERWRITE_CONTRECORD` (FACT-SRC).
- **A lost acked tail is NOT detected at recovery.** It looks like a clean end of WAL (INF; no watermark is compared in crash recovery). The only late, indirect signal is FACT-SRC `elog(ERROR, "xlog flush request %X/%X is not satisfied --- flushed only to %X/%X")`, raised when a data page with a too-new LSN is written back. It is ERROR and not PANIC from bufmgr. The comment attributes it to a "corrupted LSN" (FACT-SRC).

### 1e. fsyncgate
- DOC (Release 11.2, commit `9ccdd7f66`, 2018-11-19): "By default, panic instead of retrying after fsync() failure ... If we reissue the fsync() request it will succeed, but in fact the data has been lost". It added `data_sync_retry` (default off = PANIC, DOC config.sgml 17).
- FACT-SRC `issue_xlog_fsync`: "/* PANIC if failed to fsync */".

**SNC implications.**
- Self-describing pages (`xlp_pageaddr`/`xlp_sysid`/`xlp_tli`) are candidate (b). They defeat stale or recycled bytes, but not lost writes.
- PG does not solve D-02 locally. Its watermark pattern (a) exists but guards only archive replay (INF).
- O-FLOOR: copy PG's order.
- Checkpoint authority: one pointer, with PANIC rather than fallback.
- R-23: SNC's A/B is wider than PG's single copy, because A/B always offers a valid older copy (INF).

## 2. LMDB 0.9.31
- **Where:** `mdb.c` `mdb_env_pick_meta`, `mdb_env_read_header`, `mdb_env_write_meta`, `mdb_txn_commit`, `mdb_find_oldest`.
- **Structures:** meta pages 0/1. `volatile txnid_t mm_txnid; /**< txnid that committed this page */` is the last field. **No checksum:** `grep -ci 'checksum\|crc' mdb.c` = 0 (FACT-SRC).
- **Order (FACT-SRC):** `mdb_page_flush` → `mdb_env_sync` → `mdb_env_write_meta`. The slot is `toggle = txn->mt_txnid & 1`. One `pwrite` covers `mm_mapsize`..`mm_txnid` via `me_mfd` ("writing to it also syncs to disk") unless NOSYNC/NOMETASYNC. On error it writes the old txnid back and sets `MDB_FATAL_ERROR`.
- **Pick (FACT-SRC):** `return metas[ metas[0]->mm_txnid < metas[1]->mm_txnid ];`. `read_header` rejects a page only on bad magic or version.
- **Lost/torn newer meta:** pages freed by txn T are reused only if `T < oldest`, with `oldest = txn->mt_txnid - 1` or lower (FACT-SRC). So txn N never overwrites txn N-1's tree, and falling back is consistent but **silently drops an acked commit** (INF). A torn meta write is not detectable. Correctness relies on the meta being smaller than a sector (INF). A garbled txnid could win the pick (HYP).
- **DOC (lmdb.h):** NOMETASYNC "preserves the ACI ... but not D"; "a system crash may undo the last committed transaction". NOSYNC: "can corrupt the database or lose the last transactions"; it keeps integrity only "if the filesystem preserves write order and the MDB_WRITEMAP flag is not used".
- **Perf:** none.
- **SNC:** this is SNC's A/B scheme minus the CRC, and R-23 by construction. It verifies the open point in D06_STORAGE_METADATA §92: the pick is txnid comparison only.

## 3. Kafka 3.8.0

### 3a. Local recovery
- **Where:** `LogManager.loadLogs`, `LogLoader.recoverLog`, `CleanShutdownFileHandler`.
- **Structures (FACT-SRC):** `".kafka_cleanshutdown"` (JSON `version`, `brokerEpoch`), deleted on load; `"recovery-point-offset-checkpoint"`. Checkpoints are written temp → `flush()` → `getFD().sync()` → `atomicMoveWithFallback`.
- **Recovery (FACT-SRC):** `if (!hadCleanShutdown) { val unflushed = segments.values(recoveryPointCheckpoint, Long.MaxValue)`. It re-validates each segment; on the first bad batch it logs "Corruption found in segment ..., truncating" and deletes all later unflushed segments. Segments below the recovery point are trusted (index sanity only). The recovery point advances "only ... when the log is flushed".
- **Detects:** torn tails above the recovery point. **Does NOT detect:** a lost tail, or damage below the recovery point (INF).
- **fsync (DOC, `TopicConfig`):** "we recommend you not set this and use replication for durability". FACT-SRC: `LOG_FLUSH_INTERVAL_MESSAGES_DEFAULT = Long.MAX_VALUE`, `LOG_FLUSH_SCHEDULER_INTERVAL_MS_DEFAULT = Long.MAX_VALUE`. **The durability boundary is replication.** Correlated power loss of the whole ISR can lose acked data (INF).

### 3b. Leader epochs
- **FACT-SRC `LeaderEpochFileCache`:**
  - It holds `epoch → startOffset`. `endOffsetFor` returns "the start offset of the first Leader Epoch larger than the Leader Epoch requested".
  - `assign` flushes synchronously ("the checkpoint file always has no missing entries").
  - Truncation flushes asynchronously "To avoid fsync latency" ("could be huge on a disk glitch"). Stale epochs after an unclean shutdown are fixed by "another truncateFromEnd call on log loading".
- **DOC (WF) KIP-101:**
  - HW-based truncation loses data: "Message m2 has been lost permanently".
  - With async flush, "the replicas can diverge".
  - Fix: "stamping every message with the LeaderEpoch", "[LeaderEpoch => StartOffset]" vector, OffsetForLeaderEpoch.
- **DOC (WF) KIP-279:** divergence remained "in spite of the improved truncation logic from KIP-101" under fast failover. The response now carries the epoch, and the follower iterates back to "the largest common log prefix, where both offsets and epochs match". Unclean election gets no special handling.
- **FACT-SRC `AbstractFetcherThread`:** `truncateToEpochEndOffsets` / `truncateOnFetchResponse` (diverging epoch). The fallback is "Truncating partition ... due to local high watermark".
- **SNC:** term per record plus an epoch→start table *repairs* a lost tail from a peer. It never *detects* it locally (INF).

## 4. Raft
- **PAPER (thesis `basicraft/consensus.tex`):** "each server persists its current term and vote ... Each server also persists new log entries before they are counted towards the entries' commitment". The commit index may reset to zero.
- **PAPER:** "**If a server loses any of its persistent state, it cannot safely rejoin the cluster with its prior identity** ... If a majority ... loses its persistent state ... log entries may be lost". Fault model: "Servers fail by stopping and may later restart from stable storage" (`correctness.tex`). So Raft assumes no silent loss and detects none (INF).
- **PAPER (`compaction/memsnapshot.tex`):** the snapshot keeps the "last included index (5) and term (3)". LogCabin snapshots are written temp+rename "when writing is complete and has been flushed to disk". A checksum is only "may also be beneficial".
- **FACT-SRC (etcd `README.md`):** "Entries first, then HardState and Snapshot"; "no messages be sent until the latest HardState has been persisted to disk, and all Entries written by any previous Ready batch".
- **FACT-SRC (etcd `log.go` `commitTo`):** `Panicf("tocommit(%d) is out of range [lastIndex(%d)]. Was the raft log corrupted, truncated, or lost?")`. This is the only lost-tail detector, and it is remote-triggered: it fires only if the leader's commit index exceeds the local log (INF).
- **SNC:** a per-entry term plus a snapshot (index, term) equals SNC `prev_term` (D06_STORAGE_METADATA:166). A node that silently lost an acked tail can still win a vote with a stale "up-to-date" log, so a committed entry can be lost (INF; CTRL demonstrates the class).

## 5. CTRL: Protocol-Aware Recovery (Alagappan et al., FAST '18), PAPER (WF)
- "We introduce protocol-aware recovery (PAR) ... exploits protocol-specific knowledge". Implemented in LogCabin and ZooKeeper.
- **§3.3.3:**
  - "Most log-based systems conflate these two cases: they treat a mismatch as a crash".
  - A small checksummed *persist record* per entry "can be atomically written and thus cannot" be crash-corrupted. The entry is not ordered before it, which saves an fsync.
  - **For the last entry, "we cannot determine whether it was a crash or a corruption"**. It is marked faulty and handed to distributed recovery.
- **§3.3.4 / Table 2:** identifiers are stored apart, because a misdirected write "can corrupt both the item and its identifier". The fault model includes "misdirected and lost writes".
- **§3.4.3:** one "have" → fix the entry. A majority of "dontHave" → the entry is uncommitted; discard it and its suffix. "haveFaulty" → wait. §3.4.1: a node with faulty entries can still be leader.
- **§3.3.1:** term/vote is never taken from peers ("could violate safety"). CTRL keeps **two local copies; "if one copy is faulty, the other copy is used"**. That is SNC's A/B shape, and no rollback detector appears in the quotes fetched (INF: same R-23 gap; UNVERIFIED whether the full text addresses it).
- **§3.5:** faulty snapshots "cannot be discarded" and are fetched from peers. §3.2: if all copies are faulty, the system "will correctly remain unavailable".
- **Perf §5.2:** "8%-10% for 32 clients on disks"; "4% in the worst case" on SSDs; log recovery 1.2 ms / 7 KB vs 1.24 s / 32 MB for stock.
- **SNC D-02:** the last flush is never locally decidable from frames alone, even with per-entry identifiers. Closing it needs a durable bound written after the data sync and before the ack (a), or a quorum query (INF).

## Cross-system conclusions for SNC
- **D-02:** none of the five detects a lost acked tail locally from log contents. Each treats it as a clean end. Frame identity (PG pageaddr, CTRL identifiers, SNC nonce (b)) defeats stale and torn data, not lost writes. Detection needs (a), a pre-ack durable watermark (PG's minRecoveryPoint pattern), or peers (Kafka epochs, CTRL, etcd panic) (INF). That points to (c), with (a) as the detector. There is no published performance number for (a). CTRL's per-entry identifiers cost 4-10% (PAPER WF).
- **R-23:** LMDB, CTRL metainfo, and SNC all use A/B by counter. None detects rollback. PG has one copy and deliberately removed its older-checkpoint fallback (PG11). Rollback is caught only when it points at already-reclaimed WAL (PG PANIC ≈ SNC FL-floor). The only remedies seen are external anchors (a peer's term or epoch) (INF).
- **O-FLOOR:** PG's order: flush the checkpoint record → fsync the authority → remove segments below the durable floor (FACT-SRC).
- **Checkpoint authority:** a single durable pointer. An unreadable target means FATAL/PANIC, never a silent fallback (FACT-SRC + DOC).
