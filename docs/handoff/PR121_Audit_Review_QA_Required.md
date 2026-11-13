# PR #121 architecture and evidence audit: review (HOLD MERGE pending independent QA)

Filed verbatim from cody's upload in the project thread, 2026-11-13 03:48 UTC
(relayed because the reviewer's GitHub connector is read-only).
Do not merge PR #121 or begin PR #122 until cody approves.

---

SNC Server OS — PR #121 Review and Required QA

PR #121 has been reviewed. The architecture audit is valuable, and the proposed engineering direction is broadly accepted.

Decision: HOLD MERGE pending independent QA verification.

Before proceeding, complete the following.

1. Independent QA Verification

Assign the independent QA agent to review PR #121.

QA must verify the audit's significant findings directly against source code, tests, and experimental evidence.

Specifically:

- Confirm SNC's native persistence architecture, including WAL, checkpoints, A/B superblocks, and recovery guarantees.
- Verify the identified cross-shard batch ordering issue and determine whether overtaking is actually possible.
- Verify the potential duplicate-application scenario during slot migration and request retries.
- Confirm the current durability guarantees and identify which power-loss and corruption scenarios remain untested.
- Verify the administrative authentication and authorization findings.
- Confirm the repository's current CI coverage and regression testing infrastructure.
- Validate the reported test failures, distinguishing deterministic failures from intermittent behavior.
- Review the proposed engineering milestones for completeness, dependencies, and architectural correctness.

Every finding must have direct supporting evidence.

Distinguish confirmed defects from plausible risks and unverified hypotheses.

2. Correct the Architecture Audit Where Necessary

If QA identifies inaccuracies, unsupported conclusions, or missing architectural dependencies, update the PR #121 report.

Do not modify production code as part of this audit.

Do not expand the scope into feature implementation.

Preserve the distinction between architectural intent, implemented functionality, and experimentally verified guarantees.

3. Reevaluate Engineering Priorities

I agree with prioritizing correctness and durability, but two additional considerations must be reflected in the roadmap.

A. Establish minimal CI early.

We should not wait until the third engineering milestone to establish automated regression protection.

Introduce a minimal CI foundation early in the implementation sequence, including clean builds, deterministic tests, and critical correctness regression checks.

B. Prioritize correctness defects.

Potential duplicate execution during migration and cross-shard ordering violations must receive high priority if confirmed.

These are fundamental state-consistency concerns.

We must not build additional functionality on top of unresolved correctness violations without explicitly evaluating their downstream effects.

4. Preserve SNC's Native Architecture

SNC is being developed as its own state-native computing platform.

Its native object store, execution engine, WAL, checkpointing, persistence, and recovery mechanisms are core architectural components.

PostgreSQL is not a required component of SNC's architecture. Do not introduce it as an implementation dependency based on earlier design discussions.

Evaluate and strengthen the native architecture we have actually built.

5. Required Completion

Before requesting merge approval:

1. Obtain independent QA findings.
2. Personally verify QA's significant conclusions.
3. Correct any audit inaccuracies.
4. Confirm the three recommended engineering milestones and their dependencies.
5. Ensure the roadmap addresses early CI and confirmed correctness risks.
6. Post the final QA evidence and your recommendation on PR #121.

Follow established project engineering rules:

- Always verify, never assume.
- Fix on site within authorized scope.
- Maintain three-way verification.
- Evaluate system-wide and downstream consequences.
- Preserve architectural consistency.
- Do not introduce unauthorized changes.
- Do not begin implementation of the next milestone yet.

Stop after updating PR #121 and posting the final verification results. Wait for my approval before merging or beginning PR #122.

PR #121: The architecture audit looks good overall, but merge approval is on hold pending final verification.

Please coordinate with R640 Claude to complete the independent QA review. Verify the significant findings, particularly cross-shard ordering, duplicate application during migration, durability guarantees, and the reported test failures.

Correct any unsupported conclusions in the audit. Ensure the proposed roadmap prioritizes confirmed correctness issues and introduces minimal CI early.

Preserve SNC's native architecture. PostgreSQL is not an implementation requirement.

Once verification is complete, post the results on PR #121 and notify me.

Do not merge or begin the next PR until the review is complete.