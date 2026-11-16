# SNC-AFR-001: Handoff 1 to Codex (plan as amended, Phase 1 status)

**From:** Claude (Claude Code), lead reviewer for SNC-AFR-001
**Date:** 2026-11-15 UTC
**Baseline:** `main` at `24006ee` (PR #135 merged). Clean RelWithDebInfo build of that commit succeeded.
**Nature:** read-only review. No product code, test or format is changed. No architecture is accepted.

## 1. Amendments cody accepted, and how the review applies them

1. **Narrow the depth, not the list.** All twelve domains (A to L) are still covered. The deep passes go to the
   areas that are costly to reverse: the object model and identity, transaction and durability boundaries,
   on-disk formats, the consistency contract, and the wire protocol. CLI, developer experience and
   observability get an inventory and a lighter assessment.
2. **A guarantees specification is the main output.** One document states what SNC promises, clause by
   clause, each clause tagged VERIFIED / DOCUMENTED-ONLY / UNTESTED / KNOWN-VIOLATED with evidence.
   Differential testing judges C++ and Rust against that document, not against C++ behavior. It builds on
   `docs/CORE_CONTRACTS.md` and `docs/FAILURE_MODEL.md` rather than replacing them.
3. **Rule on the known defects first.** Each gap on record gets a proposed ruling, DEFECT (the reference must
   be fixed or the behavior marked as a known C++ defect in the differential harness) or ACCEPTED BEHAVIOR
   (written into the spec as a stated limit). cody accepts or rejects each ruling. Gaps in scope:
   - Audit D1: cross-shard overtaking of a batched EXEC by a non-batchable request.
   - Audit D2: mid-record corruption in the newest WAL segment drops acknowledged writes silently.
   - Audit D3: a reused `client_id` gets a false `OK, replayed`.
   - Audit D4: no authentication; admin, witness, seed and node-link listeners bind all interfaces.
   - A cut-off old primary may serve stale reads.
   - Async standby promotion without acknowledged writes (R08).
   - Standby receive is unbounded up to the 1 GiB WAL maximum.
   - Paxos F9 (witness acceptor state loss), excluded only by `--new-state 1` on first start.
   - F6 (degraded mode has no back-pressure), F7 (readiness latency with a stopped witness).
   - O1 (a resumed old primary reports `role=p` at its old term).
   - Session TTL expiry and sessions that do not move with slots (duplicate routes across a move).
   - test_xnode X16 flake (recorded as a test race; to be confirmed, not assumed).
   Each is re-checked against current source before a ruling is proposed; nothing is taken from the
   2026-11-13 audit on trust.
4. **Rust readiness is tied to the validation decision.** The readiness criteria (Deliverable 7) will propose
   a GO on the open experimental-validation GO / REFINE / NARROW / STOP decision as an entry criterion.
5. **Codex is the independent check.** The findings register, the proposed rulings and the guarantees spec go
   into the repository (`docs/`) so Codex can read them, and nothing in them is treated as accepted until
   Codex's review comes back through cody.

## 2. Phase 1 status (reconnaissance and inventory)

- Done: read SNC-ADR-RUST-001, AGENTS.md (invariants 1-18 and the latest log), STATUS.md, the PR #121
  architecture and evidence audit and its approval. Audit defects D1 to D4 are still open on `main`: every
  merge since PR #122 was CI, F1, checkpoint failover, test_migrate, control connections (K0), CPU placement
  or D2-a.
- Running: four bounded, independent inventory reviews, each citing file:line for every claim:
  (a) object model, execution, persistence, runtime and memory; (b) replication, HA, Paxos, placement,
  migration; (c) every executable's real flags, outputs and exit codes, every listener and protocol;
  (d) tests, CI, fault injection, observability and the governing-document map.
- Next: verify the gaps above against source, then independent QA of the inventory and of any critical
  finding, then a docs-only PR with Deliverable 1 (inventory) and the immediately critical findings. Work
  then **stops for cody's and Codex's review** before broad recommendations, the guarantees spec or the
  roadmap.

## 3. Isolation from active work

The shard-count slices D3 to D5 (CPU placement thread, PR #136 open) and K1 (on hold) are not touched. This
review changes no code. A critical defect found here is documented and put forward as its own slice.

## 4. Points where Codex's view is wanted now

1. **Label collisions.** "D1 to D5" means audit defects in `ARCHITECTURE_EVIDENCE_AUDIT_20261113.md` and
   CPU-placement items in `CPU_PLACEMENT_R1.md`; "F1 to F9" is reused across several documents. The review
   proposes prefixing every ID it issues (`AFR-F-nn` for findings, `AFR-G-nn` for spec clauses) and citing
   older IDs with their source document. Agree?
2. **The external v0.1 specification.** README and STATUS point to an artifact outside the repository as the
   authoritative spec. If no copy is in the repository, the review will recommend importing it (verbatim) so
   the guarantees spec has an in-repo predecessor. Agree?
3. **Ruling format.** Proposed per gap: current behavior with evidence, proposed ruling (DEFECT or ACCEPTED
   BEHAVIOR), the spec clause it implies, the differential-harness treatment, and the smallest fix or
   experiment. Any field Codex wants added?
