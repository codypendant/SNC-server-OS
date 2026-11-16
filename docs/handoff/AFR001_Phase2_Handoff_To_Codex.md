# SNC-AFR-001 Phase 2: handoff to Codex (Guarantees Specification, proposed)

**From:** Claude (Claude Code), lead reviewer for SNC-AFR-001
**Date:** 2026-11-16 UTC
**Baseline:** `main` at `2fb029c` (PR #138 merged)
**Document under review:** `docs/SNC_GUARANTEES.md` (PROPOSED; nothing accepted)

## What is asked of Codex

Review the proposed guarantees, rulings and decisions independently before any of them becomes authoritative.
Particular points where a challenge is wanted:

1. **Section 13.1, the FIX-IN-C++ set.** Five narrow slices are proposed:
   - WAL fail-closed, with no format change;
   - audit-D1 ordering;
   - audit-D3 dedup binding;
   - pinned hash vectors;
   - reporting the acknowledgement class after a lost standby.

   Everything else is a known violation that Rust must not reproduce. The rule applied is: fix in C++ only when the
   defect makes C++ an unreliable oracle in scenarios the harness must cover. Is the line drawn in the right place?
   In particular, should storage damage get a `KD` treatment like partitions instead?
2. **Section 13.2, ready versus not ready.** Following the planning note, nothing in the "not ready" column should be
   frozen in order to move faster. HA_DURABLE is deliberately in that column, because it depends on the group model.
3. **AFR-D-01 (authority model).** This one decision closes stale reads, O1 and non-durable fencing together.
4. **AFR-D-02 (end of log).** No recommendation is made until both options are measured. Is the proposed
   experiment enough?
5. **Documented limitation versus violation.** Every ruling now says whether the behavior is documented today. A
   DEFECT ruling on a documented limitation becomes a violation only if its clause is accepted.
6. **Security (G-40 to G-44, G-63).** This section covers forged consensus messages, WAL injection into a standby,
   data disclosure and authentication on every endpoint. The mechanism is left to AFR-D-07.

## Not done

- No product code, test, format or protocol change.
- No owner disposition is filled in.
- None of the proposed slices is started.
- The roadmap and the Rust readiness criteria (Milestone 3) are not written.
