# Codex review of PR #168 (consolidated D-02 acceptance review)

Pasted by cody on 2026-11-18 at 23:09 UTC and relayed by the coordinator. It is reproduced verbatim below.

**Status (Claude):**
- #168 was merged (107b157) on Codex's approval for documentation merge.
- Y1, Y2 and Y3 were Codex's YES recommendations "as a design direction". cody accepted them at 23:09:37 UTC
  ("Yes, I'm going with your recommendations."). They are recorded as OD-7 to OD-9 in
  `docs/handoff/PR168_Owner_Decisions.md`, as design directions only.
- Y4, Y5 and Y6 stay on HOLD.
- The next task, the focused design note (sections A-E), was started: `docs/AFR_001_D02_RESTART_RESEED_NOTE.md`.

---

SNC Server OS — Codex Review of PR #168

VERDICT: APPROVED FOR DOCUMENTATION MERGE. D-02 REMAINS ON HOLD.

I reviewed PR #168 and its consolidated D-02 architectural acceptance analysis.

The review is accepted as a documentation deliverable, not as acceptance of the proposed storage mechanisms.

Owner decision recommendations

Y1 — YES, as a design direction. R-CHAIN should validate every retained WAL segment after the first. Do not narrow the check to the newest segment.

Y2 — YES, as a design direction. Use HeaderFirst spare ordering: write and synchronize the take-time header before rename, with "RENAME_NOREPLACE" and a clearly defined "spare.tmp" recovery namespace.

Y3 — YES, as a design direction. A replaced WAL segment preserves its nonce and identity.

Y4 — HOLD. Do not revise the conditionally accepted D-06 format until the complete changes are written, reconciled, and reviewed.

Y5 — HOLD formal acceptance. The intended safety interpretation is that a standby cannot count toward HA_DURABLE until its watermark covers the entry. Examine R9-A/R9-B and "ack_p" carefully. If this requires revising frozen text, identify it explicitly and request owner approval.

Y6 — HOLD. The restart-safety and per-stream reseed procedures must be written and independently reviewed before acceptance.

The YES decisions above authorize recording the selected design directions only. They do not authorize implementation or a final D-06 revision.

Next authorized task

Prepare one focused design note, documentation only, covering the following.

A. V8 restart safety

Specify how SNC prevents asynchronous writes from a terminated process from modifying state after a new process begins recovery.

Address:

- Replacement of the newest WAL segment after every restart.
- Outstanding "O_DIRECT" writes.
- Protection of node-record and watermark files.
- V8-L versus V8-A, with a justified recommendation.
- Lock lifetime, open file descriptions, and recovery ordering.
- Crash-only recovery refusal under G-48.
- Block reuse and the remaining C15 interaction.

Do not assume the existing directory lock protects outstanding direct I/O.

B. Spare-header and temporary-file handling

Specify:

- HeaderFirst ordering.
- Durable header write before rename.
- "RENAME_NOREPLACE".
- Distinct temporary-file namespaces.
- Recovery of an interrupted spare adoption.
- Recovery of an interrupted segment replacement.
- Conditions under which "EEXIST" is a legitimate refusal versus a crash-only recoverable state.

C. Per-stream reseed

Write the complete RS-0 through RS-6 procedure.

Verify its interaction with:

- The node-wide metadata authority.
- The RESEEDING intent.
- The stream WAL and watermark writers.
- Durable deletion of old stream files.
- Segment identity and predecessor links.
- The checkpoint manifest and compaction floor.
- Node-wide checkpoints during RESEEDING.
- Repeated crashes and recovery.
- Incarnation uniqueness and stream remapping.

Do not assume the existing whole-node seed implementation satisfies R6.5.

D. Acknowledgment semantics

Independently review write-path rule 4 against frozen R2′, R9-A and R9-B.

The required safety property is that an HA_DURABLE acknowledgment never counts a replica whose durable watermark does not cover the acknowledged entry.

Determine whether this is already required by the frozen text or requires a recorded revision.

Do not silently change "ack_p" or the meaning of HA_DURABLE.

E. D-06 reconciliation

Complete the proposed revision inventory for D-06 items 4–14, including:

- Four-slot node metadata.
- R-CHAIN and "prev_nonce".
- HeaderFirst spare adoption.
- Same-nonce segment replacement.
- V8 protection.
- Watermark files and truncation ordering.
- Per-stream reseed and checkpoint/floor interactions.

Identify every conflict with conditionally accepted text. Do not silently replace that text.

Verification requirements

Use independent three-way adversarial review.

Verify claims against actual source code and the frozen contracts.

Record counterexamples and unresolved findings.

Fix documentation inconsistencies on site.

Do not start a new TLA+ revision, X-2, R640 testing, C++ or Rust implementation, or migration work.

Stop condition

Submit the completed design note and its independent review for Codex and owner approval.

D-02, R-23, O-FLOOR and final D-06 acceptance remain open.

PR #168 is approved for documentation merge. The next authorized task is the focused design note, not implementation or another model campaign.
