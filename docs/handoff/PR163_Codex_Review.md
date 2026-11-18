# Codex review of PR #163, updated (relayed by cody, 2026-11-18 18:53 UTC)

This is a transcription of four screenshots that cody pasted into the project. The screenshots overlap, and together
they show the whole review.

---

## PR #163 — Updated Review

PR #163 — MetaWatermark Verification. *Approved for documentation/model merge*

I reviewed the updated PR, including `MetaWatermark.tla`, the preregistered results, and the second independent
review.

**Decision: APPROVE PR #163 for merge. Do not accept the D-02 design as final yet.**

### Verification results

- Revision 1: 27 of 28 runs matched expectations. One mutation was invalid because of a model defect.
- Revision 2: **All 77 runs matched their preregistered expectations.**
- Four-slot metadata recovery passed its bounded tests.
- All five weaker metadata schemes produced rollback counterexamples.
- Watermark/truncation safety passed under serialized data writes.
- The pipelined-write test exposed a crash-only recovery refusal.
- Independent review found another unverified segment-revival scenario.

These are useful results, but they establish safety only within the model's bounds and abstractions.

### My three decisions

Since you've approved all three, these are the positions I recommend recording:

**1. Serialized WAL flushes — YES.**

Require at most one data flush in flight per stream. This matches the current implementation and avoids the
crash-only refusal exposed by W0P.

**2. Persistent truncation binding — YES.**

Carry the `(L+1, segment nonce)` binding in every subsequent watermark update until the next truncation. Do not
discard it after the first append.

This addresses the counterexample found during revision 2's independent review. It still requires verification.

**3. Watermark outside node-wide metadata authority — CONDITIONALLY YES.**

Accept the watermark as an independently persisted **durability validator**, not another authoritative source for the
node's term, truncation intent, or incomplete state.

The condition is that disagreement must cause recovery refusal, never an alternative interpretation of authoritative
state. The combined crash behavior still needs verification.

### What happens next

Claude should record those decisions, update the proposed design, and perform **one narrowly scoped revision 3**
covering the persistent nonce binding and the corrected serialization assumptions.

Keep the existing preregistration discipline, negative mutations, and independent review.

**Final verdict: PR #163 APPROVED for documentation/model merge. Your three decisions can be recorded, but D-02,
R-23, and O-FLOOR remain open. No implementation or X-2 authorization.**
