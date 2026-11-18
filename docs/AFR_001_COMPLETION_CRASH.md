# Crash consistency of truncation completion (AFR-D-06 4.3): preregistration and results

Status: **PREREGISTERED.** This plan, the model `spec/tla/Completion.tla` and the configuration
`spec/tla/Completion.cfg` were committed and pushed before any counted run. Section 5 is filled in after the runs and
changes nothing above it.

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

## 5. Results

(Filled in after the runs.)

## 6. Review

(Filled in after the review.)
