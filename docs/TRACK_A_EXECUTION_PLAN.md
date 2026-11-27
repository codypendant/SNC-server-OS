# Track A execution plan (PROPOSED, for Codex review)

## 0. Status

- **Authority:**
  - cody gave P-A on 2026-11-27 ([handoff/TRACK_A_PA_Owner_Decision.md](handoff/TRACK_A_PA_Owner_Decision.md));
  - the design is [DIRECTION_COMPARISON_PLAN.md](DIRECTION_COMPARISON_PLAN.md) section 3 (revision 3, approved at
    P-G1);
  - AR-1's G3 ended NARROW, which was accepted ([handoff/AR1_G3_Outcome_Accepted.md](handoff/AR1_G3_Outcome_Accepted.md)).
- **What this document adds:** only how Track A is carried out, and the readings it needs. Section 3's design, the
  matrix, the limits of 3.5 and the rows of section 6 are not changed.
- **Order:**
  1. Codex reviews this plan and `bench/results/dc1-tracka/CRITERIA.md`, through cody.
  2. CRITERIA.md is frozen.
  3. Work starts.
- **Rules that still hold:** no product pivot, no S1 runtime development, no change to SNC's engine, and no tuning of
  the SNC arm to win.
- **Labels:** FACT (read in this repository), EXT (external, cited), INF (inference), UNRESOLVED and PROPOSED.

## 1. Already fixed by the approved plan [FACT]

- **Scenarios:**
  - S-M: a model call, then three tools (T-I, T-F, T-N), a memory write, and completion;
  - S-AP: S-M with an approval;
  - deterministic causality from the stub's reply id.
- **Guarantees:** O-1 to O-5, judged only from the service logs and from each arm's own `state(agent)` and
  `reconstruct(task)` (plan 3.3).
- **Matrix:** 17 points and 114 runs per arm (`docs/DIRECTION_COMPARISON_TRACK_A_MATRIX.csv`, sha256 `73b285ff…`).
- **Timings:** a 10 s quiesce, and T = 60 s for a zombie takeover.
- **Process:**
  - step 1 is written idiomatically by a separate implementer, strengthened by a second reviewer, then frozen;
  - step 2 is the smallest additions that make it pass.
- **Line limits ("within L"):**
  - N lines: at most 300 in Python and 450 in Rust;
  - total lines: at most 1,000 in Python and 1,500 in Rust.
- **Classifiers:** two, labelling each line independently, with 90% agreement required.
- **Rows:** the section 6 rows are ordered, and the first row that matches wins.

## 2. Readings this plan needs (fixed before any result)

- **R-1. All four competitor arms are built.** cody's P-A answered a question that named DBOS, Restate, Temporal and
  LangGraph. So the plan's early stop (section 7) no longer cuts the later arms.
  - The section 6 row is still decided by the plan's rule: the first competitor in build order that meets O-1 to O-5
    within L gives "SDK at most".
  - The later arms add evidence for criterion 1 (what existing systems solve) and criterion 3 (what SNC provides
    that they cannot readily reproduce). They cannot move the row.
- **R-2. SNC's arm** is `snc_rt` at the G3 frozen tree (`575f493`, tree `1a6aef5`), plus an adapter that serves
  `state` and `reconstruct` from its existing read interface.
  - The plan's words "as closed in PR #185" are read as this tree, because it is the code G3 measured.
  - The adapter is counted and reported, but no limit applies to it.
- **R-3. G3's plain DBOS step 2 is not Track A's DBOS arm.** It was written against SNC's read interface and the AR-1
  checker's clauses. It uses SNC-shaped commit-token rows, which would be N structures here. It was not written from
  DBOS's docs by an implementer who had not seen SNC.
  - Track A's DBOS arm is written fresh, by a new implementer who is not shown `plain2/`.
  - G3's numbers are cited in the report as context only.
- **R-4. Line counting is plan 3.5's rule, not AR-1's.** It counts whole-arm N lines and total lines after `black` or
  `rustfmt`, and a line modified between steps counts once. AR-1's rule of counting only added lines does not apply.
- **R-5. Unreachable points.** An arm that cannot reach a boundary has its own run limit hit at that point, and the
  run is UNRESOLVED. It is not a failure.
  - Section 6 already maps "an arm not built, a criterion-3 result UNRESOLVED, a cap hit" to UNRESOLVED.
  - A point no arm can reach is reported and is not a defect of any arm [INF: none expected, since every point is
    triggered by a service].
- **R-6. Code-change behaviour is reported.** G3's divergence probe showed that DBOS reported nothing when steps were
  reordered across recovery (FACT, G3_REPORT).
  - Each arm's behaviour after a code change is recorded in the operations rubric (plan 3.6, "operator actions after
    a code change").
  - It uses the same probe: T-I and T-F swapped between an A02 kill and recovery, seed 1 only.
  - It is reported only, not gated.

## 3. Lessons from AR-1 that are applied (each was a real defect or a near miss) [FACT]

- **Checker adequacy before any counted run (PROPOSED addition; it changes no threshold).**
  - Each guarantee gets at least one mutation. The mutations are applied to the SNC arm, as in AR-1 7.3, and to the
    frozen DBOS step-1 arm where it passes the guarantee.
  - All seeds at the points where a mutation is observable must be flagged. A miss makes Track A UNRESOLVED until
    the checker is fixed and reviewed again.
  - Proposed mutations:
    - O-1: re-call the model although a downstream request used the reply;
    - O-2: a second writer after takeover;
    - O-3: T-I sent with a new key; T-N retried automatically; T-F's stale token not rejected;
    - O-4: drop one attempt from `reconstruct`;
    - O-5: drop an approval posted before the wait.
- **An independent checker author** writes O-1 to O-5 from the plan text and the service logs alone, and reviews no
  arm code. Two adversarial reviews: one on the checker, one on the driver.
- **The driver reuses G3's tested patterns:**
  - a freeze check (clean tree, frozen SHA, `black --check`) and a manifest;
  - results that survive restarts, with phase caps persisted;
  - an invalid run repeated once with the same seed, and a violation seen in an invalid run kept;
  - a cap or harness error taking precedence over a verdict;
  - stray server processes killed with `pkill -f -- <pattern>`;
  - partial rows cut off after a power loss.
- **Seeds:** `seed(point, i, "tracka:<frozen SHA>")`. The coverage of release orders is reported at freeze; seeds are
  never chosen.
- **The VM:**
  - the cards are pasted from cody's own SSH terminal, because a cloud session cannot SSH out;
  - a size the provider does not offer is recorded as a deviation before the run;
  - the cards avoid `set -e`.

## 4. Work order, gates and stops

| Step | Work | Gate to the next step |
|---|---|---|
| W0 | Codex review of this plan and CRITERIA.md; then freeze CRITERIA.md | cody relays Codex's approval |
| W1 | Services: AR-1's services made generic (plan 3.2), with an asynchronous approver and an ingress per arm, an owner service, and T-F's integer token. Driver and matrix runner | unit tests; a services smoke run |
| W2 | O-1 to O-5 checker (separate implementer) and the adequacy mutations | adversarial review; adequacy passes on SNC |
| W3 | SNC adapter (R-2); bring-up on 2 seeds per point, uncounted | SNC reaches every point it can, or each gap is explained |
| W4 | Competitor arms in build order: DBOS (Python), Restate (Rust SDK 0.13), Temporal (Python), LangGraph (Python). For each: implementer, then strengthening reviewer, then step 1 frozen, then matrix, then step 2 if needed, then the shortest correct version, then step 2 frozen, then matrix. Classifiers label each frozen arm | each arm's step 1 is frozen before it runs anything in the matrix |
| W5 | Counted matrix runs on the dedicated VM, one card, all arms, from one frozen commit per run set | the freeze check |
| W6 | Performance and operations (plan 3.6), reported only | none |
| W7 | Report `docs/DIRECTION_COMPARISON_REPORT.md`, Track A part (plan 5 and 6), with a recommendation to cody | Codex review |

- **Where things run:**
  - development and uncounted bring-up run in the cloud session;
  - counted matrix runs go on one dedicated VM (plan 7: 8 vCPU, 32 GB, local SSD).
  - Each engine is installed at the pinned versions in `docs/research/DIRECTION_COMPARISON_SOURCES.md`:
    - DBOS 3.6.0 on PostgreSQL 18;
    - Restate server 1.8.1 with Rust SDK 0.13.0;
    - Temporal server 1.32.2 with PostgreSQL persistence, never the SQLite dev server, and sdk-python 1.36.1;
    - LangGraph 1.2.24 with checkpoint-postgres 3.2.1 and durability `sync`.
  - The binaries are hash-locked.
  - UNRESOLVED: whether the Temporal and Restate server binaries can be downloaded through this session's proxy.
    If they cannot, bring-up of that arm also moves to the VM.
- **Caps:**
  - plan 7 sets 12 agent-days after P-A;
  - building all four arms (R-1) may not fit. If the cap is hit, the arms not finished are reported as not built and
    UNRESOLVED, in build order, and the row rule still applies to what was built;
  - matrix run cap (PROPOSED): 5 minutes per run and 4 hours per arm and step. The plan's "up to 8 h of matrix runs"
    assumed the early stop.
- **Stop rule for SNC:** if SNC's arm fails a guarantee, the first row of section 6 applies ("neither product
  direction, on this evidence"). It is reported, and the SNC arm is not fixed inside Track A.

## 5. Questions for Codex

1. Is R-1 (build all four arms, rows unchanged) the right reading of cody's P-A?
2. Should the proposed adequacy step (section 3) be added before counted runs?
3. Is it acceptable that the matrix caps (section 4) replace the plan's 8 h total?
4. Is R-2's choice of the G3 frozen tree for SNC's arm right?
