# SNC Server OS — Reference Architecture Research Program

Competitive systems to study, lessons to extract, and integration with the SNC engineering roadmap.

Source: handoff from cody, 4 November 2026 (verbatim text: [../handoff/SNC_Reference_Architecture_Research_Program.txt](../handoff/SNC_Reference_Architecture_Research_Program.txt)). Claude's review and the map to SNC's current contracts: [README.md](README.md).

## Executive decision

Continue SNC Core development. Do not pivot SNC into an MMO engine, AI-only operating system, or clone of another runtime. Run a parallel reference-architecture research lane whose purpose is to challenge SNC’s design with mature prior art and recent research.

The individual ideas behind SNC are not unique. The potential differentiation is a single state-native execution substrate combining durable identity, authoritative state, owner-local execution, locality, recovery, migration, durable obligations, and future runtime services across multiple workload classes. That distinction must be demonstrated by implementation and comparative evidence.

Priority order: Styx → Restate → Orleans → SpacetimeDB → DBOS.

## Why this research matters now

PRs #33–#40 have moved SNC from broad architecture ideas into explicit contracts: acknowledgement stages, HA durability, promotion eligibility, retry/deduplication, migration safety, fail-closed serving proof, health-path separation, recovery barriers, and bounded-work requirements. This is the right time to compare mechanisms rather than product marketing.

The research lane must not interrupt the current Core stabilization roadmap. Its output should feed future design reviews before SNC commits to mechanisms that other systems have already explored.

## 1. Styx — highest immediate priority

Why: Styx is the closest immediate research reference for SNC migration, transactional execution, acknowledgement semantics, exactly-once claims, fault tolerance, and online state movement.

- Study online state migration: asynchronous/background movement versus on-demand state movement, routing during migration, and how transactional guarantees are preserved.
- Compare Styx migration directly with SNC S1/S2/S3: bounded destination staging, converging copy-ahead, and future streamed/cursor migration.
- Study acknowledgement semantics and distinguish exactly-once state effects from exactly-once outputs/external effects.
- Study treatment of nondeterminism and replay, including external calls and idempotency.

Do not copy Styx mechanically. Its execution model differs from SNC’s owner-local object substrate. Extract mechanisms and invariants, then test whether they fit SNC.

- Primary reference: https://github.com/delftdata/styx and the 2026 VLDB Journal Styx publication.

## 2. Restate — highest priority for the future platform/runtime layer

Why: Restate overlaps SNC’s future Durable Events, Timers, dormant entities, activation, reliable invocation, backpressure, and state-native scheduler concepts.

- Study Virtual Queues and the 1.7/1.8 scheduler architecture: fairness, concurrency limits, flow control, admission, blocked-work representation, and observability.
- Determine whether runnable state is authoritative durable state or derived scheduler state.
- Study virtual objects, durable execution, reliable RPC/messaging, and recovery.
- Compare Restate’s dormant execution model with SNC’s proposed wait conditions and activation obligations.

Do not import Restate’s architecture wholesale. SNC intends deeper ownership/locality and native state authority. Restate is primarily a reference for runtime services above the Core.

- Primary references: https://github.com/restatedev/restate and https://restate.dev/blog/announcing-restate-1-7

## 3. Microsoft Orleans — study mature lifecycle and failure scars

Why: Orleans has years of production experience with stable logical identity, activations that come and go, placement, clustering, persistence, timers/reminders, and migration/locality.

- Study grain directory semantics, especially weak versus strongly consistent directory choices and duplicate-activation tradeoffs.
- Study activation repartitioning: communication graph tracking, probabilistic heavy-edge tracking, balancing, and migration decisions.
- Study timers versus durable reminders. Use this to force SNC to define whether a durable timer represents a deadline obligation, periodic notification, or another semantic.
- Study activation placement and lifecycle behavior during failure and recovery.
- SNC should compare Orleans’ willingness to tolerate some duplicate activations with SNC’s stricter fencing/authority model rather than assuming one is universally better.
- Primary reference: https://github.com/dotnet/orleans and Microsoft Learn Orleans documentation.

## 4. SpacetimeDB — strongest state-plus-computation comparison

Why: SpacetimeDB places application computation directly next to authoritative database state. Reducers execute as atomic transactions and are the primary mutation mechanism.

- Compare reducer transaction semantics with SNC’s owner-local atomic transitions plus durable cross-owner obligations.
- Study subscriptions/change propagation as a possible future reference for state-native observation.
- Study partitioning/scaling and how developer ergonomics change when the database is also the application execution substrate.
- Use matched workloads to test whether SNC’s owner-local model earns measurable locality/coordination advantages over a stronger database-transaction abstraction.
- Primary reference: https://github.com/clockworklabs/SpacetimeDB and https://spacetimedb.com/docs/

## 5. DBOS — the control experiment / null hypothesis

Why: DBOS asks the most important strategic question: can PostgreSQL plus a durable-execution library provide most of SNC’s value with dramatically less new infrastructure?

- Study workflow checkpoint/recovery semantics and fencing of recovered execution.
- Study queues, concurrency/flow control, and distributed recovery.
- Study workflow code versioning/patching. Durable computation must define what happens when persisted work resumes under changed code.
- Use DBOS as the null hypothesis for future agent/workflow workloads. SNC should have to prove why a new substrate is worth the complexity.
- Primary reference: https://github.com/dbos-inc and https://docs.dbos.dev/architecture

## Cross-system teardown template

Every reference-system review should answer the same questions so the output becomes engineering input rather than product research.

- State authority — where is truth?
- Identity — what survives process/host death?
- Execution ownership — who may mutate state?
- Atomicity — what commits together?
- Acknowledgement — what does success guarantee?
- Retry/deduplication — how is repeated work recognized?
- Failure ambiguity — what happens after uncertain execution?
- Scheduling — how does durable runnable work become execution?
- Dormancy — can state exist without a resident process?
- Migration — how does ownership/state move?
- Backpressure — what happens at resource limits?
- Locality — is placement communication/data aware?
- Timers — what survives downtime and what is replayed?
- External effects — how are non-idempotent calls handled?
- Code upgrades — how does durable work survive code-version changes?
- Security — what principal/capability/tenant model exists?
- Observability — can causal history be reconstructed?

## Required comparison matrix

For each mechanism record: SNC CURRENT; SNC INTENDED; REFERENCE SYSTEM; BETTER IDEA; ADOPT?; WHY/WHY NOT?; TEST REQUIRED; SOURCE VERSION/COMMIT.

Do not import mechanisms based on elegance alone. A proposed adoption must identify the SNC contract it changes, the failure model it affects, compatibility consequences, and a deterministic validation plan.

## Immediate lessons to investigate

### Exactly-once is not one property

Separate exactly-once state effects, output delivery, and external side effects. SNC’s ACKNOWLEDGED / HA_DURABLE / SETTLED vocabulary should remain explicit.

### Durable scheduler and backpressure semantics

Before generic Activation/WaitSets are designed, inspect Restate’s Virtual Queues and DBOS queues so SNC does not invent weak fairness/admission semantics.

### Communication-aware locality

Before dynamic locality is designed, inspect Orleans activation repartitioning and its communication-edge tracking. Compare with SNC’s future NUMA/network-aware placement goals.

### Durable code versioning

Add code-version compatibility to the future SNC platform research backlog. Long-lived durable activation creates a hard question: what code semantics resume old state after deployment? DBOS explicitly addresses this with patching/versioning and compatible recovery.

### Migration before S1/S2/S3 hardens

Styx should be reviewed before SNC goes far into S1/S2/S3. The goal is not to stop current safety work, but to ensure SNC’s migration protocol is informed by published online-migration mechanisms.

## Research execution plan

- Lane A — Styx now: paper + source teardown focused on migration, acknowledgements, deterministic execution, replay, and external effects.
- Lane B — Restate before SNC Platform scheduler work: Virtual Queues, flow control, dormant execution, durable invocation.
- Lane C — Orleans before locality/timers: grain directory, activation placement/repartitioning, reminders.
- Lane D — SpacetimeDB as ongoing comparative runtime: reducer transactions, subscriptions, state/computation integration.
- Lane E — DBOS before Agent Runtime/workflows: recovery, queues, fencing, workflow versioning, and Postgres-backed null hypothesis.

## Engineering rules

- Do not stop the current SNC Core stabilization work.
- Do not copy architecture without mapping its assumptions to SNC’s contracts and failure model.
- Use primary source, paper, and code evidence; record exact version/commit.
- Separate shipping behavior from roadmaps and marketing claims.
- Preserve licenses and dependency due diligence; learning from a design is not authorization to copy source.
- Every proposed adoption requires a bounded design slice and review before implementation.
- Use competing systems adversarially: if SNC’s answer is not materially better for a target workload, improve SNC or simplify it.
- Keep workload-specific features outside SNC Core.

## Recommended deliverables from the research lane

- STYX_REFERENCE_REVIEW.md
- RESTATE_REFERENCE_REVIEW.md
- ORLEANS_REFERENCE_REVIEW.md
- SPACETIMEDB_REFERENCE_REVIEW.md
- DBOS_REFERENCE_REVIEW.md
- REFERENCE_ARCHITECTURE_MATRIX.md — cross-system comparison
- ADOPTION_CANDIDATES.md — only mechanisms worth proposing to SNC, each with contract impact and tests

## Conclusion

The research should make SNC harder to fool, not more complicated. The best outcome is not a long list of borrowed features; it is a smaller number of mechanisms whose semantics are stronger because they were compared against mature systems and published research.

The governing question for every comparison is: How does the reference system solve this, what assumptions does it rely on, and can SNC produce a demonstrably better or simpler answer for the workloads it targets?

## Primary references

- Styx — https://github.com/delftdata/styx
- Restate — https://github.com/restatedev/restate
- Restate Virtual Queues — https://restate.dev/blog/announcing-restate-1-7
- Orleans — https://github.com/dotnet/orleans
- Orleans placement — https://learn.microsoft.com/en-us/dotnet/orleans/grains/grain-placement
- SpacetimeDB — https://github.com/clockworklabs/SpacetimeDB
- SpacetimeDB transactions — https://spacetimedb.com/docs/databases/transactions-atomicity/
- DBOS — https://github.com/dbos-inc
- DBOS architecture — https://docs.dbos.dev/architecture
- DBOS workflow upgrades — https://docs.dbos.dev/golang/tutorials/upgrading-workflows
