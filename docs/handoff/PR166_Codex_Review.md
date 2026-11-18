# Codex review of PR #166 and revision 5 (relayed by cody, 2026-11-18 21:04 UTC)

Transcribed from three screenshots that cody pasted into the project. **They show only the later part of the
review.** The opening, including Codex's verdict on merging #166, was not pasted. cody stated separately (21:05 UTC):
"Yeah, 166 is approved to be merged." #166 was merged on that statement (2bc73a0).

---

[cut off] "...WAL segment lifecycle. Revision 5 should address that weakness directly."

### What I want Revision 5 to accomplish

1. **Verify predecessor linking across the lifecycle.** Include normal segment rollover, prepared-spare adoption, and
   reopening after recovery. The model must distinguish the correct predecessor nonce from an incorrect or constant
   nonce.
2. **Resolve the torn spare-header issue (U2).** Verify the proposed separate durable header write before the first
   frame, including crashes between those operations.
3. **Examine per-stream reseeding (O-R).** Ensure a reseeded stream establishes its own segment identities and
   predecessor links without accepting stale segments.
4. **Test the same-nonce C15 scenario.** Confirm that replacing a segment while preserving its nonce cannot cause
   stale data to be accepted after a lost format write.
5. **Strengthen mutation testing.** The three incorrect R-CHAIN variants that survived Revision 4 should fail in
   Revision 5 for the defects they represent.

### One change I would make

I would **not automatically put all five areas into one large state-space model.**

The previous verification was deliberately small. Adding rollover, spare adoption, reseeding, and media faults
simultaneously could cause unnecessary state explosion.

Instead, use separate bounded configurations or narrowly scoped models, with preregistered expectations and a
two-hour cap per run. Reuse the existing abstractions wherever possible.

Most importantly, establish the expected failure traces before running the model.

### Where this leaves SNC

[partly cut off] We have made meaningful progress: bounded evidence against silent rollback; persistent truncation
bindings have been tested; R-CHAIN has detected a real obsolete-segment recovery failure.

But these mechanisms have not yet been verified together across the full recovery lifecycle.

**Revision 5 should be a closure exercise, not another opportunity to redesign the architecture.**

I would authorize it with three conditions: keep the scope bounded, require the incorrect variants to be
distinguishable, and stop for review after the results.

If Revision 5 passes, I would move toward a consolidated D-02 architectural acceptance review rather than continuing
indefinitely with small model revisions. That review would still need to address O-FLOOR, outstanding durability
cases, and the unmeasured X-2 costs.

**My decision: YES to Revision 5. No implementation or R640 authorization.**

---

**Status** (recorded by Claude):
- #166 merged on cody's statement above.
- Revision 5 started: the coordinator asked cody to authorize it, and cody answered by pasting this review (standing
  rule for pasted reviews). Treated as closure: a design defect found by a run is reported, not redesigned in the
  same PR.
- The consolidated D-02 acceptance review is NOT started.
