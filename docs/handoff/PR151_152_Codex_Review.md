# Codex review and engineering direction on PRs #151 and #152 (relayed by cody, 2026-11-17 16:56 UTC)

Transcribed verbatim from cody's message in the project. PR #152 merged as `3ddd0d5` after repository checks.

---

SNC Server OS — Codex Review and Engineering Direction

PRs #151 and #152 | AFR-001

1. Review Decisions

PR #151 — APPROVED FOR MERGE

The targeted R9 standby-read-lease verification is accepted as bounded verification evidence.

The model exposed important protocol requirements that must be incorporated before R9 is frozen.

PR #152 — APPROVED FOR MERGE

The R2′ verified-survivor model results are accepted as bounded verification evidence.

The three primary verification runs passed:

- V1: 14,186,264 states.
- V2: 38,833,104 states.
- V3: 19,225,779 states.

Mutation testing also demonstrated that removing the survivor-reseed restriction or allowing premature READY can violate acknowledged-write preservation.

Neither R9 nor R2′ is authorized for final architectural freeze.

Both PRs may merge after normal repository checks.

2. R9 — Required Protocol Corrections

Apply the following corrections to "docs/AFR_001_SAFETY_ANALYSIS.md" and the corresponding Guarantees Specification.

R9-A: Apply before acknowledgment

Every member of acknowledgment set A must apply an entry before acknowledging it.

This requirement applies regardless of whether that member currently holds a standby read lease.

The model demonstrated that acknowledgments sent before a lease is granted can subsequently be counted while that lease is active.

R9-B: Lease eligibility

A standby read lease may be granted only when the member:

- Belongs to A.
- Has persisted the primary's current term.
- Is positioned on the primary's log.
- Has applied entries through at least "max(primary inherited log end, primary highest acknowledged LSN)".

R9-C: Degradation and acknowledgment ordering

The primary must not remove a standby from A while a previously granted standby read lease may remain valid.

Every acknowledgment must include all required members of A.

The separate prohibition on degradation may be retained as an explanatory consequence, but it is not an independent safety mechanism in the A-based protocol.

R9-D: Term fencing

Explicitly reference R4: a standby must stop serving lease reads when it learns a newer term.

R9-E: Lease timing

Specify that the standby lease and the primary-authority cap are anchored consistently at the standby's request-send time.

Retain the explicit clock-drift assumptions and the primary-side expiry wait.

Update G-30 accordingly.

3. R2′ — Verified Survivor Architecture

Continue with R2′ as the preferred architecture.

The bounded verification supports preserving eligible surviving replicas after promotion rather than automatically reducing S to the new primary alone.

The following remain mandatory:

- Durable term fencing.
- Log reconciliation before a survivor counts toward acknowledgments or grants.
- Removal from S before reseeding.
- READY only after the required survivor-positioning conditions are satisfied.
- Preservation of acknowledged writes across repeated promotions.

The model confirms that the reseed restriction and READY barrier are necessary.

However, do not claim that every R2′ rule has been independently proven necessary.

M2 and M2b did not establish the necessity of the positioning rule at the checked bounds. M3 did not distinguish KIP-101 from KIP-279.

Retain those limitations.

4. Critical Finding — Operator Promotion During Reconciliation

PR #152 identified a potential acknowledged-write-loss scenario:

A member can be promoted while it has a pending reconciliation or durable truncation intent from an earlier primary. After promotion, applying that stale intent may remove an entry that has already been acknowledged.

Automatic promotion is protected by the READY barrier, but operator promotion inside S can waive READY.

Required correction:

Extend P6 and S2 so that a promotion candidate cannot begin serving while an older-term reconciliation or truncation operation remains capable of changing its authoritative log.

The design must explicitly handle:

1. Pending durable truncation intents.
2. Reconciliation in progress against a previous primary.
3. Interrupted reconciliation across crashes.
4. Cancellation or completion of obsolete reconciliation work.
5. Recovery of the promoted node before accepting new writes.
6. Operator promotion that waives READY.

A proposed solution is to require reconciliation quiescence before serving, with a durable term-aware rule preventing stale operations from modifying the new primary's log.

Do not assume cancellation alone is sufficient. Verify the crash and recovery ordering.

5. Degraded-Primary Self-Re-Promotion

The proposed design remains:

A restarted primary that was the sole member of S must obtain a new witness-approved term before serving again.

It must not resume authority under its old term.

However, the existing bounded model has not demonstrated that the self-repromotion branch is reachable.

Add a targeted reachability and safety check covering:

- Restart of the sole surviving primary.
- Witness record recovery and comparison.
- Re-promotion to a new term.
- The R3 lease-expiry barrier.
- Durable term fencing.
- WAL continuity and "(term, LSN)" uniqueness.
- Refusal when the durable log cannot be trusted.

Keep the dependencies on AFR-D-02 explicit.

6. Cross-Protocol Verification

Prepare a narrowly scoped TLA+ follow-up covering the interaction of:

- R2′ survivor retention and positioning.
- R9 standby read leases.
- Operator promotion inside S.
- Pending truncation intents.
- Crash/restart.
- Acknowledgment preservation.

Prioritize a small model that can reproduce the identified failure and demonstrate the proposed correction.

Use mutation testing to verify that removing the correction reproduces the counterexample.

Do not simply increase state-space limits or launch another broad verification campaign.

Preregister the model bounds, expected counterexamples, runtime limits, and acceptance criteria before execution.

7. Independent Verification

Apply mandatory three-way verification.

The reviewers must independently examine:

- Protocol correctness and invariant preservation.
- Model completeness, reachability, and potentially hidden behavior.
- Downstream implications for the Rust architecture, WAL recovery, replication, witness consensus, and client-visible guarantees.

Any model-checking run that times out is NOT COMPLETED.

Any unexplained safety violation is a blocker.

Correct defects on site and rerun the affected verification.

8. Authorization and Stop Conditions

Authorized:

- Merge PRs #151 and #152 after repository checks.
- Correct the R9 and G-30 normative wording.
- Prepare the P6/S2 correction.
- Perform focused, bounded model verification on the existing non-R640 environment.
- Conduct independent QA.
- Produce a concise evidence-backed recommendation for the remaining architectural freeze decisions.

Not authorized:

- New C++ or Rust product implementation.
- R640 performance experiments.
- Final acceptance of AFR-D-02.
- Final freeze of R2′ or R9.
- Changes to the default "m = 2" policy.
- Expansion into unrelated architectural work.

Next deliverable: One focused PR containing the corrected protocol text, targeted model evidence, counterexample traces, QA findings, and explicit remaining limitations.

Stop after that PR is ready for Codex and owner review.

Maintain the established principles: always verify, never assume; fix on site; mandatory three-way verification; and evaluate system-wide consequences before accepting architectural changes.
