# Reference architecture research

A lane that runs alongside SNC Core work. It challenges SNC's design with
mature systems and published research, and feeds design reviews; it
builds nothing in Core by itself. The program, as cody handed it over on
2026-11-04: [REFERENCE_ARCHITECTURE_PROGRAM.md](REFERENCE_ARCHITECTURE_PROGRAM.md).
Each system review follows [TEMPLATE.md](TEMPLATE.md).

**Feeds the validation program.** This lane also serves the experimental
validation / falsification milestone ([EXPERIMENTAL_VALIDATION.md](../EXPERIMENTAL_VALIDATION.md)),
which decides GO / REFINE / NARROW / STOP for SNC after S1. Each system is
an adversarial reviewer, not a feature list. Its review feeds the design
of the comparative workloads: which semantics can be matched with SNC,
and how the system is configured fairly. SpacetimeDB, Redis and
PostgreSQL bear on workload A; SpacetimeDB and Orleans on B; Restate and
DBOS/PostgreSQL on C. DBOS is the null hypothesis.

## Status

| Deliverable | Lane | When it is needed | State |
| --- | --- | --- | --- |
| STYX_REFERENCE_REVIEW.md | A | before the S2/S3 designs (see 1.1) | not started |
| RESTATE_REFERENCE_REVIEW.md | B | before any scheduler, timer or activation design | not started |
| ORLEANS_REFERENCE_REVIEW.md | C | before locality or timer design | not started |
| SPACETIMEDB_REFERENCE_REVIEW.md | D | ongoing; first pass before a matched benchmark | not started |
| DBOS_REFERENCE_REVIEW.md | E | before Agent Runtime or workflow work | not started |
| REFERENCE_ARCHITECTURE_MATRIX.md | all | grows with each review | not started |
| ADOPTION_CANDIDATES.md | all | only once a review proposes something | not started |

Nothing here changes the current order: the legacy-role review, then
S1, then its review (AGENTS.md).

## 1. Review of the program (Claude, 2026-11-04)

The program is sound and I'd keep it as written. These notes adjust how
it lands in this repository.

### 1.1 Styx and S1/S2/S3

The program asks for Styx "before SNC goes far into S1/S2/S3". The three
stages are not equally exposed to it ([MIGRATION_STAGING.md](../MIGRATION_STAGING.md)):

- **S1** (bounded final stage at the destination) is node-local: no wire
  or format change, only how one shard breaks a long task into bounded
  units. Published online-migration designs have little to say about it.
- **S2** (converging copy-ahead, driver only) and **S3** (streamed transfer
  with versioned MOVE verbs) are exactly where Styx's questions apply:
  background versus on-demand movement, routing while state is in
  flight, and keeping transactional guarantees across the cut-over.

Recommendation: S1 goes ahead as already ordered, once reviewed. Lane A
(Styx) is finished before the S2 or S3 design is written. The Styx
review should also check the fail-closed model
([MIGRATION_FAIL_CLOSED.md](../MIGRATION_FAIL_CLOSED.md): arrival records,
serving proof, start barrier, commit gate) against how Styx decides who
may serve state after a failure during migration.

### 1.2 Vocabulary

SNC's stages are RECEIVED, ACCEPTED, APPLIED, LOCALLY COMMITTED,
LOCAL_DURABLE, REPLICATED, HA_DURABLE, ACKNOWLEDGED and SETTLED
([CORE_CONTRACTS.md](../CORE_CONTRACTS.md) section 3), not only the three
the program names. Its "exactly-once is not one property" maps onto them
as follows:

| Property | SNC today |
| --- | --- |
| Exactly-once state effects | sessions deduplicate a resent op at the owner (section 7, [RETRIES.md](../RETRIES.md)); one known gap: R11, a resend after session expiry re-runs ([EVIDENCE.md](../EVIDENCE.md)) |
| Output delivery | ACKNOWLEDGED, with IN-DOUBT outcomes the client resends (section 6) |
| Cross-object effects | SETTLED: durable follow-ups in outboxes, atomic with the transition (section 13; no timer yet, O-2) |
| External side effects | none: R02 is MISSING |

Reviews should use these names, so each finding lands on a contract
that already exists.

### 1.3 Where SNC has no answer yet

Several template questions have no SNC CURRENT answer: dormancy,
scheduling, timers (R09), external effects (R02), security (R05), and
code upgrades beyond refusing a changed type catalog (R01). For those,
the reviews compare against SNC INTENDED, and that is where this lane is
worth the most: prior art shapes the design before one exists. The map in
section 2 marks each gap.

### 1.4 The null hypothesis needs SNC's workloads

[CONVENTIONAL.md](../CONVENTIONAL.md) already compares SNC with
PostgreSQL, Redis and ScyllaDB on the same cores. DBOS (and SpacetimeDB,
lane D) should be added to that harness rather than given a new one, with
the same fairness rules ([BENCHMARKS.md](../BENCHMARKS.md)). The workloads
must include durable cross-object obligations (transfers), not only
single-key ops. Otherwise the comparison tests a key-value store, which
is not the claim being made.

### 1.5 Evidence and licensing rules for each review

- Pin a release tag or commit for every claim about code, and the paper
  version for every claim from a paper. Mark each statement as shipped,
  documented-only, or roadmap.
- Check each project's license at the pinned commit before reading its
  code closely, and record it in the review. Some of these projects may
  use source-available rather than open-source licenses; I have not
  verified any of them. Reviews describe mechanisms in our own words and
  copy no code.
- Verify the citations in the program itself (for example the 2026 VLDB
  Journal Styx paper) before relying on them.

### 1.6 Systems worth adding (cody's call; not in scope now)

- **FoundationDB's deterministic simulation testing**, for R07
  (reproducible verification). SNC's failure testing uses real processes
  and timing ([EVIDENCE.md](../EVIDENCE.md)), and simulation is the main
  alternative with a long production record.
- **Temporal**, for durable execution and workflow versioning. It overlaps
  DBOS and Restate, and is the most widely deployed of the three.
- **Akka Cluster Sharding**, for single-writer entities and rebalancing.
  It is close to Orleans, but its single-writer guarantee is nearer to
  SNC's fencing model than Orleans' duplicate-activation trade-off.

## 2. SNC CURRENT, by template question

The starting column for every review. Each answer links to where the
contract or evidence lives.

| Question | SNC CURRENT | Where |
| --- | --- | --- |
| State authority | the owner shard's in-memory object, made durable by its WAL; one owning group per slot (placement), one primary per group (term) | CORE_CONTRACTS 2, [CLUSTER.md](../CLUSTER.md) |
| Identity | 128-bit ObjectID, never reused; NodeID never reused; space UUID per group | CORE_CONTRACTS 1, [CONTROL_PLANE.md](../CONTROL_PLANE.md) |
| Execution ownership | only the owner shard of the primary serving the slot, with proof of the slot's arrival (placement v2) | CORE_CONTRACTS 2 (MS-1 to MS-4), MIGRATION_FAIL_CLOSED 20 |
| Atomicity | one object per op, or a same-shard `local()` group, as one WAL group; across shards or groups, durable follow-ups | CORE_CONTRACTS 3, 13; [TRANSFERS.md](../TRANSFERS.md) |
| Acknowledgement | by mode: APPLIED, LOCAL_DURABLE or HA_DURABLE; never SETTLED | CORE_CONTRACTS 4, 6 |
| Retry and deduplication | client sessions with sequence numbers at the owner; same-sequence resend after failover | CORE_CONTRACTS 7, RETRIES.md; gap R11 |
| Failure ambiguity | IN-DOUBT outcomes (`E_FENCED`, timeouts) resent with the same sequence; async promotion can lose acked writes (R08) | CORE_CONTRACTS 6, 9 |
| Scheduling | run-to-completion handlers on pinned shards; follow-ups re-emitted on events only | CORE_CONTRACTS 11, 13; R03 partial |
| Dormancy | none: every object is resident in memory | gap |
| Migration | slot moves: seal, staged copy, commit by placement CAS, fail-closed proof; staging bounded later (S1-S3) | [MIGRATION.md](../MIGRATION.md), MIGRATION_FAIL_CLOSED, MIGRATION_STAGING |
| Backpressure | `E_OVERLOADED` above a follow-up backlog per shard; no fairness or admission policy | CORE_CONTRACTS 11; R10 partial |
| Locality | owner-local execution, `create_near`, owner-aware routing; static slot placement, no communication-aware rebalancing | [ROUTING.md](../ROUTING.md), TRANSFERS.md |
| Timers | none | gap R09 |
| External effects | none | gap R02 |
| Code upgrades | a changed type catalog refuses to start; no versioned ops, no mixed versions | EVIDENCE R01, R12 |
| Security | none: no principal, tenant or capability | gap R05 |
| Observability | counters, STATUS/HEALTH, MOVE INFO, a trace design not yet built | [TRACING.md](../TRACING.md) |
