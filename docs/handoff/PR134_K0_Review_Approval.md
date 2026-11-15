# PR #134 review: K0 approved (cody, 2026-11-15)

Transcribed from cody's message in the project chat, 2026-11-15 21:20 UTC.
The same message also approved PR #135 (D2-a), handled in its own thread.

> SNC Server OS — PR #134 and #135 Review Decisions
>
> Both PRs have been independently reviewed.
>
> PR #134: APPROVED FOR MERGE.
>
> K0 successfully introduces persistent admin connections for the migration test probe, substantially reducing connection churn while preserving one-shot HEALTH and HA behavior.
>
> PR #135: APPROVED FOR MERGE.
>
> D2-a correctly derives the default shard count from the process's CPU affinity set. The change is appropriately scoped and supported by regression evidence.
>
> Next Engineering Priorities
>
> After merging both PRs:
>
> Priority 1 — Resolve shard-count correctness defects
>
> Address the findings from PR #135 in separate, narrowly scoped work:
>
> - D3: Cluster/control-mode shard defaults must be consistent with the authoritative placement map.
> - D4: Engine construction failures must produce controlled startup errors rather than aborting without useful diagnostics.
> - D5: Shard-count mismatches must produce explicit, actionable refusals.
>
> Before implementing, inspect the interactions between these defects and determine the correct dependency order.
>
> Preserve durability, recovery, replication compatibility, and existing on-disk state. Do not silently remap clustered state or change the authoritative shard count.
>
> Use deterministic regression tests, including restart and mismatched-affinity scenarios.
>
> Priority 2 — Continue persistent-connection implementation
>
> K0 is accepted. K1 may proceed as its own engineering slice once the shard-count correctness work is appropriately resolved or isolated.
>
> Follow "docs/CONTROL_CONNECTIONS_DESIGN.md".
>
> Maintain these requirements:
>
> - HEALTH and the complete HA decision path remain one-shot.
> - No new pooling delay on witness decisions or promotion.
> - Preserve request deadlines and ambiguous-outcome semantics.
> - Enforce sequence-number validation and connection lifecycle limits.
> - Maintain compatibility with older nodes.
> - Verify operation with keep-alive enabled and disabled.
> - Preserve the PR #132 HEALTH baseline and K0 regression coverage.
>
> Do not combine K1 with unrelated CPU-placement changes.
>
> Engineering Requirements
>
> For each slice:
>
> 1. Establish the exact root cause and affected architectural contracts.
> 2. Implement the smallest correct change.
> 3. Demonstrate regression failure before the fix and success afterward.
> 4. Run relevant integration, recovery, and fault-injection tests.
> 5. Complete independent three-way QA.
> 6. Document evidence, remaining risks, and downstream implications.
> 7. Open a PR and stop for my review before merging or beginning the next slice.
>
> Remember SNC-ADR-RUST-001. C++ remains our architectural validation and behavioral-reference implementation. Do not expand C++ functionality beyond what is necessary for validation, correctness, or maintaining that reference.
>
> Proceed with D3 as the next proposed slice, after checking its dependencies on D4 and D5. Do not start K1 simultaneously unless explicitly authorized.
>
> Always verify, never assume. Fix on site. Preserve system-wide architectural consistency.

## What this means for K0

- K0 is accepted and merged as PR #134. The review made no corrections.
- The EMFILE baseline correction (docs/K0_TEST_PROBE.md) was part of the
  approved PR. The review did not comment on it separately.
- **K1 is not started.** It waits until the shard-count work (D3 to D5) is
  resolved or isolated, and then for cody's explicit go. When it starts, it
  keeps the requirements listed above and is not combined with CPU-placement
  changes.
