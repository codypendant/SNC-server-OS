# Track A criteria: campaign dc1-tracka

**Status: thresholds APPROVED (P-G1, 2026-11-22), not yet FROZEN. Codex put the freeze on HOLD on 2026-11-28
(`docs/handoff/PR197_Codex_Review.txt`). It waits for Codex's review of execution plan revision 2. cody made the two decisions of its section 6 on
2026-11-28 (`docs/handoff/TRACK_A_O5_Budget_Owner_Decision.md`).**
- **Source:** `docs/DIRECTION_COMPARISON_PLAN.md` revision 3, section 3, and `docs/TRACK_A_EXECUTION_PLAN.md`.
- **Authorization:** cody gave P-A on 2026-11-27 (`docs/handoff/TRACK_A_PA_Owner_Decision.md`).
- **Freeze:** a later commit freezes this file after Codex's review, and records its own SHA here. It lands before
  any comparative run. After the freeze, any change is a new commit, reported next to the results.
- No comparative number existed when this was written.

## Scope
| Item | Value |
|---|---|
| Arms | SNC (`snc_rt` tree `1a6aef5` at `575f493`, plus an adapter), DBOS 3.6.0 (Python), Restate 1.8.1 with Rust SDK 0.13.0, Temporal 1.32.2 with sdk-python 1.36.1 (PostgreSQL persistence), LangGraph 1.2.24 with checkpoint-postgres 3.2.1 (durability `sync`) |
| Build order | DBOS, Restate, Temporal, LangGraph; all four built (execution plan AM-1, an amendment to the plan's early stop; rows unchanged; 12 agent-day cap kept) |
| SNC and O-5 | DECIDED by cody 2026-11-28, option A (execution plan section 3): the frozen SNC arm has no asynchronous event ingress, so O-5 is UNSUPPORTED. SNC is judged on O-1 to O-4, A09 and A10 are not run on SNC, and O-5 is counted against SNC's layer in criteria 1 and 3 |
| Durability | competitors: one copy with fsync before acknowledgement. PostgreSQL 18: `synchronous_commit = on`, `fsync = on`, no standby. Restate: RocksDB WAL fsync on (the default). SNC: AR-1's same-host synchronous standby, charged to SNC |
| Retention | any default that deletes history inside a run is raised by configuration, and the value is reported |
| Forbidden | engine source changes; LangGraph durability `async` or `exit`; Temporal's SQLite dev server; DBOS on SQLite; changing Restate code under a live deployment |

## Matrix and checker
| Item | Value |
|---|---|
| Matrix | `docs/DIRECTION_COMPARISON_TRACK_A_MATRIX.csv` (sha256 `73b285ff8bea1308…`): 17 points, 114 runs per arm, 94 S-M and 20 S-AP |
| Seeds | `seed(point, i, "tracka:<frozen SHA>")` |
| Guarantees | O-1 to O-5, as written in plan 3.3, judged from the service logs and the arm's `state` and `reconstruct` only |
| Quiesce | 10 s |
| Zombie release time T | 60 s; no takeover within T makes the run UNRESOLVED |
| Checker adequacy | hard gate, execution plan section 4: layers L1 (service logs), L2 (each arm's state, reconstruct and event interfaces) and L3 (one mutant per guarantee of each frozen arm), by an independent adequacy author; every fault is flagged, and every L3 mutant at every seed of each observable point; a miss makes the campaign UNRESOLVED |
| Invalid runs | repeated once with the same seed; a second invalid run at a point is UNRESOLVED; a violation seen in an invalid run counts |

## Line limits (plan 3.5)
| Item | Value |
|---|---|
| Formatter | `black` 25.11.0 (Python), `rustfmt` pinned with the Rust toolchain |
| Counted | logical lines that are not blank, not comment, and not only braces or punctuation; a line modified between step 1 and step 2 counts once; DDL, YAML and manifests that create N structures count at a factor of 1.0 against the host language's limits |
| N lines in the whole arm | at most 300 in Python, at most 450 in Rust |
| Total lines in the whole arm | at most 1,000 in Python, at most 1,500 in Rust |
| Classifiers | two, independent, labelling after the arm is frozen and before any cross-arm comparison; on disagreement a line is not N; agreement below 90% on lines either one calls N makes that arm's criterion-3 result UNRESOLVED |
| Not counted | shared services and the harness |

## Outcome rows (plan section 6, unchanged)
| Result | What it supports |
|---|---|
| SNC's arm fails a guarantee | Neither product direction, on this evidence |
| Any competitor arm meets O-1 to O-5 within L | SDK at most, over that engine |
| DBOS, Restate and Temporal were all built, and each needs more than L or an engine change | Agent runtime remains a candidate, subject to the product gate |
| Anything else | UNRESOLVED |

## Caps
| Item | Value |
|---|---|
| Agent effort | 12 agent-days after P-A |
| Run | 5 minutes per run (accepted by cody 2026-11-28) |
| Run set | 3 h per arm and step (accepted by cody 2026-11-28) |
| VM total | 30 VM hours for counted runs, adequacy, performance and any VM bring-up (accepted by cody 2026-11-28; the plan's 8 h assumed the early stop) |
| A cap hit | stops the item; what completed is reported and the rest is UNRESOLVED |

## Reported, not gated (plan 3.6)
Added latency net of stub and tool time at p50 and p99, for 1 agent and for 100 concurrent agents; bytes stored per
task; recovery time from a kill to the next external request; the operations rubric; and the behaviour after a code
change: DV-A, a new non-gating variant of G3's divergence probe (execution plan R-6). G3's own finding is unchanged.
