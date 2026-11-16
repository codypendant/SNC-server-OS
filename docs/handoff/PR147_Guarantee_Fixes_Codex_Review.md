# SNC Server OS — Codex Review of PR #147 and Next Steps

Relayed by cody as text, 2026-11-16, and transcribed verbatim.

PR #147: APPROVED FOR MERGE.

I reviewed the revised Guarantees Specification, protocol rules, TLA+ changes, and recorded verification results.

The changes adequately address the issues identified during the PR #146 review.

## 1. PR #147 Review Findings

The following corrections are accepted:

- G-15: Log identity is "(term, LSN)", with uniqueness across the entire history.
- HistoryUnique: The new history-sensitive invariant detects same-term LSN reuse that the previous instantaneous
  invariant could miss.
- Verification: The corrected model passed exhaustive checks at approximately 5.23 million and 16.00 million states
  within the configured bounds.
- G-14(a): Damaged term history must fail closed rather than trigger automatic reseeding.
- G-30 / R9: A primary cannot degrade, remove a standby from its acknowledgment set, or acknowledge without that
  standby while its read lease may remain valid.
- G-35: Operator promotion outside the in-sync set must explicitly disclose possible loss of acknowledged writes.
- G-46: Lease safety requires an explicit clock-rate drift bound and a suspend-aware clock.

These changes are approved as PROPOSED contracts and verification evidence, not as proof of complete protocol
correctness.

## 2. Architectural Direction

My recommendations for the owner's decision are:

**AFR-D-01 — Authority Model**

Conditionally accept term-scoped primary leases with the documented eligibility barriers, durable fencing, and
restart requirements.

**AFR-D-03 — Replication Group Model**

Conditionally accept the witness-coordinated in-sync replica-set architecture, with versioned membership records and
explicit promotion eligibility.

**Minimum In-Sync Set Size**

Recommend "m = 2" as the default.

This means SNC refuses new writes rather than silently weakening durability to a single copy. Explicitly distinguish
this admission policy from the redundancy of previously acknowledged writes after a failure.

**AFR-D-02 — Durability Boundary**

Remain on HOLD.

The end-of-log durability mechanism cannot be frozen until experiment X-2 and the single-node recovery semantics are
resolved.

These are recommendations requiring the owner's acceptance. Do not record D-01, D-03 or "m = 2" as owner-approved
until explicitly confirmed.

## 3. Remaining Engineering Obligations

Before freezing the relevant contracts:

1. Define how a standalone node advances to a new term after discarding an unacknowledged torn WAL tail without
   violating "(term, LSN)" uniqueness.
2. Add targeted model coverage for the R9 standby-lease restriction, including degradation and acknowledgment
   interactions.
3. Complete the X-2 durability-boundary experiment, with preregistered performance and correctness criteria approved
   by the owner.
4. Retain the unresolved model-composition obligations A2, A7, A8 and A9, including their documented limitations and
   dependencies.
5. Ensure the corrected PR #147 specification receives the required independent QA verification.

Do not silently promote incomplete or bounded verification evidence into a proven guarantee.

## 4. Next Phase — Controlled Architectural Acceptance

We should now transition from broad exploratory verification into controlled architectural acceptance.

Prepare a concise owner-decision package covering:

- D-01: proposed acceptance, assumptions and remaining obligations.
- D-03: proposed acceptance, membership and promotion guarantees.
- Minimum set size: "m = 2" versus "m = 1", with explicit availability/durability tradeoffs.
- D-02: unresolved choices, X-2 requirements and single-node recovery.
- Any remaining issues that would prevent the Rust architecture from being designed safely.

Distinguish between:

1. Accepting the normative architectural contract.
2. Verifying the existing C++ reference implementation.
3. Authorizing Rust implementation.

These remain separate decisions.

Do not begin additional C++ or Rust implementation without authorization.

## 5. Standing Engineering Principles

Continue applying the project's established rules:

- Always verify, never assume.
- Fix on site.
- Mandatory independent three-way verification.
- Research comparable implementations and production failures before making significant architectural decisions.
- Adopt sound engineering principles, not copied implementations.
- Evaluate the entire system and downstream consequences before changing an architectural contract.
- Preserve evidence, provenance, and explicit owner approval gates.

Immediate action: Merge PR #147 after normal repository checks, prepare the concise architectural decision package,
and stop for owner review.

Do not initiate X-2, additional implementation, or resource-intensive verification runs until authorized.

---

## Action taken

- PR #147 merged (`747bfcf`).
- The owner-decision package is `docs/AFR_001_OWNER_DECISION_PACKAGE.md`. Nothing in it is recorded as approved.
