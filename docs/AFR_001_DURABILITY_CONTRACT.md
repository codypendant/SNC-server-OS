# AFR-001: SNC durability contract, consolidated (PROPOSED)

Status: **PROPOSED, for Codex and owner review.** Docs only. Nothing here is accepted, frozen or implemented, and
nothing here is an owner decision.

Authority:
- Codex's review of the coordinator's 16-finding assessment (`docs/handoff/Assessment_Codex_Review_2026-11-19.md`),
  section 4: "one SNC Durability Contract Consolidation package", items 1-10, consolidation from existing documents,
  three-way adversarial review of the integrated contract, then stop;
- Codex's review of #169 (`docs/handoff/PR169_Codex_Review.md`), section 4, "Required next work", items 1-6, folded in
  here.

Unchanged and not touched: D-02 (HOLD), R-23, O-FLOOR, final D-06 acceptance. [2026-11-19: the owner has since
decided Y5 (R9-A′ recorded in place), Y6a, Y6b, Y6c-1 and Y8 as Codex worded them; Y6c-2 is conditional; Y4 and Y7
HOLD (`docs/handoff/PR171_Owner_Decisions.md`). Everything else here stays a proposal.] Not authorized and not done:
a new model, X-2, R640, C++, Rust, migration, any freeze, any other change to R2′ or R9. `m = 2` and the
current guarantees are preserved.

Labels: **[FACT-SNC]** read in this repository at `849f7b9` (the code is unchanged at `1558aa6`, where round 3
re-read it); **[FACT-SRC]** Linux source as cited in the note;
**[FACT-EXT]** an external primary source, as cited; **[FACT-MODEL]** a recorded TLC result; **[INF]** argued here,
not model-checked; **[HYP]** untested guess.

**The note** below means `docs/AFR_001_D02_RESTART_RESEED_NOTE.md`; **design** means
`docs/AFR_001_D02_CRASH_DESIGN.md`; **boundary** means `docs/AFR_001_D06_BOUNDARY.md` section 3. **The decisions package** means
`docs/AFR_001_DURABILITY_DECISIONS.md`, which carries round 3 of the review and the owner questions.

## 0. How to read this, and what it supersedes

This document is the contract-level index: the fault model, the recovery state machine and the refusal matrix live
**only** here. Mechanisms keep their one authoritative text elsewhere:

| Topic | Authoritative text | Changed here |
|---|---|---|
| fault model | section 1 | supersedes design section 0 where they differ |
| recovery order | section 2 | supersedes D-06 4.3 "Recovery, per stream", boundary rule 4, design 2's placement of rule 4, the note's F.1 and E10 (as proposals; the accepted D-06 text stays until Y4) |
| refusals and repair | section 3 | new |
| node record (four slots, rule 4) | design section 2 | no |
| frames, void rule, replacement | design section 3; note A, B | locking extended to segments (2.3) |
| watermark, binding, step 2.7 | design sections 4-5 | the raise follows the segment barrier (2.3; 2.2 step 9) |
| R-CHAIN | `docs/AFR_001_D02_OBSOLETE_SEGMENT.md`, OD-5 to OD-9 | no |
| reseed | note C.2, amended by 6.5 | yes; R6.5 revision proposed (6.6) |
| acknowledgement | section 5 (proposed text) | proposed revision of R9-A |
| D-06 revisions | note E (E1-E25), plus E26-E29 and E14′ in 7 | additions only |

| Codex item | Where |
|---|---|
| assessment review 4.1 fault-model matrix | 1 |
| 4.2 recovery state machine; #169 4.4 (two-phase versus repair and PENDING) | 2 |
| 4.3 refusal-to-repair matrix | 3 |
| 4.4 reconciliation of the 16 findings with #169 | 4 |
| 4.5 R9-A′/R9-B revision; #169 2D | 5 |
| 4.7 per-stream versus whole-node; #169 4.2 (R6.5) and 4.3 | 6 |
| 4.6 unresolved D-06 changes and owner decisions | 7, 11 |
| prior art (FAST 2018, etcd) and witness facts | 8 |
| 4.8 remaining obligations; the kill test and cost measurement proposals | 9 |
| 4.9 closing order for D-02, R-23, O-FLOOR | 10 |
| 4.10 decisions needing the owner; #169 4.5-4.6 (Y5, Y6a-c, Y4) | 11 |
| #169 4.1 re-review of the reseed procedure and Section F; integrated review | 12 |

## 1. Fault model

This table is the one authoritative fault model for D-02, D-06 and the restart rules. Where an earlier document says
otherwise, this table wins (section 0). It restates `docs/AFR_001_D02_CRASH_DESIGN.md` section 0 and adds the rows
earlier documents left implicit.

**Budget.** On one node, over one execution history: any number of crashes and kills, plus **at most one** media
fault (FM-3 to FM-7). The one media fault may happen at any time, before, between or after crashes. Two media faults
on one node, or a device that loses more than one acknowledged write (FM-9), are out of scope. Faults on different nodes are independent;
replication (`m = 2`, HA_DURABLE) is what covers the loss of a node.

| # | Fault | In scope | Budget | Correlates with | Required behaviour | Guarantee | Evidence | Premise |
|---|---|---|---|---|---|---|---|---|
| FM-1 | crash or power loss at any instant | yes | unbounded | anything in scope | recover with no operator and no loss of acknowledged state | G-14, G-48 (no refusal from crashes alone). Today two crash-only refusals exist, both **unresolved violations**, not exceptions (section 3, rows marked C; Y8) | models `Completion.tla`, `MetaWatermark.tla`, `SegChain.tla` within bounds [FACT-MODEL]; the rest argued | FLUSH/FUA honoured (FM-9) |
| FM-2 | process kill or `abort()` (`storage_fatal`) with async `O_DIRECT` writes in flight | yes | unbounded | FM-1, one media fault | V8-1 to V8-3; no stray write is read as acknowledged state; no refusal | G-48 | Linux v6.8 and v6.12 source (note A.1, F.2); **no experiment** (E-1, section 9) | supported kernel (2.7) |
| FM-3 | one lost write: a whole write, data or in-place metadata, acknowledged by the device and never on media | yes | one media fault | crashes only | detect at or below the watermark and refuse (G-14(a)); never silent | G-14(b) | `MetaWatermark.tla` revisions 1-4 [FACT-MODEL] | the device loses at most that one write (FM-9) |
| FM-4 | torn write (part of a multi-sector write) | yes; at a crash it is FM-1 | as FM-1, or the one media fault | crashes | a torn tail above the watermark is discarded; a torn record slot loses to the other pair | G-14(a)/(b) | models above | 512-byte sector atomicity |
| FM-5 | zeroed or unreadable sector (latent sector error) | yes | one media fault | crashes | CRC fails: refuse, unless the bytes are void or above the watermark and of an allowed kind. A read error (`EIO`) during recovery is a **named refusal**, not FM-12's fatal restart: today `read_file` calls `storage_fatal` (`storage.cpp:52`), so it would loop (recorded, 4.1). Recovery reads whole files, so a read `EIO` refuses even on void bytes or bytes above `W`: over-refusal after a fault, allowed | G-14(a) | argued; CRC per frame and slot | — |
| FM-6 | misdirected write (right data, wrong place) | yes | one media fault | crashes | caught by nonce, `space_uuid`, stream index, generation and slot spacing: refuse | G-14(a) | `MetaWatermark.tla` adjacency case (rev 2) [FACT-MODEL] | — |
| FM-7 | one lost directory operation (create, rename, unlink) **on the WAL, record and watermark files** | yes | one media fault; a lost journal commit that drops several operations at once is out of scope | crashes | R-CHAIN, the OD-2 binding and `EEXIST` refuse every revival; never silent (OD-5) | G-14 | `SegChain.tla` rev 5 [FACT-MODEL] | directory `fsync` honoured. Not covered: `links_generation.tmp` and `role.tmp`, where a lost rename brings back the old value unrefused, and O-FLOOR's revived front segment (UNRESOLVED, 11) |
| FM-8 | metadata rollback: a whole node-record file returns to older valid content | **partly**: one lost slot write is FM-3 and covered; a restored or swapped file is not | — | — | four slots and rule 4 cover the one-write case; a file-level rollback is **R-23, open** | G-02, G-04, G-14 | `MetaWatermark.tla` record part [FACT-MODEL] | — |
| FM-9 | the device acknowledges FLUSH or FUA and later loses several related writes (volatile cache without power-loss protection, firmware fault) | **no** | — | — | none possible locally: the watermark and the log can agree on a shortened history (1.1) | — | — | FLUSH/FUA honoured; power-loss protection or cache off. R640: **not verified** |
| FM-10 | loss of a whole device or node | per node: no. Group: yes, **provided the surviving copy had no media fault on that write**, counting as that copy's media fault an FM-12 write or sync error that left the write only in the page cache (step 9); if it had (FM-3 on node A, FM-10 on node B, both within budget), the entry is lost, detected only when A restarts, and not at all while A is live and promoted (5.3) | — | independent across nodes | the group keeps every HA_DURABLE write on another member of S (P5) | G-11, P5 | R2′ models | `m = 2` |
| FM-11 | the same entry lost on two nodes | no | — | — | none | — | — | independent node faults |
| FM-12 | an I/O error returned on a durability **write** path (`EIO`, `ENOSPC` and others) | yes | unbounded | anything | fatal to the process, never retried; recovery then judges the files (2.5, note F.5). Read errors at recovery: FM-5. With the buffered fallback (`storage.cpp:541-546`), a write or `fsync` error can leave pages cached but not on media, which a restart without a reboot reads: UNRESOLVED; step 9's sync (2.2) only reports the error again if the kernel still holds it | G-14 | code check (note F.5); one nonconformance (4.1) | — |
| FM-13 | full disk | environmental | — | — | fatal (FM-12); a recovery that needs space loops without loss until space is freed (2.5) | none: an operator step | argued | disk budget (2.5) |
| FM-14 | hung device | liveness only | — | — | recovery waits; no safety effect (2.6) | G-48 bound **UNRESOLVED** | argued | device timeout; a command the block layer aborted never reaches media later |
| FM-15 | software bug, operator error (wrong directory, copied or swapped files) | no, except what the identity fields catch | — | — | a misplaced file with another `space_uuid` or stream index refuses | — | argued | — |
| FM-16 | memory or CPU corruption | no | — | — | none | — | — | ECC memory |

### 1.1 What the watermark detects, and what it does not [INF]

**Detects:** under the budget above, the loss of any acknowledged record by FM-3 to FM-7, data or watermark: an
acknowledged record is at or below the watermark (write-path rule 3), and its loss makes `W > E`, a CRC or term
mismatch at `W`, a binding mismatch or an R-CHAIN mismatch, each a refusal (design 3, 4, 5). A lost watermark write
is tolerated, not detected: recovery uses the older slot, `W` is lower, and no record still present loses coverage.

**Does not detect:**
- **FM-9:** a lying flush that drops a data write and the later watermark write together. The watermark then agrees
  with the shortened log, and acknowledged writes are lost silently on that node. The watermark is **not** proof
  against correlated loss.
- **FM-8 at file level:** the watermark file and the WAL returned together to an older consistent state (a restore).
- loss above `W`: such records were never acknowledged, so nothing needs detecting.

**Where that leaves HA_DURABLE:** FM-9 on one node is **uncovered**. The other copy survives it only if the damaged
node is not the one promoted, and under P5 nothing enforces that, so it is not coverage (5.3). Peer repair of a damaged promoted node is outside today's frozen rules (V6;
section 8).

## 2. The recovery state machine

### 2.1 What it reconciles

- Boundary rule 4 orders: read the record and manifest; remove orphan stream directories; complete PENDING intents;
  adopt a spare; delete below `retain_from`; FL-floor; scan, `H_s`, FL-chain. Completion runs before FL-floor because
  it may recreate the floor segment (boundary rule 7).
- Design 2 rule 4: before recovery acts, it restores two copies of the chosen generation, then makes the boot write.
- The note's F.1 proposed "phase 1 reads everything, phase 2 writes", with two errors: it read the watermark of every
  stream (design 4 reads it only for NONE streams; a PENDING stream's watermark may name `k > L` until step 2.7), and
  it checked FL-floor before a completion that may recreate the floor segment.
- Codex's direction: phase 1 establishes ownership, inspects without modifying, classifies and decides; phase 2 does
  only permitted idempotent repairs and completion, validates the result, establishes the writer, and serves only
  after the protocol's authority requirements.

### 2.2 The states

**Phase 1: read only.** Nothing is written to the data directory in steps 1-5.

| Step | Action | Outcome |
|---|---|---|
| 1. Own | directory lock, `LOCK_NB` (`engine.cpp:897-905`) | held: exit with a named error; the supervisor retries |
| 2. Lock the record | open `superblock.a` and `superblock.b` once each, without `O_CREAT`, `flock(LOCK_EX)` blocking (V8-L) | a missing file: step 3 decides |
| 3. Read the record | read all four slots and, for generation `g`, the manifest it names (design 2 rule 3) | no valid slot and the `seeding` marker present (E30): **SEED** (phase 2 removes only the seed's own files, `ckpt/`, stream directories, the record files, and fetches again; `links_generation*`, `role` and the lock file are kept). No valid slot and only init artefacts (record files, watermark files, empty stream directories, temporary files, `links_generation*`, `role`, the lock file), with `init` configured: **INIT** (init restarts; phase 2 is init's own sequence, which creates the watermark files and the links generation before the record write; without `init` configured such a directory refuses, `engine.cpp:956`, as today). No valid slot but segments or `ckpt` present: **REFUSE** (C13, `engine.cpp:943-946` with `--seed-from`, `:954-959` otherwise; section 3, row C1). Tie, or a missing or wrong-size file once a slot is valid: **REFUSE**. Today's code has no SEED outcome (row C1); it is the Y8 correction (E30) |
| 4. Lock the streams | for each stream of the record (its per-stream slots, so this works with no checkpoint yet), in stream order: open `watermark` without `O_CREAT` and lock it, keeping the descriptor; then, for `spare.tmp` and every segment, open, take the lock and close again (2.3) | any stream named by the record without `watermark`, NONE, PENDING or RESEEDING: **REFUSE** (V8-2 creates it before the record write that names the stream) |
| 5. Validate and classify | every stream, by its record state (2.4), and the links generation file, read only. Orphan stream directories are not read; any that step 8 would not remove (no checkpoint, or a checkpoint whose manifest's stream count differs from the record's) is a **REFUSE** here, so a stream directory is never left unreplayed and unrefused | any failure: **REFUSE**. Else **PROCEED**, plus **COMPLETE** for each PENDING stream and **RESEED** if any stream is RESEEDING. Under Y6c-1 RESEEDING covers every stream, so the two do not mix; under per-stream reseed they can |

**Phase 2: writes, each idempotent; a crash anywhere restarts at step 1.**

| Step | Action |
|---|---|
| 6. Repair the record | rule 4's duplicate of `g` if it has one copy, then the boot write (new incarnation), under `meta_mu`. The first write recovery makes |
| 7. Node files | open the links generation (`engine.cpp:1080`); write `DIR/role` through `role.tmp`. Today both happen elsewhere (E27). A links generation missing here, with a valid record, is a refusal; today a crash between `engine.cpp:1074` and `:1080` on a fresh boot produces exactly that (section 3, row C2) |
| 8. Clean and complete | remove orphan stream directories: a directory whose index is not below the record's stream count, removed only when the record names a checkpoint whose manifest has that count (boundary 4.2). With no checkpoint (`active_ckpt = 0`) such a directory is not removed: step 5 refuses, because today's code would have replayed it (`engine.cpp:1136-1195`). After a SEED outcome's record write, and on any boot that finds a valid record and a `seeding` marker: unlink the marker and `fsync` the directory, before step 10. Per stream that is not RESEEDING (RS-2 alone touches those): unlink `seg.tmp` and `wm.tmp`; resolve `spare.tmp` (note B.4); for a PENDING stream run completion 2.1-2.7 and the clear, then validate it again as a NONE stream, watermark included; delete segments below `retain_from` |
| 9. Cover the end | per NONE stream, one stream at a time: `fdatasync` every segment holding records in `(W, E]`, then raise the watermark to `E` (note D.4), then V8-1 at `E + 1`. Step 4's barrier means no write of the dead process is still in flight. With `wal_sync` on, every WAL write carries `RWF_DSYNC`, on the `O_DIRECT` and the buffered path alike (`src/native/runtime.cpp:213`, `:231`), so the sync finds nothing to do; it is a cheap guard for a previous boot with `wal_sync=none`, which stays outside every guarantee (2.7): that boot may already have raised `W` over records that were never durable, which no sync at recovery can repair (round 4). **A failed sync is a named refusal, not FM-12's restart**: the kernel reports a writeback error once, so a restarted process could sync "successfully" over cached bytes that are not on media and raise over them (round 4). A segment created during this boot (completion's `L + 1`) is not replaced again |
| 10. Writers | for every stream that is not RESEEDING: open the WAL writer; start the preparer |
| 11. Reseed | if the outcome was RESEED: first the boot read (R7), so that RS-3's term check and the choice of `j` use the current group record; then the reseed (whole node in place under Y6c-1, section 6; note C.2 per stream otherwise), which needs the network and the incarnation of step 6. It ends with RS-6 and continues at step 12 |
| 12. Authority | the boot read (R7) before the node grants anything to any primary. **A standby** is then positioned by R6.2 in the current term before it counts or holds a lease; its request reports the watermarked end (write-path rule 4). That a restart in the same term needs a new positioning rests on PR155's TEXT ONLY clause ("its positioned record for `k` ends at `k`'s restart"), not on R2′ rule 2 alone. **A primary** never resumes its old term (R6.0): it serves only through a new promotion, including a sole primary's re-promotion (frozen 4.2), and then S1-S5 with S2′ |

Why validation precedes repair [INF]:
- reading is not acting, so design 2's invariant ("restores the invariant before acting") holds when step 6 runs
  before the first write that acts on the record. The same `g` is chosen whether step 6 runs before or after step 5;
- a phase-1 refusal then leaves the data directory as the crash left it, for the operator. Steps 1-5 write only lock
  state, which is not on disk.

**Refusals that can still come in phase 2** [INF]: the revalidation of a completed PENDING stream in step 8
(FL-floor, the watermark), which needs a fault; `EEXIST` on spare adoption; a read `EIO` in phase 2 (step 2.7's read
of record `L`, the completion replay, a replacement header); a failed step-9 sync (a named refusal); and an I/O error
on a write (FM-12). A restart after either repeats
step 6's boot write once per attempt; that changes only the generation and incarnation, and rule 4 keeps the chosen
generation's two copies.

**No destructive repair erases evidence** [INF]. Every deletion in phase 2 removes only what phase 1 classified as
outside the judged state:

| Deletion | Why it holds no evidence |
|---|---|
| orphan stream directories | index at or above the stream count of the checkpoint the record names (boundary 4.2); never removed with no checkpoint (step 8) |
| `seg.tmp`, `wm.tmp`, a spare without a take-time header or record | never named (note B.4) |
| completion's segments above `L` | void by the durable intent (D-06 4.3 item 4); validated up to `L` in step 5 |
| segments below `retain_from` | below the published floor (boundary rule 3) |
| a RESEEDING stream's directory (RS-2) | the flag was set durably before anything was discarded (R6.5) |

A spare that holds a valid record without a header, or any bytes phase 1 cannot classify, is never deleted: it
refuses.

**Today's code writes earlier** [FACT-SNC, from the review]: `make_dirs` before the lock (`engine.cpp:895`);
`DIR/role` before the record is read (`:938`) and again at `:1025`; `seed_fetch` (`:949`); `take_lease_block` (a
control-plane call that can throw); the links generation (`:1080`); a lease commit (`:1222-1227`); the legacy-flag
commit (`:1243-1246`). Under this order each moves to phase 2,
or is a temporary file that recovery unlinks. Step 3 also counts empty stream directories as init artefacts, where
today's `has_data` (`list_streams`) refuses them. Listed as E27; not changed here.

**Migration precondition** [INF]: V8-L assumes the dead process took the locks. The first boot after an upgrade from
today's binary, which takes none, gets no barrier; it must follow a clean stop or a reboot.

### 2.3 Locks (Codex 2C: order, release, outstanding writes) [INF from FACT-SRC note A.1]

- **Order:** directory lock; `superblock.a`, `superblock.b`; then per manifest stream, in stream order, `watermark`,
  then the segment files in name order. Each lock is taken on the one locking descriptor that will do every write of
  that file, before the first read. A process holds at most one locking descriptor per file at a time (unlocked
  read-only descriptors aside, below); recovery closes a
  segment's descriptor before the writer reopens it, or hands the descriptor over.
- **Segments and spares too.** V8-2 named only the record and watermark files. The scan in step 5 reads segment
  blocks that a dead process's last flush may still be writing; without a barrier it may see those frames before the
  write completes, count them in `E`, and step 9 would cover them before they are durable. A later power loss leaves
  `W > E`: a refusal after crashes alone, and a counted copy that was never durable (round 1, r3 M1). So **every
  descriptor that carries an async `O_DIRECT` write takes `flock(LOCK_EX)` when it is opened, by whichever thread
  opens it**: the preparer for `spare.tmp` (the writer later writes through that same descriptor,
  `storage.cpp:690-713`, `:772-792`), the writer for a segment it opens. Recovery, before it reads a segment or
  `spare.tmp`, opens it, takes the lock blocking, and **closes it again**: once the lock is granted, no request of the
  dead process holds that file, so the barrier is passed and nothing needs holding. This keeps at most a few
  descriptors open, not one per retained segment (`RLIMIT_NOFILE`; round 2, i1 M2). Read-only descriptors
  (`read_file`, `read_stream_range`, `storage.cpp:37-58`, `:452`) never lock.
- **Watermark descriptors** are opened and locked in step 4 and handed to the stream's serial writer; nothing opens
  them a second time. A remap that adds streams creates their watermark files before the record write that names
  them, as init does (V8-2's creation order); otherwise a crash leaves a NONE stream without one (round 2, i1 M10).
- **Release:** never explicit. After the process dies, each lock goes in `__fput` when the last reference drops; an
  async `O_DIRECT` request holds a reference until its bio completes. The directory lock has no request and goes at
  `exit_files`.
- **Durable, not just complete:** with `wal_sync` on, WAL writes carry `RWF_DSYNC`, so a write that completed is on
  stable media (FUA or a flush, as the device honours them, FM-9). The buffered fallback uses the same flag (`runtime.cpp:231`).
  Records written with `wal_sync` off are complete but not known durable; step 9 syncs before it raises, as a guard
  only (2.2 step 9).
- **No deadlock:** a dead process takes no locks, so the new one only waits; a second new process stops at step 1.
  An in-process restart must close its V8-L descriptors only after its rings drained, or it would wait on itself.
- **Outstanding writes at termination:** buffered writes and synchronous `O_DIRECT` drain at exit (io-wq); AIO drains
  in `exit_mm`; async `O_DIRECT` bios run to completion or error after the task is gone, and only then release their
  file (note A.1, F.2). Writes through `wm.tmp`, like every write in B.2-B.3, are synchronous or awaited before the
  rename (B.2 step 0).

### 2.4 Step 5 per stream, including the PENDING floor case [INF]

- **NONE:** `spare.tmp` by B.4's checks, read as adopted if B.4 would adopt it; FL-floor; the scan with the void rule;
  FL-chain; R-CHAIN; the binding; the watermark table (design 5); the checkpoint part (`storage.cpp:485-492`, and the
  object count, `engine.cpp:1115`); and, if a file named `E + 1` exists, its header (B.3, replacement source).
- **RESEEDING:** nothing. The stream will be emptied.
- **PENDING `(t_j, L)`**, the watermark not read (design 4):
  - `spare.tmp` with a valid take-time header for `X ≤ L` is read as adopted (D-06 2.2), before the floor check: a
    lost rename of the floor segment then does not refuse where completion would continue;
  - `L < retain_from − 1`: **REFUSE**. The intent step requires `L ≥ max_lsn ≥ start_lsn − 1 ≥ retain_from − 1`
    (boundary rule 7), checkpoints and intents are serialized by `control_mu_`, and no checkpoint runs while a stream
    is PENDING, so only damage gives this;
  - `L ≥ retain_from`: the floor segment starts at or below `L`, and completion deletes only segments that start above
    `L`. Check FL-floor; scan to `L`; FL-chain and R-CHAIN up to `L`; record `L` present with a valid CRC (step 2.7
    reads its term and CRC); the part's `max_lsn ≤ L`;
  - `L = retain_from − 1`: no retained record survives the truncation. Then `max_lsn = start_lsn − 1 = L`. The floor
    segment starts above `L`, so it is void by the intent, and completion recreates it with
    `prev_term = term(L) = floor_term` (rule 7). Check the part and that its `max_lsn = L`. FL-floor is checked in
    step 8, after completion; in this case it compares the segment completion just wrote from `floor_term` with
    `floor_term`, so it verifies nothing independent: `floor_term` rests on the record's CRC alone. Nothing below `L`
    is retained, so nothing can be harmed;
  - with no checkpoint yet (`active_ckpt = 0`, boundary rule 6), the floor is `(0, 0)`, `retain_from = 1`, and `L`
    may be any value at or above 0 (a later boot need not take a checkpoint, `engine.cpp:1250`). Then `L ≥ retain_from`
    or `L = 0 = retain_from − 1`, both covered above; there is no part to check.
- **So recovery can validate a PENDING stream whose floor segment must first be recreated:** it checks the part and
  the intent's consistency in phase 1 and FL-floor after completion, which is boundary rule 4's own reason for its
  order.

### 2.5 Space (Codex 2C: ENOSPC) [INF, HYP where marked]

- A replacement (V8-1 when `E + 1` exists) allocates `seg.tmp` before the rename frees the old inode: one segment of
  headroom at that moment. Usually no file named `E + 1` exists, and step 9 **creates** one new segment per stream,
  which nothing frees (review r2 m3).
- Step 8 unlinks prepared spares (no take-time header), which frees a segment per stream before step 9, but step 10's
  preparers allocate them again. Net, recovery needs one segment per stream beyond the state before the crash
  [INF from `storage.cpp:772-792`].
- **If the disk is full:** `ENOSPC` is fatal (FM-12); nothing is lost; the next recovery unlinks `seg.tmp` and fails
  the same way. The node loops without service until space is freed: an operator step.
- **Proposal [HYP]:** count one segment per stream beyond the spare as reserved in the disk budget that reclaim
  already computes (boundary rule 1). Then `ENOSPC` at recovery means something outside SNC filled the disk. Whether
  G-48 lists that as an exception is the owner's question (11, other items).
- Not taken, as a new mechanism: using a prepared spare as `E + 1` instead of formatting a new segment. The dead
  process could not write a spare without a take-time header, so it would be safe and would save a format per stream
  [INF]; it changes OD-8's adoption rule and is left for Y4.

### 2.6 A hung device, and G-48 (Codex 2C) [INF]

- A V8-L wait lasts while a dead process's write is stuck: until the block layer completes or fails it (its command
  timeout, then error handling). Under dm-multipath `queue_if_no_path` it can last forever. The bound is the
  platform's; the R640's values were not read (E-3): **UNRESOLVED**.
- **Recommendation:** keep the blocking `flock`; log a named state ("waiting for the previous writer's I/O") at start
  and at intervals. No timeout that gives up the wait: reading then would defeat V8-L.

**G-48 recovery time, the added terms [HYP, unmeasured]:**

| Term | Size |
|---|---|
| V8-L wait | 0 normally; up to the device's timeout and error handling |
| step 1 retry | the supervisor's interval, if the old process is still in `do_exit` |
| step 5, and step 8's second validation of a completed PENDING stream | one scan per stream; two for a PENDING stream; B.4 reads each spare in full |
| step 9 | an `RWF_DSYNC` watermark write and a whole formatted segment (64 MiB at the default, `snc.h:371`; `engine.cpp:1205`) per stream, serially |
| step 10 | one formatted spare per stream (K1) |
| PENDING completion | a checkpoint load and replay to `L` (D-06 4.3, "Time bound") |
| reseed | the transfer (6.3) |

Formatting grows with the stream count and may dominate a clean restart. **UNRESOLVED as a G-48 number** (X-1, not
authorized).

### 2.7 Supported kernel, file system and device (Codex 2C)

The restart-safety argument holds only under these. Anything else is unsupported until read or measured.

| Area | Supported | Status |
|---|---|---|
| Kernel | upstream Linux v6.8 and v6.12, the two trees read (note A.1, F.2) | Ubuntu `6.8.0-149-generic` on the R640: upstream base matches, Ubuntu patches **not read** (E-3) |
| File system | ext4 with `data=ordered`, a journal and barriers (defaults), or xfs with defaults; on a local disk or a single partition | R640: ext4, defaults [FACT-SNC `docs/R640_TOPOLOGY.md` 40] |
| Not supported | btrfs and other copy-on-write file systems; NFS and network file systems; tmpfs; overlayfs; ext4 `data=journal`, `data=writeback`, no journal, `nobarrier`; DAX; md, dm and multipath stacks | by statement; not tested |
| `renameat2(RENAME_NOREPLACE)` | ext4 and xfs (v6.8 `fs/ext4/namei.c:4202`, `fs/xfs/xfs_iops.c:458`, review r2) | ext4: supported |
| `O_DIRECT` | the WAL uses it where the file system allows; otherwise writes are buffered and V8 does not arise (`storage.cpp:541-546`) | as today |
| Device | FLUSH and FUA honoured; power-loss protection or the volatile cache off; 512-byte sector writes atomic | R640: `WCE 1`, `fua=1`; power-loss protection **not verified** (E-3) |
| io_uring use | no registered files, no SQPOLL, no `fork` | SNC today: none [FACT-SNC `src/native/runtime.cpp:514`] |
| `wal_sync` | on | default on; `wal_sync=none` is outside every guarantee |

## 3. Refusal-to-repair matrix

**What a refusal means.** A refusal for damage (kind D) is a G-14(a) detection, and under the budget it never follows
crashes alone (1). Two refusals today follow crashes alone (kind **C**). They are **not** G-14(a) detections and no
accepted text makes them exceptions to G-48: they are unresolved violations of G-48 and of design 0 requirement 2,
with a proposed correction for each (decisions package, Y8). Other refusals below are not damage at all (a version or
configuration mismatch).

**How a refusal exits.** Today a refusal either throws or calls `storage_fatal` (`abort()`, for example
`storage.cpp:384`, `engine.cpp:1039`, `:1115`), and a supervisor restarts either one, so every refusal loops; nothing
is lost, but nothing distinguishes a refusal from a transient failure. Proposed: every refusal exits with one stable
status that the supervisor does not restart, and FM-12's fatal errors keep `abort()` (E27). Refusals found in
phase 1 leave the data directory as the crash left it (2.2); the few found in phase 2 are listed there.

**Default for a damage refusal** (rows marked D):
- **S and A:** the node does not serve, misses the failure-detection timeout and is removed by the ordinary shrink
  (R2′ rule 5);
- **evidence:** the data directory untouched; the operator copies it before any repair;
- **repair:** an operator-started reseed (whole node in place under Y6c-1; note C.2 per stream otherwise). R6.4
  already makes a reseed after damage an operator action;
- **rejoin:** after the reseed, R7's boot read, R6.2 positioning in the current term, then R1's grow;
- **sole copy** (degraded `S = {p}`, a standalone node, or any node holding LOCAL_DURABLE writes no peer has): a
  reseed would lose them. The operator chooses between a restore from outside SNC and an unsafe override (G-35,
  case 5c); SNC never chooses.

| Refusal | Violated invariant | Kind | Exception to the default |
|---|---|---|---|
| no valid record, but data present (segments or `ckpt`), after a fault | C13 | D | — |
| **C1:** the same state after a crash during an empty-directory seed (a new node, or the slot-epoch mismatch case, E14′) | G-48; design 0 requirement 2 | **C** | today the operator clears the directory, then seeds. Proposed correction: a seed marker (decisions package, Y8) |
| **C2:** valid record, links generation missing, after a crash between the first record write and the links generation's creation (`engine.cpp:1074`, `:1080`; `links_generation.cpp:85-90`) | G-48; design 0 requirement 2 | **C** | today the operator passes `--links-generation-min`. Proposed correction: create it before the first record write (Y8) |
| record: two valid slots, same generation, different bytes | design 2 tie rule | D | — |
| record: file missing or wrong size once a slot is valid | design 2 read rule | D | — |
| record, manifest, part, WAL or watermark: valid CRC, unknown format version (E17, G-49/G-50) | format versions | not damage | no reseed: run a binary that reads the version; stays out of S until then |
| type-catalog mismatch (`engine.cpp:1044`) | catalog | not damage | no reseed: run the matching binary |
| shard-count mismatch for a cluster member (`engine.cpp:1054`) | slot map | not damage | no reseed: the remap procedure, or start with the group's shard count |
| manifest, checkpoint directory or part missing or invalid (`engine.cpp:1039`, `storage.cpp:485`) | boundary rule 2 | D | — |
| read error (`EIO`) during recovery (FM-5), in phase 1 or 2 | G-14(a) | D | if the device is failing, replace it first |
| step 9's sync fails | G-14 | D (a write error is a media fault, FM-10's proviso) | not restarted: a second sync could report success over bytes not on media; replace the device, then reseed |
| an orphan stream directory that step 8 may not remove (step 5) | boundary 4.2 | not damage | no reseed: the operator inspects it (a remap before the first checkpoint, or a record and manifest with different stream counts) |
| FL-floor or FL-chain mismatch | FL | D | — |
| R-CHAIN mismatch | OD-7 | D | — |
| WAL gap (`storage.cpp:383-384`); invalid header on a named segment; damage above `W` that is not a torn frame or format sector | G-14(a), K1 | D | — |
| watermark: `W > E`; CRC or term mismatch at `W`; floor-term mismatch; binding missing or wrong nonce | G-14(b) | D | — |
| NONE stream without a watermark file | V8-2 creation order | D | — |
| `spare.tmp`: a record without a take-time header; another `space_uuid` or stream; `EEXIST` on adoption | OD-8 | D | — |
| replacement source (`E + 1`) header invalid | K1 | D | — |
| PENDING: `L < retain_from − 1`; record `L` missing or invalid | boundary rule 7; step 2.7 | D | — |
| damaged `H_k` | PR157 | D | — |
| links generation truncated or corrupt (`links_generation.cpp:97-101`) | incarnation uniqueness | not WAL damage | no reseed: the operator sets `--links-generation-min` above any generation used, as today |
| directory lock held (step 1) | one engine per directory | transient | the supervisor retries; nothing else |
| `ENOSPC` loop (FM-13) | — | environmental | free space; nothing lost |
| device hang (FM-14) | — | environmental | wait, or replace the device |

## 4. Reconciliation: the 16 findings against #169 and this package

Source: `docs/handoff/Coordinator_Adversarial_Assessment_2026-11-18.md`. "Closed" means closed as a proposal here,
subject to Codex and the owner; nothing is accepted.

| # | Finding | #169 | Now | Where |
|---|---|---|---|---|
| 1 | no written fault model | not addressed | **closed** (proposal) | section 1 |
| 2 | watermark assumes independent failure | stated in F.4 | **closed** as a stated limit; evidence held by peers is V6, open | 1.1, 8 |
| 3 | recovery order wrong; witness placement omitted | F.1, with two errors | **partial**: one state machine, errors fixed, witness placement and the boot read in steps 11-12; round 3 found four more gaps, fixed on site, not re-reviewed | 2 |
| 4 | no I/O error policy | rule in F.5; `links_generation` callers unchecked | **partial**: the callers are now checked and one does not conform (4.1) | 4.1 |
| 5 | file system not pinned | F.6 | **partial**: pinned; Ubuntu patches and power-loss protection unverified | 2.7 |
| 6 | V8 needs a test | v6.8 and v6.12 source read; truncate is no barrier | **partial**: experiment E-1 specified, not run | 9 |
| 7 | fresh inodes (V8-R) | evaluated in F.2; fallback | **partial**: V8-L leads, V8-R fallback; depends on row 6 (E-1 not run, Ubuntu patches not read) | note F.2; 11 (Y6a) |
| 8 | nonce + `prev_nonce` versus etcd's chain | not addressed | **closed** (comparison): see 8.2 | 8.2 |
| 9 | same-ID replacement may be unnecessary | F.3: a replaced segment is always the newest; the nonce is kept for the OD-2 binding | **closed**: OD-9 kept, reason restated | note F.3 |
| 10 | which rollback fields the witness can cover | not addressed | **closed** (analysis): none of the local fields | 8.3 |
| 11 | 64-bit IDs fine; ID source at early boot | not addressed | **closed** (proposal): nonces from `getrandom(…, 0)` as `space_uuid` is today, fatal on a short read (`storage.cpp:193-194`) | 4.2 |
| 12 | refusal has no repair path; reseed is load-bearing | RS-0..RS-6 | **partial**: refusal matrix and amended reseed steps; reseed pins (E29) and liveness under repeated term changes UNRESOLVED | 3, 6 |
| 13 | watermark cost unmeasured | not addressed | **open**: measurement E-2 specified, not run | 9 |
| 14 | FAST 2018 not read | not addressed | **closed** (reading) | 8.1 |
| 15 | ambition level undecided | not addressed | **open**: owner decision Y7 | 11 |
| 16 | unreviewed fixes; text spread over many documents | #169's section F unreviewed | **partial**: C, F and this package reviewed (12.1, 12.2), and the round-2 fixes re-reviewed in round 3 (decisions package); round 3's own fixes are not re-reviewed; the byte-level storage specification follows Y4 | 0, 12 |

### 4.1 I/O errors: the `links_generation` caller [FACT-SNC, INF]

- `LinksGeneration::store` returns `false` on a failed write, `fsync`, `rename` or directory `fsync`
  (`links_generation.cpp:117-137`). At startup the caller throws (`:108-111`): fatal, conforms.
- At run time `next()` returns `false` and keeps the old value (`:150-155`); `Shard::start_links` then leaves the
  links closed and logs once (`shard.cpp:1871-1877`). A later `start_links` calls `next()` again, which writes a new
  `.tmp` and retries the rename and the directory `fsync`.
- **Formal nonconformance with F.5's rule [INF]:** the store is retried. The retry stores the same `v`, and a
  successful directory `fsync` makes it durable, so a reuse of `v` needs a directory `fsync` that reports success
  after an earlier failure without the rename being durable. Whether ext4 can do that is not established [HYP].
- **Also recorded:** `read_file` treats a read `EIO` as fatal (`storage.cpp:52`), so an unreadable sector loops
  instead of refusing by name (FM-5).
- **Recorded, not fixed:** C++ is not authorized. The conforming rules are "a failed store is fatal" and "a read error
  at recovery is a named refusal".

### 4.2 Nonce source (finding 11)

One format line, proposed: every nonce (segment, spare, binding) is drawn with `getrandom(buf, 8, 0)`, which blocks
until the kernel's random pool is ready, and a short read is fatal, as `space_uuid` already is
(`storage.cpp:193-194`) [FACT-SNC]. No new mechanism.

## 5. The acknowledgement revision: R9-A′ (Codex 2D)

### 5.1 The precise text

Frozen R9-A, as it stood before OD-10 (`docs/AFR_001_SAFETY_ANALYSIS.md` R9) [FACT-SNC]:

> **R9-A. Apply before acknowledging.** Every replica (a member of A, or one catching up to join it) reports an entry
> as acknowledged only once it has applied it, whether or not it holds a lease at that moment. p counts no other
> report. An acknowledgement sent before a lease was granted can be counted while that lease is valid (RESULTS_R9
> M_ackLeased, M_ackDurable).

R9-A′, changing only the condition of the first sentence and keeping every other clause [proposed here; **recorded
2026-11-19 as owner decision OD-10**, `docs/handoff/PR171_Owner_Decisions.md`, and now in SA's R9 in place]:

> **R9-A′. Apply and cover before acknowledging.** Every replica (a member of A, or one catching up to join it)
> reports an entry as acknowledged only once it has applied it **and its durable watermark (D-02) covers it**, whether
> or not it holds a lease at that moment. p counts no other report. An acknowledgement sent before a lease was granted
> can be counted while that lease is valid (RESULTS_R9 M_ackLeased, M_ackDurable).

"p counts no other report" stays: without it, today's durable-only report (`shard.cpp:2234` sends
`wal_->durable_lsn()`) would not be excluded (review r3 M2). It is a **strengthening** of frozen text, as Codex says,
not an interpretation, and needs the owner's yes.

### 5.2 Texts whose meaning moves (complete list) [INF]

| Text | Status | Words kept | Meaning after R9-A′ |
|---|---|---|---|
| R9-A | conditionally frozen (PR155) | all but the condition | a report is min(applied, watermarked) |
| R9-B lease test | conditionally frozen | words change: "judged by the applied position `m` reports (R9-A)" becomes "judged by the position `m` reports (R9-A′)", since the old words become literally false | `m`'s watermark must reach `max(inh_p, ack_p)` |
| R9-B `ack_p` | conditionally frozen | words change: "reported every entry up to it as applied (R9-A)" becomes "as acknowledged (R9-A′)" | lags each member's watermark, not its applied position |
| R9-F and G-30 (F) | conditionally frozen | — | `ack_p` lags, so `c_m` waits longer |
| G-30 (A) and (B) (`docs/SNC_GUARANTEES.md` 186) | conditionally frozen with R9 | words change: "only once it has applied it" and (B)'s "has applied (by its own report)" follow R9-A′ | as R9-A′ |
| HA_DURABLE definition (`docs/CORE_CONTRACTS.md` 126, 148) | accepted | "durable on … every member a witness may promote" | durable and watermarked |
| SA section 3's preservation table | analysis | — | its R9 rows argue R9-A; not re-argued |
| R2′ rule 2, S5 | conditionally frozen | a member "counts toward an acknowledgement" once positioned | the count is of R9-A′ reports |
| R6.3′ item 1 | frozen except TEXT ONLY parts | "`k`'s last term and durable end LSN" | the durable end reported is the watermarked end (write-path rule 4) |
| "caught up" (SA section 2), R1.1, S admission | protocol under analysis | the durable end | the watermarked end, through write-path rule 4 (not through R9-A′ itself) |
| section 0's acknowledgement (SA 107-109) | not frozen | "has `e` durably" | "durably, under its watermark" |
| G-11 (`docs/SNC_GUARANTEES.md` 136) | HA_DURABLE clause ACCEPTED (PR148) | "at least two durable copies" | a copy counts only once watermarked |
| D-03 (`docs/handoff/PR148_Owner_Decisions.md` 48) | accepted direction | "durable acknowledgment from every required member" | from every required member, watermarked |
| R9-C | conditionally frozen | "every acknowledgement needs every member of A" | counts R9-A′ reports |
| PR155's accepted argument "R9-A for replicas still catching up" | accepted as argued | — | now argued for R9-A′; not re-argued here |
| R9-A's evidence citation "RESULTS_R9 M_ackLeased, M_ackDurable" | kept in the text | — | those runs checked R9-A, not R9-A′: no run backs the new condition |
| D-06 E18, E20, E25 | proposed revisions | — | tied to this decision |
| the primary's own copy | section 0 (not frozen) and write-path rule 3 | — | R9-A′ binds replicas only; the primary's copy is covered by its own watermark through write-path rule 3, argued only (note D.3 calls it debatable) |

**Unchanged:** R9-D, R9-E, `m = 2`, the `m = 1` LOCAL_DURABLE rule, P1-P4, P6, P7, S1-S4.

### 5.3 Effects [INF]

| Affected | Effect |
|---|---|
| Lease test, `ack_p` part | already met: every member of A when `ack_p` was recorded reported a watermark at or above it, and a watermark falls only through step 2.7, which stays above every common entry (I3), or through a lost watermark write (FM-3), which step 9 of 2.2 repairs at the next recovery before any report. A member added to A later waits on its R9-A′ report, as under R9-A on its applied position |
| Lease test, `inh_p` part | the new wait, after a promotion: the survivor's watermark follows its last flush through the serial writer (write-path rules 1-2) without needing a new flush, so the wait ends |
| `ack_p` and HA_DURABLE latency | one watermark write per flush on each replica before its report (unmeasured; E-2 estimates the device part only) |
| Lease eligibility | the other R9-B conditions (A, R4, positioned, no pending intent) unchanged |
| R6.3′ durable end | steps 2 and 3 of R6.2 use the reported end only for the binding and the answer; `k` truncates by `end_k(t*)` of its own log, so a reported end below the held end cuts nothing more. A running member that learns a new term may have a watermark write in flight: if `W` rises after its request, the binding no longer matches, the reply is rejected and `k` asks again, which converges. After a restart, step 9 of 2.2 makes the two ends equal before any request |
| Promotion | every entry acknowledged in a term is covered by the watermark of every member of A that counted it. A member admitted to S later is covered through write-path rule 4 at admission ("caught up"). Under the one-fault budget, a single lost write of such an entry makes that member's **recovery** refuse; a live promoted member with a lost write is detected only when it restarts. So R9-A′ is a necessary part of what P5's excluded silent-loss clause needs, not all of it: a live promoted member with a lost write, and FM-9, stay uncovered. **P5's clause stays outside the freeze while D-02 is on HOLD**: a yes to Y5 does not close it |
| Reconciliation | `j` answers from its log, not its watermark (R6.2 item 2), unchanged; `k`'s truncation lowers its watermark in step 2.7 |
| Recovery raise (step 9) | with `wal_sync` on, covers only records that recovery validated and that are durable (2.2 step 9; a failed sync refuses), so it hides no loss and adds no acknowledgement retroactively. A raised `W` over a divergent tail is lowered by the truncation that removes it (step 2.7), or the node is reseeded |
| HA_DURABLE, `m = 2` | preserved: R9-A′ only removes reports. The raise does not add one, because, with `wal_sync` on and no media fault (FM-10's proviso), what it covers is durable |

Evidence: argument only. No model checks R9-A′, and none is proposed (9).

## 6. Reseed: whole node or per stream, and R6.5 (Codex 2A, 2B)

### 6.1 Facts that decide it [FACT-SNC]

- S and A are sets of nodes; one group record holds one term and one in-sync set
  (`docs/AFR_001_SAFETY_ANALYSIS.md` 44-52). Reseeding one stream takes the whole node out of S (R2′ rule 3).
- The code reseeds only a whole node, into an empty directory (`seed.cpp:225-275`; `engine.cpp:940-946`), and copies
  the primary's `next_lease` (`seed.cpp:131`, `:269`).
- Keys spread over 4096 hash slots across the node's streams (`docs/AFR_001_INVENTORY.md` 59).

### 6.2 When R6.4 fires [INF, HYP where marked]

| Trigger | Typical cause | Streams affected |
|---|---|---|
| truncation point below `k`'s checkpoint `max_lsn` | a deposed primary or lagging member holds a divergent tail that a node-wide checkpoint captured | every stream with a divergent tail at that checkpoint: under spread load most or all; under a hot-object load few [HYP] |
| `H_j` no longer covers `t_k` | `k` was away longer than `j`'s retention | every stream (one node-wide checkpoint) |
| damaged `H_k`, or any refusal (section 3) | a fault | operator reseed of the whole node |

### 6.3 Comparison

| | Whole node, in place | Per stream (note C.2) |
|---|---|---|
| Data transferred | every part and every stream's WAL from its `start_lsn` | one part and one WAL: saves up to `(N−1)/N` of the bytes, **only** when one stream needs it (6.2) |
| Time out of S [HYP] | (parts + WAL) / min(network, primary read, local write), plus a memory load and one checkpoint | the same formula for one stream; the other streams can run R6.2 meanwhile, so per stream finishes sooner by the transfer of `N−1` streams |
| Checkpoint coordination | RS-6 checkpoints every stream, all loaded from the primary; no gate exception | RS-6 walks live streams and the reseeded one; ordinary checkpoints are refused for the whole transfer except RS-6's (E16) |
| Memory reconstruction | the node's memory is discarded and loaded, as a recovery loads it | one stream discarded and rebuilt while the other shards run; the discard must not race them |
| Replication catch-up | every stream from its `start_lsn` | one stream |
| Reclaim, local | no other live stream | every other stream's reclaim waits for the transfer (UNRESOLVED bound) |
| Reclaim, primary | the primary keeps its checkpoint and WAL for every stream during the transfer | for one stream. Today's pins do not fit either case (6.5, pins row) |
| Remapping | refused across slot-map epochs, checked at the start and at RS-6; on a mismatch the only path is today's wipe and seed, where V7 applies | the same, plus a remap on the primary mid-transfer with no defined mapping for one stream (UNRESOLVED) |
| Record states | RS-1 and RS-6 are each one write over every stream | one stream RESEEDING beside NONE and PENDING streams; D-06 4.1's "two streams reseeding clear independently" then conflicts with one reseed per node (E28) |
| Copies kept during the transfer | none: every stream is emptied. If the primary is lost mid-transfer, the node holds nothing, even for an unsafe recovery (G-35) | `N−1` stale streams, usable only under G-35 |
| Implementation | today's seed path, in place, plus the note's new steps (watermark, floor segment, binding); the record and its generations continue, so V7 does not arise unless the epoch mismatches | the above plus a per-stream discard, a checkpoint gate exception, mixed record states, a per-stream pin and the remap case |

### 6.4 Recommendation [INF]

**Whole node, in place, as the initial direction**, which is also Codex's preference. Per stream saves data and time
only when one stream needs reseeding, which the common triggers do not produce (6.2), and it keeps the node out of S
either way. Its real gains, a smaller pin on the primary and stale copies kept for an unsafe recovery, do not justify
its complexity now. Per stream stays a later option if measurements show a skewed-load case that matters. **Not
selected: the owner's decision (Y6c).**

### 6.5 The reseed steps, amended (note C.2, read for every stream)

Review r1's findings change C.2 as follows. The note's C.2 is marked as amended by this section.

| Step | Amendment | Why |
|---|---|---|
| preconditions | the node has left S by a chosen shrink (R2′ rule 3), which it learns from the witness. "Not in A" is not checked by `k`: `p` drops it after the shrink (R1.2), and `k` sends no report while RESEEDING. Every stream NONE: a PENDING stream completes first | `k` cannot see `p`'s A (r1 F7) |
| RS-0 | `k` also stops serving lease reads and drops any lease it holds, before RS-1. R9-C keeps `k` in `p`'s A while a lease may be valid, so `k` must not serve from the map RS-2 empties | G-30 (round 2, i2 M5) |
| RS-1 | a compare-and-write on every stream NONE **and on the durable term**; the same record write takes a **new incarnation**, so every primary's record that `k` is positioned ends (R6.3′ item 1, answering side) | as D-06 4.3's intent (r1 F2); round 4: without a new incarnation a primary that had positioned `k` before the reseed would still count it as positioned |
| RS-3 | `k` first persists `j`'s term `t` (R4), as a compare-and-write on RESEEDING, serialized like R6.3′ item 2; if `t` is below `k`'s durable term, `j` is not the current primary and `k` asks again. The request is bound to `t`, `k`'s incarnation and `j`'s incarnation; `k` receives the primary's `next_lease` with the parts | today's seed sets the record term from the primary (`seed.cpp:271`) and copies `next_lease`; without it a reseeded node can reissue a lease ID whose objects were deleted before `start_lsn` (r1 F2, F3) |
| RS-3 to RS-5 | if `k` learns a newer term, it persists it (R4) and returns to RS-2. Repeated term changes restart the whole transfer, with no bound (UNRESOLVED) | otherwise RS-5 stalls. RS-6's compare checks only terms `k` has learned, so a deposed primary's tail is caught by the R6.2 round after the clear (6.6), not by RS-6 (round 3, rr2 m4): any cut of the copied tail is below `c + 1`'s `max_lsn`, so R6.4 reseeds again (round 4). A failed compare at RS-1 ends the reseed with nothing changed; at RS-3 to RS-6, or a change of `j`'s incarnation, `k` returns to RS-2 |
| pins | the primary must keep the checkpoint until the parts are sent and the WAL from each `start_lsn` until RS-5 ends or `k` releases it. Today's pins do not do this: the checkpoint pin ends at the standby's "done" (`seed.cpp:166`, `:170-180`), the WAL pin is one global one-hour deadline released once the standby's `keep_from` passes it (`engine.cpp:1514-1519`), and the seed gate forces the primary NOT READY (`seed.cpp:139`). A long transfer can lose its pin and loop RS-5 to RS-2 | UNRESOLVED as a design item (E29) |
| RS-6 | a compare-and-write on the term `t` and every stream RESEEDING. `next_lease` = max(`k`'s durable `next_lease`, the primary's, every shard's `max_lease_seen + 1`), as the code computes it at a checkpoint (`engine.cpp:1533`); the lease-block case (`lease_hi ≠ 0`) is UNRESOLVED. Each stream's `retain_from` is its `start_lsn` (the first retained segment, as boundary rule 1 computes when nothing is deleted) and `floor_term = start_term`; `c + 1`'s own `start_lsn`, `start_term` and `max_lsn` are those of an ordinary checkpoint. The flag is cleared in this write; `k` is **not** positioned by it, and runs one ordinary R6.2 round next (6.6) | r1 F2, F3, F10; round 2, i2 M1, M2 |

### 6.6 R6.5: clear at RS-6, then position by R6.2 (a proposed revision) [INF]

R6.5 (`docs/AFR_001_SAFETY_ANALYSIS.md` 224-228): the flag is "cleared only once the copied state is durable and
positioned at a `(term, lsn)` of `j`'s log." It is not on PR155's freeze list by name, but frozen text depends on it:
R2′ rule 3, R6.3′ item 1 ("not incomplete or reseeding", `docs/AFR_001_PROMOTION_DURING_RECON.md` 46), P6's
attestation ("not reseeding") and R2′'s promotion evidence; I2 is stated for members "that are not incomplete"
[FACT-SNC]. Here "reseeding" and "incomplete" both mean exactly the durable flag. Its meaning is treated as frozen.

**Three ways to meet it, and why two fail:**
- **Literal, through R6.2 before the clear:** not available. R6.3′ item 1 lets `k` use an R6.2 reply only if it is
  "not incomplete" (round 1, r1 F1).
- **Treat RS-6 as the positioning, as the model's `RDone` does** (`spec/tla/Log.tla` 218-223 clears the flag and sets
  `pos` in one step, guarded by `isPrim[j]` and `Prefix(dlog[k], ilog[j])`): rejected after round 2 (i2 M1, M2).
  Frozen text ties positioning to R6.2 (R2′ rule 2: "completed R6.2/R6.3 against j's log"; R9-B; R1.1's "caught up";
  I2's induction), so this would change four frozen texts silently. And RS-6 cannot check `j`'s state as `RDone`
  does: if `j` restarted mid-transfer and lost a tail it had shipped (R6.0), `k` would clear while holding entries in
  no log of `j`.
- **Clear at RS-6, unpositioned, then one ordinary R6.2 round: recommended.** After RS-6 `k` is a member that is neither
  incomplete nor reseeding, and not positioned, so R6.3′ item 1 allows the round. If `j` lost a tail, the round finds the cut, which
  is below `c + 1`'s `max_lsn`, so R6.4 reseeds again (a liveness cost, UNRESOLVED as a bound).

**What still holds between the clear and positioning:** leases and growth into S need positioning (R9-B, R1; the
model's `AddA` and `Grow`, `Log.tla` 96-117); promotion inside the contract names a member of S, which `k` is not.
**Acknowledgements are not held off by the cited rules** (round 3, rr2 M3): R2′ rule 2 binds members of S′, which `k`
is not, and R1.1 adds a catching-up member to A before it has caught up. If `j` lost a shipped tail and was
re-promoted, `k` could hold an older-term entry at an LSN `j` reuses, and an LSN-only report would count it. G-12
keeps a second, real copy in S, so this breaks section 0's "every member of A has `e`", not G-11 (round 4); the
revision says that `k` sends no acknowledgement report until R6.2 positions it (text below), as defence in depth.
The analysis of this state is `docs/AFR_001_R65_INVARIANTS.md`. Whether
reports carry `(term, lsn)` or only an LSN is UNRESOLVED.

**Invariants not re-argued for the new state** (round 3, rr2 M1): I2's induction assumes a member accepts entries
only after R6.2 has positioned it, and after the clear `k` is "not incomplete" while holding RS-3 to RS-5 data taken
unpositioned. I3 (through I2's contiguity), I4 step 6 and I5's three states also never describe "complete,
unpositioned". The intended argument [INF]: until its R6.2 round, `k` sends no report and serves nothing, so I2's
hypothesis is applied to `k` only from its positioning on; the round truncates `k` to the common prefix (I3). This is
argument only and must be re-argued in SA before Y6c-2 can be accepted. R7's promotion outside S is in the contract with loss accepted (G-35)
and could promote `k` as any node outside S; `Log.tla` has no outside-S promotion, so no model covers it.

**Supporting evidence, with limits:** the same state, flag clear and unpositioned after a completed copy, is what
`RDone` followed by `Crash(k)` leaves in the model, and the fixed model passed exhaustively at MaxIds 2 (L_ids2,
5,134,137 states, `spec/tla/RESULTS.md` 34) [FACT-MODEL]. Limits: L_ids2 overrides `Log.cfg`'s MaxIds 3 (whose run
did not complete); it runs today's R2, not R2′; its reseed copies a log, not a checkpoint; `MaxCrashes = 1`; and no
coverage data shows TLC reached that path [FACT-SNC]. So this is argument, not a check.

**The revision (needs the owner's yes, Y6c-2).** Replace R6.5's first sentence with:

> A durable `incomplete` flag is set before `k` discards anything, and cleared only once the copied state is durable
> and published, in the record write that names its new checkpoint. Clearing it does not position `k`: `k` is then
> positioned by R6.2/R6.3 like any member, and until then `k` sends no acknowledgement report, holds and requests no
> lease, and is not admitted to S.

The second and third sentences stay. Texts that cite R6.5 keep their words: R2′ rule 3 and P6 still refer to the flag;
R6.3′ item 1's "not incomplete" now admits the R6.2 round that follows the clear, which is the intended effect.

## 7. D-06: unresolved changes

The note's inventory E1-E25 and its two conflict lists stay intact and on HOLD (Y4). No conditionally accepted D-06
item is replaced here. This package adds:

| # | D-06 item | Accepted or earlier text | Proposed | Source |
|---|---|---|---|---|
| E26 | V8-2 scope (note A.3) | V8-L on "the two record files and each stream's watermark file" | every descriptor that carries an async `O_DIRECT` write is locked at open, the preparer's `spare.tmp` included; recovery passes each segment's and the spare's lock (open, lock, close) before reading it | 2.3 |
| E27 | recovery order (D-06 4.3, boundary rule 4; supersedes the note's E10) | D-06 4.3's six steps; boundary rule 4's seven | the twelve steps of 2.2; today's earlier writes (`make_dirs`, `role`, `seed_fetch`, lease block, links generation, lease and legacy-flag commits) move to phase 2; a read `EIO` refuses by name | 2.2 |
| E28 | 4.1 RESEEDING | per stream, "so that two streams reseeding clear independently" (`docs/AFR_001_D06_STORAGE_METADATA.md` 116) | under a whole-node reseed, set and cleared over every stream in one write | 6.3, 6.5 |
| E29 | seed pins (boundary rule 1 and the seed path) | the checkpoint pin ends with the send; one global WAL deadline | pins held for the transfer, released by `k` or a stated timeout; the NOT READY gate on the primary reconsidered | 6.5 |
| E30 | seed marker (boundary rule 5; the seed path) | a crash before the seed's record write refuses (C1) | a durable `seeding` marker, created and its directory synced before anything else of the seed; step 3 classifies "no valid slot and `seeding`" as SEED, removes only the seed's own files and fetches again; the marker is removed durably in step 8 before the node serves. In the slot-epoch case it covers the seed only once the operator's wipe has finished; V7 stays open. A lost marker create refuses as today; FM-7's scope is not widened (OD-14, design direction) | 3 |
| E31 | links generation creation order (D.2 2.1) | created after the first record write (`engine.cpp:1074`, `:1080`) | created durably before the first record write, on every init and seed whether or not the node is a cluster member, as V8-2 orders watermark files; missing with a valid record is then a fault, or a node started standalone before this rule (its own row, as today's `--links-generation-min`) (C2; OD-14, design direction) | 3 |
| E14′ | boundary rule 5 (seed floor) | the note's E14: "whole-node: unchanged" (an empty directory) | whole-node **in place** follows 6.5; the empty-directory seed stays only for a new node and the slot-epoch mismatch case | 6.3 |

**Corrections to the note's E rows (on site):** E10 points to E27; E11's "only after a fault" is wrong, since an
interrupted take and a floor published before the writer's directory `fsync` both need adoption after crashes alone
(review r2 m7); E14 points to E14′.

**Conflicts with frozen protocol text, complete list:** R9-A, R9-B's meaning, G-30 (A)/(B), R2′ rule 2 and S5's count,
G-11 and D-03 in meaning, R9-C's count, R6.3′ item 1's durable end (all Y5, section 5); R6.5's first sentence (Y6c-2, 6.6).

## 8. Prior art and the witness

Read from primary sources on 2026-11-19 under the standing prior-art rule. **[FACT-EXT]** read in the source at the
cited place; **[INF]** argued here. Nothing is adopted or embedded.

### 8.1 Protocol-Aware Recovery (CTRL), Alagappan et al., USENIX FAST 2018

Source: `https://www.usenix.org/system/files/conference/fast18/fast18-alagappan.pdf`, read through a text extraction;
page numbers are the proceedings pages and may be off by one. The separate proof document it cites was not read.

| | What it says [FACT-EXT] |
|---|---|
| Faults | corrupted data, inaccessible data (EIO, latent sector errors), file-system metadata faults (missing or unopenable files, wrong sizes, read-only or unmountable file system) (section 3.1, Table 2). Lost and misdirected writes appear only as causes of corruption (section 2.1) |
| Not covered | a device that acknowledges a flush and loses it (durability is taken from `fsync`, section 3.3.3); memory errors (p. 29). A lost entry write is caught locally when its separately stored identifier survives; losing the entry and its identifier together is left to distributed recovery, which needs a correct copy elsewhere [INF from sections 3.3.3-3.4] |
| Budget | no per-node cap. The guarantee is conditional: "if there exists at least one correct copy of a committed data item, it will be recovered" (section 3.2). An item and its identifier are assumed not to be faulty together (section 3.3.4) |
| Local layer | CRC32 on every entry, snapshot chunk and the metainfo; an identifier per entry (index, epoch, offset, checksum) kept apart from the entry; the metainfo (term, vote) in two local copies, never recovered from peers; a metadata fault crashes the node (sections 3.3, 4.1) |
| Crash or corruption | write the entry, then its identifier, then `fsync`. A bad entry with no identifier is a crash and is discarded; with an identifier it is corruption. The last entry cannot be told apart locally and is left to distributed recovery (section 3.3.3) |
| Distributed recovery | a node with faulty entries may still be elected and repairs before serving; the leader polls followers (have / dontHave / haveFaulty): a copy repairs, a majority dontHave discards the entry and its successors as uncommitted, haveFaulty waits (sections 3.4-3.5) |
| Evidence | a proof of the crash/corruption rule (separate document); a model of over 2.5 million log states; CTRL added to Raft's TLA+ specification; fault injection, all correct; LogCabin and ZooKeeper, about 1500 lines each, at most 4% overhead on SSD (sections 5-6) |

**For SNC [INF]:**
- CTRL supports Codex's remark that SNC's per-node conclusion follows from its own frozen rules. Its repair of
  committed entries rests on Raft's election restriction and a majority poll, and on some correct copy existing. SNC's
  P5 makes any member of S eligible, the witness compares no LSNs, and with `|S| = 2` there may be no second copy.
  A CTRL-style peer repair would be a protocol change (V6), not a storage change.
- CTRL's local layer resembles SNC's watermark (evidence kept apart from the data), but relies on peers when both
  are lost. SNC's binding and R-CHAIN (lost directory operations, revived segments) have no counterpart in CTRL.
- Parallels: CTRL's two metainfo copies, never repaired from peers, match the node record; crashing on a metadata
  fault matches G-14(a)'s refusal; CTRL's identifier kept apart from the entry matches the watermark kept apart from
  the WAL, and both assume the item and its evidence do not fail together. In SNC that assumption is the one-fault
  budget (section 1), and FM-9 is where it breaks for both.

### 8.2 etcd WAL checksum chain, tag v3.5.17, `server/wal/`

| Mechanism | What it does [FACT-EXT] |
|---|---|
| Running CRC | each record's CRC is CRC32C over all record data so far (`encoder.go:44-47`, `:67-68`); the decoder checks it (`decoder.go:107-116`) and fails with `ErrCRCMismatch` |
| Segment cut | a new file `%016x-%016x.wal` (sequence, first index) starts with a CRC record carrying the previous running CRC, then metadata and state, then rename and directory `fsync` (`wal.go:720-761`). The check is skipped when the stored CRC is 0 (`wal.go:475`), so the first file read is not anchored to what came before |
| Torn tail | a zero length means end of file; in the last file a CRC failure with an all-zero 512-byte sector is a torn tail (`decoder.go:70-78`, `:135-168`); write-mode open zeroes everything after the last valid record (`wal.go:508-523`); `Repair` keeps a `.broken` copy and truncates (`repair.go:30-100`) |
| Snapshot anchor | files are selected from the last one whose first index is at or below the snapshot's, with contiguous sequence numbers; a snapshot record with the same index and another term fails (`util.go:41-70`; `wal.go:481-489`, `:527-530`) |
| Truncation | no suffix is truncated on disk; conflicting entries are overridden in memory (`wal.go:457`) |
| Lost newest records, revived files, lost unlink, file-set rollback | nothing: no forward pointer, no external high-water mark |

**Mechanism by mechanism (Codex's list) [INF]:**

| SNC mechanism | etcd's chain | Reason |
|---|---|---|
| `prev_nonce` (R-CHAIN) | supplements, does not replace | the chain binds content; R-CHAIN binds the instance. A revived segment whose predecessor content is identical (the G-15 equal-entry case) passes a content CRC and fails a random nonce. etcd also leaves its first file unanchored |
| truncation binding `(L + 1, nonce)` | does not address | etcd never truncates a suffix on disk; a lost directory entry of a new file is not detected |
| WAL record verification | supplements | a chained CRC also rejects a valid but misplaced record; SNC's per-frame CRC with the nonce in every frame covers the same case. etcd reads zeros as end of log, which K1's formatted sectors replace |
| compaction-floor anchoring (FL-floor) | parallel | etcd's snapshot (index, term) check is a term-only identity at the floor, like FL-floor; neither catches stale content with the same term (O-FLOOR) |
| acknowledged-tail loss (watermark) | does not address | the chain state and the records live in the same files and vanish together |

**Does a checksum chain detect rollback of data and its evidence together? No [INF].** A chain proves only that the
records present agree with each other in write order. If the last records vanish with the chain state, the prefix
verifies and ends cleanly; if a whole stale file set returns, it is self-consistent. Only an anchor kept elsewhere
catches either: etcd's snapshot, when it is newer than the rolled-back set, or SNC's watermark and manifest.

**Recommendation [INF]:** no change to OD-7, OD-8, OD-9. A chained CRC would add one property SNC already gets from
the nonce in every frame, and would remove none of SNC's mechanisms. Finding 8 is answered; nothing is simplified.

**Not read (UNRESOLVED):** etcd v3.6 `server/storage/wal`; etcd's bootstrap code that decides when `Repair` runs;
whether raft's `HardState` commit index catches a lost WAL tail.

### 8.3 What the witness durably knows (finding 10)

The group record R holds the term, the primary, S, the mode, `m` and `ready`, changed by compare-and-set on its
version through the witnesses' Paxos (`docs/AFR_001_SAFETY_ANALYSIS.md` "Group record R") [FACT-SNC]. P5's design
choice is that "the witness compares no LSNs".

| Local fact a rollback can lose | Does the witness know it? |
|---|---|
| the node's durable term | only the group's current term, not what this node persisted; a node that missed a term persisted less, legitimately |
| a PENDING truncation intent `(t_j, L)` | no |
| RESEEDING | no; only that the node is out of S |
| `active_ckpt` and its manifest | no |
| the incarnation (boot generation) | no |
| the watermark, the binding, any LSN | no (P5) |

(R also has its `version`.) **Finding [INF]:** knowing the current term proves none of WAL integrity, truncation
completion or checkpoint validity. A node's durable term is never above R's term, so the boot read (R7) already
restores a rolled-back term for fencing, but it cannot **detect** the rollback, and it restores nothing else. So
the witness cannot replace the local protection of the four-slot record. Moving one of these facts to the witness would be a
protocol change (a per-node durable field in R, written through Paxos at every intent and checkpoint), with a write
per truncation and checkpoint on the consensus path. Not proposed. The witness does keep its existing role: a
recovered node grants and serves only after its boot read (R7) and, as a standby, R6.2 positioning (state machine step 12).

## 9. Remaining obligations, minimum set

Nothing in this section is authorized. Each item needs the owner's yes.

**Formal verification.**

| # | What | Why it cannot be argued | Scope |
|---|---|---|---|
| M-1 | O-FLOOR: checkpoint publication, floor and reclaim under crashes and one fault | the ordering of publish, delete and the floor check has crash interleavings the argument covers only by cases (boundary 3); O-FLOOR is the condition Codex set before checkpoint conformance | the bounded option already described: reclaim added to `Completion.tla`, two mutations, the R2-D1 bounds, 2-hour cap (`docs/AFR_001_D06_BOUNDARY.md` section 3) |

Not proposed now [INF]: a model of R9-A′ (it only removes reports, section 5) or of R6.5's order (6.6). **The
twelve-step order is not modelled**: the V8-L barriers (steps 2 and 4), the sync and raise (step 9), the reseed
(step 11), step 12 and the order of phase 2 rest on argument; `MetaWatermark.tla:22` treats recovery as terminal, so
no model runs a raise followed by later operation. The decisions package lists every cross-mechanism invariant that
rests on argument only. If Codex wants the composition checked, one model of the state machine is the candidate,
after Y4.

**Experiments** (no product code; not X-2).

| # | What | Design | Decides |
|---|---|---|---|
| E-1 | V8 restart barrier | a throwaway C program on a scratch ext4 file system over `dm-delay` with **writes only** delayed by several seconds (dm only as a test harness, not a supported stack): a preallocated file; a multi-threaded process opens it, `flock(LOCK_EX)`, submits 4 KiB-aligned, non-extending async `O_DIRECT` writes with `RWF_DSYNC` through io_uring, as SNC does (`src/native/runtime.cpp:213`), then is killed (`SIGKILL`, and `abort()` from another thread). A second process takes `flock` blocking. Pass: the lock is granted only after the write's completion, timed from a block-layer tracepoint (`block_rq_complete`), not from a read. Variants: AIO; the directory lock (expected to be released early); xfs. On the target kernel (Ubuntu `6.8.0-149`); an upstream kernel is E-1b (safeguards) | V8-L's premise, beyond source reading (finding 6) |
| E-2 | watermark write cost | a throwaway microbenchmark on the target data SSD (not the boot device, which has `fua=0`): N concurrent streams, each per flush an `O_DIRECT | RWF_DSYNC` WAL write, then a 4 KiB `O_DIRECT | RWF_DSYNC` overwrite of a preallocated file, against the WAL write alone; batch sizes and flush rates as in X-2's prereg; latency percentiles and throughput | the device cost only (finding 13): whether a replicated measurement is needed before more design work. **Never** an HA_DURABLE or end-to-end number, and a pass is not acceptance |
| E-3 | platform facts | read-only: the Ubuntu kernel source package for `6.8.0-149` (the files of note F.2), the device timeouts, the SSD's power-loss protection from its datasheet or mode pages | 2.7's open rows |

E-1 and E-2 run on lab hardware, which means an R640 authorization if run there. Crash-injection tests of HeaderFirst, the temporary files and `EEXIST` (the coordinator's list) need product code,
so they belong to the implementation phase, not here.

**Safeguards before any authorization** (rounds 3 and 4). The text is a plan; a run card written from it must keep
every line:
- **Before and after:** record `lsblk -o NAME,MODEL,SERIAL,UUID,MOUNTPOINTS`, `findmnt`, `df -h`, `losetup -l` and
  `dmsetup ls` before the run and compare after; any difference other than the run's own objects stops the run.
- **Disks are named by model, serial or UUID**, never by `/dev/sdX`, which moved at every reboot
  (`docs/R640_TOPOLOGY.md` 135, 168, 222). The old VMFS HDD and the boot (BOSS) device are never written, and no
  partition is created anywhere.
- **E-1's place:** a scratch file of at most 8 GiB, created with `mktemp` under a directory made for the run on the
  root file system, after a check that at least 16 GiB stay free there; never under `/srv/snc-data`, `/boot` or
  `/boot/efi`. It is attached with `losetup --find --show`, and only the device path printed there is used.
- **E-1's system changes, all reverted at teardown:** `modprobe dm-delay`; one `dm-delay` target with a name chosen
  for the run; `mkfs.ext4` / `mkfs.xfs` only on that target; a mount point under the run's directory; the
  `block_rq_complete` tracepoint enabled, then disabled and the trace buffer cleared. Processes are signalled by
  the PID the test printed, never by name.
- **E-1b, another kernel:** installing and booting an upstream kernel writes `/boot`. It is a separate experiment
  with its own authorization, never implied by E-1.
- **E-2's place:** files in a directory made with `mktemp -d` on `/srv/snc-data`, after `findmnt` confirms that the
  mount's source is the Samsung data SSD by model and serial; at most 4 GiB written, and at least 100 GiB left free
  before it starts. `pgrep -f snc_` must find nothing before each phase, or the run stops. The teardown removes only
  that directory. `O_DIRECT` is an open flag; `RWF_DSYNC` the per-write flag. Never the raw device.
- **E-3 runs an allowlist** of read-only commands (`uname -a`, `cat` of sysfs and procfs files, `lsblk`,
  `smartctl -i`, `sdparm --get`/`--inquiry`, `findmnt`). Forbidden: `sdparm --set`, `hdparm -W`, any sysfs write,
  and any change to `/etc/apt` (the Ubuntu kernel source package is fetched on another machine).
- **Teardown, in order:** unmount; `dmsetup remove <the run's name>`; `losetup -d <the printed path>` (never `-D`,
  which would also detach the system's snap loop devices); disable tracing; remove the run's directory; then the
  "after" check.

## 10. Proposed order for closing D-02, R-23 and O-FLOOR

1. **Owner decisions** Y5, Y6a, Y6b, Y6c-1 and Y6c-2 (section 11), after Codex's review of this package.
2. **E-2** (device cost). If the extra write is large next to the WAL write, a replicated measurement (X-2, or a
   smaller one the owner authorizes) comes before more design work, and Y7 is decided on that, not on E-2.
3. **E-1 and E-3** (V8 and the platform). A failure moves V8 to V8-R.
4. **Y4:** one D-06 revision package (E1-E31 with E14′, written as the consolidated byte-level storage specification), which
   supersedes the distributed texts.
5. **O-FLOOR:** M-1, then its review. A pass discharges O-FLOOR as Codex's condition requires.
6. **R-23:** the one-write case is covered by the four-slot record (FM-8); the file-level rollback stays out of the
   fault model unless the owner widens it. Closing R-23 then means an owner ruling that FM-8's scope is the contract,
   plus the untested tie branch reached in a bounded check if Codex asks (acceptance review B1).
7. **D-02 acceptance review**, against this package, once 1-6 are done.

## 11. Decisions requiring the owner

Each row is a proposal. **[2026-11-19: Y5, Y6a, Y6b, Y6c-1 and Y8 decided as Codex worded them; Y6c-2 conditional;
Y4, Y7 HOLD: `docs/handoff/PR171_Owner_Decisions.md`.]**

| # | Decision asked | Recommendation | What a yes changes |
|---|---|---|---|
| **Y5** | R9-A′ (5.1), with every change of 5.2, and the recovery sync and raise after the segment barrier (2.2 step 9) | **yes**, as a recorded revision of frozen R9-A; argument only. It does not close P5's excluded clause while D-02 is on HOLD | R9-A text; the words of R9-B and G-30 (A)/(B); in meaning R9-C, R9-F and G-30 (F), G-11, the HA_DURABLE definition, D-03, R2′ rule 2 and S5, R6.3′ item 1; write-path rule 4; E18, E20, E25 |
| **Y6a** | V8-1 (replace the newest segment after every restart, except one created during this boot); V8-L on every async `O_DIRECT` file with the lock order of 2.3; V8-3; the supported platform of 2.7 | **yes**; V8-R stays the fallback if a kernel or E-1 breaks V8-L's premise | design directions; E3, E4, E9, E10/E27, E26 |
| **Y6b** | the name spaces (`<16hex>.wal`, `spare.tmp`, `seg.tmp`, `wm.tmp`, `watermark`), HeaderFirst recovery (note B.4), refusal on `EEXIST` | **yes**, as the realization of OD-8 | design directions; E5, E6, E7, E11, E21, E22, E23 |
| **Y6c-1** | whole-node in-place reseed as the initial direction (6.4), with 6.5's amended steps; per stream kept as a later option | **yes** | R6.4's scope; E14′, E15, E16, E28, E29 |
| **Y6c-2** | the R6.5 revision of 6.6: the flag clears at RS-6's publication, and `k` is then positioned by an ordinary R6.2 round | **yes**, as a recorded revision; argument only; I2-I5 must first be re-argued for the "complete, unpositioned" state (6.6) | R6.5's first sentence; through it, the meaning of R2′ rule 3, R6.3′ item 1, P6, R2′'s promotion evidence, and I2, I3, I4 step 6 and I5; a new window in which R7 may promote `k` outside S after the clear and before positioning (G-35) |
| **Y7** | D-02's ambition (finding 15): keep the per-node detector (the watermark, K1, R-CHAIN) as the first-version contract, or accept an industry-level fault model for the first Rust version with the format fields reserved and the detector staged | **decide after a replicated measurement**, not on E-2 alone (10, step 2) | the scope of G-14(b) for the first version |
| **Y8** | the two crash-only refusals (section 3, C1 and C2): correct them (a seed marker; the links generation before the first record write), or record them as G-48 exceptions | **correct them**, as design directions (decisions package) | G-48 stays without a seed exception; E30, E31 |
| Y4 | the D-06 revision inventory E1-E31 and the conflict lists | **HOLD** until Y5 and Y6 are recorded, then one package written as the consolidated storage specification | nothing now |

**Also for the owner, not Y items:**
- whether G-48 lists "disk filled from outside SNC" as an exception (2.5);
- whether FM-8's scope (a one-write rollback is covered, a file-level restore is not) is the contract that closes
  R-23 (10, step 6);
- authorizations: E-1, E-2, E-3 (section 9) and M-1 (O-FLOOR), each separately.

**Still UNRESOLVED after this package:**
- Ubuntu 6.8.0-149's patches; the R640's device timeouts; power-loss protection (2.6, 2.7; E-3);
- V8 beyond source reading (E-1); the watermark cost (E-2);
- every G-48 number (2.6);
- `ENOSPC` with a disk filled from outside (2.5);
- the note's A.6 block-reuse path (argued, not modelled);
- the `links_generation` retry and the read-`EIO` loop (4.1), and today's early writes (2.2), recorded, not fixed;
- lost renames of `links_generation.tmp` and `role.tmp` (outside FM-7);
- two crash-only refusals (section 3, C1 and C2): unresolved violations of G-48, corrections proposed (Y8);
- the buffered fallback after a write error (FM-12); exit semantics of refusals under a supervisor (section 3);
- whether R9-A′ reports carry `(term, lsn)`; I2-I5 for the post-RS-6 state (6.6);
- reseed pins (E29), the lease-block case at RS-6, the liveness of repeated reseeds under term changes or a lost
  shipped tail, a remap on the primary mid-transfer;
- whether io_uring's exit fallback ever drops a file reference before the bio completes (not traced);
- R9-A′, the recovery raise and the R6.5 revision rest on argument only.

This list replaces the open lists of the note (sections 6 and 7) and of `docs/AFR_001_D02_ACCEPTANCE_REVIEW.md` for
the items it names.

## 12. Independent review

### 12.1 Round 1: re-review of the note's corrected reseed (C), Section F, and this package's first draft

Three reviewers, independent, each told to break one part: **r1** the note's C.2-C.3 and the draft's R6.5 and
reseed sections; **r2** the note's Section F (never reviewed before) and the draft's recovery order; **r3** R9-A′,
the decision package and the note's D and 7. r2 fetched Linux v6.8 and v6.12 sources. Every finding was taken up as
below.

| Rev | Severity | Finding | Outcome |
|---|---|---|---|
| r3 | MAJOR | the recovery raise could cover frames a dead process's write had not yet completed or made durable: a refusal after crashes alone, and a counted copy that was never durable | 2.3: V8-L extended to segments (E26); the sync was then dropped as redundant in round 2 and restored in round 3 (2.2 step 9) |
| r3 | MAJOR | R9-A′'s quoted text dropped "p counts no other report", the catching-up clause and the M_ackLeased sentence | 5.1: only the condition changes; every clause kept |
| r3 | MAJOR | Y5 left out G-30 (A)/(B), R2′ rule 2 and S5, "caught up", E18/E20/E25 | 5.2 and Y5 list them |
| r1 | MAJOR | the "no revision" alternative needed one (R6.3′ item 1 excludes incomplete members); the model's `RDone` positions through the transfer | taken up, then reversed in round 2 (i2 M1): 6.6 now proposes the revision |
| r1 | MAJOR | RS-6 lacked `RDone`'s term guard; a term change mid-transfer had no rule; RS-3 did not persist `j`'s term | 6.5: term persisted at RS-3, restart on a newer term, compare on the term at RS-1 and RS-6 |
| r1 | MAJOR | `next_lease` regressed against today's seed | 6.5: RS-3 receives the primary's; RS-6 takes the max |
| r1 | MAJOR | the model evidence was overstated | 6.6 states the limits and labels the claim INF |
| r1 | MAJOR | the comparison omitted per stream's kept copies and parallel positioning, and "V7 does not arise" was false for the epoch-mismatch case | 6.3 rows corrected; E14′ |
| r2 | MAJOR | the R-1 lock order needed the stream set before the manifest was read | 2.2: the record files first, the streams after step 3, without `O_CREAT` |
| r2 | MAJOR | the reseed restarted before the network and the incarnation existed | step 11, after the writers |
| r2 | MAJOR | today's code writes before the record is read (`role`, `seed_fetch`, lease block, links generation, legacy-flag commit) | 2.2, E27: listed, moved to phase 2 |
| r2 | MAJOR | phase 1 missed the PENDING spare, the checkpoint part and the `E + 1` header, so refusals came after writes | 2.4: all in step 5 |
| r1, r2, r3 | MINOR | operator promotion outside S is in the contract (G-35); `k` cannot check A; positioning's volatility rests on a TEXT ONLY clause; pin semantics; D-06 4.1's per-stream clear; RS-6's floor; hot-object loads; xfs truncate fact; `links_generation` retries; ENOSPC beyond replacement; G-48 terms; `wm.tmp` writes; line references; E11; F.3's `L + 1`; F.4's lost watermark write; the no-checkpoint PENDING case; platform scope; R6.2's cut; the promotion claim; Y6c split; E mappings; p's own copy | each fixed in this package or on site in the note |

**Fixed on site in the note:** A.1 line references and the `runtime.cpp` path; A.3's space wording; F.1 marked
superseded; F.2's xfs fact; F.3's `L + 1` case; F.4's lost watermark write; F.5's `links_generation` answer; D.3's
remark on `p`'s own copy labelled INF; E10, E11, E14 pointers; C.2 marked as amended; section 7 points here.

**Checked and found sound by the reviewers:** S and A are node-level; the 4096 slots; RS-1 before RS-2; RS-2's
removal of unpublished checkpoint directories; the memory discard; lost-unlink handling under OD-5 and OD-7;
`AddA`/`Grow` need positioning; R6.5 not on the freeze list by name; the PENDING inequality chain and that a crash
alone cannot give `L < retain_from − 1`; validation before rule 4 keeps design 2's table; deadlock freedom across
processes; `EEXIST` only after a fault; F.2's v6.8 references; `RENAME_NOREPLACE` in v6.8 ext4, xfs and the VFS; no
registered files, no SQPOLL, no `fork`; R9-A′ called a strengthening throughout; Y4 on hold; whole node recommended
and not selected; `m = 2` untouched; `PR169_Codex_Review.md` verbatim with a correct status note.

### 12.2 Round 2: the integrated contract

Three reviewers, independent, each told to break the whole document from one side: **i1** storage and Linux; **i2**
replication protocol and formal methods; **i3** scope against both Codex reviews, consistency and the public-repository
rules. No BLOCKER. Every MAJOR was taken up; the fixes are **not** re-reviewed.

| Rev | Finding | Outcome |
|---|---|---|
| i1 | the preparer's `spare.tmp` descriptor, which the writer writes through, was never locked | 2.3: whoever opens a write-carrying descriptor locks it; recovery passes the spare's lock |
| i1 | holding a lock on every segment exhausts descriptors (`EMFILE`, a crash-only loop) | 2.3: recovery opens, locks and closes; the barrier needs no holding |
| i1, i3 | INIT dropped C13's refusal when data is present; a seed crash refuses after crashes alone, contradicting section 3 | step 3 and section 3's first row; the seed case is stated as an existing exception (11) |
| i1 | refusals after phase-2 writes; repeated boot writes | links generation validated in step 5; phase-2 refusals listed in 2.2 |
| i1 | FM-7 overclaimed (`links_generation.tmp`, `role.tmp`, a lost journal commit) | FM-7 scoped; the two files listed UNRESOLVED |
| i1, i3 | a read `EIO` at recovery crash-loops (FM-5 against FM-12) | FM-5, FM-12, section 3 row, 4.1 |
| i1 | the budget sentence contradicted FM-3 | budget names FM-9 as "more than one" loss |
| i1 | the matrix missed version, catalog, shard-count and links-generation refusals, which are not damage | section 3 rows with their own actions |
| i1 | E-1 could pass for the wrong reason (synchronous fallback, delayed read) | E-1: writes-only delay, `RWF_DSYNC`, aligned non-extending writes, tracepoint timing, a multi-threaded kill |
| i1 | watermark files of streams added by a remap had no creation rule | 2.3: created before the record write that names them |
| i2 | "RS-6 is a positioning" changed frozen positioning text silently, and RS-6 cannot check `j`'s state as `RDone` does | 6.6 rewritten: clear at RS-6, then R6.2; the R6.5 revision is proposed (Y6c-2) |
| i2 | step 12 broke a sole primary's re-promotion and omitted the boot read and S1-S5 | step 12 split for standby and primary |
| i2 | 5.2 omitted G-11, D-03, R9-C, PR155's catching-up argument and the evidence citation | 5.2 rows added |
| i2 | RS-0 left lease reads running while RS-2 empties the map | 6.5: RS-0 drops leases and stops lease reads |
| i3 | the matrix lacked evidence and rejoin columns | the default block states them; rows state exceptions |
| i3 | finding 12 marked closed | partial |
| i3 | the note's A.3 insert contradicted 2.5 | fixed on site |
| i3 | E-2 used to decide option (c) against end-to-end limits | E-2 decides only whether a replicated measurement comes first; Y7 waits for that |
| i3 | step 11 and the matrix assumed whole node before the owner chose | "under Y6c-1; note C.2 otherwise" |
| i3 | model names in the repository | removed |
| i1, i2, i3 | MINOR: `RWF_DSYNC` makes the extra sync redundant (reversed in round 3: it is not, when `wal_sync` was off or writes were buffered); the stream list with no manifest; read-only descriptors; RESEEDING streams in steps 10-11; `L` with no checkpoint; citations (`:1243-1246`, `:1222-1227`, `make_dirs`); 4.1 overstated; E-2 setup; LOCAL_DURABLE sole copies; FM-14 premise; `next_lease + 1` and the lease block; RS-3's term write; model citations (`Log.cfg` MaxIds 3; no outside-S promotion); P5 "in substance"; FM-3 lowering `W`; 8.3 step and R7; 8.3's "replace"; CTRL fairness; stale text in the note; cross-references; one UNRESOLVED list | fixed here or on site |

**UNRESOLVED from round 2** (also in 11): whether "positioned" in R6.5 was meant as `k`'s state or `j`'s record (the
revision makes it moot); io_uring's exit fallback; whether `fdatasync` issues a flush when the journal is already
committed (moot under `RWF_DSYNC`); etcd and FAST 2018 line and page citations were not re-fetched by the reviewers;
the FACT-MODEL claims were not re-run.

### 12.3 Round 3: focused re-review of the corrected text

Recorded in the decisions package, section 6.
