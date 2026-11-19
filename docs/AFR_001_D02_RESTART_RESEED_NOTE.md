# AFR-001: D-02 restart safety, spare and temporary files, per-stream reseed, acknowledgement semantics (design note, PROPOSED)

Status: **PROPOSED, for Codex and owner review.** Docs only. Nothing here is accepted, frozen or implemented.

Authority: Codex's review of #168 (`docs/handoff/PR168_Codex_Review.md`), "Next authorized task": "Prepare one focused
design note, documentation only", sections A-E, with independent three-way adversarial review, then stop. The owner
decisions it builds on:
- OD-1 to OD-6 (`docs/handoff/PR163_Owner_Decisions.md`, `docs/handoff/PR165_Owner_Decisions.md`);
- **OD-7, OD-8, OD-9** (`docs/handoff/PR168_Owner_Decisions.md`, cody, 2026-11-18 23:09 UTC), as design directions
  only: R-CHAIN on every retained segment after the first; HeaderFirst with `RENAME_NOREPLACE` and a defined
  `spare.tmp` namespace; a replaced segment keeps its nonce and identity.

Still open and not touched here: D-02 (HOLD), R-23, O-FLOOR, final D-06 acceptance, Y4, Y5 and Y6 (HOLD). Not
authorized: a TLA+ revision, X-2, R640, C++, Rust, migration.

Labels: **[FACT-SNC]** read in this repository at `107b157`; **[FACT-SRC]** read in Linux v6.12 (`torvalds/linux`
tag `v6.12`); **[INF]** argued here, not model-checked; **[HYP]** untested guess. Section 6 records the independent
review.

Prerequisite reading: `docs/AFR_001_D02_ACCEPTANCE_REVIEW.md` sections 4-6 (the facts this note builds on) and
`docs/AFR_001_D02_CRASH_DESIGN.md` (the design it revises).

## A. V8: restart safety against writes from a dead process

### A.1 What the kernel does [FACT-SRC, from the acceptance review section 5 and its storage review]

- At exit, io_uring drains its io-wq workers (`io_uring_clean_tctx`, `io_uring/io_uring.c:3219`, then
  `io_wq_put_and_exit`, `io_uring/tctx.c:193`), so buffered writes, `fdatasync` and synchronous `O_DIRECT` finish
  before `exit_files` (`kernel/exit.c:934`).
- **`O_DIRECT` writes already submitted asynchronously are not waited for.** The tracked-inflight wait
  (`io_uring/io_uring.c:3174`) counts only requests whose target is an io_uring file (`:1907-1915`).
- AIO is drained in `exit_aio` (`fs/aio.c:925-928`) from `exit_mm`, unless another task holds the address space.
- Each io_uring request on a non-registered file holds a reference to that open file description (`fget` in
  `io_file_get_normal`, `io_uring/io_uring.c:1907`). `flock` locks are dropped in `__fput` by `locks_remove_file`
  (`fs/file_table.c:423`), which runs only when the last reference goes.
- The same holds when the process dies by its own `storage_fatal`, which calls `abort()`
  (`src/core/storage.cpp:103-106`) while other workers' writes are in flight [FACT-SNC].

### A.2 What SNC writes asynchronously [FACT-SNC]

| File | How it is written | Exposed to a stray write? |
|---|---|---|
| WAL segment | `sri_write`, io_uring `writev2`, `O_DIRECT` when the file system allows it (`storage.cpp:541-546`, `:687`; `src/native/runtime.cpp:211-233`) | **yes** |
| checkpoint part | `sri_write`, buffered (`shard.cpp:2326`, `:2426`) | no: drained at exit |
| manifest, role, spare format, segment header today | synchronous `write`/`pwrite` (`storage.cpp:286-321`, `:560`; `engine.cpp:204`) | no |
| node record (design 2) | proposed: two `O_DIRECT` writes, "io_uring or AIO" | **yes**, if io_uring is used |
| watermark (design 4) | proposed: `O_DIRECT | RWF_DSYNC` through a serial writer | **yes**, if io_uring is used |

The one-engine directory lock (`engine.cpp:897-905`) is a non-blocking `flock` on its own `O_DIRECTORY` descriptor.
No write references it, so it is released at `exit_files`, while `O_DIRECT` writes may still be in flight [INF from
the two facts]. **It does not protect against V8, and this note does not rely on it for that.**

### A.3 The rules

**V8-1. After every restart, the writer never appends into an inode that existed before the restart.**
- Recovery ends with the writer's first segment at `E + 1`, created by 3(a): `seg.tmp`, formatted whole, `fdatasync`,
  `rename`, directory `fsync` (section B).
- If a file named `E + 1` already exists, it is **replaced** this way (3(b)), keeping its nonce (OD-9). It is never
  "opened as is" after a restart. "Open as is" stays allowed only for a segment created during this boot.
- This closes the BLOCKER of the acceptance review (section 5): a header-only newest segment whose first flush was in
  flight at the kill. It is what today's code already does by `unlink` + `O_EXCL` (`storage.cpp:519-527`), done
  with `rename` so the name never goes missing (D-06 item 9's purpose).

**V8-2. Every file that is overwritten in place (the two record files and each stream's watermark file) is protected
by V8-L.** [Widened 2026-11-19 to every file written by async `O_DIRECT`, segments included, with a sync before
the recovery raise: `docs/AFR_001_DURABILITY_CONTRACT.md` 2.3, E26.] Recommended over V8-A; the reason is in A.4.
- **One open file description per file per process.** The file is opened once, with `O_CLOEXEC`, and that
  descriptor issues every read and write. A second `open` of the same file by the same process is forbidden, because
  `flock` on a second description would deadlock against the first.
- **The lock.** Immediately after opening, the process takes `flock(fd, LOCK_EX)`, blocking, and holds it for the
  life of the descriptor. Nothing ever unlocks it explicitly.
- **Recovery order.** Recovery takes the directory lock first (unchanged), then the V8-L locks in a fixed order
  (`superblock.a`, `superblock.b`, then each watermark file by stream index), each before the first read of that file.
  A lock held by a dead process is released only when its last in-flight request drops its file reference, which is
  after that request's bio completed (A.1) [INF from FACT-SRC].
- **New files.** A watermark file is created through `wm.tmp` + `rename` + directory `fsync` (section B). The
  `wm.tmp` descriptor is closed first; the file is then opened and locked once under its final name before its first
  in-place write (the two names are one inode).
- **When watermark files are created.** At init, every stream's watermark file is created durably **before** record
  generation 1 is written (design 2 step 2), as the record files themselves are. A reseed creates its stream's file in
  RS-4, while the stream is RESEEDING. From then on, **a NONE stream with no watermark file is damage: refuse.** A
  crash during creation leaves either no valid record (init restarts) or a RESEEDING stream (the reseed restarts), so
  this refusal never follows a crash alone [INF] (review of this note, MAJOR 1).
- **Today's recovery reads the record before any lock**, through separate descriptions (`engine.cpp:919`, `:933`,
  `:941`, after the directory lock at `:900`). Under V8-L those reads move behind the locks. That is an implementation change, listed in E10.
- **No registered files** (`IORING_REGISTER_FILES`) for these descriptors: a registered file's reference lives in the
  ring's table, not in the request [FACT-SRC for the `fget` path; SNC registers none, `src/native/runtime.cpp:211-233`].
- **No `fork` while the descriptors are open.** SNC does not fork [FACT-SNC: no `fork` in `src/`].

**V8-3. No other in-place file is written by asynchronous `O_DIRECT`.** Checkpoint parts stay buffered, or are
created fresh (`O_EXCL` after an `unlink`) if they ever become `O_DIRECT`. Today's `O_TRUNC` reopen of a part
(`shard.cpp:2325-2326`) is safe only because parts are buffered.

**Space [HYP].** V8-1 allocates `seg.tmp` before the rename frees the old file, so a replacement needs one free
segment at that moment; today's `unlink` + `O_EXCL` needs none (`storage.cpp:526-527`). [2026-11-19: usually no `E + 1`
exists, so V8-1 creates one new segment per stream and recovery needs one segment per stream beyond the state before
the crash; `docs/AFR_001_DURABILITY_CONTRACT.md` 2.5.] On a nearly full disk a kill
could become a recovery that fails with `ENOSPC`, which is a G-48 question. Reserving that space is left open.

**Other files, for completeness [FACT-SNC]:** `links_generation` and its `.tmp` (`links_generation.cpp:117-134`),
`role.tmp` (`engine.cpp:201-212`) and `manifest.tmp` (`storage.cpp:314-320`) are written synchronously, or are
temporary files unlinked at recovery. None is exposed.

### A.4 V8-L against V8-A: recommendation V8-L

| | V8-L (lock on the I/O descriptor) | V8-A (record and watermark writes only through synchronous calls or AIO) |
|---|---|---|
| What it relies on | a request pins the open file; `flock` dies with the last reference (A.1) | `O_DIRECT` synchronous writes wait in the kernel; AIO drains at `exit_mm` |
| Record writes (rare, serial, D-06 4.1 already says `pwrite`) | either way | natural fit |
| Watermark writes (one per flush, on the write path) | unchanged: stay on the shard's ring | a synchronous device write per flush on a pinned shard core, or a new thread per node and a cross-thread handoff per flush |
| New code | one `open` + `flock` per file, one blocking `flock` per file at recovery | an I/O path beside `sri_write` |
| Failure mode | a hung device holds the lock, so recovery waits; nothing else can progress then either | AIO's drain is skipped if anything else holds the address space |

**Recommendation: V8-L [INF].** It is one rule for both files, it leaves the write path and the runtime alone, and
it costs nothing per operation. Record writes may still be synchronous, as D-06 4.1 says, and V8-L then simply holds.
V8-A would put a device write on the shard core's critical path for every flush, or add a thread, which is the
opposite of KISS for the hot path. (HeaderFirst does put a DSYNC write, a rename and a directory fsync on the shard
core at each rollover, which today's code avoids on purpose, `storage.cpp:676-677`. That is once per segment, not per
flush; its cost is unmeasured.)

**Recovery time [INF].** A V8-L wait ends when the dead process's last `O_DIRECT` write completes or fails, bounded by
the block layer's I/O timeout. The bound is the device's, not SNC's, and it is UNRESOLVED as a G-48 number (G-48 asks
for a stated bound).

### A.5 Crash-only refusal under G-48 [INF]

With V8-1 to V8-3, every place a stray write can land, and what recovery then does:

| Where the stray lands | Result |
|---|---|
| a record or watermark slot | impossible: recovery reads only after the dead process's last request released the lock |
| the newest segment `E + 1` | in the old inode only, which V8-1 replaced; the new boot never reads or writes it |
| an older segment past `E` | void by name (3(c), 3(d)): `E + 1` exists, so bytes past `E` are never judged |
| a segment past `L` after a later truncation | void by name, as above |
| an unlinked inode (replaced or deleted file) | invisible, while pinned |
| freed blocks, reused later (A.6) | visible only after a lost write (a fault) |

So no stray write makes a crash or kill alone refuse, and no stray write is read as acknowledged state.

Remaining G-48 points, not caused by V8:
- the directory lock is `LOCK_NB` (`engine.cpp:900`). A restart while the old process is still inside `do_exit` is
  refused and must be retried by the supervisor. That is a restart-policy question, not an operator step [INF];
- the V8-L wait bound (A.4).

### A.6 Block reuse and the remaining C15 interaction [INF, UNRESOLVED as a model question]

**The path** (storage review of #168, MAJOR 4):
1. A stray first flush lands in the old inode of `E + 1` after V8-1 replaced it.
2. The request completes, the inode's last reference goes, and its blocks are freed.
3. A later `seg.tmp` (for example a second replacement of the same name after another crash) reuses those blocks.
4. Its format write is lost (the one fault).
5. The stray frame then sits at the same name with the same nonce (OD-9), with valid CRCs.

**Why this is not silent loss [INF]:**
- The stray frame is the dead process's own write, at its own `(term, LSN)`. By G-15 that identity names that same
  content everywhere. The new boot creates nothing in the old term (R6.0).
- If the new boot appended after step 3, its frames start at the same offset and overwrite the stray frame's header.
  What remains of the stray is not a valid frame, sits above `W`, and is neither a torn frame of this writer nor a
  format sector, so recovery **refuses** (design 4, "above the watermark"). That refusal follows a fault, which is
  allowed.
- If the new boot appended nothing, the stray frame is a complete, unacknowledged tail above `W`, of the kind a crash
  leaves when the last flush landed. Recovery keeps it, and reconciliation (R6.2) treats it as any unacknowledged
  tail.
- It is close to SegChain's PStale case (a lost format write brings back the replaced file's own sectors), which
  passed (`docs/AFR_001_D02_MODEL.md` section 19). PStale never held a complete frame written after the replacement,
  so this case is only argued.


## B. Spare header and temporary files

### B.1 Name spaces in a stream directory

| Name | Contents | Adopted by recovery? |
|---|---|---|
| `<16 hex>.wal` | a segment; `list_segments` reads only these names (`storage.cpp:178-187`) | it is the log |
| `spare.tmp` | the spare. Today's name (`storage.cpp:719`) | **the only file ever adopted by its header** |
| `seg.tmp` | 3(a) creation, 3(b) replacement, completion step 2.4 | never; unlinked |
| `wm.tmp` | creation of the watermark file | never; unlinked |
| `watermark` | the stream's watermark (design 4) | in place, under V8-L |

One fixed name each: at most one creation runs per stream at a time, because completion and a reseed close the writer
first (D-06 4.3 step 2.1; RS-0).

### B.2 HeaderFirst (OD-8)

**Prepare** (preparer thread): `unlink` any old `spare.tmp` (only after recovery has resolved it, B.4), create it
`O_EXCL`, format every sector with a fresh nonce (K1), `fdatasync`, directory `fsync`. Its header block carries no
take-time header: a format sector, or a header whose first LSN is 0.

**Take** (writer thread, at rollover):
0. (prepare's formatting writes, like every write in B.2 and B.3, are synchronous, or awaited before the
   `fdatasync` and the rename: `fdatasync` does not wait for an `O_DIRECT` write still in flight);
1. write the take-time header into `spare.tmp`: name and first LSN `X`, nonce, `space_uuid`, stream, `prev_term`,
   `prev_nonce`; `RWF_DSYNC`. Synchronous `pwrite`, so V8 does not reach it [INF].
2. `renameat2(spare.tmp, X.wal, RENAME_NOREPLACE)`;
3. `fsync` the directory;
4. only then the first flush, frames only.

Facts the order relies on:
- the take happens in `start_flush`, which runs only when no flush is in flight (OD-1; `storage.cpp:643`, `:651-660`).
  So every record below `X` is durable at the take, and **a crash alone always leaves `E ≥ X − 1`** [INF from
  FACT-SNC]. This needs `wal_sync` on; the development-only `wal_sync=none` (`include/snc/storage.h:141`) is outside
  every guarantee here;
- step 3 completes before any frame, so **a `spare.tmp` holding frames needs a lost rename**, which is a fault [INF];
- rename is atomic across a crash on ext4 and xfs, a file-system property (design 3(a)). So after a crash the name
  `X.wal` and a take-time header in `spare.tmp` never both exist unless a fault occurred [INF].

### B.3 Segment creation and replacement through `seg.tmp`

1. `unlink(seg.tmp)` if present; create it `O_EXCL`;
2. write the header (name, nonce, `space_uuid`, stream, `prev_term`, `prev_nonce`) and format every sector; `fdatasync`;
3. `rename(seg.tmp, X.wal)`. A plain `rename`: replacing `X.wal` is the purpose in 3(b) and V8-1;
4. `fsync` the directory;
5. only then the first append.

Header values:
- **Replacement** (3(b), V8-1): the old file's name, nonce (OD-9), `prev_term` and `prev_nonce`, read from its header.
  If that header is not valid, the named segment is damaged (under K1 every named segment was created whole), and
  recovery **refuses**.
- **New name:** a new random nonce; `prev_nonce` and `prev_term` from the segment that holds record `X − 1`; at a
  reseed, `prev_nonce` 0 and `prev_term = start_term` (C.2).

### B.4 Recovery of `spare.tmp`, `seg.tmp` and `wm.tmp`

Per NONE or PENDING stream (a RESEEDING stream skips B.4, because RS-2 removes every name), after the record is read
and before the floor check and the scan (D-06 recovery step 3, boundary rule 4.4),
and **before the preparer starts**, because its first act would be to unlink `spare.tmp`:
1. `unlink(seg.tmp)` and `unlink(wm.tmp)`, then `fsync` the directory. Neither was ever named, so neither holds
   acknowledged state [INF].
2. If `spare.tmp` has no valid take-time header **and no valid record**: `unlink` it. It was never named. If it
   holds a valid record without a valid take-time header: **refuse**. Under one fault this cannot follow a crash
   alone: frames need a durable rename first (B.2), so a spare with frames needs a lost rename, and a bad header as
   well would be a second fault [INF]. This keeps R0 step 1's rule that a spare "holding any valid record is ...
   never unlinked" (`docs/X2_PREREG.md` 8.1).
3. If `spare.tmp` has a take-time header with a valid CRC but another `space_uuid` or stream: **refuse** (a
   misdirected write or a misplaced file, so a fault).
4. If `spare.tmp` has a valid take-time header for `X` (and the right `space_uuid` and stream):
   - **if the stream is PENDING**, D-06 4.3 step 2.2 applies: adopt it if `X ≤ L`, otherwise unlink it;
   - otherwise `renameat2(spare.tmp, X.wal, RENAME_NOREPLACE)`, then `fsync` the directory. This completes an
     interrupted take (crash between steps 1 and 3 of B.2). The adopted file holds only a header, and V8-1 then
     replaces it at `E + 1`, or it holds frames after a lost rename (a fault), which the scan then reads. SegChain
     revision 5 modelled the other reading, refusing an unnamed spare that holds a valid frame
     (`docs/AFR_001_D02_MODEL.md` section 18); both are allowed after a fault, and adoption is **unmodelled** [INF].
   - **`EEXIST`: refuse**, with a named error.

**When `EEXIST` is legitimate [INF]:**
- at recovery, `X.wal` and a take-time header for `X` in `spare.tmp` coexist only after a fault: a lost rename, a lost
  unlink of an old `X.wal` (for example after a truncation), or a misdirected write. A crash alone cannot produce it
  (B.2, third fact). So refusing on `EEXIST` never refuses after a crash alone;
- at run time, an `EEXIST` from the take means the same, or a bug. The writer fails (`storage_fatal`), and recovery
  then refuses as above;
- `EEXIST` never arises from `seg.tmp` or `wm.tmp`, because their renames replace on purpose.

**Interrupted replacement:** a crash before step 3 of B.3 leaves the old `X.wal` and a `seg.tmp`. Recovery unlinks
`seg.tmp` (step 1) and V8-1 replaces again. A crash between steps 3 and 4 leaves either file at the name. The new one is a
valid header-only segment of the same nonce. The old one has the same header, and may also hold a torn first frame or
a stray frame that completed after the scan; recovery discards the first and keeps the second as an unacknowledged
tail, then replaces again. A **lost** rename (a fault) after appends began on the new inode loses
those appends; the watermark then names an LSN above the recovered end, and recovery refuses (design 4, `W > E`)
[INF; the acceptance review lists "a lost rename under 3(b)" as untested].

**Today's code differs** [FACT-SNC]: `take_spare` renames without a directory fsync and writes the header with the
first frame (`storage.cpp:671-687`, `:699-705`); `adopt_spare` uses a plain `rename` and unlinks a spare with a bad
header (`:797-812`). HeaderFirst changes all three.

## C. Per-stream reseed (R6.5)

### C.1 What exists [FACT-SNC]

- R6.4 says when a member is reseeded; R6.5 sets a durable `incomplete` flag "before `k` discards anything, and
  cleared only once the copied state is durable and positioned at a `(term, lsn)` of `j`'s log"
  (`docs/AFR_001_SAFETY_ANALYSIS.md` 215-228).
- D-06 4.1 makes the flag a per-stream record state, RESEEDING. It is part of the node's one metadata authority (owner
  condition, `docs/handoff/PR155_Owner_Decision.md`: "every stream's truncation intents and the incomplete flag").
- R2′ rule 3: a member of S that would set the flag must first leave S by a chosen shrink.
- **The code reseeds only a whole node**, into a directory with no superblock, streams or `ckpt`
  (`engine.cpp:940-946`; `seed.cpp:225-275`). It copies the primary's checkpoint files byte for byte, under the
  primary's sequence number (`seed.cpp:234`). **It does not satisfy R6.5 for one stream, and this note does not
  assume it does.**
- A checkpoint part's header must carry its manifest's sequence number (`storage.cpp:490`). So a part received from
  the primary cannot be linked into the local checkpoint as it is.

- **S and A are sets of nodes, not streams** [FACT-SNC]: the group record holds one term and one in-sync set
  (`docs/AFR_001_SAFETY_ANALYSIS.md` 44-52), and R9-C needs every member of A for every acknowledgement. So R2′ rule 3
  takes the **whole node** out of S, and in practice out of A, even when only one stream is reseeded. A per-stream
  "not counted" does not exist in the frozen text (reseed review, MAJOR 3).

### C.2 The procedure (revised after the reseed review)

[Amended 2026-11-19 by `docs/AFR_001_DURABILITY_CONTRACT.md` 6.5 (preconditions, RS-0's lease drop, term binding,
`next_lease`, pins, RS-6's compare and floor, failure branches) and 6.6 (R6.5: RS-6 clears the flag but does **not**
position the stream; an ordinary R6.2 round follows). Where this section differs, 6.5 and 6.6 win. The package
recommends running these steps over every stream at once, as a whole-node reseed in place.]

Preconditions, checked under `control_mu_`:
- **the node** has left S by a chosen shrink (R2′ rule 3), and is not in A [amended: "not in A" is not checked by
  `k`, contract 6.5];
- the stream is NONE: not PENDING (a PENDING stream sends no R6.2 request, so R6.4 cannot pick it; completion owns
  the directory until the clear, R6.3′ item 3), and **no other stream is RESEEDING** (one reseed per node at a time);
- the node and the primary agree on the slot map's epoch for the stream. It is checked again at RS-6.

| Step | Action | Durable before the next step |
|---|---|---|
| RS-0 | Close the stream's WAL writer and spare slot, wait for any `prepare()`; drain the serial watermark writer and drop coalesced writes it has not submitted; stop catch-up reads. As completion step 2.1 | no flush or watermark write of this stream in flight |
| RS-1 | Record write (four slots, design 2), a compare-and-write on NONE: stream state `RESEEDING`, published as a restricting value before the write (D-06 4.1 step 4) | the record |
| RS-2 | **Discard the stream's in-memory state** (its object map and indexes). Empty the stream directory: unlink every name in it (segments, `spare.tmp`, `seg.tmp`, `wm.tmp`, `watermark`), then `fsync` it. Remove any checkpoint directory with a sequence above `active_ckpt`, then `fsync` `ckpt` | an empty directory (condition C-S) and an empty stream in memory |
| RS-3 | Ask the primary for its part of the stream, **bound to the primary's term and this node's incarnation**. The primary **pins** that checkpoint and its WAL from the part's `start_lsn`, as the whole-node seed does (`seed.cpp:141-149`). Receive `start_lsn`, `max_lsn` and `start_term` (the term of record `start_lsn − 1`), and **load the part into memory**, as a whole-node recovery loads a part. Nothing of the part is written to disk here | memory only |
| RS-4 | Create segment `start_lsn` by B.3: new nonce, `prev_nonce` 0, `prev_term = start_term`. Create `watermark` through `wm.tmp` with every slot `(start_lsn − 1, start_term, binding (start_lsn, nonce))`, then open and lock it (V8-2) | first segment and watermark |
| RS-5 | Catch up the stream's WAL from the primary, from `start_lsn` through at least `max_lsn` (C5's wait, boundary rule 7). The stream acknowledges nothing. If the primary can no longer serve `start_lsn` (its pin expired), go back to RS-2 | WAL to `max_lsn`, watermarked |
| RS-6 | Run **one ordinary node checkpoint** (`control_mu_`, no other stream PENDING). Its walk covers every stream from memory, this one included. Its manifest gives this stream `retain_from = start_lsn`, `floor_term = start_term`. Its commit is **one record write**: `active_ckpt = c + 1`, `next_lease` raised from every shard's `max_lease_seen` (as `engine.cpp:1524-1534` does today, which now sees the loaded part's IDs), and the stream's state `NONE`. Then reclaim `c` (boundary rules 1-3), and position the stream on `j`'s log [amended: not positioned here; an
ordinary R6.2 round follows, contract 6.6] | the record |

Why this shape [INF]:
- **Memory is discarded in RS-2 and rebuilt from the part in RS-3**, as completion step 2.5 rebuilds. Without it,
  catch-up applies on top of old state (`replication.cpp:752` applies in place; `shard.cpp:1781-1788` compares only
  versions), and an object the primary deleted before `start_lsn` would survive and be checkpointed (reseed review,
  BLOCKER 1).
- **One reseed at a time**, and no part copied to disk under a second sequence. Two concurrent reseeds collided in
  `ckpt/c + 1`, and a crash then led to "missing checkpoint part" (`storage.cpp:485`): a refusal after crashes alone
  (reseed review, BLOCKER 2). RS-6's ordinary checkpoint writes every part under one sequence, as today.
- **RS-6 is one record write**: publication and the clear are atomic, so no state has the flag clear and the copied
  state unpublished (R6.5).

### C.3 Interactions

| Codex's point | Answer [INF unless marked] |
|---|---|
| Node-wide metadata authority | RS-1 and RS-6 are record writes. The flag and `active_ckpt` live only in the record. The watermark is a validator only (OD-4) and is not read while RESEEDING |
| The RESEEDING intent | restricting before the write, permitting after (D-06 4.1). While set the stream sends no R6.2 request, is not positioned, acknowledges nothing, holds no lease, serves no catch-up, cannot be added to S and cannot be promoted (R6.5; P6 attests it). The node has left S by a chosen shrink (precondition); `p` drops it from A after the shrink, and `k` drops any lease and stops lease reads in RS-0 [2026-11-19, `docs/AFR_001_DURABILITY_CONTRACT.md` 6.5] |
| WAL and watermark writers | drained in RS-0; recreated in RS-4. The watermark file is a new inode, so no stray write from an earlier process can reach it |
| Durable deletion | RS-2 unlinks every name and fsyncs once. A lost unlink (a fault) revives an old segment: below `start_lsn` the below-floor rule deletes it; above it, its `prev_nonce` names an old nonce and R-CHAIN refuses (OD-7); at `start_lsn` itself, RS-4's rename replaces it, and if that rename is also lost, the binding's nonce refuses |
| Segment identity and links | every segment of the stream after RS-2 is new: new nonces. The first has `prev_nonce` 0 and is the first retained segment, which R-CHAIN does not check. FL-floor checks it against the manifest (`floor_term = prev_term = start_term`) |
| Manifest and floor | published only by RS-6's checkpoint. Until then the record names `c`, whose manifest still describes the old stream |
| Node-wide checkpoints | no ordinary checkpoint starts while any stream is RESEEDING (extends D-06 item 11, which says PENDING), except RS-6's. **Cost:** reclaim of the other streams waits for the whole transfer (the concern of `docs/AFR_001_PROMOTION_DURING_RECON.md` 176-181) [UNRESOLVED as a liveness bound] |
| FL and recovery while RESEEDING | recovery does not scan, check FL or read the watermark of a RESEEDING stream. It restarts the reseed at RS-0, with the stream empty in memory. The old manifest's floor for that stream is not checked, because RS-2 may already have deleted its floor segment |
| Repeated crashes | each recovery with `RESEEDING` repeats RS-2 to RS-6. RS-2 removes every name and any unpublished checkpoint directory, so a restart never meets a leftover. A crash alone never refuses. Each crash restarts the transfer (liveness, unbounded under repeated crashes) |
| Incarnation uniqueness | unchanged: the record and its generations continue. V7's concern (generations restart at 1) is about a whole-node wipe and reseed, which this is not |
| Stream remapping | refused while any stream is RESEEDING, as for PENDING. A reseed across slot-map epochs is refused (checked at the start and at RS-6). A remap on the primary during the transfer is **UNRESOLVED**. Carrying watermark files over a remap stays open (V7) |
| Leases | the loaded part's lease IDs reach `next_lease` through RS-6's commit (`engine.cpp:1524-1534`) |

**Recorded conflicts with existing text** [2026-11-19: answered as proposals in
`docs/AFR_001_DURABILITY_CONTRACT.md` section 6. Reading RS-6 as a positioning was rejected there; the proposal is a
revision of R6.5 (6.6, Y6c-2): clear at RS-6, then an ordinary R6.2 round]:
- **R6.5's order.** R6.5 clears the flag "only once the copied state is durable and positioned". RS-6 clears it at
  publication, before positioning. [2026-11-19: this is a revision of R6.5, proposed in `docs/AFR_001_DURABILITY_CONTRACT.md` 6.6 (Y6c-2); after
  RS-6, `k` is positioned by an ordinary R6.2 round.]
- **The node leaves S for a stream's reseed** (C.1). The per-stream procedure's only gain over a whole-node reseed is
  less data transferred.

**Question for Codex and the owner [INF]** (answered as a recommendation, whole node in place, in
`docs/AFR_001_DURABILITY_CONTRACT.md` 6.4; not selected): given that the node leaves S anyway, is a per-stream reseed worth having,
or should R6.4 always reseed the whole node in place (the same steps over every stream at once, RS-1 setting every
stream RESEEDING in one write)? The procedure above is written so that both readings use it.

## D. Acknowledgement semantics: write-path rule WP-4 against R2′, R9-A and R9-B

"Rule 4" names four different rules in these documents (design section 2, the write path, boundary rule 4, R0 step 4).
Here the write-path rule is called **WP-4**: "anything that reports a durable end outside the node waits for its
watermark" (`docs/AFR_001_D02_CRASH_DESIGN.md` section 4).

**Required property (Codex):** "an HA_DURABLE acknowledgment never counts a replica whose durable watermark does not
cover the acknowledged entry."

### D.1 What the texts say [FACT-SNC]

| Text | Status | Words |
|---|---|---|
| R9-A (`docs/AFR_001_SAFETY_ANALYSIS.md` 293-296) | conditionally frozen (PR155) | "reports an entry as acknowledged only once it has applied it" |
| R9-B lease grant (same, 297-303) | conditionally frozen | `m` "has applied every entry up to `max(inh_p, ack_p)`, judged by the applied position `m` reports (R9-A)"; "The Grow test of R1, applied to the applied state" |
| R9-B, `ack_p` (same, 304-310) | conditionally frozen | "every member of A has reported every entry up to it as applied (R9-A), and `p` held authority when it recorded it" |
| R2′ rule 2 (`docs/AFR_001_R2_AND_SINGLE_COPY.md` 92-101) | conditionally frozen | a member counts toward an acknowledgement only once positioned; "an acknowledgement needs every member of A" |
| P5 (same, 202) | frozen **except** "P5's clause that relies on a copy not being silently lost" (PR155) | "By I4, `j` then holds every acknowledged write, unless that copy was silently lost (G-14(b), D-02 on HOLD)" |
| R6.3′ item 1 (`docs/AFR_001_SAFETY_ANALYSIS.md` 199-200) | frozen, except the TEXT ONLY parts PR155 lists (group and stream identity, incarnation, in-progress checkpoint) | a reply is bound to "`k`'s last term and durable end LSN" |
| Acknowledgement (`docs/AFR_001_SAFETY_ANALYSIS.md` 107-109, section 0) | protocol under analysis; not on PR155's frozen list | "every member of its ack set `A` has `e` durably (p included)" |
| G-11 (`docs/SNC_GUARANTEES.md` 136) | HA_DURABLE clause ACCEPTED (PR148) | "HA_DURABLE means at least two durable copies" |
| D-03 (`docs/handoff/PR148_Owner_Decisions.md` 48) | accepted direction, not frozen | "Durable acknowledgment from every required member" |
| Today's standby | code | reports its WAL's durable LSN (`receiver_->poll(next, wal_->durable_lsn(), ...)`, `shard.cpp:2234`) |

### D.2 Finding

- **No frozen text requires watermark coverage.** R9-A and R9-B define `ack_p` and the lease test from *applied*
  reports. "Applied" and "durable" are independent (neither implies the other), and neither is "watermarked".
- **Durability is required, but not by frozen text:** by section 0's acknowledgement rule, by G-11's accepted "two
  durable copies" and by D-03. None of them says "watermarked".
- **Why durable is not enough under D-02 [INF].** D-02's mechanism detects loss only at or below a node's watermark.
  A replica's copy that is durable but above its watermark can be lost by one fault (a lost or zeroed flush) without
  that replica's recovery noticing. If that replica is later promoted, P5's "j then holds every acknowledged write"
  fails silently. That is exactly P5's silent-loss clause, which PR155 left **outside** the freeze until D-02. So the
  property is what D-02 must supply for that excluded clause, not something the frozen text already says.
- **So the property needs a recorded revision.** It must not be read into the frozen text silently.
- **Earlier text said otherwise, and is corrected on site:** `docs/AFR_001_D02_CRASH_DESIGN.md` section 6 ("The frozen
  text already requires that every such report be durable, so this reads the frozen text") and
  `docs/AFR_001_D02_ACCEPTANCE_REVIEW.md` section 8 ("No frozen text needs a change") now point here.

### D.3 Proposed revision (needs the owner's explicit yes; Y5 stays on HOLD)

One kind of report, used for both counts:
- **R9-A′:** "Every replica reports an entry as acknowledged only once it has applied it **and its durable watermark
  (D-02) covers it**, whether or not it holds a lease at that moment."
- **R9-B, named changes** (the text's formulas stay, their meaning moves):
  - the lease test is judged by the R9-A′ report, so a lease now waits until `m`'s watermark reaches
    `max(inh_p, ack_p)`. This can delay leases after a promotion, when `inh_p` is p's whole drained log [INF];
  - `ack_p` is still "every member of A has reported every entry up to it" (R9-A′), so **its value lags each member's
    watermark** rather than its applied position. That is a change of meaning, named here.
- **R2′ rule 2:** unchanged. It counts members, whose reports now carry R9-A′.
- **R6.3′ item 1:** "`k`'s durable end LSN" is reported as its watermarked end. Recovery raises the watermark to the
  recovered end (D.4) before any R6.2 request, so at that moment the two are equal [INF].
- **Section 0's acknowledgement rule** (not frozen): "has `e` durably" becomes "has `e` durably, under its watermark".
  p's own copy already waits for p's watermark (design write-path rule 3). Whether frozen `ack_p` covers p's own copy is
  debatable, since section 0 counts p in A [INF]; R9-A′ does not change it.
- **Unchanged:** R9-C to R9-E, `m = 2`, the `m = 1` LOCAL_DURABLE rule, P1-P4, P6, P7, S1-S5.
- **Evidence:** argument only. PR155 says frozen text changes "through a recorded revision with new evidence"; no model
  run checks R9-A′, and none is authorized.
- **Cost:** HA_DURABLE latency grows by a standby's watermark write per flush (unmeasured; X-2 not authorized).

### D.4 A liveness gap the revision needs closed: recovery raises the watermark

Found by the review (MAJOR 4, a hypothesis):
- a node that restarts with durable, valid records above its watermark recovers `E > W`;
- under WP-4 and R9-A′ it may report only `W`, and `W` rises only with a new flush, which a member does not receive
  before it is positioned (`docs/AFR_001_SAFETY_ANALYSIS.md` 434-435). Positioning needs the report: a possible
  deadlock.

**Rule (proposed) [INF].** After the scan and the watermark check pass, and before the node reports any durable end,
recovery writes the watermark `(E, term(E), crc(E))` through the serial writer, keeping the current binding, and
waits for it. Raising `W` to `E` only adds coverage of records recovery has just validated, so it cannot hide a loss:
a later loss of any of them is then detected. It runs only for NONE streams, under V8-L.

## E. D-06 reconciliation: the complete revision inventory for items 4-14

Every row is a **proposed** revision to conditionally accepted text (`docs/handoff/PR160_Owner_Decision.md`). None is
applied. The accepted text is quoted so that nothing is replaced silently. Y4 stays on HOLD.

| # | D-06 item | Accepted text (quoted) | Proposed revision | Source |
|---|---|---|---|---|
| E1 | 4 (record, header) | "`WalRecord` grows from 56 to 64 bytes, with `u64 term`"; "Each segment header gains `prev_term`" (STORAGE_METADATA 4.2) | header adds nonce, `space_uuid`, stream index, `prev_nonce`; K1 frames carry the nonce | design 3; OD-7 |
| E2 | 5 (node record) | "`SNCSUPR2` is a fixed 8 KiB copy ... stored in `superblock.a` and `superblock.b` by generation parity" (4.1) | four slots in two files, spaced pairs, `O_DIRECT`, rule 4, tie refusal, refusal of a missing or wrong-size file | design 2 |
| E3 | 5 / 6 (write rule) | "`pwrite` it to the other file, then `fdatasync`, then fsync the directory" (4.1 step 3) | both pairs written per generation; no per-write directory fsync (preallocated); V8-L on both files (A.3) | design 2; V8-2 |
| E4 | 7 (incarnation) | "the generation of the record write that each boot makes before its first R6.2 request" (4.1) | the boot write moves to the start of recovery, after rule 4's duplicate, and after the V8-L locks | design 2; A.3 |
| E5 | 8 (completion 2.1) | "Close the stream's WAL writer and its spare slot ... Unlink a leftover `TMP` file" | also drain the watermark writer and drop coalesced writes; the leftover is `seg.tmp` (B.1) | design 4; B.1 |
| E6 | 8 (completion 2.2, spare) | "If its header is valid and its first LSN is at or below `L`, adopt it ... Otherwise unlink it." | adoption by `renameat2(RENAME_NOREPLACE)`; `EEXIST` refuses (B.4) | OD-8 |
| E7 | 8 (completion 2.4) | "Create segment `L+1` with a header and no records: write a temp file, `fdatasync`, rename" | the temp file is formatted whole (K1) and named `seg.tmp`; new step 2.7 lowers the watermark with its binding | design 3(a), 4; OD-2 |
| E8 | 8 (void rule) | "records at or above the next segment's first LSN are void" (4.3 item 4) | void bytes are not judged, frames or damage (3(d)); R-CHAIN on every retained segment after the first | design 3(d); OD-7 |
| E9 | 9 (reopen) | "an existing segment at the name is truncated to one block, its header rewritten ... and opened without `O_EXCL`" (4.3 step 3) | **replaced** by `seg.tmp` + `rename`, keeping its nonce; after a restart always replaced, never opened as is | design 3(b); OD-9; V8-1 |
| E10 | 8 / boundary 4 (recovery order) | "1. read the record; 2. ... completion ... 3. adopt a spare; 4. delete segments below `retain_from` and check FL; 5. scan; 6. build `H_s`" | 0. directory lock, V8-L locks; the record's rule 4 duplicate (design 2) and the boot write as the first writes, after a read-only validation of every stream (superseded by E27 in `docs/AFR_001_DURABILITY_CONTRACT.md` 7, the order of its 2.2); RESEEDING streams restart the reseed; `seg.tmp`/`wm.tmp` unlinked; spare by B.4; after the scan, R-CHAIN, the binding and the watermark check; the watermark raised to `E` (D.4); then V8-1 at `E + 1`; the preparer starts last | design 2, 5; A, B, C |
| E11 | boundary 4 note | "The spare is adopted before the check because the floor segment can be the newest one, whose rename is not directory-fsynced" | under HeaderFirst the rename is fsynced before any frame; adoption before the check is still needed, after a lost rename (a fault) | OD-8 [2026-11-19: wrong, adoption is also needed after crashes alone; `docs/AFR_001_DURABILITY_CONTRACT.md` 7] |
| E12 | boundary 6 | "FL-floor accepts no segment at all, or a single all-zero segment 1" | under K1, segment 1 is created whole; an all-zero named segment is damage. Rule 6 accepts no segment, or a valid header-only segment 1 [INF; O-FLOOR] | design 3(a) |
| E13 | boundary 4.2 (orphans) | "remove every stream directory that the manifest does not list" | the stream's `watermark` goes with its directory | design 7 |
| E14 | boundary 5 (seed floor) | "creates an empty segment `start_lsn` with `prev_term = start_term`, durably (temp file, sync, rename, directory fsync)" | whole-node: unchanged, plus its own `watermark` (never the primary's). Per stream: C.2 | design 4; C [2026-11-19: superseded by E14′, `docs/AFR_001_DURABILITY_CONTRACT.md` 7] |
| E15 | 10 (floor) | manifest `retain_from`, `floor_term`, `start_term`, `max_lsn` | per-stream reseed publishes one stream's floor in a node-wide manifest at RS-6; FL is not checked for a RESEEDING stream | C.3 |
| E16 | 11 (checkpoints) | "refuses to start one while any stream is PENDING" (4.3) | also while any stream is RESEEDING, except RS-6's checkpoint | C.3 |
| E17 | 12 | (paraphrase) a copy with a valid CRC and an unknown format is refused | "exact copy size" read as "exact file size" | design 7 |
| E18 | 14 (frame fields) | "Request: `(stream, incarnation, request_no, last_term, end_lsn)`" (4.4) | field list unchanged; `end_lsn` means watermarked (design rule 4); no layout change | design 4; D |
| E19 | D-06 section 8 | "option (a)'s watermark would be one more per-stream field, lowered in the same write as an intent" | the watermark is a file of its own, lowered in step 2.7; OD-4 accepts it as a validator only. The format decision's rationale ("works with both D-02 options", FORMAT_DECISION 70) must be restated | OD-4 |
| E20 | write path (new) | none | one data flush in flight per stream (OD-1); ack after watermark; rule 4 for every durable end reported outside the node (D) | OD-1; design 4 |
| E21 | R0 (`docs/X2_PREREG.md` 8.1) | rule 4's end marker; rule 5's valid prefix above `W`; step 1: an invalid header on the newest segment holding "only zeros" is "an unwritten new segment, and is discarded"; "A spare (`spare.tmp`) holding any valid record is judged as the newest segment, never unlinked" | the void rule for void bytes; `E + 1` opened at recovery (V8-1); above `W` only torn frames or format sectors are discarded; the below-floor rows; under K1 a zero header on a named segment is damage (the F-arm reading of step 1); a spare without a take-time header and without records is unlinked (B.4), one with records is never unlinked | design 3, 4, 5; B.4 |
| E22 | 8 (void rule by name) | "a zero-header next segment still voids" (STORAGE_METADATA 4.3 item 4); reopen rationale: "a crash inside segment creation can leave a zero header" (4.3 step 3) | under K1 and B.3 no crash leaves a named segment with a zero header, so a zero-header segment is damage, and recovery refuses before the void rule matters | design 3(a); B.3 |
| E23 | boundary FL (BOUNDARY section 3) | "A segment header is written at creation, and the only rewrite, D-06's reopen of an existing segment, writes the same `prev_term`" | no header is rewritten in place: a replacement (B.3) writes a new inode with the same header values. "Fixed once written" then holds per inode | design 3(b); OD-9 |
| E24 | 4.2 floor | "Recovery refuses if ... a segment is missing above it, except the newest one, which is D-02's (condition 4)" | under D-02 the newest-segment exception is closed by the watermark (`W > E` refuses) and the binding; the exception's text is replaced by a pointer to D-02 | design 4, 5 |
| E25 | recovery (new) | none | after the scan and checks, recovery raises the watermark to `E` before reporting any durable end (D.4) | D.4 |

Items 13 (`DIR/role`) and 11's node-wide choice itself are unchanged.

**Conflicts with accepted text, in one list:** E2 and E3 (the A/B record), E9 (truncate-in-place reopen), E6 (plain
adoption), E11's reason, E12 (all-zero segment 1), E16 (checkpoint gate), E19 (the watermark's place and the
rationale), E21 (R0 step 1's zeros as unwritten), E22 (zero-header next segment), E23 (header rewrite), E24 (the
newest-segment exception).

**Conflicts with frozen protocol text (section D and C):** R9-A, and R9-B's meaning (D.3); R6.3′ item 1's "durable
end LSN" (D.3); R6.5's "cleared only once ... positioned" (C.3). Each needs a recorded revision with the owner's yes.
Earlier drafts of this note had RS-1 replace a PENDING intent, which conflicted with R6.3′ item 3 and completion
item 4; the revised C.2 requires NONE instead.

Every other row adds to the accepted text without contradicting it [INF].

## F. Coordinator's checks (inside A-E)

The coordinator raised six points as claims to check, not as decisions (`/mnt/project-files/snc/` review of
2026-11-18). Each answer stays inside sections A-E; what does not fit is listed as unresolved.

**F.1 Two-phase recovery (E10) [INF].** [Superseded 2026-11-19 by `docs/AFR_001_DURABILITY_CONTRACT.md`
section 2, which reconciles it with D-06 boundary rule 4. Two errors below are corrected there: the watermark is read
only for NONE streams, and a PENDING stream whose floor segment completion recreates cannot have FL-floor checked
before completion.] Recovery is split:
- **Phase 1, read only:** directory lock and V8-L locks; read all record slots; read the manifest; for each stream
  read `spare.tmp` (as if adopted), FL, the scan with the void rule, R-CHAIN, the binding and the watermark. A PENDING
  stream is validated up to `L` only (its tail above `L` is void by the intent). The phase ends with one outcome per
  node: proceed, complete named intents, restart a reseed, or refuse.
- **Phase 2, writes, each idempotent:** the record's rule-4 duplicate and the boot write; unlink `seg.tmp` and
  `wm.tmp`; adopt `spare.tmp` (B.4); complete PENDING intents; restart a RESEEDING stream; delete below the floor;
  raise the watermark (D.4); V8-1 at `E + 1`; start the preparer.
- So no repair or completion runs before the whole picture is validated. The record's rule 4 moves after the reads:
  reading is not acting, so the design-2 invariant is kept as long as rule 4 runs before the first write that acts
  on the record.
- **Not fitted, UNRESOLVED:** completion can recreate the floor segment when `L = retain_from − 1` (boundary rule 7),
  so phase 1's FL check for a PENDING stream must accept a floor segment that completion would recreate. And "a
  recovered node serves only after the witness places it in a term" is already R6.0 and 4.2; not restated here.

**F.2 V8 on the R640's kernel [FACT-SRC, upstream v6.8].** The R640 runs Ubuntu `6.8.0-149-generic`
(`docs/R640_TOPOLOGY.md` 45). Upstream v6.8 has the same structure as v6.12:
- `io_file_get_normal` takes `fget(fd)` and marks inflight only io_uring files (`io_uring/io_uring.c:2038-2048`);
- the exit wait counts `tctx_inflight(tctx, !cancel_all)` (`:3354`), then `io_uring_clean_tctx` (`:3399`), which
  calls `io_wq_put_and_exit` (`io_uring/tctx.c:193`);
- `do_exit` order: `io_uring_files_cancel` (`kernel/exit.c:829`), `exit_mm` (`:858`), `exit_files` (`:866`);
  `locks_remove_file` in `__fput` (`fs/file_table.c:368`).
So V8-L's premise holds upstream at v6.8. **Ubuntu's patched 6.8.0-149 source was not read; UNRESOLVED.**

**Truncate as a barrier [FACT-SRC v6.8]:** ext4 waits for direct I/O only when the size **shrinks**
(`fs/ext4/inode.c:5363-5375`, `inode_dio_wait`); xfs calls `inode_dio_wait` for any size change, even to the same size (`fs/xfs/xfs_iops.c:832`); `:815-818` is only the empty-file short cut (corrected after review r2 of 2026-11-19). So truncate is a barrier only by destroying or changing the file;
it cannot guard an in-place file without rewriting it. Not used.

**"Never reuse an inode the dead process could write" (V8-R) [INF].** V8-1 already does this for segments. For the
record and watermark files it would mean: at every boot, after phase 1 reads them, write a fresh copy of each through
`TMP` + `rename` + directory `fsync`, and use only the new inodes.
- It needs no lock lifetime and no wait on a hung device. A stray lands in the old inode.
- A read can race a stray landing on the old inode: a torn read fails its CRC and the other pair decides, and a stray
  that has landed is a completed write of a generation the dead process never acted on, which recovery may adopt
  exactly as after a crash.
- It costs `2 + n_streams` file rewrites and renames per boot, each a recovery crash point (idempotent), and it
  changes design 2's "later writes are pure overwrites" to "pure overwrites within one boot".
- **Recommendation stays V8-L** (fewer writes, no new recovery crash points), with V8-R as the fallback if Ubuntu's
  kernel or a later one breaks V8-L's premise. The choice is the owner's (Y6a).

**F.3 When is a replaced segment not the newest? (OD-9 unchanged) [INF].**
- Replacement happens in V8-1 (segment `E + 1`), including segment `L + 1` after a later restart with `E = L`.
  [Corrected 2026-11-19: completion does not replace `L + 1`; step 2.3 deletes it and 2.4 creates a new segment with a
  new nonce.] **Both make the newest segment. No path replaces a segment that has a successor.**
- So R-CHAIN's successor link is not the reason to keep the nonce. **The reason is OD-2's binding:** the watermark
  carries `(L + 1, nonce of L + 1)` until the next truncation, and a new nonce fails it (SegChain C2). Changing the
  nonce would need a new binding written in order with the rename, which adds a crash window in which the binding
  names a nonce that is not on disk.
- A fresh nonce would be safe where no binding names the segment: V8-1 at `E + 1` when `E + 1 ≠ L + 1`. Keeping one
  rule for both cases is the KISS choice; the cost is that the nonce cannot tell the replaced file's stale sectors from
  fresh ones, which is why C15 had to be modelled (it passed, `docs/AFR_001_D02_MODEL.md` section 19).
- Finding reported, OD-9 not changed.

**F.4 What the watermark detects [INF, plainly].** It detects **one** lost, torn, zeroed or misdirected write of the
data, and a damaged watermark slot; a lost watermark write is tolerated, not detected (recovery uses the older slot and loses no coverage of records still present). It does **not** detect a fault that drops both: a device or cache that acknowledges a
flush and loses it, together with the later watermark write. The watermark then agrees with the shortened log, and
acknowledged writes are lost silently. That is outside design 0's fault model (one media fault), and rests on the
premise that the device honours FLUSH and FUA (F.6). Peers hold the replicated copy, but recovering it is R6/P5's
business, outside D-02's per-node guarantee (design 1).

**F.5 I/O errors [FACT-SNC, INF].** Rule: **any error from a write, `fsync`, `fdatasync`, `rename` or directory
`fsync` on a durability path is fatal to the process, never retried, and treated at restart like a crash** (recovery
then judges the files). Today's C++ mostly does this: WAL flush errors and short writes (`storage.cpp:816-818`),
`write_all_sync` and its `fdatasync` (`:60-71`), directory fsync (`:128-133`), the manifest and spare renames
(`:319`, `:699-701`), checkpoint part writes (`shard.cpp:2458-2465`) all call `storage_fatal`. **Exception:**
`links_generation` returns `false` on a failed write, fsync, rename or directory fsync (`links_generation.cpp:125-136`)
and leaves the decision to its caller. [Answered 2026-11-19, `docs/AFR_001_DURABILITY_CONTRACT.md` 4.1: at startup the caller throws; at run time
`Shard::start_links` keeps the links closed and a later call retries the store, which does not conform to this rule.]
Under the rule a failed `fsync` is never retried; today's `links_generation` is the exception.

**F.6 What the storage steps depend on (pinned premises).**

| Premise | Required | R640 today [FACT-SNC, `docs/R640_TOPOLOGY.md` 40, 45] |
|---|---|---|
| File system | ext4 `data=ordered` (default) or xfs; no copy-on-write | ext4, default options (`rw,relatime,stripe=2`) |
| Mount options | barriers on (ext4 default); no `nobarrier`, no `data=writeback` | defaults: met |
| Kernel floor | the behaviour of A.1/F.2, read in upstream v6.8 and v6.12 | Ubuntu 6.8.0-149: upstream base matches; Ubuntu patches unread |
| `RENAME_NOREPLACE` | ext4 and xfs support it (VFS `fs/namei.c`, `fs/ext4/namei.c`, `fs/xfs/xfs_iops.c` in v6.12, review A) | ext4: supported |
| Drive write cache | FLUSH and FUA honoured; power-loss protection, or the volatile cache off | `WCE 1`, `fua=1`. Power-loss protection is **INFERRED, not verified** (`docs/R640_TOPOLOGY.md` 70-73): UNRESOLVED |
| `wal_sync` | on | default on (`include/snc/storage.h:141`) |
| Sector atomicity | a 512-byte sector write is atomic (header sector) | 512 B logical / 4096 B physical: assumed |

## 6. Independent review

Three reviewers, each told to break one part, working independently: **A** on sections A-B (kernel and code;
they fetched Linux v6.12 sources), **C** on section C, **D/E** on sections D-E and the header. Every finding was taken
up in the text above. The fixes have not been re-reviewed. **Section F was added after the three reviews**, from the coordinator's checks,
and has had no independent review. [2026-11-19: C, F and D/7 were re-reviewed independently; findings and outcomes in `docs/AFR_001_DURABILITY_CONTRACT.md` 12.1.]

| Reviewer | Severity | Finding | Outcome |
|---|---|---|---|
| C | BLOCKER | the stream's in-memory state was never discarded or rebuilt from the copied part: catch-up applies in place (`replication.cpp:752`), so an object deleted on the primary before `start_lsn` survives; lease IDs in the part never reached `next_lease` | C.2 rewritten: RS-2 discards memory, RS-3 loads the part, RS-6's ordinary checkpoint raises `next_lease` |
| C | BLOCKER | two concurrent reseeds collided in `ckpt/c + 1`; a crash then refused with "missing checkpoint part" (G-48) | one reseed per node; no part written under a second sequence |
| C, D/E | MAJOR | S and A hold nodes, not streams: a per-stream reseed takes the whole node out of S | C.1, C.2 precondition; a question for Codex and the owner (C.3) |
| C | MAJOR | the primary pinned nothing during the transfer, so under steady checkpointing the reseed might never finish | RS-3 pins, as the whole-node seed does; RS-5 restarts at RS-2 if the pin is gone; the reclaim stall of other streams is UNRESOLVED |
| C, D/E | MAJOR / MINOR | RS-1 "replaced" a PENDING intent, against R6.3′ item 3 and completion item 4 | precondition: the stream is NONE |
| D/E | MAJOR | section D contradicted CRASH_DESIGN 6 and ACCEPTANCE_REVIEW 8 without fixing them | fixed on site; D.2 |
| D/E | MAJOR | section D left out frozen P5/I4 and R6.3′ item 1, and argued through G-14(b) | D.1 and D.2 argue through PR155's P5 exclusion; R6.3′ item 1 listed |
| D/E | MAJOR | R9-A′ silently changed R9-B's lease test and `ack_p`'s meaning | D.3 names both changes |
| D/E | MAJOR (HYP) | a restarted node with records above `W` could never report its end: a possible deadlock | D.4: recovery raises `W` to `E`; E25 |
| D/E | MAJOR | missing conflicts: zero-header next segment, R0 step 1 (zeros, spare never unlinked), boundary 3's header rewrite, 4.2's newest-segment exception | E21-E24; B.4 step 2 keeps R0's spare rule |
| A | MAJOR | a missing watermark file after an interrupted creation had no defined outcome | V8-2: created before record generation 1; missing means damage |
| A | MINOR | B.4's adoption differs from SegChain's modelled reading | labelled unmodelled |
| A | MINOR | A.6's "fallocate'd files not affected" was wrong under a lost write | deleted |
| A | MINOR | interrupted-replacement wording; incomplete file tables; formatting writes must be awaited; superblock reads before the locks; the `wm.tmp` descriptor; `ENOSPC`; `wal_sync=none`; "this boot"; wrong `space_uuid`; RESEEDING ordering; rollover cost | all taken up in A and B |
| C | MINOR | R6.5 clears "only once positioned" but RS-6 clears first; binding of RS-3 replies; slot-map epoch recheck; "cannot be added to S" | C.2, C.3 |
| D/E | MINOR | "applied weaker than durable" was wrong; requirement 1 wording; line refs; D-03; p's own copy; E17 paraphrase; "rule 4" ambiguous (now WP-4); R9-A′ rests on argument only | D, E |

**Checked and found sound by the reviewers:**
- every v6.12 kernel line cited in A.1;
- an in-flight request keeps the lock alive: the file is dropped only when the request is freed after completion,
  including the fallback path for a dead task, and ring teardown waits for in-flight requests;
- `RENAME_NOREPLACE` is enforced in the VFS and accepted by ext4 and xfs;
- no crash-only path to `EEXIST`, or to an invalid header at `E + 1`;
- RS-6 as one record write; a crash between RS-5's manifest and RS-6; a crash during RS-2; FL-floor after RS-6;
- every quoted accepted text in E (E17 is now marked as a paraphrase);
- R9-A′ only strengthens the count, and leaves R9-C to R9-E, `m = 2` and the `m = 1` rule alone.

**UNRESOLVED after review:**
- V8-L's wait bound on a hung device;
- recovery reading a sector while a stray write lands on it (treated as a torn write, argued);
- the reclaim stall of other streams during a reseed, and a remap on the primary mid-transfer;
- whether R6.5 and R6.3′ item 3 are inside the frozen R2′ [2026-11-19: R6.5's change is treated as a revision of frozen
  meaning, `docs/AFR_001_DURABILITY_CONTRACT.md` 6.6; R6.3′ item 3 is not changed by the reseed, whose precondition is NONE];
- whether a per-stream reseed should exist at all (the C.3 question).

## 7. What needs Codex and the owner

Each is a proposal. None is recorded as decided. [2026-11-19: the compact package after Codex's #169 review is
`docs/AFR_001_DURABILITY_CONTRACT.md` section 11, which supersedes this table where they differ.]

| # | Decision | Recommendation |
|---|---|---|
| Y5 | R9-A′ and the named changes to R9-B and R6.3′ item 1 (D.3), with D.4's recovery raise | yes, as a recorded revision once Codex has reviewed it; it rests on argument only |
| Y6a | V8-1 (always replace the newest segment after a restart) and V8-2 (V8-L), V8-3 | yes, V8-L over V8-A (A.4) |
| Y6b | B's name spaces, HeaderFirst recovery and the `EEXIST` rule | yes, as the realization of OD-8 |
| Y6c | the per-stream reseed C.2, **or** reseeding the whole node in place with the same steps (C.3 question) | Codex's view first: since the node leaves S anyway, whole-node-in-place may be the simpler contract |
| Y4 | the D-06 revision inventory E1-E25 and the conflict lists | hold until Y5 and Y6 are ruled, then one package |

**Still open after this note:**
- V8-L's recovery-time bound as a G-48 number (A.4); the `ENOSPC` question of V8-1 (A.3).
- The block-reuse path of A.6, argued and not modelled.
- The reclaim stall of other streams while one reseeds; a remap on the primary mid-transfer (C.3).
- Watermark files over a remap; incarnations after a whole-node wipe (V7).
- The coordinator's items not fitted here (F.2 Ubuntu patches, F.6 power-loss protection). [2026-11-19: the F.1 floor
  case and the F.5 callers are answered in `docs/AFR_001_DURABILITY_CONTRACT.md` 2.4 and 4.1; its section 11 holds the one open list.]
- D-02, R-23, O-FLOOR, Y4, Y5, Y6: unchanged, on HOLD.
