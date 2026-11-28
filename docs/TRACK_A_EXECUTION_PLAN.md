# Track A execution plan (PROPOSED revision 3, for Codex review)

## 0. Status

- **Authority:**
  - cody gave P-A on 2026-11-27 ([handoff/TRACK_A_PA_Owner_Decision.md](handoff/TRACK_A_PA_Owner_Decision.md));
  - the design is [DIRECTION_COMPARISON_PLAN.md](DIRECTION_COMPARISON_PLAN.md) section 3 (revision 3, approved at
    P-G1);
  - AR-1's G3 ended NARROW, which was accepted ([handoff/AR1_G3_Outcome_Accepted.md](handoff/AR1_G3_Outcome_Accepted.md)).
- **Revision 2** applied Codex's HOLD on revision 1 ([handoff/PR197_Codex_Review.txt](handoff/PR197_Codex_Review.txt)).
  **Revision 3** applies Codex's HOLD on revision 2 ([handoff/PR199_Codex_Review.txt](handoff/PR199_Codex_Review.txt)):
  - option A is no longer a reading of the outcome rows. The rows apply as written (section 3, evaluation A), and
    SNC's O-1 to O-4 comparison is a separately labelled diagnostic (evaluation B);
  - the adequacy gate takes Codex's five conditions for every behavioural mutation (section 4).
  Revisions 1 and 2 were merged in PRs #197 and #199 and stay in git history.
- **The freeze is on hold.** CRITERIA.md stays unfrozen, and no counted work starts, until Codex approves this
  revision. cody's decisions of 2026-11-28 are in section 6.
- **What does not change:** the design, the matrix, the limits of 3.5 and the outcome rows of section 6. There is no
  product pivot, no S1 runtime development, no change to SNC's engine, and no tuning of the SNC arm to win.
- **Labels:** FACT (read in this repository), EXT (external, cited), INF (inference), UNRESOLVED and PROPOSED.

## 1. Already fixed by the approved plan [FACT]

- **Scenarios:**
  - S-M: a model call, then three tools (T-I, T-F, T-N), a memory write, and completion;
  - S-AP: S-M with an asynchronous approval;
  - deterministic causality from the stub's reply id.
- **Guarantees:** O-1 to O-5, judged only from the service logs and from each arm's `state(agent)` and
  `reconstruct(task)` (plan 3.3).
- **Matrix:** 17 points and 114 runs per arm (`docs/DIRECTION_COMPARISON_TRACK_A_MATRIX.csv`, sha256 `73b285ff…`).
  - S-AP appears only at A09 (kill at approval) and A10 (event before the wait), with 10 seeds each.
- **Timings:** a 10 s quiesce, and T = 60 s for a zombie takeover.
- **Process:**
  - step 1 is written idiomatically by a separate implementer, strengthened by a second reviewer, then frozen;
  - step 2 is the smallest additions that make it pass.
- **Line limits ("within L"):**
  - N lines: at most 300 in Python and 450 in Rust;
  - total lines: at most 1,000 in Python and 1,500 in Rust.
- **Classifiers:** two, labelling each line independently, with 90% agreement required.
- **Rows:** the section 6 rows are ordered, and the first row that matches wins.
- **Effort cap:** 12 agent-days after P-A.

## 2. Amendment and readings

- **AM-1. Amendment to the plan's early-stop rule (section 7).**
  - **Change:** all four competitor arms are built, in the order DBOS, Restate, Temporal, LangGraph. Codex supported
    this; the basis is cody's P-A, which answered a question naming all four.
  - **Unchanged:** the outcome rows. The first competitor in build order that meets O-1 to O-5 within L still gives
    "SDK at most", and the later arms add evidence for criteria 1 and 3 only.
  - **Cap:** the 12 agent-day limit is kept. If it is reached, the arms not finished are reported as not built, in
    build order, the rows apply to what was built, and the cap is not extended without cody.
- **R-2. SNC's arm** is `snc_rt` at the G3 frozen tree (`575f493`, tree `1a6aef5`), plus a read-only adapter that
  serves `state` and `reconstruct` from its existing read interface. The adapter is counted and reported, and no limit
  applies to it.
- **R-3. The DBOS arm is written fresh,** by an independent implementer working from DBOS's own docs and idioms
  (approved by Codex).
  - The implementer is not shown G3's `plain2/`, AR-1's checker, or SNC's runtime.
  - G3's numbers appear in the report as context only.
- **R-4. Line counting is plan 3.5's rule.** It counts whole-arm N lines and total lines, and a line modified between
  steps counts once. AR-1's rule of counting only added lines does not apply.
- **R-5. A point an arm cannot reach** is UNRESOLVED for that arm at that point, not a failure (plan section 6).
- **R-6. The divergence probe is a new, non-gating variant, DV-A.** G3's probe used SNC's internal `B/b3:T-I`
  boundary, which other arms cannot reach.
  - **What it does:** a kill right after T-I's reply is flushed (A06 with a delay of 0 ms), then recovery with the
    T-I and T-F steps swapped and the version pinned. It runs on seed 1, in each arm.
  - **Reporting:** it goes in the operations rubric only.
  - G3's divergence finding is kept as it is and is not re-run.

## 3. SNC and O-5: the blocker (Codex section 2)

**Finding (FACT, read in the frozen tree `575f493`).**
- **How approval works:**
  - `policy_phase` commits `AP-req`, then `approve()` makes a synchronous `GET /approve` to the shared approver
    (`experiments/ar1/ar1/rt.py:713-733`);
  - the reply is committed as `AP-dec`;
  - on recovery, a still-requested approval is fetched again by the same `GET`.
- **What is missing:**
  - there is no ingress: no HTTP listener, no `DBOS.recv` and no event table;
  - nothing in the arm receives a decision that is pushed to it.
- **Where the event's durability lives:** the synchronous approver answers deterministically from the seed and stores
  nothing for the arm. So in AR-1, the "event" was durable only because the harness could answer the same question
  again, not because the arm stored it.

**Conclusion.**
- The frozen SNC arm does not support O-5.
- An adapter cannot add it without becoming new runtime functionality: a durable inbox, or polling a harness-held
  decision. In both cases the durability would come from new code or from the harness, which R-2 does not allow.
- This is recorded as **SNC: O-5 UNSUPPORTED (no asynchronous event ingress)**. It is not a fault-injection failure.

**Two evaluations (Codex review of PR #199).** The results are reported as two explicitly separate evaluations.

**A. Formal Track A outcome.**
- The O-1 to O-5 contract and the section 6 rows apply exactly as written. Nothing is reinterpreted.
- SNC's O-5 is recorded as **UNSUPPORTED**. A09 and A10 cannot be executed on SNC, because the arm has nowhere to
  receive the event. They are recorded as O-5 not met, not as UNRESOLVED and not as pass.
- So row 1 applies: the frozen SNC candidate does not meet the complete guarantee set, and the formal outcome is
  **"neither product direction, on this evidence"**. This is known before any run [FACT, from the code].
- **Scope of that conclusion:** it is about the frozen candidate of PR #185. It is not proof that an agent SDK or
  runtime built on SNC cannot be viable, and it is not a finding against SNC's engine.
- The competitor arms are still built, run and reported (AM-1). Under the ordered rows they cannot change the
  formal outcome.

**B. Diagnostic comparison (labelled DIAGNOSTIC; it does not replace or override A).**
- SNC and every competitor are compared on O-1 to O-4, at the same points (the 94 S-M runs), with the same checker.
- Each competitor's O-5 (A09 and A10, 20 runs) is evaluated and reported separately. SNC's column reads UNSUPPORTED.
- For each competitor arm, the report states whether it meets O-1 to O-5 within L, and whether it meets O-1 to O-4
  within L. These are evidence for criteria 1 and 3, and they go to cody with the recommendation (W7).
- If SNC fails O-1 to O-4 in a counted run, that is reported in B as well. It changes nothing in A.

**What was not done, and why.**
- No SNC event ingress is built, and SNC's frozen runtime is not changed to hide the gap. Building one is new S1
  runtime work, which is not authorized.
- No amendment to the rows is proposed. Using option A to decide the formal outcome would be an amendment to the
  preregistered rules and would need cody's explicit authorization before the freeze. It is not requested.

## 4. Checker adequacy: a hard gate before counted runs

The gate checks every arm's own observable interfaces, not only SNC (Codex section 1).

- **Who writes it:** an adequacy author who has not written the checker or any arm. That author builds independent
  violations of O-1 to O-5 at three layers.
- **The three layers:**

| Layer | How it is built | What it shows |
|---|---|---|
| L1, the service logs | Synthetic edits to a passing run's logs: a second T-I effect, a T-N request after an unknown outcome, an accepted stale T-F token, a tool request after a denial, a model re-call after a downstream request used the reply, a tool request outside the single-execution set | the log checks of O-1, O-2 and O-3, for any arm |
| L2, each arm's interfaces | A proxy in front of each arm's `state`, `reconstruct` and event ingress, answering from a recorded passing run with one fault each: a wrong memory value, a dropped attempt, a wrong model-input hash, a missing policy rule version, an approval event dropped or delivered twice, a decision recorded without its event | the checks of O-2, O-4 and O-5 read through every arm's own interface shape |
| L3, behaviour | One mutant per guarantee of each frozen step-1 arm, written by the adequacy author: no idempotency key on T-I (O-3), the model called again on recovery (O-1), a second writer after takeover (O-2), an attempt not recorded (O-4), the approval wait skipped or the event dropped (O-5) | that real misbehaviour reaches the logs and the interfaces as the checker expects |

- **Pass:**
  - every L1 and L2 fault is flagged, with the clause it breaks;
  - every L3 mutant is flagged at every seed of every point where it is observable;
  - SNC has no O-5 mutant, because it has no event ingress (section 3). Its L2 proxy covers `state` and
    `reconstruct` only.
- **Conditions for every L3 mutant, in every arm (Codex review of PR #199):**
  - **Positive control:** the unmodified frozen step-1 arm passes that guarantee at that point and seed. A point
    and seed without a passing control gives the mutant no credit there.
  - **The mutation ran:** evidence independent of the checker's verdict shows the mutated code path executed and
    produced the intended violation, for example a marker the mutant writes to its own log, or the service-side
    count of duplicate effects.
  - **A new violation is detected:** the checker flags a clause of that guarantee that the control did not flag,
    from the service logs or the arm's interfaces.
  - **No borrowed credit:** a mutant flagged only for a clause that the unmodified step-1 arm already fails is not
    credited.
  - **Unexercised:** a mutant that is unreachable, or never executes at any seed of a point, is UNRESOLVED for
    that point, and is reported.
- **A miss** makes Track A UNRESOLVED until the checker is fixed and reviewed again.
- **Order:** the adequacy layers L2 and L3 for an arm run after that arm's step 1 is frozen, and before its counted
  matrix run.

## 5. Work order, budget and stops

| Step | Work | Gate to the next step |
|---|---|---|
| W0 | Codex review of this revision; cody's two decisions (section 6); then CRITERIA.md is frozen | all three |
| W1 | Services made generic (plan 3.2): asynchronous approver with a per-arm ingress, owner service, T-F integer token. Driver and matrix runner, reusing G3's patterns (freeze check, manifest, restart-safe rows, persisted caps, invalid run repeated with the same seed, cap and harness error over verdict, stray-process cleanup) | unit tests; a services smoke run |
| W2 | O-1 to O-5 checker (separate implementer) and adequacy layer L1 | adversarial review; L1 passes |
| W3 | SNC adapter (R-2); bring-up on 2 seeds per point, uncounted | SNC reaches every point it supports |
| W4 | Arms in build order: DBOS (Python), Restate (Rust SDK 0.13), Temporal (Python), LangGraph (Python). For each arm: an implementer and a strengthening reviewer, then step 1 frozen, then L2 and L3, then the matrix, then step 2 if needed and the shortest correct version, then step 2 frozen, L2 and L3 again, and the matrix. The classifiers label each frozen arm | step 1 frozen before any matrix run |
| W5 | Counted matrix runs on the dedicated VM, from one frozen commit per run set | freeze check |
| W6 | Performance and operations (plan 3.6), and DV-A; reported only | none |
| W7 | Report (plan 5 and 6) with a recommendation to cody | Codex review |

- **Where things run:**
  - development, bring-up and the adequacy layers run in the cloud session where its 4 cores allow;
  - counted runs and performance runs go on one dedicated VM (8 vCPU, 32 GB, local SSD);
  - versions are pinned as in `docs/research/DIRECTION_COMPARISON_SOURCES.md`, with the binaries hash-locked.
    Temporal uses PostgreSQL persistence, never the SQLite dev server;
  - UNRESOLVED: whether the Temporal and Restate server binaries can be downloaded through this session's proxy. If
    they cannot, that arm's bring-up also moves to the VM, and the budget below already allows for it.
- **The VM budget, for cody to accept (section 6, item 2).** The plan's "up to 8 h of matrix runs" assumed the early
  stop. With AM-1 the estimate is [HYP]:

| Item | Run sets | Hours each | VM hours |
|---|---|---|---|
| Counted matrix: SNC (the 94 S-M runs; A09 and A10 cannot run on it), and step 1 plus step 2 for each of four competitors | at most 9 | about 1.5 | about 14 |
| Adequacy L3 on the VM, if it does not fit the cloud session | 5 arms | about 1 | about 5 |
| Performance (1 agent and 100 concurrent agents) and operations, 5 arms | 5 | about 1 | about 5 |
| Bring-up of arms that cannot run in the cloud session | up to 2 | about 1 | about 2 |
| **Proposed maximum** | | | **30 VM hours in total**, at most 3 h per run set, and 5 min per run |

- **At the caps:**
  - a run that hits 5 minutes is UNRESOLVED;
  - a run set that hits 3 h is UNRESOLVED for that arm and step;
  - when the 30 h total is reached, everything not run is UNRESOLVED and is reported in build order;
  - there is no extension without cody.
- **Stop rule for SNC:** the SNC arm is not fixed inside Track A. An O-1 to O-4 failure is reported in the
  diagnostic (section 3, B). The formal outcome is already row 1 (section 3, A).

## 6. Owner decisions

cody said yes on 2026-11-28 to both recommendations of revision 2
([handoff/TRACK_A_O5_Budget_Owner_Decision.md](handoff/TRACK_A_O5_Budget_Owner_Decision.md)):
1. **SNC and O-5.** After Codex's review of PR #199, the coordinator's reading of that yes is that it covers SNC being
   judged on O-1 to O-4 as the diagnostic B of section 3, with O-5 recorded as UNSUPPORTED. It is not an amendment to
   the rows, and none is requested. If cody wants option A to decide the formal outcome, that is a separate
   amendment, and it needs cody's explicit yes before the freeze.
2. **The VM budget.** 30 VM hours in total, at most 3 h per run set and 5 min per run, and the 12 agent-day cap. These
   are hard caps, not estimates, and they are not extended automatically.

## 7. Questions for Codex

1. Does section 3's split into the formal outcome A and the diagnostic B settle the blocker?
2. Do section 4's five conditions apply Codex's clarification correctly to every arm?
