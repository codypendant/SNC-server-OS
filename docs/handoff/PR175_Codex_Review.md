# Codex review of PR #175 (filed verbatim)

Status (2026-11-19): PR #175 merged at `20cc2a5` after this approval. **Nothing in this review is an owner decision,
and it authorizes no model run.** Its section 3 plan is `docs/AFR_001_REPORT_MODEL_PREREG.md`; its section 4 analysis
is `docs/AFR_001_DURABILITY_DECISIONS.md` section 12.

---

SNC Server OS — Codex Review of PR #175

VERDICT: APPROVED FOR DOCUMENTATION MERGE. ARCHITECTURAL ACCEPTANCE REMAINS ON HOLD.

I reviewed PR #175, including the round-seven findings, revised Y9-T report identity, checkpoint/remap corrections, buffered-WAL analysis, and convergence assessment.

1. Engineering assessment

Round seven found two blockers and six major issues.

The proposed corrections address the identified cases, but the final corrections have not been independently re-reviewed.

The report/reseed/grant protocol is not converging through textual review alone.

I agree that the next justified verification effort is a bounded formal model of those protocol interactions.

2. Y9-T

The current proposed report identity includes:

- Entry "(term, LSN)".
- Reporter incarnation "(node nonce, generation)".
- Reporter's durable term.
- Verification against the primary's own log.
- Verification against the incarnation the primary positioned.
- No reports during pending truncation or incomplete/reseeding state.
- Applied-and-watermarked acknowledgment requirements.

These are reasonable design directions, but Y9-T is not accepted.

Do not modify frozen R9-A′ or the replication wire format yet.

3. Recommended formal verification

Prepare a narrowly scoped, preregistered model of the report/reseed protocol.

It must include:

- Delayed and duplicated acknowledgment reports.
- Entry identity and reporter incarnation.
- Reporter durable term.
- RS-0 grant wait and RS-1 incarnation change.
- R6.2 positioning and truncation.
- Grow and acknowledgment membership.
- Process crashes and restarts.
- Primary promotion, including R7 outside S.
- The m = 2 durability requirement.

The model must distinguish correct behavior from known incorrect variants.

Required negative mutations:

1. Remove reporter incarnation checking.
2. Remove reporter durable-term checking.
3. Remove the RS-0 grant wait.
4. Remove entry-term checking.
5. Permit a report during RESEEDING or PENDING.
6. Reuse the incarnation after a wipe and seed.

Use small bounded configurations initially: three or four nodes, up to three terms and three LSNs.

Define the exact safety invariants, reachability requirements, expected mutation failures, state-space limits and stopping criteria before running TLC.

This message recommends the model but does not authorize execution. Obtain the owner's explicit approval before starting the formal-model campaign.

4. Buffered WAL

I support the proposed direction of refusing unsupported buffered WAL configurations.

However, the kernel's internal fallback from direct I/O to buffered I/O remains an unresolved premise.

Do not claim that the proposed startup gate proves every write remained direct.

Verify the relevant ext4/XFS behavior and determine whether the durability contract can instead rely on "RWF_DSYNC" semantics independent of the actual I/O path.

Any supported-platform restriction requires an explicit owner decision.

5. Checkpoint and remap

Retain the round-seven corrections:

- Explicit "start_lsn = E + 1" and "max_lsn = E" when checkpointing without an open writer.
- Durable initialization of new streams before publication.
- No unlogged session expiry or reclaim during the remap checkpoint.
- Compute recovered "E" from the complete retained-history pass.

These remain design requirements, not implemented fixes.

6. Restrictions

- Y9 and Y6c-2 remain unaccepted.
- Y4 and Y7 remain on HOLD.
- D-02, R-23 and O-FLOOR remain open.
- No implementation, wire-format change, migration or architectural freeze.
- No X-2 or R640 execution.
- No model or experiment without separate owner authorization.
- Preserve m = 2 and existing guarantees.

STOP after preparing the preregistered formal-model plan and submit it for Codex and owner approval.

FINAL: PR #175 APPROVED FOR DOCUMENTATION MERGE. RECOMMEND A BOUNDED FORMAL MODEL AS THE NEXT STEP.
