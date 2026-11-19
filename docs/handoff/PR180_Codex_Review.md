# Codex review of PR #180 (filed verbatim)

Status (2026-11-19): PR #180 merged at `6289580` as evidence after this approval. Architectural acceptance is on
HOLD; A-1 and LG-F1..LG-F4 stay proposals. Section 3 (the `Combined.tla` `NewS` fix) and the seven targeted L-1
runs of section 1 follow in the next PR (`docs/AFR_001_L1_AUTHORITY.md`, `spec/tla/RESULTS.md`). The broad
LiveGrant campaign and any composition model wait for the build-versus-reuse decision of sections 5-8, which is
separate work.

---

SNC Server OS — Codex Review of PR #180
Engineering Verification and Architectural Strategy
VERDICT: APPROVED FOR DOCUMENTATION AND MODEL-EVIDENCE MERGE.
ARCHITECTURAL ACCEPTANCE: HOLD.
COUNTED MODEL EXECUTION: NOT AUTHORIZED BY THIS REVIEW.
I reviewed PR #180, including the L-1 authority analysis, L1Authority.tla, LiveGrant.tla, preregistration, regression traces and reported verification evidence.
The project owner has also emphasized an important strategic requirement:
Lean heavily on established prior art. Before continuing to build increasingly complicated replication, consensus and durability mechanisms, determine whether an established implementation can replace them. SNC's primary innovation is state-native computing, not reinventing distributed storage.
This requirement must guide the next engineering stage.
1. L-1 Authority Safety
The L-1 counterexample establishes a real exclusive-authority problem.
A degrading membership shrink can be chosen while the originating primary misses the reply. Under R7 case 1, a different node can subsequently acquire authority while the original primary still counts older replica grants.
Support A-1/R8.0 as a candidate correction:

* Membership changes must be bound to their base witness-record version.
* Only one change from a primary may remain outstanding.
* Sending a potentially degrading change immediately invokes the stricter authority requirements.
* The primary cannot resume less-restrictive authority merely because a request timed out or a read returned the unchanged version.
* Authority transitions must be safe against delayed requests, Paxos adoption and concurrent promotions.

The recorded TLC counterexample and three negative controls are useful.
However, the corrected L1Authority model has not completed its preregistered counted campaign.
Recommendation: request separate owner authorization for the seven targeted L-1 runs.
Do not treat the smaller uncounted sizing result as architectural acceptance.
2. LiveGrant
The replacement model and regression suite are suitable verification artifacts.
The four newly discovered gaps, LG-F1 through LG-F4, require explicit treatment:

* LG-F1: outstanding membership changes cannot be declared resolved by an unchanged read.
* LG-F2: admission to S requires a valid grant even if the required covered prefix is empty.
* LG-F3: a disk replacement must not allow an empty replica to serve or acquire primary authority merely because an earlier Grow was chosen.
* LG-F4: a node entering reseed must enforce its own eligibility for promotion, including R7 outside-S promotion.

These corrections remain proposals.
The twelve regression traces reportedly pass their prescribed checks, but they do not establish exhaustive correctness.
The counted plan consists of approximately 40 launches. Several configurations are unlikely to complete without substantial depth reductions.
A completed depth-16 or depth-20 check is evidence for that depth only. It does not establish the absence of longer counterexamples.
Do not promote these reduced checks into claims of full protocol verification.
3. Required Model Correction — Fix on Site
PR #180 identifies a defect in the existing `Combined.tla` model:
`NewS(j)` can remove the primary from its own membership set during self-repromotion under R2′.
The new L1Authority model corrects the analogous case, but Combined.tla remains incorrect.
Fix this defect on site before relying on the affected earlier R2′ results.
Identify the affected model configurations, rerun the necessary verification, and explicitly update the evidence record.
Do not silently retain results whose assumptions the defect invalidates.
Maintain independent three-way verification for the correction.
4. Critical Unverified Composition
L1Authority.tla and LiveGrant.tla are independent models.
Their assumptions are not yet composed.
In particular:

* LiveGrant models Shrink atomically.
* L1Authority models the unresolved shrink and authority problem but not replicated application state.
* Neither establishes both exclusive authority and acknowledgment safety under the complete joint protocol.

This is a hard boundary on the evidence.
If SNC retains its custom protocol, final acceptance will require a composition argument or targeted integrated model covering authority, membership, replication acknowledgments and destructive state transitions.
5. NEW HIGH-PRIORITY WORK: Prior-Art and Build-Versus-Reuse Assessment
Before requesting authorization for the broad LiveGrant counted campaign, prepare a focused architectural decision analysis.
This is not another generic research document.
The objective is a concrete answer to:
Should SNC retain its custom replication/consensus/durability stack, replace part of it with an established library, or place SNC's state-native runtime above an existing distributed storage system?
Research these candidates from their primary documentation and implementations.
Candidate A — OpenRaft
https://github.com/databendlabs/openraft
Investigate using OpenRaft as the Rust consensus and replication engine, with SNC supplying its own state-machine integration, storage adapter and networking.
Evaluate:

* Leader election and exclusive authority.
* Membership changes and catch-up.
* Snapshot transfer and log compaction.
* Crash recovery and durability obligations.
* Integration with SNC's worker-local and NUMA-local execution model.
* Performance and operational implications.
* Changes required to the existing HA_DURABLE contract.

Candidate B — TiKV raft-rs
https://github.com/tikv/raft-rs
Investigate raft-rs as the lower-level consensus component.
Determine what SNC must still implement itself: durable WAL, snapshots, transport, scheduling, applied-state persistence and failover integration.
Compare engineering effort and correctness risk against OpenRaft.
Candidate C — PostgreSQL with established HA management
https://www.postgresql.org/docs/18/
https://patroni.readthedocs.io/
SNC already has a direction involving PostgreSQL persistence.
Evaluate whether PostgreSQL synchronous replication and an established HA manager could serve as the first release's authoritative persistent state layer.
Determine whether SNC could preserve its durable state objects, scheduling, application activation and state-native programming model without maintaining a custom replicated WAL engine.
State the performance and transactional limitations explicitly.
Candidate D — FoundationDB
https://apple.github.io/foundationdb/architecture.html
Study its transaction system, recovery generations, log-server durability and fencing.
Determine whether FoundationDB should be considered as:

* An external durable state backend.
* A conceptual reference for SNC's own mechanisms.
* Or unsuitable due to integration and architectural costs.

Do not portray FoundationDB's recovery machinery as a small drop-in library.
Candidate E — Kafka and Redpanda Prior Art
Study Kafka KIP-497, KIP-903, KIP-966 and Redpanda's Raft-based replication.
Concentrate on:

* Stale replica epochs.
* Membership changes in flight.
* Controller-side eligibility checks.
* Replica restarts with missing data.
* Recovery after leader changes.
* Compaction and snapshot boundaries.

In particular, compare Kafka's controller-side stale-replica protection with SNC's LG-F3 rule that permits membership selection but requires the affected node to refuse service.
Determine whether SNC should reject invalid membership at the decider rather than recover from it later.
6. Required Decision Matrix
Compare custom SNC, OpenRaft, raft-rs, PostgreSQL HA and FoundationDB against the same requirements.
At minimum:

1. G-11 and G-12: HA_DURABLE and write admission.
2. G-14 and G-15: no silent loss and log identity.
3. G-35: operator promotion and accepted weaker guarantees.
4. G-48: recovery and operator intervention.
5. m = 2 and the consequences for quorum size.
6. Exclusive authority and fencing.
7. Replica rejoin and membership changes.
8. Checkpoint, snapshot and log recovery.
9. Crash consistency and storage corruption detection.
10. Rust interoperability, licensing and maintenance.
11. NUMA locality, latency, throughput and resource overhead.
12. Integration effort, ongoing complexity and testing requirements.

Do not assume a witness-only Raft voter can safely substitute for a full durable replica without changing the failure model.
Identify exactly which SNC guarantees would have to change for each alternative.
Present three possible decisions:
BUILD: Continue the custom protocol, identifying the specific requirement existing implementations cannot satisfy.
ADOPT: Replace the custom subsystem with a mature implementation and define the SNC integration boundary.
HYBRID: Use established consensus, replication or persistence components while keeping SNC's state-native runtime and specialized execution machinery.
Give a justified recommendation, not merely a list of projects.
7. Preserve SNC's Larger Objective
Keep the larger system architecture central:

* Database-native persistent application state.
* Durable tasks, events, timers and wait conditions.
* State-native scheduling.
* NUMA-local workers and execution.
* Model-independent agent persistence.
* Distributed object and state management.
* Linux-hosted implementation initially, with Rust as the intended systems language.

These are SNC's differentiating capabilities.
Do not allow replication protocol development to consume engineering effort indefinitely if a mature subsystem can provide equivalent foundations.
Conversely, do not substitute a library merely because it is popular. Its guarantees, failure model and integration costs must be compatible with SNC.
8. Recommended Work Sequence
First: Correct the known Combined.tla defect and reconcile affected evidence.
Second: Complete the narrowly scoped L-1 authority verification after explicit owner authorization.
Third: Produce the prior-art build-versus-reuse decision, using primary-source evidence and a concrete compatibility matrix.
Fourth: Present the decision to Codex and the owner.
If the custom protocol remains justified, proceed with a separately authorized LiveGrant campaign and later composition verification.
If an established implementation is preferable, propose the smallest isolated integration experiment before committing to it.
Do not automatically continue adding bespoke protocol rules.
9. Existing Restrictions

* OD-15 authorizes LG-1 through LG-6 and R1.1 as design directions only.
* A-1 and LG-F1 through LG-F4 are not yet accepted.
* Y9-T remains blocked.
* Y10 and Y6c-2 remain conditional.
* D-02, R-23 and O-FLOOR remain open.
* Y4 and Y7 remain on HOLD.
* No final D-06 freeze.
* No unauthorized counted model campaign.
* No X-2 or R640 execution.
* No C++ or Rust implementation or wire-format change.
* No migration or architectural freeze.
* Preserve m = 2 and every currently accepted guarantee unless the owner explicitly approves a revision.

Apply the established project rules:
Always verify, never assume. Fix on site. Three-way independent verification. Consider the complete architecture and all downstream consequences.
FINAL: PR #180 APPROVED FOR EVIDENCE MERGE. NEXT STRATEGIC GATE IS WHETHER WE SHOULD CONTINUE BUILDING THIS DISTRIBUTED STORAGE PROTOCOL OR REUSE A PROVEN IMPLEMENTATION.
