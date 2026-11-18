# AFR-001: consolidated D-02 architectural acceptance review

Status: **REVIEW, for Codex and owner approval.** Docs and analysis only. It accepts nothing, selects nothing and
freezes nothing.

Authority: Codex's review of #167, section 3 (`docs/handoff/PR167_Codex_Review.md`), relayed by cody under the
standing rule for pasted reviews: "Proceed with a consolidated D-02 architectural acceptance review, documentation and
analysis only." Codex's method rules, followed here:
- answer an open question from existing code, specifications or evidence before proposing another model revision;
- independent three-way verification (section 13);
- fix documentation errors on site, keep the evidence, and name unresolved findings.

Not done and not authorized here: D-02 acceptance (it stays on **HOLD**), declaring R-23 or O-FLOOR resolved, a final
D-06 freeze, X-2, R640, C++, Rust, migration, a model revision, or any change to a frozen contract.

Labels:
- **[FACT-SNC]**: read in this repository, cited by file and line;
- **[FACT-SRC]**: read in a primary source, here Linux v6.12 (`torvalds/linux` tag `v6.12`), cited by file and line;
- **[FACT-MODEL]**: a bounded model result, within its stated bounds only;
- **[INF]**: an argument made here, not checked by a model;
- **[HYP]**: an untested guess.

## 0. Result

**D-02 is not ready for acceptance.** The proposed design (candidate (c): K1 frames, the K2 watermark, the four-slot
record, R-CHAIN) survived five model revisions within small bounds, and no reviewer found a BLOCKER in revisions 4 or
5. This review's own storage reviewer found one BLOCKER in the design (section 5): after a kill, a stray write can
land in the newest segment that 3(b) reopens as is. Ten blockers remain (section 9). Three can be closed by text
alone, without a model:
- **V8** is answered by the kernel source: `O_DIRECT` writes already submitted through io_uring outlive the process;
  io-wq work and AIO are drained. Today's directory lock is released before such writes land. The design needs two
  rules: always replace the newest segment after a restart, and a lock (or synchronous writes) for the record and
  watermark files (section 5).
- **O-R**: SNC has no per-stream reseed procedure in code or text. Its storage steps can be written down (section 4).
- **Rule 4 of the write path** was never re-reviewed, and its reading of frozen R9-A/R9-B is a choice (section 8).

**Recommendation (section 10):** one short design note, text only, for those three, with three-way review and no
model. Then an owner-decision package for the items in section 11.

## 1. Reconciliation with D-06 items 4-14 (Codex item 1)

D-06 items 4-14 are conditionally accepted (`docs/handoff/PR160_Owner_Decision.md`) under six conditions; condition 1
makes final acceptance depend on D-02. D-02 changes most of them. Most changes are listed as **proposed** revisions in
`docs/AFR_001_D02_CRASH_DESIGN.md` section 7; none is accepted. **Section 7 is incomplete** (contract review, MAJOR 2):
it lacks `prev_nonce` (R-CHAIN), HeaderFirst with `RENAME_NOREPLACE`, same-nonce replacement and V8-L. HeaderFirst
changes item 8 (completion step 2.2 and recovery step 3, "adopt a spare",
`docs/AFR_001_D06_STORAGE_METADATA.md` 252, 289) and boundary rule 4.4. A yes to section 7 as it stands would not
cover them, so they are listed separately in section 11 (Y4).

| D-06 item | What D-06 accepted | What D-02 changes | Conflict? |
|---|---|---|---|
| 4 (record, header) | 64-byte record with per-record term | K1 frame fields; segment header gains nonce, `space_uuid`, stream, `prev_term` (D-06), `prev_nonce` (R-CHAIN) | additive [INF] |
| 5 (node record) | one 8 KiB A/B copy per write | four slots in two files, spaced pairs, `O_DIRECT`, rule 4, tie refusal | **replaces** the A/B scheme. A/B is the R-23 defect itself (MetaWatermark rev 2, A/B variant rolls back [FACT-MODEL]) |
| 6 (write rule) | `meta_mu`, compare-and-write, lock order | `meta_mu` held across two writes; no per-write directory fsync | compatible; concurrency still not modelled |
| 7 (incarnation) | generation of the boot write | the boot write moves to the start of recovery, after rule 4 | moves a step; V7 (uniqueness across a wipe and reseed) open |
| 8 (completion, void rule) | intent, completion, clear, void rule by name, spare adoption | 2.1 drains the watermark writer; 2.4 creates a whole segment; new step 2.7; void bytes not judged (3(d)); HeaderFirst changes 2.2 and recovery's spare adoption (not in design 7) | extends; 2.7 adds a refusal that only a fault can trigger [INF] |
| 9 (reopen) | "reopen truncates to one block and rewrites the header" | open as is, or replace by `TMP` + `rename` keeping the nonce (3(b), C15) | **replaces** the truncation in place, which would leave zeros that R0 refuses |
| 10 (floor) | manifest floor, FL, publish before delete, seed floor | watermark-vs-floor table (design 5); binding skipped below the floor | adds O-FLOOR dependencies (section 7) |
| 11 (checkpoints) | node-wide, none while PENDING | a per-stream reseed puts one stream's floor into the node-wide manifest (RS-5) | **open** [INF]: nothing says whether a checkpoint may run while a stream is RESEEDING |
| 12 (unknown format refused) | text | "exact copy size" becomes "exact file size" | wording |
| 13 (`DIR/role`) | unchanged | none | none |
| 14 (frame fields list) | field list | write-path rule 4 changes what `end_lsn` means: watermarked, not merely written (`docs/AFR_001_D06_STORAGE_METADATA.md` 321; design 4) | meaning, not layout |

Two further additions have no D-06 item yet: the watermark file (OD-4: a validator only, outside the node authority)
and the write-path rules 1-5 (OD-1 is rule 5).

**Finding [INF]:** the rationale for the conditional acceptance ("works with both D-02 options",
`docs/AFR_001_D06_FORMAT_DECISION.md` 70) assumed a watermark inside the record, "lowered in the same write as an
intent" (`docs/AFR_001_D06_STORAGE_METADATA.md` 364). D-02 puts it in its own file (OD-4) and lowers it in step 2.7.
That rationale needs restating when D-06 is finally reviewed.

**Finding [INF]:** items 5 and 9 are conditionally accepted text that D-02 contradicts. They stay valid as accepted
direction until an owner decision records the revisions. Nothing here edits them.

## 2. Evidence inventory (Codex item 2)

| Evidence | Where | What it checks | Result |
|---|---|---|---|
| Completion.tla rev 3 (D-06) | `docs/AFR_001_COMPLETION_CRASH.md`; `spec/tla/Completion.tla` | void rule, spare, `L + 1`, clear order, `TMP`, reopen as one step | all lanes as preregistered (#160) [FACT-MODEL] |
| MetaWatermark rev 1 (prereg 3aa39d6) | `docs/AFR_001_D02_MODEL.md` 1-7 | record part (U3), watermark part | RM4 lane invalid (review section 7); superseded by rev 2 |
| rev 2 (8104678, results ce97345), 77 runs | sections 8-11 | four-slot record vs five weaker variants; watermark lowering, drain, nonce check | four slots did not roll back; every weaker variant did, A/B and K5 included. **Tie branch never reached.** Design gap W0P, closed by OD-1. Re-review: lost `L + 1` binding revives void entries, closed by OD-2 |
| rev 3, 85 runs | sections 12-14 | persistent `(L + 1, nonce)` binding | closes rev 2's counterexample. Review MAJOR: a lost unlink revives the same void entries, ruled harmful by OD-5 |
| rev 4, 113 runs | sections 15-17 | R-CHAIN tail case | all as preregistered. Only one link exercised; three wrong variants survived; claim narrowed |
| SegChain rev 5, 123 runs (120 as preregistered) | sections 18-20; `spec/tla/SegChain.tla` | R-CHAIN over the lifecycle, spare orders (U2), C15, seed | today's spare order refuses after a crash alone; HeaderFirst passes every lane; C15 same nonce passes; wrong links fail. Three deviations recorded. Review: no BLOCKER; R-CHAIN must stay on every segment (lost directory block) |
| Design reviews | `docs/AFR_001_D02_CRASH_DESIGN.md` 9; `docs/AFR_001_D02_FOLLOWUP.md`; `docs/AFR_001_D02_OBSOLETE_SEGMENT.md` 5-6 | every rule, by three break-it reviewers | fixes recorded; the design's own section 9 says its fixes were not re-reviewed |

Bounds of every model run: `MaxLsn` at most 5, at most 2-3 crashes, at most 2 truncations, **one** media fault, one
stream. No result says anything beyond them.

## 3. Verified, assumed, untested (Codex item 3)

**Verified within the model bounds [FACT-MODEL]:**
- the four-slot record with rule 4 and spaced slots does not roll back under three crashes and one listed fault; A/B,
  K5 and three other variants do;
- crashes alone never refuse, in every passing watermark and SegChain lane (requirement 2, G-48). In the record
  part this is vacuous: the tie is unreachable, so RecNoCrashRefusal never fires (`docs/AFR_001_D02_MODEL.md` 228);
- no acknowledged entry is lost without a refusal (requirement 1), for one stream;
- the persistent binding (OD-2) and R-CHAIN together refuse every revival tried: lost `L + 1` entry, lost unlink, lost
  rename, lost directory block (rev 5 review);
- removing any one ordering rule (lowering before the clear, the drain, the nonce check, ack after watermark)
  breaks a property. **Not** "R-CHAIN on every segment": LastOnly and AboveL1 passed every preregistered lane
  (`docs/AFR_001_D02_MODEL.md` 907-908). LastOnly failed only in one reviewer run with a widened fault, a lost
  directory block, and AboveL1 survived even that (1016-1017). The case for Y1 is that run plus an argument [INF];
- HeaderFirst is the only spare order that passes the unnamed-damage lane;
- replacement keeping the nonce passes C15; a new nonce fails.

**Assumed (premises, not checked):**
- ext4 `data=ordered` or xfs, no copy-on-write; rename atomic across a crash (a file-system property, not POSIX);
- `O_DIRECT | RWF_DSYNC` overwrites durable on completion (FUA or flush);
- a 512-byte sector write is atomic (the header sector, rev 5 review);
- random 64-bit nonces do not collide (about 2⁻⁶⁴ per pair);
- one media fault per node between repairs;
- G-15's prefix property for SNC beyond `Log.tla`'s bounds (FL-chain relies on it);
- a crash keeps an ordered subsequence of the pending directory operations (the models' directory abstraction);
- the record is atomic in part `wm`. The composition of parts `rec` and `wm`, each with its own one-fault budget, is
  argued only (`docs/AFR_001_D02_MODEL.md` 30-36). **So OD-4's condition, "combined crash behaviour still to
  verify", is not discharged.**

**Argued only [INF]:** every rule in design sections 2-5, the below-floor rows of section 5, the "after a restart the
writer reloads the binding" step, and the claim that rule 4 of the write path reads the frozen text rather than
changing it (section 8).

**Untested failure modes:**
- the record's tie refusal (never reached);
- the watermark as two alternating slots: the models use one atomic register, with no zeroed, misdirected or
  two-generations-stale slot (`docs/AFR_001_D02_MODEL.md` 226, 401, 532); K1 torn frames; C16b against the
  above-`W` rule;
- the loss of record `L`, so 2.7's refusal row is unreachable (227);
- R6.2 repairing a revived tail (the models' recovery is terminal, 357);
- several streams; concurrency of record writers (item 6);
- crashes inside completion, the seed, recovery itself, or a 3(b) replacement, in the lifecycle model;
- a lost rename under a 3(b) replacement;
- the truncation at `L = retain_from − 1` (boundary rule 7);
- FL-floor and reclaim (O-FLOOR);
- torn or misdirected WAL writes in the watermark model;
- a write from a killed process (V8, section 5: now answered by source, not tested);
- the RESEEDING crash window (section 4).

## 4. R6.5 per-stream reseed against the storage requirements (Codex item 4, O-R)

**What exists [FACT-SNC]:**
- R6.4 and R6.5 (`docs/AFR_001_SAFETY_ANALYSIS.md` 215-228) define when a member is reseeded and the `incomplete`
  flag. D-06 4.1 makes that flag a per-stream record state, RESEEDING
  (`docs/AFR_001_D06_STORAGE_METADATA.md` 116).
- The code has only a whole-node seed. `seed_from` refuses a directory that already holds streams or `ckpt`
  (`src/core/engine.cpp:943-946`). The "reseed it" messages in `src/core/replication.cpp` are operator instructions.
  No code reseeds one stream.
- The design names two reseed steps: "A reseed ... before the clear, writes `(start_lsn − 1, start_term, nonce of the
  seed's first segment)`. A seed never copies the primary's watermark file" (design 4), and boundary rule 5 covers the
  whole-node seed's floor.

**What the D-02 evidence requires of a per-stream reseed [INF, from rev 5 sections 19-20 and the storage review]:**

| # | Requirement | Why |
|---|---|---|
| RS-0 | the stream's WAL writer and its serial watermark writer are drained, and unsubmitted coalesced watermark writes dropped, before anything below | the counterpart of completion step 2.1: otherwise an old `W` or binding can land on the new watermark file |
| RS-1 | RESEEDING is recorded (a record write: both pairs of design 2) before anything in the stream directory is removed | R6.5 |
| RS-2 | the stream directory is emptied, every name included, and the empty state is durable (directory fsync) before the first new segment is named (C-S) | SegChain D3 passes only when the old name `f` is unlinked before it is bound (rev 5 review MAJOR 2) |
| RS-3 | the first segment is created whole (3(a)) with a new nonce, `prev_nonce` 0 and `prev_term = start_term` | R-CHAIN exempts the first retained segment; boundary rule 5 |
| RS-4 | the stream's watermark file is replaced, not copied, and its binding names the seed's first segment, replacing any older binding | design 4; rev 5 SeedKeepBind and SeedNoBind |
| RS-5 | the stream's copied state (its checkpoint part) and the manifest's floor for that stream (`retain_from`, `floor_term`, `start_term`) are durable and published before the clear | R6.5's "copied state is durable"; boundary rule 5, FL-floor |
| RS-6 | the clear (RESEEDING to NONE) is the last step; a crash at any earlier point restarts the reseed from RS-2 | R6.5 "recovery with the flag set restarts the reseed" |

**Open inside RS [INF]:**
- the manifest and checkpoints are node-wide (item 11). A per-stream reseed must publish one stream's part and floor
  into a node-wide manifest, and nothing says whether a node checkpoint, or FL, runs while a stream is RESEEDING.
  A restart at RS-2 deletes the floor segment that RS-5 may already have published, so FL must not judge a
  RESEEDING stream;
- whether the restart path of RS-6 can refuse after a crash alone. [INF] no, if every step is idempotent and RS-2
  removes every name; a later bounded check could test it, if Codex asks.

**Finding:** no document states RS-0 to RS-6 for a per-stream reseed. This is a **specification gap**, answerable as
text first; a model would have nothing to check until it exists.

**Linked open items:** V7 (incarnation uniqueness after a wipe; a seed crash before its record write needs an
operator today, boundary rule 5).

## 5. V8: outstanding asynchronous writes after a kill (Codex item 5)

**Question (design 8, V8):** can a write still in flight from a process that dies (killed from outside, or by its own
`storage_fatal`, which calls `abort()` with other workers' writes in flight, `src/core/storage.cpp:103-106`) land
after the next boot's write?

**Answer from the kernel source, Linux v6.12 [FACT-SRC]** (checked by me, then corrected and extended by the storage
reviewer, section 13):
- **io_uring's tracked-inflight wait does not cover ordinary files.** `do_exit` calls `io_uring_files_cancel()`
  (`kernel/exit.c:895`), which calls `__io_uring_cancel(false)` (`include/linux/io_uring.h:16-21`).
  `io_uring_cancel_generic` waits on `tctx_inflight(tctx, !cancel_all)` (`io_uring/io_uring.c:3170`), which counts
  only requests whose target is itself an io_uring file (`io_uring/io_uring.c:1907-1915`).
- **But the io-wq workers are drained.** It then calls `io_uring_clean_tctx` (`io_uring/io_uring.c:3220`), which
  calls `io_wq_put_and_exit` (`io_uring/tctx.c:193`). That waits for every io-wq worker to exit and cancels pending
  work (reviewer: `io_uring/io-wq.c:1249`, `:1267`). Buffered writes, `fdatasync` and synchronous `O_DIRECT` (which
  waits uninterruptibly, reviewer: `fs/iomap/direct-io.c:741`) therefore finish before `exit_files`.
- **What outlives the task: `O_DIRECT` bios already submitted asynchronously** (`-EIOCBQUEUED`, reviewer:
  `fs/iomap/direct-io.c:737`). On ext4, an aligned overwrite is issued that way inline (reviewer).
- **AIO is drained** in `exit_aio` (`fs/aio.c:925-928`), from `exit_mm`, before `exit_files`, unless something else
  holds the address space.
- **Each io_uring request holds its own file reference** (`fget` in `io_file_get_normal`, `io_uring/io_uring.c:1907`;
  SNC registers no files). `flock` locks drop in `__fput` (`locks_remove_file`, `fs/file_table.c:423`), when the last
  reference to that open file goes.

**What SNC does [FACT-SNC]:**
- WAL flushes and checkpoint parts go through `sri_write`, io_uring `writev2` with a synchronous `pwritev2`
  fallback (`src/native/runtime.cpp:211-233`; `src/core/storage.cpp:687`; `src/core/shard.cpp:2426`). WAL segments
  are `O_DIRECT` when the file system allows it (`storage.cpp:541-546`). Checkpoint parts are buffered
  (`shard.cpp:2326`), so they are drained.
- The one-engine-per-directory lock is a non-blocking `flock` on a separate `O_DIRECTORY` descriptor
  (`src/core/engine.cpp:897-905`). No I/O request references it, so it is released at `exit_files`
  (`kernel/exit.c:934`) while `O_DIRECT` bios may still be in flight [INF]. **Today's lock does not cover V8.**

**Consequences [INF]:**

| Target of the stray `O_DIRECT` write | Effect |
|---|---|
| A node-record slot (design 2, in place) | a stray generation `g + 1` can land on one copy after the next boot wrote its own `g + 1`: a tie, so a **refusal after a kill alone** (G-48) |
| A watermark slot (design 4, in place) | a stray older slot can overwrite a newer one. A lower `W` weakens the check (a later fault could lose acknowledged entries silently); a higher `W` after a truncation refuses |
| **The newest segment, opened as is by 3(b)** (storage review, **BLOCKER**) | under HeaderFirst and 3(a), every newly named segment holds only its header and format sectors until its first flush. If that flush is in flight at the kill, the next boot opens the **same inode** as is and appends; the stray can land on the new frames: same nonce, valid CRCs. Silent replacement of records below `W` (the watermark checks only record `W`'s CRC), or a refusal after a kill alone. Today's code avoids this by unlinking the name and creating it `O_EXCL` (`storage.cpp:521-527`) |
| An older segment past `E` | void by name (3(c), 3(d)). Today's scan reads it to its end, so a stray frame there ends in "WAL gap" (`storage.cpp:383-384`): a refusal after a kill alone in today's code, removed by 3(d) |
| A new inode (3(a) `TMP`, 3(b) replacement) | not while the stray is in flight. **After it completes, the old inode is freed** and a later `TMP` can reuse its blocks; a lost format write could then expose a valid same-nonce stray frame (storage review, MAJOR 4). Rev 5's [HYP] stays **UNRESOLVED** |

**Rule needed (proposed, text only) [INF]:**
- **3(b) after a restart:** "open as is" applies only to a segment this process created. After any restart, the
  newest segment is replaced (`TMP` + `rename`), never reopened. This closes the BLOCKER and matches today's code.
- **V8-L for the in-place files:** the two record files and each watermark file are opened through exactly one
  open file description each, `flock`ed exclusively on the descriptor that issues the writes, for the process's
  life. Recovery takes that lock, waiting for it, before it reads the file. The lock lives as long as the last
  in-flight request's file reference.
- **Alternative V8-A:** the record and watermark writes use synchronous `O_DIRECT` (drained at exit, above). The
  record write is rare and serial; the watermark write is per flush, on the hot path, and would need its own thread.
  **Which is simpler is UNRESOLVED** (both reviewers raised it); I lean to V8-L because it leaves the write path
  alone, and the owner or Codex should pick.

Untested: recovery time while a hung device holds the lock (when nothing else progresses either); today's
directory lock being `LOCK_NB`, so a restart during exit is refused (a G-48 question for the restart policy).
A host crash is not V8: power loss stops every write.

## 6. Spare-header ordering and replacement against the storage mechanics (Codex item 6)

**Today [FACT-SNC]:**
- `prepare` fallocates and fsyncs the spare, with no header; its extents are unwritten (`storage.cpp:786`).
- `take_spare` renames it to its segment name with no directory fsync; the first flush writes the header and the
  first frame in one write (`storage.cpp:671-687`).
- `adopt_spare` reads the spare's header, trusts it to name the segment, uses a plain `rename`, and unlinks a spare
  with a bad header (`storage.cpp:797-812`).
- The scan treats a bad header as fatal unless the segment is the newest and all zero (`storage.cpp:378-381`).

**So [FACT-MODEL, INF]:** a crash that tears the combined header-and-frame write leaves a named newest segment that is
neither valid nor all zero, and recovery refuses after a crash alone (SegChain U2, "Code" order, 396 states). It needs
a torn multi-sector write, which a power loss can produce.

**HeaderFirst against the mechanics [INF]:**
1. write the take-time header (name, first LSN, nonce, `prev_term`, `prev_nonce`) into `spare.tmp` with DSYNC;
2. `rename` `spare.tmp` to the segment name with `RENAME_NOREPLACE`;
3. `fsync` the directory;
4. the first flush writes frames only.

- Step 1 is an overwrite of a sector the file already owns only if the spare is formatted (K1, 3(a)); today's
  unwritten spare would convert an extent.
- Step 2 must not replace: under HeaderFirst, adopting a named `spare.tmp` becomes a crash-only path, so a plain
  `rename` could replace a live segment (rev 5 review). `renameat2(RENAME_NOREPLACE)` exists on ext4 and xfs.
- **`EEXIST` must not refuse after a crash alone** (storage review, MAJOR 5). The design must scope the names:
  only `spare.tmp` is ever adopted by its header; every other `TMP` (3(a), 3(b)) is unlinked at recovery. Otherwise
  a crash between creating a 3(b) `TMP` and its rename leaves both, and adoption refuses. The `TMP` naming is
  **UNRESOLVED** in the design.
- After a crash, the name holds either no file or a file with a valid header.
- It rests on the 512-byte header sector being written atomically, and `adopt_spare` does trust `spare.tmp`'s header,
  so a torn header there is a refusal question too. [INF] Under HeaderFirst a `spare.tmp` with a bad header was never
  named, so unlinking it (as today) discards nothing acknowledged; this needs saying in the design.

**Replacement (3(b), C15) [INF]:** the `TMP` is allocated before the `rename`, so it cannot take the old file's
blocks while the old inode lives. Keeping the nonce passes C15 in the model; it relies on every frame carrying the
nonce (rev 5 C1). The block-reuse path after a stray write completes (section 5) is open.

**Untested:** a lost rename under 3(b); a crash inside recovery during a replacement.

## 7. Remaining O-FLOOR dependencies (Codex item 7)

D-02 depends on these, and none is model-checked:
1. the watermark row `W = retain_from − 1`: the slot's term against `floor_term` (design 5), a term not a CRC;
2. the binding is skipped when `L + 1` is below the floor segment (3(e)), which needs the floor published durably
   before any delete;
3. the seed floor (boundary rule 5) and the per-stream reseed's floor (RS-5);
4. the truncation at `L = retain_from − 1` (boundary rule 7), never taken in any model;
5. a front segment revived by a lost reclaim unlink (OBSOLETE_SEGMENT 3.4): FL-floor's case;
6. SegChain does not read names below the floor;
7. FL-floor is not modelled anywhere;
8. stale content at exactly the floor LSN with the same term (design 5, "not covered");
9. the `W < retain_from − 1` "accept" row, which rests on the checkpoint covering every acknowledged record (C5 and
   the new `max_lsn`), not only on the floor;
10. boundary rule 6 accepts an all-zero segment 1, which K1's whole-creation rule 3(a) treats as damage; the two
    must be reconciled;
11. remap: carrying the floor and the watermark files, and removing an orphan stream's watermark file (boundary
    rule 4.2 and its UNRESOLVED list).

Codex's condition stands: O-FLOOR is discharged by a bounded model or an argument Codex accepts, before checkpoint or
reclaim conformance work.

## 8. Reconciliation with the contracts (Codex item 8)

| Contract | Status under D-02 [INF unless marked] |
|---|---|
| G-14(a) detection | strengthened: every new rule adds refusals on damage, none removes one |
| G-14(b) boundary | D-02 is the boundary. Holds within the model bounds [FACT-MODEL]; open for several streams, the floor, V8 and the reseed |
| G-14's named exceptions | newest-segment damage: closed in design by K1 frames and the watermark; metadata rollback (R-23): closed in design by the four slots, checked in the bounded record part, tie branch untested |
| G-15 `(term, LSN)` identity | R-CHAIN and the binding prevent revived void entries, which would re-identify an LSN. Relies on the prefix property beyond `Log.tla` (assumed) |
| G-48 no operator after a crash | requirement 2; holds in every passing watermark and SegChain lane. **Open:** today's spare order (fixed by HeaderFirst), V8 record ties and watermark-slot refusals after a kill and the reopened newest segment (closed by the V8 rules, section 5), the RESEEDING restart (RS-6), crashes inside recovery, V7's seed crash before its record write (operator today) |
| R2′ (P1-P7, S1-S5, R6.3′) | text unchanged. Rule 4 of the write path makes the durable ends that I4, P5 and SYNC_READINESS read mean "watermarked"; the frozen text already requires those reports to be durable, so this reads it, not changes it |
| R9-A to R9-E | **a choice, not "unchanged"** (contract review, MAJOR 1). R9-A counts a report once the replica has applied the entry, and R9-B defines `ack_p`, the HA_DURABLE point, from those reports (`docs/AFR_001_SAFETY_ANALYSIS.md` 294, 305). G-11's accepted text needs "two durable copies". Rule 4 delays a standby's acknowledgement until its watermark. Either that adds a condition to the report that R9-A and `ack_p` count, or HA_DURABLE may count a standby copy that is not watermarked. [INF] The first reads R9-A's "only once it has applied" as necessary, not sufficient, so the text stands, but it touches frozen R9-B's meaning and is the owner's to rule on (Y5). R9-C to R9-E: untouched |
| `m = 2` | unchanged; D-02 adds no admission rule. An `m = 1` group stays LOCAL_DURABLE (OD of #148) |
| OD-4 (one authority) | the watermark is a validator only; disagreement refuses. The combined crash behaviour with the record, OD-4's own condition, is **not verified**: the parts are composed by argument (section 3) |

**No frozen text needs a change [INF].** Rule 4 is the one place where it could be read as a change, against R2′
and R9-A/R9-B both. It is listed in section 11 (Y5), and rule 4 itself has not been re-reviewed since it was added
(design 9).

## 9. Blockers before D-02 can be accepted (Codex item 9)

| # | Blocker | Kind | Smallest closure |
|---|---|---|---|
| B1 | R-23: the four-slot record replaces D-06 item 5, not accepted; the tie branch untested; concurrent writers not modelled | design decision + evidence | owner decision on the item 5 revision; tie branch reached in a later bounded check if Codex asks |
| B2 | O-FLOOR: the eleven dependencies of section 7 | evidence | Codex's existing condition: a bounded floor/reclaim model or an accepted argument |
| B3 | V8: no rule; today's lock does not cover it; **BLOCKER**: 3(b) reopens the newest segment as is after a kill | specification | replace the newest segment after every restart, plus V8-L or V8-A (section 5), text |
| B4 | O-R: the per-stream reseed has no storage procedure | specification | RS-0 to RS-6 and its open points (section 4), text |
| B5 | pending owner decisions (section 11) | owner | one decision package |
| B6 | costs unmeasured: four slots, the watermark and its gating, whole-segment creation (V3, V6) | measurement | X-2, not authorized |
| B7 | untested modes of section 3 (several streams, crashes inside recovery or completion, header sector atomicity, prefix property, two-slot watermark) | evidence | a bounded check scoped by Codex after B3 and B4 are written, not before |
| B8 | OD-4's condition: the combined crash behaviour of record and watermark is argued, not verified | evidence | Codex to say whether the composition argument suffices or a joint bounded check is needed |
| B9 | the design's own review fixes, write-path rule 4 among them, were never re-reviewed (design 9); R9-A/R9-B reading of rule 4 (section 8) | review + owner | re-review rule 4 in the same text-only note as B3 and B4, before the owner package |
| B10 | V7: incarnation uniqueness after a wipe and reseed; a seed crash before its record write needs an operator; carrying watermark files over a remap | specification | written with B4 (the reseed note) where it overlaps; the rest stays listed |

B3, B4 and B9 come first because they are text, they change what a later model would have to cover, and Codex's method
rule asks for that order.

## 10. Recommendation: the smallest justified next step (Codex item 10)

**One short design note, text only:** the two V8 rules (section 5), the `TMP` naming rule (section 6), the
per-stream reseed's storage steps RS-0 to RS-6 (section 4) with the overlapping part of V7, a re-review of write-path rule 4 against R2′ and R9-A/R9-B, and the
missing rows of design section 7. Written into `docs/AFR_001_D02_CRASH_DESIGN.md` as proposed revisions, with a
three-way break-it review. No model, no code. Then one owner-decision package for section 11, and Codex's ruling
on B8.

Why this and not a model revision: both questions were answerable from the kernel source and the repository, and
Codex asked for exactly that order. A model run before the text exists would check a procedure nobody has written.

## 11. Items that need cody's explicit yes

Each of these is a Codex recommendation or a proposal. None is recorded as an owner decision.

| # | Item | Source |
|---|---|---|
| Y1 | R-CHAIN on every retained segment after the first | Codex's #167 review section 2; rev 5 review MAJOR 1 |
| Y2 | HeaderFirst spare order, with `RENAME_NOREPLACE` on adoption and only `spare.tmp` ever adopted | Codex's #167 review; section 6 |
| Y3 | same-nonce replacement (C15) | Codex's #167 review; design 3(b) |
| Y4 | the D-06 revisions of design section 7, items 5 and 9 above all, **plus** the rows section 7 lacks: `prev_nonce`, HeaderFirst's change to item 8 and boundary rule 4.4, same-nonce replacement, V8-L | design 7; section 1 |
| Y5 | that rule 4 of the write path reads R2′ and R9-A/R9-B rather than changing them, after its re-review (B9) | section 8 |
| Y6 | the V8 rules (and the V8-L or V8-A choice) and RS-0 to RS-6, once written and reviewed | sections 4, 5 |

## 12. Documentation errors fixed on site

- `docs/AFR_001_D02_CRASH_DESIGN.md` 3(e): "It is not modelled, and it awaits the owner" was stale. Now points to
  OD-5, OD-6 and revisions 4-5.
- `docs/AFR_001_D02_CRASH_DESIGN.md` section 8, V8: now records the kernel answer, the BLOCKER and the proposed
  rules.
- `docs/AFR_001_D02_OBSOLETE_SEGMENT.md`: the status line ("nothing is model-checked") and section 7 ("NOT started")
  were stale. Both now point to revisions 4 and 5, and to OD-5 for the ruling that 2 still called open.

## 13. Independent verification

Three-way: my own reading, plus two Opus reviewers, each told to break the review, working independently on copies.
Both found real defects. Every finding below was taken up in the text above; nothing was dropped.

**Storage mechanics and code (sections 4-6):**

| Severity | Finding | Outcome |
|---|---|---|
| BLOCKER [INF from FACT] | 3(b) opens a header-only newest segment as is, the same inode; a stray first flush from the dead process can land on the new frames (silent below `W`, or a refusal after a kill alone). The draft called WAL past `E` "void by name" and left the segment out of V8-L | section 5 row and rule "replace the newest segment after every restart"; B3 |
| MAJOR [FACT] | the io_uring exit claim was overstated: io-wq workers are drained (`io_uring_clean_tctx` → `io_wq_put_and_exit`); only already-submitted async `O_DIRECT` bios outlive the task | section 5 rewritten; `io_uring.c:3220` and `tctx.c:193` re-checked by me |
| MAJOR | the checkpoint-part row was wrong: parts are buffered (`shard.cpp:2326`), so drained; on ext4 `O_TRUNC` also waits for direct I/O | row removed |
| MAJOR [HYP] | "V8 against C15 is not a hazard" did not follow: after the stray completes, its freed blocks can be reused by a later `TMP` | row corrected; stays UNRESOLVED |
| MAJOR | `RENAME_NOREPLACE`'s `EEXIST` refuses after a crash alone unless only `spare.tmp` is ever adopted | section 6; Y2 |
| MAJOR | RS list incomplete: drain first, the checkpoint part and manifest, checkpoints and FL during RESEEDING, `prev_term = start_term`, "two-copy" wrong for four slots | RS-0, RS-3, RS-5 and the open points |
| MINOR | V8 also arises from `storage_fatal`'s `abort()`; one open file description per locked file; the directory lock is `LOCK_NB`; AIO's drain depends on the address space; `adopt_spare` does trust `spare.tmp`'s header; step 1 needs a formatted spare; a citation off by one; registered files | all taken up in sections 5-6 |

**Contracts, D-06 and evidence (sections 0-3, 7-11):**

| Severity | Finding | Outcome |
|---|---|---|
| MAJOR | rule 4 against R9-A/R9-B was not reconciled: it either adds a condition to what `ack_p` counts or lets HA_DURABLE count an unwatermarked copy | section 8 row; Y5; B9 |
| MAJOR | design section 7 does not list every change (`prev_nonce`, HeaderFirst, same nonce, V8-L); HeaderFirst changes item 8 and boundary rule 4.4 | section 1; Y4 |
| MAJOR | OD-4's combined crash behaviour was described as checked; the parts are composed by argument only | sections 3 and 8; B8 |
| MAJOR | "R-CHAIN on every segment breaks a property" was overstated: LastOnly and AboveL1 passed every preregistered lane | section 3 |
| MAJOR | the untested list missed carried items (two-slot watermark, record `L` loss, R6.2 repair, torn frames, C16b) | section 3 |
| MAJOR | missing blockers: rule 4 never re-reviewed; V7 | B9, B10; the recommendation now includes the re-review |
| MINOR | RecNoCrashRefusal is vacuous; items 11 and 14 are touched; the conditional-acceptance rationale assumed an in-record watermark; three O-FLOOR dependencies missed; the directory-order premise; V8 watermark refusals under G-48 | sections 1, 3, 7, 8 |

**UNRESOLVED after review:** V8-L against V8-A (which is simpler); the block-reuse path of section 5; the `TMP`
naming; checkpoints and FL during RESEEDING; xfs's behaviour for `O_TRUNC` against direct I/O (not checked).

The fixes above have not been re-reviewed. The next text-only note (section 10) is where they would be.
