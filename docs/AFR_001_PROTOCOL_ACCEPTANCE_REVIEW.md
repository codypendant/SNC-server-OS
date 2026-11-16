# SNC-AFR-001: protocol-acceptance review

- **Status: PROPOSED.** The verdicts below are recommendations for cody and Codex to rule on. Nothing here accepts
  AFR-D-01, AFR-D-02 or AFR-D-03, and nothing authorizes implementation.
- **Trigger.** Codex's review of PR #145 (`docs/handoff/PR145_Model_Composition_Codex_Review.md`): "stop expanding
  the verification documents indefinitely. The next milestone should be a bounded, explicitly scoped
  protocol-acceptance review. We should determine which guarantees are sufficiently established to freeze and which
  require targeted experiments or further modeling."
- **Scope.** The protocol rules R1-R9, Claims L and D (`docs/AFR_001_SAFETY_ANALYSIS.md`), the guarantees that
  AFR-D-01 to AFR-D-03 unlock (`docs/SNC_GUARANTEES.md` section 14), and the open items Codex listed. Other
  guarantees are out of scope.
- **Method.** No new modelling and no new runs. Every verdict cites evidence that is already merged:
  - **SA**: `docs/AFR_001_SAFETY_ANALYSIS.md`;
  - **CL**: `docs/AFR_001_VERIFICATION_CLOSURE.md`;
  - **MC**: `docs/AFR_001_MODEL_COMPOSITION.md`;
  - **RES**: `spec/tla/RESULTS.md`, with run names as they appear there;
  - **CD**: `docs/AFR_001_CORE_DECISIONS.md`;
  - **SG**: `docs/SNC_GUARANTEES.md`.

  All model results are bounded: three nodes, three witnesses, at most three terms, and two or three entry ids, with
  drift folded into the lease length. Every completed Log run has one crash, and every completed Authority run has
  at most one restart and two fences (RES section 5). Labels are FACT, EXT, INF and HYP, as in
  `docs/ENGINEERING_PRINCIPLES.md`.
- **Review.** An independent adversarial QA reviewed the first draft and reported 13 findings. Twelve were confirmed
  and corrected here. One was partly wrong: `Safety` in `Authority.tla` includes `OneAuthority` (`Authority.tla:261`),
  so every completed Authority run did check it. The corrections are listed in section 7.

## 0. What "freeze" means here

Freezing a rule or guarantee makes its text the contract that the Rust architecture is designed against. It does not
claim the C++ meets the contract: most C++ statuses stay VIOLATED or ABSENT. It does not claim a proof, and it
authorizes no code. A frozen item changes only through a recorded revision with new evidence.

| Verdict | Meaning |
|---|---|
| **FREEZE (checked)** | A model checks the property within the stated bounds, and removing the rule produces a counterexample |
| **FREEZE (argued)** | No model covers it. It rests on an argument [INF] or on established prior art [EXT]. Freezing it accepts that argument |
| **+ ASSUMPTION** | Added to either of the above when it rests on a named assumption. The contract must state the assumption and the implementation must check it |
| **FIX TEXT** | The evidence supports the intent, but the current wording is wrong or incomplete. The correction is given; it freezes once corrected |
| **EXPERIMENT** | Only a measurement can settle it. The experiment is named, and so is what it unblocks |
| **MODEL** | One named, bounded check, tied to the later design step that needs it. None of these is open-ended |
| **OUT OF CONTRACT** | Excluded by design. The contract must say so |

## 1. Summary

**Recommended to freeze now:**
- **Checked:** R1, R3, R4, R6.0 and R6.2-R6.5, R7 inside S, R8, Claim L and Claim D. R3, R8 and Claim L carry the
  clock assumptions. Claim D carries the R7 outside-S exception.
- **Argued:** R2's READY rule, R5, R6.1, R6.6, G-28, G-32, G-33, G-34, and G-31 and G-37 as far as migration goes.

**Wording fixes before freezing:**
- **G-15:** the identity must be `(term, LSN)`, not "LSN". R6.3 and R6.4 reuse LSNs.
- **R9:** add the rule G-30 already states. A primary must not degrade, or acknowledge without a standby, while that
  standby's read lease may be live. The C++ refuses already (`src/core/shard.cpp:2254-2258`), and CD 5.2 intends it.
  Neither the protocol text nor either model has it.
- **G-14(a) and R6.4:** a member whose term history H is damaged must refuse, not reseed. Reseeding discards content
  it cannot interpret, which G-14(a) forbids.
- **G-35:** state that both outside-S branches of R7 can lose acknowledged writes.
- **G-46:** add a clock that counts through suspend, with VM pause out of contract.

**One experiment gates AFR-D-02:** X-2, the cost of a durable watermark. G-14(b) and the end-of-log rule cannot
freeze without it. It can run on the R640 before any implementation.

**Codex's open items:** none blocks the freeze, but each keeps a stated limit.
- **A2:** closed by a bounded argument from existing runs, without operator promotion.
- **A7 and A9:** move to the consensus-service design (AFR-D-13).
- **A8:** argued for fencing. On the standby-lease side it is the R9 fix, and its check needs writes in the model
  that has standby leases.
- **Unfinished runs:** eight runs did not finish. They are recorded gaps, not prerequisites. One consequence: R9 is
  checked only without a restart and without fencing.

## 2. Protocol rules and claims

| Item | Verdict | Evidence | Gap or assumption |
|---|---|---|---|
| **R1** ack set ordering: A ⊇ S, add to A before S, drop only after a shrink or a read at p's own term | FREEZE (checked) | C_term3 PASS (15,151,183 states). With the rule removed: C_growPack, C3_noDropTerm, L_B1, S_AinS (RES 2, 7) | — |
| **R2** promotion from S only, new S = {j} | FREEZE (checked) | C_term3, L_ids2 and L_term2 PASS. Relaxing it (`KeepS`) breaks reconciliation (L_keepS_kip101, RES 3) | — |
| **R2** READY recorded only after the barrier | FREEZE (argued) | No counterexample shows this rule is needed. S_ready_only passes without it, though with no operator promotion (RES 2). S_FixOp removes R7's READY-or-fenced condition, not this timing rule, and no run combines the two | Its necessity is UNRESOLVED. It is kept because it is cheap and R7 is defined in terms of READY |
| **R3** serve only after `learned + W` | FREEZE (checked) + ASSUMPTION | R3Lemma holds in every completed Authority run (RES 4). With the barrier removed: C_noBarrier, A_mut_barrier_core, M_barrier. Codex issue 1 is covered by the barrier alone (S_codex1) | Drift bound ρ, and a clock that counts through suspend (CLOCK_BOOTTIME; VM pause is out of contract) (SA 1, CD 5.1) |
| **R4** fencing by term | Step-down and grant check: FREEZE (checked). Durability of the persisted term: FREEZE (argued) | Without the step-down on `Learn`: M_learnStep. Without the grant term check: S_grantTerm (RES 7). A_restart_core PASS | `Restart` keeps T by construction (`Authority.tla:212`), so "durable" was never removed. Restart and fencing together did not finish (section 4, item 5) |
| **R5** check authority at answer time, after capturing the read | FREEZE (argued) + ASSUMPTION | SA 1 case 1. **Reads are not modelled** (CL 5). Same rule as Raft and etcd lease reads [EXT] | As R3 |
| **R6.0, R6.3-R6.5** term ownership, durable intent, reseed, incomplete flag | FREEZE (checked) | L_ids2 (MaxTerm 3, MaxIds 2) and L_term2 (MaxTerm 2, MaxIds 3) PASS. Each removed: L_R60, L_clear, L_ckintent, L_reseed, L_inc (RES 2) | One crash. `Log.cfg` at MaxTerm 3 **and** MaxIds 3 did not finish (L_fixed). Loss of H is not modelled (RES 5); see G-14(a) for that branch |
| **R6.1** drain before writing at the new term | FREEZE (argued) | No switch for it. It was never removed | — |
| **R6.2** KIP-279 iteration | FREEZE (checked), but necessary only once R2 is relaxed | With R2 in place, KIP-101's single answer changes nothing reachable (L_M1_ids2; L_M1_kip101 did not finish). With `KeepS`: L_keepS_kip101_recon fails, L_keepS_kip279_recon passes (RES 3) | Kept for robustness against R2 relaxations [EXT: KIP-279] |
| **R6.6** no silent fallback | FREEZE (argued) | Its only outcomes are a retry or R6.4, and R6.4 is modelled [INF] | — |
| **R7** operator promotion inside S | FREEZE (checked) | C_opInside_t3 PASS (16,505,916 states), with `Safety` and the first, stronger form of StaleAuthBounded (MC 1) | — |
| **R7** outside S from a degraded READY record (5b), or with every member of S confirmed fenced, with the boot read | Claim L: FREEZE (checked) + ASSUMPTION. **Claim D does not hold** | Q_opOut_t3_ids2, U_gate_f1, U_gate_f2_norestart, B_stale_gate PASS. Without READY-or-fenced or the boot read: S_FixOp, U_noBootRead (RES 1-2, MC 1). Write loss on the 5b branch: C_opOutsideDeg_safety. On the fenced branch it is asserted, not checked (`Combined.tla` has only the degraded branch) | Assumes the fencing action reports success only when the node has stopped. U_gate_f2 (two fences and a return) did not finish. See G-35 |
| **R7** case 5c (outside S, not degraded, S not confirmed fenced) | OUT OF CONTRACT | A_mut_5c reproduces two holders (RES 2). Every system checked treats it the same way (CL 2) | If offered at all, it is a labelled unsafe override |
| **R8** witness lease with W1 | FREEZE (checked) + ASSUMPTION | Without W1: S_W1 (`Authority.tla`). Without only W1's term clause, in `Combined.tla` with operator promotion: Q_w1Mut_opOut_ids0 (MaxIds 0). StaleAuthBounded holds with per-witness W1 and competing promotions (B_compete_ops_t3, B_stale_gate) (MC 1) | Drift bound; A9 (witness reads are linearizable); A7 (section 4) |
| **R9** a standby lease never outlasts p's authority | FIX TEXT, then FREEZE (checked) | Without R9: S_M2 (RES 2). R9 is checked only in A_all_norestart: no completed run has standby leases with a restart or with fencing | The missing rule: see G-30 |
| **Claim L** at most one node holds valid authority | FREEZE (checked) + ASSUMPTION | `OneAuthority` is part of `Safety` in both authority models, so it holds in every completed Authority and Combined run (RES 1, 7). StaleAuthBounded was checked in the B_, C_ and Q_ runs of MC 1 | Drift bound, a clock that counts through suspend, honest fencing, the boot read. No completed run combines every scenario |
| **Claim D** I1-I5: entry uniqueness, log matching, safe truncation, acknowledged writes kept, crash-atomic truncation | FREEZE (checked), except after R7 outside S | Log PASS at the completed bounds, and every Log switch is caught (R6.1 has none). Combined C_term3 PASS | One crash per run. Reads are not modelled (G-28) |

## 3. Guarantees that AFR-D-01 to AFR-D-03 unlock

The C++ status column is copied from SG and is unchanged by this review.

| Guarantee | C++ today | Verdict on the contract | Evidence and remaining work |
|---|---|---|---|
| **G-14(a)** detect damage and refuse; never truncate damaged content | VIOLATED | FIX TEXT (in R6.4) | R6.3 truncates only content that is intact and proven divergent, which is consistent with (a). But R6.4 reseeds when "`H_k` … cannot answer". If H_k is damaged, the member would discard content it cannot interpret, which (a) forbids. Not modelled (RES 5). **Fix:** when H_k is damaged, the member refuses with a named error, and a reseed becomes an explicit operator action. The C++ fix is AFR-S-1, with experiments E-01 and E-02 |
| **G-14(b)** restore every acknowledged write, or refuse | ABSENT | EXPERIMENT (X-2) | With replicas, I4 covers HA acknowledgements. A node without a replica needs a durable boundary: a watermark, self-describing flushes, or both (CD D-02). The choice waits on X-2. Single-copy windows weaken it (SA 3); the minimum set size `m` is cody's decision |
| **G-15** an LSN identifies one record for life | VIOLATED | FIX TEXT | R6.3 truncation and R6.4 reseeding refill LSNs that were durable on that member. The current wording forbids both. Proposed: "A log position `(term, LSN)` identifies one record for the life of a stream. A member reuses an LSN only through R6.3 truncation or R6.4 reseeding, and only for content that was never acknowledged; after an R7 outside-S promotion this exception covers acknowledged content too, as the contract states for R7." Kafka (epoch, offset) and Raft (term, index) work the same way [EXT, CD D-02]. **Evidence is partial:** `EntryUnique` compares nodes at one instant (`Log.tla:230`), and nothing in the models checks identity over history. How this interacts with a single node discarding a torn tail (G-14(b)) is UNRESOLVED |
| **G-28** read your acknowledged writes | VIOLATED (partition) | FREEZE (argued) + ASSUMPTION | It follows from Claim L, R5 and I4 [INF]. Reads are not modelled. The C++ conformance check is R-09's netns partition test, after implementation |
| **G-30** standby read under a valid lease | PARTIAL | FIX TEXT in R9; G-30's own wording is right | **A gap this review found.** G-30 says the primary "may not degrade or acknowledge without that standby while the lease is live". R9 does not say it, and neither model enforces it. In `Authority.tla`, `Shrink` sets A = {p} and leaves `sl` untouched (`:173-179`), and `NoStaleStandby` checks only holders at a newer term (`:256-257`). The C++ refuses to degrade until the standby's lease runs out (`src/core/shard.cpp:2254-2258`, CD F-5). **Proposed R9 addition:** "p does not remove `m` from A, and does not acknowledge without `m`, while a standby lease it granted to `m` may still be valid." The check is in section 4, item 3 |
| **G-31** one serving owner | PARTIAL | Two primaries of one group: FREEZE (checked) + ASSUMPTION, through Claim L. Two groups on one slot: FREEZE (argued). Refusing weak configurations: owner decision (AFR-D-15) | Two groups on one slot is governed by MS-1 to MS-4 (G-37). The lease interaction is argued in CD 5.2 [INF] and not modelled |
| **G-32** a newer term is durable and final | VIOLATED | FREEZE (argued) | R4. Its step-down and grant parts are checked; its durability is not mutated (R4 row). C++ fix: R-12 |
| **G-33** bounded authority | ABSENT | FREEZE (checked) + ASSUMPTION | R3, R8, R9; StaleAuthBounded (MC 1). Drift bound. The standby-lease part waits on the R9 fix |
| **G-34** status reports only provable authority | VIOLATED | FREEZE (argued) | Follows from G-33 by definition. No model. C++: R-14 |
| **G-35** promotion eligibility; operator promotion carries the expected term | PARTIAL | FIX TEXT | R2 and R7 as in section 2. The text must say that both outside-S branches of R7 can lose acknowledged writes, so the operator's explicit acknowledgement names that. E1-E11 must be restated for the in-sync-set model when AFR-D-03 is written up (SG 14.2 item 4) |
| **G-37** migration serving proof | HOLDS (tests) | FREEZE (argued) for the new clause; existing text unchanged | CD 5.2 adds "seal **and** expiry of any older-term source lease" [INF]. Not modelled. Check it when migration is designed for Rust |
| **G-46** no safety depends on clocks, except a rate-drift bound for leases | VIOLATED | FIX TEXT | The contract also needs a clock that counts through suspend, and must put VM pause out of contract (CD 5.1); the current text allows only the drift bound. It must also say how the bound is checked: at configuration time, and optionally by self-fencing on an observed offset [EXT: CockroachDB]. Beyond the bound, overlap is silent (SA 1) |

**AFR-D-01 (authority model).** cody chose the lease (2026-11-16). With the verdicts and text fixes above, D-01's
contract is ready for a ruling. Its costs are still unmeasured: X-3 needs a prototype, which needs an
implementation go.

**AFR-D-02 (end-of-log rule).** Not ready. The models assume a durable log, and nothing in them chooses between a
watermark and self-describing flushes. X-2 decides it.

**AFR-D-03 (group model).** The in-sync set plus a versioned witness record is what every model checks, and Claim D
holds for it. Two parts of SG 14.2 have no evidence and belong to later decisions:
- item 5, changing the witnesses themselves (AFR-D-11, AFR-D-13);
- item 6, cost (X-1).

## 4. Codex's open items, and the unfinished runs

| # | Item | Verdict | Reason and limits |
|---|---|---|---|
| 1 | **A2**: is `Combined.tla`'s single witness-lease timer equivalent to per-witness leases? | Closed by a bounded argument [INF]; no model work | In `Combined.tla`, `wl` and `pdeg` feed `Auth`, `WGrant` and `Rem`. Of these, only `Auth` gates log actions, and only `NewEntry` and `Ack` (`Combined.tla:161`, `:177`). With the authority check on `Ack` removed, `Safety` still passes exhaustively (C3_noAckAuth, MaxTerm 3, 20,989,156 states). `Log.tla` checks Claim D with any primary allowed to append (MC A4, `Log.tla:70`). The authority side is checked directly with per-witness W1 in `Authority.tla`. **Limits:** C3_noAckAuth ran without operator promotion; it still gates `NewEntry` by `Auth`, so the step to "any append" rests on A4, which is itself an argument; and the Log bounds (one crash) are smaller than Combined's. Equivalence is not proven |
| 2 | **A7**: lagging witnesses and overlapping consensus operations | MODEL, in the consensus-service design (AFR-D-13). It does not block freezing R8 | Per-witness acceptance already lets a minority of witnesses lag, even across a term (`Authority.tla:148-153`), and those runs pass. A lagging minority can grant to an old primary, but the holder needs a majority, which intersects the chosen version's acceptors (SA R8) [INF]. What is not modelled: grow, shrink and READY reaching witnesses at different times, and a promotion accepted by a minority and revived later. The bounded check is one action in `Authority.tla` that applies those set changes per witness |
| 3 | **A8**: fencing and standby leases interacting with acknowledgements | Fencing: FREEZE (argued), bounded. Standby leases: the R9 fix, plus a MODEL check when standby reads enter the Rust scope | **Fencing.** `Log.tla` lets any primary append and models crashes, so stopping a node adds no behaviour Claim D was not checked against [INF]. Limits: every completed Log run has one crash, and a fence, return and restart are at least two stops. The write loss on R7's fenced branch is asserted, not checked. The boot read sets T without `Learn`'s reset (`Authority.tla:225`), and R6 positioning is what covers the log [INF]. **Standby leases.** The check cannot be stated in `Authority.tla`, which has no writes. It needs standby leases added to `Combined.tla`, or writes added to `Authority.tla`. It is needed only when standby reads, a weaker read under G-29, enter the Rust scope |
| 4 | **A9**: nodes learn only chosen records | FREEZE (argued) + ASSUMPTION; verified as a requirement on the consensus service (AFR-D-13) | It holds if witness reads are linearizable [INF]. Today's witness Paxos excludes F9 only by a launch-time guard, which SG treats as unacceptable for production (AFR-D-13, R-19). The requirement belongs in the D-13 design, with a linearizability test of witness reads under partition |
| 5 | **Unfinished runs.** Eight runs did not finish: U_gate_f2, A_term2_full, A_term3_full, A_restart, A_fence, L_fixed, S_codex1_ctrl and L_M1_kip101 (RES 1-3) | Recorded gaps. Optional: rerun the committed models, unchanged, on a larger machine | No completed run combines a restart, fencing, operator promotion and standby leases. In particular, standby leases (R9) are checked only without a restart and without fencing. The unfinished runs found no violation in 20-149 M states, which is weak evidence. A rerun on the R640 (376 GiB) with a preset time limit would need no modelling. It needs cody's go, because the R640 is a shared resource |
| 6 | No refinement mapping between the three models | Accepted as a limit, not pursued | Codex: proceed "without pretending the entire distributed protocol has been formally proven". The C++/Rust differential harness (SG section 15) checks the implementation against the contract |

## 5. Owner decisions this review does not make

- Whether to freeze the items in section 1, and whether the five wording fixes are right.
- The minimum set size `m` and how single-copy windows are reported (SA 3).
- X-1 and X-2 thresholds, set before any run (SA 4).
- Whether standby reads are in scope for the first Rust milestone. That decides when the G-30 check is needed.
- AFR-D-15: refuse weak configurations, or accept them with a flag.
- Whether to spend R640 time on the optional reruns (section 4, item 5).

## 6. What this review does not establish

- No proof. Every model result is bounded, and every argument marked [INF] is unchecked.
- Nothing about the C++. Its statuses are unchanged.
- Nothing about cost. X-1 to X-3 have not run.
- No implementation, model change or experiment is authorized by this document.

## 7. Corrections from the independent QA

The QA reported 13 findings on the first draft (9 major, 4 minor, no blocker).

| Finding | Correction |
|---|---|
| Items without model evidence were marked FREEZE | "FREEZE (argued)" separated from "FREEZE (checked)" (R5, R6.1, R6.6, G-28, G-32, G-34, G-31 migration part, G-37) |
| READY's necessity was attributed to S_FixOp, which removes R7's condition | R2 split. READY's necessity is UNRESOLVED |
| "Each R6 sub-rule removed" was wrong | R6.1 is unmutated. R6.2 is necessary only once R2 is relaxed |
| StaleAuthBounded claimed in every completed run | Narrowed to the runs that checked it. **Partly rejected:** `OneAuthority` is in `Authority.tla`'s `Safety` (`:261`), so it was checked everywhere |
| Only two unfinished runs named | All eight listed. R9's coverage limit stated |
| The proposed G-15 text was false under R7 outside S and left out R6.4 | Rewritten. Evidence marked partial, and the torn-tail interaction left UNRESOLVED |
| G-14(a) conflicts with R6.4's reseed on a damaged H | Now FIX TEXT |
| G-30: wrong C++ line, impossible check, R9 listed twice | Line corrected to `shard.cpp:2254-2258`; check moved to a model with writes; R9 only under fixes |
| A2 overclaimed | Now a "bounded argument", with its limits listed |
| A8 fencing called closed | Now "argued, bounded", with its limits listed |
| G-35 and G-46 marked FREEZE | Now FIX TEXT |
| R4's durability never mutated | R4 split |
| Wrong run name for W1, and imprecise A7 wording | Q_w1Mut_opOut_ids0 named, with its model and scope. A7 reworded |
