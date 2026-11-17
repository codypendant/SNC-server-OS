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

Revised after the three-way review (section 6). Every rule here is PROPOSED.

**R6.3′ (amends R6.3). Reconciliation work cannot outlive its context.**
1. **Binding.** Each R6.2 request from `k` carries:
   - the group and stream it concerns;
   - `k`'s boot incarnation, a durable counter incremented and persisted at every start, before the first request;
   - a request number, counted within the incarnation;
   - `k`'s last term and durable end LSN when it sent the request.

   The reply echoes these and adds the answering primary's term `t_j`. `k` uses the reply only if all five hold:
   - `t_j` equals `k`'s durable highest term;
   - the incarnation and the request number are those of `k`'s latest outstanding request, and `k`'s log has not
     changed since it sent it;
   - `k` is not a primary, is not positioned in `t_j`, and is not incomplete or reseeding (R6.5);
   - `k` has no pending intent;
   - the truncation point is not below `k`'s current checkpoint. This is R6.4, re-checked when the intent is written,
     because a checkpoint may have been taken while the reply was in flight (found by TLC, X1). A checkpoint that is
     still in progress counts: the intent write waits for it or aborts it.

   Any other reply is discarded, and the next step starts again: a duplicate, a late reply, a reply from before a
   restart, a reply to an older request, a reply for another stream.

   On the answering side, `j` stops answering as soon as it stops being primary, including when it loses its
   in-memory tail on a write error without crashing (R6.0). Its record that `k` is positioned ends when `k`'s
   incarnation changes, as a Raft leader resets its matchIndex for a restarted follower. (Added after the review of
   this step, `docs/AFR_001_RECON_BLOCKERS.md` section 4.)
2. **One serialized step.** The checks of item 1 and the durable intent write `(t_j, L)` form one local step. It is
   serialized against persisting a newer term (R4), against positioning, against checkpoint publication, and against
   S2′'s quiescence test. Concretely: the durable term, the term history, every stream's intent and the `incomplete`
   flag live in one metadata store that is read before the WAL scan, and the intent write is a compare-and-write
   that fails if the term changed.
3. **A pending intent blocks the node, in every term.** While `k` has a pending intent, it:
   - sends no R6.2 request;
   - is not positioned;
   - does not count toward an acknowledgement or toward READY;
   - is not granted a standby lease;
   - takes no checkpoint.

   If it learns a newer term with an intent pending, it persists that term first (R4), then completes the intent
   before any other action in that term. Completion runs in-process. A restart instead would forfeit a promotion
   (2.3 case 5). (Ordering stated after the review of this step; the model does exactly this. Found by review: without this,
   a node holding an intent from term `t` could be positioned against a term `t+1` primary that holds entries the
   term-`t` primary lacked, count toward an acknowledgement of one of them, and then delete it by completing the old
   intent. `PromoteRecon.tla` enforces this rule as a guard and never removed it. `ReconBlockers.tla` run MI removes it and TLC finds this trace; run C2 passes with it.)

**S2′ (amends S2). Quiescence before serving.** After its promotion is chosen and it has persisted `t+1` (S1), `j`
quiesces:
1. It **completes** any pending intent and clears it durably. Completion is defined in 2.2. It is not merely cancelled.
2. Every reconciliation reply still outstanding is now unusable (R6.3′ item 1).
3. Only then does `j` drain (R6.1), set `inh_j` (its log end after quiescence and drain), and start serving as R3
   defines it (acknowledging, answering reads, granting standby leases, recording READY). Shipping, answering
   reconciliation and writing new entries also wait for quiescence.
4. Quiescence may overlap the R3 `learned + W` wait. It is local and serves nothing. Failover then costs
   max(W, quiescence) rather than their sum.

S2′ applies to every promotion: automatic, operator inside S (P2 waived) and operator outside S (R7). Outside S, the
possible loss of acknowledged writes is already accepted by contract (G-35). Completion there can remove a copy of an
entry that a later primary acknowledged without `j`.

**P6′ (amends P6).** `j` also attests that it is not reseeding. This is defence in depth. Rule 3 of R2′ already
removes a reseeding member from S, and the shrink's compare-and-set makes the promotion fail. A pending intent does
**not** make `j` ineligible, because S2′ handles it.

### 2.2 Completion, and why not cancellation

**Why completing before serving is safe [INF].** Let `k` hold a pending intent `(t_j, L)` computed against primary
`j`. Every entry of `k`'s log above `L` was absent from `j`'s log at that point.
- **Entries acknowledged at terms below `t_j`.** They are in `j`'s log by I4, because `j` was in S when it was
  promoted. So they are at or below `L`.
- **Entries acknowledged at term `t_j` or later while the intent is pending.**
  - Each such acknowledgement needs every member of A positioned in that term.
  - By R6.3′ item 3, `k` is positioned in no term while the intent is pending. So either `k` was not in A, and then
    not in S, or the acknowledgement could not happen.
  - `k` re-enters S only through Grow, which requires `k` to be positioned, so the intent has been completed by then.
- So completing the intent removes nothing that any member of S must hold.

**Exception.** R7 outside-S promotion of `k` (G-35), where loss is accepted by contract.

**Completion is a crash-consistent procedure, not one write.** It has to be specified as such for the C++ reference
and for Rust. The C++ has no truncation and no intent today. Requirements [INF, from review; not modelled, because
the model's `ApplyIntent` and `Crash` are atomic]:
1. The valid log end `L` is recorded logically, in the metadata store of R6.3′ item 2, before any file is changed.
   The WAL scan at recovery consults the intent before it scans.
2. Records above `L` are removed from the newest segment backwards, followed by a directory fsync.
   - A block that also holds records at or below `L` is never rewritten. Appends resume in a new segment starting at
     `L+1`.
   - Spare files that recovery could adopt by their header (`storage.cpp:797-812`) are dealt with before the
     intent is cleared.
   - Segment names must not collide with names of removed segments (today they are "never reused",
     `storage.cpp:539`).
3. Applied state above `L` is undone by rebuilding: load a checkpoint at or below `L` and replay the WAL to `L`. The
   C++ applies in place on receipt (`replication.cpp:753`). So completion, and also the positioning of a truncated
   survivor, is a state rebuild on the failover path. It is not "one local truncation". Its cost belongs in X-1.
4. The intent is cleared only once 1 to 3 are durable. Re-running the procedure after a crash at any point gives the
   same result. That is the idempotence the argument relies on.
5. Clearing the intent must not lose the valid end of the old segment. Item 2 leaves records above `L` inside a block
   that also holds records at or below `L`, and appends resume at `L+1` in a new segment. Without a marker, a later
   recovery would find two contents for `L+1` (G-15). So a durable valid-end marker for that segment stays in the
   metadata store until the segment is reclaimed, independent of the intent. The alternative is to copy the mixed
   block's valid prefix into the new segment. (Found by the downstream review of this step.)

These are WAL and metadata format items for AFR-D-06, and they need a crash-consistency test.

**Why not cancellation.**
- A cancellation **in memory** is lost on a crash. Recovery re-applies the durable intent after the node may have
  acknowledged entries above `L` (model run MD).
- A **durable** cancellation, clearing the intent in the same write that persists `t+1`, is safe only if a truncation
  can never be half applied. It is not checked here.

Completion is the recommendation.

### 2.3 The six cases (Codex item 4)

| # | Case | Handling |
|---|---|---|
| 1 | Pending durable truncation intents | R6.3′ item 3: they block the node in every term. S2′ item 1: completed and cleared before serving |
| 2 | Reconciliation in progress against a previous primary | R6.3′ item 1: a reply from an older term, or for an older request or incarnation, is discarded. A reply already written as an intent is case 1 |
| 3 | Interrupted reconciliation across crashes | Recovery completes a pending intent (2.2), which is safe before serving. A reply that arrives after a restart belongs to an older incarnation and is discarded (item 1). `PromoteRecon.tla` keeps one reply in flight across a crash, with no duplicates. `ReconBlockers.tla` adds duplicated, reordered and lost replies and checks the binding at bounded scope (`docs/AFR_001_RECON_BLOCKERS.md`, runs C1 and MB) |
| 4 | Cancellation or completion of obsolete work | Intents: completion (2.2). Replies: discarded by binding and term. No in-memory cancellation is relied on |
| 5 | Recovery of the promoted node before accepting writes | If the node persisted `t+1` and then crashed before serving, it is not primary after the restart (R6.0); it recovers like any member and serves only through a new promotion. If it crashed before learning, the boot read (R7) tells it of the promotion and it starts at S1 |
| 6 | Operator promotion that waives READY | S2′ applies to every promotion, automatic or operator, inside or outside S. P2 is the only condition waived |

### 2.4 Downstream implications (open items)

- **WAL and metadata format (AFR-D-06):**
  - the metadata store of R6.3′ item 2;
  - per-record terms, which are needed by R6.2 and R6.3′ and should be a format item in their own right, not tied to
    D-02;
  - the completion procedure of 2.2;
  - a crash-consistency test.
- **Wire.** Recommendation: a term on every replication frame (records, acknowledgements, lease requests and grants,
  positioning, reconciliation replies), as Raft does [EXT: Raft, every RPC carries the term], plus the request binding
  of R6.3′ item 1. Today the C++ carries the term only in `Hello` (`replication.h:34`).
- **Apply before acknowledging (R9-A) in Rust.** If the WAL is written before applying, as in Raft, the acknowledged
  position is min(durable, applied). That puts apply on the synchronous acknowledgement path. Its cost belongs in
  X-1.
- **Witness record and consensus.** Today's record (`snc_witness.cpp:112-138`) has no S, mode, `m` or self-promotion
  path. Self re-promotion needs a restarted primary to ask the witnesses, the P6′ and P7 attestations in its status,
  and a linearizable boot read (D-01 conditions A2 and A7).
- **Failover time.** The state rebuild of 2.2 item 3, overlapped with W (S2′ item 4). It is unmeasured.
- **D-02.** A watermark of D-02(a) must be lowered in the same step as a legitimate truncation, or it would trigger a
  G-14(b) refusal. Framing of D-02(b) must tell a half-done truncation from damage. Added to the D-02 row. The intent
  itself (term, `L`, checksum) must be protected, so that a damaged intent cannot silently lower the watermark.
- **Scope per stream (open, from the downstream review of this step).** Intents, the block of R6.3′ item 3 and the
  binding are per stream. But checkpoints are node-wide today (one superblock with a start LSN per stream,
  `docs/CHECKPOINT_BACKPRESSURE_DESIGN.md` 1.1-1.2). "Takes no checkpoint" must therefore be defined per stream:
  for example, the node-wide checkpoint keeps the pending stream at its previous start LSN. Otherwise one shard's
  reconciliation stalls checkpoints, and so WAL reclamation, for every shard. A pending intent also needs a stated
  time bound, after which the stream is reseeded or refused. Whether the metadata store of R6.3′ item 2 is one store
  per node (a serialization point across cores) or one per shard (R4's term write is then not atomic across
  shards) is a D-06 decision.
- **Self re-promotion costs (open).** Each restart of a sole primary costs recovery, the boot read, a witness
  compare-and-set, `W` and quiescence. Under m = 2 it restores reads only, because G-12 refuses writes while
  |S| < 2. A crash loop grows the term and the term history by one per boot, which needs a stated bound. A damaged
  or inconsistent witness record has no named refusal yet: it belongs in G-14(a) and the G-48 exceptions.

## 3. Degraded-primary self re-promotion (Codex item 5)

The rule is R2C 4.2: a restarted primary `p` that the record names as the sole member of S gets a new
witness-approved term `t+1` by the ordinary compare-and-set before it serves. It never resumes at `t`.

| Codex point | Where it is covered | Status |
|---|---|---|
| Restart of the sole surviving primary | Model: `Crash` of the primary after S has shrunk to {p} | reachable (P1) |
| Witness record recovery and comparison | P7 boot read: `Promote` requires `p`'s durable term to equal the record's. Witness Paxos itself is Authority.tla's concern | the condition is modelled; Paxos is not |
| Re-promotion to a new term | `Promote`'s self branch; `selfT` records the term | reachable (P1, MB) |
| R3 lease-expiry barrier | Untimed `Barrier`. Run MB removes it for the self re-promoted term, and a lease granted before the restart then outlives it. **The timed length is not checked:** StandbyLease.tla has no self re-promotion, and Combined.tla has no standby leases. Argument [INF]: every grant `p` sent before its crash precedes `learned`, so `learned + W` covers it | barrier needed (MB); length argued only |
| Durable term fencing | `Learn` persists the term first (R4); a standby stops lease reads on learning it (R9-D) | modelled |
| WAL continuity and `(term, LSN)` uniqueness | `HistoryUnique`, `LogMatching` | **UNRESOLVED**: X1 has a single entry, so these are trivially true there; X2 has two entries but no crash, so no self re-promotion. No run checks them on this branch. The argument is unchanged: the term is new |
| Refusal when the durable log cannot be trusted | Not modelled (no damage or silent loss). G-14(a); the silent-loss part is AFR-D-02 | **UNRESOLVED until D-02** |

**Availability gap (from review).** If the sole member crashes after its promotion is chosen but before READY (the
window covers W, quiescence and the drain), P2 blocks self re-promotion. Only an operator can then bring the group
back (R7). That is a G-48 exception, and it is recorded there. Whether self re-promotion from a non-READY record is
safe when `p` is the record's own primary is not analysed.

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

## 5. Results (2026-11-17)

Section 4 was committed in `2017cbe` before any run. Only its heading line has changed since.

**Deviations from the preregistration, disclosed.**
1. **The first launches aborted.** Ten of the eleven runs (all but X2) were first launched on `2017cbe`.
   - Seven (X1, MQ, MQa, MD, MR, MB, MW) stopped within 5 s on a TLC evaluation error: a reply record compared with
     the integer `NONE`. They checked nothing.
   - P1, MP and MS ended in violations before reaching that comparison.
   - The model was fixed on site in `6b92ac7` (a typed `NOREP`), and nine runs were relaunched on it. Read literally,
     those relaunches are reruns after an on-site fix, and with the five below they exceed the preregistered limit
     of six reruns (14 against 6). They are reported here. They were not counted at the time because the first
     launches had produced no usable result for the seven runs that aborted.
2. **X1 found a counterexample** (below). It was fixed on site in `6307554`, and X1, MQ, MD, MR and MB were rerun on
   the fixed model. X2 and MQa ran for the first time on the fixed model.
3. **P1, MW, MP and MS were not rerun after `6307554`.** The fix changed only `UsableRep`, which matters only while a
   reply is in flight. Strengthening it also enables `DropRep` in more states, so the fix does not only remove
   behaviour. Even so, each of these four traces is still a behaviour of the fixed model, because `rep` is `NOREP` in
   every state of each trace. An independent reviewer verified this by replaying them.
4. **No further runs were made after the three-way review.** Its findings changed the rules (2.1, 2.2). The changed
   rules are checked by no run. See "not checked" below.

**X1 counterexample (first run, depth 10), explained.**
1. n1 (term 1) writes an unacknowledged entry. n2 is promoted to term 2 with S′ = {n2, n3}, and n1 learns term 2.
2. n2 answers n1's reconciliation with "truncate to 0", and the reply is in flight.
3. n1 takes a checkpoint, which R6.3 allows because no intent is pending yet.
4. The reply arrives, n1 writes the intent, and applying it truncates below the checkpoint (`StateMatchesLog`).

This is finding T1 again, opened by a reply in flight. Fix: R6.4 is re-checked when the intent is written (R6.3′ item
1). It is a protocol finding, not only a model one.

| Run | Result | Distinct states, depth | Trace (shortest found) |
|---|---|---|---|
| X1 | **PASS**, exhaustive, 82 s (after the fix) | 891,002, 36 | |
| X2 | **PASS**, exhaustive, 13 min 26 s | 7,423,089, 43 | |
| P1 | violated as required: self re-promotion is **reachable** | 5 states | S shrinks to {n1}; n1 crashes; the witnesses re-promote n1 to term 2 with S = {n1} |
| MQ | **violated** | 12 states | M4's shape: n3, unpositioned with a term-1 entry, is promoted by an operator to term 3 (READY waived), writes an intent from n2's term-2 reply, becomes primary, acknowledges the entry, then applies the intent and deletes it (`AckedPreserved`) |
| MQa | **no violation**, exhaustive | 726,412, 37 | READY protects automatic promotion, as expected |
| MD | **violated** | 12 states | as MQ up to the acknowledgement, with the intent cancelled in memory; n3 crashes and recovery re-applies the durable intent, deleting the acknowledged entry while n3 is the only member of S (`AckedPreserved`) |
| MR | **violated** | 12 states | n3's reply from term 2 arrives after n3 became primary at term 3 and acknowledged; without the term check it becomes an intent and deletes the entry (`AckedPreserved`) |
| MB | **violated** | 10 states | n1 leases n2 (in A, outside S); S shrinks to {n1}; n1 crashes, is re-promoted to term 2, and acknowledges at once while n2's term-1 lease is still held without the entry (`StandbyFresh`) |
| MW | **violated** | 8 states | R9's M_dropWait: n3 is leased, dropped from A, and p acknowledges without it (`StandbyFresh`) |
| MP | **violated** | 7 states | n3, a survivor not yet positioned in term 2 and holding n1's unacknowledged term-1 entry, is leased by the current primary n2; its lease reads show an entry n2's log does not have (`LeasedPrefix`) |
| MS | **not shown** | 6 states | the model's `LeasedPrefix` fails, but only against n1, a deposed primary that has not learned of term 2, while the lease holder n3 is itself the new primary. Under the property as section 4 states it ("while the granting primary is current"), this is not a violation. `StandbyFresh` holds. R9-D is not shown necessary here |

TLC reports only "Safety is violated". The conjunct named in each row was found by evaluating the trace's final
state.

**What this shows (bounded, and only within the section 4 abstractions).**
- X1 and X2 pass with the corrected rules as they were before the review. X1 covers:
  - operator promotion inside S;
  - one reconciliation reply in flight that survives a crash;
  - untimed standby leases;
  - one crash;
  - self re-promotion.
- Within that model, these parts are necessary:
  - quiescence (MQ);
  - durable completion rather than cancellation in memory (MD, across a crash);
  - the reply term check (MR, which removes all three conjuncts together, so it does not show which one matters);
  - the R6.4 re-check (found by X1 itself);
  - the barrier for a self re-promoted term (MB);
  - R9-C's wait before dropping a member from A (MW);
  - the positioned part of R9-B, but for `LeasedPrefix` only (MP). This is the first model evidence for R2′ rule 2's
    lease clause.
- MQa confirms that READY alone covers automatic promotion, so the gap is specific to operator promotion.

**What it does not show, or checks only vacuously.**
- **Not checked at all:**
  - the review's changes: request binding, the serialized step, the pending-intent block in every term, and the
    completion procedure of 2.2;
  - P6′, because `Promote` keeps `~Busy(j)` in every run;
  - R6.3′'s atomicity, because `RecvRep` is atomic;
  - the barrier for promotions other than self re-promotion;
  - R9-D (MS).
- **The pending-intent block** (R6.3′ item 3) is built into the model as guards (`ReconStep`, `Ship`, `SGrant`,
  `MarkReady`), and no run removes it. Its counterexample needs four nodes (review finding P1, section 6), and the
  model has three.
- **One entry in X1.** `nid` is global and MaxIds is 1, so `EntryUnique`, `HistoryUnique` and `LogMatching` are
  trivially true there. After a truncating completion the new primary can never write again, so "serve new writes
  after quiescence" is covered only vacuously.
  - X2 has two entries but no crash, so it has no self re-promotion and no crash after quiescence.
  - No lane combines a crash, self re-promotion and two entries.
- **`inh` is set at `Learn`, before completion.** So after a truncating completion, leases and Grow are dead in the
  model until new entries arrive. That is conservative, and it removes "lease after completion" from X1.
- **The reply channel** has one slot per node, no duplication and no reordering, so the blocker of section 6 (QA
  finding P2/D1) cannot occur in it.
- **The untimed abstractions are stronger than the protocol:**
  - `Barrier` is re-evaluated globally at every acknowledgement;
  - `SGrant` requires `Current(p)`, so a deposed primary never grants, whereas the timed model has such grants
    (RESULTS_R9 M_m2);
  - R3 and the M2 cap are therefore built in and cannot fail here.
- **No coverage run.** Reachability in the fixed model rests on replaying trace prefixes (done by the model reviewer
  for RecvRep, operator promotion with a pending intent, a lease to a survivor, and self re-promotion with a held
  lease).
- **Liveness is not checked.** `-deadlock` is off. Under the X1 fix, a member of S whose checkpoint is above the
  truncation point cannot proceed until it is shrunk (rule 3).
- **Not in the model:** damage, torn tails or silent loss. The D-02 dependency of 4.2 is unchanged and
  **UNRESOLVED**. Durable cancellation is not checked either.
- **Unchanged from RESULTS section 9:** M2 and M2b still stand (positioned counting is not shown necessary for
  durability), and M3 still does not distinguish KIP-101 from KIP-279.

## 6. Three-way independent review (2026-11-17)

Three independent reviewers (Opus) were each told to break the work. One covered protocol correctness (P), one
model completeness (M), one downstream implications (D). None edited files or ran TLC. Findings and dispositions,
most severe first. Duplicates are merged.

| # | Finding | Severity | Disposition |
|---|---|---|---|
| P2 / D1 | A same-term reply that is late or duplicated, used after a restart (positioning is volatile), deletes an entry acknowledged after positioning. The model's single reply slot hides it | **blocker** | Fixed in text: request binding (R6.3′ item 1). **Not checked**: needs a model with duplicated replies (section 7) |
| P1 | A node holding an intent from term `t` can be positioned against a term `t+1` primary that holds entries the term-`t` primary lacked, count toward an acknowledgement of one of them, then delete it by completing the old intent. Needs four nodes; the model enforces the guard silently | **blocker** | Fixed in text: R6.3′ item 3 (pending intent blocks the node in every term), also in R9-B and R2′ rules 2 and 4; 2.2 argument rewritten. Disclosed in section 5. **Not checked by mutation** |
| P3 / D3 | Term check and intent write not atomic against R4's term write, S2′'s check, or per-shard streams | major | Fixed in text: one serialized step, one metadata store, compare-and-write (R6.3′ item 2) |
| D2 | "Completion is idempotent, as recovery already does" does not hold for the real segmented WAL: segment gaps, partially zeroed blocks, spare adoption, segment name reuse | major | Fixed: 2.2 defines completion as a crash-consistent procedure; listed as D-06 format items; crash test needed |
| D4 | Truncating applied in-place state needs a state rebuild; failover cost understated | major | Fixed: 2.2 item 3, 2.4 |
| D5 | Checkpoint in progress can publish above `L` after the re-check | major | Fixed: R6.3′ item 1 (a checkpoint in progress counts; wait or abort) |
| P4 / D7 | R9-E misstates p's anchor; the cap's drift conversion is unstated | major | Fixed: R9-E and G-30 give both anchors and the cap formula [INF, not checked] |
| P6 | R9-A binds only members of A; a catching-up replica's durable report can be counted after it joins A | major | Fixed: R9-A covers every replica's reports; R9-B is evaluated on the reported applied position |
| D6 | Standby lease reads can show writes never acknowledged and later truncated; G-30 silent; R2′ makes it reachable with 3+ members | major | **Owner decision** (section 7, item 3) |
| D8 | Per-frame or per-connection term tagging undecided | major | Recommendation in 2.4 (per frame) |
| D9 | Sole member crashing before READY needs an operator | major | Recorded as a G-48 exception (section 3, SG G-48) |
| D10 | G-55 ("resumes at its durable term, HOLDS") contradicts R6.0 and 4.2 | major | G-55 reworded, status PARTIAL |
| D11 | D-02 dependency broader (per-record terms, watermark vs truncation, framing vs half-done truncation) | major | D-02 row amended; per-record terms moved to a format item (2.4) |
| D12 / P5 / M1 | Overclaims: format "no change", witness "no change", failover "one truncation", X2 claimed before it finished, P6′ "necessary" | major | Fixed: 2.4 restated as open items; X2 reported after it finished; P6′ listed as not checked |
| M2 | MaxIds 1 makes EntryUnique, HistoryUnique and LogMatching vacuous in X1; no run checks 4.2 uniqueness | major | Section 3 row UNRESOLVED; section 5 limits |
| M3 | MS's trace compares against a deposed primary; not a violation of the stated property | major | MS reported as "not shown" |
| M4 | No timed model checks the barrier for self re-promotion with standby leases | major | Section 3: barrier length argued only |
| M5 | Budget miscounted (10 first launches, not 11; NOREP relaunches exceed the literal rerun budget) | major | Section 5 deviation 1 corrected |
| P7-P12, D13-D18, M6-M12 | Minor: R6.3′ texts disagreed; `inh_p` before quiescence; serving list; R7 outside-S scope; G-30 evidence for D; lease lost on standby restart; R1.2 cross-reference; case 5 ambiguity; stale G-15/G-35/G-48 text; R9-A cost in Rust; P6′ redundant; S2′ outside S; quiescence vs barrier ordering; "fix only removes behaviour"; abstraction strength; `inh` in the model; reply channel limits; untested parts; liveness; no coverage run | minor | Fixed in text (2.1-2.4, sections 3 and 5, SA R1.2/R9, SG rows) |

**Not broken by any reviewer:**
- S2′ completion for a node promoted from S, given P1's rule, across a crash before or after the intent;
- the term tag against replies from other terms;
- R9-B, R9-C and R3 against leases granted by the old primary across a promotion, including self re-promotion;
- `(term, LSN)` uniqueness under self re-promotion (by argument);
- no old R9 guarantee lost in the rewrite;
- no new two-primaries interleaving;
- the validity of P1, MW, MP and MS traces without rerun;
- preregistration integrity;
- UNCHANGED completeness of every action.

## 7. Recommendation and owner decisions

**Update, 2026-11-17 (after Codex's review of #153).** Decision 1 was authorized and run: see
`docs/AFR_001_RECON_BLOCKERS.md`. Decision 3 was adopted as R9-F (PROPOSED, text only). The text below is kept as
written for #153.

**Recommendation.**
- Adopt S2′, R6.3′ (with binding, serialization and the pending-intent block) and P6′ as the PROPOSED correction.
- Keep R2′ and R9 **unfrozen**. The bounded evidence supports the mechanism: X1 and X2 pass, and quiescence,
  durable completion, the reply term check, the R6.4 re-check and the self re-promotion barrier are each shown
  necessary.
- But the two blockers the review found are fixed only in text. A freeze should wait for a model check of them.

**Decisions needed from cody:**
1. **Authorize one more focused model step**, preregistered in the repo before it runs. Proposed scope:
   - replies as a bag, with duplication and reordering, and the request binding (fixed lane plus a mutation without
     binding, expected to reproduce P2/D1);
   - a four-node lane with the pending-intent guard removed, expected to reproduce P1;
   - one lane with MaxIds 2, a crash and self re-promotion, for 4.2's uniqueness claim.

   Same limits: at most 2 h per run, no R640, no raised bounds after results, a timeout is NOT COMPLETED.
2. **Freeze order.** R9-A to R9-E wording can be reviewed for freeze independently of the reconciliation changes.
   R2′ should not be frozen before item 1.
3. **Standby reads of unacknowledged data (D6).** Recommendation: a leased standby serves reads only up to the
   primary's acknowledged LSN, which the primary sends with each frame (as a Raft follower learns the
   commit index from `leaderCommit` in every AppendEntries) [EXT: Raft paper, Figure 2]. The alternative is to state in G-29 and G-30 that lease reads may show writes that are
   later discarded. The recommendation makes `LeasedPrefix` moot for clients, and it costs nothing on the
   acknowledgement path.
4. **D-02 stays on HOLD.** The silent-loss dependency of 4.2 is unchanged. No decision is asked here.
