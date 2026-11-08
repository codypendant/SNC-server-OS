# S3: bounded checkpoint writeback (C3)

Status: **S3 built (PR #77); S3.1 lifecycle fix (section 10) for review.**
Work order: `docs/handoff/PR76_S3D_Review.txt` (C3 selected, S3 authorized
with conditions); S3 review: `docs/handoff/PR77_S3_Review.txt` (core
approved, teardown blocker). Mechanism selection: `docs/S3D_WRITEBACK.md`.

## 0. Summary

Each 1 MiB checkpoint part chunk is written with `RWF_DSYNC`, at most W
chunk writes are in flight per shard, and the walk stops producing chunks
while the window is full. Each write owns its buffer through a per-write
context that frees it when that write's own completion is processed, so
retained buffer memory is bounded by the window, not the checkpoint size.
The final `fdatasync` stays. `--ckpt-io legacy` restores the write path
before S3. CC-1..CC-5 are unchanged (section 7).

Not in S3 (by the work order): S4 sync tokens, staggered sync, follow-up
intake backpressure (its own pre-S6 slice), B2, C2, R640 load.

`RWF_DSYNC` is a Linux durability flag for `pwritev2` and io_uring, with
the semantics of `O_DSYNC` for one write. It is not a POSIX interface,
and its behaviour on each filesystem and device still needs validating
(review section 7).

## 1. Conditions of the work order, and where each is met

| Review section | Condition | Where |
|---|---|---|
| 3 | completed buffers freed at once; ownership does not assume completion order; retained memory W-bounded | `CkptWrite` (`shard.h`); `checkpoint_write_complete` frees `w->buf` (`shard.cpp:2084`); gauge `ckpt_buf_bytes(_peak)`; T2, T2b, T4 |
| 4 | a full window stops the walk structurally | `checkpoint_submit_chunk` refuses (`shard.cpp:2002`); the visit loop stops on `blocked`; a blocked step retries the stored chunk before encoding (`checkpoint_step`); T1, T3, T11 |
| 5 | final partial chunk: not done, no DRAIN, IDs kept until it is submitted | `visited` / `blocked` states (`shard.cpp:2261-2283`); gauge `ckpt_final_pending`; T5 |
| 6 | a positive short write is fatal, like -errno | `shard.cpp:2070`; T6 |
| 7 | `RWF_DSYNC` wording | section 0 here; `docs/S3D_WRITEBACK.md` section 0 item 5 corrected |
| 8 | synchronous fallback counted and published; tested | `sri_write` returns false on the synchronous path; `checkpoint_issue` counts it; gauges `ckpt_sync_writes(_total)`, HEALTH `ckpt_sync_writes`; T10 |
| 9 | final `fdatasync` stays | unchanged: `shard.cpp:2299` |
| 10 | explicit code-level CC mapping | section 7 |
| 11 | C2 not built | not built |
| 13 | scope: dsync chunks, window, structural stop, buffer lifetime, `--ckpt-io legacy\|dsync`, observability, exact completion | sections 2-6 |
| 14 | W configurable and a hypothesis; tests at W = 1, 2, 4, stalled and out-of-order completion | `Config::ckpt_write_window` (default 4, 1..64), `--ckpt-window`; T1 (1, 2, 4), T2 (stalled), T4 (out of order) |
| 15 | 1 MiB chunks kept | `kCheckpointChunk` unchanged |
| 16 | tests S3-T1..T12 | section 8 |
| 17 | validation gate | section 9 |

## 2. As built: the write path

`Config::ckpt_io_dsync` (default true; `--ckpt-io dsync|legacy`) and
`Config::ckpt_write_window` (default 4; `--ckpt-window N`, 1..64; the
Engine refuses anything else) reach each shard as `Shard::CkptIo`.

**dsync.** `checkpoint_submit_chunk` (`shard.cpp:1999`):
1. refuses when `writes_inflight >= W`; the chunk being built stays where
   it is;
2. otherwise takes a free slot (or a new one; with dsync there are never
   more than W), moves the chunk's buffer into it by swap, records the
   length, chunk index and submit time, marks it busy, counts it in
   flight, advances the file offset;
3. issues `sri_write(fd, off, buf, len, dsync=true, checkpoint_write_done,
   slot)` (`checkpoint_issue`).

**legacy.** The same code with no window and `dsync=false`: every chunk is
submitted as soon as it fills, buffered, which is the I/O pattern before
S3. It shares the new ownership code, the exact-length check and the
per-completion free (review finding 5, accepted here): legacy is a rollback
of the *I/O behaviour*, not a copy of the old code. A defect in the new
ownership code would show in both modes; T9 and the full suite under
`SNC_CKPT_IO=legacy` cover legacy.

## 3. Buffer ownership and the memory bound

`Shard::CkptWrite` is the completion context: shard pointer, buffer,
submitted length, chunk index, submit time, busy flag. The shard owns the
slots (`Checkpoint::slots`, unique pointers, so addresses are stable); a
free list holds the idle ones.

- A slot is busy from submit until its own completion is processed.
  `checkpoint_write_complete` aborts if it ever sees a completion for an
  idle slot (a double completion).
- On completion the slot's buffer is released by swapping with an empty
  vector, the slot goes to the free list, and `writes_inflight` drops.
  Nothing else references the buffer: the kernel finished with it before
  the completion was posted, and the runtime only holds the slot pointer.
- Completions in any order release exactly their own slot. FIFO position
  is never used.
- `c = Checkpoint{}` (at begin and in `checkpoint_sync_done`) only runs
  with `writes_inflight == 0`, so it never frees a busy slot.

**The chunk being built** is reserved at `kCheckpointChunkReserve` (1 MiB
plus 64 KiB). A chunk is submitted once it reaches `kCheckpointChunk`, so
it holds at most `kCheckpointChunk - 1` bytes plus one record. A record
that does not fit grows the reservation to the exact size, never
geometrically, so no buffer exceeds `kCheckpointChunkMax` (1 MiB +
`kMaxPayload` + 4 KiB). Growth happens at most once per chunk: after it
the chunk is past `kCheckpointChunk` and is submitted.

**Bound** (retained chunk-buffer bytes, per shard):
- small records: (W + 1) x `kCheckpointChunkReserve`;
- in general: (W + 1) x `kCheckpointChunkMax` + `kCheckpointChunkReserve`.
  The last term is the moment of a growth copy, when the old and new
  buffers are both held; the peak gauge counts it (review finding 3).

The gauge `ckpt_buf_bytes` sums the buffers' own capacities (the building
chunk and every slot), so it cannot drift from what is held. T2 measured
the small-record bound exactly (3,342,336 = 3 x reserve at W = 2, part
51 MB); T2b, with 1 MiB objects ending chunks, 6.0 MB against a 7.4 MB
bound (part 23 MB).

## 4. The walk: structural stop and the final chunk

`checkpoint_step` (`shard.cpp:2180`):
1. Held test completions and deferred test submissions first (section 8).
2. `pace()` as in S2.
3. If a chunk is **blocked** (refused by a full window), it is retried
   before anything is encoded. Still refused: the step counts a
   window-full step and returns. The walk does not move.
4. Otherwise the S2 walk: batches until the budget, stopping early when a
   chunk fills and the window refuses it (`blocked`). The cursor has
   already passed the ID whose record filled the chunk, so nothing is
   visited twice or skipped (CC-1).
5. **visited** is set when the cursor reaches the snapshot's end, even if
   that last record also filled a refused chunk.
6. Once visited and not blocked, the last chunk is submitted. Refused:
   blocked, phase still WALK, IDs kept. Submitted: `walk_done`, the IDs
   are freed, DRAIN begins.

States, as published:

| State | `walk_done` | `visited` | `blocked` | Phase | Gauges |
|---|---|---|---|---|---|
| walking | no | no | no | WALK | |
| stopped by a full window | no | no | yes | WALK | `ckpt_window_full` 1 |
| every ID visited, last chunk waiting | no | yes | yes | WALK | `ckpt_window_full` 1, `ckpt_final_pending` 1 |
| all chunks submitted, completions pending | yes | | | DRAIN | `ckpt_writes_inflight` > 0 |

**Liveness.** `blocked` implies `writes_inflight >= W >= 1`: some write is
in flight and its completion will free a slot. An active checkpoint keeps
the loop polling (`Shard::poll` returns busy), so completions are reaped
and the walk resumes. Slow storage slows the walk; it cannot stop it
forever while the device completes writes.

## 5. Exact completion

`checkpoint_write_complete` treats `result < 0` and
`result != submitted length` alike: `storage_fatal` before the commit
(spec C7). The process aborts; no manifest or superblock names the
checkpoint, so the previous checkpoint and its retained WAL stay
authoritative (CC-5). T6 injects a one-byte-short write and an `-EIO`.

## 6. Observability

17 gauges (`include/snc/gauges.h`), per checkpoint unless noted:
`ckpt_io_dsync`, `ckpt_window` (configured; 0 = none, legacy),
`ckpt_window_full`, `ckpt_final_pending`, `ckpt_window_full_steps`,
`ckpt_chunks`, `ckpt_buf_bytes` (now; 0 when idle), `ckpt_buf_bytes_peak`,
`ckpt_write_lat_last_us`, `ckpt_write_lat_ewma_us`,
`ckpt_write_lat_max_us`, a four-bucket latency histogram (under 1 ms, 1-10,
10-100, 100 ms or more), `ckpt_sync_writes`, `ckpt_sync_writes_total`
(since start). The existing `ckpt_writes_inflight(_peak)` and
`ckpt_last_sync_us` cover writes in flight and the final sync. HEALTH
appends `ckpt_io=dsync|legacy ckpt_sync_writes=N` (sum over shards, since
start).

Chunk write latency runs from submit to the processing of the
completion, so it includes loop delay (and, in tests, hold time).

**Run validity (review section 8).** A performance or isolation run with
`ckpt_sync_writes_total` > 0 on any shard is invalid: a synchronous
durable write blocked the shard loop. The fallback happens when io_uring
is unavailable, when its submission queue stays full after a submit, or
when forced by `sri_debug_force_sync_io` (tests). `sri_write` now returns
whether it queued asynchronously (both runtimes).

## 7. CC-1..CC-5, code-level mapping

- **CC-1, visit set.** The snapshot is taken in one step as before
  (`begin_checkpoint`, `shard.cpp:1932-1937`). The cursor advances once
  per ID (`c.ids[c.cursor++]`, `shard.cpp:2211`) before the object is
  looked up; a window stop happens only after that ID's record is in the
  chunk; the retry encodes nothing (step 3 of section 4). Every snapshot
  ID is visited exactly once; which IDs are visited is unchanged.
- **CC-2, WAL retention and reclamation.** Not touched. `start_lsn` is
  taken at begin (`shard.cpp:1932`); reclamation still follows the commit
  (`engine.cpp` step 5). A slower, back-pressured walk only lengthens the
  retained WAL.
- **CC-3, local durability before the part sync.** Unchanged:
  `writes_inflight == 0 && lsn_local_durable(c.max_lsn)` before
  `sri_fdatasync` (`shard.cpp:2296-2299`). `RWF_DSYNC` makes chunks
  durable earlier; nothing reads the part before the commit, and the check
  still gates the final sync.
- **CC-4, versioned idempotent install.** The record encoding is
  unchanged (`encode_record`, `shard.cpp:2227`, same fields); the part
  format and the recovery parser are unchanged.
- **CC-5, commit ordering.** Unchanged: every part synced (`done.wait()`),
  `fsync_dir`, `write_manifest`, `write_superblock`
  (`engine.cpp:978-1024`). The final `fdatasync` is kept, so the part's
  size and metadata are durable before the manifest names it. Any chunk
  error is fatal before the commit (section 5).

## 8. Tests (`tests/test_s3_writeback.cpp`)

Test hooks (tests only; `Engine::debug_ckpt_*`): **hold** keeps chunk
completions unprocessed (in flight, buffers retained) until release tokens
let them through, oldest or newest first; **defer** keeps submitted
writes unissued (in flight, buffers owned) until tokens issue them;
**fail** turns one chunk completion into a one-byte-short write or
`-EIO`; **commit hold** stops `Engine::checkpoint` after every part is
synced and before the manifest; **force sync io** sends every write down
the synchronous path.

| Test | Work order | What it proves |
|---|---|---|
| T1 | S3-T1 | W = 1, 2, 4, completions held: exactly W writes submitted, the walk stopped (cursor still for 200 ms), window-full steps counted, buffers within (W+1) reservations |
| T3 | S3-T3 | one release: exactly one more chunk, the walk resumes and stops again (3 rounds, each W) |
| T2 | S3-T2 | 49-chunk part; a completion is let through only when the window is full (or the walk has ended), at most one per 10 ms: retained peak = 3 x reservation (W = 2), part 15x larger |
| T2b | S3-T2 | 1 MiB objects ending chunks: peak within (W+1) x max + reservation, and above the small-record envelope (the large case was exercised) |
| T4 | S3-T4 | submissions deferred and issued newest first, so each completion arrives while older slots own unissued buffers: a FIFO or wrong free issues freed memory. Bytes written = issued, part size exact, recovery exact |
| T5 | S3-T5 | W = 1: every ID visited and the last chunk waiting: phase WALK, `final_pending` 1, chunk count still for 200 ms; one release: the last chunk submitted, then DRAIN |
| T6 | S3-T6 | forked child: short write and `-EIO` on chunk 1 each abort (SIGABRT, message checked); recovery uses the previous checkpoint and the WAL exactly; a new checkpoint commits |
| T7 | S3-T7 | forked child exits with: chunks in flight; 2 complete and 4 in flight; all complete with the part waiting on WAL durability (DURABLE_WAIT, an applied but undurable deposit excluded); every part synced, before the manifest. Each recovers exactly from the previous checkpoint; a new checkpoint commits and recovers |
| T8 | S3-T8 | a dsync part commits: objects exact, bytes written = issued = size on disk, latency histogram sums to the chunk count |
| T9 | S3-T9 | legacy: no window, all chunks in flight at once, exact |
| T10 | S3-T10 | forced synchronous path: every chunk write counted, exact; with io_uring and no force, none |
| T11 | S3-T11 | window full, walk stopped: 500 durable deposits and 500 reads complete; the walk stays stopped |
| T12 | S3-T12 | the rest of ctest, with dsync the default; also the whole suite under `SNC_CKPT_IO=legacy` (section 9) |
| T13 | | window 0 and 65 refused, 1 and 64 accepted |

**T7's limit.** The child exits the process; it does not cut power. Bytes
the kernel accepted survive a process exit, so "some durable, some not"
is the shard's view of its completions, not a device state. A power-loss
variant (fault injection below the filesystem) is not built.

**Negative mutations** (`bench/results/s3-validation-devvm-20261108/negative/`):
each must fail the named test: no window (T1), advisory window that keeps
encoding (T1), no buffer release (T2), FIFO free (T4), walk done with the
last chunk unsubmitted (T5), short write accepted (T6), synchronous
fallback uncounted (T10).

## 9. Validation, deviations, open items

Validation gate (review section 17): full serial Release, ASan+UBSan and
TSan suites with zero sanitizer reports, the S3 tests, the negative
mutations, an independent source review, the mapping above, no R640 load.
Evidence: `bench/results/s3-validation-devvm-20261108/README.md`.

`SNC_CKPT_IO=legacy|dsync` (Engine constructor) overrides the config's
write path for whole-suite validation runs; it is announced on stderr
whenever it applies, and `test_s3_writeback` ignores it, since each of
its tests sets its own path. It also applies to `snc_server`, which the
Python suites start.

Independent review (no blockers) and what was done:

| Finding | Resolution |
|---|---|
| T4 could not detect a wrong free (the hold only delays processing) | deferred-submission hook; T4 issues newest first while older slots own unissued buffers; FIFO-free mutation added |
| T7 is not a power loss | stated in the test header and section 8 |
| transient double buffer during a growth copy not in the peak | counted in the peak; bound and T2b updated |
| `SNC_CKPT_IO` breaks test_s3_writeback | the test unsets it |
| legacy shares the new ownership code | accepted and documented (section 2) |
| shard teardown with writes in flight frees slots under the kernel | *Corrected in S3.1 (section 10): this was inferred, not traced, and the "caller error" call was rejected by the review. The use-after-free seen was a different defect (exit freeing the static engine state); shard teardown already waited for checkpoint I/O; stop during `checkpoint()` abandoned its waiter.* |
| T5 could miss a last record that exactly fills a refused chunk | `visited` is set at the end even when blocked, so the state reads as final pending |
| fallback test covers only the forced path; the synchronous final `fdatasync` is not counted | noted; the SQ-full fallback is effectively unreachable (W <= 64, ring 256) |
| window-full steps undercounted by one per stop | counted on the first stop too |
| T1 resume compared with the first cursor | compares with the previous one |

Found in validation, not by the review: the first serial ASan suite failed
in T2 (`writes_inflight_peak == W`), because the test released one
completion every 10 ms and assumed the walk filled a chunk faster, which
ASan breaks. T2 and T2b now release only once the window gauge reads full
(or the walk has ended). The CHECK failure then exited with the engine
running and hung on the teardown defect (section 10), so `test_s3_writeback`
now has a ctest timeout (a CI guard, not a fix). Both are test changes.

Open items:
- the teardown item above: fixed in S3.1 (section 10);
- W, and chunk size, are dev-VM hypotheses (review sections 14, 15);
  physical characterization of C3 against C2 on the R640's SAS SSD comes
  before any R640 campaign (section 18);
- a power-loss test of partially durable parts.

## 10. S3.1: engine lifecycle under checkpoint I/O

Work order: `docs/handoff/PR77_S3_Review.txt` sections 9-14, 18.

### 10.1 What the use-after-free was (traced, ASan, symbolized)

Reproduced by exiting a test process (`std::exit`) with an Engine running
and chunk writes held. The read was `Shard::publish_gauges` on a shard
thread (`shard.cpp`, from `Shard::poll`); the free was
`EngineState::~EngineState`, run by the static destructor of the file
static `g_state` in `engine.cpp`. So the engine's own state (gauge blocks,
hooks, final stats) was owned by a static and freed at exit under live
shard threads. It was not the checkpoint slots: the PR #77 note that blamed
"shard teardown with writes in flight" was an inference, and wrong.

Traced separately, a real checkpoint lifecycle defect: `stop()` from
another thread while a thread waits in `Engine::checkpoint()`. The shards
quiesce only once their checkpoint is over (below), so the part completes;
then `stop()` finishes stopping the runtime, and the waiting
`checkpoint()` continues into the commit, whose `run_on_all` is submitted
to stopped workers and never returns: an abandoned waiter (reproduced;
the stop returned and the checkpoint thread hung).

### 10.2 The contract, and how it is enforced

**Checkpoint finishes, then stop proceeds** (review section 11, first
option).

1. `Engine::checkpoint()` holds `lifecycle_mu_` for its whole run, through
   the manifest and superblock. `Engine::stop()` takes it first. A stop
   (or `~Engine`, which stops first) therefore waits for a checkpoint in
   progress to commit; a storage error during it is still `storage_fatal`
   (abort, no destructors). A `checkpoint()` that gets the lock after a
   stop finds `running_` false and starts nothing (report seq 0). No
   sleep or timeout is part of it.
2. Shard state is destroyed only after quiescence (pre-existing,
   confirmed for both runtimes): a worker leaves its loop only when
   `Shard::quiesced()` (which requires `!ckpt_.active`), its runtime
   `io_inflight == 0` and its `io_deferred` queue empty; only then does
   `hook_fini` delete the shard. `ckpt_.active` stays true until the final
   `fdatasync`'s completion is processed; the sync is issued only with no
   chunk write in flight; the `CkptWrite` slots are freed (`c =
   Checkpoint{}`) only in that completion. So when a shard is deleted: no
   busy `CkptWrite`, no `IoOp` targets it, no deferred completion points
   into it, no final sync is outstanding (review sections 10, 12). The
   synchronous fallback queues its completion on `io_deferred`, covered
   the same way. Test-hook queues (held and deferred writes, the held
   sync) all keep the checkpoint active.
3. `EngineState` is owned by the Engine (`Engine::state_`), not a static.
   Shard threads reach it only through the hooks pointer set in the
   constructor; Engine members use `state_`. The engine frees nothing of
   its own at process exit.

`running_` is now atomic. Supported lifecycle: one controlling thread
calls `start()`, then any thread may call `checkpoint()`, and `stop()`
(then the destructor) ends it. Calling into an Engine once its destructor
has begun is not supported, as for any C++ object. `std::exit` with an
Engine running is not a supported shutdown: other static destructors
(type registry, allocator) still run under live threads.

### 10.3 Tests (S3-T14..T18)

| Test | Review | Shows |
|---|---|---|
| T14 (+T17) | 13 T14, T17 | `stop()` from another thread with W chunk writes held and a thread in `checkpoint()`: for 300 ms stop has not returned, the checkpoint has not returned, the shard still publishes gauges with W in flight (not destroyed). Released: the waiter returns (not abandoned), the checkpoint committed (recovery uses it), stop returns. `checkpoint()` after stop: seq 0 |
| T15 | 13 T15 | the same with the final `fdatasync` completion held (`debug_ckpt_hold_sync`): phase SYNC throughout, then commit and a clean stop |
| T16 | 13 T16 | the owning thread `stop()`s and deletes the Engine while the checkpoint thread is mid-checkpoint: both wait, then the checkpoint commits and the Engine is destroyed; recovery exact |
| T16b | 9 | fork child: `std::exit` with the engine running and writes held exits with its own status and no sanitizer report (the PR #77 use-after-free, now absent) |
| T18 | 13 T18 | fork child: `-EIO` on chunk 0 while chunk 1 is in flight: abort with only the storage report, no sanitizer report; recovery from the previous checkpoint exact |

The 300 ms probes are evidence that something did *not* happen yet, not
a safety mechanism. After release, each wait is bounded (60 s) so a
lifecycle defect fails a CHECK instead of hanging.

Negative mutations (`bench/results/s31-validation-devvm-20261108/negative/`):
stop without the lock (T14: the waiter is abandoned), the same through the
destructor (T16), `checkpoint()` without the lock (T15), the engine state
back on a static owner (T16b under ASan: the use-after-free returns), and
stop without the lock plus a quiesce that ignores the checkpoint (T14:
stop returns with the checkpoint unfinished, the shard destroyed under
it). The last one is the only way to exercise the runtime drain through
the Engine, since the lock otherwise keeps `rt_->stop()` from running
during a checkpoint.

### 10.4 Independent review (lifetime), and what was done

| Finding | Resolution |
|---|---|
| the runtime drain was untested (the lock hides it) | mutation 5 added |
| Engine members read the global `g_state`, not their own state; with two Engines alive (recovery checks) one could read the other's or null | the global is removed; members use `state_` |
| T16 called `checkpoint()` on an Engine whose destructor had begun (formally outside its lifetime) | T16 now stops, then deletes; destruction during a call is documented as unsupported |
| T16b proves only that the engine's own state is not freed; other statics still run at exit | stated in the test and 10.2 |
| other Engine methods that use `run_on_all`/`submit` (`stats`, `replication_status`, slot report, `update_placement`, `promote`, `release_emits`/`wait_emits`, `on_shard*`) are not serialized with `stop()`: a call racing stop can wait forever on a stopped worker | out of the checkpoint scope of S3.1; open item (below) |
| `start()` is not serialized with `stop()`/`checkpoint()` | documented contract: one controlling thread starts the engine before others use it; open item |
| post-`fini` drain would call into a deleted shard if `~Shard` ever issued I/O | today `~Shard` only closes fds; noted |
| `stop()` waits as long as the checkpoint takes (a hung device blocks it) | by design under the no-timeout rule |

Open items (S3.1):
- a general lifecycle guard for every Engine entry point that submits to
  the shards, not only `checkpoint()` (and `start()`), for review;
- the rest of section 9's open items.
