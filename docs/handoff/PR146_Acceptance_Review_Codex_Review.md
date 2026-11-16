# Codex review of PR #146 (protocol-acceptance review)

Transcribed from four phone screenshots that cody relayed. They appear to cover the whole review. Above the title,
a partly visible header reads "Reviewed Pull Request 146" and "Documentation changes and comments".

## PR #146 — Protocol Acceptance Review

PR #146 · **Approved for documentation merge**

I reviewed the proposed acceptance matrix, supporting evidence references, and QA corrections.

**Decision: APPROVE PR #146 as a proposed decision record. Do not freeze the contracts yet.**

Claude has correctly separated model-checked guarantees, argued guarantees, unresolved defects, and decisions
requiring experiments. This is the decision framework we needed.

## My rulings

| Area | Decision |
|---|---|
| R1–R4, R6, R7, R8 | Accept as candidates for freezing, subject to stated assumptions |
| R5 and other argued rules | Accept as proposed contracts, not formally verified properties |
| R9 standby leases | Hold until the missing degradation restriction is incorporated |
| G-15 log identity | Hold until `(term, LSN)` semantics and reuse rules are corrected |
| G-14 durability boundary | Hold pending X-2 |
| G-35 operator promotion | Require explicit acknowledgment of possible data loss |
| G-46 clock requirements | Require suspend-aware clock and explicit drift assumptions |
| D-01 authority model | Ready for conditional architectural acceptance |
| D-02 end-of-log | Not ready |
| D-03 replication groups | Ready for conditional architectural acceptance |

## One additional issue

The proposed G-15 revision allows `(term, LSN)` identity to be reused after truncation or reseeding.

That requires precise wording. **An LSN may be reused under a different term, but the same `(term, LSN)` must never
identify different content.**

The current model's `EntryUnique` invariant does not establish uniqueness across the entire history. I would
require a history-sensitive test or invariant before freezing this clause.

## What I recommend now

We should move toward architectural acceptance rather than continue generating broad verification documents.

Claude should make the five identified wording corrections, plus the G-15 clarification, in the actual Guarantees
Specification.

Then we can accept D-01 and D-03 conditionally, with explicit assumptions and remaining obligations. D-02 should
remain pending until the durability experiment.

I would also recommend **minimum in-sync set size `m = 2` by default**. That means SNC refuses new writes rather
than silently dropping to single-copy durability after a failure. An explicitly configured weaker mode could be
considered separately.

**Bottom line:** PR #146 is approved for merge as documentation. It does not itself authorize Rust or C++
implementation, and the final architectural acceptance remains a separate owner decision.

## Action taken

- PR #146 merged (`031a75f`).
- The five wording corrections and the G-15 clarification are made in `docs/SNC_GUARANTEES.md`. The R9 restriction
  is added to `docs/AFR_001_SAFETY_ANALYSIS.md`.
- `HistoryUnique`, a history-sensitive `(term, LSN)` invariant, is added to `spec/tla/Log.tla`, with a deliberately
  broken variant that violates it (`spec/tla/RESULTS.md` section 8).
- Not done here, because they are cody's decisions: conditional acceptance of D-01 and D-03, and `m = 2` as the
  default.
