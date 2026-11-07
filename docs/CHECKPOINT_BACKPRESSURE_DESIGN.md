# Checkpoint and backpressure architecture: design (revision 2, for review)

*Status: **DESIGN-R, PROPOSED.** Revision 1 (PR #67) was requested by
`docs/handoff/PR66_Checkpoint_Design_Review.txt`. It was accepted as
design direction with required corrections in
`docs/handoff/PR67_Design_Review.txt`. This revision makes those
corrections.*

*Authorized after this revision is reviewed: **S0 (observability) only.**
No S1-S5 engine work. No R640 load, no CP-1A rerun, no competitors, no
Profile C. `kEmitBacklog` stays 1,024. `r640-diag-telemetry` stays off
main.*

Labels:

| Label | Meaning |
|---|---|
| **AS-BUILT** | read from code on main (`e41e7f6`), with `file:line` |
| **MEASURED** | from the R640 evidence (`r640-diag-telemetry`: diag2 `DIAGNOSIS.md`; diag3 `run/` c88cb70, `CAUSAL_REPORT.md` c9537dd) |
| **INFERRED** | reasoned, not measured |
| **PROPOSED** | this design |
| **HYPOTHESIS** | a number or threshold that S0 must measure before anything is frozen |
| **DECIDED** | decided by the PR #67 review |

## 0. Summary

The checkpoint causes two measured bottlenecks:
1. **The walk** takes shard CPU in fixed slices that ignore load.
2. **The storage burst.** Checkpoint parts are written through the page
   cache, leaving up to about 1 GB dirty across 8 shards (INFERRED). All
   8 shards then force it out with `fdatasync` within about 1 ms of each
   other, on the WAL's device. WAL flushes slow from about 0.05 ms to
   about 12 ms. Durable throughput collapses. Follow-ups pile up in
   `emit_wait_`, and the per-shard gate then refuses **every** client op,
   including reads.

The approved direction (DECIDED) keeps the snapshot and recovery
semantics exactly as they are. It changes only *when* and *how fast*
checkpoint work happens, and *which limit refuses which op*:

- **A, admission by resource.** The follow-up backlog no longer refuses
  durable reads. Each op class is gated by the resources it actually
  consumes. Mutations keep the follow-up gate (`kEmitBacklog` = 1,024).
- **B1, paced walk.** The walk gets a per-iteration **time** budget that
  shrinks under pressure. The one-step ID snapshot stays.
- **C, bounded checkpoint writeback.** Checkpoint bytes reach the device
  while the walk runs, with a bounded amount in flight. **The mechanism
  is not chosen yet** (C1, C2 or C3, section 3C); S3 starts with that
  choice.
- **D, staggered final sync.** A node-level token admits K shards at a
  time to the final sync. It lands only with or after C.
- **E, I/O priority.** Optional, off by default; the design must succeed
  without it.
- **B2** (incremental `begin` snapshot) after the CP-1A rerun. **F**
  (incremental or COW checkpoints) deferred.

### 0.1 Correction to revision 1: the WAL *is* `O_DIRECT` by default

Revision 1 said `snc_server`'s WAL is **not** `O_DIRECT`. **That was
wrong.** I read the field `ShardStats::wal_direct = false`
(`include/snc/snc.h:202`), which is a STATS default, as the configuration
default. The configuration is `EngineConfig::wal_direct = true`
(`include/snc/snc.h:309`). `snc_server` does not override it
(`tools/snc_server.cpp` has no `wal_direct`). The engine passes it to the
WAL (`engine.cpp:645-646`).

AS-BUILT: each WAL segment is opened with `O_DIRECT` and falls back to
buffered I/O only if the filesystem refuses with `EINVAL`
(`storage.cpp:539-546`; spares the same, `storage.cpp:750-755`). Writes
carry `RWF_DSYNC` (`runtime.cpp:185`). So on ext4 or XFS the WAL is
`O_DIRECT` + `RWF_DSYNC`, and the diag2 report's "O_DIRECT" was probably
right.

The R640 evidence does not record which path the run took: no STATS
dump in `bench/results` on that branch carries `wal_direct`. S0 makes
it visible (section 11).

Consequences for this design:
- **C2 is not a new technique for this codebase.** The WAL already does
  aligned `O_DIRECT` writes with a buffered fallback. C2 would reuse that
  pattern for parts. It still needs its own proof (section 5.3).
- **E is less speculative.** The WAL's writes are direct, so the
  submitter's I/O priority applies to them. It stays optional.
- **The interference mechanism is unchanged.** The WAL's small
  `O_DIRECT|RWF_DSYNC` writes queue behind the checkpoint's writeback
  burst and its `fdatasync` cache flushes on one device.

### 0.2 How the review was applied

| Review § | Required | Where |
|---|---|---|
| 1 | I1-I5 become test and doc anchors | section 1.3: **checkpoint correctness contract CC-1..CC-5**, with tests per clause |
| 2 | direction A + B1 + C + D, E optional, B2 later, F deferred | sections 0, 4 |
| 3 | C2 is a candidate only; compare C1/C2/C3 on 8 criteria | section 3C, slice S3 |
| 4 | numeric budgets are hypotheses; keep E_OVERLOADED; `refused_by` in STATS | sections 6, 6.4 |
| 5 | WAL budget reserves before mutation; trace the path | section 6.2 |
| 6 | resource matrix incl. output, waiters, forwarded reads, memory bandwidth; explicit scheduling priority; time-based read quantum | sections 6.1, 6.3 |
| 7 | B1 time budget; no frozen 20/200 µs; catch-up observable | sections 3B, 7 |
| 8 | B2 after CP-1A unless S6 shows it is needed; no mark bits now | sections 3B, 10 |
| 9 | D sound; K=1 proposed; sweep K; not before bounded writeback | sections 3D, 10 |
| 10 | controller: two driving signals, hysteresis, bounded update rate, min/max, reset, deterministic | section 7 |
| 11 | target completion time separate from hard maximum age | section 7.3 |
| 12 | HEALTH/STATS fields for checkpoint pressure | section 7.4 |
| 13 | E off by default; no mq-deadline dependency | section 3E |
| 14 | revised regression bar | section 9 |
| 15 | S0 only, field list, requirements | section 11 |
| 16 | T14-T18, O_DIRECT length classes | section 8 |
| 17 | formal contract before S3/S4; C2 proof obligations | sections 1.3, 5.3 |
| 18 | revised order | section 10 |
| 19 | open-question decisions | section 12 |

## 1. Current snapshot and recovery semantics (AS-BUILT)

*Unchanged from revision 1 and approved (review §1).*

### 1.1 What a checkpoint is

A **fuzzy, per-shard checkpoint** with one atomic commit point for the
whole node.

**Trigger.** `snc_server` calls `Engine::checkpoint()` on the main thread
every `--checkpoint-s` (`tools/snc_server.cpp:1130`). The default is never.
Recovery step 8 also calls it after a remap, orphaned streams or a fresh
directory (`engine.cpp:687-692`). It blocks the main thread until every
shard reports (`engine.cpp:872-882`).

**Per shard s,** `Shard::begin_checkpoint` (`shard.cpp:1545-1569`) runs
**on the shard thread, in one step**:
- `start_lsn_s := wal.next_lsn()`;
- copies the IDs of every live object into `ckpt_.ids`. This is O(map),
  about 625,000 IDs per shard here. **MEASURED** at about 50 ms per
  shard; at 436,646 ops/s it alone left about 7,000 late ops;
- opens `part-<s>.ckpt` (buffered, `O_TRUNC`) and stages a header block.

**Walk.** `checkpoint_step` (`shard.cpp:1609-1648`) runs once per loop
iteration (`poll`, `shard.cpp:614-617`), 256 objects per call
(`kCheckpointSlice`, `shard.cpp:22`). For each snapshot ID still live, it
appends an IMAGE record of the object's **current** state (version and
`last_lsn` at visit time) to a 1 MiB chunk and tracks `max_lsn`. Full
chunks go out as buffered `writev2` through io_uring (`sri_write`,
`dsync=false`, `shard.cpp:1571-1583`; `runtime.cpp:183`).

**Seal (C5).** Once the walk is done, no write is in flight and
`max_lsn` is **locally** durable (`shard.cpp:1643`), the shard issues
`fdatasync` on its part (`sri_fdatasync`, `shard.cpp:1646`; io_uring
`IORING_FSYNC_DATASYNC`). The completion reports
`{start_lsn, objects, bytes}`. Local durability rather than the standby's
acknowledgement is the B1 fix (`shard.cpp:1636-1642`).

**Commit (C6).** The main thread runs, in order (`engine.cpp:883-916`):
1. `fsync_dir(ckpt/<seq>)`;
2. writes the **manifest** (seq, slot map, `start_lsn[s]`, part counts
   and bytes, type catalog);
3. writes the **superblock** with `active_ckpt = seq`, into the
   generation-alternating slot, synchronously (`storage.cpp:228-243`).

This superblock write is the **only** commit point.

**Reclaim (C10).** Only after the commit (`engine.cpp:931-973`), the
previous checkpoint's directory is removed (unless a seed pins it). WAL
segments whose records all fall below `start_lsn[s]` are deleted. Segments
a standby or seed may still need are kept, up to the retention budget.

### 1.2 Recovery

Recovery is `engine.cpp:484-560` when the slot map is unchanged:
1. Read the superblock, then the manifest it names.
2. Each shard loads its own part with `scan_part` (`storage.cpp:483-506`)
   and `install` (version rule: skip if the current version is greater or
   equal, `shard.cpp:1220`). Installed objects get `last_lsn = 0`, meaning
   durable. The object count must equal the manifest's
   (`engine.cpp:552`).
3. Each shard replays its own WAL stream from `start_lsn[s]` with the same
   version rule. DELETE erases only an older version (`shard.cpp:1213-1218`).
4. Index rebuild, new segments, then step 8 if needed.

**Part format fact that matters for C2 (AS-BUILT).** `scan_part` reads
the whole file and parses records from the first block **to end of
file**. A zero-length or invalid record is fatal ("corrupt checkpoint
part", `storage.cpp:497`). The manifest's byte count is not used to stop
the scan. So zero padding after the last record would be fatal today
(section 5.3).

### 1.3 Checkpoint correctness contract (normative; review §1, §17)

These clauses are the contract every checkpoint change must keep. Each
implementation PR states which clauses it touches, and each clause has
tests that must pass before that PR merges. With S0 the contract is also
added to `docs/CORE_CONTRACTS.md` (docs only), so that it does not live
only in this design.

| Clause | Statement | Holds today because | Test anchors |
|---|---|---|---|
| **CC-1 (I1) coverage** | Every object that is live at `begin` and **not mutated** in `[start_lsn, crash]` is in the part, with its image as of before `begin` | the ID snapshot is taken in one step; every untouched snapshot ID is still found by `map_.find` | T1, T13 (B2 only) |
| **CC-2 (I2) redo completeness** | Every mutation at LSN ≥ `start_lsn` is in the WAL as a full post-op image (CREATE/UPDATE) or a DELETE, and stream s is kept from `start_lsn[s]` until a later checkpoint commits | WAL format; reclaim only after commit | T4, T16 |
| **CC-3 (I3) no future state** | No image in a part has `last_lsn` above what was locally durable before that part's sync | the C5 check (`shard.cpp:1643`) before `fdatasync` | T2, T3 |
| **CC-4 (I4) idempotent redo** | Installing a part image, then replaying any record with version ≤ that image's, is a no-op; a newer one replaces it | version rule in `install` and replay | T1, T12 |
| **CC-5 (I5) atomic publication and reclamation** | No reader looks at `ckpt/<seq>` until a durable superblock names it. Nothing the previous checkpoint and the WAL need is deleted before that superblock is durable | generation-alternating superblock; reclaim after commit | T2, T3, T18 |

Two consequences:
- **Parts may become durable at different times, safely** (CC-5).
  A crash before the commit recovers from the previous checkpoint plus a
  longer WAL replay.
- **The checkpoint is not a single instant across shards.** Shard s
  represents "WAL prefix of stream s, from `start_lsn[s]`". Cross-shard
  consistency comes from each stream's durable prefix plus follow-up
  re-emission, exactly as after any crash. The checkpoint adds no
  cross-shard guarantee and needs none.

## 2. The two measured bottlenecks

*Unchanged, except the WAL I/O mode (section 0.1).*

| | Walk (CPU) | Storage burst (I/O) |
|---|---|---|
| What | 625,000 map lookups, IMAGE encodes and page-cache copies per shard; one 50 ms ID snapshot at `begin` | about 1 GB of dirty pages across 8 shards (INFERRED: about 1,090 MB/s × about 1.1 s), forced out by 8 `fdatasync`s that start within 1 ms of each other |
| MEASURED | walk 1.17-1.20 s at 436,646, 1.69-1.71 s at 2,183,233. Closed loop: polls -58%, throughput about -55% during the walk | `fdatasync` 1.00-1.26 s. WAL write await 0.048-0.065 ms → 10.7-12.1 ms. Durable capacity about 0.35-0.57M ops/s |
| Refusals at 436,646 | **none** in the walk | **all** of them (2,267 / 6,488 / 9,455) |
| Refusals at 2,183,233 | 17k-126k | most of the rest |
| Why it bites | slices are fixed at 256 objects per iteration and ignore load | one WAL flush in flight per shard, each about 200× slower, so each shard's durable rate collapses; cross-shard follow-ups wait in `emit_wait_` (`shard.cpp:1787-1800`) |
| Gate | `emits_in_flight() > 1024`, checked **before** the op is looked up (`shard.cpp:110`), so reads are refused too | same |

A third cost is the O(map) `begin` snapshot (about 50 ms per shard,
MEASURED). It is not what blocked CP-1A (review §8).

## 3. Options A to F

### A. Read and mutation admission separation

**What.** Decide admission per operation class and per resource, instead
of one gate for everything. The full resource matrix, the reservation
rule and the scheduling priority are in section 6.

**Correctness.**
- A read never writes the WAL. It does not move `durable_lsn` or emit a
  follow-up, so it cannot delay backlog recovery *through state*.
- It still answers only once the object's last write is durable
  (`out.wait_lsn`, `shard.cpp:364`). The durability contract is unchanged.
- A standby's read lease rules are untouched.

**Not CPU only (review §6).** A read can also hold connection output
buffer, a durability waiter (undurable object), a forwarding pending slot
and SRI ring capacity (non-owner), and memory bandwidth (cache misses on
a large map). Section 6.1 bounds each one.

**Risk.** Low for durability. Medium for client behaviour: E_OVERLOADED
on reads becomes rarer and comes from a different limit.

**Fixes:** reads refused during stalls; adds two missing memory bounds.
**Doesn't fix:** the stall itself.

### B. Paced, incremental checkpoint walk

**B1 (DECIDED as the first CPU fix).** Keep the one-step ID snapshot.
Bound the walk per loop iteration by **time**, not object count; check
the clock every small batch of objects. CC-1 holds unchanged: pacing
changes visit *times*, not the visit *set*. Concurrent mutation is
already handled: a visited object shows its current image, and CC-2/CC-4
cover later writes.

No budget values are frozen (review §7). The controller bounds
(`walk_min_ns`, `walk_max_ns`) are HYPOTHESES until S0 measures:
- per-object walk cost (lookup + encode + copy), as a histogram;
- the foreground cost of a walk slice: loop iteration time and request
  latency with and without the walk at a fixed open-loop rate.

The bounds are then preregistered before S6.

**Catch-up is observable and bounded (review §7, §10, §11).** When the
checkpoint falls behind its target completion time, the walk budget
rises, one controller step at a time, but never above the
foreground-protection ceiling (section 7.2). Every iteration in catch-up
is counted. Pacing is never turned off: there is no mode that returns to
"walk until done".

**B2 (DEFERRED until after the CP-1A rerun, review §8).** Walking the hash
table directly over several iterations can miss untouched objects
(backward-shift deletion moves entries across the cursor; growth
rehashes), which breaks CC-1. Its fixes (a per-object mark bit, or a carry
hook in `LocalMap`) are **not** built now, and no mark bits are added.
B2 is reconsidered only if S6 shows that the one-step `begin` itself
breaks the regression bar.

### C. Bounded checkpoint writeback (mechanism not chosen; review §3)

**What.** Checkpoint bytes reach the device while the walk runs, with a
bounded amount in flight. The final sync then has little to flush.
Revision 1 preferred C2. That preference is withdrawn: C2 is one
candidate, and S3 begins with a short design note (S3-D) that picks a
mechanism against the criteria below.

**Candidates.**
- **C1, buffered write plus early writeback.** Today's buffered writes,
  plus `sync_file_range(SYNC_FILE_RANGE_WRITE)` per chunk
  (io_uring `IORING_OP_SYNC_FILE_RANGE`). Optionally
  `SYNC_FILE_RANGE_WAIT_BEFORE` on the chunk before last, to cap dirty
  bytes. The final `fdatasync` stays.
- **C2, `O_DIRECT` aligned writes.** Parts opened `O_DIRECT`, written in
  aligned chunks from aligned buffers, with a buffered fallback on
  `EINVAL` like the WAL's (`storage.cpp:539-546`). The last chunk is
  padded to a block, and the file is truncated back to its logical
  length before the final sync (section 5.3). The final `fdatasync`
  covers metadata and the device cache.
- **C3, buffered writes with `RWF_DSYNC` per chunk.** Already supported
  by the runtime (`sri_write(..., dsync=true)`, `runtime.cpp:183-186`;
  the same in the Seastar runtime, `runtime.cc:360-362`). Each chunk
  completes only when it is durable. Bytes in flight are bounded by the
  window. The final sync is then nearly empty.

**Comparison against the review's criteria** (INFERRED; S3-D confirms):

| Criterion | C1 `sync_file_range` | C2 `O_DIRECT` | C3 `RWF_DSYNC` per chunk |
|---|---|---|---|
| Bounded bytes and requests in flight | requests yes; dirty bytes only if `WAIT_BEFORE` is used | yes, exactly | yes, exactly |
| No large dirty page cache | yes with `WAIT_BEFORE`, else only bounded by the window loosely | yes (no page cache) | yes (each write is written back before it completes) |
| File format and recovery unchanged | yes | needs truncation to logical length, or a versioned format (5.3) | yes |
| Predictable final sync | mostly; metadata plus whatever is still dirty | yes; metadata plus device cache | yes; nearly empty |
| CPU and copy overhead | one page-cache copy (today's) | none beyond the encode buffer | one page-cache copy |
| Works across Linux filesystems and devices | `sync_file_range` gives **no** durability and its writeback behaviour varies by filesystem; it is only a hint here | ext4 and XFS yes; tmpfs and some others refuse (fallback needed) | yes, standard |
| No reliance on mq-deadline behaviour | yes | yes | yes |
| Clean error handling | `sync_file_range` errors are advisory; real errors surface at `fdatasync` | partial or short direct writes, alignment errors: must be handled explicitly | errors surface per chunk |
| Extra device work | none | none | one cache flush or FUA per chunk; at 1 MiB chunks that is about 1,000 per GB, competing with the WAL's own flushes (to be MEASURED) |

**Current leaning, not a decision.** C3 is the smallest change, keeps
the format, and uses a path the runtime already has. Its open question
is the per-chunk flush cost on the WAL's device. C2 has the best
behaviour on paper and a pattern already proven in the WAL, but it
changes the part's on-disk path (padding and truncation) and adds error
cases. C1 is the weakest bound, because its writeback is only a hint.

S3-D will propose a small dev-machine micro-characterization of C1, C2
and C3: WAL `RWF_DSYNC` latency while a paced checkpoint writer runs,
on the dev VM's disk. It needs approval first. The R640 is not used for
it (review §3).

**Pacing.** A window of checkpoint writes in flight per shard, driven by
the controller in section 7. The walk produces a chunk only when the
window has room, so the walk is back-pressured too.

**Correctness.** I/O order is not a correctness issue: the part is inert
until the commit (CC-5), and the C5 check still runs before the final
sync (CC-3). C2's extra obligations are in section 5.3.

### D. Staggered shard sync (conceptually approved, review §9)

**What.** A node-level token (`std::atomic<u32>` in the Engine) admits K
shards to the final sync step at a time. The others hold in
`checkpoint_step`, so the thread never blocks.

**Atomicity and crashes.** Unchanged: commit only after all parts report
(CC-5). A crash during staggered syncs is the same as a crash during
today's syncs: the previous checkpoint plus a longer replay.

**K.** K = 1 is PROPOSED, not frozen. After bounded writeback exists,
K = 1, 2, 4 and 8 are compared on a non-campaign characterization or a
deterministic model. The smallest K that meets the target completion
time without harming WAL latency is chosen.

**Not alone.** D lands with or after C. Serializing eight of today's
large buffered `fdatasync`s would lengthen the checkpoint about 8× and
only lower the peak (review §9).

### E. I/O prioritization (optional, off by default; review §13)

**What.** Mark checkpoint writes with a lower I/O priority
(`sqe->ioprio`, `IOPRIO_CLASS_BE` level 7). The WAL stays at the default.

**Notes.**
- The design must succeed without E. It is never a correctness or
  performance dependency.
- Whether a scheduler or device honours priority is not assumed
  (`mq-deadline`, `none` and device-internal queues differ).
- Buffered writeback is issued by kernel workers, so E has effect only
  on direct writes (C2) or, partly, on `RWF_DSYNC` writes (C3, to be
  checked). The WAL's own writes are direct (section 0.1).
- If tested later (S5), it is an optimization dimension only.

### F. Incremental or copy-on-write checkpoints (deferred)

Unchanged from revision 1: delta checkpoints, log compaction or COW
snapshots would make checkpoint cost scale with change, not size, at
the price of a new on-disk format, chain management for reclamation,
seeds and standbys, and a much larger proof burden. Reconsidered only if
S1-S6 fall short at campaign scale.

### Comparison

| | Fixes reads refused | Fixes the I/O burst | Fixes walk CPU | Semantic risk | Code size | Status |
|---|---|---|---|---|---|---|
| A | **yes** | no | no | low | small | approved, S1 |
| B1 | no | no | **yes** | none | small | approved, S2 |
| B2 | no | no | yes (+ `begin`) | medium | medium | after CP-1A |
| C (C1/C2/C3) | no | **yes** | partly (backpressure) | low (C2: low-medium) | small to medium | approved in principle; mechanism at S3-D |
| D | no | partly | no | low | small | approved, S4, after C |
| E | no | maybe | no | none | tiny | optional, S5 |
| F | no | yes | yes | high | large | deferred |

## 4. Preferred design (approved direction)

**A + B1 + C (mechanism at S3-D) + D, E optional, B2 after CP-1A, F
deferred.**

```
shard loop iteration (section 6.3 gives the priority rules):
  I/O completions                   WAL completions, waiter release
  run_emits()                       follow-ups (unchanged position in poll)
  links / WAL poll / replication    (unchanged)
  admitted foreground work          SRI requests, client requests;
                                    per-class gates (A); a time quantum
                                    only under backlog pressure
  checkpoint_step(budget)           B1: walk until the time budget is spent
                                    or the write window is full
                                    C: issue chunk writes while
                                    in flight < window
                                    D: walk done, writes done, max_lsn
                                    locally durable -> take the sync token
                                    -> sync -> release the token -> report
```

**Unchanged:** the checkpoint content; `start_lsn`, the manifest and the
superblock commit; reclamation, recovery and `install`; C5;
`kEmitBacklog`; the Engine API; the client protocol (E_OVERLOADED stays
the refusal status, DECIDED).

**New configuration** (all HYPOTHESES until S0; none frozen): the waiter
and WAL-buffer budgets, the foreground time quantum, the walk budget
bounds, the window bounds, the sync token count, the target completion
time and the hard maximum checkpoint age. Each slice adds only the knobs
it needs.

## 5. Conceptual durability and recovery proof

### 5.1 Claim

For every shard s, the state recovered after any crash equals the state
produced by applying stream s's durable WAL prefix, as today. No
acknowledged write is lost or reordered.

### 5.2 Proof sketch (approved at design stage, review §17)

Under the approved direction, CC-1 to CC-5 still hold:

- **CC-1.** B1 keeps the one-step ID snapshot. The walk visits exactly
  the snapshot IDs and skips only those absent or deleted at visit time;
  their DELETE is at LSN ≥ `start_lsn` and is replayed (CC-2). Pacing
  changes visit times, not the visit set.
- **CC-2.** The WAL format and logging are unchanged. Reclamation still
  runs only after the commit. A slower checkpoint only keeps segments
  longer; the hard maximum age (section 7.3) bounds that and is
  reported.
- **CC-3.** The C5 check still gates the sync step. With D it runs
  before taking the token and again after taking it. `max_lsn` only grows
  with visits, and the walk is finished by then.
- **CC-4.** Unchanged.
- **CC-5.** Parts are still unreferenced until the manifest and
  superblock. Bounded writeback and staggering change only when part
  bytes reach the device. The commit still waits for every shard's sync
  completion.

**Admission (A).** A read admitted under backlog returns either a
durable value at once, or a deferred answer released only when
`durable(last_lsn)` holds (`shard.cpp:364`, `release_waiters`). A
refused op leaves no state behind: section 6.2 proves this for
mutations, which is the hard case.

**Crash points** (each recovers by CC-5 to the previous checkpoint plus
replay):
1. during the walk;
2. with chunk writes in flight (some on disk, some not);
3. a shard waiting for the sync token;
4. some shards synced, others not;
5. all synced, before the manifest;
6. after the manifest, before the superblock;
7. after the superblock, before reclamation (the next start sees the new
   checkpoint; old files are garbage only).

### 5.3 Extra obligations if C2 (`O_DIRECT`) is selected (review §17)

- **Representation.** The committed part must be byte-identical to
  today's format, or carry a new version. PROPOSED: identical. The last
  chunk is padded to the block size for the direct write, then the file
  is `ftruncate`d to its logical length **before** the final sync, so
  the sync also makes the size durable. Recovery is unchanged.
- **Padding is never parsed as records.** Follows from truncation.
  As defence in depth, `scan_part` could stop at the manifest's byte
  count. That is a recovery change, so it is proposed only together with
  C2 and tested on old and new parts.
- **The manifest's logical byte and object counts stay authoritative.**
  The object count check (`engine.cpp:552`) stays. A byte-count check
  against the file size is added with C2.
- **An error before the commit leaves the previous checkpoint
  recoverable.** Any short write, `EIO`, alignment error or sync failure
  aborts this checkpoint: no manifest, no superblock, no reclamation.
  The previous superblock still names the previous checkpoint, and the
  WAL from its `start_lsn` is still retained (CC-5). T18 tests each case.
- **Length classes.** Tests cover a part that is header only, exactly
  one block, one byte over a block, exactly a chunk, one byte over a
  chunk, and a multi-chunk part with a short tail.

C1 and C3 keep buffered files, so they need only the error-path
obligation (the last bullet but one).

## 6. Admission and backpressure resource boundaries

### 6.1 Operation and resource matrix (review §6)

| Class | Resources consumed | Gate today | PROPOSED gate |
|---|---|---|---|
| Pure READ, owner shard, object durable | CPU; memory bandwidth; connection output buffer for the answer | follow-up backlog | foreground time quantum under pressure (6.3); the existing 8 MiB per-connection output limit, at which the connection stops reading (`conn_set.cpp:20,103`) |
| READ of an object whose last write is not yet durable | the above plus one **durability waiter** until the WAL catches up (`complete_local`, `shard.cpp:222-236`) | follow-up backlog | **waiter budget**, checked before the op runs (6.2) |
| READ with `allow_undurable` | as pure READ | follow-up backlog | as pure READ |
| Forwarded READ (non-owner origin) | at the origin: a pending slot (`pending_`, `shard.h:500`) and SRI ring or overflow; at the owner: a REMOTE waiter if undurable (`serve`, `shard.cpp:1148-1165`) | follow-up backlog at the origin, then the ring | pending slots and the ring at the origin (existing). The owner's REMOTE waiters are bounded by the origins' pending pools; they are **not** refused at the owner (6.2) |
| INDEX lookup and gather | a gather slot, SRI ring and overflow to every shard (`shard.cpp:785-813`) | ring and overflow (separate path) | unchanged: own gather pool and ring |
| MUTATE, CREATE, DELETE (client) | WAL active-buffer bytes; a waiter; possibly follow-ups; a session record if sequenced | follow-up backlog | follow-up backlog (**unchanged**, 1,024) **plus** a WAL-buffer reservation **plus** a waiter reservation, both before the op runs (6.2) |
| Resent sequenced MUTATE already applied | a session read; a waiter until the session record is durable | special-cased (`shard.cpp:111-122`) | unchanged: answered, never refused, before any budget check |
| Follow-up apply (`serve_emit`, `run_emits`) | WAL bytes, maybe more follow-ups | never refused | **never refused** (part of an already acknowledged op); bounded by the senders' follow-up gate |
| Standby apply (`append_raw`), recovery, migration install, session expiry | WAL bytes | never refused | **never refused**; bounded by their own protocols (replication retention, staging) |

### 6.2 Reservation before mutation (review §5)

**Traced mutation path (AS-BUILT,** `run_exec`, `shard.cpp:294-420`**).**
In order:
1. standby and withdrawn checks; session replay (`replay_if_seen`);
   slot serving; `map_.find`; op, type and argument checks; expected
   version. **No state changes.**
2. `validate` (`shard.cpp:354`). Checked not to modify the object.
   **No state changes.**
3. `apply` (`shard.cpp:376`, the MUTATE branch): **mutates the object in place.** This is
   the first irreversible step.
4. `++version`; the local peer's UPDATE is logged first if the op used
   `local()` (`shard.cpp:398-403`); index maintenance; `record_session`
   may allocate a session object, insert it in the map and index, and
   log it (`shard.cpp:662-692`); then the object's UPDATE is logged
   (`shard.cpp:415`); then follow-ups are queued (`queue_emits`).
5. The answer either completes at once or becomes a waiter
   (`complete_local`).

`run_create` (`shard.cpp:426`) and `run_delete` have the same shape:
checks, then the first mutation, then logging.

So a WAL-buffer or waiter budget **must be checked after step 2 and
before step 3.** Refusing any later would leave an applied but unlogged
mutation.

**Rule (PROPOSED).**
- **R1. Reserve, then mutate.** After step 2 succeeds, the op computes
  its worst-case WAL bytes: `record_size(type.max_size)` for the object
  (`registry.h:56`, at most `kMaxPayload` = 1 MiB, `types.h:21`), plus
  the same for the `local()` peer's type if the op declares one, plus
  one session record if sequenced. If
  `active_bytes + reserved + worst_case > wal_buffer_budget`, or
  `waiters + 1 > waiter_budget`, it is refused with E_OVERLOADED before
  `apply`, and `refused_by` records which budget refused it.
- **R2. Appends never fail.** `WalStream::append` keeps growing its
  buffer if it must (`ensure_capacity`, `storage.cpp:566-575`). The
  budget is an admission bound, not an append limit. So a refusal can
  only happen at R1, where nothing has changed.
- **R3. The reservation cannot be invalidated.** Steps 2 to 4 run on
  the shard thread in one call, with no yield, no I/O wait and no
  re-entry into admission. No other op can consume the reserved bytes in
  between. The worst case is computed from type declarations, so the
  actual append can only be smaller. (The reservation is released when
  the call returns; it is a check, not a held counter.)
- **R4. Multi-record ops.** The `local()` peer and the session record are
  on the same shard and are logged in the same call, so R1's sum covers
  them. Cross-shard effects are follow-ups, applied later on their
  target shard under "never refused" (6.1); the origin's follow-up gate
  bounds them.
- **R5. Sessions stay idempotent.** The replay check runs before R1. A
  resend of an applied op is answered from its session, never refused.
  A new sequenced op refused at R1 has not touched its session, so the
  client's retry with the same sequence number runs as a new op exactly
  once.
- **R6. No acknowledged or partly applied mutation is lost.** A refused
  op was never applied (R1, R2). An admitted op is logged in full (R2).
  Its answer waits for durability exactly as today.

Exempt paths (follow-ups, standby apply, recovery, migration, session
expiry) skip R1 and are bounded by their own protocols. T14 tests every
boundary.

### 6.3 Scheduling priority (review §6)

**As built, one loop iteration** (`runtime.cpp:335-372`; `engine.cpp:80-88`;
`shard.cpp:604-618`):
1. drain the SRI rings from every other shard, `batch_in` = 32 messages
   per ring (`native_runtime.h:20`): forwarded requests, answers,
   follow-ups, index messages;
2. injected control tasks;
3. `reap_io`: I/O completions, including WAL flush completions that
   release waiters;
4. `Shard::poll`: `run_emits`, links, WAL poll (starts a flush),
   replication, fence, then `checkpoint_step`;
5. `app_poll`: client connections (`ConnSet::poll`, up to 64 epoll
   events, each of which may carry many requests);
6. `flush_batches`, `publish_all`.

So today the checkpoint step runs **before** client requests in each
iteration, and inbound requests on the rings are drained **before** I/O
completions.

**Required priority (DECIDED by the review):**
1. WAL and I/O completions and durability progress;
2. follow-up and backlog recovery;
3. bounded admitted foreground work;
4. checkpoint background work.

**PROPOSED mapping, for S1 and S2:**
- (1) `reap_io` moves before the ring drain. Completions are cheap and
  unbounded today. This is a behaviour change in S1, behind the S1
  switch, measured by T9 and T10.
- (2) `run_emits` and the WAL poll keep their place, right after the
  completions. They are never budgeted.
- (3) Foreground work, ring requests plus client requests, gets a **time
  quantum per iteration only while the follow-up backlog is above a
  pressure threshold** (HYPOTHESIS). When the quantum is spent, the rest
  waits in the ring or socket for the next iteration: deferred, not
  refused. Below the threshold there is no quantum, which is today's
  behaviour. A time quantum, not a count, because op cost varies with
  type and payload (review §6).
- (4) `checkpoint_step` moves after `app_poll`, and its time budget
  comes from section 7. Its floor keeps it from starving (7.3).

The quantum and threshold are derived after S0 measures per-op CPU cost
by class and the backlog's drain rate.

### 6.4 Bounds and how they will be derived (review §4)

| Resource (per shard) | Bound | Refused with | Derivation (after S0) |
|---|---|---|---|
| Follow-up backlog `emits_in_flight` | 1,024 (**unchanged**, DECIDED) | E_OVERLOADED | not revisited until after the architecture is fixed |
| Durability waiters | `waiter_budget` (HYPOTHESIS; revision 1's 16,384 withdrawn as a default) | E_OVERLOADED | `waiter_bytes_budget ÷ measured bytes per waiter`. A waiter is a `Waiter` in a `std::deque` (`shard.h:332-346`, `shard.h:524`) plus a heap copy of the result, so its size varies; S0 measures the mean and peak |
| WAL active buffer | `wal_buffer_budget` (HYPOTHESIS; 64 MiB withdrawn as a default) | E_OVERLOADED | from S0's peak active bytes under the causal cells, and a latency budget: bytes ÷ measured flush rate ≤ the tolerated wait |
| Foreground time quantum | HYPOTHESIS | deferred, not refused | from S0's per-class op cost and backlog drain rate |
| SRI ring, overflow, pending, gather pools | existing | E_OVERLOADED | unchanged |
| Connection output | existing 8 MiB | the connection stops reading | unchanged; T17 checks it holds under a read flood |
| Checkpoint write window | 1..`window_max` (HYPOTHESIS) | internal | S3-D |
| WAL retained by a checkpoint | hard maximum age (7.3) | n/a | S0 measures retained bytes |

**Status code (DECIDED).** E_OVERLOADED for every budget refusal; no new
client status. The resource that refused goes to STATS as `refused_by`
counters.

## 7. Adaptive pacing: signals and control (simplified, review §10)

### 7.1 Signals

**Driving signals, exactly two:**

| Signal | Source | Normalized pressure |
|---|---|---|
| WAL flush latency EWMA | `WalStream` per-flush submit-to-completion (already bucketed into `flush_us_log2`) | `ewma ÷ baseline`, where `baseline` is the EWMA learned while no checkpoint is active (per device, so no hardware number) |
| Follow-up backlog fraction | `emits_in_flight ÷ kEmitBacklog` | the fraction itself |

**Observed only, not driving (initially):** waiter fraction, undurable
records (`next_lsn − durable_lsn`), checkpoint write latency. All go to
STATS so S6 can show whether they would have helped.

### 7.2 Controller (PROPOSED structure; numbers are HYPOTHESES)

A pure function, `next = step(state, signals, now)`, with no I/O and no
clock of its own, so synthetic traces can drive it in tests (T15).

- **State:** window (chunks in flight), walk budget (ns per iteration),
  mode (normal, catch-up), and the last update time.
- **Pressure:** `high` if `wal_ratio > H_wal` or `backlog > H_bl`; `low`
  only if `wal_ratio < L_wal` **and** `backlog < L_bl`, with `L < H`
  (**hysteresis**). Otherwise hold.
- **Update rate bounded:** at most one change per chunk completion and
  at most one per `T_update`.
- **AIMD:** on `high`, halve the window and the walk budget, down to
  their minimums (`window_min` = 1, `walk_min_ns`). On `low`, add one
  window step and one walk-budget step, up to `window_max` and
  `walk_max_ns`.
- **Reset at checkpoint start:** window = `window_min`, walk budget =
  `walk_min_ns`, mode normal. Nothing carries over from the previous
  checkpoint except the learned baseline.
- **Constant pressure cannot starve the checkpoint:** at the minimums,
  the checkpoint still makes progress, `walk_min_ns` per iteration and
  one write in flight. Constant pressure cannot make it oscillate
  either: under `high` it stays at the minimums.
- **Foreground-protection ceiling:** catch-up mode (7.3) may raise the
  walk budget and window above what pressure allows, one step per
  update, but never above `walk_ceiling_ns` and `window_ceiling`. If
  the target still cannot be met at the ceiling, the controller does not
  go further. It reports pressure and lets the checkpoint take longer,
  up to the hard maximum (7.3).

The 0.25/0.5 thresholds and the 20/200 µs walk bounds in revision 1 are
withdrawn as defaults. They are chosen from S0 data and preregistered
before S6.

### 7.3 Target completion time and hard maximum age (review §11)

Two separate concepts. The checkpoint interval is only a trigger
cadence.

- **Target completion time** `ckpt_target_s`: the pacing goal. If the
  remaining work at the floor rate would miss it, the controller enters
  **catch-up** (7.2), within the ceiling. A miss is counted
  (`past_target`) and reported. It is not an error.
- **Hard maximum checkpoint age** `ckpt_max_age_s`: the safety bound for
  WAL retention and operations. Its value comes from the WAL disk
  budget and acceptable recovery time, not from the interval.
  - Approaching it (for example at a preregistered fraction) sets a
    warning in HEALTH.
  - Reaching it sets the checkpoint state to **degraded** in HEALTH and
    STATS.
  - Even then the checkpoint is not abandoned, since restarting would
    take longer. Pacing is not turned off, and no WAL is reclaimed
    early.
  - Whether a breach should also raise the ceiling is an open question
    (section 12).

Revision 1's "deadline = 0.5 × interval" is withdrawn.

### 7.4 HEALTH and STATS (review §12)

Published per shard as single-writer relaxed atomics, the same pattern
as the progress counters (`docs/HEALTH_DECOUPLING.md` 3.2). HEALTH reads
only published values: no shard task, no cross-shard poll, no lock a
shard holds (HL-1). The node line shows the worst shard; STATS shows
every shard.

Fields:
- `ckpt_phase`: idle, begin, walk, writeback, wait_token, sync, report;
- `ckpt_age_ms` (active age) and `ckpt_target_ms`;
- `ckpt_past_target` (flag) and `ckpt_past_target_count`;
- `ckpt_state`: ok, warn (near the hard maximum), degraded (past it);
- `wal_retained_bytes` attributable to the uncommitted checkpoint, where
  practical;
- `ckpt_pressure` and `ckpt_window`;
- `ckpt_last_ms`, `ckpt_last_result`.

**HEALTH semantics follow HL-0 and HL-4.** These fields are
diagnostic. A degraded checkpoint never triggers promotion, withdrawal
or a grant on its own. The exact warn and degraded thresholds are
defined with S0 and reviewed.

## 8. Deterministic correctness and crash tests

In-process tests extend `tests/test_persistence.cpp` and
`test_crash.cpp`. Fault points use a test-only hook compiled into test
builds only. Every test runs in Release, ASan+UBSan and TSan. Each test
names the CC clause it anchors.

| # | Test | Proves |
|---|---|---|
| T1 | **Concurrent-mutation consistency.** Mutate, create and delete a known set during a paced checkpoint (walk budget forced tiny, so the walk spans thousands of iterations). Crash, recover, compare with a shadow model of the durable prefix | CC-1..CC-4 under pacing |
| T2 | **Crash at every phase.** Crash points 1-7 (5.2), on each shard, with K = 1 and K = 8 | CC-3, CC-5 |
| T3 | **Partial or staggered parts.** Some parts synced, others half-written, then a crash: recovery uses the previous checkpoint. A corrupt uncommitted part is ignored | CC-5 |
| T4 | **Replay from a long WAL.** The checkpoint is slowed past its target. Reclamation deletes nothing still needed. Recovery after the next commit is exact | CC-2 |
| T5 | **Reads under mutation backpressure.** Delay WAL completions until the backlog exceeds 1,024. Durable reads answer; undurable ones defer; none is answered before it is durable. MUTATE gets E_OVERLOADED | A |
| T6 | **Waiter and WAL-buffer budgets.** Exceed each budget: E_OVERLOADED with the right `refused_by`, no growth beyond the budget plus one op's worst case, recovery after release | 6.2, 6.4 |
| T7 | **Bounded walk time.** Instrumented `checkpoint_step` time per iteration ≤ the current walk budget plus one batch, including in catch-up | B1 |
| T8 | **Bounded I/O issuance.** Checkpoint writes in flight never exceed the window; the window halves under injected WAL latency | C, 7.2 |
| T9 | **Backpressure recovery.** After the fault is released, the backlog falls below the low threshold within a bound; no op is lost or answered twice (sequenced resends) | A, sessions |
| T10 | **No starvation.** A read flood during a mutation backlog: the backlog still drains, and mutations are admitted again | 6.3 |
| T11 | **Multi-shard completion.** 8 shards, K = 1: every shard completes, within the target when unloaded, and the commit happens once | D |
| T12 | **Unchanged durability and profile semantics.** Existing suites pass unchanged: persistence, crash, replication (sync and async), migration with checkpoint (`q10_*`), B1 (`q3_runtime`), the profile guard, sessions | regression |
| T13 | **B2 only, later.** Deletes and growth during an incremental walk; untouched-object coverage after a crash | CC-1 for B2 |
| **T14** | **Reservation atomicity.** Force WAL-buffer and waiter budget exhaustion at each boundary of 6.2: before validate, after validate, after apply, after each log call, after `queue_emits`. Only the post-validate check may refuse. Every refusal leaves the map, object versions, indexes, sessions and the WAL byte-identical to before. No mid-op exhaustion aborts an op. Covers `local()` peers, sequenced creates that make a new session, and follow-up and standby paths (never refused) | 6.2 R1-R6 |
| **T15** | **Controller trace determinism.** Feed synthetic WAL-latency and backlog traces to `step()` and assert the exact window, walk budget and mode after every step: hysteresis band, bounded update rate, min and max, reset at start, no oscillation under constant pressure, ceiling never exceeded | 7.2 |
| **T16** | **Target and hard-maximum pressure.** Sustained foreground pressure so the target is missed: the checkpoint stays paced within the ceiling, HEALTH shows past-target, then warn, then degraded at the hard maximum, deterministically. No WAL is reclaimed before the commit | 7.3, 7.4, CC-2 |
| **T17** | **Read-flood resource bound.** A durable-read flood on many connections during a mutation backlog: per-connection output stays ≤ its limit, the foreground quantum holds, waiters ≤ budget, and the backlog recovers | 6.1, 6.3 |
| **T18** | **I/O error paths.** Short write, direct-I/O alignment error (if C2), `EIO` on a chunk, `fdatasync` failure, checkpoint abort: the previous checkpoint stays active, its WAL is retained, the next checkpoint succeeds | CC-5, 5.3 |
| T18-L | **C2 only: length classes** from 5.3, each written, truncated, synced, recovered and compared | 5.3 |

## 9. Physical R640 regression plan (revised, review §14)

Preregistered on the diag3 harness with isolated scratch, on a new
frozen SHA. **The exact bar is frozen before any S6 data exists.**

**Primary cell.** 436,646 ops/s open loop, Profile A, `--checkpoint-s 60`,
60 s window, 3 reps. Today it refuses in 3/3.

**Comparators, same SHA, same machine state:**
- 436,646 with no checkpoint, 3 reps (today 0/3);
- 2,183,233 w60 with checkpoint, 3 reps (recorded, no pass/fail);
- closed-loop capacity with a checkpoint, on the corrected scratch
  harness, for the build before S1 and for the new SHA.

**Pass, in every rep of the primary cell:**
- 0 refusals;
- `late_over_1ms` ≤ 0.1% of `scheduled_in_window`;
- total p99 ≤ 1.5× the same-SHA no-checkpoint median (or paired
  reference, chosen at preregistration);
- p99.9 ≤ 10 ms;
- follow-up backlog never reaches `kEmitBacklog` (1,024). ≤ 50% is a
  **target**, reported but not a hard pass until S0 evidence supports
  it;
- the checkpoint completes within the preregistered hard maximum age;
- no correctness or durability error.

**Capacity guard.** Closed-loop checkpoint capacity ≥ 95% of the build
before S1, on the same harness and machine state.

**Controller evidence, reported (not pass/fail):**
- WAL-latency EWMA peak and time above the high threshold;
- backlog peak and time above 25% and 50%;
- checkpoint duration, per-phase times, and the window and walk-budget
  trace;
- `refused_by` counts; device await; `wal_direct`.

Revision 1's "WAL flush latency EWMA max ≤ the pressure target" is
withdrawn: a controller reacts *because* it exceeds its threshold, so
that criterion was circular.

**Then,** only after review: freeze the new SHA, preregister, and rerun
the full SNC-only CP-1A from the beginning.

## 10. Implementation slices and rollback points (revised, review §18)

Each slice is one PR. Each passes Release + ASan/UBSan + TSan and its
T-tests, gets an independent review, and waits for the Codex/cody
review before the next slice starts.

| Slice | Content | Switch / rollback | Tests |
|---|---|---|---|
| **DESIGN-R** | this revision | n/a | n/a |
| **S0** | permanent low-overhead observability (section 11); the CC contract into `docs/CORE_CONTRACTS.md`. **No behaviour change** | read-only; revert | existing suites; counter unit tests; overhead check |
| S1 | A: per-class admission, R1 reservations, waiter and WAL-buffer budgets, scheduling priority and foreground quantum (6.3) | `--admission legacy` restores today's single gate and loop order | T5, T6, T9, T10, T14, T17 |
| S2 | B1: time-budgeted walk, controller (7.2) with only the walk budget, target and hard maximum (7.3), HEALTH fields (7.4) | `--ckpt-pacing off` (256 per iteration) | T1, T7, T15, T16 |
| S3-D | short note choosing C1, C2 or C3 against 3C's criteria; may propose a dev-VM micro-characterization (approval needed) | n/a | n/a |
| S3 | the chosen bounded writeback, the window in the controller | `--ckpt-io legacy` | T2, T3, T8, T18 (+ T18-L for C2) |
| S4 | D: sync token; K sweep (model or non-campaign characterization) | `ckpt_sync_tokens = 0` (unlimited, today) | T11 |
| S5 | E: ioprio switch, only if still useful | off by default | A/B |
| S6 | preregistered R640 regression (section 9) | | full |
| Later | B2 after the CP-1A rerun unless S6 requires it; F only if campaign-scale evidence requires it | separate design | T13 |

## 11. S0: permanent observability (the only slice authorized after this review)

**Scope.** Counters and gauges only. No admission, pacing, I/O or
ordering change.

**Fields (per shard in STATS; node worst case in HEALTH where marked H):**
- `refused_by`: `backlog`, `ring`, `pending`, `gather`, `standby`,
  `moved` (budget reasons are added by S1);
- `waiters_now`, `waiters_peak`, and bytes per waiter (sampled mean and
  peak of `sizeof(Waiter)` plus the result copy);
- `wal_active_bytes_now`, `wal_active_bytes_peak`;
- `wal_flush_ewma_us` and the existing `flush_us_log2` histogram
  (exposed in STATS if not already);
- `emits_in_flight_now`, `emits_in_flight_peak` (H);
- `ckpt_phase` (H), per-phase durations of the last checkpoint,
  `ckpt_age_ms` (H);
- `ckpt_bytes_written`, `ckpt_writes_inflight`, `ckpt_writes_inflight_peak`;
- `wal_retained_bytes` attributable to the uncommitted checkpoint, where
  practical (H);
- `wal_direct` (the WAL's actual I/O mode, so section 0.1 is checked on
  every run);
- per-class op CPU cost samples (READ, MUTATE, CREATE, walk per object),
  as a cheap sampled histogram, to derive 6.3's quantum and B1's bounds.

Target and deadline pressure (`ckpt_target_ms`, `ckpt_past_target`,
`ckpt_state`) need the target and hard maximum, which arrive with S2.
S0 publishes `ckpt_age_ms` and phase durations, which are what S2
needs to choose them.

**Requirements (review §15):**
- read through the ordinary STATS and HEALTH paths;
- **no synchronous cross-shard diagnostic polling** like the discarded
  telemetry: each shard owns and publishes its own counters
  (single-writer relaxed atomics, or plain fields read by the existing
  STATS path);
- low overhead: no allocation, lock or syscall on the op path; sampled
  timing only;
- **no behaviour change**: the existing suites pass unchanged, and a
  test checks that every new field exists and moves under a scripted
  workload;
- full Release, ASan+UBSan and TSan, sequentially;
- independent review, then the Codex/cody review before S1.

## 12. Open questions for the DESIGN-R review

1. **Loop order (6.3).** Is moving `reap_io` before the ring drain, and
   `checkpoint_step` after `app_poll`, acceptable inside S1 behind the
   S1 switch, or should each be its own measured step?
2. **Reservation form (6.2).** A worst-case check at R1 with appends
   that never fail (no held counter), versus an explicit reservation
   counter taken at R1 and released after the log calls. The first is
   simpler and, per R3, sufficient on a single-threaded shard.
3. **Hard-maximum breach (7.3).** HEALTH reports degraded and the
   checkpoint keeps going within the ceiling. Should a breach also
   allow exceeding the foreground ceiling, or stay strictly diagnostic?
   The proposal is diagnostic.
4. **S0 field list (11).** Anything to add or drop before S0 starts.
5. **S3-D micro-characterization.** Whether a dev-VM comparison of
   C1, C2 and C3 is wanted, or the choice should rest on analysis
   alone.
