# SNC-AFR-001: R2 survivors and single-copy recovery

- **Status: PROPOSED.** Nothing here is frozen. It authorizes no implementation and no R640 runs.
- **Owner direction, 2026-11-17** (`docs/handoff/PR149_150_Owner_Direction.md`):
  - R2′ is the **preferred design candidate**, not frozen until the model evidence and independent QA are reviewed.
    The bounded validation in 2.4 is authorized: at most eight runs, two hours each, no R640. An unexplained safety
    counterexample is a blocker; a timeout is NOT COMPLETED.
  - P1-P7 and S1-S5 are the **proposed** replacement for E1-E11, subject to verification against the selected R2.
    Promotion eligibility stays distinct from permission to begin serving.
  - Standalone node: monotonic LSN reservation is the **preferred candidate**. Restarted degraded-group primary:
    witness-coordinated re-promotion into a new term is the **preferred candidate**; it must never resume serving
    under its old term; fencing, leases, log matching and recovery are to be verified before it is frozen.
  - D-02 and the silent-loss durability boundary remain unresolved. No D-02 selection and no R2 freeze.
- **Trigger.** cody's owner decisions on PR #148 (`docs/handoff/PR148_Owner_Decisions.md`):
  - section 2, the R2 evaluation against eight points and a rewrite of E1-E11;
  - section 5, single-copy recovery by monotonic LSN reservation, establishing seven points.
- **Out of scope.** X-2 preregistration and the R9 model belong to separate threads.
- **Inputs.**
  - Protocol: `docs/AFR_001_SAFETY_ANALYSIS.md` (SA).
  - Contract: `docs/SNC_GUARANTEES.md` (SG).
  - Evidence: `spec/tla/RESULTS.md` (RES).
  - Current eligibility rules: `docs/CORE_CONTRACTS.md` section 8.
- **Labels.** FACT (quoted from a primary source), FACT-SNC (checked in this repository), EXT, INF (inferred here),
  HYP (unverified).
- **Review.** An independent adversarial QA reviewed this document. Its findings and the corrections are in
  section 6.

## 0. Recommendations

| # | Question | Recommendation |
|---|---|---|
| 1 | R2: after a promotion, new S = {j}, or keep verified survivors? | **Keep survivors**: new S = old S without the old primary. A survivor's acknowledgements count only after it is positioned in the new term. A survivor that must reseed leaves S first. READY waits until every member of S is positioned. The bounded runs passed (2.5); **freeze only after they are reviewed.** |
| 2 | E1-E11 | Replace them with the in-sync-set rules in section 3 |
| 3 | Single-copy recovery | **Standalone node: monotonic LSN reservation** with a durable local mark. **Degraded group (S = {p}, witnesses exist): the witnesses re-promote the restarted p to `t+1` by the ordinary compare-and-set**, instead of reservation, because QA showed reservation inside a group breaks rules other proofs depend on (section 4). Both are now cody's preferred candidates (2026-11-17), not frozen. Both still depend on D-02 for silently lost writes |

What does not change: the safety invariants (Claims L and D, G-15), `m = 2`, and the in-sync-set direction.

## 1. Why R2 matters

Today R2 sets S = {j} after every promotion (SA R2). Under `m = 2` this has two costs:
- **Writes.** They stop after every failover until a survivor catches up and a grow is chosen.
- **Recoverability.** While S = {j}, survivors are outside S. If j fails before the grow, no automatic promotion is
  possible, although the survivors hold every acknowledged write (I4). Operator promotion outside S is then allowed
  only from a degraded, READY record or with every member of S fenced (R7). Neither holds right after a promotion.

## 2. R2: `{j}` against verified survivors

### 2.1 What other systems do

Every system checked except one keeps survivors as members after a leader change, and resets what they count for:

| System | Survivors after a leader change | When a survivor counts again |
|---|---|---|
| **PacificA** (MSR-TR-2008-25) | Kept: "The secondary proposes a new configuration with itself being the new primary and with the old primary excluded" [EXT] | The new primary "starts to process new requests only after it completes a reconciliation process", re-committing its prepared requests on the new configuration. Replicas "truncate their prepared lists and keep only all requests up to serial number sn" [FACT] |
| **Kafka** (KRaft controller; `Partition.scala`) | Kept. The failed broker is removed from the ISR. Only an unclean election "forcibly set[s] the ISR to just the new leader. This can result in data loss!" [FACT] | The new leader resets remote log-end offsets. A replica must be "caught up to an offset within the current leader epoch" [FACT]. Truncation is by leader epoch (KIP-101, iterated in KIP-279) |
| **Raft** (Ongaro thesis) | Configuration unchanged | `matchIndex` is "Reinitialized after election". "Only log entries from the leader's current term are committed by counting replicas" [FACT] |
| **Chain replication** | Kept; only the failed node is removed [FACT] | Downstream history is a prefix of upstream history, so there is nothing to reconcile [INF] |
| **Vertical Paxos II** | The new configuration is "initially inactive" until "the state transfer … is complete" [EXT] | After the new ballot's phase 2 [INF] |
| **FoundationDB** | Replaced: recovery recruits new log servers and discards data after the recovery version [FACT] | — |

**Documented failures to avoid:**
- **KIP-101.** Truncating to the high watermark lost a committed message.
- **KIP-279.** Divergence after fast back-to-back failovers.
- **KIP-966.** An uncleanly shut-down member was re-elected and made followers truncate.
- **Jepsen Kafka 2013.** Acknowledged writes were lost (about half in the reported run; count not re-checked) when the ISR shrank to one and an arbitrary node
  was promoted.
- **Redpanda #3003 and #3616.** Uncommitted entries were applied, and a term was not re-read across leadership
  changes.

Sources are listed in section 7.

Quotes marked [FACT] were read in the primary source during this work. QA could not confirm from memory the PacificA,
Vertical Paxos II and Redpanda wording; treat those rows as [EXT] until re-checked against the sources in section 7.

**The pattern:** keep survivors, but count none of them until it has matched the new leader's log in the new epoch.
That needs three things:
- epoch-fenced replication;
- a divergence point found by (largest common term, end of that term);
- durable truncation before the survivor acknowledges.

SA already has all three: R4, R6.2 (KIP-279) and R6.3.

### 2.2 The proposed rule ("verified survivors")

**R2′ (replaces R2's last bullet).** A promotion is a compare-and-set to `t+1` naming `j ∈ S`, as today. It also
sets:
1. **The new set.** S′ = S \ {old primary}, or {p} when the sole member p re-promotes itself (4.2). The new
   primary's A starts as S′.
2. **A survivor counts only once positioned.** A member of S′ counts toward an acknowledgement, and grants j a lease,
   only after:
   - it has persisted `t+1` (R4);
   - it has completed R6.2/R6.3 against j's log, with the truncation durable, and has no pending intent from any term
     (R6.3′ item 3, `docs/AFR_001_PROMOTION_DURING_RECON.md`);
   - j has positioned it in term `t+1`.

   Until every member of A is positioned, j acknowledges nothing. This is today's rule that an acknowledgement needs
   every member of A, applied to survivors. j also grants a standby read lease (R9) only to a positioned member:
   an unpositioned survivor may hold an unacknowledged tail that reconciliation is about to truncate.
3. **Reseed means leaving S first.** A member of S that needs a reseed (R6.4) or would set the incomplete flag (R6.5)
   must first be removed from S by a chosen shrink. The model has already shown why: with survivors kept and no such
   rule, "a member still in S reseeds and drops an acknowledged entry" (RES section 3, L_keepS_kip279).
4. **READY.** It is recorded only after j's R3 point **and** once every member of S′ is positioned in `t+1`, with no
   pending intent, or has been removed. This plays the role of PacificA's reconciliation and Vertical Paxos II's activation.
5. **A survivor that does not position** within the failure-detection timeout is removed by the ordinary shrink.
   A standby lease it held from the old primary is already fenced by j's `learned + W` barrier (R3); j granted it
   none (rule 2).

### 2.3 The eight evaluation points

| # | Point | `{j}` (today) | Verified survivors (R2′) |
|---|---|---|---|
| 1 | **Acknowledged writes preserved** | I4: every member of S holds every acknowledged write. Model-checked within bounds | Same argument [INF]. Writes of terms ≤ t are on every member of old S (I4), so on S′. Writes of `t+1` need every member of A ⊇ S′, positioned. R6.2/R6.3 truncation removes only entries above the common prefix, and every acknowledged entry is in the common prefix (I3). Rule 3 targets the counterexample found so far (L_keepS_kip279: a member still in S reseeds and drops an acknowledged entry; there the reseeding node is the old primary, which rule 1 already removes, so the trace is weak evidence for rule 3 itself). TLC stops at the first violation, so other failures may exist. Existing evidence: `KeepS` (which keeps all of S, old primary included) passes I1-I3 exhaustively under KIP-279 at MaxIds 2 (L_keepS_kip279_recon, 28,625,801 states). **R2′ itself is not model-checked** |
| 2 | **Promotion eligibility and log-prefix evidence** | Only j is eligible until a grow | Any member of S′ holds every acknowledged write, positioned or not, so promoting it preserves them (point 1; subject to G-14(b), whose mechanism D-02 is on HOLD). Automatic promotion still needs READY (P2), so before READY only operator promotion inside S (P2 waived) gains. Evidence: membership in S at the chosen version, plus the durable `incomplete` flag (R6.5) being clear. No LSN comparison is needed. That matches today's E5 intent without the witness comparing LSNs |
| 3 | **Membership changes concurrent with promotion** | Compare-and-set on the version; the first chosen wins (SA 1, case 3a) | Same. A grow chosen first adds a member that was caught up and in A (R1), so keeping it is safe. A shrink chosen first removes it. A7 (lagging witnesses) is unchanged |
| 4 | **Lease fencing and authority handoff** | The old primary's lease needs grants from all of its A, which includes j. j stops granting when it learns `t+1`, so R3 holds | Unchanged: the same argument. A survivor grants j a lease only once positioned at `t+1` (rule 2), so j has no normal-mode authority until then. j serves after `learned + W` (R3) once every member of A has granted. If a survivor is dead, j serves only after it is removed (rule 5) |
| 5 | **Crash recovery, incomplete replication** | A survivor rejoins later through R6 and a grow | A survivor that crashes mid-truncation recovers through its durable intent (R6.3), still in S. A survivor needing a reseed leaves S first (rule 3). A survivor whose recovery detects damage refuses (G-14(a)) and is removed (rule 5) |
| 6 | **Two, three and more replicas** | — | **Pair: no difference.** S′ = {j} either way, so a pair under `m = 2` still stops writes on any loss. **Three or more:** writes resume once every member of S′ is positioned (S5) or removed, without waiting for a grow. Before READY, j's data is promotable only by operator promotion inside S |
| 7 | **Recovery time and write availability** | Writes return after catch-up, a grow and a Paxos round | Writes return after positioning: truncating the unacknowledged tail plus fetching j's suffix. That is usually small, because survivors were in sync. One Paxos round is saved. **Cost:** if a survivor is dead, j waits for the failure-detection timeout and a shrink before it has any authority, including for reads. Today, degraded mode serves reads after `learned + W`. Both durations are unmeasured (X-1) |
| 8 | **Implementation complexity** | Baseline | Small to moderate: S′ in the compare-and-set, an extra condition before READY, "leave S before reseed", per-member positioned tracking (Raft's `matchIndex`, Kafka's reset log-end offsets). No new message types [INF] |

**Assessment.** R2′ preserves the established invariants by the argument above. It strengthens recoverability: more
promotable copies of every acknowledged write. It can shorten the `m = 2` write outage for groups of three or more (unmeasured).
It changes nothing for pairs. Its one regression is read availability when a survivor is dead at failover. The
recommendation does not rest on performance (cody's rule): the main gain is that acknowledged data stays
automatically promotable.

### 2.4 Bounded validation plan (authorized 2026-11-17; results in 2.5)

**Model changes needed first** (QA found the models cannot test R2′ as they stand):
- an `Init` with S = all three nodes, so that S′ = S \ {p} has more than one member (today `Init` has S = {P0, Q0},
  so S′ = {j}, identical to R2, unless a grow happens first);
- in `Log.tla`, a `ready` field, so rule 4 and P2 can be checked;
- rule 2 as an explicit condition: neither model's `Ack` checks positioning today (`pos`), only the term and a
  matching entry;
- in `Combined.tla`, `Promote` sets `deg` from |S′| instead of always `TRUE`;
- out of reach of both models: a standby's volatile tail and torn tails, which "survivor crash mid-positioning"
  needs. These stay argued only.

| Run | Model | Change | Checks | Expected |
|---|---|---|---|---|
| V1 | `Log.tla` | R2′ rules 1-4, three-member `Init`, MaxTerm 3, MaxIds 2 | `Safety` (`AckedPreserved`, `PositionedPrefix`, `HistoryUnique`) | PASS |
| V2 | as V1 | MaxTerm 2, MaxIds 3 | same | PASS |
| M1 | as V1 | without rule 3 | `AckedPreserved` | violated |
| M2 | as V1 | without rule 2: an unpositioned member counts | `Safety` | **uncertain**. A member counted for durability must still hold the entry, so this may pass; rule 2 may matter only for leases (V3/M2b) |
| M2b | as V3 | an unpositioned survivor grants j a lease | `StaleAuthBounded` | violated |
| M3 | as V1 | KIP-101 single answer instead of KIP-279 | `PositionedPrefix` | violated (as L_keepS_kip101) |
| V3 | `Combined.tla` | R2′ promotion, leases from positioned members of A | `Safety`, `StaleAuthBounded` | PASS |
| M4 | as V3 | READY before every survivor is positioned | `Safety` | records whether rule 4 is necessary or only conservative |

**Cost.** Not estimated from evidence: the closest existing run (`KeepS`, MaxIds 2) reached 28.6 M states, against
5.1 M under R2, so R2′ runs may be several times larger than H_fixed. Proposed bound: each run gets a preset
wall-clock limit of 2 hours on the 4-core cloud container, no R640, at most 8 runs. A run that hits its limit is NOT
COMPLETED, not a pass, and is reported as such; bounds are cut rather than limits raised.

**Stop rule.** Any violation in a V run blocks the freeze, explained or not; a rule change after it must be re-run
within the same eight-run budget. An unexplained violation in a mutation run is also a blocker. Until then R2 stays
at `{j}`. The results get an independent review before R2 is
frozen.

### 2.5 Bounded validation results (2026-11-17)

Full table, traces and limits: `spec/tla/RESULTS.md` section 9. Eight completed runs, all exhaustive, no timeout; one
earlier launch of V2 was killed by a container restart and rerun, which is disclosed there.

| Run | Expected | Result |
|---|---|---|
| V1, V2 (Log), V3 (Combined) | PASS | **PASS** (14.2 M, 38.8 M and 19.2 M distinct states) |
| M1 (no rule 3) | violated | **violated**: a survivor still in S reseeds and drops an acknowledged entry |
| M4 (no rule 4) | record | **violated**: rule 4 is necessary, not only conservative |
| M2, M2b (no rule 2) | uncertain, violated | **no violation**: rule 2 is not shown necessary in these models |
| M3 (KIP-101) | violated | **no violation**, and the same state count as V1: the switch does not discriminate here |

**What it means for the freeze.** No V run violated, so the stop rule is not triggered. Rules 1, 3 and 4 have model
evidence within bounds. Rule 2 stays argued only (the models have no standby read leases), and self re-promotion
(4.2) is in the models but not shown reachable. R2′ is **not frozen**: the results still need cody's and an
independent review.

**Finding from M4.** Now addressed as a proposal by S2′, R6.3′ and P6′ (`docs/AFR_001_PROMOTION_DURING_RECON.md`,
checked within bounds there). The original note follows. M4's trace promotes a survivor that then applies a
truncation intent from a reconciliation it began against the older primary, after it has acknowledged the entry
the intent deletes. Rule 4 stops this for automatic promotion, because READY waits. Operator promotion inside S
waives P2 and is not modelled, so nothing stops it there. Proposed addition to P6 and S2: `j` has no reconciliation
in progress and no truncation intent pending, and on learning `t+1` it abandons any reconciliation against an older
primary before it serves. This is unverified: the eight-run budget is spent, so checking it needs another run.

## 3. E1-E11 rewritten for the in-sync-set model

These replace `docs/CORE_CONTRACTS.md` section 8 (E1-E11) for the protocol. They also hold under today's R2. Rows
marked R2′ apply only if R2′ is adopted.

**Automatic promotion.** The witnesses choose a compare-and-set naming `j` only if every row holds:

| # | Condition | Source |
|---|---|---|
| P1 | The group's mode permits automatic promotion: a sync group with witnesses. Asynchronous members are never in S | old E1; G-31, D-15 |
| P2 | The record is READY at its current version (R2; READY recorded after the primary's R3 point and, under R2′, after every member of S′ is positioned or removed) | old E2 |
| P3 | The promotion is a compare-and-set on the current version, to term `t+1`, chosen by a witness majority | old E3, E10 |
| P4 | `j` belongs to this group: same space and group identity | old E4 |
| P5 | `j ∈ S` at the chosen version. By I4, `j` then holds every acknowledged write, unless that copy was silently lost (G-14(b), D-02 on HOLD). It replaces old E5 and E6: the witness compares no LSNs | old E5, E6 |
| P6 | `j` attests, in the status it reports to the witnesses (as old E8's STATUS check), that its durable `incomplete` flag (R6.5) is clear, that it is not reseeding, and that it passed its recovery barrier, including G-14(a). The witnesses cannot check these local facts directly. A pending truncation intent does not make `j` ineligible; S2 handles it (P6′, `docs/AFR_001_PROMOTION_DURING_RECON.md` 2.1) | old E6, E8 |
| P7 | `j` attests that it read the group record at boot and that its durable highest term is ≤ `t` (R4, R7 boot read). Old E7's no-divergence condition is now covered by I4 and reconciliation (R6.2/R6.3) | new; replaces E7 |

**After the promotion is chosen, before `j` serves:**

| # | Condition | Source |
|---|---|---|
| S1 | `j` persists `t+1` (R4) and waits `learned + W` (R3) | R3 |
| S2 | `j` quiesces, then drains. It completes any pending truncation intent and clears it durably; it discards any reconciliation reply from an older term (R6.3′); only then does it drain the older-term entries it holds (R6.1) and serve. This applies to every promotion, including operator promotion that waives P2 (S2′, `docs/AFR_001_PROMOTION_DURING_RECON.md`; model runs MQ, MD, MR) | R6.1, R6.3′ |
| S3 | `j` reads placement linearizably and re-seals before serving slots (MS-3), and waits for any older-term source lease to expire (CD 5.2) | old E9; G-37 |
| S4 | `j` obtains a fresh mint lease block | old E11 |
| S5 | `j` acknowledges only once every member of A is positioned in `t+1` (R2′ rule 2) | new |

**Operator promotion** carries the expected term and goes through the same compare-and-set (R7).
- **Inside S:** P2 is waived. The barrier still applies.
- **Outside S:** only from a degraded, READY record, or with every member of S confirmed fenced. It requires an
  explicit acknowledgement of possible loss of acknowledged writes (G-35). Case 5c is not a protocol operation.

## 4. Single-copy recovery

**Problem.** A sole copy is a standalone node, or a degraded group's primary (S = {p}). If it restarts, R6.0 forbids
its old term, and R2 can promote only a different member. It therefore cannot resume, which is a G-48 gap. Resuming
at the end of its local log could reuse a `(term, LSN)` whose earlier content was lost in a crash (H_one_hu).

### 4.1 Prior art

- **PostgreSQL sequences** "pre-log a few fetches in advance. In the event of crash we can lose (skip over) as many
  values as we pre-logged" (`SEQ_LOG_VALS 32`) [FACT]. The docs warn that sequence values can have gaps after a
  crash [EXT, wording not re-checked].
- **Kafka KIP-730** persists each generated producer-ID block so it is never handed out again [EXT, quote not
  re-checked].
- **KAFKA-15552.** A migration misread the meaning of a stored block mark (last allocated against next free), and
  the same block was handed out twice [EXT, direction of the mix-up not re-checked]. The lesson: fix and test the
  mark's meaning at every handoff.
- **PostgreSQL timelines and pg_rewind** exist because an old primary's WAL can diverge after a failover. "The
  timeline ID number is part of WAL segment file names" [FACT]. A restarted copy gets a new history identity rather
  than reusing the old one.
- **Snowflake** refuses to issue IDs when "Clock moved backwards", but keeps the guard only in memory [FACT].

### 4.2 Degraded group: re-promote the restarted primary to `t+1`

QA showed that resuming in the old term inside a group (the reservation design of the previous draft) breaks
premises other rules rely on:
- R3 and R9 rely on R6.0 ("a restarted primary never serves its old term again"). A restarted p has forgotten A
  and any standby lease it granted to a catching-up member, so it could acknowledge without that member while the
  member still serves reads.
- p ships a flush to members before writing it locally (`src/core/storage.cpp:651`, FACT-SNC). If p crashes and
  loses the flush, resumes above its mark and writes `(t, 10)`, a member can hold `(t, 6..7)` and `(t, 10)` while
  p holds `(t, 1..5)` and `(t, 10)`. R6.2 works per term, so it cannot see this divergence, and `PositionedPrefix`
  breaks.
- The reservation would also have to be kept in normal mode, or a shrink to S = {p} followed by a crash reuses
  LSNs the removed member holds.

**Candidate rule.** A restarted p that the record names as the sole member of S may ask the witnesses for an
ordinary promotion compare-and-set to `t+1` naming itself. All of P2-P7 apply (P5 holds because p ∈ S), the boot
read included. If the record is not READY (p crashed before its own barrier), only operator promotion (R7) remains. Then S1-S4 apply:
p persists `t+1`, waits `learned + W`, drains, and serves in `t+1`. Every earlier lease and A is void with term `t`.
- R6.0 is unchanged. G-15 and HistoryUnique hold because the term is new and minted by the witnesses.
- The term stays the group's HA term, minted by the witnesses, not a local one. cody accepted this as the preferred
  candidate on 2026-11-17; p must never resume serving under its old term.
- G-55 holds: the node does not mint a term itself. G-48 improves only partly: the group resumes reads without an
  operator, but under `m = 2` it admits no new writes until a grow (G-12).
- It is a small case of today's promotion path [INF]. `spec/tla/PromoteRecon.tla` shows the branch is reachable
  (run P1). It also shows that the R3 barrier is needed for it (run MB): a lease granted before the restart outlives
  the restart (`docs/AFR_001_PROMOTION_DURING_RECON.md` section 3).
- **Depends on D-02 (re-review finding).** If p's newest flush was silently lost and p passes G-14(a) undetected,
  a member still in A but not in S may hold an acknowledged write p lacks. Reconciling that member against the
  re-promoted p truncates it, so the protocol itself deletes the last copy, where today an operator could still
  promote that member (R7). p cannot tell this from an unacknowledged shipped tail, because p ships before its local
  write. **Unresolved** until D-02; the models have no silent loss, so the runs cannot show it.

### 4.3 Standalone node: monotonic LSN reservation

A standalone node has no A, no standby leases and no members, so the failures in 4.2 do not arise.
1. **The mark.** `R_hi` is the highest LSN reserved, inclusive; the next free LSN after a restart is `R_hi + 1`.
   This meaning is fixed and tested (KAFKA-15552).
2. **Where it lives.** In the local superblock, in both copies, with a CRC and a generation number.
3. **Ordering.** The new `R_hi` is durable before any LSN above the old `R_hi` is assigned.
4. **Restart.** Resume at `max(R_hi, highest LSN in any intact flush frame) + 1`. The term does not change (G-55).
5. **R6.0** does not apply: a standalone node is not a group primary.

| # | Point | Result |
|---|---|---|
| 1 | **Crash safety** | A crash between reserving and using leaves a gap, never a reuse (rule 3) [INF]. A torn write of the newest mark copy is detected by its CRC; the node then uses the older copy, which rule 3 keeps ≥ every LSN assigned before that write began, plus the frame scan (rule 4) |
| 2 | **Silently lost reservation write** | If the mark write is lost but later frames survive, the scan in rule 4 resumes above them. If the mark and the frames above it are both silently lost, nothing local can tell, and an LSN could be reused for content that was never durable. Whether a LOCAL_DURABLE acknowledgement was lost too is G-14(b), so this **depends on D-02** |
| 3 | **Stale or rolled-back metadata** | Using the older superblock copy is safe only if its mark is ≥ every LSN assigned, which rule 3 ensures for a torn newest write but not for a silently lost one; proving that nothing exists above the older mark again needs D-02. The torn case of point 1 (newest copy fails its CRC) is exempt. A fallback for any other reason (both copies valid but the newer one missing or older than the WAL shows) refuses to start until D-02, as the mint-lease `next_lease` precedent already requires (G-02, AFR-R-23). A restored backup is not detectable without an external authority; out of contract, like a witness rollback (AFR-R-19) |
| 4 | **WAL formats with LSN gaps** | Every record carries its LSN explicitly. Readers, reconciliation (R6.2 end offsets), the term history H and "caught up" tests compare LSN values, never counts. A skip record marks each gap, so readers can tell a gap from a missing tail. Format items for AFR-D-06 |
| 5 | **Acknowledged writes preserved** | Reservation only skips; it never removes content. G-14 applies unchanged |
| 6 | **G-55 and G-48** | G-55 holds: the term is unchanged. G-48 improves: the node resumes without an operator unless G-14(a) or point 3 refuses |
| 7 | **Later joining a replicated group** | Standalone nodes never mint terms, so all of them share the same initial term, and term history alone cannot tell two nodes' logs apart. A group created from node X carries X's stream identity, term and `R_hi` in the record; members reseed from X (R6.4). The reconciliation handshake carries the stream and group identity (as P4 does) and refuses on a mismatch, so a node with its own data never "positions" against an unrelated log; reseeding such a node, which discards its data, is an explicit operator action. Moving the mark into the record is a handoff to test (KAFKA-15552) |

### 4.4 Still open

- the standalone silently-lost case (points 2 and 3), which waits on D-02;
- the 4.2 dependence on D-02 (silent loss on the restarted primary);
- standby read leases are not in either model, so rule 2's standby-lease clause is argued only (the R9 model is a
  separate thread); M2 and M2b found no case for rule 2 at all (2.5);
- the M4 finding on operator promotion inside S (2.5): the proposed correction is checked only within the bounds and
  the untimed-lease abstraction of `docs/AFR_001_PROMOTION_DURING_RECON.md`.

## 5. Owner direction and what remains

cody's direction of 2026-11-17 (`docs/handoff/PR149_150_Owner_Direction.md`) answered the questions of the first
draft: R2′, the re-promotion of 4.2 and standalone reservation are preferred candidates; P1-P7 and S1-S5 are the
proposed replacement for E1-E11; the bounded runs of 2.4 are authorized (at most eight runs of two hours, no R640).
The model results are in 2.5. What remains: review of those results before any R2 freeze, the M4 finding (2.5),
the D-02 dependencies (4.2, 4.3), and the revisit of `m = 2`'s failover cost. The default is not weakened.

## 6. Independent QA

An independent reviewer (Opus) was told to break this document and the decision-record edits, without editing or
running anything. It reported 14 findings (no blocker against R2′ itself; 8 major, 6 minor). All were addressed:

| # | Finding | Change |
|---|---|---|
| 1 | Resuming in the old term inside a degraded group breaks R3, R9 and log matching (`storage.cpp` ships before the local write) | Degraded groups now re-promote p to `t+1` (4.2); reservation is for standalone nodes only (4.3). Needs cody's ruling |
| 2 | Reservation kept only in degraded mode reuses LSNs after a shrink | Moot for groups after #1 |
| 3 | "Fails only through a reseed" and "the one known failure" overclaimed | Reworded (2.3 point 1) |
| 4 | M2 targets a check the models lack; its expected violation is doubtful | Rule 2 is a model change; M2 marked uncertain; M2b added for leases (2.4) |
| 5 | The models cannot test R2′ as they stand; runtime estimates were unsupported | Required model changes listed; estimates replaced by a capped budget (2.4) |
| 6 | `m = 2` revisit and the three gates missing from the decision records | Added to SG 14.3 and the D-01/D-03 rows |
| 7 | G-12 still allowed OK-with-achieved-class under `m = 2` | Limited to `m = 1` or async; LOCAL_DURABLE-only requests left open (AFR-D-14) |
| 8 | Standalone nodes share a term, so a join can "position" against an unrelated log | Stream and group identity in the handshake, refuse on mismatch (4.3 point 7) |
| 9 | Records said more than the owner approved (CD header, D-01 conditions, D-03 options column, unmarked G-11/G-12) | Narrowed; options column restored; [ACCEPTED] markers on the owner sentences only |
| 10 | P5 too strong; P6/P7 unverifiable by witnesses; P7 is not E7; P2 omitted "or removed" | Fixed (section 3) |
| 11 | Availability overstated | Fixed (2.3 points 2, 6; assessment) |
| 12 | Standalone mark fallback rules conflicted | Fallback refuses until D-02 (4.3 point 3) |
| 13 | Prior-art wording: KAFKA-15552 possibly inverted; several quotes unconfirmed | Reworded neutrally and labelled [EXT] (2.1, 4.1) |
| 14 | Broken references (SG 14.3 after section 15, placeholder, SA status of R1/R6/R7) | Fixed |

The reviewer could not break, by hand: a `t+2` promotion before survivors are positioned (blocked by READY);
operator promotion of an unpositioned survivor; survivor crash mid-truncation; grow or shrink racing the promotion;
old-primary lease overlap; pairs; HistoryUnique under R2′.

**Re-review (2026-11-17).** A second independent reviewer (Opus) checked the corrected text. Of the 14 findings
above, 8 were resolved and 6 partly. It found 4 major and 10 minor new issues. Fixed in this text: rule 1 with the
self-promotion; standby leases only for positioned members (rule 2); the G-11 wording (HA_DURABLE copies must be in
S); the stop rule; stale owner-decision text; G-48/G-55 wording; the torn-mark exemption; the [FACT] labels; rule 5's
reference; the SG D-02 row against the 2026-11-17 direction. **Unresolved:** 4.2 depends on D-02 (a silently lost
flush on the restarted primary lets reconciliation delete the last copy); M1 may pass, because the earlier trace's
reseeding node is the old primary (left to the runs). The model runs are reported in a follow-up PR.

## 7. Sources

- PacificA: https://www.microsoft.com/en-us/research/wp-content/uploads/2008/02/tr-2008-25.pdf
- Kafka controller: `metadata/src/main/java/org/apache/kafka/controller/ReplicationControlManager.java`,
  `PartitionChangeBuilder.java`; `core/src/main/scala/kafka/cluster/Partition.scala` (apache/kafka trunk)
- KIP-101: https://cwiki.apache.org/confluence/display/KAFKA/KIP-101+-+Alter+Replication+Protocol+to+use+Leader+Epoch+rather+than+High+Watermark+for+Truncation
- KIP-279: https://cwiki.apache.org/confluence/display/KAFKA/KIP-279%3A+Fix+log+divergence+between+leader+and+follower+after+fast+leader+fail+over
- KIP-320: https://cwiki.apache.org/confluence/display/KAFKA/KIP-320%3A+Allow+fetchers+to+detect+and+handle+log+truncation
- KIP-966: https://cwiki.apache.org/confluence/display/KAFKA/KIP-966%3A+Eligible+Leader+Replicas
- KIP-730: https://cwiki.apache.org/confluence/spaces/KAFKA/pages/177049344/KIP-730+Producer+ID+generation+in+KRaft+mode
- KAFKA-15552: https://issues.apache.org/jira/browse/KAFKA-15552
- Raft: https://web.stanford.edu/~ouster/cgi-bin/papers/OngaroPhD.pdf, https://raft.github.io/raft.pdf
- Chain replication: https://usenix.org/legacy/events/osdi04/tech/full_papers/renesse/renesse_html/index.html
- Vertical Paxos: https://www.microsoft.com/en-us/research/uploads/prod/2009/08/Vertical-Paxos-and-Primary-Backup-Replication-.pdf
- FoundationDB: https://www.foundationdb.org/files/fdb-paper.pdf
- Aurora: https://www.cs.purdue.edu/homes/bb/cs542-20Spr/readings/impl/sigmod-18-amazon-aurora-avoiding-consensus.pdf
- Jepsen Kafka: https://aphyr.com/posts/293-call-me-maybe-kafka; Jepsen Redpanda: https://jepsen.io/analyses/redpanda-21.10.1
- PostgreSQL: `src/backend/commands/sequence.c`; https://www.postgresql.org/docs/current/functions-sequence.html;
  https://www.postgresql.org/docs/current/app-pgrewind.html;
  https://www.postgresql.org/docs/current/continuous-archiving.html
- Snowflake: https://github.com/twitter-archive/snowflake (IdWorker.scala)
