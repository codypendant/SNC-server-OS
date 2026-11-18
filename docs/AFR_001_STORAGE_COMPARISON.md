# AFR-001: storage-system comparison for D-02, R-23 and O-FLOOR (research, PROPOSED)

Status: **research and documentation only.** Asked for by Codex's review of #160 (`docs/handoff/PR160_Codex_Review.md`
section 5). It selects no D-02 mechanism, does not resolve R-23, and does not discharge O-FLOOR. Its recommendation is a
**promising direction, not an accepted design.** Nothing here authorizes C++ or Rust work, X-2, R640 runs, experiments
or migration work.

**Labels:**
- [FACT-SRC] was read in upstream source at the pinned tag, with a short quote in the evidence notes;
- [DOC] comes from official documentation, changelogs, commit messages or papers;
- [WF] was read through a fetch helper that extracts quotes, so it is not byte-verified;
- [INF] is inferred here;
- [HYP] is an untested guess.

**Evidence notes.** Each claim below is backed by one of these files, which give the paths, functions, quotes, and
what could not be fetched:
- `docs/prior_art/ROCKSDB_WAL_TRACKING.md`: RocksDB v9.7.4 (3c27a3dd) and v11.13.2 (17b624b5).
- `docs/prior_art/ETCD_CONSISTENT_INDEX.md`: etcd server v3.5.0, v3.5.2, v3.5.3, v3.5.6 and v3.6.0, and
  go.etcd.io/raft v3.6.0.
- `docs/prior_art/COCKROACHDB_TIKV_TRUNCATION.md`: CockroachDB v24.3.0 and tikv/raft-engine 0.4.2.
- `docs/prior_art/POSTGRES_LMDB_KAFKA_RAFT.md`: PostgreSQL REL_17_2, LMDB_0.9.31, Kafka 3.8.0, the Raft dissertation
  (0c10ea1b), and CTRL (FAST '18).

**Limits of the evidence.** github.com issue and PR pages, usenix.org and cwiki.apache.org returned 403 to the
researchers. Issue contents are therefore known only through code comments, changelogs and commit messages, and are
marked UNVERIFIED where the notes say so. KIP-101, KIP-279 and the CTRL paper are [WF]. File systems (ZFS, btrfs,
ext4/jbd2), SQLite and dm-integrity were not researched; where they are mentioned they are [HYP].

## 1. The three problems, restated

- **D-02** (G-14(b)). After a crash, a reader cannot tell an unwritten tail from an acknowledged tail that was silently
  lost: a lost write, a zeroed block, missing newest flushes. The candidates (X-2 prereg) are:
  - (a) a durable per-flush watermark;
  - (b) self-describing flush frames with a per-segment nonce;
  - (c) both.
- **R-23.** An A/B metadata copy silently falls back to an older valid copy after the newer acknowledged write is lost
  or damaged. G-14(b) names this case too ("a superblock rolled back to its older copy").
- **O-FLOOR.** The compaction floor (`retain_from`, `floor_term` in the manifest) must be crash-consistent with segment
  reclaim, catch-up and remapping (`docs/AFR_001_D06_BOUNDARY.md` section 3). It is argued, not model-checked.

## 2. Mechanism matrix

| System, mechanism | Persistent evidence | Write → sync → ack → recovery order | Detects | Does not detect | Cost data |
|---|---|---|---|---|---|
| **PostgreSQL** pg_control | one 8 KiB file, CRC last, data under 512 B "for atomic disk writes" [FACT-SRC] | overwrite in place, fsync; any error PANIC [FACT-SRC] | a torn or corrupt file: FATAL, no fallback [FACT-SRC] | a lost write leaving an older CRC-valid file [INF] | none |
| PostgreSQL checkpoint / WAL removal | checkpoint record in WAL; pointer in pg_control | `CheckPointGuts` → checkpoint record `XLogFlush` → `UpdateControlFile` → `RemoveOldXlogFiles` [FACT-SRC] | an unreadable checkpoint record: PANIC; the secondary-checkpoint fallback was removed in PG 11 ("We now just fail") [FACT-SRC, DOC] | a rollback pointing at WAL that still exists (benign replay) [INF] | none |
| PostgreSQL page headers | `xlp_pageaddr`, `xlp_sysid`, `xlp_tli`, `xl_prev`, record CRC [FACT-SRC] | in every WAL page | stale recycled segments, foreign or out-of-timeline pages, torn records [FACT-SRC] | **a lost acknowledged tail**: crash recovery reads with `ReadRecord(..., LOG, ...)`, so the first invalid record ends replay [FACT-SRC] | none |
| PostgreSQL `minRecoveryPoint` | LSN in pg_control | advanced before data pages are written during archive recovery; FATAL "WAL ends before consistent recovery point" [FACT-SRC] | WAL ending before a durable watermark, **only in archive or backup recovery** ("Zero when not doing archive recovery") [FACT-SRC] | anything in local crash recovery | none |
| **RocksDB** `track_and_verify_wals_in_manifest` (default off) | MANIFEST `WalAddition{number, synced size}`, `WalDeletion` floor [FACT-SRC] | an inactive WAL's size is recorded only when it is synced after becoming inactive; the live WAL only `FinishSync()`; `CheckWals` skips WALs with no synced size [FACT-SRC] | a missing or shortened **closed** WAL **that had a synced size recorded** [FACT-SRC] | **the live (newest) WAL**, "not tracked for performance/efficiency reasons" [DOC]; WALs never synced while inactive; zeroing at the same size (size only, no checksum) [FACT-SRC] | tracking every live sync halved db_bench fillsync, 1287 to 629 ops/s; reverted (#10087 → #10330, commit c8a70ed4) [DOC] |
| RocksDB `track_and_verify_wals` (9.11+, experimental) | first record of each WAL: predecessor's number, size, last seqno [FACT-SRC] | written at WAL switch; durable with the new WAL | a lost, truncated or extended predecessor, without needing a sync [FACT-SRC] | **the newest WAL** (no successor) [INF]; disabled in the crash test at v11.13.2 [FACT-SRC] | fillrandom on /dev/shm 310,517 vs 311,469 ops/s, no sync [DOC] |
| RocksDB recyclable records | 32-bit log number inside each record's CRC [FACT-SRC] | in every record | stale recycled bytes (`kOldRecord`) [FACT-SRC] | unwritten vs lost tail; a zeroed header is skipped without a report in every mode [FACT-SRC; replay past it HYP] | none |
| RocksDB CURRENT / MANIFEST | CURRENT names the MANIFEST: tmp, rename, directory fsync [FACT-SRC] | MANIFEST replayed tolerating a corrupt tail [FACT-SRC] | a rolled-back MANIFEST that lists WALs deleted since: "Missing WAL" when tracking is on [FACT-SRC, INF] | **a MANIFEST or CURRENT rollback** that names no reclaimed file: no external generation [INF] | none |
| **etcd** consistent index (bug #13766) | `consistent_index`, `term` in the bbolt `meta` bucket [FACT-SRC] | v3.5.0-3.5.2: the index was set before the apply and saved by a background commit, so it could precede its effects. v3.5.3: the index is staged and promoted inside the apply's write lock, or set directly in a `defer` when no v3 apply ran [FACT-SRC] | — (a prevention, not a detector) | the bug itself: "For single member cluster it is totally undetectable" [DOC] | none |
| etcd WAL | CRC chained across records and segments; a CRC record opens each segment [FACT-SRC] | `Save` fsyncs before followers send; snapshot file, then WAL snapshot record [FACT-SRC] | torn last record (`ErrUnexpectedEOF` repaired once by truncation), splices, missing or out-of-order segments [FACT-SRC] | **a lost acknowledged tail**: a zeroed tail reads as a clean end; no watermark outside the WAL [FACT-SRC, INF] | none |
| etcd segment purge | file locks released by `ReleaseLockTo` after the snapshot record is durable [FACT-SRC] | purge stops at the first locked file [FACT-SRC] | — | — | none |
| **CockroachDB** log truncation | `RaftTruncatedState` | default strongly coupled: in the same Pebble batch as the applied state. Loosely coupled (off by default, #78412): applied only after `RangeAppliedState` is durable, then committed unsynced, because "loss of a truncation means we have more of the suffix" [FACT-SRC] | — | — | none |
| CockroachDB entries and HardState | one synced batch before the ack [FACT-SRC] | — | commit index beyond the log: fail-stop "Was the raft log corrupted, truncated, or lost?" [FACT-SRC] | **a lost tail together with its HardState** (consistently shorter log); whole-engine rollback [INF] | none |
| CockroachDB snapshot apply | one SST: HardState, `TruncatedState{meta.Index}`, data; asserts applied = snapshot index [FACT-SRC] | atomic ingest | — | — | none |
| **raft-engine** (TiKV) | per-file seq XORed into the checksum; floor is a `Compact` log record, no durable first index [FACT-SRC] | purge only below every group's minimum referenced file, never the active file [FACT-SRC] | a missing middle file, **only for a region whose entries span the gap within one queue**: panic "has a hole" [FACT-SRC] | across the rewrite/append merge (`allow_hole = true`, older entries dropped); regions with no entries on both sides; **a missing oldest file** (looks like compaction); a lost tail on a batch boundary; default mode truncates a corrupt tail with only a warning [FACT-SRC, INF] | none |
| **Kafka** recovery point and leader epochs | `recovery-point-offset-checkpoint`; `leader-epoch-checkpoint` (epoch → start offset) [FACT-SRC] | topic partitions: flush defaults `Long.MAX_VALUE`; "use replication for durability" [FACT-SRC, DOC]. The KRaft metadata log was not checked | torn data above the recovery point; divergence repaired from the leader (KIP-101, KIP-279) [FACT-SRC, WF] | a lost tail; damage below the recovery point [INF] | none |
| **LMDB** meta pages | two meta pages; the pick is `mm_txnid` only, **no checksum in `mdb.c`** [FACT-SRC] | data sync, then meta write [FACT-SRC] | — | a lost or torn newer meta: silent fallback, dropping an acknowledged commit [INF, DOC "may undo the last committed transaction" under NOMETASYNC] | none |
| **Raft** | a term on every entry; snapshot keeps last index and term [PAPER] | persist before responding [PAPER] | — | storage loss is outside the model: a server that loses state "cannot safely rejoin the cluster with its prior identity" [PAPER] | — |
| **CTRL** (FAST '18) | a checksummed per-entry identifier record, stored apart from the entry [WF] | not ordered before the entry, to save an fsync [WF] | corruption vs crash for every entry except the last [WF] | **the last entry**: "we cannot determine whether it was a crash or a corruption"; resolved by asking peers [WF] | 8-10% on disks, 4% worst case on SSD, as reported [WF] |

## 3. Failure-coverage matrix (single node, local evidence only)

Rows are faults under SNC's single-fault storage model (`docs/SNC_GUARANTEES.md`, the media-damage row: one lost,
torn, zeroed or misdirected write on one node, plus crashes). "Refuse" means fail closed with a named error (G-14(a)).
"D-06 as designed" is the conditionally accepted design, not today's C++. The (a), (b), (c) columns give X-2's
**expected** outcomes (`docs/X2_PREREG.md`, case IDs in brackets); nothing has been run. Peer-based repair is in
section 4.

| Fault | PG | RocksDB (`..._in_manifest` on) | etcd | CRDB | raft-engine | LMDB | D-06 as designed | + (b) frames | + (a) watermark | + (c) both |
|---|---|---|---|---|---|---|---|---|---|---|
| Torn last flush, not acknowledged | discard | discard | repair | discard | discard | — | discard | discard | discard | discard |
| Stale bytes from an earlier run (C3s, C6) | refuse or end | `kOldRecord` = end | CRC chain | — | checksum | — | silent [INF] | detect | detect | detect |
| Mid-log bit flip | end of WAL (LOG) | per recovery mode | refuse | — | per mode | — | refuse (AFR-S-1) | refuse | refuse | refuse |
| **Lost newest acknowledged flush (C3, C4)** | **silent** | **silent** | **silent** | **silent** | **silent** | — | **silent** (R-01) | **silent** | detect | detect |
| Zeroed newest flush (C2) | silent | silent | silent | silent | silent | — | silent (R-01) | detect | detect | detect |
| Whole newest segment missing (C14) | silent [INF] | silent (live WAL) | silent [INF] | — | silent [INF] | — | silent [INF] | **silent** | detect | detect |
| Misdirected data write over the newest data (C16a) | silent | silent | CRC chain [INF] | — | checksum | — | silent | detect | detect | detect |
| Misdirected **watermark** write over the only copy of data (C16b) | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | **silent** | detect |
| Missing middle segment | — | detect, if a synced size was recorded | detect | — | detect within one queue only | — | refuse (gap) | refuse | refuse | refuse |
| Missing oldest retained segment | PANIC | detect, if a synced size was recorded | refuse | — | **silent** | — | refuse (FL-floor) | refuse | refuse | refuse |
| Metadata write lost, older valid copy remains | silent unless the older state names removed WAL (then refuse) | silent unless the older MANIFEST names deleted WALs (then refuse) | — | — | — | **silent** | **silent (R-23)**, except FL-floor side effects | silent | silent | silent |
| Floor advance lost | — | — | — | harmless | harmless | — | harmless (rule 4.5) [INF] | — | — | — |
| Delete before the floor is durable | prevented by order | prevented | prevented by locks | prevented | prevented | — | prevented (rules 1-2) [INF] | — | — | — |

**What the matrix shows [INF]:**
- **None of the systems researched detects a lost newest acknowledged flush from local evidence.** Each reads it as a
  clean end of log. They differ only in whether replication covers the loss. File systems that store a child's
  checksum and birth generation in a durable parent (ZFS block pointers, btrfs "parent transid verify failed") detect
  a lost *child* write, but appear to fall back silently when the newest *root* is lost [HYP, not researched]. The
  claim is therefore about the newest tail or root write.
- **Frames (b) defeat stale, torn and misdirected data, never a lost write.** PostgreSQL's page addresses, RocksDB's
  recyclable records, raft-engine's seq-in-checksum and CTRL's identifiers are all (b), and none closes D-02.
- **Every local detector of a lost write that we found is a second, independent durable record written before the
  data is trusted:** PostgreSQL's `minRecoveryPoint` (archive recovery only), RocksDB's reverted per-sync MANIFEST
  record, and, by report, parent-checksum file systems [HYP]. That is candidate (a).
- **None of the systems researched detects metadata rollback locally.** PostgreSQL keeps one copy and refuses on
  damage. LMDB and CTRL keep two copies chosen by a counter, and RocksDB keeps an append-only MANIFEST. All of them
  fall back silently after a lost write. Rollback is caught only as a side effect, when the older state points at files
  already deleted. SNC's FL-floor has the same partial detection.

## 4. With peers: what replication can and cannot cover

Kafka, etcd, CockroachDB and CTRL repair a lost local tail from peers: leader overwrite, epoch-based truncation, or
CTRL's "have" and "dontHave" votes. For SNC [INF]:
- **A standby's lost tail.** The primary knows how far each standby acknowledged. Comparing that with the standby's
  reported end in the R6.2 exchange could detect the loss, but only while that primary is alive. The R6.2 binding
  already carries `end_lsn` and `last_term` (D-06 4.4). Whether the primary keeps the per-member ack point durably is
  not defined. **UNRESOLVED.**
- **Under the frozen R2′, peers cannot repair a lost flush on the promoted member, for any size of the in-sync set
  `S`.** Every member of `S` acknowledges every write; `m` only controls admission (G-12).
  - If `|S| = 2` and the primary is lost, the surviving member is the only copy left. If it silently lost the flush,
    no copy exists.
  - If `|S| ≥ 3`, other members do hold the flush. But P5 makes any `j ∈ S` eligible and "the witness compares no
    LSNs", and R6.3′ then reconciles the other members toward the promoted `j` (G-15(2)). The surviving copies are
    truncated away.
  - The same holds in 4.2, the re-promotion of a restarted sole primary: reconciliation against it "can delete the last
    copy of an acknowledged write" (`docs/handoff/PR155_Owner_Decision.md`).
- **So G-14(b) for HA_DURABLE writes needs a local detector on the member that is promoted.** Peers alone are not
  enough, whatever `m` is. P5's silent-loss clause and 4.2 are excluded from the freeze for exactly this reason
  (`docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md` sections 4-5, obligation O4).
- The Raft dissertation's rule, that a server which lost state cannot rejoin under its old identity, is the same
  requirement stated as a rule. Detection must come first.

## 5. Candidate approaches SNC could adapt

| # | Candidate | Prior art | Covers | Does not cover | Cost, expected | Fit with D-06 and the frozen protocol |
|---|---|---|---|---|---|---|
| K1 | **(b) frames**: magic, segment nonce, term, first and last LSN, CRC per flush; every segment formatted (X-2 "F") | PG page header; RocksDB recyclable record; raft-engine signature; CTRL identifiers | stale, torn, zeroed and misdirected data; splices (C2, C3s, C6, C16a) | a lost write (C3, C4); a missing newest segment (C14) | **unmeasured [HYP].** Formatting every segment roughly doubles device bytes and makes a spare miss synchronous; a lost format write (C15) is a refusal on a healthy log, an operator case under G-48. RocksDB's predecessor record (no sync, /dev/shm) measures none of this | extends the per-record term (item 4) and the segment header (item 10, D-06 4.2). Must tell a half-done truncation from damage (O4): void records above the next segment's name (item 8) and the reopen that rewrites the header (item 9) are not yet analyzed (U10) |
| K2 | **(a) per-flush watermark**, outside the segment, per stream | PG `minRecoveryPoint` (archive only); RocksDB #10087 (reverted) | **a lost newest flush** (C3, C4, C14), if the order below holds [INF] | a misdirected watermark landing on data (C16b); the correlated loss of both writes (outside the single-fault model); damaged watermark slots refuse a healthy log (C8, required by G-14(a)) | **unmeasured.** At low queue depth a pipelined watermark still adds a full synced write to the ack latency (X-2 WP), so the RocksDB 2x warning applies to pipelined writes too, not only serial ones. The figure does not carry over in either direction: a MANIFEST append with fsync through a file system is not an O_DIRECT overwrite of a preallocated slot [INF] | **requires a truncation rule not yet designed.** D-06 section 8 lowered the watermark in the same write as the truncation intent; moving it out of the node record splits that state across two durable places and touches boundary item 1 (one atomic metadata authority per node). See the windows below and U11 |
| K3 | **(c) = K1 + K2** | none complete | the union; C16b caught by the frame [INF] | correlated multi-write loss | the sum | the union of both fit problems |
| K4 | **Peer evidence in R6.2**: the primary compares a member's reported end with that member's acknowledged point | Kafka KIP-101/279; CTRL; etcd's commit-range panic | a standby's lost tail while its primary lives | the promoted member's loss (section 4) | none on the write path | needs a defined, durable per-member ack point; touches frozen R6.2 text. **UNRESOLVED** (U4) |
| K5 | **Mirrored record writes (R-23)**: every record write goes to copy A, then copy B, each synced; act only after both | not found in the systems researched | **a single lost or torn copy write, only until the next record write** [INF] | a lost B write followed by a crash that tears the next A write: only the older B remains, and the node silently rolls back past an action it took (one media fault plus a crash; see below). Two lost writes; whole-store rollback | one extra 8 KiB synced write per control event, and twice the `meta_mu` hold, including R4 on the promotion path [HYP] | **conflicts with stated D-06 premises**: "a write touches only the copy that is not current" and "two copies never share a generation" (D-06 4.1), on which M7 and the boundary's O-FLOOR argument rest. Needs a tie rule, a boot re-mirror before any action, and a fresh crash argument (U3) |
| K6 | **Boot cross-checks against independent durable evidence (R-23, partial)**: record term ≥ the highest term in any WAL record; `active_ckpt` against manifests | etcd's verify checks (index ≤ WAL commit, ≥ snapshot index), test-gated [FACT-SRC] | rollbacks that leave newer evidence elsewhere (the term) | rollbacks with no newer evidence (an intent clear) | a boot-time scan only | the term check holds only if R4 persists the term before any entry of that term is accepted, seeds included (D-06 4.2 says so for R4) [INF]. A `next_lease` check was considered and dropped: standbys replicate leases minted elsewhere and take `max(next_lease, max_seen + 1)` (`docs/CORE_CONTRACTS.md` E11), so a higher lease in the WAL is legitimate |
| K7 | **External anchor (R-23, whole store)**: the witness records each node's record generation on control events | none found. P7 is only a self-attestation that the durable term is ≤ t; a term that is too low is caught by the R7 boot read | whole-store rollback | witness unavailability couples to control events | one witness round trip per control event [HYP] | needs witness protocol work; out of scope here |
| K8 | **O-FLOOR order and rules** | PG `CreateCheckPoint`; RocksDB `WalDeletion` in the flush edit; CRDB loosely coupled truncation; etcd lock floor; CRDB atomic snapshot apply | confidence in the boundary rules 1-7 | it does not verify them | none | already the D-06 boundary design |

**K2's required order [INF, matches X-2 WP].** (c) is sound only if:
1. watermark(k) is submitted after data(k) has completed durably;
2. the acknowledgement of k waits for watermark(k);
3. at most one watermark write is in flight, with later flushes coalesced into it;
4. on a standby, the acknowledgement to the primary also waits for the standby's own watermark, which adds HA-path
   latency.

If data and watermark are submitted concurrently, "watermark durable, data not" becomes a legitimate crash state.
Recovery would then have to accept it, and a lost data write would be silent again. The slot must also carry the
stream and space identity, so that a watermark misdirected from another stream is recognised (U5).

**K2 and legitimate truncation [INF].** A truncation (R6.3, S2′) moves the end backwards, so the watermark must come
down with it. With the watermark outside the node record, neither order is safe on its own:
- **Lower first:** a crash before the intent is written leaves acknowledged records above the watermark with no second
  record. One loss there is silent.
- **Lower after the intent:** if the lowering write is lost and a crash comes after the clear but before the next
  watermark, recovery sees the watermark above `L` and refuses a healthy node (an operator case, G-48). Once appends
  resume from `L+1`, the LSN is reused and the CRC check also mismatches.
- An idea not checked: if the watermark carries the term of the record it covers, a watermark with term ≥ `t_j` above
  `L` would mean the clear was rolled back, while a lower term would mean the lowering is still pending. This could
  also detect a rolled-back clear, which D-06 alone does not: completion would rerun and delete acknowledged writes
  after `L`. **UNRESOLVED** (U11).
- The watermark can also point at a record already reclaimed below the floor (after a full checkpoint, or an empty
  `L+1`). Then there is no CRC to compare. **UNRESOLVED** (U12).

**K5's failing sequence [INF].** Generation g+1 goes to A (succeeds), then to B, and the B write is silently lost. The
node acts on g+1, with A = g+1 and B = g. The next write, g+2, goes to A first, and a crash tears it. Recovery finds
A invalid and falls back to B = g, rolling back g+1 after the node acted on it. Two copies overwritten in a fixed
order cannot hold the newest generation while one of them is being rewritten. A layout that never touches the
newest generation's copies, such as two mirrored pairs (four slots) written alternately, is a direction to examine,
not a result [HYP].

**Lessons for SNC's existing design [INF]:**
- **etcd #13766 is SNC's metadata-atomicity risk in another form.** A progress pointer persisted by a path that does
  not hold the apply's lock got ahead of its effects, and on one node this was "totally undetectable". D-06's rule
  (record writes under `meta_mu`; `active_ckpt` named only after the manifest and parts are durable; the clear after
  completion) is that same discipline. Any future progress field, such as a watermark or `ack_p` on disk, must follow
  it.
- **CockroachDB's "a lost floor advance is harmless"** is the property boundary rule 4.5 relies on (segments below the
  floor are ignored and deleted). raft-engine shows the opposite risk: without a contiguity check, a missing oldest
  file passes as compaction. **Keep FL-floor.**
- **CockroachDB snapshot apply** sets floor, applied index and log state in one atomic step. This supports boundary
  rule 5 (the seed writes its own floor).
- **PostgreSQL removed its fallback to an older checkpoint.** SNC's A/B record is a standing fallback, which is why
  R-23 is wider for SNC than for PostgreSQL.

## 6. Recommended direction for the eventual D-02 decision (promising, not accepted)

**Direction: (c) as the leading candidate.** It means K1 frames, plus a K2 per-stream watermark under the order in
section 5, on every member, with the promoted member's own watermark as the evidence that matters. K4 is an extra
check while the primary lives, not a substitute.

Why [INF]:
1. The prior art shows that frames alone never detect a lost acknowledged flush (C3, C4, C14), and under the frozen R2′
   no peer repairs a loss on the promoted member, for any `|S|` (section 4). Without K2, G-14(b) cannot hold for any
   class, and G-14 is not weakened.
2. K2 alone fails the misdirected-write case C16b, as X-2 already predicts. K1 closes that case.
3. Under the single-fault model, two ordered, independent writes leave at least one record of the newest acknowledged
   flush. That is the principle behind PostgreSQL's `minRecoveryPoint`.

**Costs this direction brings, before any measurement:**
- the latency of one extra synced write on every acknowledgement at low queue depth, on the primary and the standby;
- formatting every segment (K1);
- refusals of healthy logs by design (C8, C15), which need an operator under G-48;
- a member that detects a loss refuses instead of serving, which is an availability cost.

**What must be true before this can be accepted:**
- Its cost measured. X-2 exists for this (WS, WP, F, FWS, FWP) and is **not authorized**. The owner's limits are 10%
  throughput and 10% p99 acknowledgement latency; at QD1 the watermark may exceed the latency limit [HYP].
- A correctness argument, or X-2's offline correctness run, over C1-C16.
- A truncation rule for the watermark (U11) and a frame rule for half-done truncation (U10).
- K4's per-member ack point defined, if it is kept.
- If the measured cost is unacceptable, the owner chooses: pay it, or refuse to acknowledge writes as LOCAL_DURABLE
  when no second durable record backs them, which touches G-12 and the open D-14. Weakening G-14(b) is not an option.

**For R-23, a separate direction.** K6's term check is promising and cheap. K5 as written is **not** sound under one
lost write plus a crash. A four-slot variant may be, but that is unexamined. Whole-store rollback (K7) stays out of
scope. **R-23 stays an unresolved blocker.**

## 7. Compatibility with the conditionally frozen protocol

- **R2′ (P1-P7, S1-S5, R6.3′):** the frozen text is not changed by K1, K2, K5 or K6, which are storage-local. But
  closing O4 means bringing back P5's silent-loss clause and 4.2, which are **excluded** from the freeze. That is new
  protocol text, a recorded revision, and an owner decision (`docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md` section 5).
  This document does not do it. K4 would add a field to R6.2 and is not part of the direction.
- **R9-A to R9-E:** unchanged. R9-F stays out. `ack_p` on disk, if R9-F ever needs it, falls under the etcd lesson
  (section 5).
- **m = 2:** kept. The direction does not depend on `m` (section 4). The owner's planned revisit of `m` is not
  affected by this document, but its cost (the standby watermark) belongs in that revisit [INF].
- **D-06 items 4-14 (conditionally accepted):** each change below would be a recorded revision under the conditions in
  `docs/handoff/PR160_Owner_Decision.md`, not a silent change:
  - K1 extends item 4's per-record term and item 10's segment header (D-06 4.2), and interacts with items 8 and 9 (U10).
  - K2 outside the record revises the D-06 section 8 note on option (a) and needs a truncation rule (U11).
  - K5 would change item 5's write rule and the premises of its crash argument (U3).
- **G-14:** not weakened. G-14(a) is served by K1 and today's fail-closed rule (AFR-S-1). G-14(b) needs K2 **and** an
  R-23 design (K5/K6, unresolved), because G-14(b) also names a superblock rolled back to its older copy.

## 8. Unresolved questions and the verification each needs

| # | Question | Needed |
|---|---|---|
| U1 | Cost of K2, serial and pipelined, at QD1 and higher, on the target disk; cost of K1's formatting | X-2 (not authorized) |
| U2 | Does (c) detect every C1-C16 fault, including C16b? | X-2 offline correctness run, or an argued case table reviewed by Codex |
| U3 | A safe mirrored or four-slot record layout: lost write plus crash, a tie rule (equal generations with different bytes refuse), a boot re-mirror before completion acts, M7 and the O-FLOOR argument re-checked, the cost to R4 | design, then a crash argument; a small model only if the argument is contested |
| U4 | Should the primary keep a durable per-member ack point (K4)? | protocol question for R6.2; touches frozen text, owner decision |
| U5 | Where K2's watermark lives (per-stream slot, separate file, second device), how it is torn-safe, and its stream and space identity | design, then X-2 |
| U6 | Does a zeroed header that RocksDB skips have an SNC analogue under K1? | design review of the frame format |
| U7 | O-FLOOR | the bounded reclaim model proposed in `docs/AFR_001_D06_BOUNDARY.md` section 3, or an equivalent argument accepted by Codex, before checkpoint or reclaim conformance work |
| U8 | Whole-store rollback (K7) | out of scope; owner decision whether it is ever covered |
| U9 | Issue pages not readable here (#13766 details, #78412, #99273, #10087 discussion); file-system prior art (ZFS, btrfs) | read from a browser or upstream source if Codex wants them; the conclusions above do not rest on them |
| U10 | K1 frames versus truncation: void records above the next segment's name (item 8), the reopen's header rewrite (item 9), a half-done completion | design review against D-06 4.3 |
| U11 | Lowering K2's watermark on a legitimate truncation, with the watermark outside the node record; whether a term in the watermark detects a rolled-back clear | design, then a crash argument |
| U12 | A watermark that points below the floor: what evidence replaces the CRC (for example `floor_term`) | design |
| U13 | The protocol text needed to bring P5's silent-loss clause and 4.2 back into the freeze | new protocol text; owner decision |

## 9. Review record

Three independent reviewers (Opus), each told to break the work. Their findings and what changed:

| Reviewer | Severity | Finding | Outcome |
|---|---|---|---|
| Source fidelity (18 claims checked; local PG, LMDB and Kafka copies byte-identical to the pinned tags) | — | No misquotes. The two claims the conclusions rest on most were confirmed | — |
| Source fidelity | MAJOR | raft-engine "detect" for a missing middle file is overstated: recovery merges with `allow_hole = true` | matrix cells narrowed |
| Source fidelity | MAJOR | RocksDB tracks a closed WAL only if it was synced after becoming inactive | rows and cells narrowed; option named in the header |
| Source fidelity | MINOR | rollback side effects under-credited; etcd's direct `defer` path dropped; PG "LOG" cited at the wrong function; a label mismatch; Kafka scope | fixed here and in the evidence notes |
| Architecture | MAJOR | K2 outside the record conflicts with lowering the watermark in the same step as a truncation | "fits" removed; windows stated; U11 |
| Architecture | MAJOR | the failover argument was pinned on `m = 2`; under P5 and R6.3′ it holds for any `|S|`, and for 4.2 | section 4 rewritten |
| Architecture, adversarial | BLOCKER (adversarial) / MAJOR | K5 breaks under one lost copy write plus a crash, and removes D-06 4.1 premises | K5 narrowed; failing sequence stated; U3 widened |
| Architecture | MAJOR | "R2′ unchanged" hid that closing O4 brings back excluded text | section 7 rewritten; U13 |
| Architecture | MAJOR | G-14(b) also needs R-23 closed | section 7 fixed |
| Architecture | MAJOR | K1 skipped the half-done-truncation dependency | U10 |
| Adversarial | MAJOR | the pipelining order was not stated | order stated in section 5 |
| Adversarial | MAJOR | K5 boot acts on a single copy | re-mirror before action, in U3 |
| Adversarial | MAJOR | the `next_lease` check fires falsely (E11) | dropped from K6 |
| Adversarial | MAJOR | the watermark can point below the floor | U12 (UNRESOLVED) |
| Adversarial | MAJOR | the RocksDB 2x was misapplied; K1's cost understated; the standby ack and availability costs were unstated | costs restated in sections 5 and 6 |
| Architecture, adversarial | MINOR | P7 wording; CTRL figures; "small" was a performance claim; the "witness" name clash; missing C14 and C16 rows; item number for the segment header; column naming; "no system" too broad; the watermark slot's identity | all fixed |

UNRESOLVED after review: U3, U11 and U12 are design gaps that no reviewer could close, and the term-in-watermark idea
(U11) is untested.
