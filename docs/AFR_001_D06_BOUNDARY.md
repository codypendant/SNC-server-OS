# AFR-D-06: checkpoint/reclaim boundary, model review, storage-format reconciliation (PROPOSED)

Status: **PROPOSED, docs only.** Asked for by Codex's review of #158 (`docs/handoff/PR158_Codex_Review.md`) before
D-06 acceptance. D-06 (`docs/AFR_001_D06_STORAGE_METADATA.md`) stays PROPOSED and is not accepted or frozen. Nothing
here authorizes C++ or Rust work, an R640 run, D-02 acceptance or migration design. No counted model run was made; the reviewer's probe runs are exploratory.

Labels: [FACT-SNC] read from this repository at 67b81f2; [EXT] a cited primary source; [INF] inferred here.

## 1. The issue

Codex: "the revised term-history contract identifies the compaction floor through checkpoint `retain_from`, while the
storage design also relies on the oldest retained segment's header. Those must have an explicit, crash-consistent
relationship."

D-06 4.2 today splits the floor: its LSN is `retain_from` in the manifest, and its term is `prev_term` in the header of
the segment at `retain_from`. Two more gaps were found while answering this:
- **Today `keep_from` is computed after the manifest is written** [FACT-SNC `engine.cpp:1499`, `:1509-1520`], so the
  manifest cannot name what reclaim will keep.
- **A seed copies the primary's manifest byte for byte** [FACT-SNC `seed.cpp:234-273`]. With `retain_from` in it, a
  freshly seeded node names the primary's segments, which it does not have, and refuses at first boot. D-06 4.2's
  sentence "freshly reseeded has `retain_from` 1 and `prev_term` 0" contradicts the copied manifest.

## 2. Prior art (primary sources, pinned tags)

| System | Floor, and how it is made durable | Publish vs delete | Leftovers below | Missing at or above the floor | Term in the floor |
|---|---|---|---|---|---|
| PostgreSQL REL_17_2 [EXT: `xlog.c`, `xlogrecovery.c`] | `checkPointCopy.redo` in `pg_control`, written by `UpdateControlFile()` with fsync (`xlog.c:7247-7260`) | control file first, then `KeepLogSeg` and `RemoveOldXlogFiles` (`:7309-7323`) | left for the next checkpoint's removal | PANIC: "could not locate a valid checkpoint record" (`xlogrecovery.c:795`); `ReadRecord(..., PANIC, ...)` for the redo segment (`:1702-1707`). The old fallback to a secondary checkpoint was removed | timeline ID in the checkpoint |
| RocksDB v9.7.4 [EXT: `db/version_edit.h`, `db/wal_edit.cc`, `db/db_impl/db_impl_open.cc`, `db/db_impl/db_impl_files.cc`] | `min_log_number_to_keep` in the MANIFEST (`version_edit.h:46`) | floor committed in the MANIFEST; purge keeps a WAL only if "number >= state.log_number" (`db_impl_files.cc:562`) | "Skipping log #… since it is older than min log to keep" (`db_impl_open.cc:1171`) | only with `track_and_verify_wals_in_manifest` (default false): `Corruption("Missing WAL with log number: N.")` (`wal_edit.cc:186-189`); otherwise not detected | none |
| etcd v3.5.17 [EXT: `server/etcdserver/storage.go`, `server/wal/wal.go`] | snapshot file, then a WAL snapshot record `{Index, Term}`, synced (`storage.go:56-71`) | `SaveSnap`, then `ReleaseLockTo`; files purged asynchronously by count (`server.go:895`) | not read: `searchIndex` starts at the last file at or below the snapshot index | `ErrFileNotFound`, `ErrSnapshotNotFound` (`wal.go:365-376`, `:529`) | yes, `Term` with `Index` |
| Kafka 3.8.0 [EXT: `UnifiedLog.scala`] | `log-start-offset-checkpoint`, written lazily | start offset raised in memory, segments deleted asynchronously; the checkpoint can lag | deleted by retention | **not detected**: load takes `max(logStartOffset, firstSegmentBaseOffset)` (`:170-172`), so a lost early segment silently moves the start | separate epoch cache |
| Raft [EXT: raft.github.io/raft.pdf, section 7] | a snapshot keeps the last included index and term | — | entries up to it are discarded | — | yes |

What SNC takes [INF]:
- Publish the floor durably **before** any delete (PostgreSQL, RocksDB, etcd). SNC already does: reclaim runs after the
  record write that names the new checkpoint [FACT-SNC `engine.cpp:1537-1581`].
- The floor names one exact file, and a missing one is fatal (PostgreSQL, etcd, RocksDB's option). Kafka's
  `max(...)` is the failure mode that the safety analysis rules out: damage read as compaction.
- The floor carries its term (etcd, Raft).
- Leftovers below the floor are ignored and removed (RocksDB).

Nothing is adopted or embedded.

## 3. The relationship (proposal)

**One authority.** The floor lives in exactly one place: the manifest that the current node record names
(`active_ckpt`). Each stream gets `retain_from` and `floor_term`, the term of record `retain_from - 1`. The segment
header's `prev_term` stops being the floor. It is now only a link, checked for equality:

> **FL.** For the manifest named by the current record, for each stream: a segment starting at `retain_from` exists
> with a valid header and its `prev_term` equals `floor_term` (**FL-floor**); and every later segment's `prev_term`
> equals the term of record `first_lsn - 1`, the last **non-void** record before it, or `floor_term` if no retained
> record precedes it (**FL-chain**, checked in the `H` pass, where the void rule's terms are known).

Both sides are fixed once written. A manifest is written once. A segment header is written at creation, and the only
rewrite, D-06's reopen of an existing segment, writes the same `prev_term`, taken from `H`. So FL, once true, stays
true unless something is damaged, and a mismatch is damage. The manifest also gains `start_term` per stream, the term
of record `start_lsn - 1`, which the seed needs (rule 5).

`floor_term` repeats the floor segment's `prev_term` on purpose [INF]. The owner's condition is one logical metadata
authority per node. With `floor_term` in the manifest, the record and its manifest alone fix where `H` starts, and the
header becomes a check. Without it, the floor would again be split across two files, which is what Codex flagged. The
cost is 8 bytes per stream.

**Rules.**
1. **Compute before publishing.** Reclaim's delete set (standby `keep_from`, seed pins, the disk budget) is computed
   before the manifest is written, not after as today [FACT-SNC `engine.cpp:1499`, `:1509-1520`]. `seed_->mu` is
   held from that computation until after `active_ckpt` changes, as it is today from the pins to the commit, so no
   seed pins the old checkpoint in between. `retain_from` is the first LSN of the oldest segment the set keeps.
   `floor_term` comes from the in-memory `H` index. The set is a prefix of the segment list
   [FACT-SNC `engine.cpp:1563-1580`], so `retain_from <= start_lsn`.
2. **Publish, then delete exactly that set.** The manifest is durable (temp file, sync, rename, directory fsync,
   `storage.cpp:286-321`) before the record write names it, and the deletes run only after that write returns.
   Reclaim deletes exactly the computed set, oldest first, and does not recompute it.
3. **One directory fsync per stream is enough.** This replaces D-06's "a directory fsync after each unlink," and it is
   what the code does today (`engine.cpp:1581`). Each deleted segment is below the durable floor, so recovery ignores
   any subset of them that a crash leaves. At run time, deleting oldest first means a segment never outlives its
   successor, so the void rule holds for a concurrent catch-up read. A catch-up that finds its segment deleted gets
   `false` and maps to R6.4, as D-06 4.2 says.
4. **Recovery order.** In order:
   1. read the record and its manifest;
   2. remove every stream directory that the manifest does not list (orphans of a remap whose reclaim a crash cut
      short; `engine.cpp:1550-1556`);
   3. per stream, complete a PENDING intent;
   4. adopt a spare;
   5. delete every segment below `retain_from`;
   6. check FL-floor, and refuse if it fails;
   7. scan with the void rule, build `H_s` from `(floor_term, retain_from - 1)`, and check FL-chain on the way.

   The spare is adopted before the check because the floor segment can be the newest one, whose rename is not
   directory-fsynced [FACT-SNC `storage.cpp:690-705`].

   Completion runs before FL-floor because it may legitimately recreate the floor segment (rule 7). It touches only
   segments above `L`, so on a damaged floor it changes nothing below `L`, and FL-floor still refuses afterwards.
5. **Seed writes its own floor.** After the files are received and before the record write that makes the directory a
   space, the receiver:
   - creates an empty segment `start_lsn` with `prev_term = start_term`, durably (temp file, sync, rename, directory
     fsync);
   - rewrites its local manifest with `retain_from = start_lsn` and `floor_term = start_term`.

   A crash before the record write leaves no superblock but leaves data, so the next start refuses with "has data but
   no superblock; clear it first" (`engine.cpp:943-946`). That is fail-closed and needs the operator. It is today's
   behaviour; the comment at `seed.cpp:264-266` says "the next start fetches again", which is wrong.
6. **No checkpoint yet.** A fresh start takes a checkpoint before READY (`engine.cpp:1252`), but `fresh` holds only on
   the boot that creates the superblock, so a crash before that checkpoint leaves this state on later boots. The floor
   is `(0, 0)`, and FL-floor accepts no segment at all, or a single all-zero segment 1 (a crash before its header
   sync, `storage.cpp:378-381`).
7. **Truncation and the floor.** D-06 4.3 step 1 requires `L >= max_lsn`. **Today's `max_lsn` is too low for this**
   [FACT-SNC `shard.h:532`, `shard.cpp:2319-2322`, `:2632-2654`]:
   - it starts at 0 and only images raise it;
   - an object deleted after the snapshot is skipped, so its delete, at or after `start_lsn`, is reflected in the
     checkpoint but in no image.

   A truncation to `L` below such a delete would then rebuild without an object that the log up to `L` still has.
   **Fix (D-06 4.3 step 1):** the manifest's `max_lsn` is the stream's last assigned LSN when the part's walk ends, a
   bound on everything the part can reflect. Then `L >= max_lsn >= start_lsn - 1 >= retain_from - 1`, and C5's wait
   for durability to `max_lsn` also makes record `start_lsn - 1` durable, which gives `start_term`. The cost is more
   reseeds for a truncation that lands inside a running checkpoint's walk.

   Completion deletes segments above `L`, so it removes the floor segment only when `L = retain_from - 1`. It then
   recreates segment `L + 1` with `prev_term = term(L) = floor_term`, so FL-floor still holds. Intents and checkpoints
   are serialized by `control_mu_`, and no checkpoint starts while a stream is PENDING.

**Crash points [INF].**

| Crash | What recovery sees | Result |
|---|---|---|
| before the manifest is durable | the record names the old checkpoint; nothing deleted | old floor, exact |
| manifest durable, record not | as above; the new checkpoint directory is unreferenced | old floor, exact |
| record write torn | CRC fails, the previous copy names the old checkpoint; nothing deleted yet | old floor, exact |
| record durable, during deletes | any subset of the delete set is gone; all of it is below the new floor | rule 4.5 removes the rest; exact |
| record durable, during a remap's orphan deletes | an orphan stream directory, partly emptied | rule 4.2 removes it. Today this boot is fatal (review finding 2) |
| completion deleted the floor segment (`L = retain_from - 1`), crash before step 4 | PENDING; floor segment missing | rule 4.3 recreates it, then FL-floor holds |
| seed, before its record write | data, no superblock | refuse, operator clears (as today) |
| floor segment's rename lost (newest segment) | the spare holds it | rule 4.4 adopts it |
| floor segment lost or damaged | FL-floor fails | refuse (fail closed, G-14(a)) |
| crash during recovery's own deletes | the same record and manifest | recovery repeats them; every step is idempotent |

**A side effect on R-23, not a fix.** If the record rolls back past a checkpoint whose reclaim has run, the older
manifest names segments that are gone, or its checkpoint directory is gone (`engine.cpp:1039` is already fatal).
FL-floor then refuses. This detects rollback only in that case. R-23 stays a D-02 blocker.

**Is a small model needed? Not now [INF].** Each crash point leaves either the old record (nothing deleted) or the
new record (only segments below its floor, or orphan streams, deleted). The review's two misses in this section
(orphans, `max_lsn`) were facts about the code, not interleavings, so a model would not have found them. The bounded
option, if Codex wants it, is to add reclaim to `Completion.tla`: one checkpoint action that publishes `retain_from`
and then deletes a prefix, with crashes in between, at the R2-D1 bounds, with two mutations (delete before publish;
recompute the set after publish), under the same 2-hour cap. Not run.

**Review of this section.** One independent reviewer (Opus), told to break it, read-only:

| # | Finding | Severity | Outcome |
|---|---|---|---|
| 1 | `L >= max_lsn >= start_lsn` is false: `max_lsn` starts at 0 and misses deletes, so a truncation could delete the floor segment and rebuild without an object; record `start_lsn - 1` might not be durable | BLOCKER | fixed: rule 7, `max_lsn` redefined in D-06 4.3 step 1 |
| 2 | Orphan stream directories after a crash in a remap's reclaim are not in any manifest; the next boot is fatal (today's code too) | MAJOR | fixed: rule 4.2 |
| 3 | The chain check against the "previous segment's last record" refuses every truncated log; FL was checked before the scan that knows the void terms | MAJOR | fixed: "last non-void record" (D-06 4.2); FL split into FL-floor and FL-chain |
| 4 | The seed crash row said "fetch again"; the code refuses | MINOR | fixed: row and rule 5 |
| 5 | Moving the pin computation could let a seed pin the old checkpoint | MINOR | fixed: hold `seed_->mu` through the commit (rule 1) |
| 6 | Rule 6 refused a new node that crashed before segment 1's header sync, and the "before READY" claim was too strong | MINOR | fixed: rule 6 |
| 7 | "Written once" was false: the reopen rewrites the header | MINOR | fixed: the rewrite writes the same `prev_term` |
| 8 | Completion runs before the floor check | MINOR | kept, with the reason (rule 4) |
| 9 | KISS: `floor_term` duplicates the header's `prev_term` | MINOR | kept, justified above |
| 10 | Several citations off by one or two lines | MINOR | fixed |

UNRESOLVED from that review:
- whether an `H` pass would refuse a gap below `start_lsn` that today's scan accepts;
- whether skipping deleted objects in images is a C5 problem of its own, apart from D-06;
- how a remap carries `H` and the floor over to a new stream count;
- whether checkpoint directories left by a crash inside `remove_tree` are ever cleaned up.

## 4. Independent review of the revision 2 model

One independent reviewer (Opus) was told to break `spec/tla/Completion.tla` revision 2. It worked read-only, ran
probe and variant models in its own scratch copy (each under 15 minutes), and deleted every TLC state directory.

| # | Finding | Severity | Outcome |
|---|---|---|---|
| 1 | The model's reopen rewrites the header (`Completion.tla:268`); D-06 4.3 said only "truncated to its header block". After a crash inside segment creation that block is zeros. A variant that keeps the zero header breaks Safety in 10 steps **with no truncation**: append, append, new segment, crash keeping the create, recovery, reopen, append | MAJOR | **design text fixed**: truncate to one block and rewrite the header. The model already matches the fixed text, so its runs stand |
| 2 | Safety is checked only while serving. During completion, after step 3 and before step 4's rename, the straddling segment is last and its void tail is exposed (probe reached at 303 states). D-06 did not gate catch-up reads, and `read_stream_range` lists segments once (`storage.cpp:444`), so a running read would ship void records. "Catch-up never ships a void record" was not covered | MAJOR | **design text fixed**: step 2.1 stops the stream's catch-up reads and waits for any in flight; a PENDING stream serves none. The model's conclusion is narrowed to "while serving" (`docs/AFR_001_COMPLETION_CRASH.md`) |
| 3 | `ServedRangeExact` checks ranges only up to the log's end; a reader returning records past it is caught only after a later append | MINOR | open: strengthen in the proposed rerun |
| 4 | A stale `TMP` is reachable when completion reaches step 4. Every create in the code uses `O_EXCL` (`storage.cpp:541`, `:777`), so it would be fatal at boot; D-06 unlinked `TMP` only after completion | MINOR | **design text fixed**: step 2.1 unlinks a leftover `TMP` |
| 5 | Probe P4 (zero header) is reached on a trivial path; a stronger probe (the scan sees a zero-header segment) is reached at 351 states | MINOR | open: replace P4 in the rerun |
| 6 | Revision 2 never ran a lane with `MaxCrash 3` | MINOR | reviewer's exploratory run (not preregistered): `MaxCrash 3`, `MaxLsn 4`, `MaxIno 22`, PASS, 30,323,715 states |
| 7 | `TakeSpare` makes rename, header and first record one step; the code renames first and writes the header with the first flush (`storage.cpp:697-711`) | MINOR | open: split it like `NewSegment` in the rerun |
| 8 | The void rule's "next segment's first LSN" was ambiguous between name and header | MINOR | **design text fixed**: by name (`list_segments`) |

**Checked and not broken:** all seven mutations fail for the defect they name; the crash semantics (any in-order
subsequence of pending directory operations) are a superset of ext4's and xfs's ordered journaling; spare adoption
and `TakeSpare` never land on an existing name; both A/B copies are never invalid at once; no stuck states; the
reopen truncation never drops a record; the first directory fsync and the clear/reopen order are harmless.

**UNRESOLVED (reviewer):**
- `MaxTrunc 3` at `MaxLsn 4` hit the reviewer's 14-minute cap: NOT COMPLETED. "Any number of later truncations" is
  checked only up to 2.
- `MaxLsn >= 5` with `MaxCrash 3`.
- Truncation at `L` = the end, or inside a `kRecGroupNext` group; the void rule against group atomicity is unchecked.
- A torn tail in a segment that is not last, followed by a zero-header last segment (the scan does no gap check on
  that branch, `storage.cpp:378-381`).
- Concurrent R4, boot and checkpoint record writes while PENDING; the `prepare()` race.
- Everything already disclosed: D-02, R-23, the rebuild, more than one stream.

**Verdict (reviewer): partly.** Within its bounds, revision 2 is real, non-vacuous evidence for the scan. It was not
evidence for "both readers exact" or for the design text as written, because of findings 1 and 2. Both are fixed in
the text above.

**Bounded rerun (revision 3): run since, every lane as expected (`docs/AFR_001_COMPLETION_CRASH.md` section 8).** As proposed: one preregistered PR, the same rules as #158:
- changes: findings 3, 5 and 7; `create` on an existing name is fatal (finding 4); one new mutation that keeps the
  zero header on reopen (finding 1);
- lanes: R3-D1 at the R2-D1 bounds; R3-D2 with `MaxCrash 3`; R3-T3 with `MaxTrunc 3` at `MaxLsn 4`; the eight
  mutations at D1 bounds;
- cap 2 hours per run, scratch disk only, TLC state deleted after each run; a timeout is NOT COMPLETED.

I recommend running it before D-06 acceptance: it is small, and it is the only way the void rule's claim covers the
fixed text. Reclaim stays out of it (section 3).

## 5. Storage-format decisions, reconciled

| # | Item | State | Where |
|---|---|---|---|
| 1 | One logical atomic metadata authority per node | **DECIDED** (owner condition, 2026-11-17) | `PR155_Owner_Decision.md` |
| 2 | No per-shard split of that authority | PROPOSED (follows from 1 unless a per-shard design proves equivalence) | D-06 5 |
| 3 | `H` derived from per-record WAL terms | **DECIDED, conditionally** (four conditions; final acceptance waits on D-02) | `PR157_Owner_Decision.md` |
| 4 | 64-byte WAL record with `term` | PROPOSED (needed by 3) | D-06 4.2 |
| 5 | Node record `SNCSUPR2`: 8 KiB A/B, whole-copy CRC, per-stream slots | PROPOSED | D-06 4.1 |
| 6 | `meta_mu` write rule and lock order | PROPOSED | D-06 4.1 |
| 7 | Incarnation = generation of the boot write | PROPOSED | D-06 4.1 |
| 8 | Completion procedure and void rule | PROPOSED; bounded model evidence (#158), narrowed by section 4; text fixed | D-06 4.3 |
| 9 | No writer open unlinks a segment; reopen rewrites the header | PROPOSED; text fixed (section 4, finding 1) | D-06 4.3 |
| 10 | Floor in the manifest (`retain_from`, `floor_term`, `start_term`), FL check, seed floor, one fsync per stream at reclaim | PROPOSED here | section 3 |
| 11 | Checkpoints node-wide, none while a stream is PENDING | PROPOSED (a choice against 2.4's suggestion) | D-06 4.3 |
| 12 | Unknown-format copy refused, not skipped (R-32) | PROPOSED | D-06 6 |
| 13 | `DIR/role` unchanged | PROPOSED | D-06 7 |
| 14 | Request binding and frame fields | PROPOSED (field lists only) | D-06 4.4 |
| 15 | Metadata rollback (R-23) | **UNRESOLVED, blocker**; with D-02 | D-06 9 |
| 16 | Damage inside a segment; lost directory entries (R-01) | **UNRESOLVED**; D-02 | D-06 9 |
| 17 | D-02 end-of-log mechanism | **UNRESOLVED**, owner HOLD | D-06 8 |
| 18 | Wire byte layout and mixed-version policy (G-49 to G-51) | **UNRESOLVED**, a later part of D-06 | D-06 8 |
| 19 | Migration of existing directories | **UNRESOLVED**; migration design not authorized | D-06 6 |
| 20 | R9-F fields | out, waits for R9-F | D-06 8 |
| 21 | Costs: 8 KiB write, 8 bytes per record, rebuild, PENDING wait | **UNRESOLVED**, not measured (O8, X-1) | D-06 9 |

**Recommendation.** Items 4-14 can be accepted together as D-06's format after the bounded rerun of section 4 passes:
they are one design and none depends on an open owner choice. Section 4's two MAJOR findings are already fixed in
the text. Items 15-17 stay with D-02, which is the next real decision.
D-06 acceptance should therefore be conditional in the same way as `H`: accepted as a format, with final acceptance
waiting on D-02. Items 18-19 are separate later steps and do not block it.
