# Codex review of PR #156 (relayed by cody, 2026-11-18 00:00 UTC)

This is transcribed from two screenshots that cody pasted into the project. PR #156 was already merged (38ad581) when
the review arrived, and the review approves it. A few words at the top of the second screenshot are hidden behind the
app's header. The text below is complete, because the first screenshot shows the same rows in full.

---

PR #156 — Owner Decision and Conditional Freeze

PR #156 — Conditional Freeze of R2′ and R9 — Approved

**Decision: APPROVE PR #156 for merge.**

I reviewed the owner-decision record, the revised protocol, the `ack_p` definition, and the guarantees updates.

PR #156 correctly records your approval of the recommendations from PR #155.

## What is now conditionally frozen

| Component | Status |
|---|---|
| R2′ verified-survivor replication | Conditionally frozen |
| P1–P7 promotion eligibility | Conditionally frozen, with exclusions |
| S1–S5 serving requirements | Conditionally frozen |
| R6.3′ reconciliation rules | Conditionally frozen |
| R9-A through R9-E | Conditionally frozen |
| R9-F standby read visibility | Not frozen |
| D-02 durability boundary | On hold |

The new `ack_p` definition correctly distinguishes HA_DURABLE acknowledgments from weaker durability classes.

## Important connection to PR #157

PR #156 establishes **node-wide atomic metadata authority** as a condition of the architectural freeze.

PR #157 proposes a physical design intended to satisfy that requirement. However, it also proposes changing the frozen
term-history contract to derive history from WAL records.

That change requires a separate owner decision; approving PR #156 does not automatically approve the revision proposed
in #157.

## Final assessment

PR #156 accurately preserves the three approval boundaries:
1. Architectural contract acceptance.
2. C++ implementation conformance.
3. Authorization to implement the Rust architecture.

**PR #156 is approved.** The conditional freeze is recorded as your architectural decision, while D-02, R9-F, and the
remaining storage guarantees stay unresolved.

No implementation is authorized by this PR.
