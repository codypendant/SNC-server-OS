# Codex review of PR #176 (filed verbatim)

Status (2026-11-19): PR #176 merged at `4c7035a` after this approval. **Nothing in this review is an owner decision,
and it is not the owner's go for the model campaign** ("This recommendation is not authorization to execute").
Corrections A-C are in `docs/AFR_001_REPORT_MODEL_PREREG.md` sections 9-10; the Y10 verification list is in
`docs/AFR_001_DURABILITY_DECISIONS.md` section 14.

---

SNC Server OS — Codex Review of PR #176

VERDICT: APPROVED FOR DOCUMENTATION MERGE. SUPPORT THE BOUNDED MODEL CAMPAIGN, SUBJECT TO PREREGISTRATION CORRECTIONS.

I reviewed PR #176, including "AFR_001_REPORT_MODEL_PREREG.md", the report-identity amendments, the Linux "RWF_DSYNC" analysis and the owner-decision package.

1. Formal-model direction

The proposed "ReportReseed.tla" verification campaign is justified.

Seven rounds of textual review have repeatedly found new protocol-interleaving counterexamples. We now need bounded state-space verification.

I support the proposed 17 counted launches, seven mutations, reachability probes, two-hour per-run cap, and predetermined fallback configurations.

However, resolve the following before executing the first counted run.

Required preregistration corrections

A. Acknowledgment eligibility

The proposed "Ack" action uses "|A[p]| ≥ 2" as a permissive approximation.

Reconcile this explicitly with the real protocol's "|S| ≥ m = 2", membership rules and authority grants.

The model must not identify an acknowledgment as a real SNC counterexample if the protocol would prohibit that acknowledgment.

If a deliberately permissive lane is useful for discovering bugs, identify it separately from the faithful-protocol acceptance lane.

B. Report provenance and coverage

Make the report's sender identity explicit in its message representation or immutable per-sender message collection.

Verify that counted coverage belongs to the correct positioning record and is invalidated when that record ends.

Ensure the message bounds and delivery/drop actions do not accidentally prevent the stale-report sequences the model is intended to exercise.

W-1 and W-2 remain proposed protocol rules, not accepted decisions. Record every assumption on which the passing model depends.

C. Three-way fidelity verification

Before counted execution, the implementing agent and two independent reviewers must verify the final TLA+ model against the preregistered protocol.

Review the specification itself, not merely its accompanying summary.

Only then perform sizing and counted execution.

The final model and corrected preregistration must be committed before counted runs.

2. Model requirements

Preserve the planned checks:

- AckSound.
- GrowSound.
- AckedPreserved.
- Entry uniqueness and log matching.
- Delayed and duplicated reports.
- Reporter incarnation and term.
- Entry identity.
- Reseed and wipe.
- Authority grants.
- Grow and promotion.
- Crashes and restarts.

Every negative mutation must be examined state by state.

A mutation failure is useful only if it represents a legitimate counterexample to the intended rule.

A completed correct-protocol violation blocks Y9.

A timeout is NOT COMPLETED, never a pass.

The untimed RS-0 grant guard does not verify the real-time grant wait across crashes. Keep that limitation explicit.

3. Y10 — RWF_DSYNC and direct recovery reads

The Linux source analysis supports further consideration of Y10.

I support it as a conditional design direction, not as an accepted storage contract.

Before acceptance, verify:

- Successful RWF_DSYNC semantics under direct and buffered fallback paths.
- Whether failed buffered writes can leave clean but non-durable cached data.
- Whether O_DIRECT recovery reads actually bypass that cache on the supported filesystem configuration.
- Whether a direct read initiates dirty-page writeback, and how that interacts with read-only recovery classification.
- Every WAL recovery and catch-up read path.
- The actual supported target-kernel behavior.

Do not remove the buffered-WAL exclusion merely because the proposed replacement sounds simpler.

Y10 requires a separate owner decision.

4. Model authorization

My recommendation to the owner is YES to the bounded model campaign once the above preregistration corrections and fidelity review are satisfied.

This recommendation is not authorization to execute.

Obtain the owner's explicit GO.

Do not expand the scope or increase the bounds after observing results.

5. Restrictions

Y9, Y10, W-1, W-2 and Y6c-2 remain unaccepted.

D-02, R-23 and O-FLOOR remain open.

Y4 and Y7 remain on HOLD.

No X-2, R640 runs, C++ or Rust implementation, wire-format change, migration or architectural freeze is authorized.

Preserve m = 2 and the existing guarantees.

STOP after the bounded model results and independent review. Submit the evidence for Codex and owner approval.

FINAL: PR #176 APPROVED FOR DOCUMENTATION MERGE. BOUNDED FORMAL MODEL RECOMMENDED, SUBJECT TO THE ABOVE CONDITIONS AND EXPLICIT OWNER AUTHORIZATION.
