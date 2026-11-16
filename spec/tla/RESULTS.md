# TLC results for SNC-AFR-001 (Revision 2)

These are the runs on the final models in this directory. Each run copies the models into a fresh directory and
applies the stated change to `Authority.cfg` or `Log.cfg`, nothing else.

**Setup:**
- TLC2 Version 2026.11.15.002146 (rev: bc1b506), from `tla2tools.jar` v1.8.0, sha256 `7e53aa63...fb84f` (full
  value in `README.md`). Java 21.
- 4 workers, `-deadlock`, `-Xmx6g`, on a 4-core, 15 GB cloud container.
- Every run has a wall-clock limit. A run that hit its limit is marked **NOT COMPLETED**, with the states it had
  explored when it stopped. It proves nothing about the states it did not reach.

A passing run is a bounded result: no violation exists **within the stated bounds and the modelling limits**
listed at the end. It is not a proof of the protocol.

## 1. The fixed protocol

| Run | Model | Bounds and scenario | Result | Distinct states | Depth |
|---|---|---|---|---|---|
| FIXED_log_ids3 | Log | `Log.cfg` as committed: 3 nodes, MaxTerm 3, MaxIds 3, MaxCrashes 1, all fixes | **PASS**, exhaustive | 10,180,593 | 47 |
| FIXED_log_ids2 | Log | as above, MaxIds 2 | **PASS**, exhaustive | 785,395 | 40 |
| OK_restart | Authority | MaxTime 4, MaxTerm 2, one restart; no degraded mode, no standby leases, no operator promotion outside S, no fencing | **PASS**, exhaustive | 8,481,832 | 25 |
| OK_standby | Authority | as OK_restart, but standby leases on and no restart | **PASS**, exhaustive | 5,646,785 | 32 |
| OK_opoutside | Authority | as OK_restart, but operator promotion outside S and one fencing on, no restart | **PASS**, exhaustive | 3,067,876 | 25 |
| OK_degraded | Authority | as OK_restart, but degraded mode (shrink and witness leases) on, no restart | **NOT COMPLETED** after 600 s, no violation in what was explored | 24,990,461 (5,076,648 on queue) | 27 reached |
| OK_all_t3 | Authority | every scenario on, MaxTime 3, MaxTerm 3 | **NOT COMPLETED** after 600 s, no violation in what was explored | 31,429,483 (17,778,289 on queue) | 17 reached |
| FIXED_auth_t4 | Authority | every scenario on, MaxTime 4, MaxTerm 3 | **NOT COMPLETED** after 1800 s, no violation in what was explored | 95,462,121 (55,621,938 on queue) | 18 reached |

**What this does and does not show:**
- Claim D (Log): exhaustive at the committed bounds.
- Claim L (Authority): exhaustive only one scenario at a time, at MaxTime 4 and MaxTerm 2. **Degraded mode (R8)
  has no completed exhaustive run.** Its fixes are supported only by the switch runs in section 2 and by the partial
  run above.
- No configuration with every scenario at once has completed.

## 2. Each fix switched off

Each fix was turned off in turn. TLC must find a counterexample. The traces are TLC's action sequences, as found.

| Run | Fix removed | Result | Counterexample (TLC's actions) |
|---|---|---|---|
| F_B1 | R1.2 (drop from A only once outside S) | violated, depth 6 | NewEntry(n1), WriteLocal(n1), Drop(n1,n2), Ack(n1,1): B1, a lost grow reply treated as failure |
| T2 (variant) | R1.2's "read still at p's own term" only | violated, depth 12 | NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n2), Learn(n3), ReconStep(n3,n2), AddA(n2,n3), Grow(n2,n3), Promote(n3), Drop(n1,n2), Ack(n1,1) |
| F_R60 | R6.0 (restarted primary never writes its old term) | violated, depth 6 | NewEntry(n1), Ship(n1,n2), Crash(n1) |
| F_clear | R6.3 (clear the intent before accepting entries beyond it) | violated, depth 17 | NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n1), Learn(n2), NewEntry(n2), WriteLocal(n2), ReconStep(n1,n2), ApplyIntent(n1), ReconStep(n1,n2), Ship(n2,n1), AddA(n2,n1), Ack(n2,1), Grow(n2,n1), ApplyIntent(n1): interrupted truncation |
| F_ckintent | R6.3 (no checkpoint while an intent is pending) | violated, depth 10 | NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n1), Learn(n2), ReconStep(n1,n2), Checkpoint(n1), ApplyIntent(n1): finding T1 |
| F_reseed | R6.4 (reseed when truncation is below the checkpoint) | violated, depth 9 | NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n1), Checkpoint(n1), Learn(n2), ReconStep(n1,n2), ApplyIntent(n1) |
| F_inc | R6.5 (durable incomplete flag) | violated, depth 10 | NewEntry(n1), WriteLocal(n1), Checkpoint(n1), Promote(n2), Learn(n1), Learn(n2), ReconStep(n1,n2), RDiscardLog(n1), Crash(n1) |
| codex1_rev1_rule_isolated | R3 as in Revision 1 (FixR3 and Barrier off; restart scenario only, MaxTime 5) | violated, depth 13 | Tick, Propose(n2), Restart(n2), Tick, Tick, Grant(n2,n1), WAccept(w1), WAccept(w2), Learn(n2), WGrant(w1,n2), WGrant(w2,n2): **Codex issue 1**, a grant after restart outlives the restart wait |
| B2_isolated | R2 READY rule and Barrier off (no operator path, no restart, MaxTime 5) | violated, depth 17 | Propose(n2), WAccept ×2, Learn(n2), AddA(n2,n3), Grow(n2,n3), Ready(n2), Propose(n3), WAccept ×2, Learn(n3), WGrant(w1,n3), WGrant(w2,n3): B2, fast double promotion |
| F_codex2_barrier | Barrier (learned + W) | violated, depth 11 | SGrant(n1,n2), Shrink(n1), Propose(n3), WAccept ×2, Learn(n3), WGrant(w1,n3), WGrant(w2,n3): **Codex issue 2**, no lease-expiry barrier |
| F_codex2_W1 | W1 (witness grants only per its highest accepted record) | violated, depth 15 | Shrink(n1), Propose(n2), WAccept ×2, Learn(n2), Tick, Tick, WGrant(w1,n1), WGrant(w2,n1), WGrant(w1,n2), WGrant(w2,n2): **Codex issue 2**, witnesses grant to two terms |
| F_M2 | R9 (standby lease capped by p's authority) | violated, depth 13 | AddA(n1,n3), Grant(n3,n1), Propose(n2), WAccept ×2, Learn(n2), Tick, SGrant(n1,n3), Tick, WGrant(w1,n2), WGrant(w2,n2): M2 |
| F_FixOp | R7 (outside-S promotion needs READY or a STONITH assertion) | violated, depth 15 | Tick, Tick, Propose(n2), WAccept ×2, Propose(n3), WAccept ×2, Learn(n3), Tick, Tick, Grant(n2,n1), WGrant(w1,n3), WGrant(w2,n3) |
| F_5c | admit case 5c (`AllowOp5c = TRUE`) | violated, depth 12 | SGrant(n1,n2), Propose(n3), WAccept ×2, Learn(n3), Tick, Tick, Grant(n2,n1), WGrant(w1,n3), WGrant(w2,n3): **case 5c stays UNRESOLVED** |

**Not reproduced:**

| Run | Fix removed | Result |
|---|---|---|
| F_M1_kip101 | R6.2 (KIP-279 iteration; FALSE = KIP-101's single answer) | **no violation**, exhaustive, 10,180,593 distinct states, depth 47 |
| F_M1_term4 | as above, MaxTerm 4, MaxIds 2 | **no violation**, exhaustive, 1,626,298 distinct states, depth 43 |

M1 is **not validated by the model**. F_M1_kip101 has exactly the same state count as FIXED_log_ids3, so at these
bounds the switch changes no reachable behaviour. M1 needs a follower holding entries from two or more terms the
leader never had. In this model that seems unreachable: R2 promotes only a member of S, and a member reconciles
before it joins A. This is a hypothesis, not a result. R6.2 therefore rests on the KIP-279 argument in the analysis
alone. A model that allows unclean promotion would be needed to test it.

**QA blocker, confirmed by TLC.** The independent QA's variant model (scratch file, not committed) used the
Revision 2 draft's "caught up" rule (p's acknowledged LSN alone). TLC violated `AckedPreserved` at depth 11. The
committed `Log.tla` uses the corrected rule (`inh`, `pack`), and FIXED_log_ids3 passes.

**R3 simplification is not model-checked.** `Authority.tla` keeps all three R3 bounds (boot, last grant and
learned + W). The analysis argues that the first two are implied by the third. The run meant to check that
(BARRIER_ONLY_auth_t4, every scenario at MaxTime 4) did **NOT COMPLETE** after 1800 s (86,836,552 distinct states,
52,128,767 on queue, no violation in what was explored).

## 3. Modelling limits

A pass is weaker than it looks because of these simplifications:
- **Time and drift.** One global integer clock. Drift within the bound is abstracted into "a grant lasts D, a wait
  lasts at least D". Drift beyond the bound is not modelled.
- **Group record.** One register. Witness Paxos is abstracted: a promotion is accepted witness by witness and chosen
  at a majority, and its proposer never fails. Grow, shrink and READY are atomic compare-and-sets, disabled while a
  promotion is pending.
- **Acknowledgement.** `Ack` is atomic in `Log.tla` and checks no authority. Claim L is assumed there, not modelled.
- **Split models.** B1 and T2 are covered only in `Log.tla`. Authority is not modelled there, and logs are not
  modelled in `Authority.tla`.
- **Term history H.** Loss of H is not modelled. The "H cannot answer" branch of R6.4 is not exercised.
- **Grants.** A member grants only while it is in the primary's A.
- **Bounds.** Three nodes, three witnesses, at most three terms (two in the per-scenario runs), one restart, one
  crash, one fencing, and two or three entry ids. Failures that need more of any of these are not covered.
- **Unclean promotion** (outside S without READY or STONITH) is excluded, except in F_FixOp and F_5c.
