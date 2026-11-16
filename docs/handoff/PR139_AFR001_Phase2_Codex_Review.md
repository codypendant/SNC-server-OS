# PR #139: Codex's Phase 2 independent review (verbatim)

Relayed by cody, 2026-11-16 00:45 UTC. Recorded verbatim below.

---

SNC-AFR-001 — Phase 2 Independent Review of PR #139

Decision: APPROVE documentation as PROPOSED. HOLD acceptance of normative guarantees and architectural decisions pending refinements.

I reviewed SNC_GUARANTEES.md and the Phase 2 handoff.

The 63 guarantees, 32 rulings and 15 decisions provide an appropriate foundation. Preserve the structure and evidence references.

Before requesting owner acceptance, address the following:

1. AFR-G-14 / AFR-D-02: Distinguish detecting damaged WAL records from proving that no acknowledged data was lost. The minimal C++ fail-closed slice is useful but must not claim full G-14 compliance without a proven acknowledgment boundary. Include completely missing or zeroed flushes, superblock rollback, and LSN reuse in the evaluation.

2. AFR-R-05 / AFR-D-05 / AFR-D-06: Do not implement a durable dedup fingerprint format before resolving session identity and format compatibility. A provisional C++ change requires explicit authorization and migration analysis.

3. AFR-D-01: Develop a unified authority model covering primary reads, writes, durable fencing, promotion, restart, migration, and partition recovery. Compare lease-based and quorum-confirmed approaches against the same safety invariants. Specify clock assumptions and stale-authority rejection.

4. AFR-D-03: Do not accept N-replica majority acknowledgment solely on architectural preference. Specify quorum intersection, data-commit semantics, witness responsibilities, promotion eligibility, and membership transitions. Compare the complexity and guarantees against the existing pair model.

5. AFR-G-09: Separate at-least-once delivery, idempotent processing, and exactly-once observable effects. Do not claim exactly-once application without a precise deduplication and crash-recovery proof.

6. AFR-G-11 / G-12: Specify requested versus achieved acknowledgment class, how degradation is represented, and what an acknowledged response guarantees. Ensure the C++ reporting slice does not prematurely freeze a wire format.

7. AFR-G-40–45: Keep authentication, authorization, cluster identity, and resource-exhaustion guarantees separate. Loopback-only binding is an interim exposure reduction, not a production security solution.

8. Differential testing: For each accepted guarantee, define a testable oracle. KNOWN-VIOLATION scenarios must remain visible in coverage reports; exclusion must not be confused with passing verification.

9. Rust readiness: Maintain the distinction between accepting a contract, proving the C++ reference satisfies it, and authorizing Rust implementation. These are separate decisions.

No implementation is authorized by this review.

Submit a focused documentation revision addressing these points. Do not redesign the whole specification or start the five C++ slices.

Preserve all owner dispositions as pending. Stop for review after the revision.

Always verify, never assume.
