# AFR-001: crash-consistent design for U3, U10, U11 and U12 (design, PROPOSED)

Status: **design only, for Codex and owner review.** Asked for by Codex's review of #161
(`docs/handoff/PR161_Codex_Review.md`, item 3): "a focused design of U3, U10, U11, and U12". It builds on candidate (c)
of `docs/AFR_001_STORAGE_COMPARISON.md`: K1 frames plus a K2 watermark.

What it does not do:
- select D-02: (c) remains the leading candidate, not the selected mechanism;
- declare R-23 resolved;
- discharge O-FLOOR;
- authorize X-2, a model campaign, C++, Rust, R640 or migration work.

Every change it proposes to conditionally accepted D-06 items, boundary rules or the X-2 prereg is listed in section
7 as a proposed revision.

Labels:
- [INF]: an argument made here, not checked by a model;
- [HYP]: an untested guess;
- [FACT-SNC]: read in this repository;
- [SRC]: read in a primary source (kernel source, man pages, OpenZFS), as cited.

## 0. Fault model and requirements

**Fault model.** Any number of crashes (power loss at any instant), plus at most **one** media fault on the node: a
lost, torn, zeroed or misdirected write, of data or of directory metadata. `docs/SNC_GUARANTEES.md` names "lost or
zeroed sector, misdirected write". Including whole lost writes and directory metadata follows X-2's cases C12-C14 and
the comparison. This is an extension, labelled as such.

**Platform premises:**
- ext4 (`data=ordered`, barriers on) or xfs;
- metadata files preallocated and overwritten in place;
- no copy-on-write file system: btrfs and reflinked extents are not assumed.

**Requirements:**
1. **No silent loss.** Every acknowledged write, and every node-record state the node acted on, is recovered, or
   recovery refuses (G-14).
2. **No refusal from crashes alone.** A crash with no media fault always recovers without an operator (G-48). A
   refusal is allowed only when a media fault occurred: the G-14(a) exception to G-48.
3. **One authority.** The owner's condition, quoted: "The durable term, the term history, every stream's truncation
   intents and the incomplete flag form one logical, atomic metadata authority per node"
   (`docs/handoff/PR155_Owner_Decision.md`). This design adds a watermark outside the record. The claim that the
   watermark is evidence and not part of that authority is [INF], and is open question V2.

## 1. On the peer argument (Codex's distinction)

The comparison concluded that peers cannot repair a silently damaged promoted replica. Codex's review requires this
distinction: **the conclusion follows from SNC's currently frozen promotion and reconciliation rules, not from an
inherent limitation of replicated systems.**
- P5 makes any member of `S` eligible, together with P1-P7, and "the witness compares no LSNs". R6.3′ then reconciles
  the other members toward the promoted one.
- Under a rule like Raft's election restriction, a leader's log must be at least as up to date as every log in the
  majority that elected it. Such a rule could let peers repair HA_DURABLE writes when `|S| ≥ 3` [INF]. That depends on
  the damage making the member look shorter, and on the witnesses comparing LSNs, which reverses a P5 design choice.

**What a protocol change would and would not do [INF].** It would not remove the need for a local detector:
- G-14(b) is per node: "recovery restores every write acknowledged at LOCAL_DURABLE or above on that node, or
  refuses";
- with `|S| = 2` and the primary lost, no peer holds a second copy.

So the owner's choice concerns the detector's role, alone or backed by peer repair. It is not a choice between a
detector and no detector. This document designs the detector and leaves that choice open (V6).

## 2. U3: metadata rollback protection for the node record

**The problem.**
- D-06's A/B record falls back silently to the older copy when the newer write is lost (R-23).
- The comparison's K5 (write A then B every time) also fails: a lost B write, then a crash that tears the next A
  write, leaves only the older B.
- The cause is the same in both: a write that overwrites the only good copy of the newest generation.

**Invariant** (restated after the re-review, `docs/AFR_001_D02_FOLLOWUP.md` section 2). The newest acted-on
generation always has a valid copy outside the slots that the next write targets. While no media fault has occurred,
it has two.

**Design: two mirrored pairs in two files.** The files `superblock.a` and `superblock.b` each hold two 8 KiB slots,
at offsets 0 and `M`, and each file is preallocated to `2M`. Each slot's CRC covers all 8 KiB: generation,
`space_uuid`, padding, everything.

**Slot spacing.** `M` is at least 8 KiB plus the largest single write the node issues (WAL flush, checkpoint part,
segment format). Nothing ever writes the space between the slots. This rule exists because of a finding in the
re-review: with adjacent slots, one large misdirected or zeroing write can cover three slots in a row, and three
adjacent slots always contain a whole pair. With the gap, one such write touches at most one slot, however the two
files are placed on the device [INF]. The fault model's "one misdirected write" is then any write that the node
itself issues.

1. **Write generation `g`.**
   - It goes to slot `g mod 2` in both files.
   - Both writes use `O_DIRECT` and are submitted together (io_uring or AIO). Then both files are `fdatasync`ed.
   - The node acts on `g` only after both syncs return.
   - Unchanged from D-06 4.1: `meta_mu`, the compare-and-write, the publish rules, and the rule that a failure is
     fatal. **Added:** after a failed write, the writer reaps every write still in flight before the process exits.
     Otherwise a write from the dead process could land after the next boot's write of the same generation, and
     produce a tie with no media fault (re-review B3). A process killed from outside is UNRESOLVED (V8).
   - In normal operation the invariant holds automatically: `g − 1` was acted on, so both copies of it are durable.
2. **Init and seed.**
   - Create both files at `2M`, and `fsync` each file after sizing it. On xfs, a directory fsync does not persist a
     new file's size.
   - `fsync` the directory.
   - Write generation 1 to slot 1 in both files, then generation 2 to slot 0 in both files, each synced.
   - All four slots then hold a real record, so later writes are pure overwrites.
3. **Read.**
   - Read all four slots with `O_DIRECT`.
   - **No CRC-valid slot in either file: init is incomplete**, and init restarts (a seed follows boundary rule 5). A
     missing or wrong-size file is not damage in this case, so a crash during init does not refuse.
   - Once any valid slot exists, both files were durably created, because generation 1 is written only after the
     fsyncs. From then on, a missing file or a file of the wrong size is damage: **refuse**.
   - Take the highest generation among the CRC-valid slots. The slot position is not trusted, because a CRC-valid
     copy found in the wrong slot still counts by its generation.
   - Two valid slots with the same generation and different bytes: **refuse**. Only damage can produce that state
     under this fault model [INF].
4. **Recovery restores the invariant before acting.**
   - If the chosen generation `g` has fewer than two valid copies, recovery writes `g` again, with the same bytes,
     to slot `g mod 2` of each file whose slot `g mod 2` does not hold a valid `g`, and syncs it. The tie rule allows
     the duplicate.
   - Rule 4 and the boot write run under `meta_mu`. No other writer in D-06 4.1's table runs until both are done.
   - Only then does recovery write anything newer (the boot write, D-06 4.1), and only then does it act, for example
     by running a PENDING completion.
   - The boot write moves to the start of recovery, before completion. Recovery's first action is therefore always a
     two-copy write.

**Why it holds [INF].** Let `G` be the newest generation that was acted on. Both of its copies were synced when it
was acted on.

| Situation | Result |
|---|---|
| Crash during the write of `G + 1` (both copies in flight) | `G`'s pair is untouched. Recovery picks `G`, or a `G + 1` copy that completed. If `G + 1` has one copy, rule 4 duplicates it into its own pair before anything else. That write overwrites `G − 1`, not `G` |
| The same, then a crash during rule 4's duplicate write | still `G`, or `G + 1` in one copy. Rule 4 repeats |
| One copy of `G` lost, torn, zeroed or misdirected | the other copy of `G` |
| One copy of `G` lost, then a crash during `G + 1` | `G + 1` touches only the other pair: `G` |
| One copy of `G` lost, `G + 1` completed, then a crash during `G + 2` | `G + 1` has two copies and the fault is already spent: `G + 1` |
| Another write, such as WAL data, misdirected onto a slot | with the slot spacing, it touches at most one slot, so at most one copy of any generation is destroyed. The lost WAL write is D-02's case (section 4) |
| Rule 4 skipped (the review's counterexample) | a crash leaves `G + 1` in one copy. A boot write `G + 2` into `G`'s pair is torn by a second crash, and one fault then destroys the lone `G + 1`, leaving `G − 1`. **Rule 4 exists to prevent exactly this** |

**What it does not cover:**
- two media faults;
- whole-store rollback (a VM snapshot or a restored disk). That remains out of scope (comparison K7, U8).

**Two files rather than one [INF, SRC partly].**
- Against data-block faults, two files and four slots in one file are equivalent.
- Two files add protection against a lost update to one file's extent metadata. That only holds while the slots are
  never reallocated.
- The two inodes may share an inode-table block, and the two names share a directory block, so the files are not
  fully independent. A fault there leaves a missing or wrong-size file, and rule 3 refuses: detected, not silent.

**Prior art [SRC].**
- The closest match is OpenZFS's vdev label sync (`module/zfs/vdev_label.c`):
  - it writes the even labels, flushes, writes the uberblocks, then writes the odd labels, so an untouched pair
    stays valid at every crash point;
  - on the next open it rewrites all labels "before any user data is modified", which is the role of rule 4 here.
- ZFS differs in two ways. It writes the halves one after the other with a flush in between, not together. It picks
  the generation from a ring of uberblocks indexed by txg.
- LMDB toggles two meta pages by `txnid & 1`, which is D-06's A/B scheme with no mirror.
- btrfs rewrites every superblock mirror on each commit, which is like K5.
- No system we verified uses exactly this scheme.

**Cost [HYP, unmeasured].** Each record write becomes two 8 KiB `O_DIRECT` writes and two `fdatasync`s. Submitted
together, they cost about one device write plus two cache flushes. With buffered I/O instead, writeback order between
the files is unspecified and the cost is about two flushes [SRC: the reviewer's reading of fsync(2)]. Record writes
are rare (D-06 4.1), but R4's write is on the promotion path (O8, X-1).

## 3. U10: frames versus truncation

**Under K1:**
- Every segment is formatted: each 512-byte sector is a format sector carrying the segment's nonce
  (`docs/X2_PREREG.md` section 4).
- Each flush is a frame with a header and a trailer.
- Each record keeps its own CRC.
- **Added:** the segment header carries `space_uuid` and the stream index, so a segment that ends up in another
  stream's directory does not validate.

**(a) Every K1 segment is created whole.** This covers spare `prepare()`, `open_segment`, completion step 2.4 and the
reopen. Each is built as `TMP`:
1. header plus every sector formatted;
2. `fdatasync`;
3. `rename` to its name;
4. `fsync` the directory;
5. only then the first append.

Why [INF, SRC]:
- R0 treats a zeroed header in a K1 segment as damage. Without this rule, a crash inside create-then-write-header
  would make a crash alone refuse, which breaks requirement 2.
- On ext4 and xfs, journal ordering makes the name refer, after a crash, to either the old file or the new one. That
  is a file-system property, not a POSIX guarantee. rename(2) promises atomicity only to other processes, and
  fsync(2) requires the directory fsync for the entry to be durable.
- Correctness depends on `fdatasync(TMP)`, then `rename`, then `fsync(dir)`, never on ext4's `auto_da_alloc`
  heuristic.

**(b) The reopen (item 9).**
- A segment at the writer's name that holds only its valid header and format sectors of its own nonce is opened as
  is.
- Any other segment at that name, such as one with a CRC-valid frame left by a torn flush, is replaced by (a).
- Why [INF]:
  - D-06's truncation in place would leave zeros that R0 refuses.
  - `rename` never leaves the name missing, which preserves item 9's purpose: no window in which a void tail revives.
  - Completion.tla models the reopen as one atomic step (the M1 and M8 mutations). `rename` matches that abstraction
    more closely than truncation in place did.

**(c) Residue after a crash.** After every recovery, the writer opens segment `E + 1` by (a), at a new name. A torn
frame left after `E` in the old segment is then void under (d). This replaces R0 rule 4's end marker.

**(d) Void records under the void rule (item 8).** For a segment `S` followed by the segment named `next`:
- Scan `S` up to and including record `next − 1`.
- Anything after that record in `S` is void and is **not judged**: frames, torn frames, format sectors, damage. This
  applies in the scan and in `read_stream_range`.
- If `S` ends, or is damaged, before record `next − 1`: that is a gap, **refuse**.
- A frame that straddles `next − 1` was complete when written. Otherwise R0 would have ended the log below it, and
  the name `next` would not exist. The frame is accepted on its header (nonce, term) and on each record's CRC up to
  `next − 1`.
- Why [INF]: void bytes are not acknowledged state. Bytes up to `next − 1` are judged as in R0.
- This is a change to R0 (section 7).

**(e) A half-done completion.** While the stream is PENDING, recovery runs completion before any scan, and completion
acts by segment name. So frames never have to tell a half-done completion from damage. After the clear, the
remaining question is whether completion's effects survive one fault. Two cases:

- **The directory entry for `L + 1` is lost**, at any time after the clear, until the next truncation.
  - Without a check, `S` is scanned to its old end, the void tail revives, and a watermark at `L` still passes.
    **That would be silent.**
  - **Rule (OD-2, `docs/handoff/PR163_Owner_Decisions.md`):** the watermark lowered by step 2.7 (section 4)
    records the binding `(L + 1, nonce of L + 1)`. **Every later watermark write carries the same binding until the
    next truncation or reseed replaces it.** Recovery refuses unless a segment named `L + 1` with that nonce exists.
  - The check produces no refusal from a crash alone, because step 2.4 fsyncs the directory before 2.7.
  - The earlier text said that once appends move the watermark above `L`, the next case covers it. That was wrong.
    The revision-2 review of `MetaWatermark.tla` found the case: `S` holds two or more void records, one append, and
    a crash. The watermark at `L + 1` then matches `S`'s old record if the entry is identical, and the rest of the
    void tail returns unrefused (`docs/AFR_001_D02_MODEL.md` section 11).
  - **Below the floor.** If reclaim has removed segment `L + 1`, the check is skipped. Reclaim removes segments from
    the front, so `S`, which holds the void tail, went first. This holds on two conditions:
    - the floor is published durably before any delete;
    - "below" means that `L + 1` is less than the floor segment's name.
    [INF; part of O-FLOOR, not modelled]
  - **After a restart**, the writer loads the binding from the newest valid slot when it opens the stream and
    carries it forward [not modelled].
  - **Open (revision-3 review, finding 1):** a lost unlink can bring back the same void entries with the binding
    intact. The proposed rule is to refuse any segment named above `L` whose nonce is older than the binding's. It
    is not modelled, and it awaits the owner (`docs/AFR_001_D02_MODEL.md` section 14).
- **The unlink of a segment `M > L` is lost**, so the old segment `M` reappears.
  - If the new log reaches past `M`, recovery refuses: the watermark falls above the recovered end, or the record at
    the watermark has another CRC, or FL-chain fails.
  - If the new log ends below `M − 1`: a gap, **refuse**.
  - If the new log ends exactly at `M − 1`: FL-chain compares `M`'s `prev_term` with the term of the new record
    `M − 1`. It **refuses** when the terms differ. They can be equal, after later failovers, and then the revived
    records are accepted (V4 below).
  - **V4 result** (`docs/AFR_001_D02_FOLLOWUP.md` section 1). The terms at `M − 1` can be equal after later
    failovers, but only when the entries are the identical entry (G-15). So FL-chain is not what makes this case
    safe. What makes it safe [INF]:
    - every acknowledged record is at or below the watermark `W`;
    - when FL-chain passes, `W ≤ M − 1`, so the revived records are above `W` and unacknowledged;
    - by G-15, each revived record is an entry that really existed under its `(term, LSN)`;
    - the member re-reconciles on rejoin (R6.2), which handles an unacknowledged tail.
    The argument does not need "`L` is exactly the divergence point", which is not always true (section 1 there).

## 4. U11: the watermark, and lowering it on a legitimate truncation

**Format.** Each stream has a file of its own, as in X-2:
- two 4 KiB slots, alternating by generation;
- each slot holds generation, `space_uuid`, stream index, LSN `W`, **record `W`'s own term** (not the writer's
  current term), the CRC of record `W`, the truncation binding `(L + 1, nonce)` from the last truncation or reseed, if any (3(e)), and
  the slot CRC.

**Writing the slots [SRC].**
- At creation, every slot is written and the file and directory are fsynced. Later writes use
  `O_DIRECT | RWF_DSYNC` as pure overwrites, which iomap completes with FUA.
- A first write into unwritten (`fallocate`d) space would also convert the extent and sync. Durability does not rely
  on that path.

**Initial value.** A new stream's slots hold `(start_lsn − 1, start_term)`, which is `(0, 0)` for a fresh space. X-2's
"the stream's start LSN" would refuse a new stream that crashed before its first record. That is a refusal from a
crash alone, so it is changed here.

**Where it is read.** Recovery reads the watermark only for a stream whose record slot is NONE, after the floor check
(FL) and the scan.

**Order on the write path:**
1. watermark(k) is submitted only after data(k) has completed;
2. one watermark write is in flight per stream, submitted by one serial watermark writer, and later flushes coalesce
   into the next write;
3. the acknowledgement of k waits for watermark(k);
4. **anything that reports a durable end outside the node waits for its watermark.** This covers a standby's
   acknowledgement to the primary, the `end_lsn` in R6.2, positioning, and SYNC_READINESS's durable LSN for READY and
   admission to `S`. Those reports are what I4 and P5 rely on.
5. **at most one data flush is in flight per stream** (OD-1). The code already does this (`src/core/storage.cpp:643`,
   `!inflight_`). With two in flight, a crash can land the later one without the earlier one, and the rule "above
   the watermark" below then refuses after a crash alone (`docs/AFR_001_D02_MODEL.md`, lane W0P).

**Rule for a truncation: lower the watermark inside the intent window.**
- Completion step 2.1 also drains the stream's watermark writer and drops coalesced writes it has not submitted.
- A new step 2.7 runs after the reopen (2.6) and before the clear (3):
  - read record `L` with `O_DIRECT`;
  - check its CRC and term;
  - write `(L, term(L), crc(L), nonce of L + 1)` through the same serial writer. If `L` is below the floor, write
    `(L, floor_term, nonce of L + 1)` (section 5).
  - The read checks what is on the device behind the page cache. It does not prove durability past a volatile device
    cache. That rests on the premise that the flush which held record `L` completed with DSYNC.
- If record `L` cannot be read and validated: **refuse**. An acknowledged record at or below `L` is missing.
- **A reseed** follows the same rule while the stream is RESEEDING. Before the clear, it writes
  `(start_lsn − 1, start_term, nonce of the seed's first segment)`. A seed never copies the primary's watermark file.

**Windows [INF]:**

| Window | Recovery sees | Result |
|---|---|---|
| A crash before 2.7 completes, or after it but before the clear | PENDING | completion reruns (idempotent) and lowers again. The watermark is not read while PENDING |
| An in-flight or coalesced watermark naming k > L | drained in 2.1, before 2.7 | it cannot overtake the lowering |
| A crash after the clear | NONE, with the watermark at or below `L`, or newer | the normal check, no refusal |
| The lowering write is lost (a fault), then the clear, then a crash before the next watermark | NONE, with the watermark above `L` | **refuse**, which is allowed after a fault. After appends resume, the next generation overwrites the stale slot |
| The clear is lost (a fault) | its twin copy survives (section 2) | NONE, as above |
| Records at or below `L` were lost earlier (a fault) | 2.7 cannot validate record `L` | **refuse** |

A crash alone never refuses. A lost lowering write still refuses a log that holds all its data. Lowering inside the
window makes that refusal one that a fault caused, which the rules allow; it does not remove it.

The comparison's idea of a term in the watermark, used to detect a rolled-back clear, is not needed, because section 2
removes rolled-back clears under this fault model. KISS.

**Above the watermark, under (c).** R0 rule 5's "keep the valid prefix" above `W` would let a misdirected watermark
write that lands on data (C16b) pass silently. Under (c), only a torn frame, or format sectors of this segment's
nonce, may be discarded above `W`. Anything else **refuses**. This is a change to R0 (section 7).

## 5. U12: the watermark versus the compaction floor

A watermark can name a record that is no longer retained:
- reclaim removed a segment boundary that the watermark lagged behind;
- a truncation to `L = retain_from − 1` (boundary rule 7);
- a seed.

**Check (stream NONE, after FL and the scan; recovered end `E`):**

| Watermark `W` | Check |
|---|---|
| `W > E` | **refuse**: an acknowledged tail is missing |
| `W ≥ retain_from` | record `W` must be retained and non-void, with the slot's CRC and term; otherwise **refuse** |
| `W = retain_from − 1` | the slot's term must equal the manifest's `floor_term`; otherwise **refuse** |
| `W < retain_from − 1` | **accept** |
| the slot carries a binding `(B, nonce)` | a segment named `B` with that nonce must exist, unless `B` is below the floor segment; otherwise **refuse** (3(e)) |

**Why [INF]:**
- FL-floor requires the segment `retain_from` to exist, so `E ≥ retain_from − 1`.
- Every acknowledged record is at or below `W` (rule 3 of the write path). When `W < retain_from − 1`, all of them are
  covered by the checkpoint.
- At the floor, FL-floor's `prev_term` and `floor_term` stand in for the missing CRC. That is a weaker identity
  check: a term, not a CRC.
- Reclaim and checkpoint never write the watermark, so this adds no ordering to O-FLOOR.

**Not covered:** stale content at exactly the floor LSN with the same term. That rests on FL-floor and the floor
segment's nonce, and neither is model-checked (O-FLOOR stays open).

## 6. The frozen protocol

- **R2′ (P1-P7, S1-S5, R6.3′), R9-A to R9-E and `m = 2`:** no text is changed.
  - Rule 4 of the write path makes the durable ends that I4, P5 and SYNC_READINESS read mean "watermarked". The
    frozen text already requires that every such report be durable, so this reads the frozen text; it does not
    change it [INF].
  - P5's silent-loss clause and 4.2 stay excluded from the freeze (O4, comparison section 7).
  - R9-F stays out.
- **G-14:** not weakened.
- **G-48:** no refusal from a crash alone (requirement 2). Every refusal above follows a fault.

## 7. Proposed revisions (none accepted)

Each becomes a recorded revision under `docs/handoff/PR160_Owner_Decision.md`, or a revision of the X-2 prereg, once
Codex and the owner rule on it.

| Where | Change | Section |
|---|---|---|
| Item 4 / 10 (record and segment header) | K1 frame fields; segment header gains nonce, `space_uuid` and stream index | 3 |
| Item 5 (record) | four slots in two files, mirrored pairs, `O_DIRECT`, a tie rule, refusal of a missing or wrong-size file; the "exact copy size" reading of item 12 becomes "exact file size" | 2 |
| Item 6 (write rule) | `meta_mu` is held across two writes. A per-write directory fsync is no longer needed, because the files are preallocated at init | 2 |
| Item 7 (incarnation) | the boot write moves to the start of recovery, after rule 4's duplicate | 2 |
| Item 8 (completion, void rule) | 2.1 drains the watermark writer; 2.4 creates a whole formatted segment; new step 2.7 with its refusal; void bytes not judged | 3, 4 |
| Item 9 (reopen) | open as is, or replace by `TMP` + `rename` + directory `fsync` | 3 |
| D-06 section 8 note | the watermark lives outside the record, is read only for NONE streams, and is lowered in 2.7. Owner decision OD-4 conditionally accepts it as a durability validator only: any disagreement refuses | 4 |
| Write path (new) | at most one data flush in flight per stream (owner decision OD-1) | 4 |
| Watermark slot (new) | the `(L + 1, nonce)` binding is carried in every write until the next truncation (owner decision OD-2, direction only; revision 3 of the model checks it) | 3(e), 4, 5 |
| Boundary rule 4 (recovery order) | rule 4 of section 2 and the boot write first; the watermark check after FL and the scan | 2, 5 |
| Boundary rules 4.2 and 5 (orphans, seed) | an orphan stream's watermark file is removed with its directory; a seed writes its own watermark before the clear and never copies the primary's | 4 |
| R0 (`docs/X2_PREREG.md` 8.1) | the void rule for void bytes (3(d)); rule 4's marker replaced by opening `E + 1` (3(c)); rule 5 under (c) above `W`; the below-floor rows; slot identity and initial value | 3, 4, 5 |

## 8. Still open

| # | Question | Needed |
|---|---|---|
| V1 | Every argument here is [INF]. They survived one round of attack (section 9), which is not proof | Codex's judgment; a small model only if Codex asks |
| V2 | Is a watermark outside the record, read only for NONE streams, compatible with the owner's authority condition? | Codex and owner |
| V3 | The cost of four slots, of the watermark and its gating of reported ends, and of whole-segment creation | X-1 / X-2 (not authorized) |
| V4 | Can R6.2 truncate below the exact divergence point? | **answered** in `docs/AFR_001_D02_FOLLOWUP.md` section 1. Yes, by design after a term change; 3(e) was reworded so that it does not depend on it |
| V5 | O-FLOOR | unchanged: the bounded reclaim model, or an equivalent argument, before checkpoint or reclaim conformance work |
| V6 | D-02 selection, and the detector's role alongside a possible change to P5 or R6.3′ (section 1) | owner decision after X-2 |
| V7 | Carried over from review, not addressed here: whether incarnations stay unique across a wipe and reseed (generations restart at 1); the seed crash before its record write, which needs an operator (boundary rule 5, as today); standalone LSN-reservation gaps against R0 rule 6; carrying watermark files over a remap | design follow-up |
| V8 | Can a write still in flight from a process killed from outside land after the next boot's write? (Whether the kernel drains io_uring or AIO writes on exit was not settled) | kernel source check, or a process-lifetime lock |

## 9. Review record

Three independent reviewers (Opus), each told to break the work:
- **Adversarial:** attack sequences against sections 2-5.
- **Architecture and compatibility:** against the frozen text, the D-06 items, R0 and the boundary rules.
- **Storage mechanics and prior art:** kernel source and man pages via the GitHub mirrors, and OpenZFS.

| Reviewer | Severity | Finding | Outcome |
|---|---|---|---|
| Mechanics, architecture | BLOCKER | the first draft's recovery boot write could overwrite `G`'s pair while `G + 1` had one copy: crashes plus one fault rolled back past `G` | fixed: the invariant and rule 4 (duplicate before advancing); a row added to the table |
| Adversarial, architecture | MAJOR | a lost directory entry for `L + 1` revived the void tail silently | fixed: the segment nonce in the lowered watermark (4(e), section 5) |
| Adversarial | MAJOR | a pending coalesced watermark could overtake the lowering and refuse after a crash alone | fixed: 2.1 drains the writer, and 2.7 goes through it |
| Architecture | MAJOR | section 1 presented a protocol change as an alternative to the detector | rewritten: the choice concerns the detector's role |
| Architecture | MAJOR | section 6 was incomplete | section 7 now lists every change, including R0 |
| Architecture | MAJOR | a zero-byte K1 segment from create-then-write refused after a crash alone | fixed: every K1 segment is created whole (3(a)) |
| Architecture | MAJOR | durable ends reported to peers or witnesses were not gated by the watermark | fixed: rule 4 of the write path; section 6 |
| Mechanics | MAJOR | `rename` atomicity after a crash was overstated | reworded as a file-system property, with the premises |
| Mechanics | MAJOR | "read from the media" had no realization | `O_DIRECT` reads, with the DSYNC premise |
| All three | MINOR | initial watermark value; slot term = record's term; C16b above `W`; the log-matching premise (G-15, not Raft; exact divergence); FL-chain in case 1; segment identity; missing-file and init rules; copy-on-write; latency with buffered I/O; two files not independent; requirement 3 quoted exactly; labels; prior art; the comparison edit's wording | fixed |

The adversarial reviewer found no break in section 2's write and read rules, or in section 5. **The fixes above,
rule 4 among them, have not been re-reviewed.** The UNRESOLVED points raised are V4 and V7.
