# R9 standby read leases: targeted model check (PROPOSED, R9 NOT FROZEN)

Owner authorization: `docs/handoff/PR148_Owner_Decisions.md` section 6 ("targeted TLA+ modeling task only", bounded
configurations, mutation tests, independent review before R9 is frozen), restated in
`docs/handoff/PR149_150_Owner_Direction.md` section 6. Models and docs only. This file does not edit
`docs/SNC_GUARANTEES.md` or the AFR decision documents; section 5 lists the wording changes the result calls for.

**Short answer.** R9's mechanism holds in every completed lane, and every guard that matters is caught when it is
removed. But R9's text, read literally, is not enough. The model refutes one of its sentences, finds one rule it
does not state, finds one clause redundant, and finds one dependence it does not cite:
1. "A standby that holds a lease acknowledges an entry only once it has applied it" is too weak. An ack sent
   before the lease was granted is counted after it (M_ackLeased). Every member of A must acknowledge only applied
   entries.
2. A lease may go only to a member that has applied everything p must hold. R9 does not say this, and without it a
   member that rejoined A after a degrade is leased while it still lacks acknowledged writes (M_leaseSync).
3. "Does not degrade" is redundant once acknowledgements follow A: removing that guard gives no counterexample in
   the two lanes checked (M_shrinkWait in L2, M_shrinkWait_L5); not run in L3, L4, L6s or at MaxTerm 3.
4. Under outside-S operator promotion, R9 relies on R4's holder rule (a standby stops serving once it learns a newer
   term), or on never rolling back a leased standby's applied state. R9 does not cite R4 (M_stopOnLearn_L5, _L6).

## 1. What the model is

`StandbyLease.tla` is `Combined.tla` (same time model, same abstractions, see its header and
`docs/AFR_001_MODEL_COMPOSITION.md`) with what Combined left out:

| Added | How |
|---|---|
| Standby read leases | `SGrant(p, m)` sets the standby's lease `sl[m]` and p's own record `sw[p][m]` ("may still be valid", R9's wait `W`). Capped by p's remaining authority (`FixM2`) |
| Apply order | `app[m]`, the applied prefix, separate from the durable log. A standby reads from `app` |
| Acks in flight | `MAck(m)` sends the prefix m reports; p counts the last ack received (`mack`). Added after the independent review (section 6, finding 1): with atomic acks, an ack sent before a lease could not be counted after it |
| Fencing | `Fence` (STONITH) and `Unfence` with the R7 boot read; outside-S promotion once every member of S is fenced (`OpFenced`) |
| Restart | volatile lease state lost; recovery replays the durable log. Mutation `FixRestartStop = FALSE` resumes the old term with p's lease record lost, which is what the C++ does today (G-30 evidence) |
| Drift | one mutation, `FixRhoWait = FALSE`: p's wait ends one time unit before the lease. This is the shape of the missing `(1+ρ)` factor (`replication.cpp:520`). Drift inside the bound is folded into `D`, as in the other models |

**Property.** `StandbyFresh`: a standby with a valid lease has applied every acknowledged write (G-30: "reflects every
write acknowledged before the read was sent"; a state invariant covers every read sent at or after the ack).
`StandbyValid` ignores the lease's term and the standby's own term: a standby that learns a newer term keeps serving
unless `StandbyStopOnLearn` is set, so the in-contract lanes do not rely on R4. With outside-S operator promotion,
acknowledged writes may be lost by contract (G-35), so those lanes check `StandbyFreshOp` instead: a lease granted at
term t reflects every write acknowledged at term t or later. `Safety` also checks `OneAuthority` and
`AckedPreserved`. `StandbyFreshNP` is `StandbyFresh` for standbys that are not primary, used where a mutation's
shortest trace would otherwise go through the new primary's own copy.

**Grants are atomic.** The standby's lease and p's record start in one step. A grant delayed in flight is anchored at
the standby's send time (R9), so it ends no later than the atomic one. This is argued, not checked, and it needs the
M2 cap to be anchored at the send time too (section 5, item 5).

## 2. Setup

TLC2 Version 2026.11.15.002146 (rev: bc1b506), `tla2tools.jar` v1.8.0, sha256 `7e53aa63...fb84f` (as in
`README.md`). Java 21. `-deadlock`, `-Xmx3g`, 4 workers, one run at a time, on a 4-core, 15 GB cloud container. Each
run copies the committed model into a fresh directory and changes only the stated constants (or, for "text"
mutations, the one stated expression). Wall-clock cap 600 s (probes 300 s). A run that hit the cap is **NOT
COMPLETED**. State files were deleted after every run.

`StandbyLease.cfg` as committed is lane L2. Three nodes, `P0 = n1`, `Q0 = n2`, `D = 2` throughout.

## 3. Fixed protocol

| Lane | Bounds and scenario (change from `StandbyLease.cfg`) | Invariant | Result | Distinct states | Depth |
|---|---|---|---|---|---|
| L2 | as committed: MaxTerm 2, MaxIds 1, automatic promotion, no restart, no fence | `Safety` | **PASS**, exhaustive | 3,476,571 | 41 |
| L3 | L2 + one restart | `Safety` | **PASS**, exhaustive | 10,878,248 | 42 |
| L4 | L2 + one fence (no return) | `Safety` | **PASS**, exhaustive | 13,906,284 | 42 |
| L1b | MaxTerm 1 (degrade, drop, rejoin, no promotion), MaxIds 1, one restart or one fence and return | `Safety` | **PASS**, exhaustive | 1,998,716 | 30 |
| L1c | MaxTerm 1, MaxIds 2, one restart, no fence | `Safety` | **PASS**, exhaustive | 10,112,619 | 39 |
| L1 | MaxTerm 1, MaxIds 2, one restart or one fence and return | `Safety` | **NOT COMPLETED** after 600 s, no violation in what was explored | 22,034,964 reached | |
| L5s | L2 + `OpOutsideDeg` (outside S from a degraded READY record) + `StandbyStopOnLearn` | `SafetyOp` | **PASS**, exhaustive | 4,249,269 | 39 |
| L6s | L2 + `OpFenced` (outside S once all of S is fenced), two fences + `StandbyStopOnLearn` | `SafetyOp` | **PASS**, exhaustive | 22,799,401 | 43 |

Restart and the return of a fenced node share `MaxRestarts`, so no lane has both. No lane has MaxTerm 3.

## 4. Each guard switched off

TLC must find a counterexample; a guard whose removal changes nothing is reported as redundant at these bounds, not
as passing. Lane L1x = MaxTerm 1, MaxIds 2, one restart, one fence (the counterexamples are found long before the
cap). Traces are TLC's action sequences after the initial state, as found; TLC returns one shortest trace, and the order
of independent steps is not unique. The count is the number of states in the trace,
including the initial one.

| Run | Guard removed | Lane | Invariant | Result | Counterexample |
|---|---|---|---|---|---|
| M_dropWait | R9: no removal of m from A while its lease may be valid | L1x | StandbyFresh | violated, 8 | NewEntry(n1), WriteLocal(n1), Shrink(n1,n2), WGrant(n1), SGrant(n1,n2), Drop(n1,n2), Ack(n1,1) |
| M_ackA | R9 "not without m": the ack needs every member of A, not only those still in S | L1x | StandbyFresh | violated, 7 | NewEntry(n1), WriteLocal(n1), Shrink(n1,n2), WGrant(n1), SGrant(n1,n2), Ack(n1,1) |
| M_ackDurable | apply before ack (acks report the durable end) | L2 | StandbyFresh | violated, 7 | NewEntry(n1), WriteLocal(n1), Ship(n1,n2), MAck(n2), SGrant(n1,n2), Ack(n1,1) |
| **M_ackLeased** | **R9's text**: only a member holding a lease acks applied entries | L2 | StandbyFresh | violated, 7 | same trace: n2 acks its durable entry before it is leased, is leased (nothing acked yet), and p counts the old ack |
| M_rho | p's wait covers the whole lease (`(1+ρ)` factor) | L1x | StandbyFresh | violated, 9 | NewEntry(n1), WriteLocal(n1), SGrant(n1,n2), Tick, Shrink(n1,n2), WGrant(n1), Drop(n1,n2), Ack(n1,1) |
| M_restartStop | R6.0: a restarted primary does not resume its term | L3 | StandbyFresh | violated, 9 | NewEntry(n1), WriteLocal(n1), SGrant(n1,n2), Restart(n1), Shrink(n1,n2), WGrant(n1), Drop(n1,n2), Ack(n1,1): today's C++ gap |
| M_m2 | R9 (M2): the lease never outlasts p's authority | L2 | StandbyFreshNP | violated, 13 | NewEntry(n1), Recon(n3,n1), Ship(n1,n2), AddA(n1,n3), Promote(n2), Learn(n2), Tick, WGrant(n2), Grant(n3,n1), SGrant(n1,n3), Tick, Ack(n2,1) |
| M_barrier | R3 `learned + W` barrier | L2 | StandbyFreshNP | violated, 11 | NewEntry(n1), Recon(n3,n1), SGrant(n1,n2), AddA(n1,n3), Grow(n1,n3), Promote(n3), Ship(n1,n3), Learn(n3), WGrant(n3), Ack(n3,1) |
| M_ackAuth | authority check on Ack | L2 | StandbyFreshNP | violated, 11 | Tick, NewEntry(n1), SGrant(n1,n2), Recon(n3,n1), AddA(n1,n3), Grow(n1,n3), Promote(n3), Ship(n1,n3), Learn(n3), Ack(n3,1) |
| M_leaseSync | lease only to a member caught up to p (whole condition) | L2 | StandbyFresh | violated, 9 | NewEntry(n1), WriteLocal(n1), Shrink(n1,n2), WGrant(n1), Drop(n1,n2), Ack(n1,1), AddA(n1,n2), SGrant(n1,n2) |
| M_leaseSync_pack | text: drop "applied ≥ p's own acks" | L2 | StandbyFresh | violated, 9 | as M_leaseSync |
| M_leaseSync_inh | text: drop "applied ≥ inherited end" | L2 | StandbyFresh | violated, 17 | NewEntry(n1), WriteLocal(n1), Promote(n2), Learn(n3), Ship(n1,n2), Apply(n2), MAck(n2), Ack(n1,1), Learn(n2), Tick, WGrant(n2), Tick, Recon(n3,n2), AddA(n2,n3), Grant(n3,n2), SGrant(n2,n3) |
| M_leaseSync_pos | text: drop "positioned on p's log" | L2 | StandbyFresh | **no violation**, exhaustive, 3,476,571 states | redundant at these bounds |
| M_leaseSync_pos_L3 | as above | L3 | StandbyFresh | **no violation**, exhaustive, 11,600,558 states | redundant at these bounds |
| M_shrinkWait | R9: no degrade while the lease may be valid | L2 | StandbyFresh | **no violation**, exhaustive, 3,581,833 states | redundant at these bounds |
| M_shrinkWait_L5 | as above, with outside-S promotion from degraded | L5s | SafetyOp | **no violation**, exhaustive, 4,432,953 states | redundant at these bounds |
| M_stopOnLearn_L5 | standby keeps serving after learning a newer term | L5s | SafetyOp | violated, 14 | NewEntry(n1), WriteLocal(n1), Shrink(n1,n2), WGrant(n1), Promote(n3), Learn(n3), SGrant(n1,n2), Ship(n1,n2), Apply(n2), MAck(n2), Ack(n1,1), Learn(n2), Recon(n2,n3): reconciliation truncates the leased standby's applied write |
| M_stopOnLearn_L6 | as above | L6s | SafetyOp | violated, 15 | the same, after Fence(n1) and promotion of n3 |

**Why the two redundant guards are redundant.**
- *Degrade (`Shrink`).* Shrinking S changes neither A nor the ack set; only `Drop` does, and `Drop` waits on its
  own. In the C++ there is no A, so degrading means acknowledging without the standby, and there the wait is the
  guard. In the A-based protocol it is implied by "does not remove m from A". The outside-S promotion that a degrade
  enables is fenced by the lease cap (M2) and the barrier, which M_m2 and M_barrier show are needed.
- *Positioned.* With R2 = `{j}`, a member reaches A only through `AddA`, which already requires it to be positioned,
  and the lease also requires applied ≥ inherited end. It can matter after a member's restart in a richer lane, at
  MaxTerm 3, or under R2′ (verified survivors, `docs/AFR_001_R2_AND_SINGLE_COPY.md` rule 2, which already requires
  it). None of these is checked here. Keep it.

## 5. Wording the result calls for (proposed, not applied)

For the thread that owns `docs/AFR_001_SAFETY_ANALYSIS.md` (R9) and `docs/SNC_GUARANTEES.md` (G-30). R9 stays
unfrozen until cody and Codex review this.

1. **Apply before ack, for every member of A.** Replace "A standby that holds a lease acknowledges an entry only once
   it has applied it" with "every member of A acknowledges an entry only once it has applied it". Evidence:
   M_ackLeased, M_ackDurable. An alternative ("p counts no ack that m sent before p's lease to m") needs each ack to
   say which lease it was sent under; the reviewer's model of it did NOT COMPLETE in 300 s (5.9M states, no
   violation), so it is not proposed. The C++ acks what is durable
   (`replication.cpp:769`), and applies on receipt (`:753`) before the record is durable, so it is applied first in
   practice; that is an observation, not a check.
2. **Lease only to a caught-up member.** Add: "p grants a standby lease only to a member of A at p's term that is
   positioned on p's log and has applied every entry up to max(inherited end, p's highest acknowledged LSN)" (the
   Grow test of R1, applied to `app`). Evidence: M_leaseSync, _pack, _inh. A loose C++ analogue is the `in_sync_`
   gate on the lease echo (`replication.cpp:519`); it tests durable catch-up, not the applied end.
3. **Degrade clause.** "Does not degrade" adds nothing once "does not remove m from A" holds and acks follow A
   (M_shrinkWait, _L5). Either drop it from R9 and G-30, or keep it labelled as a consequence. Recommendation: say
   "does not remove m from A while its lease may be valid; every acknowledgement needs all of A" (two requirements:
   M_dropWait and M_ackA each fail alone), and note that in the C++ (no A) degrading is that removal.
4. **Cite R4.** Add to R9: "R9 relies on R4's holder rule: a standby stops serving lease reads once it learns a newer
   term". At these bounds it is needed only with outside-S operator promotion (M_stopOnLearn_L5, _L6), where a write
   acknowledged at the old term after the promotion was chosen (lost by contract, G-35) disappears from a leased
   standby when reconciliation truncates it. The in-contract lanes pass without it at these bounds. The reviewer
   found an alternative that also passes L5s (exhaustive, 5,283,994 states): reconciliation never truncates a standby
   whose lease is valid. It was not run in L6s. R4 already states the holder rule, so this is a cross-reference,
   not a new rule.
5. **Anchor the M2 cap at the send time.** The cap "never outlasts p's own authority" is computed by p at the grant
   and applied on the standby from its send time, like the lease itself. The atomic-grant argument (section 1)
   depends on it.
6. **G-30 evidence.** Add: the model confirms the consequence of each listed gap. A primary-side wait shorter than
   the lease breaks G-30 (M_rho; in the C++ this happens only beyond its 0.9 margin, the assumption G-30 already
   states), and a lease record lost on restart while the primary keeps serving breaks it (M_restartStop; the C++ has
   no terms or A, so the mapping is loose).

## 6. Independent review (told to break the model and the claims)

The reviewer worked on the first version of the model (atomic acks, no drift mutation) in a separate scratch copy.

| # | Finding | Disposition |
|---|---|---|
| 1 | Atomic acks hide the race R9's wording allows: a durable ack sent before the lease is counted after. 7-step counterexample in the reviewer's copy | Fixed: acks in flight (`MAck`, `mack`) and `AckRule`; reproduced as M_ackLeased; wording item 1 |
| 2 | `FixShrinkWait` redundant | Confirmed in two lanes; wording item 3 |
| 3 | FixBarrier/FixAckAuth counterexamples went through the leased standby becoming primary | Rerun against `StandbyFreshNP`: both fail through a separate standby (M_barrier, M_ackAuth) |
| 4 | Drift not visible (`sl ≤ sw` by construction) | Fixed: `FixRhoWait` mutation (M_rho) |
| 5 | `FixLeaseSync` tested only whole | Split: two parts needed, positioned part redundant here (section 4) |
| 6 | Committed cfg matched no lane; results file and README missing | Fixed: cfg = L2; this file; README row |
| 7 | Lanes weaker than they sound (shared restart budget, L4 is crash-stop, fenced member's state counted in acks, `sw` reset in `Learn` never exercised at MaxTerm 2) | Recorded in section 7 |
| 8 | `OpInside` (case 5a) dropped; compare-and-sets atomic; grants in flight not modelled | Recorded in section 7 |
| 9 | `StandbyFreshOp` ignores acks below the lease's term, including ones the granting primary inherited | Recorded in section 7 |
| 10 | No action both assigns and freezes a variable, none missing from UNCHANGED (first version) | Revised model re-audited by script: clean (`Recon`'s two branches each freeze only what the other assigns) |

**Second pass on the revised model and this file.** The reviewer reproduced L2 (3,476,571 states), M_shrinkWait,
M_ackLeased and M_stopOnLearn_L5 exactly, found the `mack` design hides nothing at L2 (without the `Learn` reset L2
still passes, 4,911,309 states; the restart, return and `Recon` caps argued), and re-audited UNCHANGED: clean. Its
findings softened items 3, 4 and 6 and the short answer, dropped the unverified alternative in item 1, made item 3's
two requirements explicit, and marked the `in_sync_` analogue as loose; all applied above. It did not rerun L3, L4,
L1b, L1c, L6s or the other mutations.

## 7. Limits (what this does not show)

- **Bounds.** At most two terms; one or two entry ids; one restart or one fence-and-return; never a restart and a
  return together; L1 (MaxTerm 1, MaxIds 2, restart or fence) NOT COMPLETED. MaxTerm 2 never re-promotes a former
  primary, and has no case 5a (`OpInside`), which needs MaxTerm 3.
- **Abstractions.** Atomic compare-and-sets (a lease renewal racing an in-flight shrink is not modelled; item 3 makes
  it mostly moot); atomic grants (argued in section 1); witness Paxos as in Combined; drift as one mutation, not a
  rate model; a fenced member's acknowledged state still counts (it cannot change while fenced).
- **`StandbyFreshOp`** is weaker than needed: in the outside-S lanes a lease from the new primary is not checked
  against writes acknowledged earlier that it inherited. The in-contract lanes check those (M_leaseSync_inh).
- **R2.** The model uses today's R2 (`S = {j}` after promotion). R2′ (verified survivors) is not modelled; its
  rule 2 already requires positioned lease holders.
- **Not modelled:** forged lease frames (AFR-F-02 item 5, an authentication question), reseed and checkpoints
  (Log.tla), torn tails, standby volatile tails.
- **A8.** This covers the standby-lease side of A8 at these bounds: fencing, restarts and degrade interacting with
  acknowledgements and leases. A8's fencing side stays as argued in the protocol-acceptance review.

## 8. Reproduce

```sh
cd spec/tla
java -XX:+UseParallelGC -Xmx3g -cp tla2tools.jar tlc2.TLC -workers 4 -deadlock -config StandbyLease.cfg StandbyLease.tla
```

Other lanes and mutations: copy the files, change the constants named in sections 3 and 4 (and the `INVARIANT`
line), run the same command, and delete `states/` afterwards. "Text" mutations edit the `FixLeaseSync` line of
`SGrant` as stated.
