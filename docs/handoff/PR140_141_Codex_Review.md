# PR #140 and #141: Codex's independent review (verbatim)

Relayed by cody, 2026-11-16 01:28 UTC. Recorded verbatim below. The PR #141 part belongs to the port-exposure
work and is recorded here only for completeness.

---

SNC PR #140 and #141 — Independent Review

PR #140: APPROVED FOR DOCUMENTATION MERGE ONLY.

The architectural analysis is sound enough to retain as a proposed decision record. Do not treat D-01, D-02 or D-03 as fully accepted contracts.

The owner's selection of term-scoped leases as the default is acknowledged.

Before final architectural acceptance, produce a focused safety analysis covering:

1. Lease validity and handoff across N-member set changes, restarts, pauses, clock drift and operator promotion.
2. Proof that every acknowledged write survives promotion and log reconciliation, including unacknowledged suffixes and concurrent membership changes.
3. Explicit guarantees for single-copy windows, including the limits of G-14(b).
4. Dependencies and acceptance criteria for experiments X-1, X-2 and X-3.

Preserve the distinction between choosing the architectural direction and proving the complete protocol.

PR #141: CONDITIONALLY APPROVED.

The security mitigation and regression coverage are acceptable.

Before merge:

1. Run the complete test suite on the final PR head after the test-harness corrections.
2. Verify the mixed-version x9 scenario, or document why it cannot run and obtain explicit approval for that coverage gap.
3. Confirm the final test results and commit SHA in the PR.
4. Keep authentication, authorization and cluster identity explicitly open for AFR-D-07.

Do not begin additional architectural implementation based on either approval.

Continue the existing review gates and independent QA requirements.

Always verify, never assume.
