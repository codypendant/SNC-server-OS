# AFR-D-06: storage format and node metadata atomicity (design, PROPOSED)

Status: **PROPOSED, docs only.** Authorized by the owner decision of 2026-11-17
(`docs/handoff/PR155_Owner_Decision.md`, "Next, authorized"). Nothing here authorizes C++ or Rust work, an R640 run,
the X-2 harness, a D-02 choice or an R9-F freeze.

**One recorded revision of frozen text is proposed and needs the owner** (section 4.2): the term history `H` is
derived from per-record terms instead of being kept as a separate structure. Everything else realizes frozen text
without changing it.

Labels: [FACT-SNC] means read from this repository at commit 38ad581; [EXT] comes from a cited primary source; [INF]
is inferred here; [HYP] is a guess not yet checked.

## 0. The recommendation

1. **One node metadata record.** This is today's superblock pair, grown from an 80-byte struct to a fixed 8 KiB copy.
   It alternates between two files by generation and carries a whole-copy CRC. It is the only place that holds the
   node's durable term, every stream's incomplete flag and truncation intent, the checkpoint pointer and today's lease
   fields.
2. **Writes go through `meta_mu`.** Every write of the record is a compare-and-write under one small mutex,
   `meta_mu`, held only while the copy is built and written. All record writes are rare control events.
3. **A term on every WAL record.** The term history of a stream is what its records say (Raft), with a durable floor
   in the oldest retained segment's header.
4. **The valid end is implied by segment layout.** After a truncation, records within a segment at or above the next
   segment's first LSN are void, so no marker is needed.
5. **No split per shard.**

## 1. The problem

The conditionally frozen contract needs one logical, atomic metadata authority per node, read before the WAL scan
(R6.3′ item 2; owner condition of 2026-11-17). It covers the durable term, the term history, every stream's truncation
intents and the incomplete flag. D-06 chooses the physical layout.

This document settles:
- that layout;
- completion (`docs/AFR_001_PROMOTION_DURING_RECON.md` 2.2, items 1-5);
- per-record terms;
- the request binding fields.

These are the format items listed under O2 in `docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md`. Section 8 lists what is
left out.

## 2. What exists today [FACT-SNC]

- **Superblock.**
  - `superblock.a` and `superblock.b` alternate by generation parity.
  - Each is one 4 KiB block with a CRC over the block.
  - Recovery takes the valid copy with the highest generation and skips a copy with an unknown `format` as damaged
    (`storage.cpp:210-226`, AFR-R-32).
  - The struct holds `term`, `generation`, `active_ckpt`, `next_lease`, `lease_lo/hi` and `flags` (`kSbRoleTracked`)
    (`wal_format.h:16-39`).
  - A write is `pwrite`, `fdatasync`, then a directory fsync (`storage.cpp:228-243`).
  - Seeding writes generation 1 only (`seed.cpp:268-273`).
- **Role.**
  - `DIR/role` is a separate file, written by temp file and rename (`engine.cpp:201-213`).
  - It is written at boot (`:938`, `:1025`), and as "primary" only after the start barrier's re-seal (`:1339`), well
    after promotion persists the term (`:1405-1409`).
  - The gap is deliberate: a crash before the re-seal must not restart as a primary (`docs/LEGACY_ROLE.md` section 3).
  - The role is not in the contract's atomic authority.
- **Locks.** Promotion and checkpoint each hold `control_mu_` throughout (`engine.cpp:1349`, `:1439`) and wait on
  shard threads while they hold it (`run_on_all_`).
- **Checkpoint.**
  - Its manifest records a start LSN per stream and is written by temp file and rename (`storage.cpp:286-321`).
  - Reclaim keeps covered segments for lagging standbys (`keep_from`, `engine.cpp:1509-1580`).
- **WAL.**
  - There is one stream per shard, in segments named by their first LSN.
  - A record header is 56 bytes, with no term (`wal_format.h:60-73`).
  - The recovery scan ends a stream at a torn block or a hole inside a segment. A bad header on a segment that is not
    the last, or a gap between segments, is fatal (`storage.cpp:362-420`).
  - Standby catch-up reads through `read_stream_range` (`:439`, `replication.cpp:163`).
  - A spare is renamed into place without a directory fsync, and recovery adopts it by its header (`:690-705`,
    `:797-812`).
  - The writer, on opening, unlinks any segment already named at its next LSN and creates it again (`:521-527`).
  - The term is node-wide (`sb_.term`). Each shard's stream replicates at that term (`replica_config`, `:1424`).
- **Absent:** truncation, intents, per-record terms, a term history, an incarnation, the incomplete flag.

## 3. Prior art (primary sources)

| System | How it keeps node or group metadata atomic | What SNC takes |
|---|---|---|
| Raft [EXT: raft.github.io/raft.pdf, Figure 2] | "Persistent state on all servers: (Updated on stable storage before responding to RPCs)": `currentTerm`, `votedFor`, and `log[]`, where "each entry contains command for state machine, and term when entry was received by leader". The log's terms are the term history; a snapshot keeps the last included term and index. | per-record terms; the history is the log; a durable floor |
| Kafka KIP-101 [EXT: cwiki.apache.org, KIP-101] | A per-replica "Leader Epoch Sequence File": "A journal of changes of Leadership Epoch mapping the Leader Epoch to a starting offset for that epoch", flushed when an epoch is added. | considered and not taken: a second structure that must agree with the log (section 4.2) |
| Kafka checkpoint [EXT: `OffsetCheckpoint.scala`, 0.8.2.1] | One file covers many partitions: "write to temp file and then swap with the existing file", after a flush and fsync. The reader checks only a version line and an entry count. | one file for many streams is ordinary; SNC adds a CRC |
| LMDB [EXT: mdb_internal(3), manpages.debian.org/unstable/lmdb-doc/mdb_internal.3] | "Pages 0-1 are meta pages. Transaction N writes meta page #(N % 2)", each with `mm_txnid`, the "txnid that committed this page". | A/B copies chosen by a counter; a write never touches the live copy. How the newer page is picked is not shown in the fetched page and is not relied on |
| PostgreSQL [EXT: `pg_control.h`, doxygen.postgresql.org] | "For reliability's sake, it's critical that pg_control updates be atomic writes. That generally means the active data can't be more than one disk sector, which is 512 bytes on common hardware", with a CRC that "MUST BE LAST" and static asserts on the size. | the contrast: SNC does not rely on sector atomicity, hence two copies |
| CockroachDB [EXT: docs.cockroachlabs.com/docs/v25.4/architecture/storage-layer; cockroach#99273] | "each store contains two instances of the storage engine": one for temporary distributed-SQL data, "One for all other data on the node". Pebble "provides atomic write batches". In #99273 one engineer notes that the separated-raft-log design "included an init step at crash recovery time" reconciling `HardState.Commit` with the applied index. Another notes that HardState is written into a snapshot's files "so that the HardState is updated atomically with the snapshot ingestion". | state split across two stores needs a recovery-time reconciliation (section 5, [INF] by analogy) |
| TiKV raft-engine [EXT: docs.rs/crate/raft-engine/0.2.1, README] | "Different Raft Groups share the same log stream", with "atomic writes. Users can stash the changes into a log batch before submitting." | many groups' metadata in one node-wide store |

No system is adopted or embedded.

## 4. Design

### 4.1 One node metadata record

**Format.** `SNCSUPR2` is a fixed 8 KiB copy (two `kBlock`s), stored in `superblock.a` and `superblock.b` by generation
parity. The CRC covers both blocks, so a tear between them fails it.

| Field | Bytes | Meaning |
|---|---|---|
| magic, format, flags | 8 + 2 + 2 | `flags` keeps `kSbRoleTracked` |
| crc32c | 4 | over the whole copy with this field zeroed |
| generation | 8 | +1 per write |
| space_uuid | 16 | as today |
| term | 8 | durable highest term (R4) |
| (reserved) | 8 | |
| active_ckpt, next_lease, lease_lo, lease_hi | 32 | as today |
| n_streams | 2 (+6 pad) | |
| per stream ×256 (`kMaxShards`): `state` u32 + pad, `t` u64, `L` u64 | 24 each | `state` is NONE, PENDING (the intent of 4.3) or RESEEDING (R6.5's incomplete flag, per stream, so that two streams reseeding clear independently) |

Total: 96 + 256 × 24 = 6,240 bytes, within 8,192.

**Incarnation** (R6.3′ item 1) is the generation of the record write that each boot makes before its first R6.2
request. It is unique and grows with every boot, so it needs no field of its own.

**Write rule.** Under `meta_mu`:
1. check the step's precondition against the in-memory copy (a compare-and-write);
2. build the next copy with `generation + 1`;
3. `pwrite` it to the other file, then `fdatasync`, then fsync the directory (as today);
4. publish the new values in memory: a copy for the control path, and atomics that shard threads read. A value that
   restricts (PENDING, RESEEDING) is published **before** step 3, and a value that permits (the clear) only after it.

Constraints on writes:
- **Lock order:** `control_mu_`, then `seed_->mu`, then `meta_mu`. `meta_mu` is never held across anything but the
  write, and never while waiting on a shard thread.
- **No action before durability:** no step acts on a value before its write returns.
- **Failure:** a failed write is fatal.
- **Init and seed** write the same record twice, at generations 1 and 2. Both files then exist and hold a real
  record, so a later write never creates a file, and a fallback never lands on an empty record.

**Why a crash leaves the old record or the new one [INF].**
- A write touches only the copy that is not current.
- A torn write fails its CRC, so recovery takes the other copy. That copy is the previous complete record, which
  every earlier action was based on.
- A completed write has the higher generation and wins.
- Two copies never share a generation, because writes are serialized.

**Who writes the record:**

| Writer | How often |
|---|---|
| R4 term persistence | once per term change |
| boot write (incarnation) | once per boot |
| intent and clear | once per reconciliation of a stream |
| checkpoint publication | once per checkpoint |
| lease-block changes | control-plane events |

None of them runs per operation. The latency of one 8 KiB write is not measured. It is a control-path cost (O8).

### 4.2 Per-record terms; the term history is the log (proposed revision)

- **Record header.** `WalRecord` grows from 56 to 64 bytes, with `u64 term`: the term in which a primary first wrote
  the entry.
  - A standby stores the term it receives and never restamps it [EXT: Raft].
  - Cost: 8 bytes per record, not measured (O8).
- **Floor.** Each segment header gains `prev_term`, the term of the record at `first_lsn - 1`. This is Raft's
  prevLogTerm, protected by the header CRC.
  - The floor of `H_s` is the oldest retained segment's `(prev_term, first_lsn - 1)`.
  - It moves only when reclaim deletes a segment, and reclaim keeps segments for lagging standbys (`keep_from`). So
    members that the retained log can still serve are not reseeded.
- **`H_s`.** It is the sequence of term changes over stream `s`'s retained records, plus the floor.
  - It is built in memory at recovery by a pass over **every** retained segment, headers and record terms, from the
    floor up, applying the void rule (4.3). The ordinary scan skips segments the checkpoint covers, so this is a
    separate pass. It is kept current on append.
  - R6.2's `t_k` is the term at `k`'s end, taken from `H_k` with its floor: after a full checkpoint, `k` may have no
    retained record at all.
  - R6.2's `t*` and `end_j(t*)` come from this index.
  - R6.4's "`H_j` no longer covers `t_k`" means `t_k` is below `j`'s floor term, **or** `k`'s end or the truncation
    point is below `j`'s floor LSN. Catch-up finding a segment already reclaimed (`read_stream_range` returns false,
    `storage.cpp:447`) is the same case and leads to R6.4, not to a retry loop (R6.6).
  - A term in which a stream wrote nothing leaves no trace. So the crash-loop growth noted in 2.4 does not apply to
    `H`.

**Why it is one authority with the record [INF].** `H_s` changes in only three ways:
- **Append.** A record's term never exceeds the durable term in the record, because R4 persists a newer term before
  any entry of that term is accepted.
- **Truncation.** It is governed only by the record's intent `L`, through 4.3.
- **Floor movement.** Reclaim moves the floor. It deletes only covered, retained-past segments, oldest first.

So every `H_s` that recovery can see is a function of one record copy and the WAL under that copy's intents.

**What changes in frozen text, and why.** The safety analysis keeps `H_i` "durable, with a checksum and a durable
compaction floor, so that damage is distinguishable from compaction", and says a damaged `H` refuses (R6.4, G-14(a)).
Under this design:
- `H` is durable and checksummed, because each record and header has a CRC.
- Compaction is only a floor change, made by reclaim.
- Damage to `H` is damage to the WAL. The node refuses exactly when the WAL scan detects damage:
  - a gap between segments or a bad header refuses today;
  - damage inside a segment is today read as the log's end (R-01), and that is D-02's obligation.

The proposed revision of the `H_i` paragraph (safety analysis, Member state) reads:

> `H_i` is the sequence of terms on `i`'s retained, non-void WAL records, with a floor `(prev_term, first_lsn - 1)`
> in the oldest retained segment's header. It changes only by append, by a recorded truncation, or by reclaim of
> covered segments. A member whose end, or whose truncation point, is below the answering member's floor, or whose
> last term is below its floor term, is reseeded (R6.4). Damage to `H_i` is WAL damage and refuses as G-14(a)
> requires; detecting damage inside a segment, and the loss of a middle segment or of a truncation's new segment
> (which would revive a void tail), is part of AFR-D-02. Losing the oldest retained segments cannot be told from
> reclaim; it leads only to a reseed.

This replaces the frozen sentence that rebuilding `H` from term stamps is an operator action: there is nothing left
to rebuild.

**Alternative considered: a separate `H` in the record, as in KIP-101.** It was drafted and reviewed (section 9). It
detects only damage that crosses a term boundary, which is more than this design detects, but not all damage. It also
added five failure modes:
- a standby must write the record at every term boundary during catch-up;
- stale entries for terms that wrote nothing refuse legitimate logs;
- entries can be ordered wrongly against S2′;
- an entry cap forces checkpoints that deadlock with promotion;
- a checkpoint-based floor forces reseeds.

That breaks KISS for a partial gain. If the owner keeps the frozen wording, that alternative and its fixes are the
fallback.

### 4.3 Truncation and completion (2.2 items 1-5)

1. **Intent** (R6.3′ item 2's serialized step).
   - The node takes `control_mu_`, which a checkpoint holds throughout, so an intent waits for any in-progress
     checkpoint by construction.
   - Under `control_mu_` it checks R6.3′ item 1 and R6.4: `L` must be at least the active checkpoint's **maximum**
     LSN for the stream, otherwise the stream is reseeded. The checkpoint is fuzzy: a part holds object images up to
     `max_lsn`, which can be above `start_lsn` (`shard.cpp:2319`, `:2654`), and a rebuild from it cannot undo an
     image above `L`. The manifest gains `max_lsn` per stream (section 7).
   - It then takes `meta_mu` and writes `PENDING (t_j, L)`, as a compare-and-write that fails if the term changed.
     That records the logical end before any file changes (item 1).
2. **Completion.** It runs on a thread of its own that never takes `control_mu_` and never turns replies into
   intents, and never on a shard thread. So R4 and a promotion can proceed while it runs, and a promotion that holds
   `control_mu_` while it waits for completion cannot deadlock against it.
   1. Close the stream's WAL writer and its spare slot, so that nothing is in flight. No flush can then complete above
      `L`.
   2. Handle the spare, after any `prepare()` in progress for this stream has finished (`storage.cpp:770-792`).
      If its header is valid and its first LSN is at or below `L`, adopt it, because it holds
      records whose rename was lost. Otherwise unlink it.
   3. Delete every segment whose first LSN is above `L`, newest first, then fsync the directory.
   4. Create segment `L+1` with a header and no records: write a temp file, `fdatasync`, rename it to the name, then
      fsync the directory. No name collides, because step 3 removed every such segment.
   5. Rebuild the stream's state. Load the active checkpoint, whose maximum LSN is at or below `L` by step 1, and
      replay to `L` (item 3). The C++ applies in place on receipt (`replication.cpp:753`).
   6. Reopen the stream's writer on segment `L+1`, with appends still gated.
3. **Clear.** Under `meta_mu` only, set the slot to NONE.
   - This is a compare-and-write on the slot being exactly `PENDING (t_j, L)`. It is not a compare on the term:
     clearing a completed intent is correct in every term.
   - It needs no `control_mu_`, so a promotion that holds `control_mu_` while it waits for completion (S2′ item 1)
     cannot deadlock against it.
   - The clear happens after steps 2.1-2.6 (item 4). The gates, the checkpoint start and S2′ wait for "cleared and
     open", so no checkpoint sees a closed writer (today it would record `start_lsn = 0`, `shard.cpp:2319`).
   - **No writer open ever unlinks a segment.** Today every open unlinks a segment already at its name and creates it
     again (`storage.cpp:521-527`). A crash in between would revive a void tail, at any boot after a truncation. Instead,
     an existing segment at the name is truncated to its header block, `fdatasync`ed, `fallocate`d again and opened
     without `O_EXCL`. Truncating it drops any CRC-valid block left from an earlier torn flush.
4. **Valid end (item 5).** Within any segment, records at or above the next segment's first LSN are void.
   - `scan_stream` and `read_stream_range` both apply this rule, so catch-up never ships a void record.
   - Segment `L+1` exists durably before the clear, and after it no step removes it while an older segment remains.
     Reclaim deletes oldest first, with a directory fsync after each unlink.
   - So the old segment's tail stays void through any number of later truncations.
   - This realizes TEXT ONLY item 5 without a marker in the record. The owner decision left the realization to D-06.

**Recovery, per stream.** In order:
1. read the record;
2. if the slot is PENDING, run completion, then the clear;
3. adopt a spare;
4. scan with the void rule;
5. build `H_s`.

Every completion step is idempotent, so a crash at any point repeats completion and converges (I5) [INF; not
modelled, section 9].

**While a stream is PENDING** (R6.3′ item 3, unchanged): the node takes no other action for that stream in any term
until the clear. Positioning takes `control_mu_`, so the intent step is serialized against it (R6.3′ item 2). In particular:
- it sends no R6.2 request;
- it does not position the stream;
- it does not count toward an acknowledgement or READY;
- it grants no standby lease;
- it starts no checkpoint.

The control path checks these against its copy of the record. Shard threads check them against the published atomic
(4.1). R4 term persistence is allowed, and completion then finishes before any other action in the new term (S2′
waits for it).

**Time bound.** Completion costs file deletions plus a state rebuild (a checkpoint load and a replay to `L`), so it is
not short, and its cost belongs in X-1. A failure is fatal and recovery retries it first. There is no automatic reseed
(R6.6). How long writes and checkpoints wait behind a PENDING stream is a liveness question and stays UNRESOLVED.

**Checkpoint scope: node-wide, by choice.** Section 2.4 suggested defining "takes no checkpoint" per stream. This
design keeps checkpoints node-wide and refuses to start one while any stream is PENDING. That is simpler, and the cost
is that one stream's completion delays the next checkpoint for every stream. This is recorded as a choice against 2.4's
suggestion.

### 4.4 Request binding and frame terms (wire fields, R6.3′ item 1)

- **Request:** `(stream, incarnation, request_no, last_term, end_lsn)`.
- **Reply:** echoes the request and adds the answering primary's term.
- **Replication frame:** carries the primary's current term. Each record carries its own term (4.2).

A "stream" is one shard's WAL stream. The group and its term are per node (section 2). These are field lists only;
the byte layout goes with the wire format (section 8).

## 5. Why not per shard

A store per shard turns R4's term write into N writes. A crash between two of them leaves shards at different terms,
so "persists the newer term first" and "blocks in every term" no longer hold for the node.

To be equivalent, a per-shard layout needs a node-level commit record naming the term the shards may act on. That is a
two-phase commit whose coordinator record is the node record anyway [INF]. The CockroachDB note shows the kind of
recovery-time reconciliation such a split brings [INF, by analogy].

Per-shard stores buy write parallelism for writes that happen once per control event. Rejected.

## 6. Format versions on disk (G-49, G-50, R-32)

- **New format versions:** `SNCSUPR2`, the 64-byte record and the segment header with `prev_term`.
- **Unknown format:** a copy whose magic and CRC check out but whose format is unknown is refused with a named error,
  never skipped as damaged. This is the R-32 fix. A torn copy is still skipped. The reader requires the exact copy size.
- **Not decided here:** how an existing data directory reaches the new format, by in-place conversion or by reseeding
  a whole group. Reseeding from an old-format primary needs the mixed-version wire of section 8. This is an owner
  decision, together with G-51.

## 7. Not changed

- **`DIR/role`** stays as it is (section 2).
- **The checkpoint manifest** stays where it is. It gains each stream's `max_lsn` (4.3 step 1).

## 8. Left out, on purpose

- **R9-F's fields**, which wait for R9-F's model (O1):
  - `ack_p` on disk;
  - per-key LSNs;
  - tombstones;
  - the safe LSN for objects loaded from a checkpoint;
  - the covering index.
- **The wire format** (G-49 to G-51): the byte layout and the mixed-version policy. These are a later part of D-06.
- **D-02's end-of-log mechanism.** This design works with each option:
  - option (a)'s watermark would be one more per-stream field, lowered in the same write as an intent;
  - option (b)'s frames live in the WAL.

## 9. Unresolved, proposed checks, review record

**UNRESOLVED:**
- **Silent fallback of the record.** A lost or misdirected write of a record copy, or later damage to the current copy
  (AFR-R-23), silently falls back to the previous generation. The term or an intent can then roll back. A/B copies and
  the CRC detect neither case.
  - The P7 boot read against the witness record catches a rolled-back term on the promotion path only.
  - This is the same class of fault as D-02's silent loss, and it stays with D-02 and R-23.
- **Damage inside a segment** (R-01, D-02). The term history inherits it, under the proposed revision of 4.2.
- **Costs, none measured** (O8, X-1):
  - the 8 KiB write;
  - the 8 bytes per record;
  - the rebuild;
  - the wait behind a PENDING stream.
- **Migration of existing directories** (section 6).
- **TEXT ONLY clauses.** The incarnation and the answering-side clauses stay TEXT ONLY. This document realizes them; it
  does not check them.

**Proposed, not run: a small model.** It would cover:
- completion's steps 2.1 to 3 as separate actions, with a crash anywhere;
- the writer's reopen;
- the void rule with two truncations before a checkpoint;
- spare adoption;
- reclaim order;
- A/B selection with a torn write;
- recovery's order.

Properties: I5, and no void record is ever scanned, shipped or acknowledged. `ReconBlockers.tla` makes `ApplyIntent`
and `Crash` atomic, so it covers none of this. Once C++ work is authorized, O3's crash-consistency test covers the
same cases.

**Review record.** Three rounds were told to break the design: three independent reviewers, then one on each
revision. All were read-only. Round 3's fixes have not been re-reviewed.

| # | Finding | Severity | Outcome |
|---|---|---|---|
| 1 | Completion left out 2.2 item 3, the state rebuild | BLOCKER | fixed: step 2.5; time bound restated |
| 2 | One slot per stream let a second truncation revive void records (G-15) | BLOCKER | fixed: the void rule, no marker |
| 3 | Unlinking a spare whose rename was lost deleted acknowledged records | BLOCKER | fixed: step 2.2 |
| 4 | Catch-up reads shipped void records; the marker cleared too early | BLOCKER | fixed: the void rule in both readers; reclaim order |
| 5 | A derived `H` dropped G-14(a)'s refusal for damaged `H` (frozen text) | BLOCKER | **owner decision**: proposed revision in 4.2. A separate `H` was drafted in round 1 and broke in round 2 (finding 13) |
| 6 | Writing term and role together skipped the start barrier | MAJOR | fixed: `DIR/role` unchanged |
| 7 | Completion ran against a live writer and a held spare | MAJOR | fixed: step 2.1 |
| 8 | No floor check after waiting for a checkpoint | MAJOR | fixed: step 1 |
| 9 | Deadlock with checkpoint and promotion over `control_mu_`; no lock order | MAJOR | fixed: `meta_mu` for writes only; completion without `control_mu_`; lock order |
| 10 | A superblock file first created by a later write could vanish | MAJOR | fixed: init writes both copies; directory fsync kept |
| 11 | The clear was not a compare-and-write | MAJOR | fixed: compare-and-write on `PENDING (t, L)` |
| 12 | Gates were asserted, not listed, and narrowed the frozen wording | MAJOR | fixed: frozen wording kept; examples listed; atomics for shard threads |
| 13 | Round 2, against a separate `H`: standby boundary writes, stale empty-term entries, ordering against S2′, cap deadlock, reseeds from a checkpoint floor | MAJOR ×5 | removed with the separate `H`. The floor is now the oldest retained segment |
| 14 | Round 2: the writer's reopen unlinks segment `L+1` and reopens the void window | BLOCKER | fixed: reopen without unlink (step 3) |
| 15 | Round 2: the record overflowed its size | BLOCKER | moot: the record is back to 6,240 bytes |
| 16 | "No in-place upgrade" asserted without support; R-23 missing | MAJOR | fixed: left to the owner (section 6); R-23 added |
| 17 | Smaller items: incarnation field, init copy at generation 0, format refusal only for a valid copy, citation wording (CockroachDB, PostgreSQL), section 2 precision, stream mapping | MINOR | fixed |
| 18 | Per-stream checkpoint scope (2.4) | MINOR | node-wide kept, recorded as a choice |
| 19 | Round 3: the floor check used `start_lsn`, but the checkpoint is fuzzy (images up to `max_lsn`) | BLOCKER | fixed: `L ≥ max_lsn`; `max_lsn` in the manifest |
| 20 | Round 3: recovery could not build `H` (covered segments are skipped; `t_k` with no retained record) | MAJOR | fixed: a separate pass; `t_k` from `H` with its floor |
| 21 | Round 3: the R6.4 test looked only at the term; catch-up past a reclaimed segment looped | MAJOR | fixed: floor-LSN case; catch-up maps to R6.4; revision text updated |
| 22 | Round 3: reopen-without-unlink held only after the clear, and reused stale blocks | MAJOR | fixed: no writer open unlinks; truncate to the header |
| 23 | Round 3: a checkpoint or promotion could see a closed writer between the clear and the reopen | MAJOR | fixed: step 2.6 before the clear |
| 24 | Round 3: gate publication order, positioning lock, completion thread, spare `prepare()` race, one incomplete flag for many streams, revision text | MINOR | fixed |
