# SNC-AFR-001: model check of the two reconciliation blockers and the two-entry self re-promotion

- **Status: PROPOSED.** Nothing here is frozen. It authorizes no implementation and no R640 runs. R2′ and R9 stay
  unfrozen; AFR-D-02 stays unresolved; the default m = 2 is unchanged.
- **Trigger.** Codex's review of PR #153 (`docs/handoff/PR153_Codex_Review.md`), relayed by cody on 2026-11-17.
  It approved #153 for merge as documentation and model evidence, held the freeze, and asked for one preregistered
  follow-up step with exactly three cases.
- **Base.** `docs/AFR_001_PROMOTION_DURING_RECON.md` (the rules R6.3′, S2′, P6′ and section 6's two blockers) and
  `spec/tla/PromoteRecon.tla`.

## 1. The three cases

| Case | What it tests | Rule under test |
|---|---|---|
| 1 | Duplicate or delayed replies across a restart | R6.3′ item 1, the request binding |
| 2 | Four nodes, several terms, a node carrying an older pending intent | R6.3′ item 3, the cross-term pending-intent block |
| 3 | Two entries, a crash, and the self re-promotion of a degraded primary | 4.2: a restarted sole primary serves only in a new term; `(term, LSN)` uniqueness checked non-vacuously |

The standby read rule (R9-F, reads expose only entries up to the primary's acknowledged LSN) is written into
`docs/AFR_001_SAFETY_ANALYSIS.md` and G-30 as text. It is **not** model-checked in this step, which is limited to the
three cases above.

## 2. Preregistration (committed before any counted run)

### 2.1 Model

`spec/tla/ReconBlockers.tla`. It is `PromoteRecon.tla` with every rule #153 showed necessary hard-wired on:
quiescence with durable completion (S2′), the reply term check, the R6.4 re-check at intent write, the R3 barrier for
a self re-promoted term, and R9-B, R9-C and R9-D. The changes:

1. **Replies form a set per node.** Every answer, including "positioned", travels as a reply. A reply stays in the
   set after delivery, so it can be delivered again (duplication), in any order (reordering), or be removed (loss).
   Replies survive a crash of the receiver. At most `MaxReps` replies are outstanding per node.
2. **`FixBind`** is R6.3′ item 1's binding. A reply carries the snapshot `(last term, end LSN)` of the log it
   answered. `k` uses it only if its log still has that snapshot. With `FixBind = FALSE`, only the #153 checks remain
   (the reply's term is `k`'s term, `k` is an unpositioned non-primary).
3. **`FixIntentBlock`** is R6.3′ item 3. While `k` has a pending intent it sends no request, is not positioned, is
   not shipped to, counts toward no acknowledgement, READY, A or S, and holds no lease. Learning a newer term leaves
   the intent pending. With `FALSE`, these guards are removed; the intent write itself still needs no other pending
   intent, and checkpoints still wait for it (R6.3).
4. **`FixNewTerm`** is 4.2. With `FALSE`, an action `Resume` lets a restarted primary that is the sole member of S
   serve again at its old term, without a witness-approved promotion (the behaviour cody ruled out on 2026-11-16).
5. Intents record the term in which they were written (for the cross-term probe only).
6. Two scope switches bound a lane and change no rule: `Ckpt` (FALSE: no checkpoints, so no reseeding) and `Leases`
   (FALSE: no standby leases).

**Why the binding is the log snapshot, not an incarnation and request number.** R6.3′ binds a reply to `k`'s boot
incarnation and request number and checks that `k`'s log has not changed. The model keeps only the snapshot check.
This is equivalent in the model for these reasons [INF, to be broken by review]:
- Within one term the answering primary only appends.
- `k`'s entries of the answering term come from that primary (`EntryUnique`).
- So for a fixed log at `k`, every reply from the same term gives the same answer. An older request with the same
  snapshot is therefore interchangeable with the latest one.
- A crash that leaves `k`'s durable log unchanged also leaves the right answer unchanged.
- `(last term, end LSN)` identifies the whole log under `HistoryUnique` and `LogMatching`, both checked.

The incarnation and request number stay in the proposed text, because an implementation needs a cheap identity
check and cannot compare logs.

### 2.2 Invariants and probes

- `Safety`, as in #153: `AckedPreserved`, `EntryUnique`, `LogMatching`, `StateMatchesLog`, `PositionedPrefix`,
  `HistoryUnique`, `StandbyFresh`, `LeasedPrefix`.
- The following are reachability probes, and TLC must report each one violated:
  - `NoSelfRepromotion`: a self re-promotion happens.
  - `NoLsnReuse`: an LSN held by an older term's entry is later held by an entry of the self re-promoted term, so
    `HistoryUnique` is exercised across the reuse.
  - `NoCrossTermIntent`: a node carries an intent written in an older term into a newer term.

### 2.3 Abstractions (limits, stated before the runs)

- Leases and the R3 barrier are untimed, as in #153.
- A standby's durable log stands for its applied state, so R9-A and R9-F are not checked here.
- Witness Paxos is a single atomic record.
- There are no torn tails, no silent loss and no volatile standby tails. "Refusal when the durable log cannot be
  trusted" stays with AFR-D-02 and is not modeled.
- The incarnation and request number are abstracted as in 2.1.
- Case 1 and case 3 use MaxTerm 2. That allows one promotion, which is enough for a restart, a self re-promotion and
  a stale reply. Case 2 uses MaxTerm 3, the least that lets an intent written in one term meet a newer primary.

### 2.4 Runs

Setup as in `spec/tla/RESULTS.md`, with these limits:
- `-deadlock -Xmx4g`, one worker per run, at most three runs at a time, and `states/` deleted after every run.
- The wall-clock cap is **2 h** per run. A run that reaches it is **NOT COMPLETED**, never PASS.
- Bounds are never raised after a result.
- Every run sets `Nodes = {n1, n2, n3}` with `P0 = n1`, `OpInside = TRUE`, `FixBind`, `FixIntentBlock`, `FixNewTerm`,
  `Ckpt` and `Leases` all TRUE, unless the table says otherwise. Lane C1's file is `spec/tla/ReconBlockers.cfg`.
  Every other lane changes only the constants the table names.

| Run | Case | Bounds (MaxTerm, MaxIds, MaxCrashes, MaxReps) | Change | Invariant | Expected | Acceptance |
|---|---|---|---|---|---|---|
| C1 | 1 | 2, 1, 1, 2 | none | Safety | PASS | required for case 1 |
| MB | 1 | as C1 | `FixBind = FALSE` | Safety | violated: positioned, acknowledged, restart, then a duplicate of an older reply truncates the acknowledged entry (`AckedPreserved`) | the counterexample must come back, or the binding is reported **not shown necessary** |
| C2 | 2 | 3, 1, 0, 1; `Nodes = {n1, n2, n3, n4}`, `SYMMETRY Sym`, `Ckpt = FALSE` | none | Safety | PASS | required for case 2 |
| MI | 2 | as C2 | `FixIntentBlock = FALSE` | Safety | violated: section 6's QA trace (an intent from term 2 completes after the node counted toward a term-3 acknowledgement, `AckedPreserved`) | as MB |
| PX | 2 | as C2 | none | NoCrossTermIntent | violated (reachable) | required: otherwise C2 does not exercise case 2 |
| C3 | 3 | 2, 2, 1, 1 | none | Safety | PASS | required for case 3 |
| MN | 3 | as C3 | `FixNewTerm = FALSE` | Safety | violated (sizing smoke at MaxIds 1 showed `PositionedPrefix`: a survivor's old positioning is stale) | as MB |
| MNH | 3 | as C3 | `FixNewTerm = FALSE` | HistoryUnique | violated: the resumed primary writes a new entry at an LSN and term another node already held | shows the uniqueness check is not vacuous; otherwise reported **not shown** |
| PS | 3 | as C3 | none | NoSelfRepromotion | violated (reachable) | required |
| PR | 3 | as C3 | none | NoLsnReuse | violated (reachable) | required: otherwise case 3's uniqueness claim is vacuous |

**Fallback lanes**, run only if the named lane is NOT COMPLETED. Each is reported as a smaller check, never as the
lane it replaces:
- C1f: C1 with MaxReps 1.
- C2f: C2 with `Leases = FALSE`. R9-B's lease clause is then not checked for case 2.
- C3f: C3 with `Ckpt = FALSE`.

### 2.5 Stop rules and how reruns are counted

- Any violation in C1, C2 or C3 blocks the correction, explained or not.
- A mutation that is expected to fail but finds nothing means that rule is **not shown necessary** at these bounds.
  It is reported that way, not as a pass.
- A mutation counterexample is read state by state. If its shape is not the expected one, it is reported as found
  and explained. An unexplained violation is a blocker.
- **Budget:** 10 launches for the table, at most 3 fallback launches, and at most 6 reruns. No other runs.
- **Reruns after a model defect.** A model defect is a TLC error, or a counterexample that shows the model does
  not encode a rule as section 2.1 states it.
  - The launch that exposed the defect counts against the 10. It is listed in the results with its output.
  - The fix is committed with a note naming the defect.
  - Every run whose result the fix could change is rerun, and each rerun counts against the 6.
  - A fix never raises bounds, weakens an invariant, or changes an expected result in this table.
- **Reruns after a protocol finding.** A counterexample that is the protocol's fault is a finding. The text is
  corrected, the model is changed to match, and the affected runs are rerun within the same 6.
- If the budget runs out, the remaining runs are reported NOT RUN, and the step stops for Codex and cody.

### 2.6 Smoke and sizing runs before this commit (not counted)

These ran at tiny bounds, or with a 5-6 minute cap for sizing. The coordinator asked for them after #153's launch
overrun. They are listed so that nothing was seen before preregistration without being disclosed.

| Run | Bounds (MaxTerm, MaxIds, MaxCrashes, MaxReps), change | Result |
|---|---|---|
| s1 | 2, 1, 0, 1 | no violation, 108,828 distinct states |
| s2 | 2, 1, 1, 1, `FixBind = FALSE` | violated in 13 states: case 1's expected shape (positioned, shipped, acknowledged, crash, duplicate reply sets an intent to 0, the entry is truncated) |
| s3 | 2, 1, 1, 1, `FixNewTerm = FALSE` | violated in 7 states, `PositionedPrefix` (a survivor positioned in term 1 holds an entry the resumed primary lost) |
| s4 | 2, 1, 0, 1, four nodes, symmetry, `FixIntentBlock = FALSE` | no violation, 1,878,793 distinct states. Expected: MaxTerm 2 cannot reach case 2's trace |
| s5 | 2, 1, 1, 1, NoSelfRepromotion | violated in 5 states (reachable) |
| q | 2, 1, 0, 1, `Leases = FALSE` | parse check of the switch; no violation, 55,992 distinct states |
| y1 | 2, 1, 1, 2 (C1's bounds) | **completed, no violation**, 929,059 distinct states in 223 s. Disclosed because it is C1's bounds; C1 is run again as a counted launch |
| y2 | 2, 2, 1, 1 (C3's bounds) | stopped at the 5-minute cap, 1.47 M distinct states, no violation yet |
| y3 | 2, 2, 1, 2 | stopped at the cap, 1.46 M distinct states, no violation yet; not used |
| z1 | 3, 1, 1, 2 | stopped at the 6-minute cap, 1.64 M distinct states, no violation yet; not used |
| z2 | C2's bounds, before `Leases` existed | stopped at the cap, 1.13 M distinct states, no violation yet |
| z3 | 3, 2, 1, 1 | stopped at the cap, 1.93 M distinct states, no violation yet; not used |

## 3. Results (2026-11-17)

Every counted run used the committed model (`ccb88f7`). No launch hit a TLC error or a model defect, so no rerun
was needed, and no fallback lane was run. Ten launches were made out of the budget of 10; there were 0 reruns.

| Run | Result | Distinct states | Time | Reading |
|---|---|---|---|---|
| C1 | **PASS**, exhaustive | 929,059 | 238 s | case 1 fixed: no violation with duplicated, reordered, lost and post-restart replies |
| MB | violated as expected | 13-state trace | 16 s | see 3.1 |
| C2 | **PASS**, exhaustive (symmetry) | 5,675,611 | 1,737 s | case 2 fixed, at its narrow bounds (section 5) |
| MI | violated as expected | 17-state trace | 100 s | see 3.2 |
| PX | violated (reachable) | 10-state trace | 5 s | a node carries a term-2 intent into term 3 |
| C3 | **PASS**, exhaustive | 23,744,032 | 5,359 s | case 3 fixed: no violation with two entries, a crash and self re-promotion |
| MN | violated as expected | 7-state trace | 3 s | `PositionedPrefix`; partly a model artifact (3.3) |
| MNH | violated as expected | 7-state trace | 3 s | `HistoryUnique`: the real evidence for case 3 (3.3) |
| PS | violated (reachable) | 5-state trace | 1 s | self re-promotion happens |
| PR | violated (reachable) | 8-state trace | 4 s | the self re-promoted term writes at LSN 1, which a term-1 entry held |

### 3.1 MB: the binding is necessary (case 1)

1. n1 (term 1) writes e in memory. n2 is promoted to term 2 with S = {n2, n3}. n3 learns term 2; its log is empty.
2. n2 learns term 2 and serves; it has e from n1's ship.
3. n2 answers n3's request for its empty log: "positioned at 0". n3 is positioned by that reply. The reply stays in
   flight as a duplicate.
4. n2 ships e to n3 and acknowledges e (n2 and n3 both hold it).
5. n3 crashes. Its positioning is lost; its durable log still holds e.
6. The duplicate reply is delivered again. Its term is n3's term, and n3 is an unpositioned non-primary, so every
   #153 check passes. Against n3's log now, "L = 0" reads as a truncation to 0. n3 writes the intent and completes
   it. e, acknowledged, is gone from a member of S (`AckedPreserved`).

With `FixBind`, the reply answered the log snapshot (term 0, end 0), and n3's log is now (1, 1), so the reply is
discarded. This is the shape section 6 of the promotion doc predicted.

### 3.2 MI: the pending-intent block is necessary (case 2)

This is section 6's QA trace, found by TLC:
1. n1 (term 1) ships e to n3. n2 is promoted to term 2 with S = {n2, n3, n4}; n2 lacks e.
2. n3 learns term 2 and receives n2's reply: truncate to 0. It writes the intent (term 2, L = 0).
3. n4 is promoted by the operator inside S to term 3 (READY waived), S = {n3, n4}. Before n4 learns it, n1, still
   serving term 1, ships e to n4 as well. n4 then learns term 3 and serves.
4. n3 learns term 3 with the intent still pending. Without the block, it asks n4, is positioned (its log matches
   n4's), and counts toward n4's acknowledgement of e.
5. n3 completes the old intent. e, acknowledged in term 3, is gone from n3, a member of S.

### 3.3 MN and MNH: the new term is necessary (case 3)

- MNH: n1 is the sole member of S and holds e (term 1, LSN 1) in memory only. It restarts and loses e. With
  `Resume`, it serves again in term 1 and writes a new entry at (term 1, LSN 1). Two contents for one
  `(term, LSN)` break G-15 (`HistoryUnique`). With `FixNewTerm`, it must be promoted to term 2 first, and PR shows the
  reuse of LSN 1 then happens under term 2.
- MN fails `PositionedPrefix` first: n2, outside S, still counts as positioned in term 1 and holds e, which the
  resumed primary lost. That depends on `Resume` not resetting the survivors' positioning, which a real resume would
  do by re-handshaking. So MN is partly an artifact of the mutation; MNH is the evidence.

## 4. Three-way independent review (2026-11-17)

Three reviewers (protocol correctness, model completeness, downstream implications) were told to break the work.
None found a blocker. Their findings, and what was done:

| # | From | Severity | Finding | Done |
|---|---|---|---|---|
| 1 | protocol, downstream | MAJOR | R9-F could expose an unacknowledged **delete** (no version is left to compare), and said nothing about ranges and indexes | R9-F rewritten: per-key last-change LSN, tombstones, index entries and ranges |
| 2 | protocol, downstream | MAJOR | R9-F could starve a read of a hot key, and did not restate the lease and term check before answering | R9-F: the target is fixed when the read executes; wait; re-check R5 and R9-D; then answer |
| 3 | protocol, downstream | MAJOR | `ack_p` was undefined: durability class (APPLIED and LOCAL_DURABLE acknowledgements happen before replication), and its value after a promotion | R9-F: HA_DURABLE only; never set from `inh_p`; inherited entries count once all of A has applied them |
| 4 | downstream | MAJOR | The C++ exposes unacknowledged, not even locally durable, data at standbys; G-30's evidence did not say so | G-30: clause F marked ABSENT in the C++, with citations (checked by hand against the tree) |
| 5 | downstream | MAJOR | R9-F as text only should not be in a freeze set | R9-F is excluded from the R9-A to R9-E freeze set and needs its own model |
| 6 | downstream | MAJOR | Completion (2.2 of the promotion doc) leaves records above `L` in a mixed block; once the intent is cleared, recovery could see two contents for `L+1` | 2.2 item 5: a durable valid-end marker kept until the segment is reclaimed |
| 7 | protocol, model, downstream | MAJOR/MINOR | R6.3′ item 3 read as "complete before persisting the newer term", against R4 and the model | Reworded: persist the term (R4), then complete in-process, then act. Text and model now agree |
| 8 | downstream | MAJOR | Intents and checkpoints are per stream versus node-wide; metadata-store layout; no time bound on a pending intent | Recorded as open D-06 items (promotion doc 2.4). **Unresolved** |
| 9 | protocol | MINOR | The binding had no stream or group identity, and the incarnation was not said to be persisted before the first request | R6.3′ item 1 amended; the answering primary stops answering when it loses its tail; its positioned record for `k` ends when `k`'s incarnation changes |
| 10 | downstream | MINOR | G-15 (2) and (3) referred to "the current primary's log", which is not the log a cross-term completion used | G-15 reworded: the primary that answered the bound request |
| 11 | downstream | MINOR | Self re-promotion costs; witness-record damage has no named refusal; term growth in a crash loop | Recorded as open items (promotion doc 2.4). **Unresolved** |
| 12 | downstream | MINOR | The intent itself must be protected against damage, or it can lower a D-02 watermark | Added to the D-02 item in 2.4 |
| 13 | model | MAJOR | Only the positioning guard of the block does work: with it, a node with a pending intent is never positioned, so the ship, acknowledgement, READY, A, S and lease guards are implied | Disclosed (section 5). MI removes every clause together, so it shows the block as a whole is necessary, not any single clause (corrected in `docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md`) |
| 14 | model | MAJOR | `LogMatching` cannot fail with at most two ids, so 2.1's appeal to it holds trivially here | Disclosed. The equivalence argument of 2.1 is INF, as labelled; the protocol reviewer tried and could not break it |
| 15 | model | MAJOR | C3's uniqueness check is narrow (reuse only at LSN 1, with an empty common prefix) | Disclosed (section 5) |
| 16 | model | MAJOR | C2 is narrow: L = 0 only, no crash, no checkpoint, no concurrent replies; no lane combines cases 1 and 2 | Disclosed (section 5) |
| 17 | model | MINOR | PX shows the dangerous starting point, not that the dangerous continuation is reachable under the fix | **Unresolved**; a probe for it is proposed for a later step |
| 18 | model | MINOR | `inh` is set at Learn, before quiescence; #153 disclosed this but this preregistration did not | Disclosed (section 5) |
| 19 | model | NOTE | With `OpInside`, the fixed lanes also check self re-promotion from a non-READY record | Recorded as bounded evidence (section 5) |
| 20 | protocol | NOTE | The answering primary's positioned record for `k` should end at `k`'s restart | Done with finding 9 |

What the reviewers tried and could not break: the binding against duplicate truncations, change-and-change-back
logs, reseeds, checkpoints in flight and term persistence with lost memory; the claim that completing an older
intent never removes an entry a member of S must hold; R9-F's direction (never an older version); the m = 2 default
(corrected later: under R2′ a node with a pending intent can be in S, but it is never positioned, so no acknowledgement completes while it is there; `docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md` section 3); the snapshot abstraction (a PASS with it transfers to the real check,
which accepts fewer replies); the completeness of every action's `UNCHANGED` list.

## 5. What this shows, and its limits

**Shown, at these bounds:**
- Case 1: with the binding, duplicated, reordered, lost and post-restart replies lose no acknowledged write (C1).
  Without it, the predicted loss comes back (MB).
- Case 2: with the block, a node carrying an older-term intent into a newer term loses nothing (C2). Without the
  block, section 6's QA trace comes back (MI). PX shows the carried intent is reachable.
- Case 3: self re-promotion to a new term is reachable (PS) and reuses an LSN under the new term (PR) without
  breaking `(term, LSN)` uniqueness or any other `Safety` property (C3). Resuming at the old term breaks uniqueness
  (MNH).
- Because `OpInside` waives READY for the self re-promotion branch too, C3 also covers self re-promotion from a
  record that is not READY, at these bounds.

**Not shown, or narrow:**
- MI removes every clause of the block together, so only the block as a whole is shown necessary (corrected in `docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md`). In the model the
  other clauses follow from the positioning clause, so no single clause is shown necessary.
- C2 covers case 2 only with L = 0, no crash, no checkpoint or reseed, and one reply at a time. No lane combines
  cases 1 and 2. Whether the dangerous continuation (complete, reposition, count in the newer term) is reached under
  the fix is not shown.
- C3 has MaxTerm 2. It does not cover a promotion after the self re-promotion, a self re-promotion of a primary that
  was itself promoted by failover, two self re-promotions, or a second crash. Its LSN reuse is at LSN 1 only.
- `LogMatching` is trivially true with at most two ids.
- The binding is modelled by the log snapshot, not by an incarnation and request number (2.1, INF). MB shows a
  binding is needed, not which part of the real one.
- `inh` is set when the term is learned, before quiescence, as in #153. A primary that truncates during quiescence
  then cannot Grow or grant a lease in that term in the model. That is conservative: it can hide behaviour, not
  allow a bad one.
- R9-F, the standby read rule, is text only.
- Abstractions of 2.3 (untimed leases, applied = durable, atomic witness record, no torn tails or silent loss, D-02
  not modelled) all still apply.

## 6. Recommendation

- The two safety blockers that Codex named are **verified at bounded scope**: the binding (case 1) and the
  cross-term block (case 2) each pass with the rule and fail without it, with the predicted traces.
- Case 3's new-term rule is verified at bounded scope, and the uniqueness check is shown to be non-vacuous.
- R2′ stays **unfrozen** until cody decides. In my view the reconciliation rules are now supported well enough to be
  reviewed for freeze together with R2′, subject to the open D-06 items (per-stream scope, metadata store, the
  completion end marker) and the narrow-bounds limits of section 5.
- R9-A to R9-E can go forward for freeze review as Codex proposed. R9-F stays outside that set until it has its own
  model.
- No C++ or Rust work, no R640 runs, no D-02 acceptance and no m = 2 change follow from this.
