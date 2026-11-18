# Codex review of PR #160 and next task (relayed by cody, 2026-11-18 13:23 UTC)

Pasted by cody into the project as text. Copied verbatim below; only the Markdown table syntax is restored.

---

SNC Server OS — Codex Review of PR #160 and External Storage-System Research

## 1. PR #160 — APPROVED FOR MERGE

I reviewed PR #160, including:

- Completion.tla revision 3.
- The preregistered verification results.
- The independent adversarial review.
- The D-06 storage-format decision package.
- The unresolved architectural and durability obligations.

Verdict: APPROVE PR #160 for documentation/model merge.

The verification results are acceptable within their stated bounds:

| Verification | Result |
|---|---|
| R3-D1 | PASS — 4,882,297 states |
| R3-D2, three crashes | PASS — 31,502,068 states |
| R3-T3, three truncations | PASS — 210,723,596 states |
| Nine negative mutations | All failed as expected |
| Reachability probes | All reached |

The model now covers additional failure scenarios involving header rewriting, stale temporary files, spare rollover,
and catch-up reads past the end.

The independent review found no remaining model defect within the tested scope.

These are bounded verification results, not proof of complete storage-system safety.

## 2. D-06 — Architectural Recommendation

I recommend that the owner conditionally accept D-06 items 4–14 as SNC's selected storage-format direction.

This recommendation is subject to the owner's explicit approval. PR #160 itself does not record that acceptance.

The following conditions are mandatory:

1. Final D-06 acceptance remains dependent on D-02.
2. Metadata rollback, R-23, remains an unresolved safety blocker.
3. O-FLOOR must be discharged before checkpoint/reclaim conformance work.
4. Performance costs must be measured before performance claims are made.
5. No final architectural freeze is authorized.
6. No C++ or Rust implementation is authorized by this decision.

Preserve the conditionally frozen R2′ and R9-A through R9-E contracts.

R9-F remains outside the freeze.

The default replication admission threshold remains m = 2.

Do not silently reinterpret the guarantees to accommodate implementation limitations.

## 3. External Research — Established Storage Systems

We have reviewed relevant engineering approaches from:

- RocksDB
- PostgreSQL
- etcd
- CockroachDB
- Raft
- Kafka
- LMDB
- TiKV

The important conclusion is that SNC's unresolved problems are not unique. Mature storage systems have encountered
closely related failures involving WAL persistence, checkpoint publication, metadata consistency, replication state,
and crash recovery.

However, similar mechanisms do not necessarily provide identical durability guarantees.

### RocksDB

Investigate its WAL-in-MANIFEST tracking and WAL verification mechanisms.

Key questions:

- How are required WAL files identified?
- What persisted information detects missing or shortened WAL files?
- When is that information synchronized?
- Can the newest acknowledged WAL flush disappear without detection?
- What is the measured or documented write-path overhead?

Relevance: D-02, R-23, O-FLOOR.

### etcd

Investigate its historical data-inconsistency issue involving the persisted consistent index and WAL application.

Key questions:

- What ordering or atomicity guarantee was violated?
- How was the defect corrected?
- How are snapshot state, applied indexes, and WAL durability coordinated?
- What crash-injection or regression tests protect the corrected behavior?

Relevance: metadata atomicity, checkpoint recovery, and durable progress tracking.

### CockroachDB

Investigate its Raft log truncation, snapshot coordination, and replica garbage collection.

Key questions:

- How are in-flight snapshots protected against premature log reclamation?
- How are committed, applying, and applied positions distinguished?
- How does recovery reconcile persisted Raft state with replicated application state?
- Which retention rules prevent a replica from losing required history?

Relevance: R2′, reconciliation, checkpoint retention, O-FLOOR.

### PostgreSQL

Investigate pg_control, checkpoint records, WAL replay, and recovery refusal behavior.

Key questions:

- How is the authoritative recovery checkpoint selected?
- What protects checkpoint metadata against torn writes?
- What happens when pg_control is corrupted?
- Which missing WAL conditions cause recovery to refuse?
- Can an older internally consistent metadata state be mistaken for current state?

Relevance: D-02, R-23, checkpoint authority.

### Raft, Kafka, LMDB, and TiKV

Use these as targeted comparisons for:

- Per-record term histories and snapshot boundaries.
- Leader epochs and log reconciliation.
- Alternating metadata copies.
- Atomic batches and crash recovery.
- Durability boundaries and failure assumptions.

## 4. Important Safety Principle

CRC checks, generation numbers, and alternating A/B metadata copies are insufficient by themselves to detect every
form of silent rollback.

An older metadata copy may remain internally valid even after a newer acknowledged state disappears.

Likewise, a WAL reader cannot automatically distinguish an unwritten tail from an acknowledged tail that was silently
lost.

Research must identify exactly what independent durable evidence each system uses.

If a system cannot detect a particular failure, record that limitation rather than treating its implementation as
proof of safety.

Do not weaken SNC-G-14 to match another system's behavior.

## 5. Next Authorized Task — Focused Comparative Research

Perform a source-level engineering investigation of the systems above, prioritizing D-02, R-23, and O-FLOOR.

This is research and documentation only.

For each relevant mechanism, provide:

1. The actual upstream implementation, including repository paths, functions, and pinned versions or commits.
2. The specific failure mode addressed.
3. The persistent metadata or WAL structures involved.
4. The exact write, synchronization, acknowledgment, and recovery ordering.
5. What failures the mechanism detects.
6. What failures it does not detect.
7. Any available performance measurements.
8. The implications for SNC's existing D-06 architecture.

Distinguish verified source-code behavior from documentation claims, inference, and untested hypotheses.

Use primary sources. Do not rely solely on secondary summaries or AI-generated explanations.

Required deliverable

Create a focused engineering comparison in the repository, with:

- A mechanism-by-mechanism comparison matrix.
- A failure-coverage matrix against SNC's G-14 and related guarantees.
- Concrete upstream source references.
- Candidate approaches SNC could adapt.
- Compatibility analysis against the existing conditionally frozen protocol.
- Expected performance and implementation tradeoffs.
- A recommended direction for the eventual D-02 decision.
- Explicit unresolved questions and required verification.

The recommendation must distinguish a promising design from an accepted design.

Do not automatically select a D-02 mechanism.

## 6. Engineering Discipline and Stop Conditions

Continue enforcing:

Always verify, never assume. Fix on site. Evaluate the entire system and downstream dependencies.

Use independent three-way review of the research conclusions and their architectural implications.

Do not start another broad formal-verification campaign.

Do not run X-2, the R640 benchmarks, or implementation experiments.

Do not modify product C++ or Rust.

Do not authorize migration work.

Do not accept D-02 or declare R-23 resolved.

Do not expand the research indefinitely. Once the relevant upstream mechanisms have been inspected and the comparative
evidence is sufficient to support a decision, stop.

Submit the research document for Codex and owner review before any further architectural commitment.

Final authorization: PR #160 may merge. The next task is the focused external storage-system comparison,
documentation only. D-06 conditional acceptance remains a separate owner decision.
