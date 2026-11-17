# SNC-AFR-001: promotion during reconciliation, self re-promotion, and the cross-protocol check

- **Status: PROPOSED.** Nothing here is frozen. It authorizes no implementation and no R640 runs. R2′ and R9 stay
  unfrozen; AFR-D-02 stays unresolved.
- **Trigger.** Codex's review of PRs #151 and #152, relayed by cody on 2026-11-17
  (`docs/handoff/PR151_152_Codex_Review.md`), items 4-6.
- **Inputs.** `docs/AFR_001_R2_AND_SINGLE_COPY.md` (R2′, P1-P7, S1-S5; "R2C" below),
  `docs/AFR_001_SAFETY_ANALYSIS.md` (R1-R9; "SA"), `spec/tla/RESULTS.md` section 9 (the M4 trace),
  `spec/tla/RESULTS_R9.md`.
- **Labels.** FACT-SNC (checked in this repository), EXT, INF (inferred here), HYP (unverified).

## 1. The defect

`spec/tla/RESULTS.md` section 9, M4 (FACT-SNC, model trace): n3 is a survivor in S that has not finished reconciling
against the term-2 primary. It is promoted to term 3. After the promotion is chosen it records a truncation intent
computed against term 2, becomes primary, acknowledges an entry above the intent's truncation point, and then applies
the intent, which deletes the acknowledged entry.

For automatic promotion, R2′ rule 4 (READY waits until every survivor is positioned) prevents it. Operator promotion
inside S waives P2 (READY), so nothing in P1-P7 or S1-S5 prevents it there.

The general hazard: work started for an older term (a reconciliation reply, a durable truncation intent) can change
a node's log after that node has started serving in a newer term.

## 2. Proposed correction

### 2.1 Rules

**R6.3′ (amends R6.3). Term-tagged reconciliation.**
1. Every reconciliation reply carries the term of the primary that computed it. `k` writes an intent from a reply only
   if all three hold, checked against its durable state when it writes the intent:
   - the reply's term equals `k`'s durable highest term;
   - `k` is not a primary;
   - `k` is not yet positioned in that term.

   Any other reply is discarded.
2. The intent is durable as `(t_j, L)` (as R6.3 already says) and is written in the same durable record as, or ordered
   after, the term check of item 1. So a reply that was valid when it arrived cannot be written after `k` persisted a
   newer term [INF].

**S2′ (amends S2). Quiescence before serving.** After its promotion is chosen and it has persisted `t+1` (S1), `j`
must quiesce before it serves:
1. It **completes** any pending intent, then clears it durably (R6.3). Completion is idempotent, and it is what
   recovery already does. It is not merely cancelled.
2. It discards any reply not yet written as an intent (R6.3′ item 1 rejects it, because `j`'s durable term is now
   `t+1` and `j` is a primary).
3. Only then does it drain (R6.1) and do anything that counts as serving (R3): acknowledging, answering reads,
   granting standby leases, recording READY, shipping, answering reconciliation, writing new entries.

**P6′ (amends P6).** `j` also attests that it is not reseeding. R6.5 already makes it ineligible while its
`incomplete` flag is set. A pending intent does **not** make `j` ineligible, because S2′ handles it, so availability
is not reduced. The witnesses still cannot check these local facts directly.

### 2.2 Why completion and not cancellation

A pending intent `(t_j, L)` truncates only entries above `L` that the older primary `j` did not hold. By I4, every
acknowledged entry is in `j`'s log, so completing the intent **before** serving removes nothing acknowledged
[INF; R6's existing argument]. The danger is only completing it **after** serving. Cancelling has two problems:
- **A cancellation in memory is lost on a crash.** Recovery re-applies the durable intent (R6.3), now after the node
  may have acknowledged entries above `L`. That deletes them. The model checks this ordering (run MD, section 4).
- **A durable cancellation** (clearing the intent in the same write that persists `t+1`) would also be safe, but only
  if a truncation can never be half applied. That needs the truncation to take effect in one durable write of the log
  end, a WAL format requirement (AFR-D-06). Completion does not depend on it, because re-applying an intent is
  idempotent.

So completion is the recommendation. Durable cancellation is an alternative, and it is not checked here.

### 2.3 The six cases (Codex item 4)

| # | Case | Handling |
|---|---|---|
| 1 | Pending durable truncation intents | S2′ item 1: completed and cleared durably before serving |
| 2 | Reconciliation in progress against a previous primary | R6.3′ item 1: a reply from an older term is discarded. A reply that is already an intent is case 1 |
| 3 | Interrupted reconciliation across crashes | Recovery re-applies a pending intent (R6.3). Before S2′ completes, that is safe, as 2.2 argues. After S2′ completes there is no intent left. A reply that arrives after a restart is still tag-checked, and the model keeps replies in flight across a crash |
| 4 | Cancellation or completion of obsolete work | Intents: completion (2.2). Replies: discarded by the term check. No in-memory cancellation is relied on |
| 5 | Recovery of the promoted node before accepting writes | A promoted node that crashes before it serves is no longer primary after the restart (R6.0). It recovers like any member (R6.3, R6.5), and it serves again only through a new promotion |
| 6 | Operator promotion that waives READY | S2′ applies to every promotion, automatic or operator. P2 is the only condition the operator waives |

### 2.4 Downstream implications [INF]

- **Rust/WAL.** The intent record holds `t_j`, and the reply carries the term on the wire. The intent write is ordered
  with the term record. No new WAL record type is needed beyond R6.3's intent, so this adds no format change beyond
  AFR-D-06's existing items.
- **Failover time.** S2′ adds one local truncation to failover in the rare case that an intent is pending. X-1
  measures nothing of this yet.
- **Witness consensus.** No change. P6′ adds an attested bit only.

## 3. Degraded-primary self re-promotion (Codex item 5)

The rule is R2C 4.2: a restarted primary `p` that the record names as the sole member of S gets a new
witness-approved term `t+1` by the ordinary compare-and-set before it serves. It never resumes at `t`.

| Codex point | Where it is covered |
|---|---|
| Restart of the sole surviving primary | Model: `Crash` of the primary after S has shrunk to {p} (run P1 shows it is reachable) |
| Witness record recovery and comparison | P7 boot read: `Promote` requires `p`'s durable term to equal the record's term. The witnesses' Paxos itself is Authority.tla's concern and is not modelled here |
| Re-promotion to a new term | `Promote`'s self branch; `selfT` records the term it created |
| R3 lease-expiry barrier | Untimed `Barrier`: nothing at the new term is served while a lease of a lower term is held. Run MB removes it for the self re-promoted term only |
| Durable term fencing | `Learn` persists the term first (R4). A standby stops serving lease reads when it learns it (R9-D), and run MS removes that |
| WAL continuity and `(term, LSN)` uniqueness | `HistoryUnique` and `LogMatching` in `Safety` |
| Refusal when the durable log cannot be trusted | **Not modelled.** The models have no damage or silent loss. This is G-14(a), and its silent-loss part is AFR-D-02. The R2C 4.2 dependency stays: if `p`'s newest flush was silently lost, reconciliation against the re-promoted `p` can delete the last copy held by a member in A but not in S. **UNRESOLVED until D-02** |

## 4. Preregistration (committed before any run)

**Model.** `spec/tla/PromoteRecon.tla`. It is `Log.tla` with R2′ fixed on, plus the following, each with a switch:
- operator promotion inside S (`OpInside`);
- reconciliation replies in flight (`FixRepTerm`);
- quiescence (`FixQuiesce`, `FixDurable`);
- untimed standby leases (`FixLeasePos`, `FixDropWait`, `FixStopOnLearn`);
- the R3 barrier for a self re-promoted term (`FixBarrier`).

`PromoteRecon.cfg` is lane X1. Three nodes, `P0 = n1`, S starts as all three nodes.

**Invariants.**
- `Safety`: `AckedPreserved`, `EntryUnique`, `LogMatching`, `StateMatchesLog`, `PositionedPrefix`, `HistoryUnique`,
  `StandbyFresh` (G-30: a leased standby holds every acknowledged entry) and `LeasedPrefix`.
- `LeasedPrefix` is stronger than G-30. While the granting primary is current, a leased standby holds a prefix of
  that primary's log.
- `NoSelfRepromotion` is a reachability probe. TLC must report it violated.

**Abstractions (limits, stated before the runs).**
- Leases and the R3 barrier are untimed. Their timing is Combined.tla's and StandbyLease.tla's evidence.
- A standby's durable log stands for its applied state, so R9-A is not checked here.
- Witness Paxos is a single atomic record.
- There are no torn tails, no silent loss and no volatile standby tails.
- MaxTerm 3 allows at most two promotions.

**Runs.** The tool setup is as in `spec/tla/RESULTS.md`:
- `-workers 2 -Xmx6g -deadlock`, two runs at a time at most, `states/` deleted after every run;
- wall-clock cap **2 h** per run, and a run that reaches it is **NOT COMPLETED**;
- bounds are never raised after a result.

| Run | Bounds | Change from X1 | Invariant | Expected | Acceptance |
|---|---|---|---|---|---|
| X1 | MaxTerm 3, MaxIds 1, MaxCrashes 1, OpInside | none | Safety | PASS | required for the correction to be supported |
| X2 | MaxTerm 3, MaxIds 2, MaxCrashes 0, OpInside | none | Safety | PASS | required; NOT COMPLETED is reported, not accepted |
| P1 | X1 | none | NoSelfRepromotion | violated (reachable) | required: otherwise MB and the 4.2 claims are vacuous |
| MQ | X1 | `FixQuiesce = FALSE` | Safety | violated: M4's shape through operator promotion | the counterexample must come back |
| MQa | X1 | `FixQuiesce = FALSE`, `OpInside = FALSE` | Safety | no violation (READY covers automatic promotion) | record. A violation is a blocker until explained |
| MD | X1 | `FixDurable = FALSE` (cancel in memory) | Safety | violated: a crash re-applies the intent after acknowledging | shows cancellation alone is not enough |
| MR | X1 | `FixRepTerm = FALSE` | Safety | violated: a stale reply truncates a serving primary | shows the term check is needed |
| MB | X1 | `FixBarrier = FALSE` (self re-promoted term only) | Safety | violated: `StandbyFresh`, a lease from the old term outlives the restart | shows the barrier is needed for 4.2 |
| MW | X1 | `FixDropWait = FALSE` | Safety | violated: `StandbyFresh` (as R9's M_dropWait) | consistency with RESULTS_R9 |
| MP | X1 | `FixLeasePos = FALSE` | Safety | uncertain: `LeasedPrefix` may fail for an unpositioned survivor | record: tests R2′ rule 2's lease clause, which M2b could not |
| MS | X1 | `FixStopOnLearn = FALSE` | Safety | uncertain: R9 needed it only with outside-S promotion | record |

**Stop rules.**
- Any violation in X1 or X2 blocks the correction, explained or not. A fix made after it is rerun within the budget
  below.
- A mutation that is expected to fail but finds nothing means that rule is **not shown necessary** at these bounds.
  It is reported that way, not as a pass.
- An unexplained violation in a mutation run is a blocker.
- **Budget:** at most 11 launches for the table above, plus at most 6 reruns after on-site fixes. No other runs.

## 5. Results

To be filled in after the runs (section 4 is not edited after them).
