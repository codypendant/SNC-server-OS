# Codex review of PR #185 (AR-1 G2 evidence)

Pasted by cody in the project chat on 2026-11-22 at 02:00 UTC. It is filed verbatim below.

---

SNC — Codex Review of PR #185
VERDICT: HOLD — G2 NOT COMPLETE.
G3 COUNTED RUNS: NOT AUTHORIZED.
The implementation and evidence represent meaningful progress, but the preregistered checker-adequacy requirements are not yet satisfied.
1. Accepted Preliminary Evidence

* Phase 0 passed at bring-up scale.
* SNC completed 246/246 uncounted correct runs.
* Plain DBOS step 1 produced 148 violations and 98 missing-point results.
* Twelve of fourteen mutations were fully detected.
* No counted runs occurred.

These are preliminary results, not completed verification.
Preserve the evidence and its limitations.
2. Required Corrections
A. M5
The current a3 schedule cannot exercise the intended unestablished-state policy read because synchronous commits block.
Develop a valid controlled schedule that exposes locally committed but unestablished state to the policy decision.
Require a correct-run positive control and a matching mutation that the independent checker detects.
B. M13
Resolve why the T-F and T-N cases are unexercised.
Either provide valid schedules or demonstrate that the expected derivation obligation cannot arise in those cases. Any change to the preregistered mutation table requires an explicit justification and review.
C. Unexercised invariants
Add positive coverage for:

* `1.upper`
* `4.policy_read`
* `5.TF_stale_rejected`
* `5.TN_duplicate_recorded`

If an invariant is unreachable under the contract, prove that and revise the preregistration. Do not silently waive it.
D. M1 independent detection
M1's 20/20 detections currently come exclusively from `4.gateway`.
A dishonest or incorrect gateway establishment declaration is not equivalent to proving that an external request escaped before establishment.
Construct a controlled unestablished-dispatch test in which the receiver actually observes the premature request.
The independent checker must detect the unsafe ordering without depending on the gateway's own claim.
E. Model regeneration
The stub currently derives its provider request ID deterministically from the SNC invocation ID.
Assign a fresh, independently identifiable provider request ID to each physical model request.
Verify that the checker distinguishes an unauthorized second request from the original response and detects the relevant M3 regeneration behavior.
3. Preregistration Amendments
Support A-1 as the correct direction: use the commit record's actual end LSN rather than the next WAL record's start.
Support A-2 as a proposed page-boundary correction, subject to focused verification of short headers, long headers, page crossings and segment boundaries.
Update `docs/AR1_PREREG.md` and the relevant architecture document before G3.
Preserve the original preregistration and document exactly what changed and why.
4. Additional Open Issues
Resolve or explicitly classify the remaining G2 report issues, particularly:

* U-7: model output replayed from a DBOS checkpoint.
* U-8: transaction snapshot order versus WAL-prefix ordering.
* U-9: the pre-resume durability cut assumes the WAL receiver remains stopped.
* U-11: dependency reproducibility and pinned hashes.
* U-12: run-cap accounting.

The G2 cloud-session deviation is disclosed. Counted runs must use the approved dedicated VM and frozen environment.
5. Verification Process
Fix on site.
Perform targeted independent review of the corrections and their interaction with the checker.
Rerun the affected uncounted schedules, positive controls and mutation adequacy.
Do not expand the experiment into S2, enterprise governance, custom replication or unrelated performance work.
Return the amended PR with:

* A complete adequacy table.
* Correct-run coverage for every required clause.
* Evidence that every fault point is reachable.
* The accepted preregistration amendments.
* Remaining limitations clearly identified.

6. Authorization
Existing G2 authorization permits the narrowly scoped corrective work and uncounted verification, subject to established project controls.
Do not start G3.
The owner must receive a completed G2 report and separately authorize counted execution.
FINAL: HOLD PR #185 AS G2-COMPLETE. CORRECT THE COVERAGE AND CHECKER ISSUES BEFORE G3 REVIEW.
