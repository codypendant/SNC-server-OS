# PR #121 architecture audit: final approval, and PR #122 (M1-0 minimal CI) assignment

Filed verbatim from cody's upload in the project thread, 2026-11-13 04:56 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

SNC Server OS — PR #121 Approved / PR #122 Authorized

Project Claude,

PR #121 is approved for merge.

The architecture audit and final QA findings are accepted. The corrections, narrowed conclusions, and documented limitations are appropriate.

Proceed with merging PR #121.

Next Assignment: PR #122 — M1-0 Minimal CI Foundation

We are moving into the correctness and durability hardening phase identified by the architecture audit.

Your next assignment is M1-0: establish a reliable automated regression-testing foundation before implementing the confirmed defect fixes.

Scope

1. Establish minimal CI using GitHub Actions.
2. Use GitHub-hosted runners initially, provided resource requirements permit. Reserve the R640 for resource-intensive and hardware-specific testing.
3. Configure clean builds, deterministic tests, and meaningful failure reporting.
4. Correct the stale "test_pilot" assertion against the existing frozen benchmark configuration. Do not alter frozen benchmark artifacts or experimental results.
5. Separate fast CI tests from long-running integration, stress, and performance tests.
6. Ensure CI failures are visible and actionable.
7. Document the testing workflow and runner requirements.

Do not introduce unrelated production changes.

Engineering Requirements

- Preserve all existing SNC architectural contracts.
- Do not change experimental evidence or preregistered outcomes.
- Do not weaken tests simply to obtain passing results.
- Investigate unexpected failures rather than suppressing them.
- Follow existing manifests and project engineering policies.
- Maintain independent three-way verification.
- Fix issues on site when they fall within the authorized scope.
- Evaluate downstream consequences before making changes.

Subsequent Work

After M1-0, the next priorities are:

D1 — Request ordering: Define and enforce the ordering contract. Preserve per-client ordering where required, including mixed batched and non-batched operations.

D2 — WAL corruption: Silent loss of acknowledged durable records is unacceptable. My preferred policy is fail-stop on detected corruption that could conceal acknowledged data. Do not silently truncate and continue. Define the precise recovery contract and verify it with fault injection.

D3 — Client identity: Prevent a new client incarnation from incorrectly inheriting an earlier client's replay identity. Design an explicit incarnation/session identity mechanism compatible with durable replay semantics.

D4 — Administrative exposure: Prioritize restricting administrative and internal listeners to explicitly configured interfaces. Do not defer this unnecessarily while developing the other milestones.

These are separate engineering work slices. Do not combine them into PR #122.

Completion Requirements

For PR #122:

1. Implement the minimal CI foundation.
2. Run and document the applicable verification suite.
3. Obtain independent QA verification.
4. Provide a concise evidence summary with passing/failing counts.
5. Open PR #122 for review.

Stop after submitting PR #122. Do not merge it or begin the next implementation slice until I review it.

Our priority now is establishing a trustworthy correctness baseline, then systematically eliminating the confirmed architectural defects.

Always verify, never assume.