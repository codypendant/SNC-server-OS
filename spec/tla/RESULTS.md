# TLC results for SNC-AFR-001 (closure phase): PROPOSED / PARTIAL VERIFICATION

These are the runs on the models in this directory, made during the closure phase
(`docs/AFR_001_VERIFICATION_CLOSURE.md`). Each run copies the models into a fresh directory and applies the stated
change to `Authority.cfg`, `Log.cfg` or `Combined.cfg`, nothing else. Authority runs go through `MC_Authority.tla`.

**Setup:**
- TLC2 Version 2026.11.15.002146 (rev: bc1b506), from `tla2tools.jar` v1.8.0, sha256 `7e53aa63...fb84f` (full
  value in `README.md`). Java 21.
- 1 to 4 workers, `-deadlock`, `-Xmx4g` or `-Xmx6g`, on a 4-core, 15 GB cloud container. Up to three runs shared
  the machine.
- Every run has a wall-clock limit. A run that hit it is marked **NOT COMPLETED**, with the states it had explored
  when it stopped. It proves nothing about the states it did not reach.

A passing run is a bounded result: no violation exists **within the stated bounds and the modelling limits** in
section 5. It is not a proof of the protocol.

**Disclosure.** The Log model merged in PR #142 had a defect: `Learn` assigned `inh` and `pack` and also froze them
in `UNCHANGED`, so a node with a non-empty log could never learn a newer term. The Revision 2 Log results
(section 6) therefore covered fewer behaviours than they claimed. It is fixed here, and every Log run below uses the
fixed model.

## 1. Fixed protocol

All Authority runs use the simplified R3 rule (`R3Full = FALSE`) and check `Safety` and `R3Lemma`.

| Run | Model | Bounds and scenario | Result | Distinct states | Depth |
|---|---|---|---|---|---|
| C_term3 | Combined | `Combined.cfg` as committed: MaxTerm 3, MaxIds 2, one restart, D 2 | **PASS**, exhaustive | 15,151,183 | 45 |
| C_term2 | Combined | as above, MaxTerm 2 | **PASS**, exhaustive | 3,235,335 | 35 |
| A_all_norestart | Authority | every scenario, MaxTerm 2, no restart, no fencing | **PASS**, exhaustive | 7,274,870 | 36 |
| A_restart_core | Authority | MaxTerm 2, one restart, degraded mode; no operator promotion, fencing or standby leases | **PASS**, exhaustive | 2,523,448 | 32 |
| A_fence_op | Authority | MaxTerm 2, operator promotion inside and outside S, two fences, degraded mode; no restart, no standby leases | **PASS**, exhaustive. At MaxTerm 2 it never exercises the 5c gate (QA): see the U_ runs | 6,466,188 | 32 |
| L_ids2 | Log | MaxTerm 3, MaxIds 2, one crash | **PASS**, exhaustive | 5,134,137 | 42 |
| L_term2 | Log | MaxTerm 2, MaxIds 3, one crash | **PASS**, exhaustive | 15,904,223 | 42 |
| U_gate_f1 | Authority | MaxTerm 3, outside-S operator promotion only, one fence, one return (`Unfence`), boot read | **PASS**, exhaustive | 30,970,751 | 35 |
| U_gate_f2_norestart | Authority | as U_gate_f1, two fences, no return | **PASS**, exhaustive | 7,445,806 | 35 |
| U_gate_f2 | Authority | as U_gate_f1, two fences and one return | **NOT COMPLETED** after 1500 s, no violation in what was explored | 45,803,117 (6,414,545 on queue) | 27 reached |
| A_term2_full | Authority | every scenario, MaxTerm 2, one restart, two fences | **NOT COMPLETED** after 2400 s, no violation in what was explored | 111,102,249 (32,409,743 on queue) | 22 reached |
| A_term3_full | Authority | `Authority.cfg` as committed (MaxTerm 3) | **NOT COMPLETED** after 3000 s, no violation in what was explored | 148,546,099 (65,282,717 on queue) | 20 reached |
| A_restart | Authority | every scenario, MaxTerm 2, one restart, no fencing | **NOT COMPLETED** after 1800 s, no violation in what was explored | 34,020,597 (5,869,078 on queue) | 24 reached |
| A_fence | Authority | every scenario, MaxTerm 2, two fences, no restart | **NOT COMPLETED** after 1800 s, no violation in what was explored | 43,323,144 (4,100,731 on queue) | 27 reached |
| L_fixed | Log | `Log.cfg` as committed (MaxTerm 3, MaxIds 3) | **NOT COMPLETED** after 2400 s, no violation in what was explored | 62,464,105 (9,240,075 on queue) | 30 reached |

## 2. Each fix switched off

TLC must find a counterexample. The traces are TLC's action sequences, as found. Depth here is the number of
states in the counterexample, including the initial state.

**Combined** (MaxTerm 2 unless stated):

| Run | Rule removed | Result | Counterexample |
|---|---|---|---|
| C_noBarrier | R3 `learned + W` barrier | violated, depth 4 | Promote(n2), Learn(n2), WGrant(n2) |
| C_growPack | "caught up" uses p's own acks only (Revision 2 QA blocker) | violated, depth 11 | NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n3), Ship(n1,n2), Ack(n1,1), Learn(n2), Recon(n3,n2), AddA(n2,n3), Grow(n2,n3) |
| C_noDropTerm | R1.2 "read still at p's term" (T2) | **no violation**, exhaustive, 3,449,907 states | T2 needs a third term |
| C3_noDropTerm | as above, MaxTerm 3 | violated, depth 13 | Promote(n2), Learn(n2), Tick, Tick, Ready(n2), Learn(n3), Recon(n3,n2), AddA(n2,n3), Grant(n3,n2), Grow(n2,n3), Promote(n3), Drop(n1,n2): n1 (term 1) drops n2 and holds authority with A = {n1} while n2 still holds term 2 |
| C_noAckAuth | authority check on `Ack` | **no violation**, exhaustive, 4,257,250 states | |
| C3_noAckAuth | as above, MaxTerm 3 | **no violation**, exhaustive, 20,989,156 states, depth 46 | Durability does not depend on the lease (closure doc section 1) |

**Authority** (MaxTerm 3 unless stated; the scenario switches named are off):

| Run | Rule removed | Scenario | Result | Counterexample |
|---|---|---|---|---|
| A_mut_barrier_core | barrier (simplified rule then has no wait) | MaxTerm 2, restart core | violated, depth 7 | Propose(n2), WAccept(w1), WAccept(w2), Learn(n2), WGrant(w1,n2), WGrant(w2,n2) |
| S_codex1b | Revision 1 R3 (full rule, no grant-after-restart bound, no barrier) | restart core | violated, depth 11 | Restart(n2), Tick, Tick, Propose(n2), WAccept(w1), WAccept(w2), Grant(n2,n1), Learn(n2), WGrant(w1,n2), WGrant(w2,n2): **Codex issue 1** |
| S_codex1_ctrl | as S_codex1b but with the grant-after-restart bound | restart core | **NOT COMPLETED** after 900 s, no violation in what was explored (20,648,400 states, 3,541,491 on queue) | control for S_codex1b; it does not show the grant-after-restart bound is sufficient on its own |
| S_codex1 | grant-after-restart bound removed, barrier kept | restart core | **no violation**, exhaustive, 8,729,382 states | the barrier alone covers it, as the simplified rule claims |
| S_B2 | READY rule and barrier | no restart, no fencing | violated, depth 9 | SGrant(n1,n2), Shrink(n1), Propose(n3), WAccept(w1), WAccept(w2), Learn(n3), WGrant(w1,n3), WGrant(w2,n3): the barrier failure (Codex issue 2) is found first |
| S_ready_only | READY rule only, barrier kept | no restart, no fencing, no operator, no standby | **no violation**, exhaustive, 13,176,230 states, depth 41 | see below |
| S_W1 | W1 | MaxTerm 2, no restart, no fencing | violated, depth 12 | Shrink(n1), Propose(n2), WAccept(w1), WAccept(w2), Learn(n2), Tick, Tick, WGrant(w1,n1), WGrant(w2,n1), WGrant(w1,n2), WGrant(w2,n2): **Codex issue 2** |
| S_M2 | R9 | MaxTerm 2, no restart, no fencing | violated, depth 12 | AddA(n1,n3), Grant(n3,n1), Propose(n2), WAccept(w1), WAccept(w2), Learn(n2), Tick, SGrant(n1,n3), Tick, WGrant(w1,n2), WGrant(w2,n2): M2 |
| S_FixOp | R7 READY / fenced condition | no restart, no fencing, no standby | violated, depth 14 | Tick, Propose(n2), WAccept(w1), WAccept(w2), Propose(n3), WAccept(w1), WAccept(w3), Learn(n3), Tick, Tick, Grant(n2,n1), WGrant(w1,n3), WGrant(w3,n3) |
| U_noBootRead | R7 boot read (found by the closure QA) | MaxTerm 3, outside-S operator promotion only, one fence, one return | violated, depth 16 | Tick, Propose(n2), WAccept(w1), WAccept(w2), Fence(n2), Propose(n3), WAccept(w1), WAccept(w2), Learn(n3), Tick, Tick, WGrant(w1,n3), WGrant(w2,n3), Unfence(n2), Grant(n2,n1): a fenced member returns at its old term and renews the old primary's lease |
| A_mut_5c | admit case 5c (`AllowOp5c = TRUE`) | MaxTerm 2, no restart | violated, depth 10 | Propose(n3), WAccept(w1), WAccept(w2), Learn(n3), Tick, Tick, Grant(n2,n1), WGrant(w1,n3), WGrant(w2,n3): **5c, now forbidden by R7** |

**R2's READY rule.** With the barrier on, removing READY from R2 gives no violation at these bounds (S_ready_only).
B2 (fast double promotion) is prevented by the barrier alone here. READY stays: R7 depends on it for outside-S
promotion from degraded mode (S_FixOp), and the bounds are small. This is an observation, not a proposal to drop
the rule.

**Log** (fixed model, `Log.cfg` bounds unless stated):

| Run | Fix removed | Result | Counterexample |
|---|---|---|---|
| L_B1 | R1.2 | violated, depth 5 | NewEntry(n1), WriteLocal(n1), Drop(n1,n2), Ack(n1,1): B1 |
| L_R60 | R6.0 | violated, depth 4 | NewEntry(n1), Ship(n1,n2), Crash(n1) |
| L_clear | R6.3 clear before accepting | violated, depth 16 | Promote(n2), Learn(n2), NewEntry(n1), WriteLocal(n1), Learn(n1), NewEntry(n2), WriteLocal(n2), ReconStep(n1,n2), ApplyIntent(n1), ReconStep(n1,n2), Ship(n2,n1), AddA(n2,n1), Grow(n2,n1), Ack(n2,1), ApplyIntent(n1) |
| L_ckintent | R6.3 no checkpoint while an intent is pending (T1) | violated, depth 9 | NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n1), Learn(n2), ReconStep(n1,n2), Checkpoint(n1), ApplyIntent(n1) |
| L_reseed | R6.4 | violated, depth 10 | NewEntry(n1), NewEntry(n1), WriteLocal(n1), Checkpoint(n1), Promote(n2), Learn(n1), Learn(n2), ReconStep(n1,n2), ApplyIntent(n1) |
| L_inc | R6.5 | violated, depth 11 | NewEntry(n1), NewEntry(n1), WriteLocal(n1), Checkpoint(n1), Promote(n2), Learn(n2), Learn(n1), ReconStep(n1,n2), RDiscardLog(n1), Crash(n1) |

## 3. M1 (KIP-279 iteration)

| Run | Configuration | Result |
|---|---|---|
| L_M1_ids2 | KIP-101 single answer, R2 kept, MaxIds 2 | **no violation**, exhaustive, 5,134,137 states: identical to L_ids2, so the switch changes nothing reachable under R2 |
| L_M1_kip101 | as above, MaxIds 3 | **NOT COMPLETED** after 1200 s (31,587,468 states, no violation) |
| L_keepS_kip101_recon | `KeepS = TRUE`, KIP-101, MaxIds 2, invariants I1-I3 | violated (`PositionedPrefix`), depth 11: NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n2), Promote(n1), NewEntry(n2), Learn(n1), WriteLocal(n2), Learn(n2), ReconStep(n2,n1): **M1** |
| L_keepS_kip279_recon | `KeepS = TRUE`, KIP-279, MaxIds 2, invariants I1-I3 | **PASS**, exhaustive, 28,625,801 states, depth 43 |
| L_keepS_kip101 | `KeepS = TRUE`, KIP-101, full `Safety`, MaxIds 3 | violated, depth 11 (as above) |
| L_keepS_kip279 | `KeepS = TRUE`, KIP-279, full `Safety`, MaxIds 3 | violated, depth 13, ending Ack(n2,1), ReconStep(n1,n2), RDiscardLog(n1): a member still in S reseeds and drops an acknowledged entry. Expected, because `KeepS` breaks R2 and R6.5 on purpose |

## 4. The simplified R3 rule

`R3Lemma` holds in every completed Authority run in section 1, all served on `learned + W` alone. When it holds,
the simplified rule allows serving only when the full rule does. S_codex1 shows that the barrier alone also covers
Codex issue 1, which the full rule needed the grant-after-restart bound for.

## 5. Modelling limits

- **Time and drift.** Countdown timers: a grant lasts D, a wait lasts D. Drift within the bound is folded into D;
  drift beyond it is not modelled.
- **Group record.** One register. In `Authority.tla`, witness Paxos is per-witness acceptance with a proposer that
  never fails. In `Combined.tla`, a promotion is chosen atomically and degraded authority is one W1 timer.
- **Combined.** Operator promotion and standby leases are left to `Authority.tla`; durable intents, checkpoints and
  reseeding to `Log.tla`. Reads are not modelled.
- **Term history H.** Loss of H is not modelled. The "H cannot answer" branch of R6.4 is not exercised.
- **Grants.** A member grants only while it is in the primary's A.
- **Bounds.** Three nodes and three witnesses; at most three terms (two in most completed Authority runs); one
  restart or crash; two fences; two or three entry ids. Failures that need more of any of these are not covered.
- **Not completed.** No Authority run with every scenario, a restart and fencing together has finished.
- **Third term.** The completed Authority runs at MaxTerm 3 are U_gate_f1 and U_gate_f2_norestart (section 1), and
  S_codex1 and S_ready_only (section 2). The last two each remove a guard, so their pass also covers the protocol in
  their scenario.
- **Out of contract.** Unclean promotion (outside S without READY or confirmed fencing) and a fencing action that
  reports success for a node still running.

## 6. Revision 2 runs (superseded)

The Revision 2 runs used the models at `2a561c1`: an absolute-time Authority model with `MaxTime`, and the Log model
with the `Learn` defect above. Their Authority switch results still stand as found on that model, and are repeated
on the current model in section 2. Their Log results are superseded by sections 1 to 3. The full Revision 2 table is
in git history (`git show 4d12cbb:spec/tla/RESULTS.md`).

## 7. Model composition (follow-up to the PR #144 review)

Runs for `docs/AFR_001_MODEL_COMPOSITION.md`, on the models in this directory (`Compete` and `StaleAuthBounded` in
`Authority.tla`; the fencing switches, `OpInside`, `OpOutsideDeg`, `StaleAuthBounded`, `AuthAtRecTerm` and
`OneTermBehind` in `Combined.tla`). 2 workers, `-Xmx3g`, two runs at a time. Depth is the number of states in the
counterexample.

`StaleAuthBounded` had three versions. "Draft": R's primary holds no `Auth`, is in the stale node's A, and its
lease to the stale node ends before its barrier. "First": R's primary holds no `Auth`, and `Rem` ends before the
barrier. "Current" (committed): R's primary holds no `Auth` **at R's term**, and `Rem` ends before the barrier.
Each is weaker than the one before when R's primary is in the stale node's A, so a Combined pass with an earlier
version carries over. The first version was refuted in `Authority.tla` (Q_A_ rows), which is why it changed.

**Combined** (MaxTerm 3 unless stated):

| Run | Change from `Combined.cfg` | Result |
|---|---|---|
| X_literal | MaxTerm 2, invariant `AuthAtRecTerm` | violated, depth 3: Tick, Promote(n2) (expected) |
| X_fenced_t2 | MaxTerm 2, draft | **PASS**, exhaustive, 3,235,335 states, depth 35 |
| X_fenced_t3 | draft | **PASS**, exhaustive, 15,151,183 states, depth 45 |
| Q_shipped | first | **PASS**, exhaustive, 15,151,183 states, depth 45 |
| C_opInside_t3 | `OpInside = TRUE`, first | **PASS**, exhaustive, 16,505,916 states, depth 44 |
| C_oneTerm_t3 | invariant `OneTermBehind` | **PASS**, exhaustive, 15,151,183 states, depth 46 |
| C_oneTerm_opInside_t3 | `OpInside = TRUE`, `OneTermBehind` only | violated, depth 8: Promote(n2), Learn(n2), Learn(n3), Recon(n3,n2), AddA(n2,n3), Grow(n2,n3), Promote(n3) |
| C_opOutsideDeg_auth | MaxTerm 2, `OpOutsideDeg = TRUE`, `OneAuthority` and first | **PASS**, exhaustive, 6,143,462 states, depth 36 |
| Q_opOut_t3_ids2 | `OpOutsideDeg = TRUE`, `OneAuthority` and first | **PASS**, exhaustive, 45,720,782 states, depth 44 |
| C_opOutsideDeg_safety | MaxTerm 2, `OpOutsideDeg = TRUE`, `Safety` | violated, depth 7: NewEntry(n1), WriteLocal(n1), Ship(n1,n2), Ack(n1,1), Shrink(n1,n2), Promote(n3): the 5b write loss Claim D excludes |
| M_grantTerm | MaxTerm 2, `FixGrantTerm = FALSE` | draft violated, depth 6 |
| S_grantTerm | as above, `Safety` only | violated, depth 7: Promote(n2), Learn(n2), Tick, Tick, WGrant(n2), Grant(n2,n1) |
| M_learnStep | MaxTerm 2, `FixLearnStep = FALSE` | `Safety` violated, depth 6: NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n1), Ack(n1,1) |
| M_AinS | MaxTerm 2, `FixAinS = FALSE` | draft violated, depth 4 |
| S_AinS | as above, `Safety` only | violated, depth 7: NewEntry(n1), WriteLocal(n1), Shrink(n1,n2), Drop(n1,n2), Grow(n1,n2), Ack(n1,1) |
| M_barrier | MaxTerm 2, `FixBarrier = FALSE` | `Safety` violated, depth 5: Tick, Promote(n2), Learn(n2), WGrant(n2) |
| M_w1Term_t3 | `FixW1Term = FALSE` | no violation, exhaustive, 15,151,183 states (identical to the protocol) |
| Q_w1Mut_opOut_ids0 | MaxIds 0, no restart, `OpOutsideDeg = TRUE`, `FixW1Term = FALSE`, `OneAuthority` | violated, depth 12: Tick, Shrink(n1,n2), Promote(n3), Learn(n3), Tick, Tick, Ready(n3), WGrant(n3), Promote(n1), WGrant(n1), Grant(n2,n1) |
| Q_opOut_ids0 | as above with `FixW1Term = TRUE`, `OneAuthority` and first | **PASS**, exhaustive, 46,823 states, depth 27 |

**Text mutations of term guards** (`Combined.tla` edited in a scratch copy; MaxTerm 2 runs check `Safety`; the
"3" runs use MaxTerm 3, MaxIds 1, no restart, `OpInside` and `OpOutsideDeg` on, and check `OneAuthority`,
`EntryUnique`, `LogMatching` and the current bound, because outside-S promotion breaks Claim D by contract):

| Run | Guard removed | Result |
|---|---|---|
| T3b_protocol | none | **PASS**, exhaustive, 1,514,402 states, depth 35 |
| T_current | `Current`'s `rec.term = T[p]` | no violation, 3,235,335 states (protocol's count) |
| T3b_current | as above | `OneAuthority` violated, depth 11: Shrink(n1,n2), Promote(n3), Learn(n3), Tick, WGrant(n3), Tick, Ready(n3), Promote(n1), Grant(n2,n1), Grow(n1,n2) |
| T_ship / T3_ship | `Ship`'s `T[m] = T[p]` | no violation at either bound (protocol's counts) |
| T_addA / T3_addA | `AddA`'s `T[c] = T[p]` | no violation at either bound (protocol's counts) |
| T_recon | `Recon`'s `T[k] = T[j]` | `Safety` violated, depth 10: Promote(n2), Learn(n2), Tick, Tick, WGrant(n2), NewEntry(n2), WriteLocal(n2), Ack(n2,1), Recon(n2,n1) |

**Authority** (`Safety`, `R3Lemma` and `StaleAuthBounded`; restart core = MaxTerm 2, one restart, degraded mode,
no operator promotion, fencing or standby leases):

| Run | Change | Invariant version | Result |
|---|---|---|---|
| B_stale_core | restart core | current | **PASS**, exhaustive, 1,441,064 states, depth 32 |
| B_compete_core | restart core, `Compete = TRUE` | current | **PASS**, exhaustive, 1,616,575 states, depth 33 |
| B_stale_gate | U_gate_f1 (MaxTerm 3, outside-S operator promotion, one fence, one return) | current | **PASS**, exhaustive, 27,814,539 states, depth 35 |
| B_compete_ops_t3 | MaxTerm 3, `Compete = TRUE`, operator promotion and degraded mode; no restart, fencing or standby | current | **PASS**, exhaustive, 18,009,677 states, depth 43 |
| Q_A_compete_ops_t3 | as B_compete_ops_t3 | first | violated, depth 10: Propose(n2), two accepts, Learn(n2), AddA(n2,n1), Grow(n2,n1), Propose(n1), two accepts: n1 is named in R again while it still holds its term-1 lease (safe; the version was wrong) |
| Q_A_stale_gate | as B_stale_gate | first | violated, depth 9: Tick, Propose(n2), two accepts, Fence(n2), Propose(n1), two accepts (same cause) |
| A_compete_ops | MaxTerm 2, `Compete = TRUE`, operator promotion and degraded mode | `T[n] < rec.term` form without the first clause | **PASS**, exhaustive, 640,719 states, depth 30 |
| A_compete_barrierMut | as A_compete_ops, `Barrier = FALSE` | same | violated, depth 7: Propose(n2), two accepts, Learn(n2), WGrant(w1,n2), WGrant(w2,n2) |
| A_compete_W1mut_ops | as A_compete_ops, `W1 = FALSE` | same | violated, depth 9: Shrink(n1), Propose(n2), two accepts, Learn(n2), Tick, WGrant(w1,n1), WGrant(w2,n1) |

State counts differ from section 1 for the same scenario (B_stale_core 1,441,064 against A_restart_core
2,523,448) because the old model also stored the set of accepting witnesses, which the new one derives from the
witnesses' records. The two encodings should have the same behaviours [INF; the independent QA checked the
enabling conditions by hand].

## 8. History-sensitive `(term, LSN)` uniqueness (follow-up to the PR #146 review)

Codex: "The current model's `EntryUnique` invariant does not establish uniqueness across the entire history. I would
require a history-sensitive test or invariant before freezing this clause" (`docs/handoff/PR146_Acceptance_Review_Codex_Review.md`).

**Change to `Log.tla`.** A history variable `hist` collects every `<<lsn, entry>>` that any node's durable log
(`dlog`) or in-memory log (`ilog`) has ever held. It is written only in `Next == Step /\ hist' = ...` and read only
by the new invariant, so the other variables have the same behaviours as before. `hist` depends on the path, so
state counts are not comparable with earlier sections. `HistoryUnique` says two entries ever held at one LSN with
the same term are the same entry, even if one of them no longer exists anywhere. It is now part of `Safety`.
- Checkpoints (`cks`) are copies of a `dlog` prefix, and reseeds write into `dlog`, so `hist` misses neither.
- Including `ilog` is stronger than "durable": a primary ships from `ilog` before its local write
  (`storage.cpp:651-652`), so in-memory content can reach a standby.
- **What it covers is narrower than G-15 (independent QA of PR #147).** The model has no torn tails, and a standby
  has no volatile tail (`Ship` writes straight to `dlog`), so the only reuse it can produce is a primary writing its
  old term again after a crash. `HistoryUnique` therefore checks R6.0 and nothing else of G-15's condition (2), with
  one crash in total.

| Run | Configuration | Result |
|---|---|---|
| H_fixed | `Log.cfg` with MaxIds 2 (L_ids2's bounds: MaxTerm 3, one crash), `Safety` with `HistoryUnique` | **PASS**, exhaustive, 5,226,372 states, depth 42 |
| H_fixed_t2 | `Log.cfg` with MaxTerm 2 (L_term2's bounds: MaxIds 3, one crash) | **PASS**, exhaustive, 15,996,310 states, depth 42 |
| H_one_hu | broken: one node (`Nodes = {n1}`, `P0 = Q0 = n1`), MaxIds 2, `FixR60 = FALSE`, `HistoryUnique` | violated, depth 4: NewEntry(n1) writes `(1, 1)` = id 0 in memory, Crash(n1) loses it, NewEntry(n1) writes `(1, 1)` = id 1 |
| H_one_eu | as H_one_hu, `EntryUnique` only | **no violation**, exhaustive, 57 states. With one node, the per-instant invariant cannot see the reuse |
| H_one_fixed | as H_one_hu with `FixR60 = TRUE`, `Safety` | **PASS**, exhaustive, 46 states. Weak control: after a crash the only node is no longer primary and nothing promotes it |
| H_r60 | broken: `Log.cfg` with MaxIds 2, `FixR60 = FALSE`, `HistoryUnique` | violated, depth 4 (same reuse as H_one_hu, on n1) |
| H_r60_eu | as H_r60, `EntryUnique` only | violated, depth 6: only once both copies coexist on two nodes |

**What this shows.** Within the completed Log bounds, no `(term, LSN)` is ever reused for different content under
the fixed protocol. Without R6.0, the reuse happens and `HistoryUnique` catches it where `EntryUnique` cannot
(H_one_eu) or catches it later (H_r60_eu). `Log.cfg` at MaxTerm 3 and MaxIds 3 remains NOT COMPLETED (section 1).

## 9. R2′ verified survivors: bounded validation (PR #150 plan, owner direction 2026-11-17)

cody authorized at most eight runs of two hours each, no R640 (`docs/handoff/PR149_150_Owner_Direction.md`). The plan
is `docs/AFR_001_R2_AND_SINGLE_COPY.md` 2.4. Every run below is exhaustive; none hit its limit. Setup as at the top
of this file: `-workers 2`, `-Xmx6g`, two runs at a time, `states/` deleted after each run.

**Disclosure.** V2's first launch was killed at about 3 GB of states by a container restart, before it finished.
Nothing from it is reported. It was run again from the start at the same configuration, so nine launches were
made for eight completed runs.

**Correction (2026-11-19, Codex review of #180 section 3).** `Combined.tla`'s `NewS` gave the empty set on R2′
self re-promotion, so the first V3 and M2b ran on a defective model. Both are rerun on the fixed model below; the
Log runs and M1-M4 are unaffected. Details: `docs/AFR_001_COMBINED_NEWS_FIX.md`.

**Model changes** (all behind switches; with `R2p = FALSE` and `InitAll = FALSE` the models behave as before):
- `InitAll`: S starts as all three nodes, so that S′ = S \ {old primary} has two members.
- `R2p` (both models): `Promote` sets S′ (rule 1), or {j} when j is the sole member and re-promotes itself at its
  own current term; the new primary's A starts as S′. `Promote` needs `rec.ready` (P2). `Log.tla` gains a `ready`
  field, set by the new action `MarkReady`; `Combined.tla` sets `deg` from |S′|.
- `FixPosAck` (both): a member counts toward an acknowledgement only once positioned in the primary's term (rule 2).
- `FixPosGrant` (Combined): a member grants the primary a lease only once positioned (rule 2).
- `FixLeaveS` (Log): a reseed (R6.4) is allowed only for a node outside S (rule 3).
- `FixReadyPos` (both): READY needs every member of S′ positioned, at the same term, with no reconciliation or
  truncation intent pending (rule 4).

| Run | Model, bounds | Change | Expected | Result |
|---|---|---|---|---|
| V1 | Log, MaxTerm 3, MaxIds 2, one crash | R2′, all rules | PASS | **PASS**, 14,186,264 distinct states, depth 42, 20 min |
| V2 | Log, MaxTerm 2, MaxIds 3, one crash | R2′, all rules | PASS | **PASS**, 38,833,104 distinct states, depth 41, 64 min (rerun, see disclosure) |
| V3 | Combined, MaxTerm 3, MaxIds 2, one restart, `Safety` and `StaleAuthBounded` | R2′, all rules | PASS | ~~PASS, 19,225,779 distinct states, depth 42, 17 min~~ (defective model, withdrawn). **Rerun on the fixed model: PASS**, 20,264,050 distinct states, depth 42, 28 min |
| M1 | as V1 | `FixLeaveS = FALSE` (no rule 3) | violated | **violated**, depth 14 (`AckedPreserved`) |
| M2 | as V1 | `FixPosAck = FALSE` (no rule 2) | uncertain | **no violation**, 14,469,966 distinct states |
| M2b | as V3 | `FixPosGrant = FALSE`, `FixPosAck = FALSE` | violated | ~~no violation, 21,048,786 distinct states, depth 43~~ (defective model, withdrawn). **Rerun: no violation**, 22,125,137 distinct states, depth 43, 21 min |
| M3 | as V1 | `FixM1 = FALSE` (KIP-101 single answer) | violated | **no violation**, 14,186,264 distinct states: the same count as V1 |
| M4 | as V1 | `FixReadyPos = FALSE` (READY before survivors are positioned) | record | **violated**, depth 12 (`AckedPreserved`) |

**M1 trace.** n1 (primary, term 1) acknowledges entry `(1, 1)` on all three nodes; n3 also holds n1's unacknowledged
`(1, 2)`. n2 is promoted to term 2 with S′ = {n2, n3}. n3 checkpoints both entries, so its reconciliation against
n2 (truncate to LSN 1) falls below its checkpoint and it starts a reseed while still in S. The reseed empties n3's
log, so a member of S, n3, no longer holds the acknowledged entry. n3 is a survivor, not the old primary, so this is a
direct case for rule 3. It answers the re-review's doubt (the earlier L_keepS_kip279 trace reseeded the old
primary).

**M4 trace.** n1 (term 1) still believes it is primary after n2 is promoted to term 2 with S′ = {n2, n3}, and ships an
unacknowledged entry `(1, 1)` to n3. Without rule 4, n2 records READY at once. The witnesses then promote n3 to term
3 (S′ = {n3}). After that promotion is chosen, n3 records a truncation intent from its reconciliation against n2,
whose log lacks the entry. n3 becomes primary at term 3, acknowledges `(1, 1)` (S = A = {n3}), and then applies the
old intent, which deletes it.
- Rule 4 is therefore necessary in this model, not only conservative.
- The trace also shows a gap rule 4 does not close for **operator promotion inside S**, which waives P2 (READY) and
  is not in either model: a node can be promoted while a reconciliation against an older primary is still in
  progress, and apply its truncation after it has acknowledged. P6 does not exclude this today. See the proposal in
  `docs/AFR_001_R2_AND_SINGLE_COPY.md` 2.4.

**What this shows.**
- Within these bounds and the modelling limits (section 5), R2′ with rules 1-4 keeps every Log invariant
  (`AckedPreserved`, `EntryUnique`, `LogMatching`, `StateMatchesLog`, `PositionedPrefix`, `HistoryUnique`) and every
  Combined invariant, including `StaleAuthBounded`.
- Rules 3 and 4 are each necessary: removing either produces a lost acknowledged write.

**What it does not show.**
- **Rule 2 is not shown necessary.** M2 and M2b find nothing. In these models a member counted for an
  acknowledgement must already hold the entry, and the lease model has no standby read leases, so rule 2's
  standby-lease clause has nothing to protect here. Rule 2 stays argued, not checked.
- **M3 does not discriminate.** Its state count equals V1's, so `FixM1` changes no behaviour at MaxIds 2 under R2′, as
  with L_M1_ids2 earlier. It says nothing about KIP-101 against KIP-279 under R2′.
- **Self re-promotion (4.2):** reachable in `Combined.tla` at V3's constants (five steps; probes in
  `docs/AFR_001_COMBINED_NEWS_FIX.md` section 5). The first V3 reached it with the `NewS` defect, so the statement
  "every Combined invariant" above rests on the rerun. In `Log.tla` it is still not shown reachable.
- Standby read leases, silent loss (D-02), torn tails, a standby's volatile tail, `m = 2` admission and operator
  promotion are not modelled. Rule 5 (shrink of a survivor that never positions) is covered only as the existing
  shrink action.
- `Log.cfg` at MaxTerm 3 and MaxIds 3 remains NOT COMPLETED (section 1); V1 and V2 each cut one bound.

## 10. Promotion during reconciliation (`PromoteRecon.tla`)

Preregistered, run and reviewed in `docs/AFR_001_PROMOTION_DURING_RECON.md` (sections 4-6), not here. In short: X1 and
X2 PASS, exhaustive (891,002 and 7,423,089 distinct states). Quiescence (MQ), durable completion (MD), the reply term
check (MR), the self re-promotion barrier (MB) and R9-C (MW) are each needed, and MP gives R2′ rule 2's lease clause
its first evidence, for `LeasedPrefix` only. X1 also found a checkpoint race with a reply in flight, now fixed. The
review's two blockers are fixed in text only and are not model-checked.

## 11. The reconciliation blockers (`ReconBlockers.tla`)

Preregistered, run and reviewed in `docs/AFR_001_RECON_BLOCKERS.md`, not here. In short: C1 (case 1, the request
binding) and C2 (case 2, four nodes, the cross-term pending-intent block) PASS exhaustively (929,059 and 5,675,611
distinct states); C3 (case 3, two entries, a crash and self re-promotion) also PASSES exhaustively (23,744,032 distinct states, 89 min). Without the binding (MB), the
pending-intent block (MI) or the new term (MNH), the predicted counterexamples come back. The probes show self
re-promotion, LSN reuse under the new term and a carried older-term intent are reachable. The bounds are narrow
(section 5 there), and the standby read rule R9-F is text only.

## 12. Report identity and reseed (`ReportReseed.tla`)

Preregistered in `docs/AFR_001_REPORT_MODEL_PREREG.md`. **Stopped before any counted run** (section 12 there): the
fidelity check found two `AckSound` counterexamples to the correct protocol, reproduced by `ReportReseedTrR7` (R7
outside S, a truncation inside a live grant) and `ReportReseedTrW` (a wipe seeded from a deposed primary). Y9-T as
written is blocked.

## 13. L-1 authority (`L1Authority.tla`)

Preregistered in `docs/AFR_001_L1_AUTHORITY.md` section 6; counted runs below. Earlier uncounted scratch runs: with A-1
off, `OneAuthority` fails in 9 steps (a degrading shrink chosen with its reply lost, then R7 case 1 outside S);
each of the other two parts of A-1, removed alone, fails too; the corrected model completes at MaxTerm 2 and MaxVer
5 with no error (743,986 distinct states). L-1 stays UNRESOLVED. The same `NewS` defect in `Combined.tla:105` is
now fixed there too (section 9). **Counted runs (2026-11-21): L1-F, L1-Fi, L1-Fr PASS** (3,726,353, 8,342,571 and 4,138,849 distinct states,
exhaustive); L1-mA1, L1-mVer, L1-mDec and L1-P violated as expected. `docs/AFR_001_COMBINED_NEWS_FIX.md` section 5.

## 14. Live grants (`LiveGrant.tla`)

Preregistered in `docs/AFR_001_LIVE_GRANT_PREREG.md`. **No counted run; the owner's GO is needed.** The twelve
regression traces pass their four checks with `live_grant_traces.sh` (uncounted). Writing and reviewing the model
found four protocol gaps (LG-F1 to LG-F4, section 2 there).

**Evidence boundary (Codex review of #180, section 4).** `L1Authority.tla` and `LiveGrant.tla` are not composed.
LiveGrant models Shrink atomically, and L1Authority has no replicated log, so neither shows exclusive authority and
acknowledgement safety together under the joint protocol. Final acceptance of a custom protocol would need a
composition argument or an integrated model. A depth-bounded LiveGrant check is evidence for that depth only.
