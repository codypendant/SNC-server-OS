# Audit B (durability / recovery) — subagent report, UNVERIFIED by me yet (spot-check before citing)

## 0. PostgreSQL persistence: SNC has none
- grep over CMakeLists.txt, src/, include/, tools/ for postgres|libpq|pgsql|psycopg: no hits.
- PostgreSQL appears only as a conventional comparison baseline: bench/conventional/run.py, run_replication.py (class Postgres, pg_ctl, pg_basebackup; run_replication.py:125-131); docs/CONVENTIONAL.md:7,17,38-39; docs/STATUS.md:454,455,458; bench/results/*.csv.
- SNC persistence is its own: WAL + checkpoint + A/B superblock on local files (src/core/storage.cpp, engine.cpp, shard.cpp).

## 1. WAL: Partially verified
- Implementation: CRC32C records, block-padded, one flush in flight per stream (storage.cpp:576-640); sri_write with RWF_DSYNC when cfg.sync (native/runtime.cpp:177-181); O_DIRECT if fs allows else buffered (storage.cpp:540-548); durable_lsn advances only in flush_done (storage.cpp:810-826); fallocate or ftruncate (554-555); spare segments renamed without directory fsync (688-708, 795-809, engine.cpp:1052); recovery always starts a new segment (engine.cpp:1165).
- Recovery rules (storage.cpp:343-420): first bad/out-of-sequence record ends a segment (torn_tails); gap between segments fatal (366-368); bad header on non-newest fatal; atomic groups (kRecGroupNext) applied only if complete (400-412).
- Tests: test_crash.cpp (SIGKILL child, 4 rounds, every acked (id,version) present); test_persistence.cpp phases A-F incl. torn tail (~262-290) and lost spare rename (~305-325); test_wal_capacity.cpp C1-C5 with sync=false, direct=false (34-40) so says nothing about disk durability.
- NOT demonstrated: power loss / fsync ordering (docs/STATUS.md:510-511 "power loss is not tested"; FAILURE_MODEL H3/S5 UNPROVEN); real partial sector write; RWF_DSYNC+O_DIRECT+fallocate behaviour; spare-rename ordering under power loss (inferred, untested); WAL EIO/ENOSPC (only handler storage_fatal abort, storage.cpp:816-819; no injection test for WAL path; FAILURE_MODEL S3 UNPROVEN).
- Risks: mid-log corruption in LAST segment silently drops that record and all later acked records, counted only as torn_tails (storage.cpp:386-393; FAILURE_MODEL S6 KNOWN GAP; STATUS.md:487-490); no WAL disk-space accounting; wal_sync=false (storage.h:141, snc.h:373) dev-only but acks still report durable.
- Next: WAL EIO/ENOSPC injection test; mid-segment bit-flip test (or make fail-stop); dm-flakey/dm-log-writes or VM power-cut run; document durable=true under wal_sync=none.

## 2. Acknowledged semantics
- Durability has two values DURABLE (default) and APPLIED (snc.h:24); APPLIED acks before WAL durable; wait LSN set only for DURABLE (shard.cpp:709, 868). Creates always wait for durability (STATUS.md ~480).
- Replication: sync mode adds standby fsync; async loses acked writes on promotion (known, test_async_failover) (STATUS.md:458, FAILURE_MODEL C7).
- "Ack means durable" rests on process-kill model only.

## 3. Checkpoint (fuzzy per-shard parts, manifest, A/B superblock): Partially verified (strongest evidence)
- shard snapshots IDs and start_lsn (shard.cpp:2318-2321), paced walk, DSYNC window W default 4 (snc.h:542-543); part fdatasync under S4 tokens, K=0 default (shard.cpp:2540-2560, sync_tokens.h). Engine::checkpoint (engine.cpp:1395-1549): fsync ckpt dir; write_manifest (tmp, fdatasync, rename, fsync dir; storage.cpp:299-320); bump generation, superblock to alternate A/B file (storage.cpp:238-251); only then delete old checkpoint and covered WAL. Recovery CRC-checks; corrupt parts/manifest are storage_fatal (storage.cpp:422-450, engine.cpp:1004-1008). Replay idempotent from start_lsn.
- Tests: S3 T6/T7/T18 (test_s3_writeback.cpp:622-756, 1047-1075) SIGKILL at in-flight/partial/all-complete/before-commit + injected -EIO and short write (debug_ckpt_fail_write); S4 T5/T6 (test_s4_sync_tokens.cpp:669-760) manifest-written-not-superblock hook (engine.cpp:1464-1471); S2 test_crash_positions (test_s2_pacing.cpp:714); test_persistence B/C/D.
- Gaps: failures injected as return values not real EIO; no test corrupts A/B superblock/manifest/parts (torn/zeroed); read_superblock CRC + generation selection (storage.cpp:200-218) untested directly; checkpoint failure = abort, no degraded serving; superblock write is 4 KiB buffered pwrite + fdatasync (atomicity by CRC and A/B only); checkpoint holds control_mu_ (B1 fixed per STATUS.md:339, test_baseline_ha q3).

## 4. Pacing S2 (src/core/ckpt_pacer.h)
- Controller logic Verified (pure function ckpt_pacer.h:1-15, synthetic traces test_s2_pacing T-controller :264); any durability benefit Implemented, unverified. All numbers are "configurable HYPOTHESIS" (S2_PACING.md s5). test_ckpt_cli.py tests flag parsing only.

## 5. Admission S1 (shard.cpp:282-300 admit): Partially verified
- Refuses E_OVERLOADED when WAL active buffer + op need > wal_buffer_budget or waiter budget exceeded. test_s1_admission.cpp (1626 lines). Beyond max_active_bytes: abort (storage.cpp ~503-511). Disk exhaustion not handled. "follow-up intake backpressure required before S6" status not fully checked.

## 6. S3 writeback: Partially verified
- window_bound 264, memory_bound 337, out_of_order 457 in test_s3_writeback.cpp. Real-disk benefit not shown. O_DIRECT fallback only exercised if test fs lacks it (unknown).

## 7. S4 sync tokens: Partially verified; default K=0 = unlimited (mechanism off by default). Tokens released before abort (sync_tokens.h, shard.cpp:2543-2550).

## 8. Seeding (src/core/seed.cpp): Partially verified. fsync per file, dirs, superblock last (seed.cpp:258-269). Seed-driven tests in test_replication + Python suites; not traced.

## 9. pages.cpp/test_pages.cpp: hot-memory page mode (SMALL/THP/HUGETLB), NOT storage pages.

## 10. Python suites: test_fail_closed.py = migration fail-closed serving (not storage); test_sync_readiness.py = witness promotion gating; test_baseline_*.py = real processes, SIGKILL/SIGSTOP on loopback; test_ckpt_cli.py = CLI parsing only.

## 11. FAILURE_MODEL.md:42-48: G5 recovery barrier partial (P3: node links and replication start before barrier); clean stop takes no checkpoint (engine.cpp:218-243). FAILURE_MODEL S3 "no test injects EIO/ENOSPC" is stale for checkpoint path, still true for WAL path and ENOSPC.

## Cross-cutting hazards (agent-verified claims, to spot-check)
1. WAL/checkpoint/seed I/O failures all abort via storage_fatal; no degraded mode; WAL path has no injection test.
2. Last-segment mid-log corruption silently drops acked records (storage.cpp:386-393).
3. WAL spare rename not directory-synced (storage.cpp:702-704); power-loss behaviour Unknown.
4. wal_sync=false acks report durable.
5. APPLIED acks pre-WAL; async standby promotion loses acked writes.
6. No byte-level corruption tests for superblock/manifest/parts.
7. No disk-full/ENOSPC handling for WAL.
8. All crash evidence is SIGKILL (page cache survives).
9. Recovery reads whole segment containing start_lsn, no seek index.

Classification summary: WAL Partially verified; Checkpoint/recovery/superblock Partially verified; S2 controller Verified (logic)/benefit unverified; S1 Partially verified; S3 Partially verified; S4 Partially verified (default off); Seed Partially verified; PostgreSQL persistence = does not exist; Power-loss durability Unknown.
