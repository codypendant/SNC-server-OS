# Codex review of PR #183, revision 2 (2026-11-21)

Relayed verbatim by cody in the project chat, 2026-11-21 15:41 UTC.

---

SNC — Codex Review of PR #183, Revision 2
VERDICT: APPROVED FOR DOCUMENTATION MERGE.
PRODUCT DIRECTION: RECOMMEND ACCEPTING THE FOCUSED AGENT RUNTIME.
NO IMPLEMENTATION, DEPENDENCY ADOPTION, PRODUCT PIVOT OR SCOPE-FREEZE OVERRIDE IS AUTHORIZED BY THIS REVIEW.
I reviewed revision 2 of:

* docs/AGENT_RUNTIME_DECISION_PACKAGE.md
* docs/adr/SNC-ADR-PRODUCT-001.md
* docs/AGENT_RUNTIME_DECISION_PACKAGE_REV1.md
* docs/handoff/PR183_Codex_Review.md

The revisions address my previous HOLD review sufficiently for documentation merge.
1. Product Direction
I support the proposed S1/S2/S3 structure.
S1 — Focused state-native agent runtime
The first product should include:

* Persistent agent identity.
* Durable tasks, events and recovery.
* Authoritative agent memory.
* Model-independent execution state.
* Basic policy-controlled tool execution.
* Lightweight decision provenance and causal history.
* Agent lifecycle inspection and reconstruction.

S2 — Later verification
High availability, scaling, database housekeeping and operational resilience.
S3 — Optional enterprise expansion
Comprehensive audit capture, cryptographic anchoring, advanced governance, regulated multi-tenancy and compliance infrastructure.
Enterprise-first development is not approved.
The owner is explicitly concerned about spending substantial engineering effort on enterprise functionality before validating customer demand.
Preserve that constraint.
2. E-10
The corrected stale-activation contract is suitable as a proposed design.
An epoch check cannot atomically fence an external network call.
The proposed runtime must:

* Commit the dispatched attempt before sending.
* Permit and record outcomes arriving after takeover.
* Track outstanding and in-doubt calls.
* Prevent automatic unsafe retries of non-idempotent actions.
* Require real fencing at the external resource when stronger guarantees are claimed.
* Preserve an owner and escalation process for unresolved effects.

Do not claim exactly-once external effects when the target tool does not support the required guarantees.
3. E-11
The corrected model-invocation identity contract is suitable as a proposed design.
Use an SNC-generated invocation ID before dispatch.
Attach provider request IDs when observable.
Distinguish prepared, dispatched, provider accepted, response received, output committed and result unknown.
A lost model response is not reconstructed as though it were the original.
Any new invocation is separately identified and causally linked.
4. OD-P6 — PostgreSQL
The observation-based durability approach is a valid research candidate, not an accepted contract.
Preserve:

* The distinction between desired configuration and enforceable guarantees.
* The ability of transactions to lower synchronous_commit.
* The requirement that consequential dispatch and acknowledgments wait for established durability.
* Timeline-aware WAL-position comparisons.
* The unknown-commit and promotion checks.
* The separation from SNC's existing HA_DURABLE contract.

One required clarification before AR-1:
Distinguish physical database rows from agent-visible authoritative state.
PostgreSQL may contain locally committed rows that are not yet replication-established.
The recovery test must allow those rows to exist while proving they are not treated as established authoritative state or used to authorize external effects.
Also explicitly verify the exact mechanism for obtaining a valid timeline identifier and recovering an unknown transaction's durable identity and establishment bound.
No durability class is accepted until the required verification is completed.
5. AR-1
The reduced proof of concept is the appropriate scope.
Use:

* One persistent agent.
* One durable task.
* One model call.
* One policy-gated action.
* Idempotent and non-idempotent simulated effects.
* Controlled crashes and restarts.
* Historical decision reconstruction.
* Direct comparison with plain DBOS.

Keep PostgreSQL and DBOS Transact Python as experiment candidates, not permanent architectural commitments.
Temporal remains an alternative.
Set the complete crash matrix, seeds, run caps, performance budget and product differentiation thresholds before execution.
Do not implement until OD-P7 receives the owner's explicit GO.
6. Product Differentiation
Do not assume that provenance automatically creates a viable enterprise product.
Test whether SNC delivers meaningful functionality beyond:

* DBOS.
* Temporal.
* Existing agent frameworks.
* Observability.
* Policy gateways.

The first product must be useful independently of a comprehensive enterprise compliance system.
If plain DBOS plus a small amount of application code satisfies the requirements, consider narrowing SNC to an SDK.
Do not build a substantial custom runtime simply to preserve the original concept.
7. Preserve Earlier SNC Research
Retain all AFR-001 and related evidence:

* L-1 and LiveGrant.
* D-02, R-23 and O-FLOOR.
* raft-rs and OpenRaft research.
* NUMA and worker-local execution.
* State-native object architecture.
* Formal verification artifacts and performance measurements.

Do not delete these assets.
They remain possible foundations for a future specialized backend.
However, do not continue developing the custom replication protocol automatically.
8. Owner Decisions
Return OD-P1 through OD-P8 to the owner individually, with their exact effects.
A yes to the product direction does not automatically:

* Override EXPERIMENTAL_VALIDATION's scope freeze.
* Authorize AR-1.
* Accept PostgreSQL or DBOS as production dependencies.
* Approve Python or Rust as the final language.
* Accept a new durability contract.
* Start enterprise governance development.
* End general-SNC falsification.

All require their own decisions.
9. Next Engineering Step
Merge PR #183 as documentation.
Then request the owner's explicit decisions on the focused product direction and limited validation scope.
Once authorized, prepare the final AR-1 preregistration and conduct independent review before implementation.
Do not expand the architecture beyond the agreed S1 capabilities.
Apply all established project rules:
Always verify, never assume.
Fix on site.
Independent three-way verification.
Evaluate the entire system and its downstream consequences.
FINAL: PR #183 REVISION 2 APPROVED FOR DOCUMENTATION MERGE. PRODUCT PIVOT AND AR-1 STILL REQUIRE EXPLICIT OWNER APPROVAL.
