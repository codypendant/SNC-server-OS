# Independent QA report: SNC Server OS (main @ 23f4a0c, code identical on branch audit-pr121-architecture-evidence)

Scratch area: /tmp/claude-1000/-home-solo/8f9ad687-3acb-4b0a-945c-92bc6af22f95/scratchpad/qa/ (build in qabuild/, programs in src/).
Nothing in /home/solo/SNC-server-OS was modified; build-* directories were only read (build-asan/Testing/Temporary/LastTest.log, CMakeCache.txt); no benchmark or pilot was run (test_pilot is pure arithmetic, see Q7).

**Disclosure of one process slip.** A `grep -i "ordering\|order"` over docs/*.md that I ran while looking for an ordering contract also printed about 25 matching lines from docs/ARCHITECTURE_EVIDENCE_AUDIT_20261113.md (statements about R2 reorder, R4 cross-group resend, M1-M3 milestones, "power loss is not tested", "CI slice early"). I did not open that file, and I had already been told R2's mechanism by the task text; every conclusion below was nevertheless re-derived from code and runs (and the tests/demos were written after reading the code, not the audit). The reader should still treat Q2, Q3, Q4 and Q8 as possibly anchored.

Classification key: CONFIRMED DEFECT / PLAUSIBLE RISK / UNVERIFIED HYPOTHESIS / REFUTED / UNKNOWN. "CONFIRMED (behaviour)" is used where I demonstrated the behaviour but it is arguably documented or out of contract.

---------------------------------------------------------------------------------------------------

## Q1. Native persistence architecture

### WAL format and write path (all file:line in /home/solo/SNC-server-OS)
- Per-shard WAL stream; files `DIR/wal/sNNN/<first_lsn hex16>.wal` (storage.cpp `segment_path`). Segment = 4 KiB header block (magic `SNCWAL01`, CRC32C, space UUID, first_lsn) followed by records. Record = `WalRecord` header + payload, 8-byte aligned, CRC32C over everything after the first 4 bytes (wal_format.cpp:34-48 `encode_record`, :50-60 `check_record`; checks length, `record_size(payload_len)==total_len`, rec_type in {1,2,3,5}, CRC).
- Records of one op are an "atomic group" (flag `kRecGroupNext`); session record, peer record and target record are written as one group (shard.cpp:693-701).
- Append path: `WalStream::append` (storage.cpp ~ :600) encodes into an in-memory active buffer and assigns the LSN. `poll()` starts a flush when force, `flush_bytes` reached, or `group_commit_us` (default 50) elapsed (snc.h:372). `start_flush()` (storage.cpp:651) zero-pads to a 4 KiB multiple, swaps double buffers, and calls `sri_write(fd, off, buf, len, cfg_.sync, ...)`. Only one flush in flight per stream.
- Syscall and flags: segment opened `O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC` plus `O_DIRECT` by default (`wal_direct=true`, snc.h:374; falls back to buffered if EINVAL), preallocated with `fallocate` (fallback `ftruncate`, i.e. possibly sparse). Write is `io_uring writev2` or `pwritev2` with `RWF_DSYNC` when `wal_sync` (default true) (src/native/runtime.cpp:188-206). `wal_sync=false` exists "development only" (snc.h:373).
- Durability acknowledgement: `WalStream::flush_done` (storage.cpp:814-826) is the only place `durable_lsn_` advances. It aborts the process on any short write or negative result (`storage_fatal("WAL write failed (spec C7: fail-stop)")`, :818; `storage_fatal` = `fprintf` + `abort()`, storage.cpp ~ :105). Then `on_durable` releases waiters (shard.cpp:885-900). A DURABLE mutation completes only when `out.wait_lsn` is durable (shard.cpp:462, :1600-1625); `Durability::APPLIED` is acked before durability, and reads default to waiting for the object's `last_lsn` unless `allow_undurable` (shard.cpp:575-626). With a sync standby the ack additionally waits for the standby (CORE_CONTRACTS section 6 table; not traced by me).
- Rollover: when a flush would not fit, `take_spare` renames a preallocated `spare.tmp` to the new segment name WITHOUT a directory fsync (storage.cpp:690-710, comment at :704); the header block rides in the same write as the first records (`header_pending_`, :675-685). `adopt_spare` at recovery (storage.cpp:797) renames a valid-header spare back into place.
- Segment preparer thread creates spares: fallocate, fsync file, fsync dir (storage.cpp ~ :760).

### Checkpointing (Engine::checkpoint, engine.cpp:1395-1560)
1. `make_dirs(ckpt/<seq>)` (fsyncs parent when it creates). 2. Each shard writes and syncs its part (`part-NNN.ckpt`, header + IMAGE records) (`begin_checkpoint`). 3. `fsync_dir(ckpt/<seq>)` (:1447). 4. `write_manifest` (:1463): tmp file, `pwrite`+`fdatasync`, `rename`, `fsync_dir` (storage.cpp:286-322). 5. `commit_superblock_()` (:1501) = `write_superblock` (storage.cpp:228): 4 KiB block with CRC, written to `superblock.a` or `.b` by generation parity, `pwrite`+`fdatasync`, `fsync_dir`. 6. Only then old checkpoint dir is removed (`remove_tree`, :1511) and WAL segments entirely below every shard's `start_lsn` (and below the replication retention point) are unlinked, followed by `fsync_dir` of the stream dirs, `ckpt` and `wal` (:1514-1560). Order: parts -> manifest -> superblock -> deletion. Superblock is in-place overwrite of the older of two files (never overwrites the newest).

### Recovery (Engine::recover, engine.cpp:864-1220; scan in storage.cpp:363-433)
Order: directory flock (invariant M) -> role file -> `read_superblock` (valid = magic, format, CRC; newest generation among the two files wins, storage.cpp:210-226) -> if `active_ckpt!=0`, `read_manifest` (CRC + exact parse) else fatal -> type catalog check (throws `runtime_error`) -> slot map -> bump `next_lease`, `++generation`, commit superblock -> `adopt_spare` for each stream -> per-shard `scan_part` (checkpoint images) then `scan_stream` from `start_lsn` -> new segment at `next_lsn` (old segment never appended to) -> index rebuild.

What aborts (fail-stop, `storage_fatal`/abort or uncaught exception): no valid superblock with data present (engine.cpp throw, spec C13); manifest invalid when superblock names it (engine.cpp:1007); checkpoint part missing/short/bad header/any corrupt record/object-count mismatch (storage.cpp:486-497); bad WAL segment header unless it is the NEWEST segment and entirely zero (storage.cpp:381); LSN gap between segments (:384); WAL missing records before the checkpoint start (:405ish); WAL write failure at runtime (:818).
What is silently truncated: in `scan_stream` the first record whose CRC/length check fails, or whose LSN is not `expected`, counts `++torn_tails` and `break`s out of THAT segment (storage.cpp:399, :410-411). Everything after it in that segment is discarded regardless of whether it contained valid, acknowledged records. This is only fatal if there is a later segment (the next segment's `first_lsn != expected` -> "WAL gap" :384). See Q4 for the demonstration.

### Which guarantees existing tests demonstrate, under which fault model
- Process kill (SIGKILL), page cache intact: `test_crash.cpp` (child acks DURABLE results over a pipe, parent SIGKILLs at arbitrary points over several rounds; recovered version >= acked version and accounts internally consistent; later rounds recover over an earlier crash tail). Also test_replication / test_transfer / test_cluster / test_migrate / test_s3_writeback T7 (crash positions during checkpoint) / test_lifecycle C4 and test_s4_sync_tokens (kill with checkpoint held before manifest / before superblock; recover from previous generation), test_baseline_move q10_* (SIGKILL all nodes after a move step), test_fail_closed.
- Clean restart, checkpoint then restart, shard-count remap, index rebuild, lease uniqueness: `test_persistence.cpp`.
- Torn tail = garbage AFTER the last good record in the newest segment: `test_persistence.cpp` section E (lines 287-312, asserts `torn_tails>=1`, state verified, stream continues in new segment); `test_transfer.cpp:295-296`.
- Lost directory entry of a rolled spare, simulated by renaming the newest segment back to `spare.tmp`: `test_persistence.cpp` section F (:327-340).
- Checkpoint I/O failure injected as a return value (short write / -EIO) aborts before commit: `test_s3_writeback.cpp` T6, T18; `test_s4_sync_tokens.cpp` T6 (via `Engine::debug_ckpt_fail_write/_fail_sync`, snc.h:683-705).
- WAL buffer growth overflow is fail-stop: `test_wal_capacity.cpp` C3/C4.
- NOT demonstrated by any test (grep for EIO/ENOSPC/LD_PRELOAD/dm-/corrupt found nothing else; see Q4): power loss or fsync reordering, WAL write EIO/ENOSPC, any bit corruption, torn/corrupt superblock, corrupt manifest or part. Fault model of every durability test is process kill or an in-process return-value injection; there is no power-loss or media model.
- Result in my scratch build: test_persistence, test_crash, test_s3_writeback, test_lifecycle, test_s4_sync_tokens, test_wal_capacity all passed (Q7).

### PostgreSQL dependency
REFUTED as a product dependency. `grep -rIil "postgres|libpq"` over src/, include/, tools/, tests/, CMakeLists.txt: no hits. Hits are only in bench/conventional/run.py and run_replication.py (a PostgreSQL/Redis/Scylla comparison workload, "Same accounts workload on PostgreSQL and Redis"), bench/results/*.csv, bench/conventional/scylla/main.go and documentation. CMake builds `baseline_bench`/`baseline_server` (in-process locked baselines, bench/*.cpp), not PostgreSQL clients. Benchmark-only.

### Additional findings
- UNVERIFIED HYPOTHESIS: first write into a spare segment is one `RWF_DSYNC` write containing header block + first records (storage.cpp:675-685). A power loss during that write could persist the record sectors but not the header sector; recovery would then see a non-zero segment with a bad header and abort (:381) rather than lose data (availability, not integrity). Not testable here without power-fault injection.
- PLAUSIBLE RISK: `fallocate` failure falls back to `ftruncate` (sparse file) at segment creation (storage.cpp open_segment, `::fallocate(...) != 0 && ::ftruncate(...)`). Then ENOSPC would surface at write time; the handling is the same fail-stop abort (:818), so this is fail-closed but loses the "space reserved" assumption.
- CONFIRMED (behaviour): `snc_server` never prints or exports `torn_tails` (grep over tools/ and src/: only `engine.cpp:1092,1154` accumulate it into `RecoveryReport`; no printer, no gauge). See Q4.

---------------------------------------------------------------------------------------------------

## Q2. Cross-shard batch ordering

### Code trace
- Batchable iff all of: `batch_remote_` (Engine::Config.batch_remote, default true, snc.h:366), `kind == MsgKind::EXEC_REQ`, `args.len <= kMsgInlineBytes`, `opts.expected_version == 0` (shard.cpp:1473-1474). Batchable requests are copied into `out_batches_[dst]` and returned (:1475-1493); the batch is sent only by `send_batch`, called when the buffer reaches `kBatchMax = 32` (:1492; shard.h:386) or by `flush_batches()` (:1536).
- NOT batchable: CREATE_REQ (shard.cpp:399), DELETE_REQ (:418), any EXEC with `expected_version != 0`, EXEC with args over the inline size, and any batchable request if the batch buffer alloc fails. These fall through to `sri_claim(dst)` / `sri_commit(dst, m)` (:1496-1528), which writes the ring slot directly.
- `flush_batches()` is called exactly once per loop iteration, after `Shard::poll()` and the application's `app_poll` (engine.cpp:145, in `hook_poll`); runtime loop order is drain rings -> run injected tasks -> reap IO -> `hooks.poll` -> `publish_all` (src/native/runtime.cpp:359-388). Injected tasks (`DataPlane::submit`) and network request handlers (ConnSet) run before `hooks.poll`.
- Therefore, within one loop iteration on shard A: a batchable request R1 to shard B is held in A's `out_batches_[B]`; a later non-batchable R2 to B goes straight into the A->B ring; the batch (R1) is written to the ring only at the end of the iteration, AFTER R2. Ring FIFO then delivers R2 before R1. Overtaking cannot cross a loop-iteration boundary because the batch is flushed before the iteration's `publish_all`.
- Sequenced requests cannot be forwarded: `Shard::execute` returns `E_NOT_OWNER` for `client_id != 0 && owner != id_` (shard.cpp:218-225), and `create` likewise (:384-390); docs/RETRIES.md:67-70 states this on purpose ("forwarding through another shard could reorder them"). So overtaking is limited to UNSEQUENCED requests.

### Demonstration (CONFIRMED DEFECT in behaviour: overtaking happens, deterministically)
Programs: src/order_demo.cpp (in-process, 2 shards, `mint_local=true` so the target is owned by shard 1; a task on shard 0 issues op0 = Deposit (batchable) then op1 in the same task) and src/net_order.cpp (real TCP against `snc_server --shards 2`, one `net::Client` connection to shard 0's port pipelines two requests for an object owned by shard 1). Output (order_demo.out):

```
batch_remote=1 mode=deposit;delete                      iters=200: in-order=0   overtaken=200   (deposit got E_NOT_FOUND: delete ran first)
batch_remote=1 mode=deposit;withdraw(expected_version=1) iters=200: in-order=0   overtaken=200   (conditional withdraw succeeded on version 1: it ran before the deposit)
batch_remote=0 mode=deposit;delete                      iters=200: in-order=200 overtaken=0
batch_remote=0 mode=deposit;withdraw(expected_version=1) iters=200: in-order=200 overtaken=0
```
Over TCP (`net_order 31100 100 N`, one connection to the NON-owner shard's port, pipelined send then poll):
```
mode0 deposit;DELETE         : in-order=0 overtaken=100   sample: req10(deposit) status=1 (E_NOT_FOUND) forwarded=1 | req11(delete) status=0
mode1 deposit;withdraw(ev=1) : in-order=0 overtaken=100   sample: req10 status=0 ver=3 | req11 status=0 ver=2   (the later request applied at version 2, the earlier at version 3)
```
Note: an earlier version of my demo created objects without `mint_local`, so some objects were owned by shard 0 (local, no forwarding) and the overtake count looked like 86/200; the identical pattern across both modes exposed that. The numbers above are from the corrected demo.

### Observability by clients
- In-process: any application code on a shard thread (task or `app_poll`) that issues dependent operations to another shard's object in one iteration without waiting for the first completion. CONFIRMED.
- Network: the shipped `net::Client::send(shard, ...)` takes the shard connection from the caller; `ClusterClient::send` routes to `c.owner_of(target)` (cluster_client.cpp:172-176), and the bench clients do too, so shipped routing avoids the forwarded path. But wire.h's own header says "A client may send any request to any shard; the shard forwards it to the owner when needed, so routing is a performance hint and never a correctness requirement". A pipelining unsequenced client that talks to any non-owner port gets reordered execution relative to what the same pipeline would do against the owner port (demonstrated above). A session client (sequenced EXEC/CREATE) never gets this; DELETE is never sequenced (Client::send sequences only EXEC and CREATE, client.cpp:184), so a sequenced client can still mix a sequenced EXEC and an unsequenced DELETE via a non-owner port? No: with a session, the EXEC would be refused `E_NOT_OWNER`, so the mix is only possible on the owner port. UNVERIFIED for that sub-case (not run).
- A client that waits for each response before sending the next is not affected.

### Documented ordering contract
None found for this case. docs/CROSSCORE.md describes batching (it states "Requests with a version condition or larger args are not batched") and says nothing about ordering. docs/RETRIES.md:67-70 acknowledges forwarding can reorder and answers it by refusing sequenced forwards. docs/CORE_CONTRACTS.md:222 states "Receive -> accept -> apply: owner shard, in order" (the owner applies in arrival order, which is the property that holds; arrival order is what is wrong). docs/FAILURE_MODEL.md N6 says reordering "UNPROVEN, no cross-stream ordering assumption found, not audited". So: no document is violated, but the wire.h statement above (routing never a correctness requirement) is contradicted for pipelined unsequenced requests.
- The fix is small in principle (flush `out_batches_[dst]` before claiming a ring slot for a non-batchable request to the same `dst`), but I did not implement or test it (no edits allowed).

---------------------------------------------------------------------------------------------------

## Q3. Duplicate application during slot migration and request retries

### Mechanism, traced
- Client side: `Client::send` (client.cpp:182-205) with a session assigns `sp.seq = l.next_seq++` per link (= per shard connection) for EXEC and CREATE only (DELETE and reads-as-EXEC: EXEC is sequenced regardless of kind, the server dedups MUTATE only). Unanswered requests are kept and resent in order after a failover with the SAME sequence (:215-226, :348).
- Server side: sequenced requests must land on the owner shard (E_NOT_OWNER otherwise). `run_exec` (shard.cpp:562-565) calls `replay_if_seen` BEFORE the `serving(slot)` check at :568, so a resend to the source group gets its recorded answer instead of E_MOVED. `replay_if_seen` (shard.cpp:1187-1210): session found by `client_id` on THIS shard; `seq > last_seq` -> run; `seq == last_seq` -> recorded answer (status OK/version/id) with `replayed`; `seq < last_seq` -> `E_REPLAYED_OLD` (outcome not kept). The same check precedes E_MOVED for creates (:370, :431, run_create :717) and for the follow-up backlog refusal (:205-209).
- Session state: one object per (client_id, owner shard) of type `System.Session` (type_id 1), minted on that shard (`record_session`, shard.cpp:1213-1245), written in the SAME WAL group as the mutation (`log(type, *s, kRecGroupNext)` then the target record, :701-703). It is therefore durable and replicated together with the write.
- Migration: `moves_with_slot(type_id)` is `type_id >= 256` (shard.cpp:3578); `slot_objects`, the digest walk and staging all filter on it (:3844, :3867, :3898, :3945). Sessions (type 1) never move; source keeps them, destination never receives them. CONFIRMED by code.
- ClusterClient (cluster_client.cpp): one `Client` per replication group, each with `enable_session(client_id_)` (:71-75), i.e. the same `client_id` but an independent sequence counter per group and per shard link starting at 1. After `E_MOVED` the application must `on_moved` (refresh, :161-166) and re-`send`; `ClusterClient::send` routes by the NEW group map and issues the request as a NEW request (new sequence) in the destination group's Client. ClusterClient never moves an unanswered request from one group's Client to another.

### Which retry sequences are protected
1. Applied-but-unanswered on source, slot moves, client resends with the same sequence to the source group's primary (or its promoted standby): answered from the source session (`kRespReplayed`), before E_MOVED. PROTECTED. Exercised by `test_baseline_move.py q2_lost_move` (DEBUG DROP-ANSWERS on source while the slot moves; asserts replays>0, moves>0, balances exact with depth 1). It passed in my runs (112 s serial, 112 s under -j4). Note its own comment: "depth 1 (nothing in doubt: each resend is its session's latest)".
2. E_MOVED from the source is only produced when the session says NOT applied (`seq > last_seq` or no session), so a client that re-sends as new work to the destination after an E_MOVED answer cannot duplicate an op the source applied: the source would have answered replay instead. For the shipped ClusterClient after E_MOVED this means the docs' INFERRED double-apply (CORE_CONTRACTS section 7 "Migration" bullet) is REFUTED as stated, on code trace plus q2_lost_move. (I could not run a cross-group scenario of my own: it needs a witness control plane, `snc_ctl move` and two groups; I relied on the existing test and code.)
3. Depth > 1 with several lost answers: resend of an older sequence gets `E_REPLAYED_OLD` (outcome unknown), the latest gets the recorded answer. No duplicate; some outcomes are "in doubt" (documented).

### Not protected (what I can establish)
- PLAUSIBLE RISK (needs an application/driver decision, not the shipped client): a driver that, for an in-doubt request, gives up on the source group and issues the operation again toward the destination (new sequence, destination has no session record for that op) applies it twice. The shipped ClusterClient does not do this automatically, but also offers no API to carry the unanswered queue across groups; if the source group becomes permanently unreachable, `poll` returns -1 and the caller must decide. `CORE_CONTRACTS D-3` is still "UNDECIDED" in the document.
- PLAUSIBLE RISK: session TTL (default 3600 s, `session_ttl_s`, snc.h:424) expiry runs only inside `Engine::checkpoint` (engine.cpp:1423-1428). A resend that arrives after its session was expired is treated as new by the source (`find_session` null -> not replay) and then answered E_MOVED if the slot moved, after which a client following the E_MOVED path to the destination applies it a second time. Needs >TTL in-doubt time plus a checkpoint, and default `snc_server` checkpoints never unless `--checkpoint-s` is set; not demonstrated.
- CONFIRMED (behaviour, but documented as out of contract D-5): reuse of a `client_id` by a new `Client` instance restarts the sequence at 1 against live sessions. Demonstration (src/sess_reuse.cpp against `snc_server`): incarnation 1 applies create + 3 deposits of 100 (balance 300); incarnation 2 with the same client_id sends four deposits of +1000:
```
incarnation2 deposit#1 (+1000) status=13 (E_REPLAYED_OLD) replayed=1 version=0
incarnation2 deposit#2 (+1000) status=13 replayed=1
incarnation2 deposit#3 (+1000) status=13 replayed=1
incarnation2 deposit#4 (+1000) status=0 (OK) replayed=1 version=4
balance after incarnation 2 = 300 (expected 4300 if all applied)
```
Deposit #4 is reported OK but was never executed: a silently lost acknowledged write. Reachable through the public `Client::enable_session` API by any restart that reuses an id. The ClusterClient's per-group Clients also each start at 1, consistent only within one ClusterClient lifetime.
- Unsequenced work (DELETE, any client without a session, MUTATE ops flagged `unlogged`) has no dedup at all, as documented (CORE_CONTRACTS section 7 "Not deduplicated").

### What the tests actually exercise
- test_baseline_move.py q2_lost_move (above) and q10_* (checkpoint/kill/restart at each move step; no session angle). test_migrate.cpp: drives transfers with a session client across a move (lines 444-477: resends after failover, "another group's by the node's map: a newer map, then resend", `cc.enable_sessions`); asserts exact balances. test_fail_closed.py: cluster fail-closed placement checks. None of them exercise: retry to the DESTINATION of an in-doubt request, session expiry vs move, or client_id reuse.
- docs/CORE_CONTRACTS.md section 7 states the migration behaviour as "INFERRED, UNPROVEN" and `[PROPOSED] D-3` "UNDECIDED"; my conclusion: the E_MOVED-resend path is protected, the remaining gap is the destination-resend/in-doubt handling and identity reuse.

---------------------------------------------------------------------------------------------------

## Q4. Durability guarantees and untested scenarios

Method: src/recov.cpp writes N acknowledged (durable=true) creates through a 1-shard Engine (1 MiB segments, one flush per op, so one record per 4 KiB block), stops cleanly, I copy the directory and damage it with Python, then restart through `Engine` and print `recovery()` and what survived. All outcomes below were observed; the crafted directories were in scratch only and have since been deleted.

| Scenario | Existing test | Observed with crafted directory |
| --- | --- | --- |
| Power loss / fsync reordering | none (docs/STATUS.md says untested; only SIGKILL tests) | not testable here (UNKNOWN) |
| WAL write EIO | none (only runtime return-value injection for checkpoint writes) | code: `flush_done` abort (storage.cpp:814-818); not executed |
| WAL ENOSPC / disk full | none | code: same abort; `fallocate` failure falls back to sparse `ftruncate`; not executed |
| Torn WAL tail (garbage after last record) | test_persistence.cpp E (:287-312), test_transfer.cpp:295 | covered; recovers, `torn_tails>=1` |
| Bit corruption mid LAST segment, acknowledged records after it | none | **silent loss, see below** |
| Bit corruption in an EARLIER segment | none | fail-stop (abort, exit 134): `WAL gap before segment (spec C4)` |
| Torn/corrupted A/B superblock | none | newest of two corrupted -> falls back to the other generation, recovered all 600 records from WAL (older generation had active_ckpt 0); both corrupted -> `runtime_error` "no valid SNC superblock ... refusing to initialize" (abort, 134). Fail-closed. |
| Corrupted checkpoint manifest | none | `superblock names a checkpoint with no valid manifest`, abort 134 |
| Corrupted / missing / truncated checkpoint part | none | `corrupt checkpoint part` / `missing checkpoint part`, abort 134 (one flipped byte; deleted file; truncated file) |
| Checkpoint write EIO | test_s3_writeback T6/T18, test_s4_sync_tokens T6 (return-value injection): abort before commit, recover from previous checkpoint | not re-run by me beyond ctest pass |
| Lost directory entry after rename of spare WAL segment | test_persistence.cpp F (:327-340), simulated by renaming newest segment back to spare.tmp; recovery adopts by header (`adopt_spare`) | not re-run beyond ctest pass; true dirent loss is not simulated |

### Last-segment mid-record corruption: CONFIRMED DEFECT (silent loss of acknowledged data)
Code: `scan_stream` (storage.cpp:363-433): `check_record` returns 0 on CRC failure; if the block is not all-zero, `++out.torn_tails; break;` (:408-411) ends the segment; it never looks at whether valid records with higher LSNs follow. The next segment does not exist (this IS the last), so nothing trips the "WAL gap" abort (:384). Recovery then opens a NEW segment at `next_lsn = last_lsn+1` (never appends to the old one) so the damage becomes permanent and the discarded records can never be recovered later.
Runs (100 acknowledged creates, single segment):
```
flip 1 byte in the payload of the 50th record's block:
recovery: ckpt_seq=1 images=0 wal_records_replayed=49 torn_tails=1 objects=49
accounts present=49 ... acknowledged-but-missing=51 (first: 50 51 52 ...)      exit 0, no stderr message
second restart: same 49 (lost for good)
600 creates over 3 segments, flip one byte in the 10th block of the LAST segment:
recovery: wal_records_replayed=519 torn_tails=1 objects=519 ... acknowledged-but-missing=81     exit 0
600 creates, flip one byte in the 50th block of the FIRST segment (earlier segment):
snc storage: WAL gap before segment (spec C4): .../0000000000000100.wal   -> abort, exit 134
```
Operator detectability: the only signal is `RecoveryReport::torn_tails` (a count, value 1 here) available to embedders through `Engine::recovery()`. `snc_server` never prints or exports it (grep: no use outside engine.cpp:1092,1154 and tests). Exit status 0, no stderr output, and no HEALTH/GAUGES field: `grep -rn torn tools/ src/ include/` shows no gauge or printer outside `RecoveryReport` (the HEALTH/GAUGES replies I printed in Q5 were truncated to 300 chars, so the grep, not the replies, is the evidence). So a normal operator cannot detect this. Bit rot in the last segment is a media-fault model that no test covers; the behaviour is a consequence of treating "any invalid record" as "unacknowledged tail".
Severity note: it requires media corruption or a firmware bit error, not a crash. A pure crash cannot produce valid records after an invalid one in the same segment, because only one flush is in flight and flushes are block-padded (comment at storage.cpp:390-394); the rule is sound for crashes but unsound for media errors, and the cheap discriminator (does any later block hold a valid record with a higher LSN?) is not used.

---------------------------------------------------------------------------------------------------

## Q5. Administrative authentication and authorization

### Listeners (code + observed)
| Listener | Flag | Binds | Auth |
| --- | --- | --- | --- |
| Data ports (shard i at base+i) | `--port` (default base), `--host` (default "127.0.0.1", snc net.h:85) | loopback by default; with `--control` the default flips to `0.0.0.0` unless `--host` is given (snc_server.cpp:586) | none |
| Admin port | `--admin-port P` (off by default; `--debug-admin` for DEBUG) | ALWAYS `INADDR_ANY` via `ha::listen_on` (tools/ha.h:31-43, used at snc_server.cpp:1040); no `--admin-host` flag exists | none |
| Witness port | `snc_witness --port` | `INADDR_ANY` (`ha::listen_on`, snc_witness.cpp:677) | none |
| Replication (standby listens on `--standby PORT`+i) | `--standby-host` | the host given (replication.cpp:622-623) | none (space UUID / term checks only; those are consistency checks, not auth) |
| Seed port | `--seed-port` | `INADDR_ANY` (engine.cpp:1235) | none |
| Node-link ports (LINK_PORT+i) | in `--cluster` spec | `INADDR_ANY` (node_link.cpp:203-205) | none |
| Slot-move "mover" (MOVE verbs) | rides the admin port when `--control` | as admin | none |

Grep for TLS/ssl/auth/password/token/HMAC/secret in src/, tools/, include/: hits are only identifiers such as sync tokens, term "token" in sync_tokens.h, `adm_`-prefixed names; no TLS, no authentication, no authorization anywhere (UNVERIFIED only in the sense of a grep; I read admin_loop.h and the request handlers for verbs).

### Admin commands that change state, and who can invoke
snc_server admin verbs (snc_server.cpp:1050-1202): HEALTH, GAUGES, STATUS, RESOURCES, AUDIT (read-only); `PROMOTE` (standby only: `kill(getpid(), SIGUSR1)` -> in-place promotion, new term); with `--control`: MOVE verbs (MOVE ROLE/REFRESH/seal/stage/commit etc, tools/migrate.h) that change slot ownership and seal slots; with `--debug-admin` only: DEBUG STALL, REPL-PAUSE, DROP-ANSWERS (discards answers), MOVE-STOP. snc_witness verbs: PLACEMENT GET/SYNC/CAS/INIT (the cluster slot map), LEASES, DEGRADE / NOTREADY / SYNCED / READY (HA state, grant a primary permission to run without its standby), and DEBUG (only with `--debug 1`). Anyone who can reach those ports can invoke them.

### Demonstration (loopback, ports 31100/31200/31300, scratch data dir, then clean shutdown)
`snc_server --shards 2 --port 31100 --admin-port 31200`:
```
ss -ltnp: 127.0.0.1:31100, 127.0.0.1:31101 (data)     0.0.0.0:31200 (admin)
HEALTH  -> role=n term=0 uuid=82f6... epoch=1 ... shards=2 ...
STATUS  -> role=n term=0 uuid=... shards=2 live=0 degraded=0 ...
AUDIT   -> AUDIT 0 0 0 1 0 0
GAUGES  -> shards=2 ckpt=idle ... (no torn-tail field)
PROMOTE -> NO not a standby      (not a standby; on a standby the same unauthenticated line would trigger promotion)
DEBUG STALL 0 1 -> NO unknown request   (gated by --debug-admin)
MOVE ROLE       -> NO unknown request   (needs --control)
```
All answered with no credentials from an arbitrary local connection; the admin socket is on 0.0.0.0 even though data ports were loopback. `snc_witness --port 31300 --state ... --new-state 1`: listens on `0.0.0.0:31300`; `PLACEMENT GET` -> `NONE`, `DEGRADE` -> `NO bad request`, `DEBUG HOLD` -> `NO unknown request`. Both exited cleanly on SIGTERM. I did not demonstrate a state-changing command that succeeds (PROMOTE on a standby, MOVE, DEGRADE grant) to avoid altering a cluster; the code path (any connection, one text line, no check) is unambiguous for PROMOTE (snc_server.cpp:1197-1201) so: CONFIRMED DEFECT (no authN/authZ, admin/witness/seed/link ports bind to all interfaces), with the state-changing verbs themselves CONFIRMED BY CODE, not by running them.
The documentation positions this as a lab/trusted-network system; I did not look for a stated deployment assumption in the docs beyond the source comments.

---------------------------------------------------------------------------------------------------

## Q6. CI and regression infrastructure

- Automatic infrastructure in the repo: NONE found. No `.github/`, `.gitlab-ci.yml`, `Jenkinsfile`, top-level `Makefile`, `.pre-commit-config.yaml`; `.git/hooks` contains only `*.sample` files (listing above); `crontab -l`: none for the user; /etc/cron.d has e2scrub_all and sysstat only; no `grep SNC-server-OS` hit in /etc/cron* or /var/spool/cron; no snc-related systemd timers. Everything is manual: `cmake`, `ctest` (README.md:20-23). AGENTS.md records manual runs ("ASan clean, full ctest passes").
- Evidence that this matters: `test_pilot` fails on main since commit 2027874 ("File CP-1A GO record; go_record points to it (coordination only)"), which changed `bench/cp1a_frozen.json` without updating `tests/test_pilot.py` (Q7). No automatic gate caught it.
- CMake registration: `ctest -N` in my scratch build lists 49 tests (35 C++ via the `foreach` at CMakeLists.txt:62-65, the rest Python at :85-126). Everything in tests/ that is a test is registered: the only unregistered file is `tests/witness_state.py` (a helper module), plus `check.h`, `harness.h`, `link_proxy.h` (headers). `tests/` has 49 test sources.
- Sanitizers: README.md:25-30 gives manual commands for asan/ubsan and tsan builds; CMakeLists has no sanitizer option or preset. `build-asan/` exists (CMAKE_CXX_FLAGS `-fsanitize=address,undefined -fno-omit-frame-pointer`, Debug) with `Testing/Temporary/LastTest.log` of 2026-11-11 15:38-16:15 showing 48/48 Passed (before the 49th test existed; stale relative to HEAD, not re-run by me). No `build-tsan` directory exists: no ThreadSanitizer evidence at all. (I did not touch build-asan.)
- Suite cost: serial 1701 s, -j4 459 s (Q7). Slowest: test_sync_readiness 286 s, test_baseline_retry 240 s, test_fail_closed 227 s, test_health 156 s, test_ready_order 142 s, test_baseline_ha 136 s.

---------------------------------------------------------------------------------------------------

## Q7. Test failures: deterministic vs intermittent

Build: `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release` into scratch/qabuild, 99 targets built, one `-Wunused-result` warning (test_proof.cpp:276). `ctest -N` -> 49 tests.

### Serial run (`ctest -j1 --output-on-failure`, started 03:38, wall 1701 s = 28.4 min): 48 passed, 1 failed
Timings (s): rate_schedule 0.01, object_id 0.01, local_map 1.16, pages 0.85, runtime 0.22, execute 0.06, persistence 0.39, crash 0.83, net 0.03, transfer 3.14, replication 15.80, cluster 4.58, migrate 29.61, resend_refusals 0.03, proof 0.94, placement 0.04, stage_chain 6.90, gauges 1.05, s1_admission 9.63, s2_pacing 4.30, s3_writeback 21.48, lifecycle 12.53, s4_sync_tokens 11.34, admin_loop 1.12, wal_capacity 7.10, intake 11.46, xnode 65.97, witness 23.81, async_failover 13.67, control 1.49, health 155.65, gauges_admin 3.97, ckpt_cli 0.26, fail_closed 226.61, s1 60.17, profile_check 0.12, baseline_retry 239.61, sync_readiness 285.73, baseline_move 112.10, xnode_proc 50.44, baseline_ha 136.23, ready_order 141.70, paxos_ballots 1.99, witness_hardening 5.59, lab_witness_guard 13.07, index_wire 1.78, **test_pilot FAILED 0.41**, s6 0.14, net_bench_rate 15.66.

### The one failure: test_pilot (CONFIRMED DEFECT in the test: stale test; not a product defect)
```
File tests/test_pilot.py, line 366, in test_freeze_gate_quick_is_not_evidence
    assert frozen["go_record"] is None
AssertionError
```
- Reproduction: 5 isolated runs `python3 tests/test_pilot.py` -> rc=1 each time, 0.41 s each (plus 1 serial and 1 -j4 ctest run: 7/7 failures) -> DETERMINISTIC.
- Cause: the test says "The committed freeze file, before the GO, refuses a real pilot by itself" and asserts `bench/cp1a_frozen.json` has `go_record: null`. The committed file now has `"go_record": "docs/handoff/CP1A_R640_GO_20261111.md"` (commit 2027874 "File CP-1A GO record; go_record points to it (coordination only)", 21:17 on Nov 11). The state changed legitimately; the test was not updated. Not a pilot/arithmetic bug.
- Masking: the script runs test functions sequentially and stops at the first failure. I ran all 24 `test_*` functions in-process separately: 23 pass, 1 fails (this one), so nothing else in the file is hidden behind it.

### Parallel run (`ctest -j4`, 459 s): 48 passed, 1 failed (test_pilot, same assertion); test_intake passed in 17.31 s (11.46 s serial)
I did NOT reproduce a failure of test_intake in any of 8 runs below. I do not know what "failed or looked slow" in the earlier parallel run referred to; the only observable difference I found is slowness under load.

### test_intake, 5 isolated runs (qabuild/test_intake): 5/5 pass, 11.45 / 11.43 / 11.43 / 11.44 / 11.44 s
Under artificial load: four `taskset -c 0..3` busy loops -> PASS in 17.61 s (+54%); additionally (my extra) 72 busy loops, one per CPU -> PASS in 43.80 s (3.8x). So test_intake is load-sensitive in duration (some waits are real-time and scale with scheduling), not in outcome, in the three loaded/parallel executions I did (j4 17.3 s, 4-busy 17.6 s, 72-busy 43.8 s). Whether a timeout assertion inside would trip under heavier load: UNKNOWN. CMake gives it TIMEOUT 2400 (CMakeLists.txt:68).
### test_pilot 5 isolated runs: 0/5 pass (deterministic, above). It runs no processes (docstring "Pure arithmetic: no processes"; `pilot.py` is only imported), so the "do not run the pilot" constraint was respected.

Classification: no intermittent failure observed; one deterministic stale test.

---------------------------------------------------------------------------------------------------

## Q8. Milestone proposal (based only on what I found)

Defects to fix before building new functionality on top (in priority order):
1. CONFIRMED: WAL recovery silently truncates acknowledged records after mid-segment corruption in the last segment, with no operator signal (Q4). Fix: on an invalid record, scan the rest of the segment (block-aligned) for any valid record of a higher LSN, and abort if one exists; surface `torn_tails` (and bytes discarded) in stderr and HEALTH/GAUGES; add the corruption test matrix.
2. CONFIRMED: batched vs non-batched cross-shard overtaking (Q2): flush `out_batches_[dst]` before sending any non-batchable request to `dst` (or document and enforce an explicit no-ordering contract and stop saying routing is never a correctness requirement). Add a regression test (my `order_demo` is a ready seed).
3. CONFIRMED (documented as out of contract): client_id reuse yields a false OK (Q3). Fix: server-issued client incarnations or a request fingerprint stored in the session; refuse `seq == last` when the fingerprint differs.
4. CONFIRMED: no authN/authZ, admin/witness/seed/link ports bind all interfaces (Q5): at minimum loopback-by-default plus an optional shared-secret, before any non-lab deployment.
5. CONFIRMED stale test_pilot, which also means HEAD is red on a plain `ctest` (Q7).
6. PLAUSIBLE: in-doubt resend/TTL handling after slot moves (Q3, D-2/D-3): decide "move sessions with the slot or refuse sequenced resends whose session the destination cannot prove ACTIVE".

### M0 (a few days, first): minimal CI and a green baseline
- Scope: GitHub Actions/any runner doing Release build + a fast ctest subset (<5 min: all tests under ~30 s, i.e. everything in the serial table except health, fail_closed, baseline_*, sync_readiness, ready_order, xnode*, s1, witness) on every PR, full ctest nightly (28 min serial / 8 min -j4), an ASan+UBSan job (the build-asan evidence is stale and manual), a TSan job (none exists), and fix test_pilot.
- Why first: it is cheap, there is currently zero automation, a test already went red unnoticed on main, and every later milestone modifies durability and cross-shard paths where regressions are otherwise invisible. Exclusions: no benchmark gating, no performance thresholds.
- Acceptance: PR check required on main; `ctest` green on HEAD in CI three consecutive times; ASan/UBSan job green over the full suite; TSan job runs (failures triaged, not necessarily zero on day one); a deliberately broken test fails the PR.

### M1: durability under media faults, fail-closed (depends on M0)
- Scope: defect 1; fault injection for WAL write EIO/ENOSPC (an LD_PRELOAD or failpoint shim; today only checkpoint writes are injectable); corruption matrix (last segment mid-record, earlier segment, superblock A/B, manifest, part) as ctest cases built from my recov/crafted-directory approach; a real power-loss test (e.g. dm-log-writes/dm-flakey or a VM-level crash harness) that replays WAL writes with reordering and verifies "nothing acknowledged is lost"; decide `fallocate` failure behaviour; document the fault model. Exclusions: replication/HA changes, performance work, new data types.
- Verification: the matrix tests; randomized crash+reorder harness over >=1000 iterations; recovery report exposed in `snc_server` output and HEALTH.
- Acceptance: every cell in the Q4 table is either covered by a named test or explicitly declared out of scope in docs; no scenario ends with exit 0 and fewer acknowledged records; 1000 power-loss-simulated recoveries with zero lost acknowledged writes; `torn_tails>0` visible in stderr and HEALTH and fails `snc_ctl` health checks unless acknowledged.

### M2: ordering and retry semantics (depends on M0; may start in parallel with M1)
- Scope: defects 2, 3, 6; write the op-ordering contract (per origin and target, what is guaranteed, including unsequenced forwarded requests); migrate session records with the slot or refuse in-doubt sequenced resends at the destination; server-issued client ids; make session expiry fail-closed (D-2); a process-level cross-group resend scenario in the `test_baseline_move` style including destination-directed resends and TTL expiry. Exclusions: new application types, schema evolution.
- Verification: randomized interleaving test over batchable/non-batchable mixes (in-process and TCP) that fails on reorder; migration test with an applied-but-unanswered request resent to every candidate owner.
- Acceptance: a pipeline of N dependent ops issued through any shard port applies in issue order (0 reorders in 10^6 randomized interleavings); a resend after a move applies exactly once or returns an explicit in-doubt status (never a second apply, never a false OK) in a repeatable test.

### M3: deployable security and operability (depends on M0; admin hardening can start immediately)
- Scope: defect 4: bind admin/witness/seed/link/replication ports to a configured interface (default loopback), shared-secret or mTLS on admin, witness, replication and node-link; separate read-only (HEALTH) from state-changing verbs; packaging and a deployment/runbook with backup/restore of a data directory and a tested rolling upgrade/format-version policy. Exclusions: multi-tenant authorization, public-internet exposure.
- Acceptance: an unauthenticated connection to every listener cannot change state (tested); `ss -ltn` of a default start shows only loopback; a documented and tested restore-from-backup procedure; all ports listed with auth mode in docs.

Early CI placement: M0, before everything, because the risky milestones (M1, M2) change recovery and cross-shard messaging and only a runnable regression gate makes them safe; the sanitizer job in particular would guard the new fault-injection and recovery code. M1 and M2 can then proceed in parallel (they touch different files: storage/engine vs shard/session); M3's port-binding change is small enough to land inside M0/M1 time.

---------------------------------------------------------------------------------------------------

## What I did NOT manage to verify
- Power loss, fsync reordering, real torn sectors, WAL EIO/ENOSPC at runtime (no fault-injection facility; only reasoned from code).
- A cross-group slot move with an in-doubt resend toward the destination (needs witnesses + control plane + two groups; relied on the existing q2_lost_move test and code reading). TTL-expiry duplicate path not demonstrated.
- That a state-changing admin verb succeeds from an arbitrary client (not run to avoid changing cluster state).
- Sequenced EXEC mixed with unsequenced DELETE on the owner port (sub-case of Q2).
- The cause of whatever the "earlier -j4 run" showed for test_intake; no failure reproduced.
- Sanitizer builds: not re-run; build-asan log is dated Nov 11 and covers 48 tests.
- Sync-standby ack path in the durability definition (read from CORE_CONTRACTS, not traced).
- Recovery of `next_lease` after falling back to an older superblock generation (possible ID-lease regression on superblock fallback): not examined.

---------------------------------------------------------------------------------------------------

## Agreement and disagreement with the audit
(Written AFTER sections Q1-Q8 above were complete. Read: docs/ARCHITECTURE_EVIDENCE_AUDIT_20261113.md and docs/handoff/PR121_Audit_Review_QA_Required.md in full where cited; the working notes A-E and the clean-build log were not read.)

### Agreement (each re-derived independently above)
1. No PostgreSQL dependency (audit sec 1 "Correction", 3.8): agree, benchmark-only (Q1).
2. R1 / 3.5: last-segment mid-record corruption silently drops that record and all later acknowledged records, counted only as `torn_tails` (storage.cpp:396-420 in the audit; :399/:408-411 by my numbering): CONFIRMED by running it (49 of 100 and 519 of 600 survive, exit 0).
3. R2: a batchable EXEC held in the out-batch can be overtaken by a non-batchable request (create, delete, version-conditioned, large args) to the same shard: CONFIRMED by running it in process (200/200) and over TCP (100/100); with `batch_remote=false` 0/200. Their proposed fix (flush the pending batch before a direct send to the same destination) matches mine. "No test and no document covers it": agree (RETRIES.md:67-70 covers only the sequenced case).
4. 3.6 / U: no test corrupts or tears the superblock, manifest or a part: agree. I characterized the outcomes: all fail-closed (abort or runtime_error), with fallback to the older superblock generation when only the newest one is bad.
5. R6: no authentication/TLS; `PROMOTE`/`MOVE` reachable by any peer; listeners on INADDR_ANY: agree (Q5, demonstrated on admin and witness ports).
6. R9 / T-5: no CI, no TSAN on record, `.git/hooks` samples only: agree. Also agree T-2: test_pilot is red, deterministic, stale (`bench/cp1a_frozen.json` go_record set by 2027874; test asserts None). The test lists `tests/test_pilot.py` among paths whose post-freeze change is refused (test_pilot.py:344ish in the list I read), so the audit's "fix collides with the freeze gate" is consistent with what I saw; I did not run pilot.py's gate against such a change (UNKNOWN to me).
7. Roadmap shape: M1 durability fault model, M2 ordering/retry, M3 CI/security; their first slice (M1-a, test-only characterization of the current behaviour) is sound, and "ordering test needs no prerequisite" is right.
8. CORE_CONTRACTS section 7 is accurate that sessions do not move with slots (`moves_with_slot` = type_id >= 256, shard.cpp:3578) and that test_baseline_move q2 exercises only the resend-reaches-source branch.

### Disagreement or correction
1. R4 / 3.12 wording ("Exactly-once breaks at a slot move or across groups"; "a resend ... that lands on the new owner after a map refresh would be a double apply"): too strong as a headline. The source answers a resend from its session BEFORE E_MOVED (shard.cpp:564 precedes :568), and E_MOVED is only produced for a request the session shows as not applied, so the shipped ClusterClient's E_MOVED-then-resend path cannot double-apply. A double apply needs a driver that re-issues an in-doubt request to the destination on its own, or session expiry (TTL) between the first attempt and the resend. That is a PLAUSIBLE RISK, not a demonstrated defect, and I could not run a cross-group scenario of my own (Q3). The audit's own M2(b) cross-group resend test is the right way to settle it.
2. R6 citation: "the admin port (PROMOTE, MOVE) binds INADDR_ANY (engine.cpp:1234, node_link.cpp:203)". engine.cpp:1234 is the SEED listener and node_link.cpp:203 the node-link listener; the admin port is tools/ha.h:38 (`ha::listen_on`, used at snc_server.cpp:1040) and the witness port is snc_witness.cpp:677. The conclusion is right, the cited lines are not the admin listener.
3. T-3 / sec 0 ("`test_intake` failed once under `ctest -j4` ... one is flaky under load"; "47 of 49 passed"): I could not reproduce any test_intake failure: 0 failures in 8 executions (5 isolated, 1 serial ctest, 1 ctest -j4, plus 2 under artificial CPU load), and my serial and -j4 ctest runs were 48/49 (only test_pilot). What I did measure is duration sensitivity to load: 11.45 s isolated, 17.3 s under -j4, 17.6 s with four pinned busy loops, 43.8 s with all 72 CPUs busy, all passing. "Flaky" is therefore UNVERIFIED; "load-slow" is confirmed. The audit's account (first failure's log overwritten) means the claim cannot currently be checked either way.
4. Executive summary item 1 and R1 call the evidence "process-kill only"; more precisely the fault model is process-kill plus in-process return-value injection for CHECKPOINT writes (not the WAL). Minor.
5. The severity framing of R1 is fair, but the audit does not say the signal is invisible to operators: `snc_server` neither prints nor exports `torn_tails` (grep). That is the part that makes the silent loss undetectable, and it should be in R1 and in the M1 acceptance criteria (my M1 acceptance asks for it).

### What the audit missed or understated
1. Client-identity reuse gives a false success: a second `Client` with the same `client_id` restarts sequences at 1 and receives `OK, replayed` for a request it never ran (demonstrated: deposit #4 reported OK, balance unchanged). CORE_CONTRACTS D-5 mentions "out of contract"; the audit does not list it as a risk, and it is reachable through the public API after any client restart.
2. The Q2 overtaking is observable by a real network client only through a non-owner shard port, since `ClusterClient::send` and the bench clients route to the owner and sequenced requests are refused (`E_NOT_OWNER`) rather than forwarded. The audit's coupling note ("client retry/session logic") is therefore mostly inapplicable (sessions are exempt), and the exposure is unsequenced pipelined clients and in-process apps, contrary to the wire.h claim that routing is "never a correctness requirement". Worth stating so the fix priority is judged correctly (still a correctness defect, narrower reach).
3. Seed, witness and node-link ports, not only the admin port, are unauthenticated on all interfaces; DEGRADE/NOTREADY/READY on the witness and PLACEMENT CAS change HA and placement state.
4. Session TTL expiry (runs only in checkpoint; default server never checkpoints periodically) as a second route to destination double-apply, and E_REPLAYED_OLD as the documented in-doubt outcome with depth > 1.
5. A cheap recovery discriminator exists for defect 1 (look for any later valid record of higher LSN in the same segment before truncating); the audit frames it as "fail-stop versus truncate", but a discriminating rule avoids the availability trade-off for the ordinary crash tail.
6. Milestone sequencing: the reviewer's handoff asks for minimal CI EARLY; the audit's text still lists CI under M3 and orders "M1, then M3, then M2". My Q8 puts a small CI/green-baseline step (M0: clean build, fast ctest subset, ASan/UBSan, test_pilot fixed) before M1 and M2, which is consistent with the reviewer's request A. Fixing test_pilot is a prerequisite for a green gate and needs the freeze decision the audit already requests.
7. The suite runs in 28.4 min serial / 7.7 min at -j4 (per-test table in Q7), which is the basis for a PR/nightly split in CI; the audit gives no timing.
