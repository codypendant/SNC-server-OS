# S3-D: bounded checkpoint writeback, mechanism selection

Status: **proposal for review.** No engine code changes here. S3 (the
implementation) waits for this note's review
(`docs/handoff/PR75_S2_Review.txt` sections 12-14).

## 0. Decision (proposed)

**C3: buffered part writes with `RWF_DSYNC` per chunk, a bounded window
of chunks in flight, and the final `fdatasync` kept.**

Why, in the review's order (section 13):

1. **Correctness simplicity.** One flag on a write the runtime already
   issues (`sri_write(..., dsync=true)`, both runtimes). No alignment, no
   padding, no truncation, no new runtime operation.
2. **Bounded dirty bytes.** Exact: a chunk's write completes only after
   its data is on stable storage, so dirty part bytes are at most
   window x chunk. Measured at 1-4 MiB with a 4 MiB window (section 3).
3. **Predictable interaction with WAL durability.** The best measured
   of the three on this dev VM: during a paced checkpoint, WAL p99 was
   1.4-2.0 ms across 9 runs, against 1.5-40 ms for C1 and 2.6-177 ms for
   C2 (section 3). The final sync stays under 0.5 ms, against 0.4-1.2 s
   of WAL stall with today's writeback.
4. **Recovery and file format.** Unchanged: the same bytes in the same
   file. DESIGN-R 5.3 (C2's obligations) does not apply.
5. **Portable failure behaviour.** *Corrected per the PR #76 review
   section 7:* `RWF_DSYNC` is a Linux durability flag for `pwritev2` and
   io_uring, not a POSIX interface. It has the semantics of `O_DSYNC`
   for the one write. SNC is Linux-hosted, so this is not a design
   problem, but its behaviour on each filesystem and device still needs
   validating. Errors surface on the chunk that failed, through the
   existing completion, not later at `fdatasync`.
6. **Operational simplicity.** No filesystem- or device-specific mode,
   no fallback path to test.
7. **Practical overhead.** One page-cache copy, as today. C2 would save
   most of the writer CPU (section 3), which is the only criterion it
   wins. Not decisive.

C2 is kept as the documented fallback if physical characterization shows
C3's per-chunk flushes harm the WAL on the R640's device (section 6). C1
is rejected: its bound is a writeback hint, its errors are deferred, and
it measured worse than C3 on the WAL.

## 1. The problem, restated

Today (`shard.cpp` `checkpoint_write_chunk`), the walk appends 1 MiB
buffered writes with **no window**: every chunk is submitted as soon as
it fills, and the page cache absorbs all of it. The final `fdatasync`
then writes the whole part at once, on the WAL's device. That is the
CP-1A abort mechanism (`docs/handoff/CP1A_Pilot_Abort_For_Review.md`,
the causal report): durable capacity collapses for about a second while
the part flushes.

S2 paced the walk. It did not change where the bytes go. S3 must make
part bytes reach the device while the walk runs, with a bound on what is
outstanding, so the final sync has nearly nothing left.

## 2. Candidates, as built in the characterization

The bench (`bench/ckpt_writeback_char.cpp`) models each mechanism with
plain syscalls, not the engine:

| Mode | Part writes | Bound on outstanding bytes | Final |
|---|---|---|---|
| legacy | buffered `pwrite` | none (page cache) | `fdatasync` |
| C1 | buffered `pwrite`, then `sync_file_range(WRITE)` on the chunk, and `sync_file_range(WAIT_BEFORE\|WRITE\|WAIT_AFTER)` on the chunk WINDOW back | window, via writeback completion of an earlier range | `fdatasync` |
| C2 | `O_DIRECT` aligned `pwrite`; last chunk padded; `ftruncate` to the logical length | window (no page cache) | `fdatasync` |
| C3 | buffered `pwritev2(RWF_DSYNC)` | window; each write returns when durable | `fdatasync` |

The WAL model is the engine's: `O_DIRECT` + `RWF_DSYNC`, 4 KiB writes,
one record slot every 250 us, group commit (one write covers every slot
due), and each record's latency measured from its slot (open loop, so a
stall charges every record queued behind it). The window is the number
of writer threads; a rate schedule models the walk producing chunks
(100 MB/s, or 0 = as fast as the mechanism allows). Dirty bytes are
`/proc/meminfo` Dirty + Writeback, sampled every 5 ms.

## 3. Results (dev VM; architectural evidence only)

`bench/results/s3d-devvm-20261108/` (runs.txt, README.md). ext4 on a
virtio disk with a volatile write cache, mq-deadline, kernel 6.18,
16 GiB RAM, dirty_ratio 20 / background 10. Three reps per cell,
rotated order, all 51 runs listed. Ranges are min-max over the 3 reps.

**Paced, 512 MiB part, 100 MB/s, window 4, 1 MiB chunks:**

| Mode | WAL p99 during write (ms) | WAL max during write (ms) | Dirty max (MiB) | Final sync (ms) | WAL p50 during final sync (ms) | Writer CPU (ms) |
|---|---|---|---|---|---|---|
| legacy | 0.7-28.9 | 2.9-51.1 | 511 | 395-643 | **186-206** | 172-255 |
| C1 | 3.8-15.0 | 41.7-63.5 | 2 | 1.2-2.0 | 0.4-0.9 | 255-286 |
| C2 | 2.6-25.4 | 10.0-77.1 | 0 | 0.5 | 0.1-0.8 | 76-86 |
| **C3** | **1.5-2.0** | **4.2-10.4** | 1 | **0.1** | 0.3 (1 of 3 runs sampled; the sync is shorter than a record slot) | 310-335 |

**Paced, 2 GiB part (larger than any dirty threshold would delay):**

| Mode | WAL p99 during write (ms) | Dirty max (MiB) | Final sync (ms) | WAL p50 during final sync (ms) |
|---|---|---|---|---|
| legacy | 19.7-25.8 | 1,568 | 1,172-1,242 | **123-616** |
| C1 | 4.9-40.3 | 2-4 | 1.3-1.8 | 0.5-0.9 |
| C2 | 22.6-176.6 | 0 | 0.3-0.5 | 0.2-0.9 |
| **C3** | **1.4-1.6** | 2-3 | **0.1** | 0.2 (1 of 3 runs sampled) |

**Other cells:**
- Window 1 (512 MiB, paced): C3 p99 1.4-1.8 ms, C1 1.5-20.6, C2 11.6-23.6.
  The window size did not change the ranking.
- C3 with 256 KiB chunks: p99 3.4-14.6 ms, worse than 1 MiB. Four times
  the per-chunk syncs per byte. Chunk size matters for C3; 1 MiB (today's
  `kCheckpointChunk`) is the better of the two measured.
- Unpaced (as fast as possible): every mechanism hurts the WAL (C3 p50
  3.5-3.9 ms, C2 4.5-4.8, C1 11-12.6 during the write). Bounded writeback
  does not replace pacing; the walk must be back-pressured by the window
  (DESIGN-R 3C "Pacing") and paced by S2.
- Baseline, no checkpoint: WAL p99 0.4-0.5 ms in quiet seconds, with
  occasional 38-40 ms VM pauses (2 of 3 baseline pre-phases). Single-run
  maxima here are VM noise, which is why the tables use p99 ranges.

**What this shows.** The legacy final sync reproduces the CP-1A mechanism
on a second machine: the WAL's median latency becomes hundreds of
milliseconds for as long as the part flushes, and that time grows with
the part. All three candidates remove it: dirty bytes stay at a few MiB
and the final sync is about a millisecond. They differ in how the
checkpoint's ongoing writes share the device with the WAL, and there C3
was the most consistent on this VM.

**What this does not show.** Why C2 and C1 had worse tails here is
INFERRED, not measured: large direct or explicitly-started writeback
requests queued ahead of the WAL's 4 KiB writes, against C3's writes
being ordered by their own sync. A virtio disk over a host page cache is
not the R640's SAS SSD. No number here is a campaign number, and none is
a hypothesis value for S3.

## 4. Error and filesystem behaviour (`probe`)

| Behaviour | ext4 (dev VM) | tmpfs (dev VM) |
|---|---|---|
| C2 `O_DIRECT` open | ok | **ok** (kernel 6.18; older kernels refuse with EINVAL) |
| C2 misaligned buffer, length or offset | EINVAL | accepted |
| C3 `RWF_DSYNC` | ok | ok |
| C1 `sync_file_range` | ok | ok (no effect) |

So C2 carries three alignment error classes that differ by filesystem
and kernel, and the WAL's "EINVAL at open means fall back" rule is not
enough on its own: a filesystem can accept the open and still treat
I/O differently. C3 has no such classes.

Error surfacing, from the syscalls' contracts:
- C1: `sync_file_range` reports only submission errors; a writeback I/O
  error surfaces later, at `fdatasync`, through errseq. The bound and the
  error are both indirect.
- C2: short writes and EINVAL per write; the logical length must be
  restored before sync (DESIGN-R 5.3).
- C3: an I/O error is returned by the chunk's own write, exactly as a
  WAL flush error is today.

All three keep today's failure rule: any part write or sync error is
`storage_fatal` before the commit, so the previous checkpoint and its
retained WAL stay authoritative (CC-5). S3 does not change that rule.

## 5. CC-1..CC-5 under C3

- **CC-1** (visit set): unchanged; S3 changes when bytes reach the
  device, not which objects are encoded.
- **CC-2** (WAL retention and reclamation): unchanged.
- **CC-3** (the C5 local-durability check before the part sync): unchanged.
  `RWF_DSYNC` makes chunks durable early; the part is still inert until
  the commit, and the check still runs before the final sync.
- **CC-4** (version-based idempotent install): unchanged.
- **CC-5** (manifest, superblock and reclaim ordering): unchanged. The
  final `fdatasync` stays, so the file's size and metadata are durable
  before the manifest names the part, whatever the per-chunk syncs did.

## 6. Risks to C3, and what S3 must measure

1. **Per-chunk sync cost on the WAL's device.** Each 1 MiB chunk is a
   data sync. Appending extends the file, so on ext4 each one can also
   commit the journal. On this VM that cost did not reach the WAL tail,
   but it is device-dependent. Before any R640 run: run this bench on the
   R640's data SSD, preregistered, and compare C3 with C2. If C3's p99
   WAL latency during the write is materially worse than C2's there, C2
   is reconsidered with its 5.3 obligations.
2. **Chunk size.** 256 KiB was worse than 1 MiB here. S3 keeps 1 MiB and
   reports per-chunk latency; larger chunks are a measured follow-up,
   not a default.
3. **Blocking without io_uring.** Both runtimes fall back to a
   synchronous `pwritev2` on the shard thread when io_uring is absent or
   its SQ is full (`src/native/runtime.cpp` `sri_write`). With
   `RWF_DSYNC` that blocks the loop for a durable 1 MiB write. Today's
   final `fdatasync` has the same property. S3 adds a gauge for chunks
   written on the synchronous path; a non-zero count on a benchmark run
   invalidates that run.
4. **Window.** S3 adds the window DESIGN-R 3C describes: the walk
   produces a chunk only while fewer than W chunks are in flight, so a
   slow device back-pressures the walk instead of growing memory. W is a
   HYPOTHESIS (the bench showed no ranking change between 1 and 4);
   it is configurable and published as a gauge, like S2's values.

## 7. S3 scope this decision implies (for the S3 review, not built here)

- `dsync=true` on part chunk writes; the final `fdatasync` kept.
- A per-shard write window W, gating chunk production in the walk.
- `--ckpt-io legacy|dsync` (DESIGN-R slice table), legacy = today.
- Gauges: chunk write latency, chunks in flight (exists), synchronous-path
  chunk count, final sync time (exists).
- Tests T2, T3, T8, T18 from DESIGN-R; T18 without its C2 cases.
- Not in S3: staggered sync (S4), follow-up intake backpressure (its own
  pre-S6 slice, review section 15), B2.

## 8. Open questions for review

1. Accept C3, with C2 as the fallback named in section 6 item 1?
2. Accept that the R640 confirmation of C3 against C2 happens at the
   preregistered R640 characterization (with S2's walk-cost
   re-derivation, review section 6), not before S3?
3. Keep 1 MiB chunks for S3 and treat chunk size as measured later?
