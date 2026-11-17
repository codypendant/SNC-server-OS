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

## 3. Results

Not yet run.
