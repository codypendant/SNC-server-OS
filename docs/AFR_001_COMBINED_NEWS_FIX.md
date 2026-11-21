# AFR-001: the `Combined.tla` `NewS` fix and the counted L-1 runs

Status: **preregistered before any run (commit 0a9e734); results in section 5.** This covers Codex's review of #180 (`docs/handoff/PR180_Codex_Review.md`),
section 3 (fix the defect on site, rerun what it affects, update the evidence record) and section 1 (the seven
targeted L-1 runs). The L-1 runs go ahead under the owner's standing instruction of 2026-11-19 16:06 UTC for small
bounded steps, as the coordinator recorded. Nothing here accepts A-1, LG-F1..LG-F4 or any contract text. Claims
are labelled FACT, EXT, INF or HYP.

## 1. The defect

`spec/tla/Combined.tla:105`, before this fix:

```
NewS(j) == IF R2p /\ j \in rec.S THEN rec.S \ {rec.prim} ELSE {j}
```

R2′'s self re-promotion (`Promote`'s third disjunct) has `j = rec.prim` and `rec.S = {j}`. `NewS(j)` then gives
`{j} \ {j} = {}`: the promoted node's S is empty, `deg` is FALSE, and under `R2p` its A starts as that empty S.
R2′ rule 1 says S′ = {p} for a sole member's self re-promotion (`docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md` row
"R2′ rule 1"). `Log.tla`, `PromoteRecon.tla`, `ReconBlockers.tla` and `ReportReseed.tla` all write this correctly
(`IF j = rec.prim THEN {j}`) [FACT, read].

**Reachable in V3's configuration** [FACT, diagnostic run on the unfixed model, V3's constants, probe
`rec.S # {}`]: violated in five steps: Tick, Shrink(n1, n2), Shrink(n1, n3), Restart(n1), Promote(n1).

**Fix:** `NewS(j) == IF R2p /\ j \in rec.S /\ j # rec.prim THEN rec.S \ {rec.prim} ELSE {j}`, the same as
`L1Authority.tla`. It changes behaviour only when `R2p` is TRUE and the old primary re-promotes itself.

## 2. Affected results

Every recorded `Combined.tla` run with `R2p = TRUE` is affected; a run with `R2p = FALSE` is not, because `NewS`
then gives `{j}` either way [FACT, read]. The only such runs are in `spec/tla/RESULTS.md` section 9 [FACT, searched
`RESULTS.md`, `RESULTS_R9.md` and `docs/` for `Combined` with `R2p`]:

| Run | Constants | Recorded result |
|---|---|---|
| V3 | `Combined.cfg` with `R2p`, `InitAll`, all rules; `Safety`, `StaleAuthBounded` | PASS, 19,225,779 distinct, depth 42 |
| M2b | as V3, `FixPosGrant = FALSE`, `FixPosAck = FALSE` | no violation, 21,048,786 distinct, depth 43 |

Both are **withdrawn** as evidence until rerun. Documents that cite them are to be annotated with the rerun result:
`docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md` rows R2′ rule 1, Rule 2, P1-P5/P7 and I4;
`docs/AFR_001_R2_AND_SINGLE_COPY.md` 2.5; `docs/AFR_001_PROMOTION_DURING_RECON.md` section 6; `spec/tla/RESULTS.md`
section 9. `docs/handoff/PR151_152_Codex_Review.md` cites V3's count; handoff files are verbatim records and are not
edited.
`StandbyLease.tla` has no self re-promotion; the other R2′ models are separate modules and unaffected.

## 3. Reruns (preregistered)

Setup as for the original section 9 runs: `-workers 2`, `-Xmx6g`, `-deadlock`, two at a time, `states/` deleted after
each run, 2 h cap, a timeout is NOT COMPLETED.

| Run | Constants | Checks | Expected |
|---|---|---|---|
| V3-fix | as V3 | `Safety`, `StaleAuthBounded`; `-coverage 5` | PASS, and the self re-promotion disjunct of `Promote` is taken at least once (coverage) |
| M2b-fix | as M2b | `Safety`, `StaleAuthBounded` | no prediction; recorded either way |

If V3-fix fails, the counterexample is inspected state by state and goes to Codex and the owner; R2′'s V3 evidence
then stays withdrawn.

## 4. The seven L-1 runs

As preregistered in `docs/AFR_001_L1_AUTHORITY.md` section 6 (merged in #180, unchanged). Configs are printed by
`spec/tla/l1_authority_cfg.sh RUN`. Setup: `-workers 1`, `-Xmx4g`, `-deadlock`, at most three at a time, 2 h cap,
a timeout is NOT COMPLETED. Every counterexample is inspected state by state.

## 5. Results

All runs completed inside their caps; none timed out. TLC as in `spec/tla/README.md` (sha256 checked).

**Combined reruns** (fixed model, 2026-11-19) [FACT]:

| Run | Result | Distinct states | Depth | Time |
|---|---|---|---|---|
| V3-fix | **PASS**, `Safety` and `StaleAuthBounded` | 20,264,050 | 42 | 28 min 28 s |
| M2b-fix | **no violation** | 22,125,137 | 43 | 20 min 47 s |

- V3-fix's coverage shows the self re-promotion disjunct evaluated to its last conjunct 9,280 times. Coverage counts
  evaluations, not truth, so it does not by itself prove the branch was taken [FACT]. Reachability is shown by
  probes on the fixed model instead (both independent reviews, below): a self re-promoted primary with S′ = {p}
  and `deg` TRUE holding authority, acknowledging in the new term, and a later term 3 [FACT, reduced MaxIds 1].
- So the R2′ evidence of `RESULTS.md` section 9 stands on the rerun, not on the first V3. M2b's reading is
  unchanged: rule 2 is still not shown necessary.

**What the defect had hidden** (review A, TLC on the unfixed model) [FACT]: after a self re-promotion the primary had
S = A = {} and `pdeg` FALSE, so it held authority with no grants and no witness lease, which never expired. It could
acknowledge on its own log, and `AckedPreserved` (over S) was vacuous. The fixed model passes regardless.

**The seven L-1 runs** (2026-11-21, `-workers 1`, three at a time) [FACT]:

| Run | Expected | Result | Distinct states | Depth | Time |
|---|---|---|---|---|---|
| L1-F | PASS | **PASS** | 3,726,353 | 31 | 3 min 29 s |
| L1-Fi | PASS | **PASS** | 8,342,571 | 31 | 7 min 31 s |
| L1-Fr | PASS | **PASS** | 4,138,849 | 33 | 3 min 48 s |
| L1-mA1 | violated | **violated**, `OneAuthority`, 8 steps | | | 2 s |
| L1-mVer | violated | **violated**, `OneAuthority`, 10 steps | | | 3 s |
| L1-mDec | violated | **violated**, `OneAuthority`, 9 steps | | | 3 s |
| L1-P | violated | **violated**, `NoL1Window`, 3 steps | | | 0 s |

Counterexamples, inspected state by state:
- **L1-mA1:** ShrinkSend(n1, n2), ShrinkChosen(n1), Promote(n3), Learn(n3), Tick, Tick, WGrant(n3), Grant(n2, n1).
  In the last state `n1` is primary at term 1 in normal mode (`pdeg` FALSE) with a live grant from `n2`, and `n3`
  is primary at term 2 with A = {n3}, degraded, holding a witness lease. Section 2 of
  `docs/AFR_001_L1_AUTHORITY.md` has the same trace with one more leading Tick; at these bounds BFS finds it
  without that Tick.
- **L1-mVer:** as section 5 there: the rebased shrink is chosen after `n1` bumped and left the pending state.
- **L1-mDec:** as section 5 there: `n1` leaves the pending state on a read showing only its base.
- **L1-P:** ShrinkSend(n1, n2), ShrinkChosen(n1), Promote(n2). With A-1 on, `n1` is already in degraded mode
  (`pdeg` TRUE) and still holds `n2`'s grant when `n2` is promoted outside S. So L1-F's PASS covers the window it
  was built for.

**What this shows.** At these bounds (3 nodes, MaxTerm 3, MaxVer 6, one restart), A-1 keeps at most one authority
holder across a missed degrading shrink and R7 case 1, also with operator promotion inside S and with R2′; each of
its three parts is needed. **What it does not show:** anything about logs or acknowledgements (section 6), Paxos
per-witness behaviour (L1-AS-2), real time (L1-AS-3), or larger bounds. A-1 stays a proposal; L-1 stays open until
Codex and the owner accept a correction.

## 6. Evidence boundary (Codex #180 section 4)

`L1Authority.tla` and `LiveGrant.tla` are independent models, and their assumptions are not composed. LiveGrant
models Shrink atomically; L1Authority has no replicated log. Neither establishes exclusive authority and
acknowledgement safety together under the joint protocol. If SNC keeps its custom protocol, final acceptance needs a
composition argument or a targeted integrated model covering authority, membership, acknowledgements and
destructive state transitions. A PASS of the L-1 runs is evidence for authority at these bounds only.
