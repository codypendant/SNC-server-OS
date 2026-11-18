# Codex review of PR #162 (relayed by cody, 2026-11-18 16:10 UTC)

Transcribed from five screenshots that cody pasted into the project. The screenshots overlap and together show the
whole review. Only the Markdown table syntax is restored.

---

## PR #162 — Crash-Consistent D-02 Design

**Decision: APPROVE for documentation merge only. HOLD architectural acceptance of the proposed recovery
mechanisms.**

I reviewed the new crash-consistency design for U3, U10, U11 and U12.

The design makes substantial progress, especially the four-slot mirrored metadata scheme and the watermark-lowering
procedure. However, I would not accept these mechanisms as safe yet.

### Review findings

| Component | Assessment |
|---|---|
| Four-slot mirrored metadata | Promising; needs verification |
| Recovery re-mirroring before advancing | Necessary safety rule |
| Frames and truncation interaction | Improved; one important assumption unresolved |
| Watermark lowering inside pending intent | Reasonable proposed ordering |
| Watermark versus compaction floor | Explicitly addressed, not verified |
| O-FLOOR | Still unresolved |
| D-02 / R-23 | Still on HOLD |

### Two concerns

**1. The four-slot metadata scheme needs adversarial verification.**

The revised design protects the last acted-on generation by maintaining two copies in an untouched pair. Its recovery
procedure repairs a single surviving newer generation before advancing.

That is a sensible approach, but the critical correction was introduced after the reviewers identified a rollback
counterexample. It has not been independently re-reviewed.

I would require a narrowly bounded crash model covering multiple crashes, one media fault, interrupted re-mirroring,
and recovery boot writes.

**2. The lost-unlink argument depends on exact reconciliation behavior.**

Section 3(e) assumes reconciliation never truncates below the actual divergence point.

That assumption is explicitly unresolved as V4. If it is false, the argument that FL-chain detects an old segment
reappearing may fail.

This needs to be checked against the actual R6.2/R6.3′ protocol—not accepted as an inference.

### Next decision

I recommend:

1. Merge PR #162 as a proposed design.
2. Verify V4 directly against the frozen reconciliation protocol.
3. Conduct an independent re-review of the corrected four-slot recovery rules.
4. If those checks do not resolve the crash-consistency uncertainty, authorize **one small, preregistered model**
   covering metadata generations and watermark/truncation ordering.
5. Keep D-02, R-23, and O-FLOOR open until evidence closes them.

The proposal to store the watermark outside the node metadata authority also needs an explicit compatibility ruling
under the owner-approved node-wide atomicity condition.

**Final verdict: PR #162 APPROVED for documentation merge. No D-02 selection, D-06 revision acceptance, X-2
execution, or implementation authorization.**
