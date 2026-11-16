# X-2 durability-boundary experiment: preregistration (PROPOSED, for owner review; NOT RUN)

- **Authority.** cody's owner decision on PR #148, section 4: "AUTHORIZE PREREGISTRATION ONLY ... No R640 test run is
  authorized by this decision." AFR-D-02 stays on **HOLD**. This document chooses no mechanism.
- **Status.** Nothing here has run. No harness, bench code, C++ or Rust is written. Every threshold is
  **PROPOSED**; cody sets the final values before any run. Building the harness, the offline correctness run and the
  R640 run each need their own go.
- **Revision 1.** An independent reviewer, told to find a misleading pass, reported 2 blocking and 11 major
  findings against the first draft. All are applied or listed as UNRESOLVED (section 12).
- **Owner rules applied.** Performance alone cannot justify weakening G-14(b). Points left open are listed as
  UNRESOLVED (section 13), not argued away.
- Labels: **FACT-SNC** (checked in this repository), **EXT** (external source, cited), **INF** (inference),
  **HYP** (unverified hypothesis).

## 1. Question

AFR-G-14(b): recovery restores every write acknowledged at LOCAL_DURABLE or above on that node, or refuses
(`docs/SNC_GUARANTEES.md`). D-02 needs a durable acknowledgement boundary. The candidates (owner decision 4;
CD section 4) are:

- (a) a durable per-flush watermark;
- (b) self-describing flush frames with a per-segment nonce;
- (c) both.

X-2 answers two questions for each candidate, on the R640 data disk:

1. **Correctness.** Which single faults in the newest flushes does it detect or prove harmless? Which does it
   still accept silently?
2. **Cost.** What does it cost in acknowledged-write throughput and in p99 acknowledgement latency, compared with
   today's flush path?

X-2 does **not** decide D-02, the byte format (AFR-D-06) or the single-copy LSN-reservation design (owner
decision 5). It produces evidence for the owner.

## 2. Today's flush path (FACT-SNC, main 80377e5)

The baseline arm must reproduce this path exactly.

- One WAL stream per shard. A flush is started at 64 KiB buffered or when the oldest record is 50 µs old
  (`WalConfig`, `include/snc/storage.h:135-149`).
- A flush is padded to 4 KiB and written as one `RWF_DSYNC` write, through io_uring, to an `O_DIRECT` segment
  file. The acknowledgement is the completion (`src/core/storage.cpp:651-687`, `:814-827`;
  `src/native/runtime.cpp:211-231`).
- Segments are 64 MiB. They are reserved with `fallocate(mode 0)`, not written, by `SegmentPreparer` off the
  shard thread. At rollover the shard renames the spare, and its header rides in the first flush
  (`storage.cpp:690-715`, `:772-795`).
- Recovery treats a whole zero block as the unwritten rest of a segment and stops there, and it also stops,
  silently, at an LSN hole inside a segment (`storage.cpp:386-411`). A zeroed or lost newest flush therefore looks
  like an unwritten tail. This is AFR-F-01 and AFR-R-01. A bad header in a segment that is not all zero is fatal
  (`:378-381`).
- After a restart, a stream opens a new segment at `next_lsn` and leaves any discarded torn tail in the old one
  (`storage.cpp:521-527`).
- **The baseline arm reproduces today's write path, not today's recovery.** Every arm is judged under the common
  recovery rule R0 (8.1), which is stricter than today's code.
- Records are 56-byte headers plus payload, CRC-32C, rounded to 8 bytes (`include/snc/wal_format.h`). An
  `AccountV1` record is 56 + 200 = 256 bytes (`include/snc/types/account.h:50`).

**INF, which the experiment must check (section 6, M6):** on ext4, writing into a `fallocate`d (unwritten)
extent needs an extent conversion. With `RWF_DSYNC` that conversion is committed through the journal before
completion. Today's baseline may therefore already pay one journal commit and cache flush per WAL flush. If so:

- formatting segments (writing every block before use) may make flushes **cheaper**, not dearer;
- a watermark that overwrites an already-written block may cost only a FUA write.

So a zero-filled control arm (B1) is required, to separate "formatting removes extent conversion" from "the nonce
costs something".

## 3. Prior art: what X-2 borrows

| Source | What it does | What X-2 takes |
|---|---|---|
| SQLite WAL ([file format](https://www.sqlite.org/fileformat2.html), WAL section) [EXT] | Two salt values in the WAL header are copied into every frame header. A frame is valid only if its salts match the header and the cumulative checksum matches. Commit frames carry the database size | The per-segment nonce in arm F, and the commit marker in the frame |
| RocksDB recyclable WAL ([log format](https://github.com/facebook/rocksdb/wiki/Write-Ahead-Log-File-Format)) [EXT] | Each record header carries a 32-bit log number "so that we can distinguish between records written by the most recent log writer vs a previous one" | The same purpose: stale but CRC-valid content is rejected. Case C6 tests it |
| PostgreSQL `wal_init_zero`, `wal_recycle` ([docs](https://www.postgresql.org/docs/current/runtime-config-wal.html)) [EXT] | New WAL files are zero-filled by default; recycling renames old files | Control arm B1 (written zeros) |
| CTRL, Alagappan et al., FAST 2018 ([paper](https://www.usenix.org/system/files/conference/fast18/fast18-alagappan.pdf)) [EXT] | Its local layer tells a crash from corruption by writing a "persist record" after each entry (3.3.3). In the final layout, the identifier at the head of the log also serves as the persist record (3.3.4) | Arm F's frame trailer. Also its warning: a crash and a corruption can leave the same bytes, so the oracle must know which writes were acknowledged |
| CORDS, Ganesan et al., FAST 2017 ([paper](https://files.usenix.org/s3fs-public/cdn/fast17-ganesan.pdf)) [EXT] | Injects "exactly a single fault to a single file-system block in a single node at a time" into Redis, ZooKeeper, Cassandra, Kafka, RethinkDB, MongoDB, LogCabin and CockroachDB, and found data loss | The fault model: one fault, one block, one run. Every position is enumerated |
| Bairavasundaram et al., FAST 2008 ([paper](https://www.usenix.org/legacy/event/fast08/tech/full_papers/bairavasundaram/bairavasundaram.pdf)) [EXT] | Field data from NetApp. Lost, torn and misdirected writes are named as causes of the corruption classes it counts ("a lost write, which typically occurs because a write destined for disk is not written") | Lost, torn and misdirected writes are in scope. It does not give their rates, so X-2 claims no rates |
| etcd WAL ([encoder.go](https://github.com/etcd-io/etcd/blob/main/server/storage/wal/encoder.go)) [EXT] | Each record's CRC is seeded from the previous one | INF: a chained CRC finds a hole in the middle, not a lost tail. Not an arm |
| Oracle `DB_LOST_WRITE_PROTECT` ([reference](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/DB_LOST_WRITE_PROTECT.html)) [EXT] | Logs buffer-cache reads in the redo, and detects lost writes on a standby or during media recovery | Lost writes are a recognised production fault, detected from a second record. That is the watermark's role |
| Kalibera and Jones, "Rigorous Benchmarking in Reasonable Time", ISMM 2013 ([paper](https://kar.kent.ac.uk/33611/45/p63-kaliber.pdf)) [EXT] | Repeat at each level of the experiment. "Report performance changes with effect size confidence intervals" | Section 7: ratios per paired block, with confidence intervals |
| Linux dm-flakey, dm-log-writes ([flakey](https://docs.kernel.org/admin-guide/device-mapper/dm-flakey.html), [log-writes](https://docs.kernel.org/admin-guide/device-mapper/log-writes.html)) [EXT] | `drop_writes` ("All write I/O is silently ignored"), `corrupt_bio_byte`; log-writes replays to FUA points or marks | Optional cross-check of the file-level injection (section 8.3). It is not the primary method |

Nothing is adopted as code. Each arm is SNC's own prototype.

## 4. Arms

All arms are bench-only prototypes built from the frozen main SHA's WAL path (`WalStream`, `SegmentPreparer`,
`sri_write`), with the arm's change and nothing else. They live under `bench/x2/` and are never linked into
`snc_server`. Each prototype has its own recovery, and every arm's recovery follows R0 (8.1).

| Arm | Segment format | Flush frame | Watermark | Acknowledged when |
|---|---|---|---|---|
| **B0a, B0b** | today: `fallocate`, unwritten | today | none | data write completes |
| **B1** control | every 4 KiB block written with zeros before use | today | none | data write completes |
| **F** = (b) | every 512 B sector written before use as a **format sector**: magic, segment nonce, sector index, CRC | header (nonce, term, first and last LSN, length) and trailer (nonce, term, last LSN, CRC over the frame) | none | data write completes |
| **WS** = (a), serial | today | today | see below | data(k) completes, **then** watermark(k) completes |
| **WP** = (a), pipelined | today | today | see below | watermark(k) completes |
| **FWS**, **FWP** = (c) | as F | as F | as WS, as WP | as WS, as WP |
| **PC-L** latency positive control | today | today | none | data write completes, plus a fixed delay `d_L` (7.3) |
| **PC-T** throughput positive control | today | today | none | as B0, with the stream's acknowledged rate capped at 90% of B0's calibration capacity (7.3) |

**Formatting (B1, F, FWS, FWP).** Every segment the arm writes into is formatted and synced before its first
flush, including each run's first segment, which is prepared before the warm-up. A segment created on the stream
thread (`open_segment`, a spare miss) is formatted there too. In these arms any spare miss makes the run
INCOMPLETE (10), because the miss path would hide the formatting cost.

**Format sectors (F).** Each 512 B sector identifies itself, so recovery can tell three things apart:

- a torn frame: some of its sectors are still format sectors of this segment's nonce;
- an unwritten tail: format sectors to the end of the segment;
- damage: a sector that is neither a valid part of a frame nor a format sector of this nonce, zeros included.

**Watermark (WS, WP, FWS, FWP).**
- A per-stream file holds two 4 KiB slots. Each slot holds a generation, the term, the last LSN of flush k, the
  CRC-32C of that last record, and a slot CRC. The file is written with `O_DIRECT`, `RWF_DSYNC`.
- The file is created, both slots are initialized (generation 0, the stream's start LSN) and synced, and the
  directory is synced, **before the stream's first acknowledgement**. From then on a missing file, or two invalid
  slots, is damage.
- Slots alternate by generation. **At most one watermark write is in flight per stream**: watermark(k+1) is not
  submitted before watermark(k) completes.
- In **WS**, watermark(k) is submitted when data(k) completes.
- In **WP**, watermark(k) is submitted together with data(k+1), or 50 µs after data(k) completes if data(k+1) has
  not been submitted by then. data(k+1) is never held back for watermark(k). At QD1 nothing follows, so WP pays
  the 50 µs. That is a measured property of WP, not an accident.

**Common to all arms.**
- Nonces are 64-bit random, drawn per segment by the preparer and recorded in the segment header.
- Payload bytes come from a seed that includes the run id, so content left on the device by an earlier run never
  equals a later run's content at the same LSN.
- Variants that are not listed (a watermark every N flushes, a watermark on another device) are out of scope.
  Adding one is a revision of this document before any run.

## 5. Host, storage and guards (R640)

- **Host:** Dell R640 "solo4", 2 × Xeon Gold 6150, 376 GiB, Ubuntu 24.04, kernel 6.8.0-149-generic. Manifest e
  `bench/manifests/r640-main-20261111.txt`, SHA-256 `768056bbee1d7f178ed0be1c195b821f0638a1de28a4aca1ea0afdf5444c27b6`.
- **Data disk:** Samsung ARFX3840S5xnFTRI 3.84 TB SAS SSD on the SAS3008 HBA (NUMA node 0). Serial S48LNA0M700325,
  WWN 0x5002538b097f68a0. One partition, ext4 `rw,relatime,stripe=2`, UUID
  `3bf857c8-0cb4-4c57-b9a5-14328e70b061`, at `/srv/snc-data`. Write cache `write back`, `fua=1`, mq-deadline.
  - Identity is checked as in A2-a revision 4: UUID, model, serial and WWN, resolved dynamically. The kernel name
    (`sda` or `sdb`) is evidence only.
  - Power-loss protection is **not verified** (`docs/R640_TOPOLOGY.md` section 2). The result describes this
    device only (section 11).
- **Every session, fail closed (else INCOMPLETE, and nothing is replaced):**
  - a new `bench/manifest.sh` capture, diffed against manifest e. Only preregistered volatile fields may differ
    (uptime, boot id, counters, the kernel disk name, free space);
  - the identity above;
  - `fstrim.timer` disabled and inactive, and `fstrim.service` inactive (A0 still holds);
  - `numa_balancing` = 0 and every governor `performance`, set and verified;
  - write-cache state unchanged, and never toggled;
  - a clean tree at the frozen SHA, with the SHA-256 of the bench binary recorded;
  - at least 200 GiB free on `/srv/snc-data`.
- **Placement (PILOT-L family):** the 8 stream threads on node-0 CPUs 0, 2, ..., 14, and the preparer on node-0
  CPU 16. No SMT sibling of a bench CPU is used. Nothing else runs on the host.
- **Recorded around each session, not judged:** `smartctl -x` and `sg_logs -a`, including the background-scan
  state, which was active in A2-a; SSD temperature and endurance used; `/proc/diskstats`;
  `/proc/fs/jbd2/<dev>/info`. Device GC state drifts across trim-free runs; the session difference (7.1) is the
  only view of it.

## 6. Performance method

### 6.1 Workloads and loads

| Id | Load | Streams | Record | Purpose |
|---|---|---|---|---|
| L1-256, L1-16K | **QD1**: one record per flush, the next appended only after the acknowledgement | 1 | 256 B; 16 KiB | Worst case for a watermark: no amortization |
| L2-256 | **Open loop** at 50% of B0's capacity per stream. Latency is measured from the intended issue time (no coordinated omission) | 8 | 256 B | A normal, loaded server. Group commit as in Profile A |
| L2h-256 | Open loop at 80% of B0's capacity | 8 | 256 B | Queueing near saturation, where a small extra cost per flush grows |
| L3-256, L3-16K | **Closed loop**, saturating; depth 512 outstanding records per stream | 8 | 256 B; 16 KiB | Capacity, with the preparer and the device contended |

- **Calibration, once per session, never judged:** one B0 run at L3-256 and one at L1-256. The rates for L2 and
  L2h are 50% and 80% of the L3-256 capacity per stream, rounded down to 1,000 records/s. `d_L` and the PC-T cap
  come from the same two runs (7.3). All are frozen in the session record before the first judged run.
- Payloads are pseudo-random (4), so they cannot compress.
- Each run uses a fresh directory, which is deleted with `sync -f` after the run. Runs never trim.
- **Rollover check:** every L2, L2h and L3 run must roll over at least 3 segments per stream, so formatting is
  inside the window. If it does not, the run is INCOMPLETE.

### 6.2 Order and repetitions

- **One run:** 5 s warm-up and a 30 s measured window, then drain, verify and delete. Budget about 60 s per L3
  run and 45 s per other run.
- **Run space per block, 51 runs:**
  - 8 arms (B0a, B0b, B1, F, WS, WP, FWS, FWP) × 6 workloads = 48;
  - PC-L at L1-256 and L2-256;
  - PC-T at L3-256.
- **Randomized complete blocks.** Each block runs all 51 in its own permutation. The permutation is
  `random.Random(int(sha256(seed).hexdigest(), 16)).shuffle(runs)` (CPython 3.12) over the run list in the order
  above, with seed strings `x2-s1-b1` ... `x2-s2-b6`.
- **6 blocks per session; 2 sessions on separate boots.** That gives 12 paired observations of each cell.
- **Why paired and interleaved:** in A2-a, session 2's median capacity was 4,356,828 ops/s, against 3,996,693 to
  4,033,870 in the other four sessions, with within-session CVs of 4.2% or less
  (`bench/results/a2a-storage-repro-r640/RESULT.md`). Comparing arms across sessions would mix that shift into a
  10% decision. Within a block it cancels [INF; the A/A control tests it, 7.3].

### 6.3 Metrics (per run)

| Id | Metric | Primary? |
|---|---|---|
| M1 | Acknowledged records/s over the window (L3) | **yes** |
| M2 | Acknowledgement latency (append to acknowledgement) p99 (L1, L2, L2h) | **yes** |
| M3 | p50 and p99.9 of M2; M2 at L3; flush device latency p50/p99 | no, reported |
| M4 | Bytes written to the device per acknowledged payload byte (diskstats), including formatting | no, reported; also as endurance cost |
| M5 | Spare misses and the maximum rollover stall | no, reported. Any spare miss in B1/F/FWS/FWP makes the run INCOMPLETE |
| M6 | Device writes completed and flushes completed per WAL flush (diskstats fields 5 and 16); jbd2 commits per WAL flush | no. This is the mechanism check for section 2 and 7.3 |
| M7 | CPU time per acknowledged record (getrusage) | no, reported |

- **Latency resolution:** an HDR-style histogram with relative error at most 0.1% from 1 µs to 1 s.
- SNC's own log2 flush histogram (`flush_us_log2`) and the server's coarse p99 bins are **not** used. A 10%
  change can vanish inside one of their bins, or appear across a bin edge.
- diskstats does not count FUA writes. Telling FUA from flush needs blktrace, which needs root (13, item 6).

## 7. Statistics and decision rules (thresholds PROPOSED)

### 7.1 Overhead per cell

For each arm X, workload w and block b, take `r = M(X) / M(B0a)` in the same block. For M1 the overhead is `1 - r`;
for M2 it is `r - 1`.

- The estimate is the geometric mean of the 12 block ratios.
- The interval is a two-sided 95% t-interval on the log ratios (df = 11), pooled over both sessions.
- The two sessions' estimates are also reported separately. If they differ by more than T/2, that is a finding.

### 7.2 Verdict per cell and per arm (PROPOSED threshold T = 10%)

- **WITHIN**: the upper 95% bound of the overhead ≤ T.
- **OVER**: the lower 95% bound > T.
- **UNRESOLVED**: anything else. It is reported as unresolved, and no runs are added to resolve it.
- Negative overhead (the arm is faster) is reported as a number, not credited to the mechanism. B1 explains it or
  it stays unexplained.
- **An arm's cost is WITHIN only if all 6 primary cells are WITHIN:** M1 at L3-256 and L3-16K, and M2 at L1-256,
  L1-16K, L2-256 and L2h-256. One OVER makes the arm OVER.
  - Requiring every cell makes a false WITHIN less likely, not more, so no multiplicity correction is applied to
    WITHIN.
- Every arm is compared with B0a, today's path. F, FWS and FWP are also reported against B1, so the nonce's own
  cost shows apart from the formatting effect.

**Power [INF, from A2-a's whole-server numbers; the bench may be quieter or noisier].**
- With a per-run SD of the log metric of 0.042 (A2-a's largest within-session p99 CV), a ratio has an SD of about
  0.059, so the half-width is 2.201 × 0.059 / √12 ≈ 3.7 points.
- A true overhead up to about 6% can then be shown WITHIN, and one above about 14% shown OVER. Arms with true costs
  between those are expected to be UNRESOLVED.

### 7.3 Controls

- **A/A (B0b against B0a), an equivalence test per primary metric.** The 90% interval of the A/A overhead must lie
  inside ±T/2.
  - If it does not, every cell of that metric is UNRESOLVED, because the method cannot resolve T there.
  - The other metrics stand.
- **PC-L.** `d_L` = 10% of the calibration B0 L1-256 **p99**, added to every acknowledgement. That shifts p99 by
  exactly 10%. PC-L's M2 overhead at L1-256 must have a lower 95% bound of at least 5%. If not, every M2 verdict
  is VOID.
- **PC-T.** A cap at 90% of the calibration L3-256 capacity. PC-T's M1 overhead must have a lower 95% bound of at
  least 5%. If not, every M1 verdict is VOID.
- **Mechanism check (M6), W arms.** Device writes completed per WAL flush must exceed B0's by at least 0.9 in the
  same block. Otherwise the arm did not issue its second durable write, and its result is VOID.
  - Flush counts are reported, not judged. A watermark overwrite may reach the device as a FUA write, not as a
    flush.

### 7.4 Owner question this leaves open

T = 10% follows the proposal in SA section 4 and the owner decision. It is **not approved**. cody sets the final T,
and decides whether T differs for throughput and latency, before the R640 run.

## 8. Correctness and fault injection (offline; no R640)

Correctness does not depend on the device. It runs offline on a dev VM or in CI, deterministically, and needs no
root.

- **Small segments offline:** 256 KiB, so every run rolls over many times and the segment-boundary positions and
  spare paths are exercised.
- **Sector model:** 512 B logical and 4096 B physical sectors, as on this SSD. Tears are applied at 512 B, the worst
  case.

### 8.1 Common recovery rule R0 (every arm)

R0 is the fail-closed rule AFR-S-1 prescribes for G-14(a), written as a decision procedure. Per stream, in LSN
order, recovery reads each segment:

1. **Header.**
   - Valid: continue.
   - Invalid, and the segment is the newest and holds nothing else valid (only zeros, or F format sectors): it is
     an unwritten new segment, and is discarded.
   - Invalid in any other case, including a newest segment with valid records after its header: **refuse**.
   - A spare (`spare.tmp`) holding any valid record is judged as the newest segment, never unlinked (today's
     `adopt_spare` unlinks it, `storage.cpp:796-812`).
2. **Records.** Each record must pass CRC and carry the next expected LSN.
   - Zero padding (B0/B1) or frame padding (F) from a valid record to the end of its 4 KiB block is skipped.
   - Otherwise, a record that fails CRC, or a block that is not a record, is the tail candidate.
   - A valid record with an unexpected LSN: **refuse**.
3. **Tail.** The candidate is discarded only if no valid record, and no valid header with a higher first LSN,
   follows it anywhere in the stream. Otherwise: **refuse**.
   - B0/B1: a zero block counts as a candidate.
   - F: a format sector of this nonce counts as unwritten. Anything else is damage, and a sector-mixed frame is a
     torn frame (4).
4. **Restart residue.** A discarded tail is recorded durably, as an end-LSN marker in the next segment's header,
   before the stream writes again. Its old bytes are then skipped, not re-judged, on a later recovery. Without
   this, the second recovery after a crash would refuse (C1b). The marker is a segment-header field in every arm,
   B0 included; it is written once per segment, not per flush, so it does not touch the measured flush path.
5. **W arms.** If the recovered end (term, LSN) is below the watermark, or the record at the watermark LSN does
   not have the watermark's term and CRC: **refuse**.
6. **LSN gaps.** Gaps between segments are refused here. Owner decision 5 (LSN reservation) would need recorded
   gaps; that format is not X-2's (13, item 7).

- **Oracle independence.** The checker is written separately from the arm prototypes and shares no parsing code
  with them. Ground truth is the driver's acknowledgement log: the LSN and CRC of every acknowledged record,
  recorded in memory and written to a separate file system.
- For every state, each arm's own recovery runs, and the checker independently compares the recovered set with the
  acknowledgement log.

### 8.2 Cases

Positions: newest flush; second-newest; the first flush of a segment (header riding in it); the last flush before
a rollover; a flush in the middle of a segment; for W arms, each watermark slot. A cell that cannot occur (for
example, "newest" at "middle") is omitted.

Each case runs at 256 B and 16 KiB records. **Expected results are fixed now:**
- **S** = silent loss;
- **D** = detected and refused;
- **OK** = every acknowledged write recovered, and no refusal;
- **FR** = a false refusal (nothing acknowledged was lost).

| Case | Fault (single unless named) | B0, B1 | F | WS, WP | FWS, FWP |
|---|---|---|---|---|---|
| C1 | Crash only (8.3) | OK | OK | OK | OK |
| C1b | Crash, recover, append 8 flushes, crash again (two cycles) | OK | OK | OK | OK |
| C2 | Acknowledged newest flush zeroed | S | D | D | D |
| C3 | Acknowledged newest flush lost; the location keeps its prior content (unwritten, zeros or format) | S | S | D | D |
| C3s | As C3, but the location holds **stale device content**: CRC-valid frames from an earlier run at the same LSNs (another nonce, other payload) | S | D | D | D |
| C4 | Newest 2 and newest 3 flushes lost | S | S | D | D |
| C5 | Acknowledged middle flush lost or zeroed, later flushes intact | D (by R0; today's code: S) | D | D | D |
| C6 | Newest flush lost; the location holds a CRC-valid frame with the expected LSN from another incarnation (reused LSN, AFR-R-03) | S | D (nonce) | D (term and CRC in watermark) | D |
| C7 | Watermark write lost, data intact | n/a | n/a | OK | OK |
| C8 | Both watermark slots damaged, or the watermark file missing | n/a | n/a | D | D |
| C9 | **Double fault:** watermark write lost **and** the newest data flush lost | n/a | n/a | S (residual) | S (residual) |
| C10 | Bit flip in an acknowledged flush, middle positions | D | D | D | D |
| C10n | Bit flip in the last record of the acknowledged newest flush | S | D | D | D |
| C11 | Acknowledged newest flush torn: at least one sector holding record bytes is not written | S | S | D | D |
| C12 | Spare rename not durable (crash after rename, before the first flush is synced elsewhere) | OK | OK | OK | OK |
| C13 | First flush of a spare (with the header) lost or zeroed, later acknowledged flushes in it intact | D (R0 step 1) | D | D | D |
| C14 | Whole newest segment file missing | S | S | D | D |
| C15 | A format write beyond the tail lost (F arms): that sector reads as zeros or stale content | n/a | FR | n/a | FR |

### 8.3 Method

- A driver runs each arm's write path on a real file system. It logs every write (file, offset, length, content
  hash), its submit and completion order, and every rename, unlink and directory sync.
- Faults are applied to copies of the resulting files and directories:
  - "lost" restores the content from before the write;
  - "stale" (C3s) restores content from an earlier run's files at the same offsets;
  - "zeroed" writes zeros;
  - "torn" keeps a sector subset;
  - namespace faults (C12-C14) undo the rename or unlink the file.
- **C1 crash states.** The crash point is after any completed write or namespace operation. Every write in flight
  at that point, data and watermark both, in every order, takes one of these sector states:
  - none of it;
  - all of it;
  - each 512 B prefix;
  - each 512 B suffix;
  - each single missing sector;
  - 64 seeded random subsets.

  This is exhaustive over those classes for runs of 64 flushes per stream with 256 KiB segments, and sampled at
  10,000 seeded states for longer runs. Arbitrary subsets are not exhaustive; sampling covers them.
- **Optional cross-check:** on a dev VM with root, `dm-log-writes` replay to each FUA point. A disagreement with the
  file-level result is a finding, not a tie-break.

### 8.4 Correctness verdicts (PROPOSED)

- **Checker positive control:** B0 must show S in C2, C3, C10n and C11, the known defect. If the checker reports B0
  clean, the checker is broken and every correctness verdict is VOID.
- **An arm covers G-14(b) for single faults** only if it has zero S in every single-fault case (all except C9) at
  every position and size.
- **False refusals** are counted apart from losses, and judged against G-48:
  - zero FR is required in C1, C1b, C7 and C12;
  - C15 is FR by design for the F arms, an availability cost of formatting reported to the owner.
- C9 is reported as the residual of a same-device watermark. It is never counted as a pass.
- Any result that differs from the table, in either direction, is a finding to explain before any verdict.

## 9. What a result can and cannot show

- **Can:** which candidates detect which single faults, with zero silent loss as the bar; and their cost on this
  device and this file system, within a 95% interval, compared with today's path.
- **Cannot:**
  - end-to-end server throughput (X-2 drives the WAL layer, not `snc_server`), so an implementation needs its own
    end-to-end check;
  - another device, especially one without power-loss protection, or another file system;
  - rates of the faults in the field;
  - faults in ext4's own metadata or journal;
  - superblock rollback, lost LSN-reservation writes (owner decision 5) and interrupted truncation (CD 4.4). These
    are out of scope and stay open under D-02.
- **Never:** a mechanism that is cheap but fails 8.4 is not a candidate for G-14(b). Cost cannot buy back a silent
  loss.

## 10. INCOMPLETE, VOID and reruns

- **INCOMPLETE** (the session stops and is not replaced):
  - a guard fails;
  - an arm's run errors;
  - any acknowledged record is missing from the run's own post-run verification read;
  - the rollover check fails (6.1);
  - a spare miss in a formatting arm (6.3, M5).
- **VOID** (data kept and reported, no verdict): a positive control or a mechanism check fails (7.3); the checker
  positive control fails (8.4).
- **One rescue** is allowed per stage, for an INCOMPLETE session whose cause is outside the bench (power, reboot,
  a host unit). The cause is recorded before the rerun.
- No rerun for UNRESOLVED or OVER cells. No threshold change after data.

## 11. Resources and expected runtime

| Item | Estimate |
|---|---|
| R640 exclusive time | per block, 17 L3 runs × 60 s + 34 others × 45 s ≈ 43 min. Per session, 6 blocks ≈ 4.3 h, plus 25 min for setup, guards and calibration. In all **about 9.5 h, on two boots** (two days) |
| Disk space | at most one run's files at a time: ≤ 50 GiB worst case (L3 at an assumed 1.1 GB/s × 35 s ≈ 39 GB) |
| Device writes | upper bound ≈ 204 L3 runs × 39 GB ≈ 8 TB, plus formatting, so under 12 TB. Endurance used is recorded before and after. INF: at 1 DWPD this is under 0.2% of rated endurance |
| Root on the R640 | none beyond today's A2-a sudoers (smartctl, sg_logs, read only). No trim, no dm devices. blktrace only if cody grants it (13, item 6) |
| Offline correctness | a dev VM or CI, no root. Estimated at a few hours of CPU at most, dominated by C1 enumeration |
| Work before any run | (1) the bench prototypes and checker under `bench/x2/`, with tests and a mutation suite for the checker; (2) review; (3) the offline correctness run and its review; (4) an R640 go. Each step is a PR, then stop |

## 12. Independent review of this design (revision 1)

An independent reviewer read the first draft and the code it cites, and was told to find misleading passes. Every
line reference checked out. Findings, all applied:

| # | Severity | Finding | Change |
|---|---|---|---|
| 1 | BLOCKING | A watermark of only (generation, LSN) accepts a stale frame with the expected LSN, so C6 was S, not D, for the W arms | The watermark carries the term and the last record's CRC (4); R0 step 5 |
| 2 | BLOCKING | On ext4 a lost DIO write can expose earlier device content, and with deterministic payloads that content could be CRC-valid at the same LSNs | Case C3s; payload seeds include the run id (4) |
| 3 | MAJOR | 64 flushes never fill a 64 MiB segment, so the boundary positions were never exercised offline | 256 KiB segments offline (8) |
| 4 | MAJOR | Namespace faults (lost rename, spare adoption unlinking a spare, missing segment, missing watermark file) were absent | C12-C14, C8 widened; the watermark file is initialized before the first acknowledgement (4) |
| 5 | MAJOR | R0 was underspecified (headers, zero and format blocks) | R0 written as a procedure (8.1) |
| 6 | MAJOR | A single crash cannot find a false refusal on the second recovery, caused by the torn tail left behind | C1b; R0 step 4 |
| 7 | MAJOR | A bit flip in the newest record is a discardable tail for B0 (S); F's torn-versus-corrupt rule needs self-identifying sectors | C10n; F's 512 B format sectors (4); C11 requires record bytes torn |
| 8 | MAJOR | "Every sector subset" cannot be enumerated | Sector-state classes plus sampling (8.3) |
| 9 | MAJOR | WP slot discipline and its timer were undefined | At most one watermark in flight; timer from data completion (4) |
| 10 | MAJOR | diskstats has no FUA count; blktrace needs root; the WS VOID rule would fire wrongly | The mechanism check uses writes completed (7.3); FUA in 13 |
| 11 | MAJOR | The A/A rule was self-contradictory, likely to fail by chance, and underpowered | Equivalence within ±T/2; 12 blocks; power stated (7.2) |
| 12 | MAJOR | The positive control used the median but was judged on p99, had no source run, and had no throughput twin | `d_L` from a dedicated L1 calibration p99; PC-T added (7.3) |
| 13 | MAJOR | The spare-miss path skipped formatting and hid its cost | Formatting on every path; a miss is INCOMPLETE; C15 |
| 14 | MINOR | Costs near saturation were not judged | L2h at 80% made primary |
| 15 | MINOR | Degenerate bootstrap over two sessions, unspecified permutation, run time too short for deletes, meaningless cells | Bootstrap dropped; permutation specified; 60 s for L3 runs; impossible cells omitted |
| 16 | MINOR | C5 = D holds only under R0, not under today's recovery | Stated in 2 and in the C5 cell |

The reviewer left two items unresolved: whether the CTRL section numbers and the other external quotes are exact
(section 3 was checked against the sources by a separate fetch, and CTRL's 3.3.3 and 3.3.4 matched), and whether
the checker exercises each arm's own recovery code (8.1 now requires both).

## 13. UNRESOLVED (owner or later decision)

1. **T.** The final threshold or thresholds (7.4).
2. **Same-device watermark.** A watermark on the same SSD shares the device's failure: C9, and a device that drops
   both writes. X-2 does not settle whether a second device, or the replica set, is the only real answer for
   G-14(b).
3. **Power-loss protection unknown.** If the SSD has it, flushes are cheap and the watermark may look free; on
   hardware without it the cost would differ (HYP). X-2 claims nothing beyond this device.
4. **Micro-benchmark against end-to-end.** A WITHIN result at the WAL layer does not prove WITHIN in `snc_server`.
   The end-to-end check belongs to implementation.
5. **The extent-conversion effect (section 2) is an inference.** If M6 shows that B0 already pays a journal commit
   per flush, today's baseline is itself an artefact of `fallocate`. That might be a cheaper fix than any arm, and
   it would be a separate question for cody.
6. **blktrace.** Telling FUA writes from flushes needs root on the R640. It is not requested; with a read-only
   sudo rule from cody, M6 would show it.
7. **LSN gaps.** R0 refuses on gaps. LSN reservation (owner decision 5) needs recorded gaps, which is a format
   question for AFR-D-06, not X-2.
8. **Power.** The power estimate (7.2) borrows whole-server variance from A2-a. If the bench is noisier, more cells
   will be UNRESOLVED; the design does not add runs after data.

## 14. Not authorized

No R640 run. No harness or bench code. No change to `snc_server`, the WAL format, `docs/SNC_GUARANTEES.md` or the
AFR decision documents. D-02 stays on HOLD.
