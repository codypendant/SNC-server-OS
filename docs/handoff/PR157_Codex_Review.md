# Codex review of PR #157 (relayed by cody, 2026-11-17 23:59 UTC)

Transcribed from three screenshots that cody pasted into the project. PR #157 is approved for merge as a proposed
design document. Final D-06 architectural acceptance is on HOLD. The owner question in the review is Codex's
recommendation, not cody's decision.

---

PR #157 — Storage Format and Metadata Atomicity

PR #157 — AFR-D-06 Storage Design — Approved for documentation merge

**Decision: APPROVE PR #157 as a proposed design document. HOLD final D-06 architectural acceptance.**

I reviewed the storage architecture, metadata layout, truncation/recovery procedure, and adversarial QA findings.

## Engineering assessment

| Design choice | Assessment |
|---|---|
| Node-wide metadata authority | Agree |
| 8 KiB A/B metadata copies | Reasonable candidate; durability gap remains |
| Serialized metadata updates | Agree with the logical atomicity requirement |
| Per-record WAL terms | Agree |
| Derive term history from WAL | Recommend accepting the proposed contract revision, conditionally |
| Segment-based void rule | Promising, but needs crash verification |
| Checkpoint `max_lsn` validation | Necessary correction |
| Node-wide checkpoint blocking | Acceptable initial tradeoff |

## Two concerns before acceptance

**1. Metadata rollback remains a safety issue.**

A/B copies with CRCs detect torn or corrupted copies, but they do not necessarily detect a successfully acknowledged
metadata write that subsequently disappears.

Recovery could select an older valid generation, potentially losing a persisted term or truncation intent.

That directly affects R4 fencing and the reconciliation guarantees. It must remain an explicit D-02/R-23 blocker, not be
treated as solved by the metadata format.

**2. Truncation completion needs crash-consistency verification.**

The proposed segment-based void rule is attractive because it avoids maintaining another durable marker. However, the
sequence involves multiple filesystem operations, including segment deletion, creation, directory synchronization,
state rebuild, and metadata clearing.

The design needs evidence that a crash at any intermediate point cannot expose a previously invalidated WAL tail.

## My recommendation on Claude's owner question

**Conditionally approve deriving term history `H` from per-record WAL terms**, rather than maintaining a separate
KIP-101-style history structure.

This reduces duplicated state and synchronization complexity.

Conditions:
- The term history must be reconstructible from validated retained WAL records and a durable compaction floor.
- WAL corruption must never silently become an acceptable history truncation.
- The revised contract must explicitly preserve G-14(a)'s refusal behavior.
- Final acceptance depends on D-02's durability-boundary design.

I would also authorize the proposed **small crash-consistency model**, with preregistered bounds and independent QA,
before accepting the completion procedure.

The remaining migration and mixed-version decisions should stay separate.

**Final verdict: PR #157 approved for documentation merge. The architectural direction is sound, but the storage format
is not ready to freeze.**
