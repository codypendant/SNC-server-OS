# Codex review of PR #171 (filed verbatim)

Status (2026-11-19): PR #171 merged at `32c040f` after this approval. **Nothing in this review is an owner decision**;
its section 1 items are Codex's recommendations. The owner's decisions on them are recorded in
`docs/handoff/PR171_Owner_Decisions.md`. Section 2's targeted review and section 3's first priority (I2-I5) were
started in `docs/AFR_001_DURABILITY_DECISIONS.md` and `docs/AFR_001_R65_INVARIANTS.md`.

---

SNC Server OS — Codex Review of PR #171

VERDICT: APPROVED FOR DOCUMENTATION MERGE. ARCHITECTURAL ACCEPTANCE REMAINS ON HOLD.

I reviewed PR #171, including the owner-decision package, revised durability contract, G-48 analysis, and round-three independent review findings.

The documentation is approved for merge.

1. Owner-decision recommendations

Y5 — YES.

Recommend accepting R9-A′ as a recorded revision: a replica counts toward HA_DURABLE only after applying the entry and durably covering it with its watermark.

Record every required textual and semantic change to R9-A, R9-B, G-30, R6.3′ and the other affected clauses.

This does not close P5's excluded silent-loss clause.

Y6a — YES, as a design direction.

Support V8-1, V8-L and V8-3, including the expanded locking requirements for asynchronous direct-I/O files.

V8-R remains the fallback if the target-kernel verification invalidates V8-L.

Y6b — YES, as a design direction.

Support HeaderFirst, the temporary-file namespaces, "RENAME_NOREPLACE", and the documented recovery behavior.

Y6c-1 — YES, as the initial direction.

Prefer whole-node in-place reseeding. Retain per-stream reseeding as a possible later optimization.

Y6c-2 — CONDITIONAL.

Support the proposed R6.5 direction: clear RESEEDING after durable publication at RS-6, then complete ordinary R6.2 positioning.

However, do not accept the frozen-contract revision until I2–I5 have been explicitly re-argued for the state where a node is complete but not yet positioned.

The analysis must cover acknowledgments, leases, promotion, membership, crash/restart, and reconciliation.

Y8 — YES, as a design direction.

Correct both G-48 crash-only failures:

1. Introduce a durable seed marker before transferring data, allowing an interrupted initial seed to restart automatically.
2. Create and synchronize "links_generation" before the first node-record write.

Do not introduce new G-48 exceptions merely to accommodate these failures.

Y4 and Y7 remain on HOLD.

2. Required verification

Round three found 12 major issues. The fixes were incorporated, but those fixes have not been independently re-reviewed.

Perform a narrowly scoped final review of the corrected text, particularly:

- Step 9 synchronization before watermark advancement.
- Recovery with no active checkpoint.
- The G-48 seed marker and links-generation ordering.
- The complete-but-unpositioned state after RS-6.
- The revised R9-A′ wording and its effect on R9-B.
- The fault model's independence assumptions.
- Experiment isolation and data-safety safeguards.

Record any remaining counterexamples.

Do not silently alter accepted contracts or expand the fault model.

3. Architectural priorities

After the owner decisions are recorded, prioritize:

1. Closing the I2–I5 argument for R6.5.
2. Validating the V8-L premise on the target kernel.
3. Establishing the watermark's actual device cost.
4. Completing O-FLOOR verification.
5. Preparing the consolidated D-06 byte-level specification once its prerequisite decisions are accepted.

Keep the current formal-model and experiment restrictions in place until separately authorized.

4. Restrictions

- D-02 remains on HOLD.
- R-23 remains unresolved.
- O-FLOOR remains unresolved.
- No final D-06 freeze.
- No X-2 or R640 execution.
- No unauthorized model or experiment.
- No C++ or Rust implementation.
- No migration.
- Preserve m = 2 and existing guarantees.

STOP after recording the owner's explicit decisions and completing the required targeted review.

FINAL: PR #171 APPROVED FOR DOCUMENTATION MERGE. NO IMPLEMENTATION OR FINAL ARCHITECTURAL ACCEPTANCE.
