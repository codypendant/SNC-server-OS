# Owner decisions on the PR #183 package (cody, 2026-11-21)

The coordinator put OD-P1 to OD-P8 to cody in the project chat on 2026-11-21 at 15:50:50 UTC. Each item was stated
in a limited form, under the heading "Eight decisions now need you, each on its own. I recommend yes to all, in the
limited form shown." cody replied to that post at 16:11:12 UTC:

> yes

**Reading.** The coordinator's reading, given to cody, who can correct it, is:
- yes to OD-P1 to OD-P7, exactly in the limited forms below;
- OD-P8 noted.

Nothing wider is decided. The items as cody saw them are quoted verbatim. The package
(`docs/AGENT_RUNTIME_DECISION_PACKAGE.md` section 8) gives the full text, and Codex's reviews
(`docs/handoff/PR183_Codex_Review.md`, `docs/handoff/PR183_Rev2_Codex_Review.md`) give the limits.

## Decided

- **OD-P1, product direction.** "The focused agent runtime becomes SNC's proposed first product, with enterprise
  governance as an optional later expansion. It authorizes nothing else."
  - SNC-ADR-PRODUCT-001 revision 2 is **accepted** in that form.
  - Enterprise-first development is not approved.
- **OD-P2, scope freeze.** "Lifts the validation freeze for the focused runtime only. Multi-tenancy and security stay
  frozen, and testing of the original database idea is paused, not ended, so its evidence stays valid."
  - `docs/EXPERIMENTAL_VALIDATION.md` section 3 no longer freezes the focused agent runtime (S1).
  - General-SNC falsification under section 14 is **suspended, not concluded**.
- **OD-P3, experiment tools.** "PostgreSQL and DBOS Python may be used in the AR-1 proof of concept only. They are
  not production dependencies, and Temporal stays the alternative."
  - The prior-art rule ("take ideas, do NOT adopt or embed") is unchanged for production.
- **OD-P4, record contract.** "The runtime's records and rules, including the corrected stale-call and model-call
  handling, become the design direction. Nothing is frozen."
  - It covers the S1 records (2.1), E-1, E-2, E-3 (lightweight), E-5, E-6, E-7, the corrected E-10 and E-11, and
    the 2.3 split.
  - `docs/SNC_GUARANTEES.md` is not revised.
- **OD-P5, product gate.** "Accepts the test of whether this is worth more than DBOS plus a little code: a demo on
  this project, tool comparison and buyer interviews. Pass thresholds get set before any results."
  - The thresholds are not set yet. The AR-1 preregistration proposes them for cody to set.
- **OD-P6, durability.** "Actions wait until the standby is seen to hold the data, as a direction only. No contract
  until the open item and seven checks pass."
  - It is a design direction, not a durability class.
  - OI-1 and C-1 to C-7 are still required.
  - It is not HA_DURABLE.
- **OD-P7, AR-1.** "Authorizes writing and reviewing the AR-1 preregistration only. No code until you approve that
  document."

## Noted

- **OD-P8, later decisions.** "Nothing to decide now. It lists what stays separate, such as high availability,
  enterprise design and the final language."

## Not decided

Each of the following needs its own explicit owner decision:
- AR-1 implementation, which needs cody's approval of the preregistration;
- PostgreSQL or DBOS as production dependencies;
- the final language, and whether SNC-ADR-RUST-001 applies to the runtime;
- any durability contract;
- the product-gate thresholds;
- running S1 on this project (7.2 step 1);
- S2 verification;
- any S3 enterprise design;
- any custom journal.

Research is kept, and nothing is deleted or expanded:
- AFR-001, L-1, LiveGrant, D-02, R-23 and O-FLOOR;
- the raft-rs and OpenRaft research;
- the per-core and NUMA work;
- the model evidence;
- the custom replication protocol is not developed further.
