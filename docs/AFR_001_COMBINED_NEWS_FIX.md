# AFR-001: the `Combined.tla` `NewS` fix and the counted L-1 runs

Status: **preregistered before any run.** This covers Codex's review of #180 (`docs/handoff/PR180_Codex_Review.md`),
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

**Reachable in the affected configuration** [FACT, diagnostic run on the unfixed model, V3's constants, probe
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

Both are **withdrawn** as evidence until rerun. Documents that cite V3 (`docs/AFR_001_PROTOCOL_ACCEPTANCE_R2P_R9.md`
rows R2′ rule 1, P1-P5/P7 and I4; `docs/AFR_001_R2_AND_SINGLE_COPY.md` 2.5) are annotated with the rerun result.
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

RESULTS_PLACEHOLDER

## 6. Evidence boundary (Codex #180 section 4)

`L1Authority.tla` and `LiveGrant.tla` are independent models, and their assumptions are not composed. LiveGrant
models Shrink atomically; L1Authority has no replicated log. Neither establishes exclusive authority and
acknowledgement safety together under the joint protocol. If SNC keeps its custom protocol, final acceptance needs a
composition argument or a targeted integrated model covering authority, membership, acknowledgements and
destructive state transitions. A PASS of the L-1 runs is evidence for authority at these bounds only.
