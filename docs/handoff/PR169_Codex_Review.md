# Codex review of PR #169 (filed verbatim)

Status (2026-11-19): PR #169 merged at `849f7b9` after this approval. Nothing in this review is an owner decision.
R9-A′ (Y5), the V8 rules (Y6a), the spare/TMP rules (Y6b) and the reseed choice (Y6c) are recorded only on the
owner's explicit yes. The "Required next work" in section 4 was started in
`docs/AFR_001_DURABILITY_CONTRACT.md`, together with the deliverable of
`docs/handoff/Assessment_Codex_Review_2026-11-19.md`.

---

SNC Server OS — Codex Review of PR #169

VERDICT: APPROVED FOR DOCUMENTATION MERGE. ARCHITECTURAL ACCEPTANCE REMAINS ON HOLD.

I reviewed PR #169, including the D-02 restart/reseed design note, owner decisions OD-7 through OD-9, the E1–E25 D-06 revision inventory, and the independent review findings.

The documentation may merge. This approval does not authorize implementation, final design acceptance, or changes to frozen contracts.

1. Engineering findings

The V8 analysis establishes a credible approach to preventing outstanding asynchronous direct-I/O writes from interfering with recovery.

I support the following directions:

- V8-1: replace the newest WAL segment after every restart.
- V8-L: protect in-place metadata and watermark files using locks tied to the I/O-bearing open file descriptions.
- V8-3: prohibit other unprotected asynchronous in-place direct-I/O writes.
- HeaderFirst: persist the spare's take-time header before rename.
- Use "RENAME_NOREPLACE" for spare adoption.
- Maintain separate "spare.tmp", "seg.tmp", and "wm.tmp" namespaces.
- Preserve segment identity and nonce during replacement.

These remain proposed design rules pending the final acceptance process.

2. Important unresolved issues

A. R6.5 reseed contract

The proposed RS-6 procedure clears RESEEDING before positioning.

Existing R6.5 text requires the copied state to be durable and positioned before the flag is cleared.

Do not silently reinterpret this requirement.

Determine whether the proposed ordering preserves every safety and availability guarantee. If it requires a frozen-contract revision, identify the exact text and supporting evidence before requesting owner approval.

B. Per-stream versus whole-node reseed

The current protocol treats S and A as node-level membership sets.

Reseeding one stream therefore removes the whole node from S and A.

My preferred initial direction is whole-node reseed unless per-stream reseed demonstrates a meaningful reduction in recovery time or transferred data sufficient to justify its added complexity.

Provide a concise comparison of both approaches, including checkpoint coordination, memory reconstruction, replication catch-up, reclaim stalls, remapping, and implementation complexity.

Do not select either approach without an owner decision.

C. V8-L and restart recovery

The proposed file-description locking approach is promising.

Before acceptance, establish:

- Lock acquisition and release ordering.
- Behavior of outstanding writes during process termination.
- Recovery behavior when the device hangs.
- G-48 recovery-time implications.
- The exact supported kernel and filesystem assumptions.
- The consequences of insufficient disk space during segment replacement.
- The relationship between read-only validation and metadata repair.
- Whether recovery can safely validate a PENDING stream whose floor segment must first be recreated.

The proposed two-phase recovery structure needs reconciliation with the existing D-06 recovery sequence.

D. Acknowledgment semantics

I support the safety objective of R9-A′:

A replica cannot count toward HA_DURABLE until it has applied the entry and its durable watermark covers that entry.

This is a proposed strengthening of the frozen acknowledgment contract, not merely an interpretation of existing text.

Review its effects on:

- R9-A.
- R9-B and "ack_p".
- Lease eligibility.
- R6.3′ durable-end reporting.
- Promotion and reconciliation.
- Recovery raising the watermark to the validated end.

Preserve the existing HA_DURABLE guarantee and m = 2 default.

Do not record R9-A′ as accepted until the owner explicitly approves the revised contract.

3. D-06 revisions

The E1–E25 inventory is useful and should remain intact.

However, Y4 remains on HOLD until the R9-A′ and V8/reseed decisions are resolved.

No conditionally accepted D-06 item may be silently replaced.

4. Required next work

Before proposing further formal verification:

1. Obtain independent re-review of the corrected reseed procedure and Section F.
2. Resolve the R6.5 positioning-versus-clear conflict.
3. Compare per-stream and whole-node reseeding and recommend one initial implementation direction.
4. Reconcile the two-phase recovery proposal with metadata repair and PENDING completion.
5. Produce a compact owner-decision package for Y5, Y6a, Y6b, and Y6c.
6. Keep Y4 pending until those decisions are recorded.

Apply three-way adversarial review. Fix documentation inconsistencies on site and preserve all unresolved findings.

Do not start another model campaign merely to accumulate passing runs.

5. Restrictions

- D-02 remains on HOLD.
- R-23 remains unresolved.
- O-FLOOR remains unresolved.
- No final D-06 freeze.
- No X-2 or R640 execution.
- No C++ or Rust implementation.
- No migration work.
- No unauthorized changes to R2′ or R9.

STOP after the targeted review and owner-decision package. Submit the findings for Codex and owner approval.

FINAL: PR #169 APPROVED FOR DOCUMENTATION MERGE. NO ARCHITECTURAL FREEZE OR IMPLEMENTATION AUTHORIZATION.
