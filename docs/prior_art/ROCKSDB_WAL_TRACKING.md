# RocksDB: WAL tracking, MANIFEST/CURRENT, recovery (primary sources)

**Pins.** Code was read with `git clone --depth 1`.
- Main: `v9.7.4`, tag object `5c5934cc`, commit **3c27a3dde0993210c5cc30d99717093f7537916f**.
- Latest: `v11.13.2`, commit **17b624b5c0d41bd42db5cf88aa867f3b26eaf1fa** (2026-10-28).
- Wiki: `rocksdb.wiki.git` @ `98273e83`.
- Commit messages: full-history tree-less clone.
- UNVERIFIED: issue/PR web pages (proxy 403); issue numbers cited only via commit messages/HISTORY.md.

Labels: FACT-SRC, DOC (options.h comments, wiki, HISTORY), INF, HYP.

## M1. `track_and_verify_wals_in_manifest` (MANIFEST WalSet)

**(1) Where (v9.7.4).**
- `db/wal_edit.{h,cc}`: `WalMetadata`, `WalAddition`, `WalDeletion`, `WalSet::AddWal`, `DeleteWalsBefore`, `CheckWals`.
- `db_impl.cc` `MarkLogsSynced`; `db_impl_open.cc` `Recover` (l.756); `version_set.cc` `WriteCurrentStateToManifest`; `db_impl_files.cc`; `db_impl_write.cc` `SwitchMemtable`.
- Default `false` (FACT-SRC options.h:642). Unchanged in v11.13.2.

**(2) Failure mode addressed.** A synced closed WAL that is missing, or shorter than it was when synced, at open. The wiki (DOC) states the problem: "If a WAL is missing or corrupted, RocksDB has no way of knowing this, and will silently recover with corrupted content."

**(3) Persistent structures.** These are MANIFEST VersionEdits:
- `WalAddition{log_number, optional kSyncedSize}`
- `WalDeletion{number}`, meaning "deleting WALs before the specified log number"

The only metadata tag is the size: `kSyncedSize = 2, // Add tags in the future, such as checksum?` (FACT-SRC). There is no WAL checksum and no content hash.

On a MANIFEST roll, the whole WalSet is rewritten along with the deletion floor: "New manifest should rollover the WAL deletion record from previous manifest" (FACT-SRC version_set.cc, fix #10892).

**(4) Ordering.**
- **When a WAL is added (FACT-SRC, MarkLogsSynced).** Only after a sync completes, and only for **inactive** WALs:
  - `if (wal.number < logs_.back().number) { // Inactive WAL ... synced_wals->AddWal(wal.number, WalMetadata(wal.GetPreSyncSize()))`
  - The active WAL only gets `wal.FinishSync()`.
- **Not added on creation (INF).** The wiki says "On creation, write the log number to MANIFEST". v9.7.4 non-test `AddWal(` callers are only MarkLogsSynced and the new-MANIFEST snapshot; none at creation.
- **Sync points that record a closed WAL's size (FACT-SRC).**
  - `SyncWAL()`, then `ApplyWALToManifest`.
  - A write with `sync=true` (db_impl_write.cc:569). This runs `MarkLogsSynced`, then `ApplyWALToManifest`, before the writer is released.
  - The flush path `SyncClosedWals`, then `ApplyWALToManifest` (db_impl_compaction_flush.cc:235).
- **MANIFEST commit (FACT-SRC ProcessManifestWrites).** `AddRecord`, then `SyncManifest`, then (new MANIFEST only) `SetCurrentFile`. The in-memory apply comes after these.
- **Obsoletion (FACT-SRC).** `wal_deletion_edit.DeleteWalsBefore(min_wal_number_to_keep)` is written together with the flush edit. Physical deletion is in `FindObsoleteFiles`/`PurgeObsoleteFiles` and uses `versions_->min_log_number_to_keep()`. That value is updated only inside LogAndApply, after the MANIFEST sync (INF from the code path).
- **Recovery (FACT-SRC).** `CheckWals` runs before replay unless `best_efforts_recovery` is set.

**(5) Detects (FACT-SRC CheckWals).**
- A tracked WAL that has a synced size and is absent: `"Missing WAL with log number: "`.
- An on-disk size below the synced size: `if (log_file_size < wal_meta.GetSyncedSizeInBytes())`, giving "Size mismatch".

**(6) Does NOT detect.**
- **The live/newest WAL.** DOC options.h: "Currently, only syncing closed WALs are tracked. Calling `DB::SyncWAL()`, etc. or writing with `WriteOptions::sync=true` to sync the live WAL is not tracked for performance/efficiency reasons." Commit #10330 (DOC/commit): "recovery ... may fail to detect corruption in synced WALs if the corruption is in the live WAL." So acknowledged sync=true writes in the live WAL can vanish or be shortened undetected while that WAL is live; it is only tracked once it becomes inactive and is synced again (INF).
- **A WAL with no synced size.** It is skipped: `if (!wal_meta.HasSyncedSize()) { ... continue; }` (FACT-SRC).
- **Zeroed or overwritten content at the same file size.** Only `GetFileSize` is compared (FACT-SRC). Corruption inside the file is left to the record CRC and the recovery mode (INF).
- **A MANIFEST that has itself rolled back** (see M4) (INF).

**(7) Performance (commit c8a70ed4, PR #10330, 2022-07-12, db_bench fillsync).**

| State | fillsync median |
|---|---|
| Before tracking every live-WAL sync (#10087) | 1287 ops/s |
| With a MANIFEST write per sync | 629 ops/s |
| After the revert | 1182 ops/s |

A MANIFEST write+sync per synced write roughly **halved** sync-write throughput; hence the revert. Wiki design (DOC) chose "option 3" because "writing to MANIFEST for every synced write can make the write slower".

**(8) Implication for SNC.**
- **D-02(a).** RocksDB *tried* a per-sync durable watermark, measured about 2x cost on sync writes, and reverted. The watermark now covers only closed segments. (a) closes D-02 only if the watermark is written on every acknowledged flush. Expect a second synchronous metadata write per ack (INF), unless it is batched with group commit.
- **O-FLOOR.** RocksDB writes the logical floor (`WalDeletion`) in the same synced MANIFEST edit as the flush result. It deletes physically afterwards, and carries the floor forward into every new MANIFEST. This is the ordering SNC's floor needs (INF).

## M2. `track_and_verify_wals` (predecessor-WAL record; added in 9.11.0)

**(1) Where (v11.13.2).**
- `db/dbformat.h` `PredecessorWALInfo{log_number, size_bytes, last_seqno_recorded}`; `log_writer.cc` `MaybeAddPredecessorWALInfo`; `log_reader.cc` `MaybeVerifyPredecessorWALInfo`; `SwitchMemtable`.
- Marked EXPERIMENTAL, default `false` (DOC). HISTORY 9.11.0 describes it as "a better replacement to `track_and_verify_wals_in_manifest`".

**(2) Failure mode addressed.**
- A "WAL hole where new WAL data presents while some old WAL data not yet obsolete is missing".
- An existing DB that has no WAL at all (DOC options.h; commit 02b41975 / PR #13226 for issue #12488).

**(3) Persistent structures.** The first record of each new WAL is `kPredecessorWALInfoType = 130` (or `kRecyclePredecessorWALInfoType = 131`) (FACT-SRC log_format.h). It holds the previous WAL's number, size and last seqno. Record types with bit 7 set are safely ignorable ("Unknown type of value with the 8-th bit set will be ignored").

**(4) Ordering.**
- **Write (FACT-SRC).** At WAL switch the new WAL records `cur_log_writer->file()->GetFileSize()` and `GetLastSeqnoRecorded()` of the old one. No MANIFEST write is involved.
- **Not on the ack path (INF):** durable only when the new WAL is synced.

**(5) Detects (FACT-SRC).**
- "Missing WAL of log number", "Mismatched predecessor log number", "Mismatched last sequence number", "Mismatched size". These cover a lost, truncated or extended (stale-tail) *predecessor* when a successor exists.
- `"Opening an existing DB with no WAL files"`.

**(6) Does NOT detect.**
- **The newest WAL.** No successor exists, so nothing vouches for its tail (INF). This is the exact D-02 case.
- **Disabled paths (FACT-SRC).** The check is skipped under `kSkipAnyCorruptedRecords` or once `stop_replay_for_corruption_` is set.
- **Maturity.** Commit e12734d5 (in v11.13.2) "Disable track_and_verify_wals temporarily ... as we see some issues". `tools/db_crashtest.py` pins it to 0 with `# TODO(hx235): enable ... after stabalizing`. It is not crash-tested at the pinned tag (FACT-SRC).

**(7) Performance.** PR #13226 reports db_bench fillrandom on /dev/shm, 3 runs, of 310,517 vs 311,469 ops/s, described as "no regression" (commit message). No sync=true numbers are given.

**(8) Implication for SNC.** A self-describing inter-segment chain, close to D-02(b): it proves segment N-1's end, never the last segment's. The newest tail still needs (a) or a per-ack mechanism; even M1+M2 leave the live tail unverified (INF).

## M3. Record format, recovery modes, recycling (log_format.h / log_reader.cc)

**(1) Where (v9.7.4).** `log_reader.cc` `ReadRecord`/`ReadPhysicalRecord`; `db_impl_open.cc` `SanitizeOptions`/`RecoverLogFiles`.

**(3) Format (FACT-SRC).**
- Blocks are 32 KiB.
- Legacy header: CRC(4) + len(2) + type(1).
- Recyclable header: CRC(4) + len(2) + type(1) + **log number (4)**: `kRecyclableHeaderSize = 4 + 2 + 1 + 4`.
- The CRC covers type, log number and payload: `crc32c::Value(header + 6, length + header_size - 6)`.

**(5) Stale-data rejection (FACT-SRC).**
- `if (log_num != log_number_) { ... return kOldRecord; }` makes stale recycled data read as EOF (except under kSkipAny).
- The number is truncated to 32 bits (INF from `DecodeFixed32`), so the effective per-file nonce is 32 bits.

**(4) Recovery modes (DOC options.h; FACT-SRC reader).** The default is `kPointInTimeRecovery`.
- **kTolerateCorruptedTailRecords** tolerates "the last record ... incomplete" and "Zeroed bytes from preallocation". It refuses to open on other corruption.
- **kAbsoluteConsistency** expects no corruption.
- **kPointInTimeRecovery** stops at the first corruption. It fails on an IOError ("This likely mean loss of synced WAL"). It aborts if a CF's SST is ahead of the corrupted WAL: `"SST file is ahead of WALs"`.
- **kSkipAnyCorruptedRecords** ignores everything.

**(6) Gaps.**
- **Zeroed region (FACT-SRC).** `kZeroType && length == 0` gives `buffer_.clear(); return kBadRecord;`. Outside a fragmented record, `kBadRecord` reports nothing and reading continues with the next block. So a zeroed run is skipped silently, in every mode, without a corruption report (FACT-SRC: `log_reader.cc` lines 274 and 524-531 at v9.7.4; the `kBadRecord` handling does not depend on the recovery mode; relabelled from INF after review). Whether later records are then replayed past the hole is HYP: I did not run it.
- **Truncation on a record boundary.** HISTORY 6.16.0 (DOC): "Gaps are still possible when WALs are truncated exactly on record boundaries; for complete protection, users should enable `track_and_verify_wals_in_manifest`."
- **Recycling (FACT-SRC).** Forced to 0 with kTolerate/kAbsolute, because it "cannot differentiate between this and a real corruption"; re-enabled for PIT in 9.1.0 (HISTORY). Commit d33d25f9 (#12918) disables recycling in crash tests over a "recovery data loss" reproducer: safe only "if the sequence numbers between them are adjacent" (issues #12403/#12639 UNVERIFIED).

**(8) Implication for SNC.**
- The per-segment number in every frame, inside the CRC, is exactly D-02(b)'s nonce. It rejects stale bytes cleanly.
- On its own it cannot tell an "unwritten tail" from a "lost acknowledged tail". Both look like EOF/old-record. RocksDB's own commit says another mechanism is needed (ideas a2/b1/b2 in #12918).
- SNC should treat all-zero frames as damage when they fall below any watermark. RocksDB does not (INF).

## M4. MANIFEST/CURRENT selection and rollback (R-23 analog)

**(1)(4) Where and ordering (v9.7.4).**
- `file/filename.cc` `SetCurrentFile`: write `tmp` with sync=true, then `RenameFile(tmp, CURRENT)`, then fsync the directory (FACT-SRC).
- `VersionSet::GetCurrentManifestPath` reads CURRENT and checks only that it ends in a newline and parses as a descriptor name (FACT-SRC).
- The MANIFEST is replayed with `log::Reader(..., true /* checksum */, 0 /* log_number */)`. `ReadRecord`'s default mode is `kTolerateCorruptedTailRecords` (FACT-SRC log_reader.h:79).

**(6) Rollback protection.**
- **None found in source (INF).** There is no generation counter outside the MANIFEST, no external anchor, and no check that CURRENT names the newest MANIFEST.
- **Lost MANIFEST tail (INF)** recovers silently to the previous edit. Only side effects catch some rollbacks (files missing vs. the old state, "SST file is ahead of WALs"), like SNC's FL-floor side effect.
- **`verify_sst_unique_id_in_manifest`** (default true, DOC) detects an SST "overwritten or misplaced" relative to the MANIFEST. It does not detect MANIFEST rollback.
- **`paranoid_checks`** (default true, DOC): "aggressively check consistency"; nothing rollback-specific.
- **`best_efforts_recovery`** (DOC) deliberately recovers to an older point-in-time version, "possibly from an older MANIFEST file". It also skips `CheckWals` (FACT-SRC). It is a rollback *tool*, not a protection.

**(8) Implication for R-23.** RocksDB has no answer to metadata rollback. A single CURRENT pointer plus an append-only CRC'd MANIFEST has the same silent-fallback class as SNC's A/B copies (INF).

## Known bugs (HISTORY.md v11.13.2 / commit msgs; issue pages UNVERIFIED)
7.2.0 #9715/#9766 false "Missing WAL"; 7.4.0 #10087 SyncWAL on sole WAL untracked; #10185 tracked size > synced bytes; 7.5.0 #10330 revert; 7.10.0 #10892 WAL floor not rolled into new MANIFEST; #12731 checkpoint linked unsynced WAL; 9.2.0 recycled WAL replayed stale records; #12918 recycle recovery data loss; #13405/#13869 `track_and_verify_wals` pulled from stress tests.
