# Crash consistency of truncation completion (AFR-D-06 4.3): preregistration and results

Status: **COMPLETED.** Results are in section 5 and the review in section 6.

How this document was built:
- Sections 1-4, the model `spec/tla/Completion.tla` and `spec/tla/Completion.cfg` were committed and pushed before any
  counted run (554ec32).
- Section 4a, revision 2, was committed and pushed before any revision-2 run (18d1891).
- Nothing above section 5 was changed after the runs it governs.

Authority:
- Codex's review of #157 (`docs/handoff/PR157_Codex_Review.md`) asks for "the proposed small crash-consistency model,
  with preregistered bounds and independent QA, before accepting the completion procedure".
- The coordinator's relay of 2026-11-18 asks for the same. That relay is cody's standing rule for pasted reviews.

This document authorizes no C++, Rust or R640 work, and it accepts and freezes nothing.

## 1. Question

Codex's concern 2: can a crash at any point of the completion procedure expose a WAL tail that was voided earlier?

The procedure has these steps:
- the spare;
- segment deletion;
- the directory fsync;
- creating segment `L+1`;
- the clear;
- reopening the writer.

The model also covers today's writer reopen (`storage.cpp:521-527`).

## 2. Model

`Completion.tla` models one stream on one node.

**File-system model:**
- **File contents are durable when written.** The design follows every write with `fdatasync`.
- **Directory operations are pending until a directory fsync.** These are create, unlink and rename. A crash keeps
  any subsequence of the pending operations, applied in order, and each rename is atomic. This is a stronger adversary
  than Linux journaling file systems, which usually keep a prefix.
- **The metadata record is two copies (A/B).** A write goes to the copy that is not current. A crash during the write
  can tear it, which leaves that copy invalid (`TornCrash`).

**Actions:**
- the writer: append, rollover onto a spare whose rename is not synced, rollover by direct create, spare preparation;
- the intent (4.3 step 1);
- completion steps 2.2, 2.3 and 2.4, then the clear (step 3), then the writer reopen;
- crash, at any step, including during recovery;
- recovery: read the record, complete a pending intent, adopt a spare (`storage.cpp:797-812`), scan with the void rule
  (`scan_stream`, including its fatal cases), and open the writer.

**Properties:**
- `ServedLogExact`: whenever the node serves, the log that a recovery would read equals the acknowledged log that has
  not been truncated. This rules out both a revived voided record and a lost acknowledged record.
- `NoFatal`: recovery never stops with a fatal scan on a state the design can produce.

**Not modelled.** These are limits of this evidence:
- torn WAL writes inside a block;
- lost acknowledged writes or directory entries, and metadata rollback (D-02, R-23, which stays an explicit blocker);
- checkpoints, reclaim and `retain_from`;
- the contents of the state rebuild (step 2.5 is a no-op);
- more than one stream, and replication;
- the `meta_mu`/`control_mu_` concurrency (completion is a sequential process here);
- timing.

## 3. Lanes and expected outcomes (fixed before the runs)

Common to all lanes:
- Each lane is one TLC run: `-workers 4 -deadlock`, Java 21, the same `tla2tools.jar` as `spec/tla/README.md`.
- Cap: 2 hours per run. A run that hits the cap is **NOT COMPLETED**, never PASS.
- The TLC `states/` directory is deleted after each run.
- No bound is changed after a result is seen.

`MaxIno` is set high enough that `PInoBound` should hold (lane P3 checks this).

| Lane | Constants (others as `Completion.cfg`) | Invariant | Expected |
|---|---|---|---|
| D1 | `Completion.cfg`: MaxLsn 4, Cap 2, MaxTrunc 2, MaxCrash 2, MaxIno 18, all M* FALSE | Safety | PASS |
| D2 | MaxLsn 5, MaxCrash 3, MaxIno 22 | Safety | PASS |
| D3 | MaxLsn 5, Cap 3, MaxIno 20 | Safety | PASS |
| M1 | D1 + MUnlinkReopen (today's writer open: unlink, then create) | Safety | FAIL: a voided tail is revived, or a fatal scan |
| M2 | D1 + MSpareUnlink (completion unlinks the spare unconditionally) | Safety | FAIL: an acknowledged record is lost |
| M3 | D1 + MNoNewSeg (segment `L+1` is not created before the clear) | Safety | FAIL: a voided tail is revived |
| M4 | D1 + MClearEarly (clear before the files change) | Safety | FAIL: a voided tail is revived |
| M5 | D1 + MNoVoid (scan without the void rule) | Safety | FAIL: a voided record is read, or a fatal scan |
| P1 | D1 | PRecComplete | VIOLATED (recovery completes a pending intent) |
| P2 | D1 | PTwoTrunc | VIOLATED (serving after two truncations) |
| P3 | D1 | PInoBound | HELD (the inode bound does not cut the search) |

**Reading the results:**
- A design lane that FAILs is a defect in the design or in the model. It is analysed and reported. It is not tuned
  away.
- A mutation lane that PASSes means the model cannot see that step. It is reported as a gap, not as evidence that the
  step is unnecessary.

**Smoke runs.** Before this commit the model was smoke-checked at tiny bounds (MaxLsn 3 or 4, MaxTrunc 1, MaxCrash
1). Those runs fixed two modelling errors: a missing `UNCHANGED`, and a type clash in the scan result. They are not
counted. At those bounds the design passed. Four mutations failed. MSpareUnlink failed only once MaxLsn was 4, because
a spare can then hold records at or below `L`.

## 4. Independent review

After the runs, reviewers who did not write the model are told to break it. They look for:
- an unfaithful step;
- a property that holds vacuously;
- a crash outcome the file-system model leaves out;
- a design step the model skips.

Their findings and outcomes go in section 6.

## 4a. Revision 2 (preregistered after the first independent review, before any revision-2 run)

Revision 1 (commit 554ec32) is the model the lanes of section 3 ran against. Its results are kept in section 5 as
recorded. The first model review ran in parallel with those lanes. It found two gaps where the model cannot see a whole
class of bugs, plus smaller faithfulness gaps. Revision 2 fixes them. It is committed and pushed before any revision-2
run.

| Review finding | Revision 2 |
|---|---|
| The catch-up reader (`read_stream_range`) was never checked. A design with the void rule in `scan_stream` only would pass | `ServedRangeExact` added to `Safety`: for every range `f..t`, the reader returns exactly the acknowledged records. New mutation `MNoVoidRange` (void rule missing in the reader only) |
| The A/B record's CRC check was vacuous: a torn copy had generation 0, so picking by generation alone also passed | A torn copy now has generation `Best.g + 1` and a junk slot (`NoSlot` or `PENDING(0)`). New mutation `MNoCrc` (pick by generation, ignore the CRC) |
| The scan's header cases could not be reached, because create and header write were one step | `NewSegment` and the writer's create are split into a create (zero file) and a header write with fsync and directory fsync (`SegHeader`, `open_segment`). Probe `PZeroHdr` checks that a zero-header segment is reached |
| A rename whose source is missing cleared the target | Rename with a missing source is a no-op (ENOENT) |

**Known differences, kept and disclosed:**
- The model clears before it reopens the writer; the design text reopens first (2.6, then 3). Appends are gated in
  both orders, and the review found no consequence.
- The reopen truncation never drops a record, because torn WAL writes are not modelled.
- The first directory fsync (after the deletions) is not needed in the model: the second one makes every pending
  operation durable before the clear. The design keeps it. The model cannot show whether it is needed.
- A stale `TMP` file is not cleaned up in the model. The design should unlink a leftover `TMP` at recovery.

**Revision-2 lanes** (the same rules as section 3; D2's size is not repeated, see section 5):

| Lane | Constants | Invariant | Expected |
|---|---|---|---|
| R2-D1 | `Completion.cfg` (D1 bounds) | Safety | PASS |
| R2-D3 | MaxLsn 5, Cap 3, MaxIno 20 | Safety | PASS |
| R2-M1 to R2-M5 | D1 + one of MUnlinkReopen, MSpareUnlink, MNoNewSeg, MClearEarly, MNoVoid | Safety | FAIL |
| R2-M6 | D1 + MNoVoidRange | Safety | FAIL: the reader ships a voided record |
| R2-M7 | D1 + MNoCrc | Safety | FAIL: a torn copy is chosen |
| R2-P1, R2-P2 | D1 | PRecComplete, PTwoTrunc | VIOLATED |
| R2-P3 | D1 | PInoBound | HELD |
| R2-P4 | D1 | PZeroHdr | VIOLATED |

Correction to section 3's wording, made before the revision-2 runs: M2 may fail as a fatal gap (the spare's records
are gone, and a later segment follows) instead of as a silently lost record. Both are Safety violations.

## 5. Results

Every lane completed under its cap, and every outcome matched its preregistered expectation. No lane timed out. TLC
`states/` directories were deleted after each run.

**Scope of these results:**
- They hold only within the bounds and the abstractions of section 2.
- They say nothing about lost acknowledged writes or directory entries, or about metadata rollback. Those stay with
  D-02 and R-23 as explicit blockers.

**Revision 1** (model 554ec32):

| Lane | Result | Distinct states | Depth | Time |
|---|---|---|---|---|
| D1 | PASS | 3,706,729 | 57 | 34 s |
| D2 | PASS | 123,863,149 | 72 | 31 min 39 s |
| D3 | PASS | 9,097,360 | 60 | 1 min 30 s |
| M1 MUnlinkReopen | FAIL (Safety), as expected | 9,237 | 17 | 1 s |
| M2 MSpareUnlink | FAIL (Safety), as expected | 12,870 | 19 | 1 s |
| M3 MNoNewSeg | FAIL (Safety), as expected | 3,058 | 14 | 0 s |
| M4 MClearEarly | FAIL (Safety), as expected | 995 | 10 | 0 s |
| M5 MNoVoid | FAIL (Safety), as expected | 2,949 | 14 | 0 s |
| P1 PRecComplete | VIOLATED (reached), as expected | 173 | 7 | 0 s |
| P2 PTwoTrunc | VIOLATED (reached), as expected | 51,001 | 24 | 1 s |
| P3 PInoBound | HELD, as expected | 3,706,729 | 57 | 26 s |

**Revision 2** (model 18d1891):

| Lane | Result | Distinct states | Time |
|---|---|---|---|
| R2-D1 | PASS | 4,648,225 | 1 min 35 s |
| R2-D3 | PASS | 10,937,781 | 5 min 2 s |
| R2-M1 to R2-M5 | FAIL (Safety) each, as expected | 935 to 14,384 | ≤ 1 s each |
| R2-M6 MNoVoidRange | FAIL (Safety), as expected | 3,315 | 0 s |
| R2-M7 MNoCrc | FAIL (Safety), as expected | 8,232 | 1 s |
| R2-P1, R2-P2 | VIOLATED (reached), as expected | 356; 49,004 | ≤ 1 s |
| R2-P3 PInoBound | HELD, as expected | 4,648,225 | 31 s |
| R2-P4 PZeroHdr | VIOLATED (reached), as expected | 89 | 0 s |

**What the mutation traces show:**
- **M3, M4 (MNoNewSeg, MClearEarly).** After the clear, a crash revives the voided record `2v0`.
- **M1 (MUnlinkReopen).** A crash outcome that keeps only the unlink revives it.
- **M5 (MNoVoid).** The voided record is read with no crash at all.
- **M2 (MSpareUnlink).** It fails as a fatal gap. This is the correction noted in section 4a.
- **M6 (MNoVoidRange).** After truncation, completion, reopen and one append, the catch-up reader ships the voided
  record.
- **M7 (MNoCrc).** After a torn record write, recovery chooses the torn copy.

**Conclusion, within the bounds.** In the model, a crash at any intermediate step of completion leaves both readers
exact: the scan and the catch-up reader return exactly the acknowledged, untruncated log. No voided record is ever
read, and no fatal scan occurs on a state the design produces. Each guarded step is needed, because removing it lets
the checker break Safety, with one exception: the first directory fsync, whose need the model cannot show (section 4a).

**UNRESOLVED:**
- torn WAL writes inside a block;
- lost acknowledged writes or directory entries (D-02);
- metadata rollback (R-23);
- checkpoints, reclaim and `retain_from`;
- the state rebuild's contents;
- concurrency between `meta_mu` and `control_mu_`;
- more than one stream.

## 6. Review

Two independent reviewers (Opus) were told to break the work. They worked read-only, with their own scratch copies for
probe runs.

| # | Finding | Severity | Outcome |
|---|---|---|---|
| 1 | The catch-up reader was never checked | MAJOR | fixed in revision 2 (`ServedRangeExact`, M6) |
| 2 | The A/B CRC check was vacuous | MAJOR | fixed in revision 2 (torn copy at `Best.g + 1`, M7) |
| 3 | The scan's header cases could not be reached | MINOR | fixed in revision 2 (split create, P4) |
| 4 | The reopen truncation is never exercised | MINOR | disclosed (torn WAL writes not modelled) |
| 5 | The order of clear and reopen differs from the design; rename with a missing source; stale `TMP` | MINOR | rename fixed; the other two disclosed (4a) |
| 6 | The first directory fsync is invisible to the model; M2's failure form | MINOR | disclosed; wording corrected (4a) |
| 7 | `H_i` revision: condition 2 claimed refusal for the newest segment, which only D-02 can give | MAJOR | fixed: "not met for the newest segment until D-02" |
| 8 | The R-23 row did not name the node metadata record | MAJOR | fixed |
| 9 | Conditions read as verbatim but were extended; floor edge cases; stale wording; condition cross-reference | MINOR | fixed: verbatim quote plus "Realized as"; floor bullets in D-06 4.2 |

The reviewers also probed these cases and could not break them:
- the spare branches (adopt at or below `L`, unlink above it, adopt in recovery);
- a second truncation with `L2 > L1` while the straddling segment still holds a voided record, which is reached and
  safe;
- no stuck steps, so `-deadlock` hides nothing;
- recovery order against `engine.cpp`;
- the frozen text of 2026-11-17, which is untouched;
- whether anything claims D-06 is accepted, and nothing does.

Revision 2 was reviewed independently after #158 merged. That review narrows the conclusion above: the catch-up
reader is exact **while the node serves**, and readers during completion were not checked. The model's reopen
rewrites the header, which the design text did not say. Both are fixed in the design text; the findings and a
bounded rerun proposal are in `docs/AFR_001_D06_BOUNDARY.md` section 4.
