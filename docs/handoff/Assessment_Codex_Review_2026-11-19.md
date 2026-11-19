# Codex review of the coordinator's 16-finding adversarial assessment (filed verbatim)

Status (2026-11-19): pasted by the owner on 2026-11-19 02:51 UTC. The assessment it reviews is
`docs/handoff/Coordinator_Adversarial_Assessment_2026-11-18.md`. The reviewer's model name for the coordinator is
replaced by "the coordinator" (no model identifiers in the repository); nothing else is changed. **Nothing in this
review is an owner decision.** The requested deliverable (section 4) was started in
`docs/AFR_001_DURABILITY_CONTRACT.md`, which also carries the "Required next work" of
`docs/handoff/PR169_Codex_Review.md`.

---

SNC Server OS — Independent Review of the coordinator's Adversarial Assessment

I reviewed the coordinator's 16 findings against the SNC engineering work through PR #169.

Overall verdict: The criticism is valid, and I recommend changing the order of the remaining D-02 work.

The recent formal verification has been productive. It exposed real failures in metadata rollback, truncation, segment revival, spare-header ordering, and process restart.

However, we now need to move from verifying individual mechanisms to defining and verifying one coherent storage and recovery contract.

1. Findings accepted

I agree that the following need explicit resolution before final D-02 acceptance:

1. A consolidated fault model covering failure types, fault budgets, independence assumptions, correlated failures, and required recovery behavior.
2. An explicit statement of what the WAL watermark can and cannot detect.
3. A canonical two-phase recovery protocol.
4. A fatal-error policy for every durability-path I/O operation.
5. Pinned filesystem, kernel, mount, storage-cache and power-loss assumptions.
6. Verification of the V8 restart barrier against the target Linux environment.
7. A defined recovery or operator action for every refusal condition.
8. Early performance evidence for the durability watermark.
9. A consolidated normative storage specification.
10. Independent re-review of corrections introduced after earlier reviews.

I agree particularly strongly with the fault-model concern.

We should not continue discovering the intended failure contract one counterexample at a time.

2. Work already completed in PR #169

Before starting new work, reconcile the review against PR #169.

That PR already contains:

- V8-1, V8-L and V8-3 proposals.
- A fresh-inode V8-R alternative.
- A two-phase recovery proposal.
- HeaderFirst and temporary-file recovery rules.
- RS-0 through RS-6 for per-stream reseed.
- R9-A′ and the changes required to R9-B and "ack_p".
- The E1–E25 D-06 revision inventory.
- A proposed fatal durability-path I/O error rule.
- Pinned filesystem and hardware premises.
- Linux v6.8 and v6.12 source analysis.

Do not duplicate this work.

Instead, identify which of the coordinator's findings PR #169 closes, which it partially addresses, and which remain open.

The corrections in PR #169 have not all been independently re-reviewed. Section F was added after its original reviews.

3. Technical positions

Fault model

Create one authoritative fault-model table.

For each fault, record:

- Failure mechanism.
- Whether it is in scope.
- Maximum faults per execution history.
- Whether it can correlate with another fault.
- Required detection, repair or refusal behavior.
- The guarantee that depends on it.
- Evidence supporting the claim.
- Any required storage-device or filesystem assumption.

Explicitly distinguish a single lost write from a device that acknowledges durability and subsequently loses multiple related writes.

The watermark is not proof against arbitrary correlated loss.

Recovery ordering

Adopt a two-phase recovery design as the direction to reconcile:

Phase 1: establish storage ownership, inspect persistent state without modifying it, classify the node and streams, and determine whether recovery may proceed, must complete an intent, must restart reseed, or must refuse.

Phase 2: perform only the permitted idempotent repair and completion operations, validate the resulting state, establish writer state, and permit serving only after all protocol authority requirements are satisfied.

Resolve the PENDING/floor exception explicitly. A legitimate pending completion may need to recreate a floor segment before final validation is possible.

Do not allow a destructive repair to erase evidence that should have caused refusal.

V8 restart protection

Retain V8-L as the leading design candidate, with V8-R as the alternative.

Do not treat Linux source inspection alone as empirical validation.

Specify a narrow process-kill experiment using delayed outstanding I/O, file-description locks and a restarting process.

Check the exact target kernel and filesystem assumptions.

Do not run the experiment without owner authorization.

Segment identity and R-CHAIN

Retain OD-7, OD-8 and OD-9 as the current design directions.

Investigate etcd's checksum-chain implementation as a possible simplification, but require a mechanism-by-mechanism comparison before recommending any change.

Specifically determine whether it replaces or merely supplements:

- "prev_nonce".
- Persistent truncation bindings.
- WAL record verification.
- Compaction-floor anchoring.
- Acknowledged-tail loss detection.

Do not assume a checksum chain detects rollback of both data and its verification evidence.

Witness-assisted recovery

Evaluate which facts the witness durably knows.

Do not assume that knowledge of the current term proves local WAL integrity, truncation completion, or checkpoint validity.

Any proposal to reduce local metadata protection must demonstrate an equivalent safety guarantee and identify the protocol changes required.

Acknowledgment contract

Preserve the intended HA_DURABLE guarantee.

A replica must not count toward HA_DURABLE until the approved durability evidence covers the entry.

PR #169 proposes R9-A′, with explicit changes to R9-B, "ack_p", lease eligibility and R6.3′ durable-end reporting.

This requires a recorded owner decision and supporting evidence. It must not be silently read into frozen text.

Refusal and repair

Produce a refusal-to-repair matrix.

Every refusal reason must identify:

- The violated invariant.
- Whether the failure is recoverable locally.
- Whether the node must leave S or A.
- Whether reseeding is required.
- Whether operator action is necessary.
- What evidence must be preserved.
- What conditions permit the node to rejoin.

Compare whole-node reseed against per-stream reseed.

Because S and A are node-level sets, per-stream reseed removes the whole node from the in-sync membership anyway.

Do not accept the added complexity without demonstrating its practical benefit.

Performance

I agree with obtaining an early storage-cost estimate.

Prepare a separate, minimal measurement proposal for the additional durable watermark write.

It must not modify product code or replace X-2.

Do not run it until the owner explicitly authorizes it.

Do not infer end-to-end HA_DURABLE performance from a single-device microbenchmark.

Prior art

Investigate the FAST 2018 paper identified by the coordinator and the actual upstream etcd WAL checksum-chain implementation.

Verify their applicability to SNC's failure assumptions and replication protocol.

Record precisely what they prove, what they assume, and what they leave unresolved.

4. Next requested deliverable

Prepare one SNC Durability Contract Consolidation package.

It must contain:

1. A complete fault-model matrix.
2. One canonical recovery state machine.
3. A refusal-to-repair matrix.
4. A reconciliation of PR #169 with the coordinator's 16 findings.
5. The precise R9-A′/R9-B acknowledgment revision.
6. The unresolved D-06 changes and owner decisions.
7. A concise comparison of per-stream versus whole-node reseed.
8. The minimum remaining formal-verification and experimental obligations.
9. A proposed order for closing D-02, R-23 and O-FLOOR.
10. A list of decisions requiring owner authorization.

Treat this as a consolidation exercise, not a new design campaign.

Use the existing documents and evidence. Do not invent new mechanisms unless an existing mechanism is demonstrably insufficient.

Require independent three-way adversarial review of the integrated contract.

5. Stop conditions

No new formal model, X-2, R640 experiment, C++/Rust implementation, migration, or architectural freeze is authorized by this review.

D-02, R-23 and O-FLOOR remain open.

Y4, Y5 and Y6 remain pending their required decisions.

Preserve the current protocol guarantees and m = 2 default.

STOP after the consolidated contract and review are complete. Submit the findings and owner-decision package before proceeding.

Final engineering position

The architecture remains promising.

The greatest immediate risk is not a lack of successful tests. It is that the overall storage contract is still distributed across multiple documents and relies on assumptions that have not been checked together.

The next milestone should establish one authoritative failure model and one complete recovery protocol.

After that, we can make a disciplined decision about whether the remaining work requires formal verification, kernel experiments, performance measurements, or architectural changes.
