# MetaWatermark: a crash model for the D-02 crash design (preregistration and results)

Status: revision 1 **COMPLETED** (results section 6, review section 7). Revision 2 **COMPLETED** (preregistration section 8,
results section 9, conclusions section 10).

How this document is built:
- Sections 1-5, `spec/tla/MetaWatermark.tla` and `spec/tla/MetaWatermark.cfg` are committed and pushed before any
  counted run. Nothing in sections 1-5 changes after the runs it governs.
- A defect found later in the model is fixed in a new revision, preregistered the same way, and the old results stay
  recorded.

Authority:
- Codex's review of #162 (`docs/handoff/PR162_Codex_Review.md`), item 4: "one small, preregistered model covering
  metadata generations and watermark/truncation ordering".
- cody's yes on 2026-11-18 (18:00 UTC): "I preregister MetaWatermark.tla as scoped in PR #163, run it under a 2-hour
  cap per lane, get it independently reviewed, then stop."

This document authorizes no C++, Rust, R640 or X-2 work. It selects no D-02 mechanism, resolves neither R-23 nor
O-FLOOR, and accepts no revision of D-06.

## 1. Question

Does the design in `docs/AFR_001_D02_CRASH_DESIGN.md` hold under repeated crashes plus at most one media fault?
- **Section 2 (U3):** the four-slot node record never rolls back an acted-on generation, and crashes alone never make
  it refuse.
- **Sections 3-5 (U10, U11, U12):** after a truncation, no acknowledged entry is lost without a refusal, crashes alone
  never refuse, and anything that comes back from a truncated tail lies above the watermark (3(e)).

## 2. Model

One module with two parts, selected by the constant `PART`. Each part is its own lane with its own one-fault budget.
This differs from the scope in `docs/AFR_001_D02_FOLLOWUP.md` section 4, which said "sharing one fault budget".
- **Why:** part `wm` treats the record as atomic.
- **Composition:** a run of part `wm` with a record fault is a run of part `rec` with that fault, and part `rec`
  checks that the record then either refuses or keeps every acted-on value [INF]. The two-part product would be
  larger without adding a check.

**Part `rec` (section 2 of the design):**
- Four slots `a0 a1 b0 b1`. Generation `g` goes to slot `g mod 2` of both files. A slot holds `<<g, v>>`, where `v`
  names the bytes, or nothing valid.
- A write is pending until it lands. The node acts on a value only after all its writes have landed (both
  `fdatasync` calls returned).
- A crash lands any subset of the pending writes, tears any other subset, and drops the rest.
- Init writes generations 1 and 2, then runtime writes follow up to `MaxGen`.
- Recovery reads the highest valid generation:
  - a tie (same generation, different bytes) refuses;
  - no valid slot means init incomplete, and init runs again;
  - rule 4 writes the chosen value into the slots of its pair that lack it;
  - then the boot write, then act. At `MaxGen` there is no boot write, and the node acts on the chosen value.
- **One fault per run (when `FaultOn`):**
  - a landing write is lost, torn, or lands in another slot;
  - or, at any time, one slot is zeroed (a foreign write; the spacing rule limits it to one slot).
- Not modelled:
  - the files themselves (missing or wrong-size file refusals);
  - process-kill I/O (V8);
  - a lost write that lands later.

**Part `wm` (sections 3-5):**
- One stream, `L = 1`, `MaxLsn = 4`. Entries are `<<term, lsn>>`, and G-15 fixes their content.
- **Starting state.** The old log is `(1,1) (1,2)` in segment 1 and `(1,3)` in segment 3. Ends through 2 are
  acknowledged. The watermark is durable at 2, and the watermark for 3 is coalesced but not yet submitted.
- **File system:**
  - inodes are durable when written (every write is followed by `fdatasync`; a K1 segment is created whole before
    its name exists);
  - directory operations are pending until a directory `fsync`;
  - a crash keeps any ordered subsequence of the pending operations.
- **Truncation:**
  - the intent (record `PENDING`; ends above `L` become void, because by V4 they were never `HA_DURABLE`);
  - 2.1, the drain: wait until the coalesced watermark has landed;
  - unlink of the names above `L`, then a directory `fsync`;
  - the new segment `L + 1` (`prev` = term of record `L`), bound by name, then a directory `fsync`;
  - 2.7: read record `L` from the media and write the watermark `(L, entry, nonce of L + 1)`;
  - the clear.
- **Appends** from `L + 1`:
  - each entry is the new primary's `(2, x)`, or, by V4, the identical old `(1, x)` returning; once a term-2 entry
    is appended, the rest are term 2;
  - data, then the watermark, then the acknowledgement.
- **Recovery:** while `PENDING`, completion reruns from 2.1. Otherwise it scans and checks, and that is terminal.
  - The scan: name 1 present with `prev = 0`; each segment contiguous up to the next name minus one (the void
    rule); FL-chain (`prev` of each segment equals the term of the record before it); the tail segment scanned to
    its last contiguous record.
  - The checks: `W > E` refuses; record `W` must equal the watermark's entry; with a nonce, name `W + 1` must be
    bound to that inode.
- **One fault per run (when `FaultOn`):**
  - a lost data write;
  - a lost watermark write;
  - one directory operation lost after its `fsync` (a lost unlink or a lost `L + 1` binding).
- **Watermark generations.** The watermark's generation is assigned when it is submitted, so the last write to land
  wins. That is the race the drain closes.
- Not modelled:
  - the floor and reclaim, so `retain_from` is 1 and only the `W > E` and match rows of U12 apply (O-FLOOR);
  - torn or misdirected WAL writes (the X-2 cases);
  - several streams, the wire, and completion internals beyond the steps above.

## 3. Properties

`Safety` is the conjunction of six properties.

**Part `rec`:**
- **NoRollback:** when recovery chooses a value, it is newer than the newest acted-on value or equal to it.
- **RecNoCrashRefusal:** a refusal implies that the fault was used. This covers NoTieWithoutFault.

**Part `wm`:**
- **NoSilentLoss:** if recovery accepts, every acknowledged end of the current history is in the recovered log.
- **WmNoCrashRefusal:** a refusal implies that the fault was used.
- **CrashOnlyExact:** with no fault, the recovered log is a prefix of the node's current log.
- **RevivalAboveW:** any recovered entry that is not in the current log lies above the durable watermark.

Each part's properties hold trivially in the other part's lanes.

## 4. Lanes and expected outcomes (fixed before the runs)

Bounds for every lane: `MaxGen 6`, `MaxCrash 3`, `L 1`, `MaxLsn 4`, as in `MetaWatermark.cfg`. Each lane changes only
the settings shown.

Every run uses 4 workers and `-deadlock`, under a 2-hour wall-clock cap. A run that hits the cap is **NOT
COMPLETED**. TLC's `states/` directory is deleted after each run.

**Part `rec`:**

| Lane | PART | MUT | FaultOn | Expected | Why |
|---|---|---|---|---|---|
| R0 | rec | none | TRUE | **PASS** | the design |
| R0n | rec | none | FALSE | **PASS** | crashes only |
| RM1 | rec | NoRule4 | TRUE | **FAIL**, NoRollback | the #162 counterexample: the boot write overwrites G's pair while G+1 has one copy; a zeroed slot then leaves no valid copy |
| RM2 | rec | BootFirst | TRUE | **FAIL**, NoRollback | the same, with rule 4 after the boot write |
| RM3 | rec | Adjacent | TRUE | **FAIL**, NoRollback | the re-review BLOCKER: one foreign write spans three adjacent slots |
| RM4 | rec | K5 | TRUE | **FAIL**, NoRollback | K5: one slot per file, A then B; one zeroed slot after a torn write |
| RM5 | rec | AB | TRUE | **FAIL**, NoRollback | D-06's A/B in one file: one zeroed slot loses the only copy |

**Part `wm`:**

| Lane | PART | MUT | FaultOn | Expected | Why |
|---|---|---|---|---|---|
| W0 | wm | none | TRUE | **PASS** | the design |
| W0n | wm | none | FALSE | **PASS** | crashes only |
| WM1 | wm | LowerAfterClear | TRUE | **FAIL**, WmNoCrashRefusal | a crash between the clear and the lowering leaves `W > E` with no fault |
| WM2 | wm | NoDrain | TRUE | **FAIL**, WmNoCrashRefusal | the coalesced watermark for 3 lands after the lowering |
| WM3 | wm | NoNonceCheck | TRUE | **PASS** | see below |
| WM4 | wm | AckBeforeWm | TRUE | **FAIL**, NoSilentLoss | a lost data write is acknowledged and no watermark records it |

**WM3 is expected to PASS, unlike the FAIL in the follow-up scope.** In this model, a lost `L + 1` binding leaves
segment 1's old bytes for `L + 1` readable. Either:
- record `W`'s check refuses; or
- what returns is the identical old entry (V4, G-15); or
- it lies above `W`, where RevivalAboveW permits it.

So the nonce check is defense in depth here [INF]. A FAIL would mean that the argument in 3(e) is wrong.

**Reachability probes** (each is an invariant expected to be **violated**, which shows the state is reached):

| Probe | PART | State reached | Expected |
|---|---|---|---|
| PRecRule4 | rec | rule 4 writing copies | reached |
| PRecReinit | rec | init rerun after a crash | reached |
| PRecLost, PRecTorn, PRecMisdir, PRecForeign | rec | the node runs again after a crash with that fault used | reached, each |
| PRecTop | rec | acted on `MaxGen` | reached |
| PRecTie | rec | a tie refusal | **not reached** (see below) |
| PWmLostData, PWmLostWm, PWmLostDirOp | wm | recovery finished with that fault used | reached, each |
| PWmRefused | wm | a refusal | reached |
| PWmRerun | wm | completion rerun reaches 2.7 after a crash | reached |
| PWmRevived | wm | an accepted log differs from the current log | reached |
| PWmOldReturn | wm | an identical old entry acknowledged at `L + 1` and recovered | reached |
| PWmNewAcked | wm | full log of acknowledged term-2 entries recovered | reached |

**PRecTie.** Smoke runs (section 5) did not reach the tie. That suggests ties need a stale write that lands late,
which is not in the fault set. It is recorded here before the counted run and is not a claim that ties cannot
happen on real devices.

## 5. Smoke and sizing (before this preregistration)

These runs were made while writing the model, to debug it. Their bounds were smaller than the counted bounds, and
they count for nothing.
- `rec`, MUT none, MaxGen 3-4, MaxCrash 1-2: no violation; at 4/2, 15,103 distinct states. PRecTie not reached at
  either bound. Other probes reached.
- `rec`, MUT BootFirst, MaxGen 3, MaxCrash 1: **no violation** (a parse check that showed the result; at the
  counted bounds RM2 is still expected to FAIL, because the counterexample needs a fault after a crash at G+1).
- `wm`, MUT none, MaxLsn 3-4, MaxCrash 2: no violation after three model fixes:
  - the outcome type;
  - the IF/THEN precedence;
  - void ends above `L` at the intent, and no end for 3 reported after the intent.
  At MaxLsn 3, 1,425 distinct states. Probes PWmRefused (with lostDirOp), PWmRevived and PWmNewAcked reached.

No other mutation was run.

## 6. Revision 1 results

TLC2 2026.11.15.002146, 1 worker per run, two runs at a time on a 4-core container, `-deadlock`, 2-hour cap. Model
as committed in 3aa39d6. Every run finished in under 5 seconds. `states/` was deleted after each run.

| Lane | Expected | Result | Distinct states | Depth |
|---|---|---|---|---|
| R0 | PASS | PASS | 97,903 | 33 |
| R0n | PASS | PASS | 4,749 | 32 |
| RM1 NoRule4 | FAIL, NoRollback | FAIL, NoRollback | 26,053 | 14 |
| RM2 BootFirst | FAIL, NoRollback | FAIL, NoRollback | 23,282 | 13 |
| RM3 Adjacent | FAIL, NoRollback | FAIL, NoRollback | 4,120 | 10 |
| RM4 K5 | FAIL, NoRollback | **PASS: INVALID** (model defect, below) | 473 | 12 |
| RM5 AB | FAIL, NoRollback | FAIL, NoRollback | 466 | 7 |
| W0 | PASS | PASS | 4,235 | 34 |
| W0n | PASS | PASS | 708 | 34 |
| WM1 LowerAfterClear | FAIL, WmNoCrashRefusal | FAIL, WmNoCrashRefusal | 231 | 11 |
| WM2 NoDrain | FAIL, WmNoCrashRefusal | FAIL, WmNoCrashRefusal (see review finding 2) | 533 | 12 |
| WM3 NoNonceCheck | PASS | PASS (see review finding 3) | 4,235 | 34 |
| WM4 AckBeforeWm | FAIL, NoSilentLoss | FAIL, NoSilentLoss | 440 | 13 |

The FAIL lanes were rerun against the named property alone to confirm which property broke.

Probes: all reached as expected, and PRecTie not reached, as preregistered.

**RM4 is invalid.** In `Synced`, `queue' = {}` and a later `queue' = {b0 ...}` contradicted each other under K5, so
init never finished and NoRollback held trivially. The independent review found the same thing.

## 7. Revision 1 review

An independent reviewer (Opus) was told to break the model's fidelity to the design. They ran their own copies.

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | BLOCKER | K5 mutation vacuous (above) | fixed in revision 2; a vacuity probe per lane added |
| 2 | MAJOR | `WmCoLand` could acknowledge LSN 3 again after the clear, so NoDrain could also fail through a false acknowledgement | fixed: only before the intent; FAIL lanes checked per property |
| 3 | MAJOR | WM3's PASS came from RevivalAboveW's definition. Design 3(e) says a lost `L + 1` binding at `W = L` "would be silent", and the preregistration called the nonce check defense in depth. The two contradict each other | the reviewer is right: the preregistration's WM3 expectation was wrong. Revision 2 adds `NoBindRevival`, which states the harm 3(e) names |
| 4 | MAJOR | appends were fully serial; the design does not say whether data flushes of a stream overlap, and its "anything else above W refuses" was not modelled | revision 2 models the rule and adds a `Pipe` lane |
| 5 | MINOR | 2.1 drops unsubmitted coalesced writes; the model always landed them | revision 2 adds the drop |
| 6 | MINOR | the watermark is one atomic register, not two alternating slots; no zeroed or misdirected watermark write | disclosed, out of scope (section 8) |
| 7 | MINOR | record `L` is in the initial log and no fault reaches it, so 2.7's refusal row is unreachable | disclosed |
| 8 | MINOR | the tie is unreachable, so RecNoCrashRefusal is vacuous | disclosed |
| 9 | MINOR | rule 4 triggers on a missing copy in position; the design says "fewer than two valid copies" | the model's trigger is the more conservative one; the reviewer ran the literal rule at 5/3 and it passed (72,302 states). Kept |
| 10 | NOTE | only `L = 1`; `L + 1` never an existing name | revision 2 runs every wm lane at `L = 1` and `L = 2` |
| 11 | NOTE | composition argument plausible | keeps [INF] |
| 12 | NOTE | NoRollback's check points are enough | none |

Reviewer's UNRESOLVED items, as reported: whether the design pipelines data flushes (finding 4); whether a stale
watermark slot two generations back plus one fault can do worse than refuse; whether a revived void tail at `W = L`
is harmful (rests on R6.2 re-reconciliation, outside the model); which conjunct a 4-worker WM2 run reports; whether
"torn WAL writes out of scope" also covers K1 torn frames.

## 8. Revision 2 (preregistered before any revision-2 run)

**Model changes:**
- K5 sets `queue'` in each branch, and init queues `b0` for generation 1.
- `WmCoLand` acknowledges LSN 3 only while `bpc = "intent"`.
- 2.1 either lets the coalesced watermark land or drops it.
- A lost directory operation is `lostUnlink` or `lostBind`.
- Appends: data(x) is issued, then completes (lands, or the fault loses it). watermark(x) follows the completion of
  data(x) and of every earlier write, and the acknowledgement follows the watermark. With `Pipe = TRUE`, two data
  writes can be in flight, and a crash lands any subset of them. `AckBeforeWm` acknowledges at data completion.
- An old entry can return only for LSN 1-3, which existed in the old log.
- The scan refuses a valid record after the tail's first gap. This is my reading of design 4: "only a torn frame,
  or format sectors of this segment's nonce, may be discarded above `W`. Anything else refuses." A write that never
  landed leaves format sectors. A later valid record is "anything else".
- New property in `Safety`: **NoBindRevival**. If the fault was a lost `L + 1` binding and recovery accepts, the
  recovered log is a prefix of the current log.
- New probes: `PRecActed` and `PWmDone` (vacuity, run for every lane), `PWmLostUnlink`, `PWmLostBind`, `PWmStray`.

**Lanes.** As in section 4, at the same bounds, plus:
- every wm lane runs at `L = 1` and at `L = 2`;
- every FAIL lane is also run against the named property alone;
- **W0P:** `Pipe = TRUE`, `FaultOn = FALSE`.

**Expected outcomes (changes from section 4 in bold):**

| Lane | L = 1 | L = 2 |
|---|---|---|
| R0, R0n | PASS | n/a |
| RM1, RM2, RM3, RM5 | FAIL, NoRollback | n/a |
| RM4 K5 | **FAIL, NoRollback** (a torn write to `a0`, then the one remaining copy in `b0` zeroed) | n/a |
| W0, W0n | PASS | PASS |
| WM1 LowerAfterClear | FAIL, WmNoCrashRefusal | FAIL, WmNoCrashRefusal (the landed watermark 3 above `E = 2`) |
| WM2 NoDrain | FAIL, WmNoCrashRefusal | FAIL, WmNoCrashRefusal |
| WM3 NoNonceCheck | **FAIL, NoBindRevival** (old record 2 in segment 1 comes back) | **PASS** (segment 1 ends at `L`, so nothing voided can come back) |
| WM4 AckBeforeWm | FAIL, NoSilentLoss | FAIL, NoSilentLoss |
| **W0P** | **FAIL, WmNoCrashRefusal** | **FAIL, WmNoCrashRefusal** |

**W0P is a test of the design text, not of the model.** A crash can land data(k + 1) without data(k). Read
literally, design 4 then refuses after a crash alone. A smoke run at MaxLsn 3 and MaxCrash 1 already showed this
(trace: data(2) and data(3) in flight; the crash lands only data(3); the scan refuses). If W0P FAILs, the design must
either keep one data write in flight per stream, or say that valid records after a format-sector gap above `W` are
discarded. That ruling is not the model's to make.

Probes: PRecActed and PWmDone reached in every lane. Other probes as in section 4, with PWmLostDirOp replaced by
PWmLostUnlink and PWmLostBind (both reached). PWmStray **not reached** with `Pipe = FALSE`, and reached in W0P.

**Out of scope, in addition to section 2:**
- two-slot watermark files and their faults (finding 6);
- loss of record `L` (finding 7);
- a third term.

Smoke runs for revision 2, made while fixing the model and counting for nothing:
- rec, R0 and K5 at 3/1;
- wm, W0 at MaxLsn 3 and MaxCrash 1;
- the W0P trace above.

## 9. Revision 2 results

The setup was the same as for revision 1, with the model as committed in 8104678. All 77 runs finished in under 6
seconds each, so none hit the cap. `states/` was deleted after each run. Every lane's PRecActed or PWmDone probe was
reached, so no lane is vacuous in that sense.

**Every result matched its preregistered expectation.**

| Lane | L = 1 | Distinct states (depth) | L = 2 | Distinct states (depth) |
|---|---|---|---|---|
| R0 | PASS | 97,903 (33) | n/a | |
| R0n | PASS | 4,749 (32) | n/a | |
| RM1 NoRule4 | FAIL, NoRollback | 26,053 (14) | n/a | |
| RM2 BootFirst | FAIL, NoRollback | 23,282 (13) | n/a | |
| RM3 Adjacent | FAIL, NoRollback | 4,120 (10) | n/a | |
| RM4 K5 | FAIL, NoRollback | 4,381 (11) | n/a | |
| RM5 AB | FAIL, NoRollback | 466 (7) | n/a | |
| W0 | PASS | 9,384 (37) | PASS | 4,186 (34) |
| W0n | PASS | 1,278 (36) | PASS | 706 (33) |
| W0P (pipelined data, crashes only) | FAIL, WmNoCrashRefusal | 217 (13) | FAIL, WmNoCrashRefusal | 208 (13) |
| WM1 LowerAfterClear | FAIL, WmNoCrashRefusal | 258 (10) | FAIL, WmNoCrashRefusal | 301 (11) |
| WM2 NoDrain | FAIL, WmNoCrashRefusal | 517 (12) | FAIL, WmNoCrashRefusal | 515 (12) |
| WM3 NoNonceCheck | FAIL, NoBindRevival | 370 (11) | PASS | 4,186 (34) |
| WM4 AckBeforeWm | FAIL, NoSilentLoss | 693 (13) | FAIL, NoSilentLoss | 668 (13) |

For each FAIL, the lane was also run against the named property alone, and it broke that property.

**Probes:**
- All rec probes were reached except PRecTie, as preregistered.
- All wm probes were reached. PWmStray was not reached with `Pipe = FALSE` and was reached in W0P, as preregistered.

**Weak probes.** PWmDone and PWmOldReturn are reached at depth 3, by a crash before the intent. This was seen after
the runs. A diagnostic that was not preregistered asked for an old entry acknowledged at `L + 1` after the truncation,
recovered, with no fault. It was reached (948 distinct states, depth 14). The NoFault lanes R0n and W0n, and the
per-lane PRecActed probes, are reached at depth 9 or more.

## 10. What this shows, and what it does not

Within these bounds and the limits in sections 2 and 8 [FACT for the model, INF for the design]:
- **Section 2 (U3):**
  - the four-slot record with rule 4 and spaced slots did not roll back under three crashes and any one listed media
    fault;
  - each of the five weaker variants did roll back, including K5 and D-06's A/B.
  - The tie refusal was never needed, so that branch is untested.
- **Sections 3-5 (U10-U12), one stream, `L` = 1 or 2, serial data writes:**
  - no acknowledged entry was lost without a refusal;
  - crashes alone never refused;
  - revived entries stayed above the watermark;
  - a lost `L + 1` binding never brought back a voided entry. **This holds only for the initial layout.** The
    re-review (section 11, finding 1) shows that it breaks when the old segment holds two or more records past `L`.
  - Each ordering rule that was removed (lowering before the clear, the drain, the nonce check at `L = 1`, the
    acknowledgement after the watermark) broke a property. The nonce check matters only where the old segment holds
    bytes past `L`.
- **A design gap found (W0P).** If a stream can have two data writes in flight, a crash can land the later one
  without the earlier one. Design section 4 then refuses after a crash alone, which breaks requirement 2. The design
  must say one of two things, and that choice is cody's, with Codex:
  - **(i)** one data write in flight per stream; or
  - **(ii)** valid records after a format-sector gap above `W` are discarded, not refused.

  I recommend (i). Today's code already keeps one flush in flight (`src/core/storage.cpp:643`, `!inflight_`), so (i)
  only writes down what exists. The model cannot rule on (ii): with the gap rule removed, it still passes, because
  C16b is outside its fault set (section 11, finding 3).

**What it does not show:**
- anything at larger bounds, or with several streams, the floor and reclaim (O-FLOOR), torn or misdirected WAL
  writes, two-slot watermark files, process-kill I/O (V8), or the wire;
- that R6.2 re-reconciliation repairs a revived tail (the model's recovery is terminal);
- the revival after a lost `L + 1` binding once `W > L` (section 11);
- any selection of D-02, any resolution of R-23 or O-FLOOR, or any acceptance of a D-06 revision.

## 11. Revision 2 re-review

A second independent reviewer (Opus) re-checked the revision-2 changes and reran the lanes in their own copies. Their
results matched section 9. They also found that NoBindRevival is not vacuous: "ok with a lostBind" is reached at both
values of `L`.

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | MAJOR | W0 passes NoBindRevival only because segment 1 holds exactly one record past `L = 1`, and none past `L = 2`. With `Old(3)` also in segment 1, W0 at `L = 1` **fails NoBindRevival** in 14 steps. The binding of name 2 is lost, `Old(2)` is appended, the watermark is written as `(2, Old(2), nonce 0)`, and then a crash. The scan of segment 1 returns `Old(1) Old(2) Old(3)`. The watermark at 2 matches, so recovery accepts and `Old(3)` comes back. After the first append, the watermark no longer carries the nonce | **open design question, below.** This was the reviewer's run, not a preregistered lane. It is recorded here and not rerun |
| 2 | MAJOR | Design 3(e) contradicts itself. It calls the revival after a lost binding "silent" only at `W = L`. Then it says that once appends move `W` above `L`, the lost-unlink case covers it, and that case accepts a revived tail above `W` (V4, G-15, R6.2). The model reads the binding case strictly and the unlink case leniently | open design question, below |
| 3 | MINOR | The gap rule ("Stray") has no safety effect in the model. With it removed, `Pipe = TRUE` plus faults passes all of Safety | section 10 changed: the model cannot rule on option (ii) |
| 4 | NOTE | Stray is a fair literal reading of design 4. W0P is a gap in the design text: the code keeps one flush in flight (`src/core/storage.cpp:643`). A coalesced multi-record flush that tears is a torn frame; two separate frames are not | section 10 now recommends (i) |
| 5 | MINOR | The RM4 counterexample TLC found is: the `b0` write at init is lost (the fault), then a crash tears `a0`. That differs from the section 8 story, though the result is the same | noted; section 8 is not edited after the runs |
| 6 | NOTE | At `L = 2`, a lost unlink of name 3 is overwritten by the bind of 3, so the unlink-revival case does not arise there | disclosed |
| 7 | NOTE | PWmStray means "refused without a fault", not "refused by the gap rule" | noted |
| 8 | NOTE | `nextW` is listed twice in `wmVars`. It does no harm | left as run |

**The open question (findings 1 and 2), for cody with Codex.** A lost `L + 1` binding lets voided term-1 entries
come back above `W`, unrefused, once one append has moved the watermark. Two rulings are possible:
- **Lenient.** This is the same as the lost-unlink case. The entries are identical by G-15, they are above `W`, and
  re-reconciliation on rejoin removes them. Then 3(e)'s "that would be silent" is wrong, and the nonce check is
  defense in depth.
- **Strict.** It is harmful at any `W`. Then the nonce check must survive the first append.

**My recommendation: the strict ruling, enforced by carrying `(L + 1, nonce)` in every watermark write until the next
truncation.**
- It is a small field.
- It stops this case depending on the R6.2 argument, which no model checks.

This recommendation has not been model-checked. Revision 3 would need its own preregistration and is not
authorized.

**Reviewer's UNRESOLVED items:**
- the 3(e) ruling above;
- whether the design serialises data flushes per stream;
- whether discarding valid records after a format-sector gap is safe against C16b (the model cannot decide);
- carried over from the first review: a stale watermark slot two generations back, and K1 torn frames.
