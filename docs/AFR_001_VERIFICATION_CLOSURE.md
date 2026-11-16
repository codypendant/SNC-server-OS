# SNC-AFR-001: formal-verification closure

- **Status: PROPOSED / PARTIAL VERIFICATION.** Bounded model-checking evidence, not a proof. Codex accepted it as a
  documentation-and-model checkpoint only (`docs/handoff/PR144_Closure_Codex_Review.md`); section 6 lists the proof
  obligations that remain open. This phase follows Codex's review of PR #142 Revision 2
  (`docs/handoff/PR142_Revision2_Codex_Review.md`). That review carried forward three open items. Each section below
  takes one, says what the closure phase did, and gives the result as bounded model-checking evidence.
- **Nothing here authorizes implementation.** AFR-D-01, AFR-D-02 and AFR-D-03 stay unaccepted.
- **Method.** Smaller focused models and sound abstractions rather than more CPU or memory, as Codex asked.
  - Every run has a wall-clock limit. A run that hit it is **NOT COMPLETED**, and it proves nothing about the
    states it did not reach.
  - Every fix has a mutation run, so the model must show that it catches the defect.
  - Every run, its bounds and its trace are in `spec/tla/RESULTS.md`.

## 0. Disclosure: a bug in the merged log model

`spec/tla/Log.tla` as merged in PR #142 (`4d12cbb`) had a defect, found during this phase.
- **The defect.** `Learn(n)` assigned `inh'` and `pack'` but also listed `inh` and `pack` in its `UNCHANGED`
  clause. TLC therefore allowed `Learn` only when the new values equalled the old ones. A node with a non-empty log
  could never learn a newer term, and an old primary that had acknowledged an entry could never step down.
- **The effect.** Every Log result in the merged `RESULTS.md` covered fewer behaviours than it claimed. That
  includes the exhaustive pass at 10,180,593 states and the "M1 is unreachable" observation. The fix switches still
  reproduced their failures, because those traces do not need the disabled steps.
- **The fix.** `inh` and `pack` are removed from that `UNCHANGED` clause. A small checker (kept in scratch, not
  committed) now looks for any action that both assigns and freezes a variable, and finds none in the three models.
- **Rerun.** The fixed Log model at the old bounds (MaxTerm 3, MaxIds 3) did **NOT COMPLETE** in 2400 s
  (62,464,105 distinct states, no violation in what was explored). Smaller bounds complete: MaxIds 2 at MaxTerm 3
  (5,134,137 states) and MaxTerm 2 at MaxIds 3 (15,904,223 states). Every Log fix switch was rerun on the fixed
  model and reproduces its failure.

## 1. Combined authority and durability model (carry-forward 3)

`spec/tla/Combined.tla` is new. It checks authority and durability together where they meet: promotion, the learn
barrier, member leases, degraded witness leases and restarts interleave with shipping, acknowledgement and
membership changes (add to A, grow, shrink, drop).

**Invariants:** `OneAuthority` (Claim L), `AckedPreserved` (I4), `EntryUnique` (I1), `LogMatching` (I2) and
`AckedAtPrimary` (every acknowledged entry is in the log of a primary that has learned the current term).

**Abstractions.** Each one either loses no behaviour that matters to these invariants, or is checked in detail by
another model:
- **Time:** countdown timers. Each timer holds its remaining time (0 to D) and `Tick` decrements all of them. Two
  states that differ only by a shift of absolute time behave the same, so this is an exact quotient of the
  Revision 2 discrete-time model [INF]. It removes the old `MaxTime` bound. Time is still discrete with zero drift:
  drift within the bound is folded into D by the argument in the analysis, not checked.
- **Witness Paxos:** a promotion is chosen atomically. Degraded authority is one timer, granted only against the
  chosen record (W1). Per-witness acceptance is checked in `Authority.tla`, which matches this by quorum
  intersection [INF]. `Authority.tla` itself has one proposer that never fails, and only one promotion in flight
  at a time.
- **Reconciliation:** one atomic KIP-279 step against the current primary. Durable intents, checkpoints and
  reseeding are member-local, and `Log.tla` checks them.
- **Operator promotion and standby leases** are left out. `Authority.tla` checks them.

**Results (3 nodes, D = 2, MaxIds 2, one restart):**

| Run | Bounds | Result | Distinct states | Depth |
|---|---|---|---|---|
| C_term2 | MaxTerm 2 | **PASS**, exhaustive | 3,235,335 | 35 |
| C_term3 | MaxTerm 3 | **PASS**, exhaustive | 15,151,183 | 45 |

**Mutations.** Each removes one rule, and TLC must find a violation.

| Run | Rule removed | Result |
|---|---|---|
| C_noBarrier | R3 `learned + W` barrier | violated, depth 4: `Promote(n2), Learn(n2), WGrant(n2)`. The new primary serves while the old one's lease from n2 is still live |
| C_growPack | "caught up" uses p's own acks only (the Revision 2 QA blocker) | violated, depth 11, ending `Ack(n1,1), Learn(n2), Recon(n3,n2), AddA(n2,n3), Grow(n2,n3)` |
| C3_noDropTerm | R1.2's "read still at p's term" (T2) | violated at MaxTerm 3, depth 13, ending `Promote(n3), Drop(n1,n2)`. Not found at MaxTerm 2 (exhaustive, 3,449,907 states), because T2 needs a third term |
| C3_noAckAuth | the authority check on `Ack` | **not violated**, exhaustive at MaxTerm 3 (20,989,156 states) |

**Finding from C3_noAckAuth [INF, bounded].** Within this model, durability does not depend on the lease. Its scope
is narrow: the model has no operator promotion, so every promotion is from S and a stale primary always has the
new primary in A. A stale primary with `A = {p}` arises only after an outside-S promotion from degraded mode (5b),
whose write loss is already excluded from Claim D. Acknowledgement under operator promotion inside S without READY
is not checked. An acknowledgement is safe
because A contains S (R1) and every member in A must be at p's term (R4). A stale primary cannot collect an
acknowledgement from a member that has learned a newer term. This matches Kafka, where a stale leader's ISR change
is rejected by leader epoch (KIP-497) and it cannot commit alone with `acks=all`. The lease protects reads
(Claim L), not acknowledgements. No rule changes: the authority check on `Ack` stays, because it costs nothing and
keeps acknowledged writes visible to linearizable reads. Reads themselves are not modelled.

## 2. Case 5c: forbidden (Codex's "resolve operator promotion case 5c")

**The case.** `S = {p, j}`. The operator promotes `k`, outside S. `j` is partitioned from the operator and the
witnesses but still reaches `p`, so `j` never learns the new term and keeps renewing `p`'s lease. `p` and `k` both
hold authority. No wait by `k` helps, because `j` keeps renewing.

**Prior art [EXT].** Every system checked forbids promoting outside the committed set as a safe operation, or
allows it only as an override documented as unsafe:

| System | Rule | Source |
|---|---|---|
| PacificA | reconfiguration requires "a new primary be a replica in the previous configuration" (§2.7) | MSR-TR-2008-25 |
| Kafka | unclean election: the new leader's log "becomes the source of truth even though it is not guaranteed to have every committed message"; the stale leader is fenced by leader epoch (KIP-497, KIP-320); ELR (KIP-966) widens the eligible set only to replicas proven to hold data up to the high watermark | Kafka design docs; KIP-497, KIP-320, KIP-966 |
| etcd | `--force-new-cluster` "may break the guarantees given by the consensus protocol!"; learners cannot lead | etcd(1) flag help; etcd learner docs |
| CockroachDB | loss-of-quorum recovery: "Stop all nodes in the cluster", "with no guarantees given whatsoever" | loss-of-quorum recovery RFC (2021-11-03) |
| Patroni | "A node that is not the leader or current synchronous standby is not allowed to promote itself automatically"; manual promotion "even if it results in transaction loss" | Patroni replication modes |
| Pacemaker | "Just because a node is unresponsive doesn't mean it has stopped accessing your data"; fencing is required in production | Pacemaker fencing docs |
| MongoDB | forced reconfig can cause "rollback of "majority" committed writes" | `rs.reconfig` docs |

None makes out-of-set promotion automatically safe. Two-leader safety always comes from a separate mechanism:
epoch checks, leases with a grace period, or STONITH.

**Resolution (simplest rule that closes it).** R7 in the safety analysis now says:
- outside-S promotion is allowed only when the record is degraded and READY (5b), or when **every member of S is
  confirmed fenced**, meaning a fencing action (for example power-off) has reported that each one has stopped. An
  operator's belief that a member is down is not a confirmation;
- anything else is not a protocol operation. If SNC offers it at all, it is an explicitly unsafe recovery override,
  labelled as able to break Claim L and lose acknowledged writes, outside the contract.

The rejected alternative (members grant only while they have confirmed the current term from a witness majority)
would close 5c inside the protocol, at the cost of witness traffic in normal mode. It stays unadopted and
unanalysed [HYP].

**Model evidence (`Authority.tla`):**
- `A_mut_5c` (MaxTerm 2) admits 5c and is violated at depth 10: `Propose(n3), WAccept(w1), WAccept(w2), Learn(n3), Tick,
  Tick, Grant(n2,n1), WGrant(w1,n3), WGrant(w2,n3)`;
- **QA finding: a fenced member can come back.** Fencing often reboots (Pacemaker's default action is a reboot), or
  an operator powers the node on. A returning member of S that never learned the new term renews the old primary's
  lease, and 5c reappears. R7 now adds a **boot read**: any member reads R from the witnesses and adopts its term
  before it grants. The model has `Unfence` (a fenced node returning) and a `FixBootRead` switch;
- runs at MaxTerm 3 with outside-S operator promotion only (inside-S operator promotion, degraded mode and standby
  leases off), where the gate is exercised:
  - `U_noBootRead` (one fence, one return, no boot read) is violated at depth 16, ending `Fence(n2), Propose(n3),
    ..., Learn(n3), Tick, Tick, WGrant(w1,n3), WGrant(w2,n3), Unfence(n2), Grant(n2,n1)`;
  - `U_gate_f1` (one fence, one return, boot read) **passes** exhaustively (30,970,751 states, depth 35);
  - `U_gate_f2_norestart` (two fences, no return) **passes** exhaustively (7,445,806 states, depth 35);
  - `U_gate_f2` (two fences and one return) is **NOT COMPLETED** after 1500 s (45,803,117 states, no violation in
    what was explored);
- `A_fence_op` above does not exercise this gate: at MaxTerm 2 the non-degraded branch needs both n1 and n2
  fenced, and the degraded branch is always READY;
- a fencing action that reports success for a node that is still running is outside the contract, like drift
  beyond the bound.

## 3. R3's simplified rule (carry-forward 2)

`Authority.tla` now tracks all three R3 bounds (boot, last grant, learned + W) but serves on the simplified rule
alone (`R3Full = FALSE`: eligible once `learned + W` has passed). It checks a new invariant:

`R3Lemma: every primary's learned timer is at least its boot timer and its last-grant timer.`

If `R3Lemma` holds, then whenever the simplified rule allows serving, the full rule does too. So the two rules
are equivalent in every reachable state.

- `R3Lemma` and `Safety` hold in every completed Authority run below, all with the simplified rule.
- `R3Lemma` holds by construction: only `Learn` makes a node primary, it sets the learned timer to D (the largest
  value any timer takes), and a node grants only at its own term. So the lemma is a mechanical check of the hand
  argument, not independent evidence. It is still the right statement: by induction it makes the two rules agree on
  every reachable state.
- `A_mut_barrier_core` removes the `learned + W` barrier and is violated at depth 7: `Propose(n2),
  WAccept(w1), WAccept(w2), Learn(n2), WGrant(w1,n2), WGrant(w2,n2)`.
- Codex issue 1 is reproduced on the new model: the Revision 1 rule (full rule without the grant-after-restart
  bound, no barrier) is violated at depth 11 (`S_codex1b`). With the barrier kept, removing that bound gives no
  violation, exhaustively (`S_codex1`, 8,729,382 states). That is the simplified rule's claim: the barrier alone
  covers the restart case.
- With the barrier on, removing R2's READY rule gives no violation at these bounds (`S_ready_only`, 13,176,230
  states). READY stays, because R7 depends on it (`S_FixOp` is violated without it); this is an observation, not a
  proposal.

| Run | Scenario (all MaxTerm 2) | Result | Distinct states | Depth |
|---|---|---|---|---|
| A_all_norestart | every scenario, no restart, no fencing | **PASS**, exhaustive | 7,274,870 | 36 |
| A_restart_core | one restart, degraded mode on; operator promotion, fencing and standby leases off | **PASS**, exhaustive | 2,523,448 | 32 |
| A_fence_op | operator promotion inside and outside S, two fences, degraded mode; no restart, no standby leases | **PASS**, exhaustive, but it does not exercise the 5c gate (see section 2) | 6,466,188 | 32 |

**Not completed.** Every scenario at once with one restart and two fences: **NOT COMPLETED** at MaxTerm 2
(2400 s, 111,102,249 states) and MaxTerm 3 (3000 s, 148,546,099 states). So did one restart with every scenario
(34,020,597 states, 1800 s) and two fences with every scenario (43,323,144 states, 1800 s). No violation in what
was explored, which is not a pass.

**What is not covered.** No completed run combines a restart with operator promotion or with standby leases, or
fencing with standby leases. Besides the `U_` gate runs in section 2, the only completed Authority runs at a
third term are `S_codex1` and `S_ready_only`. Each removes a guard, so it allows more behaviour than the protocol, and its pass carries over to the
protocol for its scenario (no operator promotion, no standby leases, and a restart only in `S_codex1`, which
serves on the full R3 rule; `R3Lemma` held there, so in every reachable state the two rules agree).

## 4. M1: the KIP-279 iteration (coordinator's request)

M1 needs a follower holding entries from terms the new primary never had.
- **Under R2** (the new S is `{j}`, and a member reconciles before it rejoins), the fixed model now shows the M1
  switch changes nothing reachable: at MaxIds 2, `L_M1_ids2` (KIP-101 single answer) and `L_ids2` (KIP-279) reach
  the same number of states (5,134,137). So under R2, R6.2's iteration is not load-bearing at these bounds [INF,
  bounded].
- **Under a relaxed R2** (`KeepS = TRUE`: promotion keeps the rest of S unreconciled, and may even promote an
  unreconciled member. That is looser than Kafka, which elects only ISR members; KIP-279's own setting is a clean
  election with a fast leader change),
  the model now distinguishes the fix. Checking the reconciliation invariants (I1, I2, I3):
  - KIP-101 (`L_keepS_kip101_recon`) is violated at depth 11: `PositionedPrefix` breaks after `ReconStep(n2,n1)`,
    which is M1's divergence;
  - KIP-279 (`L_keepS_kip279_recon`) **passes** exhaustively (28,625,801 states, depth 43).
- With `KeepS = TRUE` the full `Safety` also fails under KIP-279 (`L_keepS_kip279`), because a member still in S
  reseeds and discards an acknowledged entry. That is expected: R6.5 already forbids an incomplete member in S, and
  `KeepS` breaks R2 on purpose. It is not a protocol defect.

**So:** R6.2 is kept as defence in depth. The model shows it is needed under the one relaxation of R2 tested, and
not needed under R2 at MaxIds 2.

## 5. What this establishes, and what it does not

**Established, bounded:**
- authority and durability together pass exhaustively at MaxTerm 3 in the combined model, with mutations for the
  barrier, "caught up" and the drop check caught;
- R3's simplified rule is equivalent to the full rule in every completed Authority run;
- case 5c is forbidden by a gated R7 with a boot read, consistent with every system checked, and the model shows
  the gate works at MaxTerm 3 where it is exercised;
- the M1 fix is distinguished by the model.

**Not established:**
- no all-scenario Authority run with restarts and fences has completed;
- the combined model abstracts Paxos, operator promotion and standby leases, and checks them only in
  `Authority.tla`;
- reads are not modelled, so the claim that acknowledged writes are visible to linearizable reads is argued, not
  checked;
- bounds: three nodes, three witnesses, at most three terms, one restart, two entry ids in the combined model.
  Failures needing more of any of these are not covered;
- nothing here says the C++ implements the protocol, or what it costs (X-1 to X-3).

## 6. Remaining proof obligations

These stay open. None of them is closed by this document.
1. **The split models.** `Authority.tla`, `Log.tla` and `Combined.tla` each abstract parts the others check. That
   the abstractions preserve the safety properties is argued, not shown (Codex's next step).
2. **The larger Log configuration.** `Log.cfg` as committed (MaxTerm 3, MaxIds 3) did not finish after the `Learn`
   fix (62,464,105 states explored). Only smaller Log bounds completed.
3. **The full Authority configuration.** No run with every scenario, a restart and fencing together has finished,
   at MaxTerm 2 or 3. Nor has the 5c gate with two fences and a returning node.
4. **Combined `Auth` and the group record (Codex concern 3).** `Auth` in `Combined.tla` does not check the node's
   term against R. The relationship that the other actions enforce is not yet stated as an invariant or
   mutation-tested.
5. **Missing combinations.** A restart with operator promotion or standby leases; fencing with standby leases;
   acknowledgement under operator promotion.
6. **Reads.** Linearizable reads are not modelled, so read safety rests on Claim L and the argument.
7. **Bounds and time.** Three nodes, three witnesses, at most three terms, small entry and restart counts; discrete
   time with drift folded into D by argument.
8. **Implementation and cost.** Nothing shows the C++ implements the protocol, or what it costs (X-1 to X-3).
