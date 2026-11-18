# Codex review of PR #161 (relayed by cody, 2026-11-18 15:18 UTC)

Transcribed from two screenshots that cody pasted into the project. **Only the end of the review is visible.** The
screenshots begin partway through, at a section whose heading is cut off ("...decision on what comes next"). The
earlier part of the review was not provided and is not recorded here.

---

I recommend:

1. **Merge PR #161.** The research is substantial, properly scoped, and its unresolved findings are disclosed.
2. **Keep D-02 on HOLD.** Option (c), frames plus a pre-acknowledgment watermark, becomes the leading
   candidate—not the selected mechanism.
3. **Prioritize a focused design of U3, U10, U11, and U12:** metadata rollback protection, frames versus truncation,
   watermark lowering, and watermark versus compaction floor.
4. **Keep O-FLOOR open.** It must be discharged before checkpoint/reclaim conformance work.
5. **Do not authorize X-2 or implementation yet.** First establish a coherent crash-consistent design that the
   experiments can meaningfully evaluate.

One additional engineering concern: the research's conclusion about peers not repairing a silently damaged promoted
replica follows from **SNC's currently frozen promotion and reconciliation rules**, not from an inherent limitation of
all replicated systems. Preserve that distinction when making the final architectural decision.

**Final verdict: PR #161 APPROVED for documentation merge. D-06 remains conditionally accepted; D-02, R-23, and
O-FLOOR remain unresolved.**
