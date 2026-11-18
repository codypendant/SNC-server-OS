# Obsolete-segment rule for a lost unlink (D-02 crash design): specification and written check

Status: **PROPOSED.** Docs only. Nothing here is accepted, and nothing is model-checked. A revision 4 of
`MetaWatermark.tla` (section 7) would need the owner's yes.

Authority:
- Codex's review of #164 (`docs/handoff/PR164_Codex_Review.md`), recommendations 3 and 4:
  - "specify the obsolete-segment detection rule, including nonce ordering and segment identity";
  - "verify the rule against the lost-unlink counterexample and existing truncation semantics before accepting it".
- The coordinator relayed that review as cody's standing rule for pasted reviews.

This document authorizes no C++, Rust, R640, X-2 or model work. D-02, R-23 and O-FLOOR stay open.

## 1. The problem

`docs/AFR_001_D02_MODEL.md` section 14, finding 1:
- A truncation to `L` unlinks segment `M > L`, and the unlink is lost (the one fault).
- After a crash, `M` is back in the directory. If the new log ends at `M − 1` and the terms match, `M`'s void records
  are accepted, unrefused.
- The binding `(L + 1, nonce)` from OD-2 is intact, so it does not catch this.

Codex agreed that recovery should reject such a segment, but asked that the comparison be defined precisely. Random
64-bit segment nonces (`docs/X2_PREREG.md` section 4) have no order.

## 2. The rule (second proposal, R-CHAIN)

My first proposal ordered segments by an epoch taken from the record generation (R-OBS). It was broken in review
(section 5): segments can still be named after the intent, a seed restarts generations at 1, and spares are named in
a later write. R-CHAIN replaces it. It needs no ordering.

**R-CHAIN.** The segment header gains `prev_nonce`, next to the `prev_term` that D-06 already adds (the FL-chain
link, `docs/AFR_001_D06_STORAGE_METADATA.md` section 4.2):
- `prev_nonce` is the nonce of the segment the writer appended in just before this one.
- Wherever recovery checks FL-chain, it also checks `prev_nonce` against the nonce of the segment just before this
  one in the directory. A mismatch **refuses**.
- The first retained segment is not checked, just as for FL-chain. FL-floor and the manifest cover it.
  - "First retained" means the segment named `retain_from`, after reclaim has deleted everything below it.
  - Where no predecessor exists, `prev_nonce` is 0 and is never checked. That covers segment 1 of a fresh space, a
    seed's first segment, and a truncation at `L = retain_from − 1` (boundary rule 7).

**Assignment and persistence.**
- `prev_nonce` is written in the same header write as `prev_term` and the segment's name and first LSN:
  - in the `TMP` before the `rename`, for completion step 2.4 and `open_segment`;
  - when the spare is taken, for a spare.
- The value comes from the writer's own state: the nonce of the segment it was appending to, or for `L + 1`, the
  nonce of `S`, the segment that holds record `L`.
- Nothing new goes into the watermark or the node record. Nothing is ordered, so restarts change nothing.

**Segment identity.** A segment is identified by:
- `space_uuid` and the stream index (design 3);
- its name (the first LSN);
- its random nonce.

Its place in the log is fixed by the link `(prev_term, prev_nonce)` to the segment before it.

**R-CHAIN depends on OD-2's binding** wherever the segment at `L + 1` is not the one completion created. Each of
these is one fault (re-review finding 6):
- **`L + 1` with no segment after it.**
- **A lost rename of the new `L + 1`, when an old segment already had that name.** The old segment's link names
  `S`, just like the new one's, so R-CHAIN passes it.
- **A lost `L + 1` entry, with an old segment directly after `S`.**

The binding's nonce check refuses all three.

**Why it detects a revived segment [INF].**
- Suppose a revived segment `M > L` was appended after segment `P` in the old log, so its link names `P`'s nonce.
- After the truncation, the segment before `M` in the directory is a segment created after the intent: `L + 1`, or
  a later one. Every such segment has a new random nonce.
  - When `L + 1` itself is not the new segment, the OD-2 binding check refuses instead (above).
- So the link mismatches, unless two 64-bit random nonces collide (probability about 2⁻⁶⁴ per pair).
- This assumes the owner rules a lost-unlink revival harmful, as OD-2 did for a lost binding. The ruling is still
  open (`docs/AFR_001_D02_MODEL.md` section 14). Codex called the case "materially similar" and agreed with the
  direction.
- If one lost directory-block write revives several old segments, the first of them already mismatches.
- **Segments named after the intent** but before step 2.1 (review finding 1) are covered the same way: their link
  names the old writer segment, not the new `L + 1`.

## 3. Written check

**3.1 The counterexample** (layout A, `L = 1`):
- Old segment 3 was appended after segment 1, so its link names segment 1's nonce `n1`.
- After the truncation, the directory holds 1, 2 (new, link `n1`) and the revived 3.
- The segment before 3 is now 2, with a new nonce `n2 ≠ n1`. R-CHAIN **refuses**.
- The fault was used, so the refusal is allowed.

**3.2 No fault: R-CHAIN must never refuse.**

| Segment | Its link | Segment before it in the directory | Result |
|---|---|---|---|
| `L + 1` from step 2.4, or from a completion rerun | `S`'s nonce | `S` (names above `L` are unlinked first) | match |
| a segment named later by rollover or a spare | the writer's previous segment | the same segment (only the tail grows) | match |
| `E + 1` opened after recovery (3(c)) | the segment holding `E` | the same | match |
| a segment replaced at the writer's name (3(b)) | rewritten with the same link | unchanged | match |
| the first retained segment after reclaim | names a reclaimed segment | none | not checked |
| a segment holding only a header and format sectors | as above | as above | match. It holds no records anyway |

**3.3 Later truncations and old revivals.**
- The check runs on every retained segment after the first, not only those above `L`.
- So a segment revived by a lost unlink from an earlier truncation is caught wherever it sits. Review finding 8
  showed that R-OBS missed this case.

**3.4 Reclaim.**
- Reclaim removes segments from the front.
- The new first segment is exempt, and every later link is unchanged.
- A front segment whose reclaim unlink was lost comes back below the floor. That is FL-floor's case, part of O-FLOOR,
  and not R-CHAIN's [INF].

**3.5 Seed.**
- The seed writes its own segments (`seed.cpp:236-260` copies only checkpoint files; boundary rule 5).
- The seed's first segment is the first retained segment.
- **Condition C-S:** a seed starts from an empty stream directory. A whole-node seed already refuses a directory that
  holds streams or `ckpt` (`engine.cpp:943-946`). Whether R6.5's per-stream reseed writes its own segments, and from
  an empty directory, is **open (O-R)**.

**3.6 OD-4.** R-CHAIN adds a refusal only. It adds no authority and touches neither the watermark nor the record.

## 4. Fixes that the reviews found outside R-CHAIN

**The replacement keeps the nonce** (review finding 3, re-review findings 2 and 7).
- After a crash with nothing appended since the truncation, `E = L`. Then 3(c) opens `E + 1 = L + 1`, and 3(b)
  replaces it if it holds a torn frame.
- A new nonce would break the binding, and the next crash would refuse with no fault.
- **Fix:** a replacement keeps the segment's nonce. Its links (`prev_term`, `prev_nonce`) are written from the
  writer's state, as at creation.
- **Why reusing the nonce is safe [INF].**
  - Under OD-1, a flush is one frame. The segment at the writer's name therefore holds at most one torn frame past
    `E`. A frame that landed whole is CRC-valid, so it is inside `E`, and the writer would open a later name.
  - If blocks freed from the old file later reappear inside the new one, for example through a lost format write
    (the C15 case), they read as this nonce's torn frame or format sectors. Those are discarded, not accepted.
  - 3(b)'s example of "a CRC-valid frame left by a torn flush" cannot happen under OD-1, and it is removed from the
    design.
  - A same-nonce variant of C15 is not in X-2. It is recorded as open.

**The spare header (review finding 2, re-review finding 1; MAJOR, open U2).**
- Today a taken spare's header goes out in the same write as its first frame (`storage.cpp:672`).
- A torn write can land the frame's sectors without the header sector. R0 then sees an unnamed prepared header
  followed by a valid frame, and refuses after a crash alone.
- This already applies to `prev_term`. It is not new with R-CHAIN.
- **Proposed fix:** write the take-time header, with the name, `prev_term` and `prev_nonce`, as its own DSYNC write
  before the first frame. That is one extra write per segment rollover. Not checked.

**Spare adoption (re-review finding 8).**
- `rename` replaces an existing name. A `spare.tmp` revived by a lost unlink could overwrite a legitimate segment.
- R-CHAIN would still refuse, but the data an operator needs would be gone.
- **Proposed fix:** use `RENAME_NOREPLACE`, and refuse if the name exists.

## 5. Review

An independent reviewer (Opus) tried to break the first proposal, R-OBS. Their findings and what became of them:

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | BLOCKER | A segment named after the intent but before 2.1 gets `c = c_B` and is accepted after its unlink is lost | R-OBS dropped. R-CHAIN covers it (section 2) |
| 2 | MAJOR | A spare's epoch is written only when it is taken, so a header without one could refuse after crashes alone | R-CHAIN shares `prev_term`'s header write. Open U2 (section 4) |
| 3 | MAJOR | Replacing the bound `L + 1` with a new nonce breaks the binding, and the next crash refuses with no fault | fix: a replacement keeps the identity (section 4) |
| 4 | MAJOR | A seed restarts generations at 1, and its first segment is created before the record exists | R-CHAIN does not use the generation. C-S added (3.5) |
| 5 | MINOR | "Headers never copied" cannot work, because K1 frames carry the nonce | restated: the receiver writes every segment and frame itself (3.5) |
| 6, 7 | MINOR | the epoch could be taken from the wrong generation, or published too early | moot under R-CHAIN |
| 8 | NOTE | a revival below a later `L2` was not covered | R-CHAIN checks every retained segment (3.3) |
| 9 | NOTE | one lost directory-block write can revive several segments; R-OBS refuses all of them | R-CHAIN also refuses all of them (section 2) |
| 10 | NOTE | R-OBS respects OD-4 | R-CHAIN does too |

R-CHAIN is the reviewer's own "alternative A", which they marked [INF], not checked. **R-CHAIN has had one targeted
re-review (section 6).**

**Reviewer's UNRESOLVED items:**
- U1: whether flushes run between the intent and 2.1 in the planned completion (R-CHAIN does not depend on it);
- U2: the spare header (section 4);
- U3/O-R: how a per-stream reseed writes segments;
- U4: leftover segments at a seed (C-S);
- U5: the replacement fix (section 4);
- U6: earlier-truncation revivals (now covered by R-CHAIN, not modelled);
- U7: O-FLOOR and V8 are unchanged.

## 6. Re-review of R-CHAIN

A second independent reviewer (Opus) tried to break R-CHAIN.
- They found **no BLOCKER**.
- No crash-only path broke the link, across rollover, completion reruns, reopen, `E + 1`, reclaim, the floor, the
  seed and several streams.
- Every one-fault revival they built was refused, either by R-CHAIN or by the OD-2 binding.

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | MAJOR (carried over) | the spare header: a torn write lands the frame without the header | open U2, proposed fix in section 4 |
| 2 | MINOR | a replacement may have no named header whose links it could keep | the replacement keeps the nonce only (section 4) |
| 3 | MINOR | `prev_nonce` is undefined for first segments | 0, never checked; "first retained" defined (section 2) |
| 4 | MINOR | C-S already holds for a whole-node seed (`engine.cpp:943-946`) | cited (3.5) |
| 5 | MINOR | design 3(e) still proposed the ordered-nonce rule, and its V4 bullet calls a lost-unlink revival safe | both marked superseded in the design; the assumption about the owner's ruling is stated (section 2) |
| 6 | NOTE | two one-fault cases pass R-CHAIN and are caught only by the binding | stated (section 2); added to the revision 4 scope |
| 7 | MINOR | the reason given for reusing the nonce was wrong | restated (section 4) |
| 8 | NOTE | spare adoption by `rename` can overwrite a segment | `RENAME_NOREPLACE` proposed (section 4) |
| 9 | NOTE | minimal; OD-4 respected | none |

**Still UNRESOLVED:**
- U2, the spare header;
- O-R, the per-stream reseed;
- the owner's ruling on lost-unlink revivals;
- the same-nonce C15 variant;
- O-FLOOR's revived front segment.

Nothing here is modelled.

## 7. Proposed revision 4 scope (NOT started; needs the owner's yes)

One narrow change to `spec/tla/MetaWatermark.tla`, part `wm` only:
- Each inode gets `pn`, the nonce of the inode the writer appended after. Old inode 3 has 1, and the new `L + 1`
  has the inode of `S`.
- `Scan` adds the R-CHAIN check.
- 3(b)'s replacement keeps the identity.
- New layouts:
  - a segment named after the intent, before 2.1 (finding 1's window);
  - an earlier-truncation revival;
  - an old segment already at name `L + 1` (the lost rename case);
  - an old segment directly after `S` (the lost-entry case).

Lanes:
- **W0** at every layout and at `L` 1 and 2: expected PASS.
- **New property NoVoidRevival:** after any fault, an accepted log never contains a voided entry. It is strict, and
  it covers both a lost binding and a lost unlink. **W0** is expected to PASS it.
- Mutations:
  - **`NoChain`** (revision 3's behaviour): expected FAIL at A with `L = 1`, reproducing finding 1.
  - **`ReplaceNewNonce`**: expected FAIL WmNoCrashRefusal (finding 3).
- Bounds, the 2-hour cap, preregistration, vacuity probes and independent review as in revision 3. No R640.
