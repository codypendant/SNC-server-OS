# Codex review of PR #145 (model composition)

**Partial transcription.** Codex's review reached Claude as three phone screenshots. The decision header at the top
of the review was not received. The partial sentence above "Significant findings" was also cut off; what is visible
of it reads: "...correctly recognizes that an old primary may retain authority briefly after a new term is chosen,
provided the new primary does not serve until the old authority expires." Everything below is transcribed as shown.

## Significant findings

- **Stale authority:** Bounded checks pass across the completed scenarios, including operator promotion and
  competing promotions.
- **Mutation testing:** Confirms several term guards are essential for safety.
- **Operator promotion:** Revealed that guards appearing redundant under automatic promotion become essential when
  operator promotion is enabled.
- **Cross-model assumptions:** Nine assumptions are explicitly classified and assessed.
- **Independent QA:** Identified an incorrect conclusion about witness term checks, which Claude corrected.

This is useful architectural progress.

## What remains unresolved

The PR correctly identifies four major limitations:

1. **A2:** The single-timer abstraction of witness-backed degraded authority is not proven equivalent to the
   per-witness model.
2. **A7:** Lagging witness records and overlapping consensus operations remain unchecked.
3. **A8:** Fencing and standby leases interacting with write acknowledgments remain unchecked.
4. **A9:** The assumption that nodes learn only chosen, authoritative records lacks a verified refinement argument.

The full Authority model with restarts and fencing also remains incomplete.

## Planning recommendation

I would merge #145, then **stop expanding the verification documents indefinitely**.

The next milestone should be a bounded, explicitly scoped protocol-acceptance review. We should determine which
guarantees are sufficiently established to freeze and which require targeted experiments or further modeling.

That will let us progress toward Rust architecture without pretending the entire distributed protocol has been
formally proven.

**PR #145 approved for merge. D-01/D-02/D-03 remain proposed, and this approval authorizes no implementation.**

## Action taken

- PR #145 merged (a78e0cb).
- The protocol-acceptance review is `docs/AFR_001_PROTOCOL_ACCEPTANCE_REVIEW.md` (PROPOSED). It adds no modelling.
