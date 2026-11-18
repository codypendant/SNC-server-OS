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

- At exit, io_uring drains its io-wq workers (`io_uring_clean_tctx`, `io_uring/io_uring.c:3220`, then
  `io_wq_put_and_exit`, `io_uring/tctx.c:193`), so buffered writes, `fdatasync` and synchronous `O_DIRECT` finish
  before `exit_files` (`kernel/exit.c:934`).
- **`O_DIRECT` writes already submitted asynchronously are not waited for.** The tracked-inflight wait
  (`io_uring/io_uring.c:3170`) counts only requests whose target is an io_uring file (`:1907-1915`).
- AIO is drained in `exit_aio` (`fs/aio.c:925-928`) from `exit_mm`, unless another task holds the address space.
- Each io_uring request on a non-registered file holds a reference to that open file description (`fget` in
  `io_file_get_normal`, `io_uring/io_uring.c:1907`). `flock` locks are dropped in `__fput` by `locks_remove_file`
  (`fs/file_table.c:423`), which runs only when the last reference goes.
- The same holds when the process dies by its own `storage_fatal`, which calls `abort()`
  (`src/core/storage.cpp:103-106`) while other workers' writes are in flight [FACT-SNC].

### A.2 What SNC writes asynchronously [FACT-SNC]

| File | How it is written | Exposed to a stray write? |
|---|---|---|
| WAL segment | `sri_write`, io_uring `writev2`, `O_DIRECT` when the file system allows it (`storage.cpp:541-546`, `:687`; `runtime.cpp:211-233`) | **yes** |
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
  "opened as is" after a restart. "Open as is" stays allowed only for a segment that this process itself created.
- This closes the BLOCKER of the acceptance review (section 5): a header-only newest segment whose first flush was in
  flight at the kill. It is what today's code already does by `unlink` + `O_EXCL` (`storage.cpp:519-527`), done
  with `rename` so the name never goes missing (D-06 item 9's purpose).

**V8-2. Every file that is overwritten in place (the two record files and each stream's watermark file) is protected
by V8-L.** Recommended over V8-A; the reason is in A.4.
- **One open file description per file per process.** The file is opened once, with `O_CLOEXEC`, and that
  descriptor issues every read and write. A second `open` of the same file by the same process is forbidden, because
  `flock` on a second description would deadlock against the first.
- **The lock.** Immediately after opening, the process takes `flock(fd, LOCK_EX)`, blocking, and holds it for the
  life of the descriptor. Nothing ever unlocks it explicitly.
- **Recovery order.** Recovery takes the directory lock first (unchanged), then the V8-L locks in a fixed order
  (`superblock.a`, `superblock.b`, then each watermark file by stream index), each before the first read of that file.
  A lock held by a dead process is released only when its last in-flight request drops its file reference, which is
  after that request's bio completed (A.1) [INF from FACT-SRC].
- **New files.** A watermark file created at init or by a reseed is created through `wm.tmp` + `rename` (section B),
  then opened and locked once under its final name before its first in-place write.
- **No registered files** (`IORING_REGISTER_FILES`) for these descriptors: a registered file's reference lives in the
  ring's table, not in the request [FACT-SRC for the `fget` path; SNC registers none, `runtime.cpp:211-233`].
- **No `fork` while the descriptors are open.** SNC does not fork [FACT-SNC: no `fork` in `src/`].

**V8-3. No other in-place file is written by asynchronous `O_DIRECT`.** Checkpoint parts stay buffered, or are
created fresh (`O_EXCL` after an `unlink`) if they ever become `O_DIRECT`. Today's `O_TRUNC` reopen of a part
(`shard.cpp:2325-2326`) is safe only because parts are buffered.

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
opposite of KISS for the hot path.

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

**Not affected:** files whose unwritten space is `fallocate`d. An unwritten extent reads as zeros whatever its blocks
held before [INF: the unwritten-extent rule of ext4 and xfs].

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
1. write the take-time header into `spare.tmp`: name and first LSN `X`, nonce, `space_uuid`, stream, `prev_term`,
   `prev_nonce`; `RWF_DSYNC`. Synchronous `pwrite`, so V8 does not reach it [INF].
2. `renameat2(spare.tmp, X.wal, RENAME_NOREPLACE)`;
3. `fsync` the directory;
4. only then the first flush, frames only.

Facts the order relies on:
- the take happens in `start_flush`, which runs only when no flush is in flight (OD-1; `storage.cpp:643`, `:651-660`).
  So every record below `X` is durable at the take, and **a crash alone always leaves `E ≥ X − 1`** [INF from
  FACT-SNC];
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

Per stream, after the record is read and before the floor check and the scan (D-06 recovery step 3, boundary rule 4.4),
and **before the preparer starts**, because its first act would be to unlink `spare.tmp`:
1. `unlink(seg.tmp)` and `unlink(wm.tmp)`, then `fsync` the directory. Neither was ever named, so neither holds
   acknowledged state [INF].
2. If `spare.tmp` has no valid take-time header: `unlink` it. It was never named. Under one fault it holds no frames,
   because frames need a durable rename first (B.2), and losing the rename **and** the header would be two faults
   [INF].
3. If `spare.tmp` has a valid take-time header for `X` (and the right `space_uuid` and stream):
   - **if the stream is PENDING**, D-06 4.3 step 2.2 applies: adopt it if `X ≤ L`, otherwise unlink it;
   - otherwise `renameat2(spare.tmp, X.wal, RENAME_NOREPLACE)`, then `fsync` the directory. This completes an
     interrupted take (crash between steps 1 and 3 of B.2). The adopted file holds only a header, and V8-1 then
     replaces it at `E + 1`, or it holds frames after a lost rename (a fault), which the scan then reads.
   - **`EEXIST`: refuse**, with a named error.

**When `EEXIST` is legitimate [INF]:**
- at recovery, `X.wal` and a take-time header for `X` in `spare.tmp` coexist only after a fault: a lost rename, a lost
  unlink of an old `X.wal` (for example after a truncation), or a misdirected write. A crash alone cannot produce it
  (B.2, third fact). So refusing on `EEXIST` never refuses after a crash alone;
- at run time, an `EEXIST` from the take means the same, or a bug. The writer fails (`storage_fatal`), and recovery
  then refuses as above;
- `EEXIST` never arises from `seg.tmp` or `wm.tmp`, because their renames replace on purpose.

**Interrupted replacement:** a crash before step 3 of B.3 leaves the old `X.wal` and a `seg.tmp`. Recovery unlinks
`seg.tmp` (step 1) and V8-1 replaces again. A crash between steps 3 and 4 leaves either file at the name, and either is
a valid header-only segment of the same nonce. A **lost** rename (a fault) after appends began on the new inode loses
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

### C.2 The procedure

Preconditions: the stream has left S (R2′ rule 3); the node and the primary agree on the slot map's epoch for this
stream (otherwise a per-stream reseed is refused and the operator reseeds the node; remaps are not covered, C.3).

| Step | Action | Durable before the next step |
|---|---|---|
| RS-0 | Close the stream's WAL writer and spare slot, wait for any `prepare()`; drain the serial watermark writer and drop coalesced writes it has not submitted; stop catch-up reads. As completion step 2.1 | no flush or watermark write of this stream in flight |
| RS-1 | Record write (four slots, design 2): stream state `RESEEDING`. A PENDING intent is replaced in the same write, because the reseed discards everything it covers. Published as a restricting value before the write (D-06 4.1 step 4) | the record |
| RS-2 | Empty the stream directory: unlink every name in it (segments, `spare.tmp`, `seg.tmp`, `wm.tmp`, `watermark`), then `fsync` it. Remove any checkpoint directory with a sequence above `active_ckpt` (an unpublished one from an earlier attempt), then `fsync` `ckpt` | an empty directory, condition C-S |
| RS-3 | Fetch the primary's part for the stream and its manifest entry: `start_lsn`, `max_lsn`, and `start_term` = the term of record `start_lsn − 1`. Write the part into a new local checkpoint directory `c + 1` with a local header (sequence `c + 1`), `fsync` | the part, unpublished |
| RS-4 | Create segment `start_lsn` by B.3 with a new nonce, `prev_nonce` 0, `prev_term = start_term`. Create `watermark` through `wm.tmp` with every slot `(start_lsn − 1, start_term, binding (start_lsn, nonce))`, then open and lock it (V8-2) | first segment and watermark |
| RS-5 | Catch up the stream's WAL from the primary, from `start_lsn` through at least `max_lsn` (the same wait that C5 makes before a checkpoint publishes, boundary rule 7). The stream acknowledges nothing and is not counted (R6.5). Then write the other streams' parts into `c + 1` by an ordinary checkpoint walk, and write manifest `c + 1` with this stream's `retain_from = start_lsn`, `floor_term = start_term`, `start_term`, `max_lsn` | WAL to `max_lsn`; manifest `c + 1` |
| RS-6 | **One** record write: `active_ckpt = c + 1` **and** the stream's state `NONE`. Then reclaim checkpoint `c` (boundary rules 1-3) and position the stream at `(start_term, start_lsn − 1)` or later on `j`'s log | the record |

Why one record write in RS-6 [INF]: it makes publication and the clear atomic. A crash before it leaves `RESEEDING`
with checkpoint `c` active, and the reseed restarts. A crash after it leaves a published checkpoint and a clear stream.
No state exists in which the flag is clear and the copied state is not published (R6.5).

### C.3 Interactions

| Codex's point | Answer [INF unless marked] |
|---|---|
| Node-wide metadata authority | RS-1 and RS-6 are record writes. The flag and `active_ckpt` live only in the record. The watermark is a validator only (OD-4) and is not read while RESEEDING |
| The RESEEDING intent | restricting before the write, permitting after (D-06 4.1). While set: no R6.2 request, no positioning, no acknowledgement, no lease, no catch-up serving, not promotable (R6.5; P6 attests it) |
| WAL and watermark writers | drained in RS-0; recreated in RS-4. The watermark file is a new inode, so a stray from an earlier process cannot reach it |
| Durable deletion | RS-2 unlinks every name and fsyncs once. A lost unlink (a fault) revives an old segment: below `start_lsn` it is deleted by recovery's below-floor rule; above it, its `prev_nonce` names an old nonce and R-CHAIN refuses (OD-7); at `start_lsn` itself, RS-4's rename replaces it, and if that rename is also lost, the binding's nonce refuses (two faults otherwise) |
| Segment identity and links | every segment of the stream after RS-2 is new: new nonces; the first has `prev_nonce` 0 and is the first retained segment, which R-CHAIN does not check; FL-floor checks it against the manifest (`floor_term = prev_term = start_term`) |
| Manifest and floor | published only in `c + 1`, in RS-6's record write. Until then the record names `c`, whose manifest still describes the old stream |
| Node-wide checkpoints | no ordinary checkpoint starts while any stream is RESEEDING (extends D-06 item 11, which says PENDING). RS-5's walk is the one checkpoint, serialized by `control_mu_`. Only one reseed publishes at a time |
| FL and recovery while RESEEDING | recovery does not scan, check FL or read the watermark of a RESEEDING stream; it restarts the reseed at RS-0. The old manifest's floor for that stream is not checked, because RS-2 may already have deleted its floor segment |
| Repeated crashes | each recovery with `RESEEDING` repeats RS-2 to RS-6. RS-2 removes every name and the unpublished `c + 1`, so a restart never meets a leftover and never judges one. A crash alone never refuses. Each crash restarts the transfer (liveness, unbounded under repeated crashes) |
| Incarnation uniqueness | unchanged: the record and its generations continue. V7's concern (generations restart at 1) is about a whole-node wipe and reseed, which this procedure is not |
| Stream remapping | refused while any stream is RESEEDING, as for PENDING. A reseed across different slot maps is refused (precondition). Carrying watermark files over a remap stays open (V7) |

**Cost [HYP, unmeasured]:** a per-stream reseed costs a node-wide checkpoint walk (RS-5). The alternative, a manifest
that names a part sequence per stream, would change D-06 item 10's manifest format; this note does not propose it.

## D. Acknowledgement semantics: write-path rule 4 against R2′, R9-A and R9-B

**Required property (Codex):** "an HA_DURABLE acknowledgment never counts a replica whose durable watermark does not
cover the acknowledged entry."

### D.1 What the texts say [FACT-SNC]

| Text | Status | Words |
|---|---|---|
| R9-A (`docs/AFR_001_SAFETY_ANALYSIS.md` 293-296) | conditionally frozen (PR155) | "reports an entry as acknowledged only once it has applied it" |
| R9-B, `ack_p` (same, 305-310) | conditionally frozen | "`ack_p` is the highest LSN that `p` has recorded as HA_DURABLE in its current term. That means every member of A has reported every entry up to it as applied (R9-A), and `p` held authority when it recorded it" |
| R2′ rule 2 (`docs/AFR_001_R2_AND_SINGLE_COPY.md` 92-101) | conditionally frozen | "A member of S′ counts toward an acknowledgement ... only after" it is positioned; "This is today's rule that an acknowledgement needs every member of A" |
| Acknowledgement (`docs/AFR_001_SAFETY_ANALYSIS.md` 107-109, section 0) | protocol under analysis; not on PR155's frozen list | "every member of its ack set `A` has `e` durably (p included)" |
| G-11 (`docs/SNC_GUARANTEES.md` 136) | HA_DURABLE clause ACCEPTED (PR148) | "HA_DURABLE means at least two durable copies" |
| Today's standby | code | reports its WAL's durable LSN (`receiver_->poll(next, wal_->durable_lsn(), ...)`, `shard.cpp:2234`) |
| Design rule 4 (`docs/AFR_001_D02_CRASH_DESIGN.md` 4) | proposed | "anything that reports a durable end outside the node waits for its watermark", including "a standby's acknowledgement to the primary" |

### D.2 Finding

- **The frozen text does not require the property.** R9-A and R9-B define `ack_p` from *applied* reports. "Applied"
  is weaker than durable, and durable is weaker than watermarked [FACT-SNC: the words above].
- **Durability is required elsewhere:** by section 0's acknowledgement rule and by G-11's accepted "two durable
  copies". Neither says "watermarked", and section 0 is not on the frozen list.
- **Why durable is not enough under D-02 [INF].** A replica's copy that is durable but above its own watermark can be
  lost by one fault (a lost or zeroed flush) without that replica's recovery noticing: requirement 1 of the design
  covers only records at or below `W`. G-11's "durable copy" would then be a copy whose loss is silent, which is what
  G-14(b) forbids.
- So **the property needs a recorded revision.** It must not be read into the frozen text silently (Codex: "Do not
  silently change `ack_p` or the meaning of HA_DURABLE").

### D.3 Proposed revision (needs the owner's explicit yes; Y5 stays on HOLD until then)

- **R9-A′:** "Every replica reports an entry as acknowledged only once it has applied it **and its durable watermark
  (D-02) covers it**, whether or not it holds a lease at that moment."
- **R9-B:** text unchanged. `ack_p` is still defined through R9-A's reports, so it inherits the stronger report
  without a new formula. The note under R9-B gains one sentence: "Under D-02, a report covers only entries at or
  below the reporter's watermark (R9-A′)."
- **R2′ rule 2:** unchanged. It counts members, and the members' reports now carry R9-A′.
- **Section 0's acknowledgement rule:** "has `e` durably" becomes "has `e` durably, under its watermark". p's own copy
  already waits for p's watermark (design rule 3).
- **What it changes [INF]:**
  - HA_DURABLE latency grows by a standby's watermark write per flush (unmeasured; X-2 not authorized);
  - nothing in R9-C to R9-E, `m = 2`, P1-P7 or S1-S5;
  - "caught up" (R1) and SYNC_READINESS use durable ends, which design rule 4 already gates. Those texts are not
    frozen.

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
| E10 | 8 / boundary 4 (recovery order) | "1. read the record; 2. ... completion ... 3. adopt a spare; 4. delete segments below `retain_from` and check FL; 5. scan; 6. build `H_s`" | 0. directory lock, V8-L locks; record rule 4 and the boot write first; RESEEDING streams restart the reseed; `seg.tmp`/`wm.tmp` unlinked; spare by B.4; after the scan, R-CHAIN, the binding and the watermark check; then V8-1 at `E + 1` | design 2, 5; A, B, C |
| E11 | boundary 4 note | "The spare is adopted before the check because the floor segment can be the newest one, whose rename is not directory-fsynced" | under HeaderFirst the rename is fsynced before any frame; adoption before the check is still needed, after a lost rename (a fault) | OD-8 |
| E12 | boundary 6 | "FL-floor accepts no segment at all, or a single all-zero segment 1" | under K1, segment 1 is created whole; an all-zero named segment is damage. Rule 6 accepts no segment, or a valid header-only segment 1 [INF; O-FLOOR] | design 3(a) |
| E13 | boundary 4.2 (orphans) | "remove every stream directory that the manifest does not list" | the stream's `watermark` goes with its directory | design 7 |
| E14 | boundary 5 (seed floor) | "creates an empty segment `start_lsn` with `prev_term = start_term`, durably (temp file, sync, rename, directory fsync)" | whole-node: unchanged, plus its own `watermark` (never the primary's). Per stream: C.2 | design 4; C |
| E15 | 10 (floor) | manifest `retain_from`, `floor_term`, `start_term`, `max_lsn` | per-stream reseed publishes one stream's floor in a node-wide manifest at RS-6; FL is not checked for a RESEEDING stream | C.3 |
| E16 | 11 (checkpoints) | "refuses to start one while any stream is PENDING" (4.3) | also while any stream is RESEEDING, except RS-5's walk | C.3 |
| E17 | 12 | an unknown format is refused | "exact copy size" read as "exact file size" | design 7 |
| E18 | 14 (frame fields) | "Request: `(stream, incarnation, request_no, last_term, end_lsn)`" (4.4) | field list unchanged; `end_lsn` means watermarked (design rule 4); no layout change | design 4; D |
| E19 | D-06 section 8 | "option (a)'s watermark would be one more per-stream field, lowered in the same write as an intent" | the watermark is a file of its own, lowered in step 2.7; OD-4 accepts it as a validator only. The format decision's rationale ("works with both D-02 options", FORMAT_DECISION 70) must be restated | OD-4 |
| E20 | write path (new) | none | one data flush in flight per stream (OD-1); ack after watermark; rule 4 for every durable end reported outside the node (D) | OD-1; design 4 |
| E21 | R0 (`docs/X2_PREREG.md` 8.1) | rule 4's end marker; rule 5's valid prefix above `W` | the void rule for void bytes; `E + 1` opened at recovery (V8-1); above `W` only torn frames or format sectors are discarded; the below-floor rows | design 3, 4, 5 |

Items 13 (`DIR/role`) and 11's node-wide choice itself are unchanged.

**Conflicts with accepted text, in one list:** E2 and E3 (the A/B record), E9 (truncate-in-place reopen), E6 (plain
adoption), E11's reason, E12 (all-zero segment 1), E16 (checkpoint gate), E19 (the watermark's place and the
rationale). Every other row adds to the accepted text without contradicting it [INF].

## 6. Independent review

Pending: filled in after the three-way review.

## 7. Still open after this note

- V8-L's recovery-time bound as a G-48 number (A.4).
- The block-reuse path of A.6, argued and not modelled.
- Whether the per-stream reseed's node-wide checkpoint (RS-5) is acceptable in cost, or a per-stream part sequence in
  the manifest is preferred (C.3).
- Watermark files over a remap; incarnations after a whole-node wipe (V7).
- D-02, R-23, O-FLOOR, Y4, Y5, Y6: unchanged, on HOLD.
