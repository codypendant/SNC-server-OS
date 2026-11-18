# AFR-D-06: storage-format decision package, items 4-14 (CONDITIONALLY ACCEPTED by the owner, 2026-11-18)

Status: **CONDITIONALLY ACCEPTED** by cody on 2026-11-18 (`docs/handoff/PR160_Owner_Decision.md`), under Codex's six
mandatory conditions (`docs/handoff/PR160_Codex_Review.md` section 2): final acceptance depends on D-02; R-23 stays an
unresolved safety blocker; O-FLOOR is discharged before checkpoint/reclaim conformance work; costs are measured before
performance claims; no final freeze; no C++ or Rust implementation. The package text below is as proposed.

## 1. The question

Accept D-06 items 4-14 as SNC's storage format, conditionally, in the same way as the term history `H` (owner
decision of 2026-11-18)?

The items are numbered as in `docs/AFR_001_D06_BOUNDARY.md` section 5. Items 1 and 3 are already decided. Item 2
follows from item 1.

## 2. The package and its evidence

Evidence types:
- **checked**: by a preregistered bounded model, with mutations that fail;
- **reviewed**: argued in the design and attacked by independent break-it reviewers;
- **text**: a field list or a choice, with nothing to check yet.

| # | Item | Evidence | Where |
|---|---|---|---|
| 4 | 64-byte WAL record with a `term` per record | reviewed | D-06 4.2 |
| 5 | Node record `SNCSUPR2`: one 8 KiB A/B copy per write, whole-copy CRC, per-stream slots | checked for tearing (M7); reviewed | D-06 4.1 |
| 6 | Record writes under `meta_mu`, compare-and-write, lock order `control_mu_` → `seed_->mu` → `meta_mu` | reviewed; the round-3 lock-order and positioning-lock fixes were not re-reviewed (D-06 9); concurrency is not modelled | D-06 4.1 |
| 7 | Incarnation = generation of the boot write | reviewed (round 3 fixes not re-reviewed) | D-06 4.1 |
| 8 | Truncation intent, completion, clear, the void rule by segment name, catch-up gated while PENDING, leftover `TMP` removed | checked for crash safety while serving (revisions 1-3: void rule, spare, `L+1`, clear order, `TMP`), reviewed. **Not checked:** catch-up during completion (gated by design), the rebuild's contents (a no-op in the model), convergence of repeated completion (no liveness property), the `prepare()` race, the deletion order (a crash keeps any subset), and the void rule "by name" (a zero-header segment that is not last is unreachable) | D-06 4.3 |
| 9 | No writer open unlinks; reopen truncates to one block and rewrites the header | the atomic reopen is checked (M1, M8); its intermediate steps, the `prev_term` taken from `H` and dropping stale blocks are argued (torn writes are not modelled) | D-06 4.3 |
| 10 | Floor authority in the manifest (`retain_from`, `floor_term`, `start_term`), check FL, publish before delete, recovery's orphan and below-floor deletes, the seed floor, `max_lsn` = last LSN assigned when the walk ends | reviewed only. **Not model-checked (obligation O-FLOOR)** | boundary 3; D-06 4.2, 4.3 |
| 11 | Checkpoints node-wide; none starts while a stream is PENDING | text (a choice) | D-06 4.3 |
| 12 | A copy with a valid CRC and an unknown format is refused, not skipped (R-32) | text | D-06 6 |
| 13 | `DIR/role` unchanged | text | D-06 7 |
| 14 | Request binding and frame fields (field lists only; the byte layout is item 18) | text. Included because the incarnation (7) and per-record terms (4) are what these fields carry; accepting the list fixes no wire bytes | D-06 4.4 |

Why these items go together: they are one layout. The record's slots (5) carry the intents that completion (8) acts
on. Completion's void rule (8) and the reopen (9) protect the WAL that per-record terms (4) turn into `H`. The floor
(10) bounds that same WAL. Accepting some of them without the others leaves a format that cannot be written down.

## 3. What stays open, and must not be read as accepted

- **D-02** (the end-of-log mechanism) and everything waiting on it: damage inside a segment read as the end (R-01), a
  lost acknowledged directory entry, and condition 2 of the `H` decision for the newest segment.
- **Metadata rollback (R-23): an explicit blocker.** A/B copies and the CRC do not detect it.
- **Gaps in the completion evidence**, from the reviews of revisions 2 and 3 (`docs/AFR_001_D06_BOUNDARY.md`
  section 4; `docs/AFR_001_COMPLETION_CRASH.md` section 8): MaxLsn ≥ 5 with MaxCrash 3; truncation at `L` = the end;
  truncation inside a `kRecGroupNext` group; a torn tail in a segment that is not last, followed by a zero last
  segment; concurrent R4, boot and checkpoint record writes while PENDING; the `prepare()`/`take()` race on the spare.
- **O-FLOOR.** The floor/reclaim crash-consistency argument interacts with checkpoint retention, replication catch-up
  and stream remapping, and no model checks it (Codex's review of #159). The review of the boundary left four
  questions UNRESOLVED (boundary section 3).
- **Costs**, none measured (O8, X-1): the 8 KiB write, 8 bytes per record, the rebuild, the wait behind a PENDING
  stream, extra reseeds from the conservative `max_lsn`.
- **Later parts of D-06:** the wire byte layout and mixed-version policy (G-49 to G-51), and migration of existing
  directories (not authorized).
- **The three gates stay separate:** accepting a contract is not C++ conformance, and neither authorizes Rust.

## 4. Recommendation

**Conditionally accept items 4-14 as D-06's storage format.** Conditions:
1. Final acceptance waits on D-02, as for `H`.
2. R-23 stays an explicit blocker.
3. O-FLOOR stays open. Item 10 is accepted as the design, and its crash consistency is argued, not checked. It must
   be discharged, by a bounded model or an equivalent argument that Codex accepts, before any conformance work on
   checkpoint or reclaim.
4. Costs are measured before any performance claim.
5. Nothing is frozen. A later finding against any item reopens it.

Why now, rather than after D-02: D-06 section 8 argues [INF] that the package works with both D-02 options it names,
(a) a watermark and (b) frames in the WAL; D-02 may still require a revision. Holding the package would delay D-02's
design, which needs a layout to attach to.
