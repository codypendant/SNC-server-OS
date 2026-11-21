# Codex review of PR #184 (2026-11-21)

Relayed verbatim by cody in the project chat, 2026-11-21 17:42 UTC.

---

SNC — Codex Review of PR #184
VERDICT: HOLD — TWO TARGETED PREREGISTRATION CORRECTIONS REQUIRED.
OWNER DECISION RECORD: ACCEPTABLE.
PRODUCT ADR AND S1 SCOPE UPDATE: ACCEPTABLE.
AR-1 PREREGISTRATION: NOT YET APPROVED.
I reviewed PR #184, including:

* `docs/AR1_PREREG.md`
* `docs/handoff/PR183_Owner_Decisions.md`
* `docs/AGENT_RUNTIME_DECISION_PACKAGE.md`
* `docs/adr/SNC-ADR-PRODUCT-001.md`
* `docs/EXPERIMENTAL_VALIDATION.md`
* `docs/STATUS.md`

The recorded owner decisions match the limited approval of OD-P1 through OD-P7. OD-P8 remains noted, not independently authorized.
The focused state-native agent runtime is now SNC's proposed first product.
Enterprise governance remains optional S3 work. General-SNC database research is suspended, not abandoned.
No AR-1 implementation or execution has been authorized yet.
1. BLOCKER — Incorrect durability oracle
`AR1_PREREG.md` section 7.1 defines established commits at a kill using the standby's WAL receive position, read after resuming the standby if it was stopped.
This can overstate what was durable at the actual kill.
A standby may receive and flush previously outstanding WAL after the application was killed.
The later receive position is therefore not an independent snapshot of the earlier durability cut.
Required correction:
Define a time-correct, independently verifiable durability oracle.
The test must distinguish:

* Durable on the standby before the kill.
* Committed locally but not established at the kill.
* Established only after recovery or replication resumes.
* Lost across a timeline change.

Do not use a later advancing receive position to infer the earlier cut.
The checker must retain sufficient evidence to establish ordering independently of the runtime's own establishment-decision log.
Review the corrected oracle against OI-1, the crash matrix and item 1's authoritative-state criterion.
2. BLOCKER — M2 cannot exercise the intended race
Mutation M2 removes the epoch check and is assigned to C-a1.
C-a1 pauses the old activation while its transaction holds the agent-row lock.
A takeover cannot advance that epoch until the old transaction releases the conflicting lock.
Therefore, the intended stale-epoch decision may be unreachable at that point, even with the epoch predicate removed.
Required correction:
Create a concrete schedule in which:

1. The old activation remains alive or has an outstanding call.
2. The takeover commits a newer epoch.
3. The old activation subsequently attempts a new consequential decision.
4. With the epoch check enabled, the transaction is rejected.
5. With the check removed, the decision can commit and the independent checker detects it.

Use a named fault point and reached-point marker.
C-b2 followed by a stale activation's subsequent memory or task update is a candidate.
Do not disable the row lock merely to make the mutation fail unless the mutation is explicitly testing lock discipline rather than epoch fencing.
Update the mutation observability table and adequacy requirements accordingly.
3. Preserve the Accepted Owner Decisions
Do not reopen the product strategy while correcting these test defects.
The accepted limited direction is:

* S1: focused state-native agent runtime.
* S2: subsequent HA, scale and operational verification.
* S3: optional enterprise governance expansion.
* PostgreSQL and DBOS Python: experimental candidates only.
* Temporal: retained alternative.
* Original SNC research: preserved.
* Custom replication: not automatically resumed.

No production dependencies, final language or new durability class have been approved.
4. Preserve AR-1's Scope
Keep the existing experiment structure:

* Phase 0 as the first hard gate.
* Independent checker and mutation adequacy.
* The named crash-point matrix.
* Fault-point reachability verification.
* Plain DBOS comparison.
* Explicit stop conditions.
* Separate G1/G2 and G3 authorization.
* No R640 work.
* No S2 or S3 expansion.

Make only the corrections necessary to establish valid verification evidence.
5. Review Process
Fix the two findings on site.
Then conduct a focused three-way adversarial review of:

* The corrected durability-cut oracle.
* The stale-epoch mutation schedule.
* Their interactions with the checker and outcome classification.

Do not modify unrelated architecture or expand the test campaign.
Submit the corrected PR for Codex approval.
6. Authorization
OD-P7 authorized writing and reviewing this preregistration, not implementation.
This review does not authorize G1, G2 or G3.
The owner must explicitly approve the corrected preregistration before AR-1 code and uncounted bring-up begin.
Counted runs require a later, separate GO.
STOP after the two corrections and their independent review.
FINAL: PR #184 HOLD PENDING TWO TARGETED VERIFICATION FIXES. THE RECORDED PRODUCT DIRECTION AND OWNER DECISIONS ARE ACCEPTABLE.
