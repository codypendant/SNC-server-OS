# SNC-AFR-001: do the split models preserve the safety properties?

- **Status: PROPOSED / PARTIAL VERIFICATION.** Bounded model-checking evidence, not a proof. Nothing here
  authorizes implementation, and AFR-D-01, AFR-D-02 and AFR-D-03 stay unaccepted.
- **Trigger.** Codex's review of PR #144 (`docs/handoff/PR144_Closure_Codex_Review.md`): "verify that the
  abstractions used to split Authority, Log, and Combined preserve the safety properties we intend to prove", with
  concern 3 (the combined model's `Auth` and the group record's term) first.
- **Method.** Try to break the split. For each model, state what it assumes about the parts another model checks.
  Then either check that assumption as an invariant in the model that has the detail, or add the hidden behaviour
  to the model and see whether safety survives. Each term guard that fences a stale node gets a mutation run. No
  state limits were raised. Every run is in `spec/tla/RESULTS.md` section 7.
- **Review.** An independent adversarial QA found one major error in the first draft: a guard this document called
  redundant is needed once operator promotion is on (section 1). It also found overclaims and a defect in how the
  shared invariant was stated. All are corrected here and the affected runs were redone.

## 1. Concern 3: `Auth` and the group record's term

`Auth(n)` in `Combined.tla` is `isPrim[n]`, the learn barrier, live member leases and, in degraded mode, a live
witness lease. It never compares `T[n]` with the record R.

**The literal invariant fails, by design.** `AuthAtRecTerm` (any node with `Auth` is at R's term) is violated in
two steps: `Tick, Promote(n2)`. The old primary keeps its lease after the new term is chosen. That is the lease
protocol working as intended: the new primary waits `learned + W`, instead of the old one being told to stop.

**The invariant that holds.** `StaleAuthBounded` says that if a node has `Auth` below R's term:
- R's primary has no `Auth` at R's term; and
- once R's primary has learned, the stale node's remaining authority (`Rem`) ends no later than R's primary's learn
  barrier.

Both models now state it with the same formula. `Rem` differs only where the models differ:
- in `Combined.tla` it is the shortest live member lease and, in degraded mode, the single witness-lease timer;
- in `Authority.tla` the witness part is the largest lease a witness majority still holds.

A first version required that R's primary have no `Auth` at all. TLC refuted it in `Authority.tla`: an old primary
promoted again, before it learns the new term, still holds its old-term lease, which is safe (Q_A_compete_ops_t3,
Q_A_stale_gate). The clause now says "no `Auth` at R's term".

**Results:**

| Run | Model, configuration | Result |
|---|---|---|
| X_literal | Combined, `AuthAtRecTerm` | violated, depth 3 (expected) |
| Q_shipped | Combined, `Combined.cfg` as committed, MaxTerm 3 | **PASS**, exhaustive, 15,151,183 states (with the first version, which implies the current one) |
| C_opInside_t3 | Combined, operator promotion inside S without READY (R7), MaxTerm 3 | **PASS**, exhaustive, 16,505,916 states (first version) |
| Q_opOut_t3_ids2 | Combined, operator promotion outside S from a degraded READY record (5b), MaxTerm 3, `OneAuthority` and the bound | **PASS**, exhaustive, 45,720,782 states (first version) |
| B_stale_core | Authority, restart core | **PASS**, exhaustive, 1,441,064 states |
| B_stale_gate | Authority, 5c gate with one fence and one return, MaxTerm 3 | **PASS**, exhaustive, 27,814,539 states |
| B_compete_ops_t3 | Authority, competing promotions with operator promotion, MaxTerm 3 | **PASS**, exhaustive, 18,009,677 states |
| C_oneTerm_t3 | Combined, the stronger `OneTermBehind` | **PASS** without operator promotion (15,151,183 states) |
| C_oneTerm_opInside_t3 | Combined, `OneTermBehind` with operator promotion inside S | violated, depth 8. The stronger form is not a protocol property; `StaleAuthBounded` holds there |

Combined runs that passed the first version also pass the current one, because the current one is weaker.

**Mutations of the indirect fencing.** Each removes one guard. A failure counts only if it breaks `Safety` or
`OneAuthority`, not just the new invariant:

| Guard removed | Result |
|---|---|
| R4: a member grants only to the primary of its own term (`Grant`) | `Safety` violated, depth 7 (S_grantTerm) |
| R6.0/R4: learning a newer term ends primary status (`Learn`) | `Safety` violated, depth 6 (M_learnStep) |
| R1: a member joins S only from A (`Grow`) | `Safety` violated, depth 7 (S_AinS) |
| R3: the `learned + W` barrier | `Safety` violated, depth 5 (M_barrier) |
| `Current`'s `rec.term = T[p]` (used by grow, shrink, READY and add) | no violation without operator promotion (T_current, MaxTerm 2). With operator promotion at MaxTerm 3, `OneAuthority` violated, depth 11 (T3b_current): n1 is promoted again before it learns, then grows S at its old term |
| W1's `rec.term = T[n]` (`WGrant`) | no violation without operator promotion (M_w1Term_t3, MaxTerm 3, protocol's state count). With promotion outside S from degraded mode, `OneAuthority` violated, depth 12 (Q_w1Mut_opOut_ids0, found by the QA) |
| `Ship`'s `T[m] = T[p]` and `AddA`'s `T[c] = T[p]` | no violation, also with operator promotion at MaxTerm 3 (T3_ship, T3_addA, protocol's state count) |

**Findings:**
- **Two guards look redundant until operator promotion is on.** `Current`'s term check and W1's term check are
  load-bearing exactly when a stale primary can be named in R again before it learns, which only operator
  promotion allows. Without operator promotion in the model, a mutation run would have called them redundant.
  The first draft of this document did, for W1.
- **The `Ship` and `AddA` term checks are implied** by `pos[m] = T[p]`. A member is positioned only at its own
  term, and learning a newer term resets its position [INF]. They are kept as written in the protocol.

## 2. What each model assumes about the others

| # | Model | Assumption about a part another model checks | Direction | How it is checked |
|---|---|---|---|---|
| A1 | Combined | Paxos chooses at most one record per version | under-approximates (hides competing proposals) | `Authority.tla` with `Compete`: two promotions race, a witness may switch, one is chosen and the other is abandoned with its acceptances in place. **Pass** at MaxTerm 2 (restart core) and MaxTerm 3 (with operator promotion). The barrier and W1 mutations are caught. Not modelled: a promotion accepted by a minority and later revived, or overtaken by a non-promotion change for the same version (see A7) |
| A2 | Combined | degraded authority is one timer, renewed only while R names the holder at its term (W1 against the chosen record) | under-approximates per-witness views | `StaleAuthBounded` holds in `Authority.tla` with per-witness W1. This makes the abstraction plausible. It does **not** prove it sound: Combined's durability results were checked over fewer witness-renewal behaviours than `Authority.tla` allows, and nothing shows Combined's log safety depends only on this invariant |
| A3 | Combined | reconciliation is one atomic step that leaves a positioned member with a prefix of the primary's log | under-approximates (no intents, checkpoints, reseeds or crashes mid-way) | `PositionedPrefix`, `AckedPreserved` and `StateMatchesLog` in `Log.tla`, which models those steps |
| A4 | Log | authority is not modelled: any primary may append, and an ack needs every member of A at its term | over-approximates authority | sound for Claim D by construction: removing a restriction only adds behaviours |
| A5 | Log | promotion is always from S, and S becomes `{j}` | excludes operator promotion | `Combined.tla` now has operator promotion inside S (passes) and outside S from degraded mode. The latter loses acknowledged writes (C_opOutsideDeg_safety, depth 7), which Claim D excludes by contract. 5c (outside S from a non-degraded record) is forbidden by R7 and not in Combined |
| A6 | Authority | `Grow` needs no log condition | over-approximates | sound for Claim L by construction |
| A7 | Authority | grow, shrink and READY are written to every witness at once, and disabled while a promotion is pending | under-approximates (lagging witnesses; a stuck proposal) | **not checked.** Argued: a lagging witness holds an older record of the same term, so W1 only makes it refuse a grant (shrink) or grant to the same primary at the same term (grow). Open obligation |
| A8 | Combined | no fencing, no standby leases | excludes them | `Authority.tla` checks both. Their interaction with acknowledgements is **not checked** |
| A9 | both | a node learns only chosen records | assumed, not tested | the protocol's `Learn` reads R from a witness majority; this holds if Paxos reads are linearizable [INF] |

A row marked "over-approximates" is sound for the property that model checks. A row marked "under-approximates"
hides behaviour the protocol allows, so it needs the check in its last column.

## 3. What this establishes, and what it does not

**Established, bounded:**
- `StaleAuthBounded`, the explicit form of the indirect fencing Codex asked for, holds in both authority models in
  every completed run listed above, including operator promotion, competing promotions and the 5c gate with a
  returning node;
- removing any guard that fences a stale node breaks `Safety` or `OneAuthority`, except the `Ship` and `AddA`
  term checks, which are implied by positioning. Two of the guards matter only once operator promotion is in the
  model;
- competing promotions with abandoned acceptances do not break Claim L at the bounds checked.

**Not established (open obligations):**
- A2 is plausible but not proven sound; A7, A8 and A9 are not checked;
- there is no refinement mapping between the models. The checks above test the assumptions each model makes; they
  do not prove that the three models together equal the protocol;
- Codex's concerns 2 and 4 are unchanged: no all-scenario Authority run with restarts and fencing has completed,
  and the larger Log configuration has not finished;
- the bounds and the time model are those in `spec/tla/RESULTS.md` section 5.
