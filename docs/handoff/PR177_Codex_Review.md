# Codex review of PR #177 (filed verbatim)

Status (2026-11-19): PR #177 merged after this approval. The owner gave the explicit GO for the bounded
ReportReseed campaign on 2026-11-19 16:06 UTC, so the steps in section 5 are authorized. Nothing in this review accepts
Y9, Y10, W-1, W-2 or Y6c-2.

---

SNC Server OS — Codex Review of PR #177

VERDICT: APPROVED FOR DOCUMENTATION MERGE.

I reviewed PR #177, including the corrected report/reseed preregistration, assumptions, three-way verification requirements, and Y10 kernel analysis.

1. Preregistration corrections

Corrections A, B and C are satisfactory.

A — Acknowledgment eligibility

The model now includes the primary's own view of S, the requirement that S contain at least two members, S being contained in A, and the authority-grant requirements.

A missed shrink is represented by a lost reply leaving the primary's view stale.

Maintain these faithful-protocol conditions. Do not introduce a permissive acceptance lane.

B — Report provenance and coverage

Reports identify their sender and remain immutable.

Counted coverage is tied to the positioning record and resets when that record ends or the primary changes term.

The message bounds and assumption register are documented.

W-1 and W-2 remain proposed protocol requirements, not accepted decisions.

C — Independent verification

Before any counted run, require the model author and two independent reviewers to inspect the actual TLA+ specification.

Correct and record fidelity defects before sizing.

Commit the model, configurations and final preregistration before counted execution.

2. Formal-model recommendation

Codex recommends YES to the bounded ReportReseed.tla campaign.

The plan includes:

- Two bounded configurations.
- Seventeen counted launches.
- Seven negative mutations.
- Reachability checks.
- Predetermined fallbacks and rerun limits.
- A two-hour cap per run.
- Explicit stop conditions.

Do not expand bounds or change expected results after seeing outcomes without recording a model defect or protocol finding under the existing rules.

A timeout is NOT COMPLETED.

Every counterexample must be inspected state by state.

A passing model establishes evidence only within its assumptions and bounds.

The campaign still requires the owner's explicit GO. This review alone does not authorize execution.

3. Assumptions that must survive into the results

The final report must explicitly preserve:

- W-1: operator wipe cannot occur while old authority grants remain valid.
- W-2: counted coverage ends with its positioning record.
- AS-3: the untimed grant guard does not verify the real-time wait or its behavior across crashes.
- Premise P: reseeding copies a valid prefix of one primary's log.
- D-02 remains outside the modeled storage-fault behavior.
- Witness and reconciliation abstractions are bounded.

Do not use a PASS to claim these unresolved assumptions have been proven.

4. Y10 — Direct-I/O recovery reads

The source analysis supports the proposed direction, but does not complete its acceptance.

Continue to hold Y10 pending:

1. A decision on whether recovery's first phase may trigger dirty-page writeback.
2. Verification that every required recovery and catch-up read uses the intended direct-I/O path.
3. Target-platform confirmation.
4. Resolution of the remaining kernel and hardware assumptions.

Retain the buffered-WAL exclusion until a reviewed replacement contract is explicitly approved.

No product-code changes are authorized.

5. Next authorized step

After the owner's explicit GO:

1. Implement the bounded TLA+ model exactly within the preregistered scope.
2. Complete three-way specification fidelity review.
3. Conduct the permitted sizing runs.
4. Execute the counted campaign within its budget.
5. Independently review every mutation counterexample and the final results.
6. Submit the evidence and unresolved findings for Codex and owner review.

Stop when the authorized budget or conditions require it.

6. Restrictions

- Y9, Y10, W-1, W-2 and Y6c-2 remain unaccepted.
- Y4 and Y7 remain on HOLD.
- D-02, R-23 and O-FLOOR remain open.
- No X-2 or R640 execution.
- No unrelated experiments.
- No C++ or Rust implementation.
- No wire-format change.
- No migration or architectural freeze.
- Preserve m = 2 and existing guarantees.

FINAL: PR #177 APPROVED FOR DOCUMENTATION MERGE. CODEX RECOMMENDS OWNER AUTHORIZATION OF THE BOUNDED MODEL CAMPAIGN.
