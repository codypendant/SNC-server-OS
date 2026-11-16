# Phase 1 recon, reviewer A: core (object model, execution, persistence, runtime)

Checkout: /home/user/snc-server-os (no .git, so commit is UNKNOWN). Read-only. Experiments were built against the
release build at `scratchpad/build/libsnc_core.a`. Their sources are in `scratchpad/exp/` (d2.cpp, d2b.cpp, d2c.cpp,
d3.cpp), and their data dirs were deleted after each run.

Labels: **V** = VERIFIED (I read the code or ran it), **D** = DOCUMENTED only, **I** = INFERRED, **U** = UNKNOWN.
Citations are `file:line` on this checkout.

---

## 1. Subsystem map

| Subsystem | Files (lines) | Responsibility | Calls into |
|---|---|---|---|
| Types/status | include/snc/types.h (1-62) | fixed-width ints, `Status` enum (19 codes), limits (`kMaxPayload` 1 MiB, `kMaxArgs`/`kMaxResult` 64 KiB, `kMaxShards` 256), LE/64-bit static_asserts (types.h:16-17) | none |
| Identity | include/snc/object_id.h (1-63), src/core/object_id.cpp (1-73) | ObjectID layout, Minter, text form, `snc_hash64` (XXH3), `status_name` | xxhash |
| Placement (intra-node) | include/snc/slot_map.h (1-33) | 4096 slots, slot = hash>>52, slot->shard `owner[]` | Identity |
| Object layout | include/snc/object.h (1-46) | 48-byte header, flags | Identity |
| Registry | include/snc/registry.h (1-105), src/core/registry.cpp (1-83) | Type, Op, Index descriptors, validation at registration (abort on bad) | none |
| Local map | include/snc/local_map.h (1-213) | per-shard open-addressing ID->header map, low hash bits, backward-shift delete, one-step rehash (local_map.h:9-11) | pages, Identity |
| Allocator/pages | include/snc/allocator.h, src/core/allocator.cpp (1-90), include/snc/pages.h, src/core/pages.cpp (1-70) | mimalloc first-class heap per shard, optional huge-page arena plus spill heap, heap limit | mimalloc, mmap |
| SRI (runtime interface) | include/snc/sri.h (1-93), include/snc/msg.h (1-68) | send/claim/commit/publish, async write/fdatasync, wake, time | backend |
| Native backend | include/snc/native_runtime.h (1-82), src/native/runtime.cpp (1-652), src/native/ring.h (1-58) | pinned pthread per shard, full-mesh SPSC rings, overflow deques, io_uring, idle eventfd and poll, io_yield | liburing, sched |
| Seastar backend | src/seastar/runtime.cc (1-510) | same `NativeRuntime` class on Seastar reactors (off by default, CMakeLists.txt:46-58) | Seastar |
| Shard (the core) | src/core/shard.h (1-926), src/core/shard.cpp (1-4347) | entry API, owner execution, WAL logging, waiters, sessions, index, cross-shard dispatch, batching, recovery install, replication hooks, checkpoint, follow-ups, slot moves (see below) | SRI, WAL, Registry, LocalMap, Allocator, replication, node_link |
| Engine | include/snc/snc.h (350-977), src/core/engine.cpp (1-2075) | lifecycle, config, recovery orchestration, checkpoint commit, promotion, slot-move control path, data-plane leases, hooks into the runtime | Shard, Storage, Runtime, seed, replication |
| Storage formats and IO | include/snc/wal_format.h (1-89), src/core/wal_format.cpp (1-61), include/snc/storage.h (1-303), src/core/storage.cpp (1-829) | CRC32C, record encode/check, superblock, manifest, segment scan, WalStream (group commit), SegmentPreparer (spares), checkpoint part scan | SRI (sri_write) |
| Sessions | src/core/session.h (1-31), src/core/session.cpp (1-50) | System.Session type (id 1), client_id index | Registry |
| Slot state | src/core/slot_state.h (1-48), src/core/slot_state.cpp | System slot-state type (id 2), SlotStateV1 | Registry |
| Follow-up intake accounting | src/core/followup_board.h (1-56) | NxN taken, ended and wait counters | atomics |
| Checkpoint pacer | src/core/ckpt_pacer.h (1-140) | pure controller for the walk budget (S2) | none |
| Sync tokens | src/core/sync_tokens.h (1-112) | K-limited final fdatasync tickets (S4) | atomics |
| Seeding | src/core/seed.h (1-54), src/core/seed.cpp (1-280) | ship the active checkpoint to a new standby over TCP | storage |
| Sample type | include/snc/types/account.h (1-115), src/types/account.cpp (1-421) | Account, transfers via outbox and inbox follow-ups | public API |
| Build | CMakeLists.txt (1-192) | one static lib `snc_core` with core, net and account (CMakeLists.txt:18-45). Links mimalloc, xxhash and uring as system libs (13-15) | none |

### shard.cpp responsibilities (4347 lines, one class `Shard`), V from the function outline

| Lines | Responsibility |
|---|---|
| 1-127 | test/debug globals, ctor |
| 129-476 | entry-shard API: `execute` 170-244, admission `admit` 282-304, `admit_answer_wait` 321-343, `pending_refuse` 345-354, `create` 356-400, `remove` 402-419, `followups_refuse` 424-444, `prefetch`, `complete_local` 461-475 |
| 477-870 | owner-side execution: `run` 479, `prepare_ctx`, `validate_checked` (debug hash check), WAL `log` 533-538, `run_exec` 540-713, `run_create` 715-786, `run_delete` 788-869 |
| 871-940 | durability waiters (`defer`, `release_waiters`), per-iteration `poll` 919-939 |
| 941-1166 | gauge publishing (observability) |
| 1167-1262 | quiesce, client sessions (`find_session`, `replay_if_seen` 1187-1211, `record_session` 1213-1243, `expire_sessions` 1245-1261) |
| 1263-1434 | secondary index partition and cross-shard index gather |
| 1435-1778 | cross-shard dispatch: `send_request` 1456-1532, batching 1536-1665, `serve`/`complete` 1711-1767, buffer return |
| 1779-1863 | recovery `install` 1781-1820, `finish_recovery` 1822-1856 |
| 1864-2111 | node links (cluster), blocked-primary handling |
| 2112-2277 | promotion, withdraw, replication glue (`apply_replicated`, `install_indexed`, `poll_replication`) |
| 2278-2750 | checkpoint: begin 2290-2342, chunk IO and windows, sync, pacing, `checkpoint_step` 2601-2749 |
| 2751-2831 | stats |
| 2832-2965 | `OpCtx` methods: reserve, resize, `local` 2863-2885, `reserve_emits`, `emit` 2902-2947, `queue_emits` 2949-2964 |
| 2966-3550 | follow-up intake (D.1): gates, windows, exchange accounting, resume scans, `route_emit`, `run_emits`, `send_emit`, `serve_emit`, hold and stale |
| 3551-4316 | slot moves: slot-state object, proofs, seal, unseal, reseal, export, stage chain |
| 4317-4347 | public free functions forwarding to `tl_shard` |

engine.cpp mixes runtime hooks (64-160), DIR/role file (170-212), lifecycle and permits (300-470), the ctor's config
validation (468-629), start and stop (630-819), `recover` (877-1255), barrier (1257-1318), `promote` (1320-1406),
`checkpoint` (1408-1576), slot-move control (1606-1937) and stats (1970-2075). V (outline).

---

## 2. Public interfaces

### 2.1 Application API (include/snc/snc.h), V

| Item | Location | Contract stated | Not stated |
|---|---|---|---|
| `execute(target, op, args, opts, done, ctx)` | snc.h:49-50 | async, called from a shard thread, completes on that shard. May complete **before returning** for local targets (snc.h:1-5) | **no ordering guarantee between calls** (see D1). No statement that `result` aliases scratch (only "valid until the completion returns", snc.h:44) |
| `create`, `create_near`, `remove` | snc.h:51-58 | create_near mints until the slot matches `near` (~4096 mints) | that a sequenced create without `near` is forced onto the entry shard (shard.cpp:380) |
| `index_lookup` | snc.h:60-64 | skips non-durable states, not a snapshot | consistency with concurrent writes |
| `ExecOptions` | snc.h:26-35 | expected_version (0 = none), DURABLE or APPLIED, allow_undurable for reads, client_id and seq | seq semantics (gaps allowed, see 4.5). No statement that a reply's `id` must be checked |
| `ExecResult` | snc.h:37-45 | status, app_code, version, durable, replayed, id, result | none |
| `OpCtx` | snc.h:81-157 | `reserve` (validate only), `resize` (apply only, at most the reserved length), `reject`, `emit` (durable follow-up, results discarded, not logged, at most 16-byte args, fail_op), `reserve_emits`, `source()`, `local()` (one other object on the same shard, payload in place, logged in one atomic group), `set_result` | violations are `contract_violation` (abort), not errors |
| `OpDescriptor` | registry.h:17-48 | validate is read-only (debug-checked by hash, shard.cpp:514-526). apply must succeed after OK. `keeps_index_key`, `unlogged`, `max_emits`, `followup` | apply is synchronous with no yield (assumed by admission, shard.cpp:306-319) |
| `TypeDescriptor` | registry.h:60-77 | type_id >= 256 for users, schema_ver, sizes, `check`, create and delete ops, one index, `resume` hook | no schema migration hook |
| `Engine` | snc.h:350-977 | lifecycle one way, start recovers and aborts on corruption (snc.h:612-620). stop flushes WAL and waits durability (621-630). checkpoint and promote are serialized (639-650) | none |

Execution phases (V, shard.cpp:540-713): refusals, then version check, then **validate**, then admission (R1),
then **apply**, then version++, then the peer record (GroupNext), then the index, then the session record
(GroupNext), then the UPDATE record. Waits follow: emits queue, and the completion waits for DURABLE.

### 2.2 Runtime interface (SRI), V, include/snc/sri.h

`sri_send/claim/commit/publish` (41-55): requests may be refused, but `must_deliver` responses never are.
`sri_write(fd, off, buf, len, dsync, done)` (69-80): RWF_DSYNC durability, and the completion is always deferred
to the loop. `sri_fdatasync` (85), `sri_watch_fd` (91), `sri_now_ns`, topology. The backend is chosen at link time
(sri.h:4). **Not stated:** FIFO order per (src, dst). The ring and overflow deque preserve it in practice
(runtime.cpp:126-193). Also not stated: any delivery guarantee across backends. Seastar sends are "never refused"
(runtime.cc header, lines 16-18).

### 2.3 Storage interface, V, include/snc/storage.h

Paths (32-37), superblock read and write (46-51), manifest (55-76), `scan_stream` (91-97),
`read_stream_range` (99-110), `scan_part` (112-115), `WalStream` (202-301), `SegmentPreparer` and `adopt_spare`
(151-198). The contract comment on `scan_stream` says it stops a segment at its first invalid record ("torn tail")
(storage.h:91-95). It does not say that this applies equally to mid-segment corruption of acknowledged data.
That is D2.

---

## 3. Object model facts

| Fact | Evidence | Label |
|---|---|---|
| ObjectID is 128 bits: `hi` = 8-bit format (0x01) plus a 56-bit lease, `lo` = 64-bit seq from 1 | object_id.h:14-23 | V |
| Minting: `Minter::next()` returns `++seq_` within the lease, one Minter per shard, not thread-safe | object_id.h:51-61 | V |
| Leases: at recovery, `next_lease += n` is committed to the superblock **before** any shard mints. Shard s gets `lease_base + s` | engine.cpp:1049-1052, 1206 | V |
| A re-lease after replay sees leases above the base (promotion case): rebase and commit | engine.cpp:1190-1208 | V |
| Cluster lease blocks come from the control plane via `take_lease_block` | engine.cpp:1044-1048, snc.h:441-451 | V (control plane U) |
| Text form: 33 chars, lowercase hex, dash | object_id.cpp:18-46 | V |
| Hash: `XXH3_64bits` over the 16 bytes (hi then lo, LE), seed 0, header-inlined (`XXH_INLINE_ALL`) from the **system** xxhash | object_id.cpp:6-16, CMakeLists.txt:13 | V |
| "Frozen" is only a comment (object_id.h:47). The test is a tautology: `snc_hash64(make(1,1)) == snc_hash64({0x0100000000000001,1})`. **No pinned numeric test vector** | tests/test_object_id.cpp:52-53 | V |
| Slot = `hash >> 52` (top 12 bits). Local map index = low bits | slot_map.h:14, local_map.h:3-4 | V |
| Owner map: `SlotMap.owner[4096]`, default `s % n_shards`, epoch +1 on change. Immutable while running. Persisted in the manifest. Rebuilt with the default when the shard count changes at recovery, followed by a forced checkpoint | slot_map.h:16-31, engine.cpp:1032-1038, 1228-1234 | V |
| Cluster: a second level of slot->group in `ClusterMap` (placement), outside this scope | snc.h:75-78 | V (pointer only) |
| Header (48 B): id, version (1 at create, +1 per logged mutation), last_lsn (runtime only, 0 after recovery), type_id, length, capacity, flags, schema_ver | object.h:18-40, shard.cpp:1811 | V |
| Flags: `kFlagDeleted` used. `kFlagDirty` and `kFlagIndexed` are declared but **never referenced** in src/ or include/ (grep) | object.h:12-16 | V |
| Unlogged MUTATE does **not** bump the version and is not logged (shard.cpp:666-672). The change persists only through a later logged change or checkpoint image | registry.h:33-38 | V |
| Type registry: process-global, populated before start, read-only after. Ids 0..255 reserved (Session=1, SlotState=2). Op ids < 65536, stored dense | registry.h:79-103, registry.cpp:9-44, session.h:13, slot_state.h:14 | V |
| Type identity persisted: the manifest lists (type_id, schema_ver, name) for **every registered type** at each checkpoint | engine.cpp:1474 | V |
| Startup check: each manifest type must be registered with the same schema_ver and name, else `runtime_error` (refuse). New types not in the manifest are accepted. **No migration path** | engine.cpp:1022-1028 | V |
| WAL and image records carry `schema_ver`, but `install` does **not** compare it with the registry. It checks only type existence, `max_size` and `check()` | shard.cpp:1793-1798 | V |
| Op ids are never persisted: the WAL is **state-based** (every UPDATE record is the full object payload). Op tables can change freely across restarts | shard.cpp:533-538 | V |
| Deletion: validate and apply the delete op, erase the index, version++, log DELETE (no payload), swap in a 48-byte tombstone (`kFlagDeleted`), ERASE waiter frees the tombstone once durable. Reads of a tombstone return E_NOT_FOUND after waiting for durability | shard.cpp:788-869, 907-913, 573-577 | V |
| IDs are never reused: a duplicate mint aborts ("ObjectID minted twice") only if the old object is still in the map | shard.cpp:732-735 | V |
| References between objects: **none at engine level**. IDs appear only inside app payloads (e.g. `OutEntry.to`, account.h:73-77). There is no referential integrity, no GC and no cascading delete | registry.h, account.h | V (absence by reading) |
| Multi-object ops: `OpCtx::local()` allows at most one peer object, same shard, live, matching type, slot served. Payload changes only in place (no resize or index change, debug-checked) | shard.cpp:2863-2885, 674-691 | V |

---

## 4. Execution

### 4.1 Threading and dispatch, V

- One worker thread per shard runs `loop()`: drain all inbound rings (`batch_in`=32 each), injected control tasks,
  reap IO, `poll` hook, `publish_all` (runtime.cpp:368-411). App code runs in `app_poll` inside `hook_poll`, then
  `flush_batches()` (engine.cpp:140-148).
- Entry shard: if the owner is local, `run_exec` runs inline and the completion may fire inline
  (shard.cpp:230-240, 461-466). If not local, `send_request` (1456-1532) uses a pending slot with gen and req_id,
  then either a **batched** record or an immediate ring message.
- Batchable means `batch_remote && EXEC_REQ && args<=16 && expected_version==0` (shard.cpp:1473-1474). Those go
  to `out_batches_[dst]`, sent at `kBatchMax`=32 (shard.h:386) or at end of iteration (engine.cpp:146). Everything
  else (create, delete, conditional, large args) gets `sri_claim/commit` **immediately** (1506-1531).
- The owner serves a MULTI_REQ in order and writes responses over the records. A response is "answered apart" if
  it must wait for durability or the result is over 16 B (shard.cpp:1577-1640).

### 4.2 Ordering from one origin

| Path | Order | Label |
|---|---|---|
| Same shard, local owner | program order (synchronous) | V shard.cpp:230-240 |
| Shard A to shard B ring | FIFO (SPSC ring plus FIFO overflow deque, a claim goes to overflow if it is non-empty) | V runtime.cpp:126-140, 167-193, 307-334 |
| A to B, mixed batchable and non-batchable | **not FIFO: D1** | V shard.cpp:1473-1531 |
| A to B, completions | a durable wait reorders answers (waiters sorted by LSN, shard.cpp:873-883) | V |
| TCP, one connection | requests dispatched in arrival order into execute, create or remove. Answers keyed by req_id, so they can return out of order. Unsequenced requests to a non-owner port are forwarded, so D1 applies | V src/net/server.cpp:126-160 |
| Sequenced requests | must reach the owner shard, else E_NOT_OWNER, never forwarded (shard.cpp:222-229, 383-388) | V |

### 4.3 Same-shard multi-object atomicity, V

Peer record logged first with `kRecGroupNext`, then the session record (GroupNext), then the op's record. Recovery
installs a group only if the last record is read (storage.cpp:416-434). A group never spans segments: rollover
happens only at `start_flush`, which writes the whole active buffer to one segment (storage.cpp:651-688), and an op
appends synchronously with no poll in between (debug-checked, shard.cpp:306-319).

### 4.4 Follow-ups (emit), V

- Allowed only from the apply of a logged MUTATE (`max_emits` > 0) or from resume hooks. Args are at most 16 B.
  Only ops declared `followup` may be targets. A follow-up replies at most once, and only to its source
  (registry.cpp:38-40, shard.cpp:2902-2947).
- Dispatch (`queue_emits`, shard.cpp:2949-2964): a **same-shard target is routed immediately, before durability**,
  on the reasoning that its record lands later in the same stream. Others wait in `emit_wait_` until the emitting
  LSN is `durable()` (released in `release_waiters`, 888-891).
- Not logged. Lost on crash and stop. The type's `resume` hook re-emits from object state after recovery
  (registry.h:71-73, snc.h:99-107, finish_recovery 1851). Idempotence is the application's job (account
  outbox/inbox seq). D.1/D.2 intake gates (WAL gate H, window W, exchange budget X, owner gate, link hold L) are in
  shard.cpp:2966-3550 and snc.h:554-595.

### 4.5 Sessions and dedup, V

| Aspect | Behaviour | Evidence |
|---|---|---|
| Storage | a System.Session object per (client_id, shard). SessionV1 has client_id, last_seq, last_id, last_version, last_status, last_app_code, touched_s (wall clock u32) | session.h:17-26 |
| Applies to | MUTATE (`run_exec`) and CREATE (`run_create`). **Deletes are never sequenced** | shard.cpp:561-565, 717, 404 |
| Check | `seq <= last_seq` means replayed: seq == last returns the stored status, id and version. Lower seq returns E_REPLAYED_OLD. Seq > last means run (gaps accepted) | shard.cpp:1187-1211 |
| Not checked | target, op and args are **not compared**. last_id is returned as `r.id` | shard.cpp:1192-1199 |
| Recorded | **only on success**, after apply, in the same atomic WAL group. Failed sequenced ops (E_APP, VERSION_CONFLICT, NOT_FOUND, OVERLOADED) are not recorded, so a resend re-executes | shard.cpp:700-706, 777-780, 1213-1243 |
| Order relative to refusals | in `execute`: standby or withdrawn refusals first (173-174). E_MOVED is skipped for sequenced (181). Under the follow-up backlog, a replay is answered before E_OVERLOADED (196-212). E_NOT_OWNER comes without a replay check (222-229). In `run_exec`: withdrawn or standby, then **replay**, then the serving (seal) check, then lookup, validate and admission (542-660). So a replay precedes admission and seal refusals | V |
| Replay wait | a replayed answer waits for the session record's durability under the waiter budget. If refused, it returns E_OVERLOADED with replayed=1 | shard.cpp:1203-1210, 321-343 |
| TTL | `expire_sessions` deletes sessions with `touched_s + ttl < now` using `::time(nullptr)` (wall clock). It runs **only at the start of `Engine::checkpoint()`**, primary only. Default TTL is 3600 s | engine.cpp:1432-1437, shard.cpp:1245-1261, snc.h:421-424 |
| TCP framing | a sequenced flag with args shorter than `SeqPrefix` silently becomes **unsequenced** (client_id 0) | server.cpp:131-137 (V; minor) |

---

## 5. Persistence formats inventory

| Format | Defined | Version field | Checksum | Unknown version or corruption handling |
|---|---|---|---|---|
| Superblock (80 B in a 4 KiB block, copies a and b, alternate by generation parity) | wal_format.h:16-33, storage.cpp:199-243 | magic `SNCSUPR1` plus `format` (=1) plus `flags` (kSbRoleTracked) | CRC32C over the 4 KiB block with the crc zeroed | a copy with a bad magic, format or crc is **skipped silently**. The valid copy with the highest generation wins (storage.cpp:210-226). If none is valid and data is present, start is refused (engine.cpp:935-941) |
| WAL segment header (48 B in block 0) | wal_format.h:42-51, storage.cpp:73-92 | magic `SNCWAL01` plus `format` | CRC32C over the struct | bad header: fatal, unless it is the **last** segment and all zero, which ends the scan (storage.cpp:378-382) |
| WAL record (56-B header plus payload, 8-aligned) | wal_format.h:53-74, wal_format.cpp:29-59 | **none per record**. `rec_type` is in {1,2,3,5} | CRC32C over [4,total) | invalid or unknown rec_type gives `check_record` = 0, so an **unknown record type is treated as a torn tail** in the newest segment (silent truncation) and gives C4 abort in an earlier one (I, from storage.cpp:388-401, 383-385) |
| Atomic group | `kRecGroupNext` flag, wal_format.h:55-58 | none | per record | an incomplete group at the end of a segment is dropped (storage.cpp:416-434) |
| Segment naming | `wal/sNNN/<first_lsn:016x>.wal`, spare `spare.tmp` | name = LSN | none | `adopt_spare` renames a spare with a valid header (storage.cpp:797-812) |
| Checkpoint part | `ckpt/<seq>/part-NNN.ckpt`: FileHeader `SNCCKP01` (seq, first_lsn=start_lsn) plus IMAGE records | header format | header CRC, per-record CRC | any problem is fatal. The object count is checked against the manifest (storage.cpp:483-506, engine.cpp:1093,1156) |
| Manifest | `ckpt/<seq>/manifest` (tmp, rename, fsync dir), storage.cpp:286-359 | magic `SNCMANI1` plus u16 format | trailing CRC32C over everything | invalid gives nullopt. If the superblock names it, start fails fatally (engine.cpp:1018-1021) |
| Session payload | SessionV1 (48 B) | the "V1" name, schema_ver 1 by default | via the record | `check` = length only (session.cpp:12) |
| Slot state payload | SlotStateV1 (40 B) | the "V1" name | via the record | (slot_state.cpp, not deep-read) |
| DIR/role | `<dir>/role`, text "primary\n" or "standby\n" | none | none | anything else reads as MALFORMED, which forces the start barrier (engine.cpp:186-198, 985-986) |
| links_generation | `<dir>/links_generation` | U | U | out of scope (snc.h:596-606) |
| Seed wire | seed.h:17-41 (`SNCSEED1`) | magic | none on the wire beyond file contents | out of scope |
| Format constant | `kFormatVersion = 1` shared by all files | wal_format.h:10 | none | any other value gives "invalid" (no upgrade or downgrade path) |

CRC32C is table-driven, in software (wal_format.cpp:10-27). V.

---

## 6. Durability and recovery

### 6.1 Acknowledgement vs WAL sync, V

- `append` buffers the record (storage.cpp:602-621). `poll` flushes once `flush_bytes` (64 KiB) are buffered or the
  oldest record is `group_commit_us` (50 us) old (642-649). At most one flush is in flight. The flush is padded to
  4 KiB and written with **`RWF_DSYNC`** via io_uring writev2, or synchronous `pwritev2` as the fallback
  (runtime.cpp:211-234). The segment is O_DIRECT, falling back to buffered (storage.cpp:543-551).
- `flush_done` sets `durable_lsn` and calls `on_durable`, which releases waiters in LSN order
  (storage.cpp:814-827, shard.cpp:887-917).
- `durable(lsn)` = local durable **and**, if sync replication holds, `lsn <= standby acked`
  (shard.h:555-559). A DURABLE completion and a REMOTE response are sent only then. APPLIED returns at once with
  `durable=false`. Creates always wait (shard.cpp:784).
- Reads wait for durability of the object's `last_lsn` unless `allow_undurable` is set (shard.cpp:624-627).
- `wal_sync=false` drops RWF_DSYNC, yet acks are still reported as durable ("development only", snc.h:373,
  storage.h:141).
- Replication ships each flush in `start_flush` **before** the local write is issued (storage.cpp:652). V

### 6.2 Recovery order (Engine::recover), V, engine.cpp:877-1255

1. role file and adopt checks
2. superblock (or seed fetch or init)
3. manifest and type catalog
4. slot map
5. lease reservation plus superblock commit (1040-1052)
6. runtime start
7. `adopt_spare` per stream (1065)
8. load parts in parallel per shard, verify owner and count (1076-1098)
9. `scan_stream` from `start_lsn` per stream (1100-1112). The remap path routes records from the control thread (1113-1176)
10. lease rebase (1190-1208)
11. `finish_recovery`: rebuild the index, a **new WalStream at next_lsn** (unlinks any file of that name, storage.cpp:526), reseal, resume hooks, links (shard.cpp:1822-1856)
12. forced checkpoint if remapped, orphaned or fresh (1228-1234)

Install rule: install if `version > existing`, so redo is idempotent. DELETE erases if `version >` existing. An
image or record over `max_size` or failing `check` aborts (shard.cpp:1781-1820).

### 6.3 Checkpoint (fuzzy), V

`begin_checkpoint`: `start_lsn = wal.next_lsn()`, snapshot of live IDs (shard.cpp:2318-2322). The walk encodes the
current image of each ID still live, with `lsn=last_lsn`, and tracks `max_lsn` (2650-2668). It waits for
`lsn_local_durable(max_lsn)` (**local only**, comment 2709-2715), then fdatasync, possibly under an S4 token
(2723-2748). Engine: fsync the ckpt dir, write the manifest, then the superblock (`active_ckpt=seq`), then delete
the old ckpt and segments wholly below `start_lsn`, minus the replica and seed retention (engine.cpp:1438-1560).
Session expiry runs first (1432-1437).

### 6.4 Failure handling matrix

| Case | What the code does | Evidence | Label |
|---|---|---|---|
| Torn tail (partial last flush, non-zero garbage) | `++torn_tails`, stop that segment. The stream continues in a new segment at `last+1` | storage.cpp:396-401, engine.cpp:1106 | V |
| Unwritten rest (zero block at a block boundary) | stop, **not counted** | storage.cpp:393-395 | V |
| Mid-record corruption in the newest segment | same as a torn tail: every later record in the segment is dropped. Start succeeds and acknowledged data is lost (**D2**). `torn_tails` is only stored in `RecoveryReport` | storage.cpp:388-401. Experiment below | V |
| Corruption in an earlier segment | the next segment's `first_lsn != expected` gives `storage_fatal` "WAL gap (C4)", exit by abort | storage.cpp:383-385. Experiment d2b aborted with that message | V |
| LSN hole inside a segment | counted as a torn tail, break | storage.cpp:409-411 | V |
| First record missing relative to the checkpoint | fatal "WAL missing records" | storage.cpp:407-408 | V |
| Bad newest superblock copy | silently falls back to the older valid copy | storage.cpp:210-226 | V |
| Both superblocks bad | refuse to start (runtime_error) if data exists | engine.cpp:935-941 | V |
| Manifest named by the superblock missing or bad, or uuid mismatch | `storage_fatal` | engine.cpp:1017-1021 | V |
| Part missing, short, bad header, bad record, non-IMAGE record, count mismatch | `storage_fatal` | storage.cpp:483-506, engine.cpp:1093,1156 | V |
| Record in the wrong shard's stream | fatal | engine.cpp:1082 | V |
| Invalid image (unknown type, too big, check fails) | abort | shard.cpp:1793-1798 | V |
| EIO, short write or ENOSPC on a WAL flush | `storage_fatal` (abort), fail-stop C7, never retried | storage.cpp:814-819 | V |
| EIO on WAL segment create or fsync | fatal | storage.cpp:551-565 | V |
| fallocate fails (e.g. ENOSPC) | falls back to `ftruncate` (sparse file). ENOSPC then surfaces on a later flush, which is fatal | storage.cpp:554-557 | V |
| Checkpoint chunk write or final fdatasync error | fatal | shard.cpp:2457-2465, 2553-2556 | V |
| Superblock, manifest or role write or fsync error | fatal | storage.cpp:60-71, 236-242, 315-320, engine.cpp:205-208 | V |
| Read error during recovery | fatal | storage.cpp:47-53 | V |
| Crash between parts synced and manifest, or between manifest and superblock | the previous checkpoint is used (superblock not yet switched) | engine.cpp:1446-1479. Tests hold points exist (snc.h:684-696) | V (code), D (tests) |
| Spare rename lost in a crash | `adopt_spare` by header | storage.cpp:690-715, 797-812 | V |

### 6.5 Experiments run (scratchpad/exp)

- **d2** (1 shard, 200 durable creates acknowledged with `durable=true`, clean stop, one bit flipped at byte 34096
  of the only segment): restart OK, **objects=117** (83 acknowledged lost), `torn_tails=1`. A second restart gave 117.
- **d2c** (8000 creates, 2 segments, one bit flipped in the **first record** of the newest segment, byte 4116):
  restart OK, **objects=4032** (3968 acknowledged lost). The **third** start reports `torn_tails=0`: the corrupted
  segment was replaced, so no evidence remains (see N1).
- **d2b** (corruption mid-record in the older of 2 segments): abort with "WAL gap before segment (spec C4)".
- **d3** (in-memory, 1 shard): client 7 seq 1 deposit on A returned OK. Then client 7 seq 1 deposit on **B**
  returned `OK replayed=1` with `id==A`. Balances: A=105, B=100.

---

## 7. Runtime and memory

| Topic | Fact | Evidence | Label |
|---|---|---|---|
| Thread model | one std::thread per shard. The control thread uses `submit` (mutex-guarded vector plus eventfd) and `run_on_all` (latch) | runtime.cpp:585-633 | V |
| Pinning | `sched_setaffinity` to `cpus[i]`, or the i-th allowed CPU. Done before allocations (first-touch) | runtime.cpp:487-511 | V |
| NUMA | **no binding in code**. `numa_node` is only reported via getcpu. The mimalloc arena uses `numa_node=-1`. There is no mbind or set_mempolicy anywhere (grep) | runtime.cpp:505-509, allocator.cpp:25 | V |
| Rings | SPSC of 64-B `Msg` (static_assert in msg.h:65). The header comments say "128-byte" (msg.h:1, runtime.cpp:2), which is stale. Default 256 slots, tail published once per iteration (or by `sri_publish`) | ring.h:15-56, msg.h:44-66 | V |
| Overflow | per-destination unbounded `std::deque`. Requests are refused at `overflow_limit` (256), but **must_deliver (responses, buf returns) are unbounded** | runtime.cpp:126-140, 167-193 | V |
| io_uring | one ring per worker (depth 256), eventfd registered, optional iowq affinity. If the SQ is full or init fails: synchronous `pwritev2` or fdatasync on the shard thread | runtime.cpp:211-266, 514-527 | V |
| io_yield (F1 fix) | while IO is in flight, every `io_yield_us` (50): `sched_yield`, or once the oldest op is older than `io_block_after_us` (1000 us), `clock_nanosleep(1 us)` | runtime.cpp:422-444, native_runtime.h:29-38 | V |
| Idle | spin `idle_spin_us` (50), then a Dekker handshake, then `poll(efd + watched fds, 1 ms)` | runtime.cpp:453-478 | V |
| Stop | the loop exits only when `quiesced()`: WAL idle, no waiters, no checkpoint, no pending, standby acked (shard.cpp:1167-1172) | runtime.cpp:385-389 | V |
| Allocator | a mimalloc heap per shard, optional huge-page arena (THP or HUGETLB with THP fallback) plus a spill heap, live-bytes limit (`heap_limit_bytes`, default 0 = unlimited). The comment says only the owner frees. Cross-shard buffers come back via BUF_RETURN | allocator.cpp:15-87, pages.cpp:28-64, shard.cpp:1445 | V |
| Local map | one-step rehash (stop-the-shard), a documented spec deviation | local_map.h:9-11 | V |
| WAL buffer | doubles from 2 MiB to a 1 GiB hard max. Beyond that, fatal | storage.h:121-133, storage.cpp:584-600 | V |
| S1 admission | an op reserves its worst case after validate and before apply: WAL bytes `record_size(max_size)+peer+session`, waiter bytes. Refused only if the buffer or waiter list is non-empty. A pending budget limits forwarded requests (131072). Follow-up backlog gate `kEmitBacklog`=1024 | shard.cpp:282-304, 643-660, 738-742, 820-823, 345-354, shard.h:448 | V |
| S2 pacing | time-budget walk via a pure controller | ckpt_pacer.h, shard.cpp:2632-2645 | V |
| S3 writeback | RWF_DSYNC chunks with a window of 4. The walk stops while the window is full | snc.h:535-543, shard.cpp:2614-2621 | V (partial read) |
| S4 tokens | at most K concurrent final fdatasyncs (default 0 = unlimited) | sync_tokens.h, shard.cpp:2723-2748 | V |
| Configured but unused or inert | `kFlagDirty` and `kFlagIndexed` (never referenced). `heap_limit_bytes` defaults to 0. `ckpt_sync_tokens` defaults to 0 (inert). `MsgKind::INDEX_QUERY/RESP` comments say "reserved" but they are used (msg.h:13-14 vs shard.cpp:1443) | grep | V |
| Seastar | "shards never sleep", "sends never refused" (unbounded) | runtime.cc:16-18 | D (comment) |

---

## 8. Status re-check of recorded defects

| ID | Still present? | Current evidence |
|---|---|---|
| **D1** cross-shard overtaking | **YES** | shard.cpp:1473-1494: a batchable EXEC goes into `out_batches_`. shard.cpp:1497-1531: a non-batchable request is claimed and committed on the ring at once. Batches go out only at kBatchMax (1492) or at the end of the iteration (engine.cpp:146). Nothing flushes the batch to `dst` before an immediate send. Sequenced requests are exempt (shard.cpp:222-229). Not re-run by me; the code path is unchanged from the audit description. |
| **D2** silent loss on mid-record corruption of the newest segment | **YES, re-run** | storage.cpp:388-401 (`++out.torn_tails; break;`). `torn_tails` is summed into `RecoveryReport` only (engine.cpp:1105, 1167). grep shows no reader in tools/ or src/net (only tests). Experiments d2 and d2c above. Worse than recorded: N1. |
| **D3** client_id reuse gives a false OK replayed | **YES, re-run in-process** | shard.cpp:1192-1199 returns the stored status when `seq == last_seq` without comparing target or op. Experiment d3. |
| (d) session TTL evaluated only in checkpoints | **YES** | engine.cpp:1432-1437 (now at those lines, previously 1423-1428). `expire_sessions` has no other caller (grep: shard.cpp:1245 and engine.cpp:1436 only). Wall clock `::time(nullptr)` in both record (shard.cpp:1236) and expiry (engine.cpp:1435). |

---

## 9. New potentially critical findings (verified in code)

**N1. D2 also destroys its own evidence, and a single bit can drop a whole segment.** V, reproduced (d2c).
- Code: when the corrupt record is the first record of the newest segment, `last_lsn` = end of the previous
  segment, so `next_lsn` = that segment's `first_lsn` (engine.cpp:1106). Then `WalStream` unlinks
  `segment_path(next_lsn)`, which is the corrupted segment itself, and creates a fresh one (storage.cpp:522-527).
  The ctor comment assumes such a file "holds only a header, zeros or an unacknowledged tail" (522-525), and that
  is false for corruption.
- Result: every acknowledged record of that segment (3968 in the run, up to 64 MiB of WAL by default) is lost.
  The next restart reports `torn_tails=0`, and no forensic copy remains.
- In the general mid-segment case the old file survives until checkpoint reclaim deletes it
  (engine.cpp:1546-1556). Either way, new records **reuse the LSNs** of the lost acknowledged records with different
  content (I). A standby that already holds the old ones diverges silently unless the replication handshake
  (tail_crc, engine.cpp:1214) catches it. That needs reviewer B: U here.
- Confirm further: run d2c with replication on and compare the standby with the primary after restart.

**N2. Cross-shard atomicity loss through D2 and follow-ups.** I, from V code.
- A follow-up to another shard is dispatched once the emitting record is durable (shard.cpp:2958-2962), and it is
  applied and logged in the destination's stream.
- If D2 then drops the source's record (corruption after the ack), the destination keeps the credit while the
  source's debit and outbox entry are gone. For Account transfers this creates money, and the resume hook cannot
  repair it because the outbox entry no longer exists.
- Confirm: transfer A to B across shards, wait_emits, stop, corrupt A's record in its stream, restart, and check
  the sum of balances.

**N3. An unknown WAL record type truncates silently.** V (code), I (impact).
- `check_record` returns 0 for `rec_type` outside {1,2,3,5} (wal_format.cpp:56). In the newest segment that is the
  torn-tail path (storage.cpp:396-401).
- So a WAL written by a future binary with a new record kind, and read by this one, silently drops data from that
  record on. Records have no version field (wal_format.h:60-72).
- Confirm: hand-craft a record with rec_type=4 and a valid CRC mid-segment.

**N4. Session wall clock.** V (code).
- `touched_s` comes from `::time(nullptr)` (shard.cpp:1236), and expiry compares against wall clock
  (engine.cpp:1435). A forward clock step (NTP, VM resume) expires live sessions at the next checkpoint.
- A resend of an applied seq after that re-executes: a double apply. A backward step makes sessions effectively
  immortal.
- This is in addition to the checkpoint-only evaluation in (d).

**N5. Session identity is not bound to the request** (the root of D3). V.
- Beyond reuse, any client bug that repeats a seq on a different target gets a false OK.
- The stored `last_id` is returned in `r.id` (shard.cpp:1196), so only a caller that compares `r.id` with its
  target can detect this.

**N6. Deletes and failed ops are unsequenced.** V.
- `remove` never consults sessions (shard.cpp:404), and failed sequenced mutations are not recorded
  (shard.cpp:700-706).
- A resend after an in-doubt E_FENCED or lost answer on a delete, or after an E_APP that later would succeed,
  re-executes. Documented for deletes (comment at 404). The second part is I as to intent.

### Speculative (not shown to cause damage, needs work)

- **S-a Superblock fallback can regress leases and term.** If the newest superblock copy goes bad (media), the
  older one is used silently (storage.cpp:219-223). Its `next_lease` may equal leases already handed out, and its
  `term` may predate a promotion. Rebase covers only leases of objects still present (engine.cpp:1190-1208). So IDs
  of deleted objects could be re-minted, and the fencing term could step back. Needs a corrupted-superblock
  experiment, and reviewer B for the term.
- **S-b An unacknowledged delete can become permanent through a checkpoint.** A DELETE at lsn at or above
  start_lsn removes the object from the image set, but the checkpoint waits only for `max_lsn` of the visited
  images (shard.cpp:2650-2668, 2723), not for the DELETE's LSN. After a crash, the object is gone with no durable
  DELETE. The outcome is in-doubt either way, so this is probably acceptable. Note it for a Rust contract.
- **S-c Local follow-ups run before the emitting record is durable** (shard.cpp:2958). This is correct only while
  the local WAL is the sole source of truth. Under sync replication, the follow-up's effect is visible and may be
  served (as an APPLIED read) before the root is standby-acked. I believe prefix shipping covers it. Not verified.
- **S-d The system xxhash library version determines placement.** XXH3 output has been stable since 0.8.0, but
  there is no pinned vector test (test_object_id.cpp:52-53). A different library could silently remap every
  persisted object to other slots, and the owner check at recovery would then abort ("record in the wrong shard's
  stream", engine.cpp:1082). So this fails closed only when the shard count is unchanged.
- **S-e Unbounded `must_deliver` overflow** (runtime.cpp:135, 176) under a stalled consumer: memory growth with
  no bound. Not shown to occur.

---

## 10. C++ assumptions a Rust-native design should not inherit (evidence-based)

1. **A truncate-on-first-bad-record WAL scan without a durable high-water mark** (storage.cpp:388-411). Separate
   "torn tail" (only after the last acknowledged flush) from "corruption" by persisting a durable-LSN watermark or
   using per-flush trailers, and fail closed or report loudly otherwise. Never delete or reuse a segment that held
   records (storage.cpp:526).
2. **Ordering as an accident of batching** (shard.cpp:1473-1531). State an explicit per-(origin, owner) FIFO
   contract, and make batching preserve it.
3. **Sessions keyed only by (client_id, seq) on wall-clock TTL, evaluated in the checkpoint path**
   (shard.cpp:1187-1243, engine.cpp:1432-1437). Bind the dedup entry to a request fingerprint, use a
   server-issued client identity and a monotonic or logical clock, and expire on an independent schedule.
4. **Global mutable process registry and thread-local current shard** (registry.cpp:78-81, shard.cpp:66,
   tl_shard). Pass explicit context, and support more than one engine per process (snc.h:349 "One Engine per
   process").
5. **`abort()` as the error model for contract violations and storage faults** (storage.cpp:103-106,
   registry.cpp:9-12, contract_violation). Keep fail-stop for storage, but type the app-contract violations.
6. **Implicit format versioning**: one `kFormatVersion` for all files, no per-record version, and schema_ver
   checked only via the manifest catalog (wal_format.h:10, shard.cpp:1793). Version each record kind and define
   unknown-version behaviour.
7. **Raw struct memcpy of LE layouts** (static_asserts types.h:16-17, wal_format.h:33,51,73). Use explicit codecs.
8. **Unbounded containers on the hot path**: overflow deques (runtime.cpp:139), `pending_` growth
   (shard.cpp:1459-1461), waiters `std::list`-like insertion (shard.cpp:880-882), one-step map rehash
   (local_map.h:9-11).
9. **Full-image redo logging per mutation** (shard.cpp:533-538). It simplifies recovery (no op replay) but costs
   write amplification up to 1 MiB per op. Choose deliberately.
10. **NUMA by first-touch only** (allocator.h:1-3, no binding in code). Make placement explicit.
11. **Stale comments in the code** (128-byte Msg vs 64, "reserved" INDEX kinds, unused flags). Do not port
    comments as spec. Port the verified behaviour.
