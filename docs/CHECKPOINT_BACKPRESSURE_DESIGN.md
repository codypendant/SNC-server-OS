# Checkpoint and backpressure architecture: design (for review)

*Status: **DESIGN ONLY, PROPOSED.** Requested by
`docs/handoff/PR66_Checkpoint_Design_Review.txt`. No engine code changes
until this design is reviewed. No CP-1A rerun, competitors or Profile C.
`r640-diag-telemetry` stays off main.*

Labels:

| Label | Meaning |
|---|---|
| **AS-BUILT** | read from code at `e903013` (main; line numbers are main's, not the diag branch's), with `file:line` |
| **MEASURED** | from the R640 evidence (`r640-diag-telemetry`: diag2 `DIAGNOSIS.md`; diag3 `run/` c88cb70, `CAUSAL_REPORT.md` c9537dd) |
| **INFERRED** | reasoned, not measured |
| **PROPOSED** | this design |

## 0. Summary

The checkpoint causes two bottlenecks:
1. **The walk** takes shard CPU in fixed slices that ignore load.
2. **The storage burst** leaves up to about 1 GB of dirty page cache across
   8 shards, then all 8 shards force it out with `fdatasync` at the same
   moment, on the WAL's device. WAL flushes slow from about 0.05 ms to
   about 12 ms. Durable throughput collapses. Follow-ups pile up in
   `emit_wait_`, and the per-shard gate then refuses **every** client op,
   including reads.

The preferred design keeps the snapshot and recovery semantics exactly as
they are. It changes only *when* and *how fast* checkpoint work happens,
and *what the overload gate refuses*:

- **P1, admission by resource.** Reads are no longer refused by the
  follow-up backlog. They get their own, bounded resource: deferred
  durability waiters. Mutations keep the follow-up gate (`kEmitBacklog`
  stays 1,024).
- **P2, paced walk.** Each loop iteration's walk work gets a time budget
  that shrinks as foreground pressure rises. A completion floor guarantees
  the checkpoint still finishes within a deadline.
- **P3, paced, streamed checkpoint I/O.** Chunks are written back as
  they are produced, with a bounded number in flight, instead of building
  up dirty cache for a final flush. Issuance follows an AIMD window driven
  by SNC's own pressure signals (WAL flush latency, follow-up backlog,
  waiters), not a fixed MB/s.
- **P4, one-at-a-time final sync.** A node-level token lets one shard at
  a time issue its (now small) final `fdatasync`.
- **P5, I/O priority hint, optional.** Checkpoint direct writes carry a
  lower I/O priority. Kept only if measurement shows it helps.

Incremental and copy-on-write checkpoints (F) are the long-term
direction, not the next step.

## 1. Current snapshot and recovery semantics (AS-BUILT)

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
2. Each shard loads its own part with `install` (version rule: skip if the
   current version is greater or equal, `shard.cpp:1220`). Installed
   objects get `last_lsn = 0`, meaning durable.
3. Each shard replays its own WAL stream from `start_lsn[s]` with the same
   version rule. DELETE erases only an older version (`shard.cpp:1213-1218`).
4. Index rebuild, new segments, then step 8 if needed.

### 1.3 The invariants that make it correct

These are the invariants any change must keep (INFERRED from the code
above; the proof in section 5 uses them):

- **I1 (coverage).** Every object that is live at `begin` and **not
  mutated** in `[start_lsn, crash]` is in the part, with its image as of
  before `begin`. Today this holds because the ID snapshot is taken in one
  step and every untouched snapshot ID is still found by `map_.find`.
- **I2 (redo completeness).** Every mutation at LSN ≥ `start_lsn` is in the
  WAL as a **full post-op image** (CREATE/UPDATE) or a DELETE, and the
  stream is kept from `start_lsn` until a later checkpoint commits.
- **I3 (no future state, C5).** No image in a part has a `last_lsn` above
  what was locally durable before the part's `fdatasync`. So a committed
  checkpoint never holds a write that the durable WAL prefix lacks.
- **I4 (idempotent redo).** Installing a part image and then replaying any
  record with version ≤ that image's version is a no-op. Replaying a newer
  one replaces it.
- **I5 (atomic commit).** No reader of the data directory looks at
  `ckpt/<seq>` until a superblock names it. Nothing that the previous
  checkpoint and the WAL need is deleted before that superblock is durable.

Two consequences:
- **Parts may become durable at different times, safely.** Nothing reads
  a part before the commit (I5). A crash before the commit recovers from
  the previous checkpoint plus a longer WAL replay.
- **The checkpoint is not a single instant across shards.** Shard s
  represents "WAL prefix of stream s, from `start_lsn[s]`". Cross-shard
  consistency comes from each stream's durable prefix plus follow-up
  re-emission, exactly as after any crash. The checkpoint adds no
  cross-shard guarantee and needs none.

## 2. The two measured bottlenecks

| | Walk (CPU) | Storage burst (I/O) |
|---|---|---|
| What | 625,000 map lookups, IMAGE encodes and page-cache copies per shard; one 50 ms ID snapshot at `begin` | about 1 GB of dirty pages across 8 shards (INFERRED: about 1,090 MB/s × about 1.1 s), forced out by 8 `fdatasync`s that start within 1 ms of each other |
| MEASURED | walk 1.17-1.20 s at 436,646, 1.69-1.71 s at 2,183,233. Closed loop: polls -58%, throughput about -55% during the walk | `fdatasync` 1.00-1.26 s. WAL write await 0.048-0.065 ms → 10.7-12.1 ms. Durable capacity about 0.35-0.57M ops/s |
| Refusals at 436,646 | **none** in the walk | **all** of them (2,267 / 6,488 / 9,455) |
| Refusals at 2,183,233 | 17k-126k | most of the rest |
| Why it bites | slices are fixed at 256 objects per iteration and ignore load | one WAL flush in flight per shard (`storage.h` "at most one flush in flight"), each about 200x slower, so each shard's durable rate collapses; cross-shard follow-ups wait in `emit_wait_` (`shard.cpp:1787-1800`) |
| Gate | `emits_in_flight() > 1024`, checked **before** the op is looked up (`shard.cpp:110`), so reads are refused too | same |

A third cost is the O(map) `begin` snapshot (about 50 ms per shard,
MEASURED), together with its sibling O(map) tasks (HEALTH_DECOUPLING W1).

## 3. Options A to F

Each option is scored on what it fixes, its correctness risk, its cost
and its evidence need.

### A. Read and mutation admission separation

**What.** Decide admission per operation class and per resource, instead
of one gate for everything.

| Class | Resources it consumes | Gate today | PROPOSED gate |
|---|---|---|---|
| Pure READ, owner shard, object durable | CPU only; answered at once | follow-up backlog | none beyond the ring and connection |
| READ of an object whose last write is not yet durable | one **durability waiter** (`waiters_`, `shard.cpp:562-570`) until the WAL catches up | follow-up backlog | **waiter budget** per shard (new; today `waiters_` is unbounded on the server side, and the connection stops reading only at 8 MiB of unsent output, `conn_set.cpp:20,103`) |
| READ with `allow_undurable` | CPU only | follow-up backlog | none |
| Forwarded READ (non-owner) | SRI ring slot, pending slot | follow-up backlog, then ring | ring and pending only (`sri_send` already fails with E_OVERLOADED) |
| INDEX lookup and gather | gather slot, SRI ring and overflow to every shard (`shard.cpp:785-813`) | ring/overflow (separate code path) | unchanged: own ring and gather budget |
| MUTATE (incl. CREATE, sessions) | WAL buffer, a waiter, possibly follow-ups | follow-up backlog | follow-up backlog (**unchanged**, `kEmitBacklog` = 1,024) **plus** a WAL-buffer budget (new; `ensure_capacity` grows without bound today, `storage.cpp:566-575`) |
| Resent sequenced MUTATE already applied | a session read | special-cased (`shard.cpp:111-122`) | unchanged |

**Correctness.**
- A read never writes the WAL. It does not move `durable_lsn` or emit a
  follow-up, so it cannot delay backlog recovery through state.
- It still answers only once the object's last write is durable
  (`out.wait_lsn`, `shard.cpp:364`). The durability contract is unchanged.
- A standby's read lease rules are untouched.

**Memory.** Bounded by the new waiter budget, the existing rings and the
new WAL-buffer budget. Today two of these are unbounded on the server side
(INFERRED). Bounding them is a hardening in its own right.

**Fairness and starvation.**
- Follow-ups already run first in each shard poll (`run_emits`,
  `shard.cpp:609`). Exactly how polls interleave with request delivery in
  the runtime loop is to be confirmed in S1 (INFERRED).
- Reads compete only for CPU. Two rules keep reads from taking the CPU
  that recovery needs while the backlog is over half full:
  - a per-iteration **read quantum**, a count;
  - an admission order that services follow-ups and WAL completions first.
- Mutations are never starved by reads either. They are refused only by
  their own backlog gate, and that gate drains on durability, which reads
  do not consume.

**Recovery delay.** Reads do not touch WAL or follow-ups, so they cannot
lengthen the time the backlog takes to drain, apart from the CPU share the
quantum bounds.

**Risk.**
- Low for durability.
- Medium for client behaviour: E_OVERLOADED on reads becomes rarer and
  comes from a different limit.

**Fixes:**
- reads refused during stalls (design gap 1);
- adds two missing memory bounds.

**Doesn't fix:** the stall itself.

### B. Paced, incremental checkpoint walk

**What.** The walk's work per iteration is bounded by **time**, not a
fixed count, and adapts to load. A completion floor still guarantees it
finishes.
- Budget per iteration: `walk_budget_ns = clamp(base × (1 − pressure),
  floor_ns, max_ns)` (section 7).
- Objects are processed until the budget is spent, checking the clock
  every 32 objects.
- The `begin` ID snapshot becomes incremental too (B2 below).

**Snapshot consistency.** I1 is the only invariant at risk.
- **B1 (keep the one-step ID snapshot, pace only the walk).** I1 holds
  unchanged. The snapshot is taken in one step, and a paced walk visits
  the same IDs later. Concurrent mutation is already handled: a visited
  object shows its current image, and I2/I4 cover later writes.
  **Zero semantic change.** It still pays the 50 ms snapshot.
- **B2 (incremental snapshot).** Walking the hash table directly over
  several iterations can **miss untouched objects**:
  - backward-shift deletion moves entries from behind the cursor to in
    front of it (`local_map.h` header);
  - growth rehashes everything.

  A missed untouched object breaks I1, which means silent data loss after
  a crash. B2 needs one of these:
  - (i) a **per-object checkpoint mark**, a parity bit flipped per seq.
    The walk restarts from slot 0 after any structural change and skips
    marked objects. Correct, with a worst case of about 2× work per
    restart;
  - (ii) a **carry hook** in `LocalMap`: every backward shift that moves
    an entry across the cursor puts that object on a carry list, which
    the walk visits. Growth during a checkpoint is handled as in (i).

  Prefer (i): simpler to prove, and growth is rare after `reserve`.

**Completion bound.**
- `floor_ns` per iteration × iterations per second ≥ objects /
  (deadline × safety).
- Deadline: half the checkpoint interval (default), at most
  `max_checkpoint_s`.
- If the floor cannot meet the deadline, the walk raises its own budget
  ("catch-up" mode) and counts `ckpt_deadline_pressure`. Completion is
  never traded away, because WAL reclamation depends on it.

**CPU overhead.** The same total work, spread out, plus clock reads,
about 20 ns per 32 objects (INFERRED).

**Risk:**
- B1: low;
- B2: medium (I1 needs its own tests, section 8).

### C. Paced checkpoint writes

**What.** Stop building up dirty page cache for a final flush. Each chunk
goes to the device as it is produced, with bounded bytes in flight.

Two mechanisms:
- **C1, buffered write plus `sync_file_range(WRITE)` per chunk**
  (io_uring `IORING_OP_SYNC_FILE_RANGE`). Writeback starts early and
  stays bounded. Small code change. The final `fdatasync` only flushes
  metadata and the device cache.
- **C2, `O_DIRECT` checkpoint writes** of aligned 1 MiB chunks (the WAL
  already uses `O_DIRECT`). No page cache. Each write completes on the
  device. Byte-exact in-flight control, and it carries an I/O priority
  (E). Needs aligned buffers and padding to the last block; the part
  header already reserves a block.
  - Note: the WAL is **not** `O_DIRECT` under `snc_server` today.
    `wal_direct` defaults to false (`snc.h:202`) and `snc_server` never
    sets it. So the WAL uses buffered writes with `RWF_DSYNC`
    (AS-BUILT). The diag2 report's "O_DIRECT" should be checked against
    the run's STATS `wal_direct`.

Prefer C2: deterministic pacing, no interaction with kernel writeback
thresholds, and I/O priority works only on direct I/O (buffered writeback
is issued by kworkers).

**Pacing.**
- An **AIMD window** of checkpoint writes in flight per shard: start at 1
  chunk, at most 4.
- Issuance runs from the shard loop, so the walk produces chunks only as
  fast as the window drains. This back-pressures the walk too.
- The control signals are SNC's own (section 7), not a MB/s number.

**Correctness.**
- I/O ordering is not a correctness issue. The part is inert until the
  commit (I5), and C5's check (`max_lsn` locally durable) still runs
  before the final sync.
- `O_DIRECT` changes neither content nor the commit.

**Risk:** low to medium (alignment, a partial last block, error paths).

### D. Staggered shard sync

**What.** Shards don't all issue their final `fdatasync` together.
- A node-level token (`std::atomic<u32>` in the Engine) admits K shards
  (default 1) to the sync step.
- The others hold until the token is free. "Hold" is a state in
  `checkpoint_step`, so the thread never blocks.
- Alternatively, start shards' walks at staggered offsets.

**Atomicity and crashes.** Unchanged: commit only after all parts report
(I5). A crash during staggered syncs is the same as a crash during today's
syncs: the previous checkpoint plus a longer replay.

**Total duration.** With C2 the final sync is mostly a cache flush,
INFERRED to be milliseconds. Staggering 8 of them adds little. Without C2,
staggering alone serializes about 1 GB of flush per checkpoint: it
lengthens the checkpoint about 8× and only lowers the peak.

**Risk:** low. Alone it is insufficient (INFERRED).

### E. I/O prioritization and isolation (one device)

**What.** Mark checkpoint direct writes with a lower I/O priority class
(io_uring `sqe->ioprio`, `IOPRIO_CLASS_BE` level 7, or IDLE). The WAL stays
at the default.

**Notes.**
- The R640 uses `mq-deadline`, which honours priority classes for queued
  requests (INFERRED from kernel behaviour, to be MEASURED). It cannot
  reorder what the device has already accepted, so host-side in-flight
  limits (C) do most of the work.
- Buffered writeback ignores the submitter's priority, so E needs C2.
  How the WAL's own buffered `RWF_DSYNC` writes are classed is to be
  MEASURED.
- IDLE risks starving the checkpoint under constant WAL load. Use BE-7,
  with the completion floor (B) as a backstop.

**Risk:** low. Value uncertain until measured. The design must not depend
on it. A second physical device is not required.

### F. Incremental or copy-on-write checkpoints (long term)

**What.** Stop writing the whole state each time.
- (F1) **Delta checkpoints:** write only objects whose version changed
  since the last checkpoint, plus a periodic full one. Recovery loads base
  plus deltas, then the WAL.
- (F2) **Log compaction:** the WAL is the only durable format, and
  checkpoints compact older segments in the background.
- (F3) **COW snapshot** via page-level copy on write.

**Pros.** Writes scale with change, not size. No periodic full walk.

**Cons.**
- A new on-disk format and recovery path.
- Chain management for WAL reclamation, seeds (which ship the latest
  checkpoint) and standbys.
- Compaction scheduling becomes its own background-interference problem.
- Much larger proof and test burden.

Revisit after the near-term pacing is measured. If paced full checkpoints
meet the regression at the campaign's object counts, F is deferred.

### Comparison

| | Fixes reads refused | Fixes the I/O burst | Fixes walk CPU | Semantic risk | Code size | Measure first? |
|---|---|---|---|---|---|---|
| A | **yes** | no | no | low | small | no |
| B1 | no | no | **yes** | none | small | yes |
| B2 | no | no | yes (+ `begin`) | medium | medium | yes |
| C2 | no | **yes** | partly (backpressure) | low | medium | yes |
| D | no | partly | no | low | small | yes |
| E | no | maybe | no | none | tiny | yes |
| F | no | yes | yes | high | large | long term |

## 4. Preferred design (PROPOSED)

The preferred design is **A + B1 + C2 + D, with E as a switch; B2 later;
F deferred.**

```
shard loop iteration:
  run_emits()                       follow-ups first (unchanged)
  links / WAL poll / replication    (unchanged)
  checkpoint_step(budget)           P2: walk until walk_budget_ns spent, or
                                    the write window is full
                                    P3: issue O_DIRECT chunk writes while
                                    inflight < window
                                    P4: walk done, writes done, max_lsn
                                    locally durable -> take the sync token
                                    -> fdatasync -> release the token -> report
  admission (execute):              P1: classify op -> its own resource gate
```

**Unchanged:**
- the checkpoint content;
- `start_lsn`, the manifest and the superblock commit;
- reclamation, recovery and `install`;
- C5;
- `kEmitBacklog`;
- the Engine API.

**What changes is behaviour:**
- when bytes are produced and issued;
- the sync order;
- which gate refuses which op.

New configuration knobs (all with defaults):
- `ckpt_deadline_fraction` (0.5);
- `ckpt_window_max` (4 chunks);
- `ckpt_sync_tokens` (1);
- `ckpt_ioprio` (off at first);
- `waiter_budget` per shard;
- `wal_buffer_budget` per shard;
- `pressure targets` (section 7).

## 5. Conceptual durability and recovery proof

**Claim.** For every shard s, the state recovered after any crash equals
the state produced by applying stream s's durable WAL prefix, as today.
No acknowledged write is lost or reordered.

**Proof sketch.** Under the preferred design, I1 to I5 still hold:

- **I1 (coverage).**
  - B1 keeps the one-step ID snapshot.
  - The walk visits exactly the snapshot IDs and skips only those absent
    or deleted at visit time. Their DELETE is at LSN ≥ `start_lsn` and is
    replayed by I2.
  - Pacing changes visit *times*, not the visit *set*.
  - Every untouched object therefore keeps its pre-`begin` image when
    visited.
- **I2 (redo completeness).**
  - The WAL format and logging are unchanged.
  - Reclamation still runs only after the commit (I5).
  - A slower checkpoint only keeps segments longer, so the disk budget
    becomes a bound to watch (section 6).
- **I3 (no future state).**
  - The C5 check (`max_lsn` locally durable) still gates the sync step.
  - With D it runs before taking the token, and again after taking it.
  - `max_lsn` only grows with visits, and the walk is finished by then.
- **I4 (idempotent redo).** Unchanged.
- **I5 (atomic commit).**
  - Parts are still not referenced until the manifest and superblock.
  - `O_DIRECT` and staggering change only when part bytes reach the
    device.
  - The commit still waits for every shard's sync completion.

**Admission (A).**
- A read admitted under backlog returns either:
  - a durable value at once; or
  - a deferred answer released only when `durable(last_lsn)` holds.
- So an answer never shows state that a crash could undo. That is the
  existing rule (`shard.cpp:364`, `release_waiters`).
- Refusing a read leaves no state behind.
- Mutation admission is unchanged.

**Crash points** (each recovers by I5 to the previous checkpoint plus
replay):
1. during the walk;
2. with chunk writes in flight (some on disk, some not);
3. a shard waiting for the sync token;
4. some shards synced, others not;
5. all synced, before the manifest;
6. after the manifest, before the superblock;
7. after the superblock, before reclamation (next start sees the new
   checkpoint; old files are only garbage).

Point 7 is covered today by the generation-alternating superblock and
"delete only after commit".

**B2 (later).** It needs its own proof of I1 under structural change, via
the mark bit and restart rule, and it is not part of the first slices.

## 6. Admission and backpressure resource boundaries (PROPOSED)

| Resource (per shard) | Bound | Refused with | Who is refused | Drains by |
|---|---|---|---|---|
| Follow-up backlog `emits_in_flight` | 1,024 (**unchanged**) | E_OVERLOADED | new MUTATE/CREATE only | durability, rings |
| Durability waiters `waiters_` | `waiter_budget` (proposed 16,384; sized from memory per waiter × shards; to be MEASURED) | E_OVERLOADED | ops that would defer (reads of undurable objects, mutations) | `release_waiters` |
| WAL active buffer | `wal_buffer_budget` (proposed 64 MiB) | E_OVERLOADED | MUTATE/CREATE | WAL flush completion |
| SRI ring and overflow | existing 1,024 + 1,024 per destination | E_OVERLOADED | forwards, gathers, follow-ups wait | the peer shard |
| Gather slots | existing pool | E_OVERLOADED | INDEX lookups | completion |
| Read CPU quantum while backlog > 50% | proposed 4,096 reads per iteration | deferred to the next iteration (not refused) | reads | each loop iteration |
| Checkpoint write window | 1..`ckpt_window_max` chunks | n/a (internal) | checkpoint | device completions |
| WAL disk retained by a long checkpoint | existing segment budget; a checkpoint past its deadline raises the walk budget (B) | n/a | checkpoint | commit |

All of these are counted. STATS gains `refused_by` per resource, so a
refusal names its cause; the diag branch's gauges show why that matters.
The telemetry branch stays off main: the counters above are added as
ordinary STATS fields, which is a separate, smaller proposal (slice S0).

## 7. Adaptive pacing: signals and control

Signals are per shard and all owned by SNC. No hardware number is used.

| Signal | Source | Pressure when |
|---|---|---|
| WAL flush latency | `WalStream` per-flush submit-to-completion, already measured into `flush_us_log2` (`storage.h`) | EWMA > `max(4 × idle_baseline, 1 ms)` |
| Follow-up backlog | `emits_in_flight / kEmitBacklog` | > 0.25 |
| Waiters | `waiters_.size() / waiter_budget` | > 0.25 |
| Undurable records | `next_lsn − durable_lsn` | > 2 × records per flush at idle |
| Checkpoint completion latency | its own chunk writes | > `max(4 × baseline, 2 ms)` |

`idle_baseline` is the WAL flush latency EWMA measured while no
checkpoint is active, so it is learned per device.

**Control.**
- `pressure = max(normalized signals)`, in [0, 1].
- **Window (AIMD):** while `pressure > 0.5`, halve the window (minimum 1);
  while `pressure < 0.25`, add 1 per completed chunk (maximum
  `ckpt_window_max`). In between, hold.
- **Walk budget:** `walk_budget_ns = floor_ns + (max_ns − floor_ns) ×
  (1 − pressure)`. Proposed `floor_ns` 20 µs and `max_ns` 200 µs per
  iteration (to be MEASURED).
- **Deadline floor:** if remaining objects ÷ remaining time to the deadline
  exceeds what the floor delivers, raise the floor, and the window minimum
  to 1. The checkpoint always finishes. "Past deadline" is counted.

Every shard runs its own controller against the same device. AIMD on
shared latency converges towards fair shares (INFERRED). The sync token
(D) removes the remaining synchronised step.

## 8. Deterministic correctness and crash tests (PROPOSED)

The in-process tests extend `tests/test_persistence.cpp` and
`test_crash.cpp`. Fault points use a test-only hook,
`SNC_TEST_CKPT_FAULT=<phase>:<n>`, compiled into test builds only. Every
test runs in Release, ASan+UBSan and TSan.

| # | Test | What it proves |
|---|---|---|
| T1 | **Concurrent-mutation consistency.** Mutate, create and delete a known set *during* a paced checkpoint (walk budget forced tiny, so the walk spans thousands of iterations). Crash, recover, compare with a shadow model of the durable prefix | I1-I4 under pacing |
| T2 | **Crash at every phase.** Fault at crash points 1-7 (section 5), on each shard, with K = 1 and K = 8 tokens. Every acknowledged write must survive | I5 |
| T3 | **Partial or staggered parts.** Some parts synced, others half-written, then a crash: recovery uses the previous checkpoint. Corrupt a half-written uncommitted part: still ignored | I5, "parts inert until commit" |
| T4 | **Replay from a long WAL.** The checkpoint is slowed past its deadline. Reclamation must not delete anything still needed. Recovery after the next commit is exact | I2, reclamation |
| T5 | **Reads under mutation backpressure.** Hold durability (WAL fault: delay completions) until the backlog exceeds 1,024. Durable reads of untouched objects still answer, undurable ones defer, and none is answered before durable. MUTATE gets E_OVERLOADED | A |
| T6 | **Waiter and WAL-buffer budgets.** Exceed each budget: E_OVERLOADED with the right `refused_by`, no growth beyond it, and recovery after release | section 6 |
| T7 | **Bounded walk CPU quantum.** Instrument `checkpoint_step` time per iteration: max ≤ `max_ns` + one object batch, except in catch-up mode | B |
| T8 | **Bounded I/O issuance.** Checkpoint writes in flight never exceed the window, and the window halves under injected WAL latency | C, section 7 |
| T9 | **Backpressure recovery.** After the fault is released, the backlog returns below 25% within a bound, and no op is lost or answered twice (sequenced resends) | A, sessions |
| T10 | **No starvation.** A read-only flood during a mutation backlog: the backlog still drains (follow-ups first), and mutations are admitted again | A fairness |
| T11 | **Multi-shard completion.** 8 shards with token K = 1: every shard completes, total duration is within the deadline, and the commit happens once | D, completion |
| T12 | **Unchanged durability and profile semantics.** The existing suites pass unchanged: persistence, crash, replication (sync and async), migration with checkpoint (`q10_*`), B1 (`q3_runtime`), the profile guard, sessions | regression |
| T13 | **B2 only, later.** Deletes and growth during an incremental walk, with an untouched-object coverage check after a crash | I1 for B2 |

## 9. Physical R640 regression plan

The preregistered regression uses the diag3 harness and isolated scratch,
on a new frozen SHA.

**Primary cell.** 436,646 ops/s open loop, Profile A, `--checkpoint-s 60`,
60 s window, 3 reps. Today it refuses in 3/3.

**Comparators, same SHA:**
- 436,646 with no checkpoint, 3 reps (today 0/3);
- 2,183,233 w60 with checkpoint, 3 reps (measured, no pass/fail set).

**Pass (proposed, for review)** requires all of the following in the
primary cell, in every rep:
- 0 refusals;
- `late_over_1ms` ≤ 0.1% of `scheduled_in_window`;
- total p99 ≤ 1.5× the no-checkpoint p99 on the same SHA;
- p99.9 ≤ 10 ms;
- WAL flush latency EWMA max ≤ the pressure target;
- follow-up backlog max ≤ 50% of 1,024;
- the checkpoint finishes within the deadline;
- closed-loop capacity with a checkpoint ≥ 0.95× the current build's
  (no capacity regression).

**Also recorded:**
- checkpoint duration;
- per-phase timeline (the off-main telemetry or the S0 counters);
- device await;
- `refused_by` counts.

Zero refusals alone is not success.

**Then,** only after review: a new SNC SHA is frozen, the procedure is
preregistered, and the full SNC-only CP-1A reruns from the beginning.

## 10. Implementation slices and rollback points

Each slice is one PR. Each must pass the full Release + ASan/UBSan + TSan
suites and T-tests, and gets reviewed before the next. Rollback is by
reverting a slice or by its switch.

| Slice | Content | Switch / rollback | Physical check |
|---|---|---|---|
| S0 | Counters: `refused_by`, waiters, WAL buffer bytes, flush-latency EWMA, checkpoint phase times (in STATS, not the diag gauges) | none needed (read-only) | none |
| S1 | **A**: per-class admission, waiter budget, WAL-buffer budget, read quantum; T5, T6, T9, T10 | `--admission legacy` restores today's single gate | 436,646 + checkpoint: reads no longer refused; mutations still are |
| S2 | **B1**: time-budgeted walk with deadline floor; T7, T1 | `--ckpt-pacing off` (256 per iteration) | the walk no longer cuts closed-loop throughput by about 55% |
| S3 | **C2**: `O_DIRECT` chunk writes, AIMD window on the section 7 signals; T8, T2, T3 | `--ckpt-io buffered` | WAL await during the checkpoint |
| S4 | **D**: sync token; T11 | `ckpt_sync_tokens = 8` (today's behaviour) | final sync duration |
| S5 | **E**: ioprio switch | off by default; kept only if measured to help | A/B |
| S6 | Regression (section 9) on a frozen SHA, then the review | | full |
| Later | B2 incremental snapshot (T13). F, only if S1-S6 fall short at campaign scale | separate design | |

Order rationale:
- A first: it is independent of storage, and it removes the read-refusal
  design gap even if pacing slips.
- S0 first of all, so every later slice can be measured without the diag
  branch.

## 11. Open questions for review

1. The waiter and WAL-buffer budget values (section 6), and whether
   E_OVERLOADED or a distinct status should mark a budget refusal.
2. The deadline fraction (0.5 of the interval), and whether a missed
   deadline should also raise an alarm in HEALTH.
3. Whether the regression pass criteria (section 9) are the right bar
   before a CP-1A rerun.
4. Whether B2 (incremental snapshot, removing the 50 ms `begin`) is
   needed before the CP-1A rerun, or after.
