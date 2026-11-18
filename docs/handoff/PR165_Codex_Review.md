# Codex review of PR #165 (relayed by cody, 2026-11-18 20:40 UTC)

Transcribed from four screenshots that cody pasted into the project; the fourth repeats the end of the third. The
final verdict's last line is cut off after "No X-2, R640, or implementation".

---

## PR #165 — Obsolete-Segment Recovery

PR #165 — R-CHAIN Design. *Approved for documentation merge*

**Decision: APPROVE PR #165 for merge. HOLD architectural acceptance of R-CHAIN until verification.**

I reviewed the new obsolete-segment design, its written crash-consistency argument, the independent reviews, and the
changes to the existing D-02 proposal.

### Engineering assessment

The team correctly abandoned the original generation-ordering approach after reviewers found a blocker.

The replacement, **R-CHAIN**, is a better direction:

- Each WAL segment records its predecessor's nonce in `prev_nonce`.
- Recovery verifies the predecessor link alongside the existing `prev_term` check.
- The existing OD-2 watermark binding protects cases where the predecessor chain alone is insufficient.
- Random segment nonces do not need to be ordered.

The written review found no new blocker in R-CHAIN itself.

However, **one significant issue remains:** a taken spare's header can be torn independently of its first frame,
potentially causing recovery refusal after an ordinary crash.

The proposed separate durable header write needs verification before accepting the complete design.

### My decisions

1. PR #165: APPROVE for documentation merge.

**2. Lost-unlink revival: Treat it as harmful.** Apply the same strict recovery policy already chosen for lost-binding
revival. Recovery must reject obsolete segments rather than relying on later reconciliation to clean them up.

**3. R-CHAIN: Approve as the direction to verify, not as a frozen design.**

**4. Revision 4: Recommend authorization.** Keep it narrowly scoped to the proposed R-CHAIN properties, negative
mutations, and existing bounded verification discipline.

Before final acceptance, also resolve the spare-header write ordering, per-stream reseed behavior, and the same-nonce
C15 scenario.

**Final verdict: PR #165 APPROVED for merge. D-02, R-23, and O-FLOOR remain open. No X-2, R640, or implementation**
[the rest is cut off]

---

**Status of Codex's items** (recorded by Claude; see `PR163_Owner_Decisions.md` for the format):
- Item 4, revision 4: started under cody's standing rule for pasted reviews. cody answered the coordinator's request
  to authorize revision 4 by pasting this review.
- Items 2 and 3 (lost-unlink revival is harmful; R-CHAIN as the direction to verify): **Codex's recommendations,
  pending cody's own yes.** They are not yet owner decisions.
