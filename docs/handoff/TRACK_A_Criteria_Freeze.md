# Track A criteria freeze (2026-11-28)

- **Approval:** Codex approved execution plan revision 4 and the freeze in its review of PR #202
  ([PR202_Codex_Review.txt](PR202_Codex_Review.txt)). Its one wording correction (S-M has an ordinary policy decision;
  only asynchronous approvals and denials are outside it) was made on site in the freeze commit, as that review allows.
- **Owner decisions:** P-A ([TRACK_A_PA_Owner_Decision.md](TRACK_A_PA_Owner_Decision.md)); keep the original rules,
  and the hard caps ([TRACK_A_O5_Budget_Owner_Decision.md](TRACK_A_O5_Budget_Owner_Decision.md)).
- **Freeze commit:** `8a0f787304e6b9df2fdf3a2a93f24ab83f34db84`.
- **Seed string:** `seed(point, i, "tracka:8a0f787304e6b9df2fdf3a2a93f24ab83f34db84")`, the `<frozen SHA>` of
  CRITERIA.md, the same for every arm.

| File at the freeze commit | sha256 |
|---|---|
| `bench/results/dc1-tracka/CRITERIA.md` | `4802b60237d053f47fda6251502bc750cc93b8388a1e50d1765ddfedcfdd98fb` |
| `docs/TRACK_A_EXECUTION_PLAN.md` | `976a14c4f7985bd44ed2218407ff86a04c9d3899e27b5282aaa603ae5c6b88e3` |
| `docs/DIRECTION_COMPARISON_PLAN.md` | `b7d6c836e40414f0ec8b6c9f7fafb772f1f8a65ae5522ff6d7fe4ace48c93121` |
| `docs/DIRECTION_COMPARISON_TRACK_A_MATRIX.csv` | `73b285ff8bea1308941ef18d874db9409df40065519a80b6090c57a9fcf0f047` |

- **SNC arm:** `snc_rt` tree `1a6aef5` at `575f493`, unchanged.
- **Formal outcome:** row 1 for the frozen SNC candidate, known before any run (SNC O-5 UNSUPPORTED). The competitor
  arms and the O-1 to O-4 diagnostic are still built and run under P-A.
- **Caps:** 12 agent-days from P-A; 30 VM hours in total, 3 h per run set, 5 min per run. Hard caps.
- Any later change to these files is a new commit, reported next to the results.
