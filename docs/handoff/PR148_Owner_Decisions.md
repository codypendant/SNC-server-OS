# Owner decisions on PR #148 (cody, 2026-11-16)

Transcribed verbatim from cody's message in the project, 2026-11-16 22:49 UTC. This is the owner's decision record.

---

SNC Server OS — Owner Architectural Decisions and Engineering Direction

Reference: PR #148 — AFR-001 Owner Decision Package
Status: Owner-approved direction, with explicit conditions and unresolved obligations
Scope: Architectural decisions and authorized research only. No C++ or Rust implementation authorized.

## 1. AFR-D-01 — Authority Model

DECISION: CONDITIONALLY ACCEPT

Adopt term-scoped primary leases as SNC's authority model.

The architecture shall use:

- Primary leases granted by every member of the acknowledgment set A in normal operation.
- Witness-majority-backed leases in degraded operation.
- Durable term fencing.
- The R3 "learned + W" eligibility barrier before a newly promoted primary serves.
- Authority verification at response time.
- Suspend-aware monotonic time, with an explicitly documented clock-rate drift bound.
- Linearizable witness reads as a required consensus-service guarantee.

The safety assumptions and incomplete verification coverage must remain documented.

Conditions: Update the normative guarantees and architectural decision records to reflect this selection. Resolve the
outstanding consensus-service obligations before designing the relevant Rust components.

This decision accepts the architectural mechanism, not a claim that the current C++ implementation already satisfies
it.

## 2. AFR-D-03 — Replication Group Architecture

DECISION: ACCEPT THE ARCHITECTURAL DIRECTION; HOLD FINAL R2 FREEZE

Adopt the witness-coordinated in-sync replica-set model.

The architecture shall use:

- A versioned witness-held group record.
- Explicit primary terms and membership epochs.
- An acknowledgment set A containing the committed in-sync set S.
- Durable acknowledgment from every required member.
- Compare-and-set membership changes.
- Explicit promotion eligibility.
- Term-aware reconciliation and durable recovery procedures.

Do not switch to majority-acknowledged data replicas without a separate architectural review.

Required R2 evaluation

The current R2 rule reduces the in-sync set to "{new primary}" after every promotion.

With a minimum set size of two, this means every failover temporarily prevents new writes, even when other surviving
replicas may already hold all acknowledged entries.

Before freezing R2, investigate whether surviving members can safely remain in the in-sync set when their logs are
proven compatible with the promoted primary.

Research relevant mechanisms in PacificA, Kafka, Raft, and other established distributed systems.

Evaluate:

1. Preservation of every acknowledged write.
2. Promotion eligibility and durable log-prefix evidence.
3. Membership changes concurrent with promotion.
4. Lease fencing and authority handoff.
5. Crash recovery and incomplete replication.
6. Behavior with two, three, and larger replica sets.
7. Recovery time and write-availability implications.
8. Additional implementation complexity.

Compare the existing "{new primary}" rule against a verified-survivor alternative.

Do not change the protocol based solely on expected performance improvements. Any replacement must preserve or
strengthen the established safety invariants.

Present a recommendation, supporting evidence, and a bounded validation plan for owner approval.

The E1–E11 promotion eligibility rules must also be rewritten for the chosen in-sync-set model.

## 3. Default Minimum In-Sync Set Size

DECISION: ACCEPT "m = 2" AS THE DEFAULT POLICY

SNC shall prioritize acknowledged-write durability over write availability when replication redundancy is
insufficient.

- Default minimum in-sync set size: 2.
- If fewer than two required durable copies are available, new writes are refused or wait according to the defined
  admission policy.
- A weaker "m = 1" configuration may be supported only through an explicit per-group setting.
- Writes acknowledged with one durable copy must be classified as LOCAL_DURABLE, never HA_DURABLE.
- The status surface must identify degraded redundancy, including reduced protection for previously acknowledged
  writes.
- G-11 must explicitly require at least two durable copies for HA_DURABLE.

The configured minimum controls admission, not the number of surviving copies of historical data.

Important: The default "m = 2" is accepted as policy. Its failover availability consequences must be revisited after
the R2 comparison. Do not silently weaken the default to improve availability.

## 4. AFR-D-02 — WAL Durability Boundary

DECISION: HOLD

Do not select the final durability-boundary mechanism yet.

The candidate mechanisms remain:

- Durable per-flush watermark.
- Self-describing flush frames with a per-segment nonce.
- A combination of both.

The selected design must detect or safely refuse recovery when acknowledged data may have been silently lost.

Performance alone cannot justify weakening G-14(b).

X-2 authorization

AUTHORIZE PREREGISTRATION ONLY.

Claude may prepare the complete experiment specification, including:

- R640 hardware and storage manifest.
- Test methodology and workloads.
- Correctness and fault-injection cases.
- Performance metrics, including throughput and p99 latency.
- Repetition counts and statistical treatment.
- Baseline comparison.
- Acceptance criteria.
- Resource requirements and expected runtime.

Use 10% maximum performance overhead as a proposed initial threshold, not an approved final acceptance limit.

Present the preregistration for owner review before executing the experiment.

No R640 test run is authorized by this decision.

## 5. Single-Copy Recovery

DECISION: PREFER MONOTONIC LSN RESERVATION AS THE DESIGN CANDIDATE

Investigate durable block reservation of LSNs so that a restarted sole copy can resume without reusing a previously
assigned "(term, LSN)".

This applies to:

- Standalone nodes without replicas.
- Degraded groups whose primary is the only remaining copy.

The design should preserve the existing HA term where possible rather than introduce an independent local term that
could conflict with future group terms.

However, this approach is not accepted as the final recovery contract.

Claude must establish:

- Crash safety of the reservation mechanism.
- Recovery after a silently lost reservation write.
- Detection of stale or rolled-back metadata.
- Compatibility with WAL formats that contain LSN gaps.
- Preservation of acknowledged writes.
- Interaction with G-55 and G-48.
- Behavior when the node later joins a replicated group.

The final decision remains dependent on D-02 and the durability-boundary evidence.

## 6. Standby Read Leases — R9

DECISION: RETAIN THE PROPOSED RULES; REQUIRE TARGETED VERIFICATION

The primary must not degrade, remove a leased standby from its acknowledgment set, or acknowledge writes without that
standby while its read lease may remain valid.

Authorize a targeted TLA+ modeling task only, focused on the interaction among:

- Standby read leases.
- Acknowledgment ordering.
- Degradation.
- Replica removal.
- Primary promotion.
- Restart and fencing.

Use bounded configurations and mutation tests.

Do not expand this into another open-ended formal-verification program.

The result must be independently reviewed before R9 is frozen.

## 7. Remaining Architectural Obligations

Preserve the outstanding obligations, including:

- A2: Soundness of the witness-lease abstraction.
- A7: Lagging witnesses and overlapping consensus operations.
- A8: Fencing and standby leases interacting with acknowledgments.
- A9: Linearizable witness reads and authoritative record learning.
- AFR-D-06: Wire and on-disk versioning.
- AFR-D-07: Authentication, authorization, and cluster identity.
- AFR-D-11 and D-13: Membership changes and consensus-service design.
- AFR-D-10: Default read consistency.
- AFR-D-14: Status and durability reporting.
- AFR-D-15: Refusal of unsafe configurations.

Do not treat incomplete model-checking runs as passing evidence.

## 8. Engineering Process and Authorization

Continue applying SNC's standing engineering principles:

- Always verify, never assume.
- Fix on site.
- Mandatory independent three-way verification.
- Research existing systems and engineering failures before introducing architectural mechanisms.
- Prefer designs supported by evidence.
- Preserve SNC's independent state-native architecture.
- Maintain explicit ownership, provenance, and decision records.

Architectural acceptance, C++ conformance, and Rust implementation authorization remain three separate gates.

No new C++ or Rust implementation is authorized.

## 9. Immediate Next Actions

1. Merge PR #148 after normal repository checks.
2. Record D-01 as conditionally accepted.
3. Record the in-sync-set architecture as the selected D-03 direction, with final R2 behavior pending.
4. Record "m = 2" as the default policy and prepare the required G-11 amendment.
5. Keep D-02 on HOLD.
6. Prepare the X-2 preregistration without running it.
7. Perform the targeted R2 architectural comparison and R9 model verification.
8. Obtain independent QA on the final corrected documents.
9. Return with a concise decision package covering the remaining R2, R9, and D-02 questions.

STOP after these authorized research, modeling, and documentation tasks. Await owner approval before implementation
or resource-intensive experiments.

---

## Where each item is handled

- Items 1-5, the R2 comparison, the E1-E11 rewrite and the single-copy recovery investigation: this PR.
- X-2 preregistration (item 6) and the R9 model (part of item 7): separate threads, in their own PRs.
