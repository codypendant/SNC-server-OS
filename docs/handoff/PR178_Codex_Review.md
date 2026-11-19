# Codex review of PR #178 (filed verbatim)

Status (2026-11-19): PR #178 merged at `3f07bc4` after this approval. The owner said they intend a yes on the
live-grant direction but wanted this review first; this review says investigate, do not accept. **P1-P3 are proposals
under investigation, not decisions. Y9-T is BLOCKED.** The revision package it asks for is
`docs/AFR_001_LIVE_GRANT_PROPOSAL.md`.

---

SNC Server OS — Codex Review of PR #178

VERDICT: APPROVED FOR MODEL AND EVIDENCE MERGE. Y9-T BLOCKED. P1–P3 NOT YET ACCEPTED.

I reviewed PR #178, including "ReportReseed.tla", the preregistration amendments, the two TLC counterexample modules, the configuration generator and the three-way fidelity findings.

1. Review outcome

The verification campaign correctly stopped before any counted run.

The fidelity review identified failures with the proposed rules enabled:

- F-1: stale coverage permits Grow during reseed.
- F-2: an R7 outside-S promotion permits truncation while an older authority grant remains valid, followed by an unsound acknowledgment.
- F-3: wipe and reseed from a deposed primary leaves another primary's positioning record valid.
- H-1: a delayed R6.2 request can reestablish an obsolete incarnation; the existing model cannot represent that interleaving.

F-2 and F-3 have fixed-action TLC counterexamples.

The uncounted random simulations are diagnostic evidence only, not exhaustive verification.

Y9-T as currently written is blocked. Do not run the remaining counted campaign against this known-failing protocol.

2. P1–P3 evaluation

I support investigating the live-promise architecture.

P1 — Incarnation-bound authority grants

A counted report must match a live authority grant identifying the same reporter incarnation.

Verify the matching rule across:

- Acknowledgments.
- Grow.
- Lease eligibility.
- Term changes.
- Restarts and reseeds.
- Delayed grant and report messages.

P2 — No destructive action while a grant remains valid

Evaluate the rule for:

- R6.2 truncation.
- RS-2 reseed discard.
- Operator wipe.
- Interrupted operations.
- Repeated crashes.
- Term changes and promotion.

Quantify the maximum additional wait and its interaction with G-48 and availability.

P3 — Witness-version CAS before RS-1

A witness-version bump must prevent an already proposed Grow from being chosen after the reseed decision.

Verify the entire interleaving:

1. Primary proposes Grow.
2. Replica stops granting and waits for old grants to expire.
3. Replica performs its witness CAS.
4. Grow either commits or conflicts.
5. Replica durably enters RESEEDING.
6. Any new Grow attempt after the CAS is also prohibited until the replica is eligible again.

A version bump alone does not prove that every later Grow is safe. The complete membership and grant conditions must establish this.

These are proposed architectural rules. No acceptance or implementation is authorized.

3. Required protocol analysis

Before another preregistration, resolve:

- Whether F-2 lies within an existing R7/G-35 unsafe-promotion exception or demonstrates an additional violation of acknowledgment safety.
- The exact scope of the live-grant promise.
- How delayed R6.2 requests are rejected or prevented from restoring obsolete positioning.
- How incarnations, request numbers and report identities interact across wipe and reseed.
- Whether existing R9-A′, R9-B, R9-C, R1.1, R3 and R6.3′ require recorded revisions.
- Whether W-1, W-2 and W-3 become redundant under P1–P3 or remain necessary independently.

Do not assume the new rules are sufficient merely because they address the known counterexamples.

4. Next recommended work

Prepare one bounded architectural revision package for P1–P3.

It must contain:

1. Exact proposed protocol text.
2. All changes to accepted or frozen contracts.
3. A complete message and state-transition definition.
4. Explicit protection against F-1, F-2, F-3 and H-1.
5. Availability and latency costs.
6. A revised model plan addressing the fidelity defects in section 12.3 of PR #178.
7. Negative mutations that remove each essential protection.
8. Independent three-way review before proposing counted verification.

Keep the new model faithful to the actual acknowledgment and membership rules.

Use the existing counterexample traces as mandatory regression cases.

Do not run a new counted campaign without separate owner authorization.

5. Restrictions

- Y9 remains BLOCKED.
- P1–P3 are proposals, not accepted decisions.
- Y10, W-1, W-2, W-3, Y6c-2 and E32 remain unaccepted.
- D-02, R-23 and O-FLOOR remain open.
- Y4 and Y7 remain on HOLD.
- No X-2 or R640 execution.
- No C++ or Rust implementation.
- No unauthorized model or experiment.
- No wire-format changes, migration or architectural freeze.
- Preserve m = 2 and existing guarantees.

STOP after the revised architectural proposal and model plan. Submit them for Codex and owner approval.

FINAL: PR #178 APPROVED FOR EVIDENCE MERGE. THE EXISTING Y9-T DESIGN FAILS VERIFICATION. RECOMMEND INVESTIGATING P1–P3, NOT ACCEPTING THEM WITHOUT EVIDENCE.
