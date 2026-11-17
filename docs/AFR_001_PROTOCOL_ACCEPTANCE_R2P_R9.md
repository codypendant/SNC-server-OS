# SNC-AFR-001: protocol-acceptance review of R2′ and R9 (evidence from PRs #151-154)

- **Status: PROPOSED.** This review classifies evidence and records obligations. It freezes nothing and accepts no
  owner decision. It authorizes no C++ or Rust work and no R640 run. AFR-D-02 stays on HOLD, and the m = 2 default
  is unchanged.
- **Three separate gates (owner ruling, `docs/handoff/PR148_Owner_Decisions.md`).** A freeze only accepts contract
  text. C++ conformance and Rust authorization are separate gates, and nothing here touches them.
- **Trigger.** Codex's review of PR #154 (`docs/handoff/PR154_Codex_Review.md`). Its recommendations:
  - accept R2′ as the preferred replication protocol, and advance it toward a conditional freeze, with the storage
    and consensus dependencies explicit;
  - keep R9-A to R9-E as the candidate freeze set, and R9-F separate;
  - start no new verification campaign. Instead, review which guarantees are checked and which are argued.
- **Scope.**
  - R2′, verified survivors: rules 1-5, P1-P7, S1-S5 and 4.2 (`docs/AFR_001_R2_AND_SINGLE_COPY.md`).
  - The reconciliation corrections R6.3′, S2′, P6′ and completion (`docs/AFR_001_PROMOTION_DURING_RECON.md` 2.1-2.2).
  - R9-A to R9-F (`docs/AFR_001_SAFETY_ANALYSIS.md`).
  - The guarantees these touch.

  The R2 and R9 rows of the earlier review (`docs/AFR_001_PROTOCOL_ACCEPTANCE_REVIEW.md`) are superseded here. Its
  other verdicts stand. Standalone LSN reservation (4.3) is not reviewed here.
- **Method.** No new modelling and no new runs. Every row cites merged evidence:
  - **R9**: `spec/tla/RESULTS_R9.md` (PR #151, `StandbyLease.tla`);
  - **R2**: `spec/tla/RESULTS.md` section 9 (PR #152, `Log.tla`, `Combined.tla`);
  - **PR**: `docs/AFR_001_PROMOTION_DURING_RECON.md` sections 3-6 (PR #153, `PromoteRecon.tla`);
  - **RB**: `docs/AFR_001_RECON_BLOCKERS.md` sections 3-5 (PR #154, `ReconBlockers.tla`).

## 0. Classes

| Class | Meaning |
|---|---|
| **CHECKED** | A completed, exhaustive bounded run passes with the rule. A run without that rule (alone, or the stated bundle) gives a counterexample to a safety property the guarantee needs |
| **CHECKED, sufficiency only** | A completed run passes with the rule, but no run shows it is needed. It may be redundant at these bounds, removed only together with other conditions, or needed only for a property stronger than the guarantee |
| **ARGUED** | No run covers it, and an argument [INF] or prior art [EXT] supports it. Accepting it means accepting the argument |
| **TEXT ONLY** | Written in answer to the last review round (RB section 4), and neither checked nor reviewed again |

**Bounds.** Every run is bounded:
- three nodes, or four in C2;
- at most three terms, and one to three entry ids;
- at most one crash or restart, or up to two fences (L6s).

No run checks liveness. No run combines timed leases with R2′. "Checked" never means proven.

## 1. Summary

- **R2′ is supported for advancing toward a conditional freeze, with exclusions.**
  - Each of these is CHECKED:
    - rules 3 and 4;
    - S2′ quiescence, and durable completion rather than cancellation in memory;
    - the reply binding, and the R6.4 re-check at intent write;
    - the pending-intent block, as a whole;
    - the new term for self re-promotion.
  - CHECKED, sufficiency only: rule 1, rule 2, the reply's three-part term check, and each clause of the block.
  - Rule 5, P6′ and the serialized step of R6.3′ item 2 are ARGUED. So is the completion procedure, at the level of
    its requirements.
  - Text added after the last review is TEXT ONLY: the binding's scope, the answering side, and the completion
    end marker.
  - Section 5 lists what a conditional freeze must exclude or condition.
- **R9-A to R9-E are supported as the candidate freeze set.**
  - CHECKED: A for members of A, C, the caught-up part of B, the rule that the primary's wait covers the whole lease,
    and the rule that a restarted primary does not resume.
  - CHECKED only with outside-S promotion, and against a weaker property: D.
  - ARGUED: the `(1+ρ)` and cap formulas, and A for replicas still catching up.
  - All timed R9 evidence uses today's R2. With R2′, leases are checked untimed only.
- **R9-F is TEXT ONLY** and stays outside the set until it has a model (O1).

## 2. Rules

| Rule | Class | Evidence | Limits |
|---|---|---|---|
| **R2′ rule 1** S′ = S \ {old primary}, or {p} for a sole member's self re-promotion | CHECKED, sufficiency only | V1-V3 PASS (R2); X1, X2 (PR); C1-C3 (RB) | Today's R2 (S′ = {j}) also passes. Rule 1 is an availability choice, not a safety rule |
| **Rule 2** a survivor counts, and is leased, only once positioned | CHECKED, sufficiency only | Acknowledgements: M2 and M2b find nothing (R2). Lease: MP violates only `LeasedPrefix`, which is stronger than G-30 (PR) | MP was not rerun after `6307554`; it was checked by replay (PR 5, deviation 3). The lease clause is kept whatever R9-F decides |
| **Rule 3** leave S before reseeding | CHECKED | M1 (R2) | MaxIds 2, one crash |
| **Rule 4** READY only after the survivors are positioned | CHECKED | M4 (R2). MQa: READY alone covers automatic promotion (PR) | `FixReadyPos` removes four conditions together |
| **Rule 5** shrink a survivor that never positions | ARGUED | Only as the existing `Shrink` action | — |
| **P1-P5, P7** | As the earlier review's R2 and R7 rows, with S′ | V3 for P2 and P5 (R2); U_noBootRead for P7 (earlier review) | P5 relies on I4, which a silent loss breaks (O4) |
| **P6** incomplete flag, recovery barrier, G-14(a) attestation; **P6′** not reseeding | ARGUED | Every run keeps `~Busy(j)` on `Promote` (PR 5) | — |
| **S1, S3, S4** persist and wait; placement re-seal (MS-3) and expiry of older source leases; mint lease | As the earlier review (R3, R4 rows; G-37 ARGUED) | No run in #151-154 | — |
| **S2′** complete any pending intent before serving | CHECKED | MQ (operator promotion inside S), MD (cancellation in memory is lost in a crash) (PR) | Needed only when READY is waived (MQa) |
| **S5** acknowledge only once A is positioned | as rule 2 | — | — |
| **R6.3′ item 1** reply binding | CHECKED | C1 PASS and MB (RB). C1: MaxTerm 2, MaxIds 1, one crash, two replies | Modelled as a log snapshot, not an incarnation and request number. The equivalence is [INF] (RB 2.1); no reviewer broke it |
| — term, not-primary and not-positioned checks | CHECKED as a bundle; each alone CHECKED, sufficiency only | MR removes all three together (PR) | — |
| — R6.4 re-check at intent write | CHECKED | X1's first run violated `StateMatchesLog`, and X1 passed after the one-predicate fix (PR) | Not preregistered as a mutation. The rule that an in-progress checkpoint counts is TEXT ONLY |
| — group and stream identity; incarnation persisted before the first request; the answering primary stops answering when it loses its tail; its positioned record ends at `k`'s restart | TEXT ONLY | RB 4, findings 9 and 20 | — |
| **R6.3′ item 2** one serialized compare-and-write step, one metadata store | ARGUED | `RecvRep` is atomic in both models | The store's layout is open (O2), and it can reopen this item (section 5) |
| **R6.3′ item 3** a pending intent blocks the node in every term | The block as a whole: CHECKED. Each clause: CHECKED, sufficiency only | C2 PASS, MI, PX (RB). `FixIntentBlock` removes every clause together | C2 has L = 0 only, no crash, no checkpoint, and one reply at a time. No lane combines cases 1 and 2. That the dangerous continuation is reached under the fix is not shown (O6). RB section 5 said "MI shows the positioning block necessary"; that overstates it |
| **Completion** requirements (2.2 items 1, 4, 5: logical end first, idempotent, cleared last, end marker kept) | Items 1 and 4: ARGUED. Item 5: TEXT ONLY | `ApplyIntent` and `Crash` are atomic in every model | Items 2-3 (segments, spare files, rebuild) are the C++ realization, under O2 |
| **4.2** a restarted sole primary serves only through a new witness-approved term | CHECKED | C3 PASS, MNH, PS, PR (RB). The barrier for the self re-promoted term: MB (PR) | MaxTerm 2. LSN reuse is only at LSN 1. MB is untimed, so the barrier's length is ARGUED (PR 3). **A silent loss of p's newest flush makes reconciliation delete the last copy of an acknowledged write: UNRESOLVED until D-02** (R2C 4.2) |
| **R9-A** acknowledge only applied entries | Members of A: CHECKED. Replicas catching up to join A: ARGUED | M_ackLeased, M_ackDurable (R9). Only members of A send `MAck` | Limits of R9 section 7 apply to every R9 row: today's R2, MaxTerm ≤ 2, no `OpInside`, atomic grants, no reseed or checkpoint, L1 NOT COMPLETED |
| **R9-B** a lease goes only to a member of A, at p's term, positioned, with no pending intent, applied ≥ max(`inh_p`, `ack_p`) | Caught up: CHECKED. Positioned: as rule 2. No pending intent: as item 3 | M_leaseSync, _pack, _inh (R9); MP (PR) | `ack_p` must be defined in B itself (section 5). M_leaseSync_pos finds nothing under today's R2 |
| **R9-C** no removal from A while the lease may be valid; every acknowledgement needs all of A | CHECKED | M_dropWait, M_ackA (R9); MW (PR) | The mutations ran in lane L1x, which never completed unmutated. Each trace has no restart, no fence and one entry, so it is also a behaviour of L1b and L1c, which passed. "Does not degrade" is redundant (M_shrinkWait) |
| **R9-D** stop serving lease reads on a newer term | Outside-S promotion: CHECKED against `StandbyFreshOp`. In contract: CHECKED, sufficiency only | M_stopOnLearn_L5, _L6 (R9); MS not shown (PR) | `StandbyFreshOp` is weaker than G-30. The lost write was acknowledged after the outside-S promotion, which G-35 already allows to be lost. "Never truncate a validly leased standby" also passes L5s, so D is one sufficient rule (R9 5) |
| **R9-E** timing | The wait covers the whole lease: CHECKED (M_rho). A restarted primary does not resume: CHECKED (M_restartStop). The `d(1+ρ)` and `(1-ρ)/(1+ρ)` formulas, and a lease not surviving the standby's restart: ARGUED | R9 | Drift is folded into one lease length; there is no rate model. G-46's clock assumptions apply |
| **R9-F** lease reads expose only changes acknowledged at HA_DURABLE | TEXT ONLY | — | C++: ABSENT (G-30) |

## 3. Guarantees

C++ statuses are unchanged by this review (`docs/SNC_GUARANTEES.md`).

| Guarantee | Class | Evidence and gap |
|---|---|---|
| **I4** acknowledged writes kept under R2′ (Claim D) | CHECKED | `AckedPreserved` passes in V1-V3, X1-X2 and C1-C3. The rules it needs are shown by M1, M4, MQ, MD, MR (PR) and MB, MI (RB). Outside-S promotion stays out (G-35). Silent loss: O4 |
| **G-15** `(term, LSN)` never names two contents | CHECKED, narrow | `HistoryUnique` passes in V1, V2, X2 and C3, and MNH makes it non-vacuous. Runs with MaxIds 1 check it only trivially. The completion end marker is TEXT ONLY. Torn tails and D-02: UNRESOLVED |
| **G-30** standby read under a valid lease | A-E: as R9 above. F: TEXT ONLY | Timed runs use today's R2; R2′ runs have untimed leases. **Without F, a lease read may return an unacknowledged change that reconciliation later truncates.** Until O1, standby reads stay outside the Rust scope, or are labelled weaker under G-29 |
| **G-29** read guarantees by read type | F: TEXT ONLY | Under R9-F, a multi-object standby read is not a snapshot |
| **G-35** promotion eligibility | Inside S, by operator: CHECKED with S2′ (MQ). Outside S: loss accepted by contract | P1-P7, S1-S5 as in section 2 |
| **G-11, G-12** m = 2 default; writes refused while \|S\| < m | ARGUED | m = 2 admission is not modelled. Under R2′ a member of S′ may hold a pending intent. It counts toward \|S′\|, but it is never positioned, so no acknowledgement completes until it is positioned or shrunk (rule 5). Writes wait; they never weaken. The intent has no time bound yet (O2). RB section 4 said such a node "is outside S"; that was wrong. In a two-member group, R2′ gives no write availability after failover (\|S′\| = 1 < 2) |
| **G-48** availability exceptions | ARGUED | Self re-promotion is reachable (PS, P1). Not modelled: its costs, crash loops, witness-record damage, and the crash-before-READY operator exception (PR 3) |
| **G-55** single copy | as 4.2 | 4.3 is not reviewed here |

## 4. Remaining obligations

This review authorizes none of these.

| # | Obligation | Gate | Blocks |
|---|---|---|---|
| O1 | Model R9-F, with separate applied, acknowledged and `c_m` positions, tombstones and a fixed read target | a later narrow model step | adding R9-F to the R9 set. A failure may reopen R9-A, which forces versioned visibility |
| O2 | D-06 storage format: per-record terms; the metadata store of R6.3′ item 2, per node or per shard; the completion realization (2.2 items 2-3) and its end marker; the intent's checksum; checkpoint scope per stream; a time bound on a pending intent; the request binding and a term on every frame; for R9-F, the `ack_p` field, per-key LSNs, tombstones, a safe LSN for objects loaded from a checkpoint, and an index that serves only covered entries | AFR-D-06 | see section 5: a per-shard store reopens text |
| O3 | A crash-consistency test of completion | C++ conformance or Rust, each its own gate | — |
| O4 | D-02 end-of-log rule: torn tails, silent loss, refusal when the log cannot be trusted. The watermark is lowered in the same step as a truncation, and framing tells a half-done truncation from damage | AFR-D-02 (HOLD), X-2 (R640, not authorized) | P5's silent-loss clause; 4.2 |
| O5 | Consensus service: A7 and A9; a witness record carrying S, mode, m and self re-promotion; a named refusal for witness-record damage (G-14(a)); a bound on term growth in a crash loop (G-48) | AFR-D-13 | Rust design of the consensus service and of 4.2 |
| O6 | C2's dangerous continuation: one probe that shows it is reached under the fix, or accept C2 as narrow | optional; no campaign | confidence in R6.3′ item 3 beyond C2's bounds |
| O7 | No run combines a restart, fencing, operator promotion, timed standby leases and R2′ | recorded gap | — |
| O8 | Costs: failover including the completion rebuild, and R9-A's apply on the acknowledgement path (X-1); lease costs (X-3) | experiments, after their own go | — |

## 5. What a conditional freeze would and would not cover

The owner decides. Nothing is recorded here.

**R2′ text that could be frozen:**
- rules 1-5;
- P1-P4, P6 with P6′, and P7;
- S1-S5 with S2′;
- R6.3′ items 1-3, conditional on one metadata store per node;
- completion requirements, 2.2 items 1, 4 and 5.

**Excluded until their obligation closes:**
- 4.2 and P5's silent-loss clause (O4). Any fix there is new protocol text.
- R6.3′ items 2-3, if D-06 picks a store per shard. R4's term write is then not atomic across shards, and "blocks
  the node" and "takes no checkpoint" become per-stream rules. That reopens items 2-3, R4, P7 and S1 (O2).
- The TEXT ONLY rows, until another review.

**R9 text that could be frozen:** R9-A to R9-E. This requires `ack_p` to be defined in R9-B itself (HA_DURABLE only;
never set from `inh_p`), with R9-F citing it. Its conditions:
- G-46's clock assumptions;
- the ARGUED formulas of R9-E;
- the independent review the owner required before any R9 freeze (`docs/handoff/PR148_Owner_Decisions.md`);
- R9-F and standby reads kept out of scope until O1.

**What it would mean, and what it cannot do:**
- O5 adds guarantee text, a G-14(a) refusal and a G-48 bound, but changes no rule above.
- The ARGUED rows are accepted as arguments, in the sense of the earlier review's section 0.
- A later finding that contradicts frozen text reopens it through a recorded revision.
- No unconditional freeze is reachable without O4, and O4 needs X-2 on the R640, which is not authorized.
- The owner's request to revisit m = 2's failover cost after the R2 comparison is not addressed here.

## 6. Independent review (2026-11-17)

Three reviewers each tried to break the first draft. One worked on the CHECKED claims, one on the ARGUED rows and
omissions, and one on downstream implications. They found 2 blockers and about 20 major and minor issues. All are
applied above. The main corrections:
- The first draft said O2, O4 and O5 change only the design, never the text. That was false for all three.
  Section 5 now excludes or conditions the affected text.
- The term check and the pending-intent positioning clause were downgraded to bundle evidence. Each mutation
  removed several conditions.
- R9-A for replicas still catching up, the `(1+ρ)` formula and the R9-D property were downgraded or qualified.
- `ack_p` must be defined in R9-B, not only in R9-F.
- Without R9-F, standby lease reads can show writes that later vanish. G-30's row now says so.
- The draft claimed "a node with a pending intent is outside S", copied from RB section 4. It is wrong under R2′
  and is corrected in G-11/G-12 above.
- Rows were added for P6, S1, S3, S4, G-29 and G-55. The silent-loss hazard of 4.2 is now named, and RESULTS_R9's
  limits are carried over.

**UNRESOLVED:**
- 4.2 under silent loss (O4).
- C2's dangerous continuation (O6).
- How long writes can wait behind a pending intent, which is a liveness question no run checks.
- Whether a "stream" maps to one per-core shard or to one replication group (O2).
