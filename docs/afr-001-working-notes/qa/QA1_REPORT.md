# QA1 independent verification: WAL recovery, sessions, cross-shard ordering

Repo `/home/user/snc-server-os` at `24006ee` (read-only; `git status` shows only untracked docs that others wrote).
Build used: `scratchpad/build` (RelWithDebInfo `libsnc_core.a`, `snc_server`). I did not read `scratchpad/recon/` or `scratchpad/exp/`.
Experiment sources in this directory: `wal_exp.cpp`, `sess_exp.cpp`, `order_exp.cpp`. Raw output: `wal_runs.out`,
`mid_ckpt.out`, `sess.out`, `order.out`, `srv.out`, `srv2.out`, `admin.out`. I deleted every data directory and stopped every process I started.
Build line: `g++ -std=c++20 -O1 -g -I include -I tests -I src X.cpp build/libsnc_core.a -lmimalloc -lxxhash -luring -lpthread`.

The engine-level experiments call the public `Engine` API: 1 shard, `wal_sync=true`, `wal_direct=true`, 1 MiB segments.
Every create returned `OK` with `durable=1` before the stop.

## Code under test (verified line numbers)

- `src/core/storage.cpp:362-437` `scan_stream`. When `check_record` returns 0 and the rest of the block is not zero,
  it does `++out.torn_tails; break;` (`:398-400`). A 4 KiB block that is entirely zero and starts on a block boundary
  ends the segment **without** incrementing anything (`:393-395`). An LSN hole gives `++torn_tails; break` (`:409-411`).
  The `break` leaves only the inner loop, so the outer loop moves on to the next segment.
- `storage.cpp:383-385`: if the next segment's `first_lsn` differs from the expected LSN, `storage_fatal("WAL gap ...")`.
  `storage_fatal` calls `abort()` (`:103-106`).
- `storage.cpp:373`: a segment that the checkpoint fully covers is skipped and never read.
- `storage.cpp:526`: the `WalStream` constructor runs `unlink(segment_path(next_lsn_))` and then creates a fresh segment with that name.
- `src/core/wal_format.cpp:51-59` `check_record`: it rejects `rec_type` values other than {1,2,3,5} at `:56`, **before** the CRC check,
  with the same return value 0 that it uses for a bad CRC.
- `engine.cpp:1105,1167`: these sum `torn_tails` into `RecoveryReport`. Its only consumers are tests and `bench/snc_bench.cpp:561`.
  `tools/snc_server.cpp:822-826` reads only `final_checkpoint_s` and `remapped`. Nothing under `src/net` or `tools` mentions `torn`.

## P1. Mid-record corruption in the newest segment: VERIFIED, with additions

Experiment (`wal_exp mid`): 100 durable creates (LSN 1..100, a single segment `1`), clean stop, then one bit flipped in
the payload of LSN 50.
```
[restart 1] start OK: wal_records_replayed=49 torn_tails=1 objects=49; acked=100 present=49 first_missing_idx=49
[restart 1] child exit: status 0
[after restart 1] segments: 1(lsn 1..49 valid) 50(recs=10,lsn 50..59)     <- 10 new creates reuse LSNs 50..59
[restart 2] start OK: ... torn_tails=1 ...  present=49
```
- `snc_server --shards 1` on the damaged directory (`srv.out`, `srv2.out`) started and served with no error message.
  It printed nothing about recovery. `HEALTH` and `GAUGES` (`admin.out`) contain no `torn` key. The only indirect sign is
  `s0.wal_next_lsn=50`, which an operator could read as a regression only if they already knew the previous LSN.
- **The claim is incomplete in four ways:**
  1. The `torn_tails` trace is not durable. With `CKPT=1` (`mid_ckpt.out`), one checkpoint after restart deleted segment 1
     (`segments_deleted=1`). The next start reports `torn_tails=0` and `present=49`, so all evidence is gone after the next checkpoint.
  2. New writes reuse the lost records' LSNs (LSNs 50..59 above), as P2 states for its case. This affects replication
     and any consumer that identifies records by LSN. I did not test standby divergence.
  3. Damage that zeroes a block can leave **no** counter at all. In `wal_exp zero`, the 4 KiB block holding LSN 50
     (at offset 204800) was zeroed. The result was `torn_tails=0` with 49 present and exit 0 (`storage.cpp:393-395`).
     So `torn_tails` is not even a reliable signal of this loss.
  4. Records of the damaged record's atomic group that come before it are dropped as well (`group.clear()`, `:434`).
     The cut can therefore start before the damaged record. I verified this in code only.
- A clean stop is not required, because the scan does not depend on how the process stopped.

## P2. Damaged first record of the newest segment: VERIFIED

`wal_exp first`: 100 creates, restart, 50 more (segment `101`, LSN 101..150), then a bit flipped in LSN 101 (the first record of 101).
```
[restart 1] start OK: wal_records_replayed=100 torn_tails=1 ... acked=150 present=100 first_missing_idx=100
[after restart 1] segments: 1(lsn 1..100) 101(recs=10,lsn 101..110)   <- original 101 unlinked, recreated, LSNs reused
[restart 2] start OK: wal_records_replayed=110 torn_tails=0 ... present=100
```
- Mechanism: `next_lsn = last_lsn + 1 = 101` (`engine.cpp:1106`), and then `unlink` the segment named 101 (`storage.cpp:526`).
  The constructor's comment assumes that such a file "holds only a header, zeros or an unacknowledged tail".
  That assumption is false here: the file held 50 acknowledged records.
- All 50 acknowledged records are lost (not 49: the damaged record itself is lost too). The next restart reports
  `torn_tails=0`, and LSNs 101..110 now belong to different records.

## P3. Valid CRC, unknown record type: VERIFIED, one wording issue

`wal_exp type`: LSN 50 was rewritten with `rec_type=9` and its CRC recomputed. `check_record` returned 0, and the
outcome matched P1 exactly: `torn_tails=1`, `present=49`, exit 0.
- The type check (`wal_format.cpp:56`) runs before the CRC check, so a record that is well-formed but unknown cannot be told
  apart from a corrupt one. No error names it.
- Wording correction: `WalRecord` has `schema_ver` (`wal_format.h:68`), but that is the object payload's schema version,
  not a version of the record format. It also has an unused `reserved` u16. The format version exists only once per segment,
  in `FileHeader.format`. `header_ok` checks it (`storage.cpp:76`), and a mismatch is fatal (`:381`).
  So "no per-record format version" is accurate. A future record type in a segment that otherwise looks current is silently truncated.
- In an older segment, an unknown type aborts, as in P4 (code inference).

## P4. Corruption in an older segment aborts: VERIFIED, NARROWED

`wal_exp older`: a bit flipped in LSN 50 of segment 1 while segment 101 exists.
```
snc storage: WAL gap before segment (spec C4): .../wal/s000/0000000000000065.wal (errno 0)
child exit: signal 6        (both restarts; files are left untouched, 49+50 records still parse)
```
- The abort is indirect. The damaged segment itself is handled as a torn tail (`torn_tails++`, break). It is the
  **next** segment's `first_lsn` check (`:383-385`) that aborts, with a misleading "WAL gap" message that names the wrong file.
  If the first scanned record is the damaged one, the abort instead comes from `:408` ("WAL missing records").
- Narrowing: a segment that the checkpoint fully covers is never read (`:373`), so damage there is neither detected nor fatal.
  Fail-stop happens before the `WalStream` constructor runs, so no file is modified. Through `snc_server` this is an
  uncaught `abort()` (SIGABRT, exit 134), not the clean "not started" path (`snc_server.cpp:815-820`).

## P5. Dedup ignores target, op and args: VERIFIED

`replay_if_seen` (`shard.cpp:1187-1211`) compares only `opts.seq` with `v.last_seq`. `SessionV1` (`session.h:17-25`) holds
`last_id` and `last_version`, but neither is compared. `sess_exp`, client 7:
```
c7 s1 deposit A +100                          status=OK replayed=0 id_is_target=1 result_len=16
c7 s1 deposit B +500 (different target+args)  status=OK replayed=1 id_is_target=0 result_len=0
c7 s1 withdraw B 1 (different op)             status=OK replayed=1 id_is_target=0 result_len=0
balances A=1100 B=1000                         <- B's ops never ran
c7 s1 CREATE (same seq)                       status=OK replayed=1  (returns A's id as the "created" object; nothing created)
```
- Additions: the answer's `id` is the **original** target, not the one requested. A caller that checked `id` could detect
  the mismatch, but `net::Client` does not compare it.
- `last_status` is hard-coded to `OK` and `last_app_code` to 0 (`shard.cpp:1237`).
- Trigger: the shipped `net::Client` numbers requests monotonically per link (`client.cpp:185-189`). Reaching this case needs
  `client_id` reuse (two processes, or a restart reusing an id) or a buggy client. That is a severity input, not a refutation.

## P6. Failed sequenced ops not recorded; deletes not deduplicated: VERIFIED, but documented design

`record_session` is called only on the success path (`shard.cpp:700-702`, `:777`). Every failure path returns earlier.
```
c7 s2 withdraw Z 1000 (insufficient)      E_APP
c7 s2 resend: deposit Z +1                OK replayed=0     (Z=6: executed)
c7 s3 deposit expected_version=999        E_VERSION_CONFLICT;  resend s3 -> OK replayed=0
c7 s4 deposit on missing object           E_NOT_FOUND;         resend s4 on A -> OK replayed=0
c7 s5 fails, s6 succeeds, resend s5       E_REPLAYED_OLD replayed=1   (s5 never applied)
c7 s7 delete Y -> OK;  resend -> E_NOT_FOUND replayed=0;  then s7 deposit A -> OK replayed=0 (delete consumed no seq)
```
- This behavior is **documented** in `docs/RETRIES.md:42-45`: "Requests that fail ... are not logged; a retry runs them again,
  which can apply at most once". It should therefore be classified as a documented limitation, not as a violation of an
  accepted guarantee. A failed op changes nothing, so re-running it is not double application. Two consequences remain:
  - A resend after a lost failure answer can now **succeed** against a later state.
  - `E_REPLAYED_OLD` with `replayed=1` is returned for a seq that never applied. The status comment says "already decided",
    which is accurate, but `ExecResult::replayed` reads "answered from the session, not run again".
- Deletes: `remove()` never consults the session (`shard.cpp:404`, "Deletes are never sequenced"; `run_delete` `:790+`).
  The wire client sequences only EXEC and CREATE (`client.cpp:185`). A delete whose answer was lost comes back as
  `E_NOT_FOUND` on resend: an ambiguous outcome, not a double effect, because IDs are never reused. A version-conditioned
  delete behaves the same way. `RETRIES.md` does not mention deletes.

## P7. Wall-clock session expiry only at checkpoint: VERIFIED

- Recording: `touched_s = time(nullptr)` (`shard.cpp:1237-1238`). Expiry: `now_s = time(nullptr)` and
  `expire_sessions(now_s, ttl)` run only in `Engine::checkpoint()`, and only on a non-standby (`engine.cpp:1432-1437`).
  The test is `touched_s + ttl < now_s` (`shard.cpp:1251`).
- In `sess_exp` I interposed `time()` in the executable (the static lib resolves to it) with TTL 3600:
```
c9 s1 deposit A +1000                                 OK replayed=0
c9 s1 resend after checkpoint, clock normal           OK replayed=1   A unchanged
c9 s1 resend after clock step(+3700s), no checkpoint  OK replayed=1
checkpoint seq=3
c9 s1 resend after clock step + checkpoint            OK replayed=0   A 2103 -> 3103: applied twice
```
- Additions:
  - `touched_s` is refreshed only by a successful sequenced mutation. A replay or a failed op does not refresh it, so a client
    stuck resending loses its session after the TTL even with no clock step.
  - `touched_s` is replicated, so a promoted standby whose clock is ahead expires the sessions at its first checkpoint.
    That follows from the code; I did not test it.
  - The value is `u32` seconds.
  - `RETRIES.md:155-157` documents the TTL. It does not document the wall-clock dependence, and it does not say that a
    client mid-retry gets double application.

## P8. Cross-shard overtaking in-process: VERIFIED

- `Shard::send_request` (`shard.cpp:1456-1534`) puts an EXEC into `out_batches_[dst]` only if `batch_remote_` is set (the default,
  `snc.h:366`), `args.len <= kMsgInlineBytes` (16, `msg.h:42`) and `expected_version == 0`. Everything else (CREATE_REQ,
  DELETE_REQ, version-conditioned EXEC, args over 16 bytes) is `sri_commit`ted at once.
- The batch is sent only at `kBatchMax` or by `flush_batches()` at the end of the poll iteration (`engine.cpp:146`).
- `order_exp` (2 shards, in-memory). From shard 0, in one task, against an account on shard 1:
```
case1 withdraw 5 (issued 1st) -> OK answered#1 ; delete (issued 2nd) -> E_APP answered#0   20/20 trials
case2 deposit (1st) -> OK ; deposit@expected_version=v+1 (2nd) -> E_VERSION_CONFLICT answered#0  20/20
```
- Additions:
  - Sequenced requests are never forwarded (`E_NOT_OWNER`, `shard.cpp:222-229`), so sequenced clients are exempt.
  - A CREATE overtaking an EXEC cannot conflict on the same object, because the ID is new. It is a reorder only in the abstract.
  - There is a second reorder: if the batch flush cannot claim a ring slot, the earlier batched requests fail with
    `E_OVERLOADED` (`:1546-1553`), even though the later direct one may already have succeeded.
  - I did not test this over TCP.

## Summary for severity

- The two claims with the highest severity are P1 and P2: silent, permanent loss of acknowledged durable writes, with LSN reuse.
  The trace disappears after one checkpoint, or is never created when a block is zeroed.
- P6 is documented design. P5, P7 and P8 are real, but each needs a specific trigger: id reuse, a clock step or a client stuck
  resending, or in-process or non-owner pipelined use.
