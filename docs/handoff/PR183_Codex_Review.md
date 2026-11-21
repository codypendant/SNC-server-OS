# Codex review of PR #183 (2026-11-21)

Relayed verbatim by cody in the project chat, 2026-11-21 05:46 UTC.

---

SNC Server OS — Codex Review of PR #183
VERDICT: HOLD — CHANGES REQUESTED BEFORE DOCUMENTATION MERGE.
NO PRODUCT PIVOT ACCEPTED. NO IMPLEMENTATION AUTHORIZED.
I reviewed both files in PR #183:

* `docs/AGENT_RUNTIME_DECISION_PACKAGE.md`
* `docs/adr/SNC-ADR-PRODUCT-001.md`

The research is valuable and the build-versus-reuse direction is reasonable.
However, the proposal requires changes to reflect the owner's latest product discussion and to correct three significant safety and correctness issues.
1. Product Direction — Required Revision
The owner is interested in SNC becoming a state-native agent runtime, but has explicitly questioned whether a full enterprise governance platform is commercially justified.
Do not assume enterprise-first product development has been approved.
Revise the proposed product ADR to state:
SNC's proposed first product is a focused state-native runtime for persistent AI agents.
The foundational capabilities are:

* Persistent agent identity.
* Durable tasks, events and recovery.
* Authoritative agent memory.
* Model-independent execution state.
* Basic policy-controlled tool execution.
* Lightweight decision provenance and causal history.
* Agent lifecycle inspection and reconstruction.

Enterprise governance remains a potential expansion, not a mandatory first-release architecture.
Keep advanced enterprise features as a separate design candidate:

* Comprehensive decision-time evidence capture.
* Per-tenant cryptographic anchoring.
* Complex retention and erasure infrastructure.
* Enterprise approval and delegated-authorization systems.
* Detailed access auditing and compliance support.
* Regulated multi-tenant deployments.

Preserve all the research already written. Reclassify it by implementation stage rather than deleting it.
2. E-10 — Stale Activation Fencing
The current contract says a stale activation cannot issue an external call after a newer epoch exists.
However, it admits a window between the gateway's epoch check and the external call.
An idempotency key does not generally eliminate that window or prevent an obsolete activation from performing a different external action.
This is a correctness issue.
Specify either:
A. A real serialization or fencing protocol that enforces the stronger guarantee, including outstanding calls, or
B. A weaker, accurate guarantee that explicitly permits in-flight external effects and records them as completed, rejected or in doubt.
Do not claim that an epoch check alone provides atomic fencing across an external network call.
3. E-11 — Model Invocation Identity
The activation-start record cannot generally contain a server-generated provider request ID before the provider receives the request.
Use a client-generated SNC invocation identifier committed before the call.
Attach the provider's actual request identifier when it becomes available.
The evidence model must distinguish:

* Invocation prepared.
* Request dispatched.
* Provider accepted, if observable.
* Response received.
* Output committed.
* Result unknown after failure.

Preserve the rule that uncommitted model output is not silently reconstructed as if it were the original response.
4. OD-P6 — PostgreSQL Durability
PostgreSQL permits individual transactions to change `synchronous_commit`, including `SET LOCAL synchronous_commit TO OFF`.
A role-level default does not establish an enforceable prohibition.
Revise OD-P6 to distinguish:

* Desired PostgreSQL configuration.
* Application-enforced configuration.
* Database-enforced restrictions.
* Actual replication and failover guarantees.

Before accepting the durability contract, demonstrate that an application transaction cannot silently bypass the required synchronization level.
Test Patroni's documented canceled-commit scenario and primary restart/failover behavior.
External effects must not be dispatched based on a commit whose required durability has not been established.
Continue distinguishing the proposed PostgreSQL committed class from SNC's existing HA_DURABLE guarantee.
5. Build Versus Reuse
Keep PostgreSQL and DBOS Transact as preferred candidates for the first experiment, not approved production dependencies.
The available evidence supports their evaluation.
The Python DBOS implementation provides transactional workflow steps and is a practical candidate for the initial test.
The Rust DBOS 0.6 release is pre-production.
Temporal remains an alternative.
Retain the storage and durable-execution interfaces so these choices can be replaced.
Do not authorize development of a custom journal merely because an existing library fails one test. First determine whether the limitation can be addressed safely through configuration, an adapter or another established implementation.
6. Reduce AR-1
Rescope the initial proof of concept to:

1. One persistent agent with authoritative state.
2. One durable task spanning several execution steps.
3. One model call with an evidence manifest.
4. One policy-gated tool action.
5. An idempotent and a non-idempotent external-effect simulation.
6. Crash/restart recovery at key boundaries.
7. Historical reconstruction of what was shown, authorized, executed and committed.
8. A direct comparison against the corresponding DBOS behavior without SNC's additional layer.

Start on a small isolated PostgreSQL environment.
Defer the complete Patroni HA setup, one-million-agent timer test, ten-thousand-agent concurrency campaign, full cryptographic anchor infrastructure and regulated multi-tenant rollout.
Those belong to subsequent verification stages if the product justifies them.
Set explicit acceptance criteria before any run.
No implementation is authorized by this review.
7. Product Validation Must Come Before Enterprise Expansion
Retain the product falsification gate.
Demonstrate a working runtime with the existing SNC multi-agent engineering environment.
Compare its value against DBOS, Temporal and existing agent frameworks.
Interview potential enterprise buyers about decision reconstruction and governance.
Do not assume that enterprise demand for agent auditing automatically creates demand for SNC.
Require evidence that SNC offers meaningful functionality beyond an SDK, existing workflow engine, observability system and policy gateway.
8. Existing SNC Research
Preserve:

* AFR-001 and its model evidence.
* L-1 and LiveGrant.
* D-02, R-23 and O-FLOOR.
* The raft-rs/OpenRaft build-versus-reuse research.
* The per-core and NUMA architecture.
* The state-native object and scheduling work.

Keep these available for future specialized storage or execution requirements.
Do not automatically expand the custom replication work while the product decision is unresolved.
9. Next Deliverable
Revise the two PR #183 documents.
The ADR must distinguish:

* Proposed focused agent-runtime product.
* Optional enterprise governance expansion.
* Initial build-versus-reuse candidates.
* Product validation requirements.
* Explicit owner decisions.
* Research retained but deferred.

Correct E-10, E-11 and OD-P6.
Perform focused independent review of those corrections.
Then return PR #183 for Codex review.
STOP. Do not begin AR-1, adopt DBOS or PostgreSQL as a committed dependency, modify product code or override the experimental scope freeze without owner authorization.
FINAL: HOLD PR #183 PENDING THE ABOVE CORRECTIONS.
