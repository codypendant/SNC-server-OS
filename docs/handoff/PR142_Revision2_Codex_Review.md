# PR #142 Revision 2: Codex review (relayed by cody as screenshots, 2026-11-16)

Transcribed from the screenshots:

> **PR #142 — Revision 2 Review** — SNC-AFR-001 Safety Analysis — *Approve docs merge*
>
> I reviewed the revised safety analysis, the TLA+ models, and the recorded model-checking results.
>
> **Decision: APPROVE PR #142 for merge as proposed research and verification evidence. Do not accept D-01/D-02/D-03 as a proven protocol yet.**
>
> Claude has addressed all four issues from my previous review.
>
> **Verification results**
>
> | Area | Result |
> |---|---|
> | R3 grantor restart | Revised eligibility rule and counterexample |
> | R8 witness-backed lease | Witness grant rule, quorum intersection, expiry barrier |
> | Single-copy durability | Correctly distinguishes admission from preservation |
> | R6 reconciliation | Explicit invariants I1–I5 and recovery rules |
> | Log model | 10,180,593 states, exhaustive at configured bounds |
> | Authority model | Selected scenarios pass; combined exploration incomplete |
> | Operator promotion outside S | Counterexample remains unresolved |
> | KIP-279 iteration | M1 fix not distinguished by current model |
>
> The most valuable result is that model checking found additional defects in the proposed protocol before implementation. That's exactly why I wanted formal verification introduced at this stage.
>
> **Three items to carry forward**
>
> 1. **The combined authority model remains unverified.** The full configuration reached approximately 95.5 million distinct states without completing. No violation found is not equivalent to a pass.
> 2. **R3's simplified eligibility rule remains an argument, not a model-checked result.** The model retains three timing bounds, while the prose reduces them to `learned + W`. That equivalence needs explicit verification before implementation.
> 3. **The authority and log models are separate.** The log model assumes correct authority rather than checking it. Eventually, we need a bounded integrated model that verifies their interaction, particularly across promotion, acknowledgment, and membership changes.
>
> **Next step**
>
> I recommend merging #142, then assigning Claude a focused **formal-verification closure phase**: resolve operator promotion case 5c, verify R3's simplified rule, and complete bounded checks of the combined authority and durability invariants.
>
> We should reduce state-space complexity through sound abstractions and smaller focused models rather than simply allocating more CPU and memory.
>
> **Bottom line: PR #142 approved for documentation merge. Architecture acceptance remains gated on the outstanding safety evidence.**

PR #142 was merged as `4d12cbb` (docs and models only). The fast test job passed; the full suite job was skipped,
which is expected for a docs-only change.
