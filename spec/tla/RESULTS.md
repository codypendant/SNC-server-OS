# TLC results for SNC-AFR-001 (closure phase)

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
