# SNC-ADR-PRODUCT-001: First product is the enterprise state-native agent runtime

- **Status:** PROPOSED. Not accepted until cody says yes. Nothing here authorizes implementation.
- **Proposed:** 2026-11-21, by Claude, from Codex's review of PRs #181/#182
  (`docs/handoff/PR181_PR182_Codex_Review.md`, section 9 priority 1).
- **Analysis:** `docs/AGENT_RUNTIME_DECISION_PACKAGE.md`.
- **Scope:** the product decision only. Storage, workflow engine, durability class and language are separate owner
  decisions (package section 8, OD-P3 to OD-P8) that depend on this one, as Codex's review asks ("reframe ... before
  selecting its storage or consensus foundation").

## Decision (proposed)

1. SNC's first product is an **enterprise runtime for persistent AI agents that is the agent system of record and its
   enforcement boundary**. It holds authoritative agent state and memory. Consequential actions can only pass through
   its policy and approval gate. It executes durably and records, at the runtime boundary, the evidence needed to
   reconstruct any consequential decision.
2. This is a **product pivot**. The general SNC database and OS goals become deferred research. AFR-001 and all model
   evidence are preserved unchanged and not expanded.
3. It is an **explicit owner override** of the scope freeze in `docs/EXPERIMENTAL_VALIDATION.md` section 3, which
   froze "the Agent Runtime", multi-tenancy and the security subsystem until a GO, REFINE, NARROW or STOP decision.
   It is not one of those four verdicts. General-SNC falsification is suspended, not concluded.
4. The vision calls Agent-SNC "a separate hypothesis with its own falsification gate"
   (`docs/handoff/SNC_Future_Architecture_Vision_v2.txt` lines 10-11). That gate is kept: the product criteria in
   package section 7.2 are preregistered before any build.

## Consequences

- The agent runtime gets its own guarantees document. `docs/SNC_GUARANTEES.md` and HA_DURABLE are not revised.
- AGENTS.md invariants keep governing the SNC core. Which of them carry over to the runtime is package section 4.1.
- SNC-ADR-RUST-001 is not changed by this ADR. Whether the runtime follows it is OD-P8.
