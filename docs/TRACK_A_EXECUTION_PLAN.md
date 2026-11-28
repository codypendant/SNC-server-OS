# Track A execution plan (PROPOSED revision 2, for Codex review)

## 0. Status

- **Authority:**
  - cody gave P-A on 2026-11-27 ([handoff/TRACK_A_PA_Owner_Decision.md](handoff/TRACK_A_PA_Owner_Decision.md));
  - the design is [DIRECTION_COMPARISON_PLAN.md](DIRECTION_COMPARISON_PLAN.md) section 3 (revision 3, approved at
    P-G1);
  - AR-1's G3 ended NARROW, which was accepted ([handoff/AR1_G3_Outcome_Accepted.md](handoff/AR1_G3_Outcome_Accepted.md)).
- **Revision 2** applies Codex's HOLD on revision 1 ([handoff/PR197_Codex_Review.txt](handoff/PR197_Codex_Review.txt)).
  Revision 1 was merged in PR #197 and stays in git history.
- **The freeze is on hold.** CRITERIA.md stays unfrozen, and no counted work starts, until both of these happen:
  - Codex approves this revision;
  - cody decides the two owner items in section 6.
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

**Effect on the outcome rows.**
- Row 1 reads "SNC's arm, as frozen in PR #185, fails a guarantee → neither product direction, on this evidence".
  Applied literally, it would fire before any competitor is judged, so the experiment's answer would be decided by a
  missing interface before it runs. That is the predetermined conclusion Codex warns against.
- The rows cannot be changed without cody. The options go to cody (section 6, item 1):

| Option | What happens | Rows |
|---|---|---|
| **A (recommended).** SNC is judged on O-1 to O-4. O-5 is recorded as SNC UNSUPPORTED, and the S-AP points A09 and A10 are not run on SNC | Row 1 fires only on an O-1 to O-4 failure. O-5's evidence is competitor against competitor. SNC's missing ingress is a FACT row under criteria 1 and 3, where it counts against SNC's layer | rows unchanged; row 1's "a guarantee" is read as "a guarantee the frozen arm implements", with O-5 recorded as unsupported |
| B. Row 1 applied literally | Track A's row is "neither product direction". The competitor arms are still built and reported (AM-1), but they cannot change the row | unchanged |
| C. An SNC event ingress is built first | New S1 runtime functionality, which needs cody's explicit authorization and its own review. Then all 114 runs | unchanged |

- **Why A:** it measures what the frozen arm actually does, and it records the gap where it belongs. It also avoids
  both a predetermined row (B) and new S1 runtime work (C). It is the strictest reading against SNC that is still
  informative: SNC gets no credit for O-5.

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
  - for SNC, O-5 is not applicable under option A.
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
| Counted matrix: SNC (94 runs under option A), and step 1 plus step 2 for each of four competitors | at most 9 | about 1.5 | about 14 |
| Adequacy L3 on the VM, if it does not fit the cloud session | 5 arms | about 1 | about 5 |
| Performance (1 agent and 100 concurrent agents) and operations, 5 arms | 5 | about 1 | about 5 |
| Bring-up of arms that cannot run in the cloud session | up to 2 | about 1 | about 2 |
| **Proposed maximum** | | | **30 VM hours in total**, at most 3 h per run set, and 5 min per run |

- **At the caps:**
  - a run that hits 5 minutes is UNRESOLVED;
  - a run set that hits 3 h is UNRESOLVED for that arm and step;
  - when the 30 h total is reached, everything not run is UNRESOLVED and is reported in build order;
  - there is no extension without cody.
- **Stop rule for SNC:** if SNC fails O-1 to O-4, the first row applies and is reported. The SNC arm is not fixed
  inside Track A.

## 6. What cody decides before the freeze

1. **SNC and O-5 (section 3).** Recommended: **option A**. SNC is judged on O-1 to O-4, and O-5 is recorded as
   UNSUPPORTED and counted against SNC's layer in criteria 1 and 3.
2. **The VM budget (section 5).** Recommended: accept **30 VM hours in total**, at most 3 h per run set and 5 min per
   run. The 12 agent-day cap is unchanged.

## 7. Questions for Codex

1. Does section 3 settle the blocker, and is option A a fair reading to recommend?
2. Do the three adequacy layers of section 4 cover every arm's observable interfaces well enough?
3. Is DV-A correctly labelled and scoped (R-6)?
4. Is the 30-hour budget's breakdown sound?
