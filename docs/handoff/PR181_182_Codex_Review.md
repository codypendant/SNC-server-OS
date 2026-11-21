# Codex review of PRs #181 and #182 (filed verbatim)

Status (2026-11-21): PR #182 merged as model and evidence only. A-1 stays proposed; the L-1 PASS results are not
protocol acceptance. Raft-rs adoption and custom-protocol acceptance are on HOLD, and the LiveGrant campaign stays on
hold. PR #181 and the strategic decision package of section 11 are separate work.

---

SNC Server OS — Codex Review of PRs #181 and #182
Engineering Decisions and Proposed Product Direction
PR #181: APPROVED FOR DOCUMENTATION MERGE.
PR #182: APPROVED FOR MODEL AND EVIDENCE MERGE.
RAFT-RS ADOPTION: HOLD.
CUSTOM REPLICATION PROTOCOL ACCEPTANCE: HOLD.
NEW STRATEGIC RECOMMENDATION: MAKE THE ENTERPRISE STATE-NATIVE AGENT RUNTIME SNC'S PRIMARY PRODUCT DIRECTION.
The owner and Codex have had an important architectural discussion since PR #180.
That discussion materially affects the decision in PR #181.
Read this handoff as a complete engineering and strategic directive for the next stage. Do not treat approval of these PRs as permission to implement architectural changes.
1. PR #182 Review
The `Combined.tla` `NewS` defect is correctly addressed.
The original model incorrectly removed a sole primary from its own S during self-repromotion.
The corrected expression preserves the primary in S and matches the intended R2′ semantics.
The affected V3 and M2b runs were withdrawn and rerun.
Recorded results:

* V3: PASS, 20,264,050 states.
* M2b: no violation, 22,125,137 states.
* L1-F: PASS, 3,726,353 states.
* L1-Fi: PASS, 8,342,571 states.
* L1-Fr: PASS, 4,138,849 states.
* Three negative controls violated OneAuthority as expected.
* The reachability probe exercised the relevant L-1 transition.

Independent reviews found no major issue with the correction.
These are valid bounded model results as recorded; Codex has inspected the model and report but has not independently rerun TLC.
APPROVE #182 FOR MERGE.
A-1 remains proposed, not accepted.
The composition boundary remains: L1Authority and LiveGrant do not jointly verify exclusive authority and acknowledgment correctness.
Do not interpret the L-1 PASS results as final protocol acceptance.
2. PR #181 Review
The build-versus-reuse analysis is useful and substantially more rigorous than continuing to assume SNC must implement its own consensus protocol.
Under the document's existing assumptions, its HYBRID recommendation is technically reasonable:

* TiKV raft-rs as consensus core.
* Three full voters and quorum two.
* SNC-owned state machine, WAL storage, checkpoints, transport and execution.
* Existing G-14/D-02/R-23 obligations retained.

It correctly identifies that a Raft library does not eliminate the storage durability problem.
It also identifies significant integration obligations:

* HardState persistence and write ordering.
* Execute-before-commit behavior.
* Snapshot consistency.
* Lost log tails and disk replacement.
* Leader placement across many shard groups.
* Checkpoint and log-retention boundaries.
* Cross-shard obligations.
* The possibility of two separate consensus infrastructures.

APPROVE #181 AS RESEARCH AND DECISION EVIDENCE.
However, do not approve its HYBRID recommendation as SNC's architecture yet.
The analysis assumes SNC remains primarily a high-throughput distributed database with authoritative in-memory object state and a specialized replication stack.
The owner is now considering a different primary product.
That requires reopening the top-level decision before selecting the storage implementation.
3. Proposed SNC Product Direction
SNC — State Native Compute
Proposed primary product:
An enterprise-grade, state-native runtime for persistent autonomous AI agents, with authoritative memory, enforceable governance, durable execution and verifiable decision provenance.
SNC would become an agent system of record.
The core commercial capability is establishing the factual and operational context surrounding autonomous decisions and actions.
The owner has emphasized the following enterprise requirement:
An organization must be able to investigate what information an agent had available when it made a decision, which information was actually presented, what policies governed it, what approvals existed, what actions it performed, and how state changed.
This should be a foundational runtime capability, not an optional logging plugin.
4. Proposed Enterprise State and Provenance Model
For each consequential execution, SNC should be able to preserve:
Agent identity

* Persistent agent ID.
* Tenant, project, role and permissions.
* Model provider, exact model identifier and version where available.
* Runtime and execution configuration.

Decision-time evidence

* Exact model-visible inputs and context.
* Versions and hashes of retrieved documents and records.
* Relevant memory objects and authoritative state versions.
* Tool results and observations actually delivered to the model.
* Applicable instructions, policies and policy versions.

Authorization and governance

* Policy decisions and their evaluated inputs.
* Tool permissions and scoped capabilities.
* Required human approvals and their outcomes.
* Delegation and responsibility chains.

Execution evidence

* Tool-call identity, arguments, results and errors.
* Durable step outcomes.
* External side-effect and idempotency records.
* Retries, cancellations, failures and recovery.

State and provenance

* Prior and resulting authoritative state.
* Causal relationships among decisions, tasks and actions.
* Historical object versions.
* Immutable or tamper-evident evidence references.
* Auditable execution and recovery history.

SNC must distinguish the complete observable execution record from a claim to know a model's private internal reasoning.
A full decision record requires deliberate capture at the runtime boundary. A database event log alone is insufficient.
Enterprise requirements must also include tenant isolation, encryption, authorization, retention, sensitive-data controls, evidence access auditing and integrity verification.
Do not interpret "track everything" as permission to retain unrestricted confidential information.
5. Separate What SNC Owns From What It Reuses
Technology SNC should investigate owning:

* Persistent agent identity.
* Authoritative agent memory and state graphs.
* Decision-time provenance.
* Policy and capability enforcement.
* Historical state reconstruction.
* Cross-agent causality and delegation.
* Model-independent agent lifecycle.
* Enterprise evidence and audit APIs.
* State-native scheduling and event activation where existing systems are insufficient.

Technology SNC should prefer reusing unless benchmarks justify otherwise:

* Linux hardware support.
* ACID database persistence.
* WAL and crash recovery.
* Database replication and HA.
* Durable workflow execution.
* Standard identity and encryption infrastructure.
* Established consensus systems.

Keep SNC's NUMA-local worker and low-latency execution research available.
But do not make custom replication or kernel replacement mandatory for the first enterprise agent-runtime product.
6. Reopen the Build-versus-Reuse Decision
PR #181 compared replication engines for the original SNC database architecture.
Now conduct a product-level build-versus-reuse analysis.
At minimum compare:
Option A: Enterprise agent runtime on PostgreSQL with an established durable workflow engine.
Candidates include DBOS Transact Rust and Temporal.
Examine persistence guarantees, recovery semantics, transaction integration, provenance integration, operational complexity and maturity.
DBOS Rust 0.6 is pre-production and must not automatically be designated production-ready.
Option B: SNC's native runtime with PostgreSQL persistence but SNC-owned scheduling and execution semantics.
Use PostgreSQL as the authoritative durable store while SNC manages memory, scheduling, policy, agent identity and evidence capture.
Determine which agent operations truly need database transactions and which can execute locally without sacrificing correctness.
Option C: SNC's specialized distributed state substrate using raft-rs or OpenRaft.
Retain this option for workloads that demonstrate a measurable need for a high-throughput, core-local, replicated state machine.
Do not assume the existing SNC-versus-PostgreSQL synthetic throughput comparison predicts enterprise agent-runtime performance.
Option D: A staged hybrid.
Release the enterprise agent runtime on mature database infrastructure first, retaining a replaceable storage interface for a later specialized backend.
For each option, compare correctness guarantees, engineering cost, throughput, latency, audit capabilities, multi-tenancy, operational burden and time to a usable product.
7. Research Requirements
Use primary documentation and actual source implementations.
Required prior art includes:

* Michael Stonebraker's DBOS architecture and original filesystem/scheduler research.
* DBOS Transact and Apiary.
* Temporal durable execution and event history.
* PostgreSQL transactions, WAL, replication and HA.
* Raft, raft-rs and OpenRaft.
* FoundationDB recovery and transaction architecture.
* Kafka membership and stale-replica fixes.

Identify capabilities already solved, limitations and integration requirements.
Do not build something simply because it has been designed in SNC.
Do not adopt an external product simply because it is established.
The decision must be based on SNC's actual enterprise workload and the guarantees needed.
8. Critical Performance Assumption
PR #181 rejects PostgreSQL partly because every authoritative mutation would leave the owning core.
That reasoning requires reexamination for agent workloads.
An enterprise agent runtime may execute comparatively few durable transactions relative to the time consumed by model inference, external services and tool execution.
SNC can preserve locality for execution while using an established transactional database at well-defined persistence boundaries.
Benchmark those boundaries.
Do not compare synthetic object-operation throughput directly with durable agent-task throughput.
Measure the relevant workload:

* Decision evidence capture.
* Agent state changes.
* Durable workflow transitions.
* Timer and event activation.
* Recovery after termination.
* Concurrent agent activity.
* Memory retrieval and provenance queries.
* Storage and retention overhead.

9. Immediate Engineering Priorities
Priority 1 — Formalize the product direction.
Prepare an architecture decision record establishing whether SNC's first product is the enterprise state-native agent runtime.
This is a proposed decision. Obtain the owner's explicit approval.
Priority 2 — Define the agent-state and evidence contract.
Specify what SNC must capture at decision time, what is authoritative, what is immutable, and what can be reconstructed.
Include a small end-to-end example showing task acceptance, evidence capture, policy approval, model execution, tool action, state transition and audit retrieval.
Priority 3 — Product-level reuse comparison.
Compare established workflow and persistence technologies against that contract.
Return a concrete recommendation.
Priority 4 — Small isolated proof of concept, only after authorization.
Build the minimum agent lifecycle necessary to demonstrate:

* Durable task acceptance.
* Decision-time evidence manifest.
* Policy-gated tool execution.
* Atomic state/evidence commits where possible.
* Crash/restart recovery.
* Historical reconstruction of a decision.

Measure actual overhead.
Priority 5 — Retain existing SNC research.
Do not delete or discard AFR-001, L-1, LiveGrant, D-02, R-23, O-FLOOR or the model evidence.
They remain valuable architectural research and may inform a future specialized SNC backend.
But do not continue expanding the custom protocol automatically.
10. Current Authorization Boundaries

* PR #181 and PR #182 approved for their stated merge scopes.
* No raft-rs or OpenRaft adoption authorization.
* No RX-1 execution.
* No broad LiveGrant campaign.
* No new C++ or Rust product implementation.
* No wire-format, replication-contract or storage-format freeze.
* No silent revision to accepted HA_DURABLE guarantees.
* No unapproved migration.
* Existing SNC engineering philosophy remains mandatory.

Always verify, never assume.
Fix on site.
Three-way independent verification.
Evaluate the full architecture, not isolated components.
11. Required Next Deliverable
Prepare a single SNC Enterprise Agent Runtime — Strategic Architecture Decision Package.
It must contain:

1. Proposed product mission and differentiation.
2. Agent-state and decision-provenance requirements.
3. Build-versus-reuse architecture comparison.
4. Mapping of reusable existing SNC technology.
5. Which original OS/database goals become deferred or optional.
6. Enterprise security, governance and evidence-retention requirements.
7. A bounded, independently reviewable first proof of concept.
8. Exact owner decisions required.

Do not begin implementation.
Submit the decision package to Codex and the owner for review.
STOP.
FINAL: MERGE PRs #181 AND #182. PRESERVE THE VERIFICATION EVIDENCE. REFRAME THE ARCHITECTURAL DECISION AROUND SNC'S ENTERPRISE AGENT-RUNTIME PRODUCT BEFORE SELECTING ITS STORAGE OR CONSENSUS FOUNDATION.
