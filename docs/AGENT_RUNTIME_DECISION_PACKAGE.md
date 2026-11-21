# SNC Enterprise Agent Runtime: Strategic Architecture Decision Package (PROPOSED)

Status: **PROPOSED, for Codex and the owner.** Answers section 11 of Codex's review of PRs #181/#182
(`docs/handoff/PR181_PR182_Codex_Review.md`). Docs only. It authorizes no implementation, no proof of concept, no
C++ or Rust, and no dependency. It revises no SNC guarantee (HA_DURABLE included) and freezes nothing. The product
direction is a proposed decision that needs cody's explicit yes (`docs/adr/SNC-ADR-PRODUCT-001.md`). AFR-001, L-1,
LiveGrant, D-02, R-23, O-FLOOR and every model result are preserved unchanged. Revised once after a three-way
independent review (section 11).

Labels (`docs/ENGINEERING_PRINCIPLES.md`):
- **FACT**: read in this repository.
- **EXT**: an external source, with URL. Most external quotes came through a fetch tool that summarises pages, so
  they are not byte-verified. **EXT-SRC** marks text read in package source: the DBOS Transact Python 3.6.0 wheel
  and the `dbos` 0.6.0 crate.
- **INF**: inferred here.
- **HYP**: untested.

## 0. Recommendation in one page

**Make the first product the agent system of record and its enforcement boundary. Build it on PostgreSQL, and reuse
an established durable-execution engine (DBOS Transact) instead of writing one.** This is option D (staged), with A2
(PostgreSQL plus DBOS) as stage 1.

What SNC owns:
- the agent identity, memory and delegation model;
- the evidence contract (section 2), as its own schema in the same PostgreSQL database as agent state;
- policy and approval gates;
- the tool gateway that holds credentials, so consequential calls cannot bypass the gate;
- the audit and reconstruction API.

What SNC reuses:
- **PostgreSQL**: ACID, WAL, crash recovery and synchronous replication, under an HA manager in strict synchronous
  mode;
- **DBOS Transact**: durable steps, queues, timers and recovery;
- a blob store for large evidence content, referenced by keyed hash.

Storage stays behind one SNC interface. The SNC core or a Raft substrate (option C) could replace it later, but only
for a workload that has measured the need.

**Said plainly: this is a product pivot** [INF]. SNC stops being, first, a high-throughput distributed object
database. The per-core runtime, the custom replication and the R640 programme become research that comes back only
if a measured agent workload needs it (section 5). It also overrides the validation scope freeze, which needs its
own owner decision (OD-P2).

**What the differentiator is, tested honestly (section 3.4).** Atomic commit of evidence with state is *not* a moat.
Any durable engine can get it:
- a DBOS step that writes state, evidence and an idempotency row in one application-database transaction;
- or a Temporal Activity doing the same.

What a library cannot give is **enforcement and completeness for every agent on the system**:
- the gateway holds the tool credentials;
- manifests are complete by construction;
- policy decisions are records, not prompt text;
- integrity, minimization and erasure apply per tenant.

So the product is that layer, and the engine underneath is a replaceable part. Whether customers will buy this layer
rather than assemble it from a policy engine, a credential vault and an SDK is the market risk. It is tested by the
product gate (7.2), not assumed.

**Why DBOS over Temporal or building our own** (section 3):
- DBOS runs as a library over the same PostgreSQL as our state and evidence. Temporal adds a separate four-service
  cluster plus its own persistence and visibility stores [EXT], and puts waits and timers in a second record.
- Writing our own journal would ignore Codex's "prefer reusing ... durable workflow execution unless benchmarks
  justify otherwise".

The caveats are maturity, not atomicity:
- DBOS's Rust SDK is "pre-production" [EXT-SRC].
- Its Python and TypeScript SDKs have transactional steps that commit "atomically in the same database transaction"
  [EXT].
- So the proof of concept uses DBOS Python. The product language is decided after it (OD-P8).

**Correcting #181's premise (section 3.3).** "Every mutation leaves the owning core" does not decide anything here.
An agent step is dominated by model and tool time. **The real costs are evidence bytes and PostgreSQL housekeeping**
(WAL volume, vacuum, transaction-ID freeze), not commit latency [HYP, to be measured].

The exact owner decisions are in section 8.

## 1. Product mission and differentiation

**Mission (proposed).** SNC is the system of record for autonomous agents. For any consequential decision or
action, an organization can establish seven things:
1. what the agent could have seen (the candidate set);
2. what it was actually shown;
3. which policies, capabilities and approvals governed it;
4. on whose behalf it acted;
5. what it did, including retries and failures;
6. how authoritative state changed;
7. who has looked at that evidence since.

SNC provides this as a property of the runtime, not as a logging add-on.

**Where this sits** [INF; the positioning is HYP until the product gate]:

| Category | Gives | Leaves to the user |
|---|---|---|
| Durable execution (Temporal, DBOS, Restate) | Resumable code, retries, timers | The evidence model, policy records, enforcement, evidence integrity and retention |
| Agent frameworks with checkpoints (LangGraph) | Resumable graph state. But replay "re-executes nodes" and calls "fire again and may return different results" [EXT] | Exactly-once effects, governance, evidence |
| Policy engines (OPA, Cedar) and credential gateways (vaults, API and MCP gateways) | Policy evaluation; credential brokering; per-call authorization [INF, not researched here] | Durable state, transactional evidence, reconstruction |
| Observability (OpenTelemetry GenAI conventions) | Traces | Authority: a trace is not a transactional record of state |
| **SNC** | The above joined in one transaction boundary: state, manifest, policy decision, approval, effect record and transition, with integrity and reconstruction | Model internals (E-6) |

The vision frames this as a **fallback**: "If SNC is not compelling as a general enterprise substrate, evaluate a
specialized persistent-agent platform before STOP", with value in "continuity, auditability, policy enforcement,
recovery, context reconstruction, token efficiency, dormant-agent efficiency and multi-agent coordination" [FACT:
`docs/handoff/SNC_Future_Architecture_Vision_v2.txt` lines 22-28]. This package proposes making it the primary path
by owner decision, not by the vision's gate. "The persistent state is the agent" (line 32) is the
model-independence principle used here. Token efficiency, dormant-agent efficiency and multi-agent coordination are
later-stage value, not stage-1 claims.

## 2. Agent-state and decision-provenance contract

### 2.1 Record types

| Record | Holds | Class |
|---|---|---|
| **Principal** | Humans, services and agents; tenant, project, role | Authoritative |
| **Agent** | Persistent id; role; capability set (versioned); runtime configuration; memory root; version | Authoritative, versioned |
| **Memory object** | A typed fact, document or artifact; provenance (the producing activation); visibility (private or project) | Authoritative, versioned, history kept |
| **Task** | Requester; **on-behalf-of chain** (principal, then delegating agent, then this agent); goal; inputs; deadline; parent; status | Authoritative |
| **Activation** | One model invocation. Holds: provider; exact model id and version as returned; parameters; code version; skill set; tool-set version; project and memory snapshot ids; causal event; status (started, completed, output-lost, failed) | Immutable per state |
| **Retrieval record** | The query, and the candidate set with ids, versions and scores, including what was *not* delivered | Immutable |
| **Evidence manifest** | The ordered items delivered to the model. Each item has kind, id, version, keyed hash, trust level (trusted or untrusted) and capture mode (raw, hash-only, redacted, quarantined or erased); plus a whole-input hash | Immutable |
| **Policy decision** | Policy id and version; evaluated inputs by keyed hash; result; obligations (for example, approval required) | Immutable |
| **Approval** | Approver principal and source (direct, or relayed by whom); the keyed hash of the approved action; decision; time | Immutable |
| **Tool call** | Call id; idempotency key; tool and version; capability snapshot; activation epoch; arguments (keyed hash, capture mode); intent; then each attempt (retry number, error) and the result, or an in-doubt marker | Immutable per state |
| **Run event** | Cancellation, failure, timeout, recovery start, epoch change | Immutable |
| **State transition** | Object; prior version; new version; causing record | Immutable |
| **Wait / timer** | A condition or due time; owning task; fired-by | Authoritative until fired |
| **Evidence access** | Reader, item, purpose, time. Written to a separate append-only sink and not itself subject to E-8 | Immutable |

**Classes:**
- **Authoritative**: the current truth, with one writer.
- **Immutable**: append-only. A correction is a new record that references the old one.
- **Reconstructable**: derived, and can be rebuilt. Current memory views, indexes, summaries, and the state "as of"
  a time (from versions and transitions).
- **Not captured**: the model's internal reasoning. Any reasoning text a provider *returns* is output and is
  captured as output.

### 2.2 Invariants (proposed)

- **E-1 Decide before acting.**
  - An external effect runs only through SNC's tool gateway.
  - It runs only after an intent record has committed. That record references the activation, the policy decision
    and any approval.
  - The intent transaction re-checks, under SERIALIZABLE isolation: the task status (not cancelled), the activation
    epoch (E-10), and the versions of the policy inputs.
  - The gateway acts only on an intent whose commit is **replicated** (its commit LSN is at or below the synchronous
    standby's `flush_lsn`). Locally visible commits can still be lost (OD-P6).
  - Enforcement assumes the tool is unreachable by any other network path. That is a deployment requirement.
- **E-2 One commit per consequential boundary.** The following commit in one transaction, or none of them does:
  - the step outcome;
  - its state transitions;
  - its evidence records;
  - its outbox entries;
  - its timer changes.

  Which operations need such a boundary is listed in 2.3.
- **E-3 Manifest completeness.** Every byte delivered to the model is listed by keyed hash. For each item, either
  the bytes are retrievable for the retention period, or its capture mode says why not (redacted, quarantined,
  erased).
- **E-4 Tamper evidence.**
  - Each agent's immutable records form a hash chain. Per agent, not per tenant, so it adds no tenant-wide serial
    point.
  - A separate batcher folds the chain heads into a per-tenant Merkle tree (RFC 9162 style [EXT]).
  - It signs tree heads with a key held in a KMS or HSM, outside database roles, and anchors them externally (WORM
    storage or a transparency log).
  - Verification is by a party the database operator does not control.
  - This is tamper-*evident* up to the last anchor, not tamper-proof.
- **E-5 Effects at least once, visible once.** One idempotency key per tool call, the same across retries and
  activation epochs. An intent with no result is either retried with the key (if the tool honours keys) or marked
  **in doubt** for policy or a human. It is never silently repeated. Outbox consumers deduplicate.
- **E-6 Observable boundary only.** The record claims what crossed the runtime boundary, nothing about what the
  model internally "considered".
- **E-7 Reconstruction.** For any tool call or state transition, the system returns the full chain: transition ←
  tool call ← approval ← policy decision ← activation ← manifest ← retrieval record ← items. An erased item is
  returned as erased, not as an error.
- **E-8 Reading evidence is controlled and recorded.**
  - A reader policy decides who may read which evidence class.
  - Each content read writes an access record to a separate append-only sink, anchored like E-4. Bulk reads are
    aggregated.
  - Auditors have their own role, separate from operators.
- **E-9 Minimization and erasure.**
  - A per-tenant capture policy, applied before anything is written, chooses the capture mode per data class and
    trust level.
  - Hashes are **keyed** (HMAC under a per-tenant key, or a per-subject key for personal data), so low-entropy
    values cannot be brute-forced and erasure can destroy the key.
  - Erasure destroys the subject key. Content becomes unreadable and keyed hashes become unlinkable. The chain stays
    verifiable over the hashes as stored.
  - The legal basis for any retention after erasure is documented per record class. Legal holds take precedence
    over erasure.
- **E-10 One writer per agent, fenced at the effect.**
  - An activation holds a lease with an **epoch** on the agent row.
  - Each commit checks the epoch. No database transaction stays open across a model or tool call.
  - The gateway re-checks the epoch immediately before each external call, and passes a fencing token to tools that
    accept one.
  - A stale activation can fail to commit, but it cannot make a call after a newer epoch exists. Between its check
    and the call there is a window, and the idempotency key covers it.
- **E-11 Model calls recorded before and after.**
  - "Activation started" commits before the call. It holds the manifest and the provider request id, so data sent
    to a provider is always on record.
  - The output commits after the call. A recorded output is never regenerated.
  - A crash in between records "output lost" before any re-call.

### 2.3 What needs a database transaction, and what runs locally (Codex section 6, option B)

| Needs a consequential commit (E-2) | Runs locally, with no durable boundary |
|---|---|
| Task acceptance; activation started; activation output; policy decision; approval; tool intent; tool result with its state transitions; timer arm or fire; cancellation; memory writes that later decisions may cite | Context assembly and ranking (its *result* is recorded at "activation started"); token streaming; caches; draft reasoning scratch; read-only memory and retrieval queries (their candidate set is recorded with the activation); UI state |

Rule [INF]: anything that can authorize, cause or explain a consequential action commits. Anything that can be
recomputed from committed records does not.

### 2.4 End-to-end example: this project merging a PR

Suggested by the coordinator as a first use case [INF]: this project's own workflow has an owner (cody), an external
reviewer (Codex), AI workers (Claude threads), standing rules (today in memory files), and consequential actions
(merges, contract changes).

Case: PR #181 merged on 2026-11-21 [FACT: merge `c996ba7`, called with `expectedHeadSha 1009b83`]. Each **C#** is
one transaction.

| Step | What happens | Commit | Records |
|---|---|---|---|
| 1. Task acceptance | The coordinator assigns "merge #181 as evidence, then write the package" to agent `thread-f0vhs6`, acting on behalf of cody → coordinator | C1 | Task T1 (on-behalf-of chain, key `relay-0529`); agent v12 → v13 |
| 2. Evidence capture and model start | Context: the Codex review text (relayed by cody); rule set `merge-rules v7` ("Claude may merge when tests pass", "contract changes need cody's explicit yes"; both held in project memory today, not the repo); PR #181 at `1009b83`; the check-run result. Blobs are written first, under a pending-blob lease | C2 | Retrieval record R1; manifest X1; activation V1 *started* (epoch 4) |
| 3. Model output | "Merge #181 as evidence; do not accept HYBRID" | C3 | V1 completed (output as returned) |
| 4. Policy check | `merge-rules v7` on {docs only, Codex approval relayed by cody, CI green, no contract change}: allowed | C4 | Policy decision P1. A frozen-contract change would instead have created a wait for cody's explicit yes |
| 5. Approval | The approval is Codex's verdict "APPROVED FOR DOCUMENTATION MERGE", relayed by cody | C4 | Approval AP1 (approver Codex, relayed by principal cody, action hash) |
| 6. Tool action | Intent, then the gateway checks epoch 4 and replication, then calls GitHub merge with the head sha | C5, call, C6 | Tool call TC1 intent (key `T1-merge-181`); attempt 1; result `c996ba7` |
| 7. State transition | In the same transaction as the result | C6 | PR #181 open → merged; memory M41 "#181 merged as evidence, HYBRID on hold" (provenance V1, TC1); outbox: notify the coordinator |
| 8. Audit retrieval | Later: "on whose authority was #181 merged, and did anyone accept raft-rs?" | read | merge → TC1 → AP1 (Codex, via cody) → P1 (rules v7) → V1 → X1 (the text shown) → R1. M41 answers the second question. An access record is written |

**What the real run lacked** [FACT]: in the actual merge the agent held GitHub credentials directly. Its rules were
memory text, and the record of the decision is this paragraph. That is the gap the product closes.

Crash cases [INF]:
- A kill between the call and C6 leaves TC1 with an intent and no result. Recovery re-reads the PR: the merge is
  keyed by head sha and idempotent.
- A kill between C2 and C3 marks V1 output-lost before any re-call (E-11).
- A second activation with epoch 5 fences epoch 4 at the gateway (E-10).
- A primary failover after C5 but before replication cannot cause the call, because the gateway waits for
  replication (E-1).

### 2.5 Cost and sensitivity of capturing exact inputs

- **Content addressing.**
  - The manifest stores references; bytes are stored once per tenant, in segments (instructions, each policy
    version, each memory version, each document version, each tool result).
  - Most of a step's context repeats from the previous one, so it costs a reference, not a copy [HYP; measured in
    AR-1].
  - Deduplication lookups happen server-side, after an authorization check on the underlying object. No client sees
    whether a blob already existed, so there is no existence oracle.
  - Deduplication never crosses tenants or subject keys.
- **Garbage collection.** Mark-epoch GC with a grace period longer than the longest step. Pending blobs are leased
  from write until commit. The blob store has its own durability class (two copies, the same as OD-P6).
- **Exactness.** The model-visible input is the ordered segments plus the runtime's framing. The framing is
  versioned code, so the exact bytes can be rebuilt and checked against the whole-input hash.
- **Untrusted content.**
  - Retrieved documents and tool results are tagged untrusted.
  - Untrusted content cannot supply arguments to a consequential call unless policy gates that call.
  - Flagged content (suspected injection, illegal content) goes to the quarantined capture mode.
  - Third-party personal data in retrieved documents defaults to hash-only, with the source system keeping the
    version, because no per-subject key exists at capture time.
- **Retention tiers.** Raw content: short, set by policy. Keyed hashes, manifests, decisions, approvals and
  transitions: the tenant's retention period, with legal holds.

## 3. Build versus reuse at the product level

### 3.1 Options against the contract and Codex's criteria

| Criterion | A1: PostgreSQL + Temporal | A2: PostgreSQL + DBOS Transact (stage 1 of D, recommended) | B: SNC-owned journal on PostgreSQL | C: SNC substrate on raft-rs/OpenRaft |
|---|---|---|---|---|
| Atomic state + evidence (E-2) | Yes, if each consequential step is an Activity that writes state, evidence and an idempotency row in one app-database transaction. Waits and timers live in Temporal and must be mirrored [INF] | Yes. Python/TS transactional steps "atomically in the same database transaction" [EXT]. Rust 0.6 has none public, but a step can do the same app transaction with an idempotency row keyed by `(workflow_id, step_id)`, which its step context exposes [EXT-SRC, per review] | Yes, by construction | Yes, if built |
| Effects (E-5) | Activities "may be executed multiple times"; idempotency is recommended [EXT] | Steps "tried at least once but ... never re-executed after they complete" [EXT] | Same rule, ours | Ours |
| Long-lived agents | 51,200 events / 50 MB per run, 2 MB payload [EXT]; continue-as-new needed; evidence must leave history anyway | No stated cap found (UNVERIFIED) | No cap | No cap |
| Audit capability | History is a replay log, not an evidence model; evidence lives in the app database | `operation_outputs` is a recovery cache [EXT-SRC]; evidence lives in our schema, in the same database | Ours | Ours |
| Multi-tenancy | Namespaces; "A single Namespace is still multi-tenant" [EXT] | Ours (section 6) | Ours | Ours |
| Operational burden | Four services plus persistence plus visibility store [EXT] | A library plus PostgreSQL; cross-executor recovery via Conductor (proprietary) or our own recovery scan | PostgreSQL only | Highest (#181) |
| Maturity | Server mature; Rust SDK 0.6 "Public Preview" [EXT] | Python/TS in production use (HYP for scale); Rust 0.6 "pre-production" [EXT-SRC] | New code | New code |
| Engineering cost [INF, unestimated in hours] | Adapter + service operations | Adapter only | Journal, recovery, queues, timers: DBOS's feature set again | Largest |
| Throughput | Not the constraint at stage-1 scale [HYP] | Bounded by PostgreSQL (3.3) [HYP] | Same | Highest, unneeded so far |
| Latency per boundary | One app transaction plus a Temporal round trip | One transaction | One transaction | Lowest |
| Time to a usable product | Medium | **Shortest** | Medium | Longest |
| Licence | MIT | MIT core; Enterprise proprietary [EXT-SRC] | PostgreSQL License | Apache-2.0 / MIT |

**D** is A2 now, with the storage and engine behind SNC interfaces, so B (if DBOS fails a gate) or C (if a measured
workload needs it) can replace them.

### 3.2 Prior art and what it settles

- **Stonebraker's DBOS OS work** [EXT, PVLDB 15(1) 2021]:
  - Claim: "a distributed transactional DBMS should be the basis for a scalable cluster OS".
  - Built scheduler, files and IPC as tables (FIFO scheduling about 750K tasks/s).
  - Measured a real cost: about 40 µs per DBMS invocation against about 1 µs per syscall, which made reads slower.
  - The 1981 CACM critique of OS support for databases is UNVERIFIED here.
  - *Settles:* putting scheduling state in a DBMS works and costs per-call latency. That latency is irrelevant
    against model latency, and it is why SNC's agent scheduling can live in PostgreSQL.
- **Apiary** [EXT, arXiv 2208.13068]:
  - Exactly-once functions as DBMS transactions.
  - Provenance "records for each data item all function executions that accessed or modified it", at up to 15%
    throughput and 10% latency cost.
  - *Settles:* transactional provenance capture is affordable, and gives a budget to measure against.
- **DBOS Transact** [EXT, EXT-SRC]:
  - Checkpoints steps in PostgreSQL; transactional steps commit with application writes.
  - Recovery re-enqueues pending workflows.
  - Workflow code must be deterministic, and "Most divergence reports nothing" in Rust.
  - *Settles:* the engine choice for stage 1. The silent-divergence risk needs a determinism lint or test in AR-1.
- **Temporal** [EXT]: event history with replay and determinism. *Settles:* a good engine, the wrong record.
- **PostgreSQL 18 and Patroni** [EXT, #181 4.4]: synchronous commit, quorum `ANY n`, data checksums by default, and
  strict synchronous mode. *Settles:* the durability class OD-P6, and its caveats.
- **Raft, raft-rs, OpenRaft** (#181):
  - Patroni already relies on a Raft- or Paxos-based DCS (etcd, Consul or ZooKeeper) for leader election [EXT].
  - *Settles:* consensus is reused through the DCS, and SNC writes none for stage 1.
- **FoundationDB** (#181 4.5):
  - Its lesson is deterministic simulation: "It seems unlikely that we would have been able to build FoundationDB
    without" it [EXT].
  - *Settles:* AR-1's fault injection should be deterministic and seedable.
- **Kafka KIP-497/903/966** (#181 3.2, 3.4): check eligibility at the decider at the moment of the change.
  *Settles:* E-1's re-check of task status, epoch and policy inputs *inside* the intent transaction, not before it.

### 3.3 Re-examining "every mutation leaves the owning core" (Codex section 8)

[INF; every number HYP until AR-1]
- **#181's comparison was per operation**: SNC sync-replicated at 640K ops/s against PostgreSQL's 29K [FACT:
  `docs/REPLICATION.md`]. That measures object operations, not agent steps.
- **An agent step** is one model call (0.3-30 s), some tools, and about 4-7 consequential commits (2.3).
- **Latency**: with commits of about 1-5 ms on a LAN with a synchronous standby, persistence is on the order of 1% of
  step latency. That is a latency statement, not a capacity one.
- **Capacity**: 10,000 concurrent agents stepping every 10 s give about 1,000 steps/s and 5,000-7,000 commits/s.
  Within one well-provisioned PostgreSQL only if the following hold:
  - evidence tables are partitioned by time;
  - manifests are stored as one row with an array of references, not about 100 item rows;
  - no tenant-wide serial point exists (E-4 is per agent);
  - LISTEN/NOTIFY is not used on the commit path;
  - pooling keeps connections at hundreds, not 10,000.
- **Housekeeping the estimate must include**: about 430-600 M transaction IDs per day against the default
  `autovacuum_freeze_max_age` of 200 M, so freezing on large append-only tables; WAL volume to the synchronous
  standby; bloat on queue, timer and outbox tables.
- **The dominant cost is evidence bytes**:
  - Context runs 20-200 KB per model call, so 1,000 steps/s is 20-200 MB/s raw, or 1.7-17 TB a day before
    deduplication.
  - Content addressing (2.5), the capture policy and retention tiers are therefore first-class design work.
- **Where SNC's core-local runtime could still matter**: very large dormant-agent populations with timers;
  memory retrieval with tight latency; evidence-graph traversal at scale. Each is an AR-1 measurement, not a premise.

### 3.4 Can the differentiator be a thin library over an existing engine?

**Atomicity, yes. Enforcement and system-wide completeness, no** [INF from EXT].
- Over DBOS or Temporal, a step or Activity writes state, evidence and an idempotency row in one app transaction
  (3.1).
- What neither gives:
  - a gateway that holds credentials, so a consequential call cannot be made outside policy;
  - complete manifests for every agent, not only for code that called the library;
  - tenant-level integrity, minimization, erasure and reader control.

If customers only want the library, the product shrinks to an SDK over DBOS or Temporal plus a policy engine and a
vault. That is the market question for the product gate (7.2).

### 3.5 Revisit triggers

- **DBOS fails a correctness gate in AR-1**, or its divergence risk cannot be contained: arm B (our own journal), on
  the same schema.
- **The Rust product language is chosen and DBOS Rust is still pre-production**: either wait, or take B.
- **PostgreSQL capacity is the measured bottleneck** at a target customer's scale: evaluate C (or the SNC core)
  behind the storage interface, starting from #181.
- **A customer runs Temporal already**: offer an adapter. The evidence schema does not change.

## 4. Mapping of existing SNC technology

| SNC asset | Use in the agent runtime | Status |
|---|---|---|
| Stable 128-bit ObjectIDs, location-independent (invariant 2) | Agent, memory and evidence ids | Reuse the concept |
| One owner per object, terms and fencing (invariant 3, G-31, AFR-001) | E-10: leases with epochs, and fencing at the effect. AFR-001's lesson that stale writers act between check and effect applies directly | Reuse the lessons |
| Obligations and outbox in one WAL group (CORE_CONTRACTS 13, R14) | E-2 outbox entries in the step transaction; durable events | Reuse the design |
| Sessions and dedup; invariant 18 | Idempotency keys (E-5); a duplicate is answered before any refusal that invites a resend | Reuse the design |
| Full post-op images | State transitions with prior and new versions; "as of" reconstruction | Reuse the design |
| Checkpoints as snapshot roots; space UUID as branch identity (FUTURE_ARCHITECTURE 3.1, 3.3) | Memory and project snapshot ids in the activation; later, agent sandboxes on branches | Later stage |
| FM-1..16, G-14 fail-closed method | Judging PostgreSQL settings (fsync, checksums default in PG 18 [EXT], strict synchronous mode) and the in-doubt rules | Reuse the method |
| #181 research; the C++ engine, per-core shards, NUMA work | Option C if triggered; deferred research backend | Preserved, not extended |

### 4.1 AGENTS.md invariants and the runtime [INF]

- **Carry over:**
  - 2 (identity independent of location);
  - 3 (one owner, here per agent);
  - 8 (correctness before benchmark wins);
  - 9 (benchmark competent alternatives: AR-1 compares against DBOS, not a strawman);
  - 15 (persistent state authoritative, delivery recoverable);
  - 16 (no externally visible computation before recovery completes: the gateway refuses during an agent's recovery
    scan);
  - 18.
- **Do not apply to the stage-1 runtime:** 4, 5 and 14 (core locality and the hot path), 7 (native runtime by
  default), and 10-13 and 17 (placement, slots, networking OS).
- **Conflict for the owner:** `docs/ENGINEERING_PRINCIPLES.md` says "maintaining SNC's independent architecture".
  PostgreSQL holding authoritative state is a deliberate dependency, so it needs OD-P3.

## 5. Goals that become deferred or optional

**Deferred:**
- the general SNC database;
- custom replication (AFR-001, L-1, LiveGrant, A-1 and the LG work: no expansion);
- D-02, R-23, O-FLOOR and D-06 as SNC-format items;
- the R640 campaigns (CP-1A, Workload A, X-2);
- cross-node exchange and slot migration;
- the Seastar, DPDK and VPP comparisons;
- the networking-OS integration;
- kernel and OS-level work;
- the 1M-object benchmark programme;
- the C++ reference's role under SNC-ADR-RUST-001.

**Optional, for later stages:**
- branches and sandboxes;
- PITR and backup beyond PostgreSQL's own;
- a specialized backend (C);
- token-efficiency, dormant-agent and multi-agent features.

General-SNC falsification under `docs/EXPERIMENTAL_VALIDATION.md` section 14 is **suspended, not concluded**. Its
evidence stays valid if it is resumed.

## 6. Security, governance and evidence retention

- **Tenant isolation.**
  - Database roles:
    - a migration owner that owns the tables;
    - a runtime role, NOSUPERUSER and NOBYPASSRLS, that does not own the tables;
    - an audit-reader role;
    - an anchor-batcher role.
  - Every table has `FORCE ROW LEVEL SECURITY`, because owners and `BYPASSRLS` roles bypass policies [EXT].
  - Tenant context is set with `SET LOCAL` inside each transaction, never per pooled session.
  - No SECURITY DEFINER functions. Views use `security_invoker`.
  - RLS is defence in depth, not the boundary: one runtime bug can still cross tenants.
  - Regulated tenants can get a database per tenant. That still leaves the control plane, KMS, blob credentials, HA
    manager and gateway shared, and the package says so. Whether to split those per tenant is OD-P7.
  - Isolation tests, including pooled-connection leaks and side channels through constraint errors, are an AR-1
    gate.
- **Encryption.** TLS in transit and disk encryption at rest. Evidence content is envelope-encrypted under
  per-tenant or per-subject data keys held in a KMS. Hashes are keyed (E-9).
- **Authorization and identity.**
  - Agents run under a workload identity.
  - Capabilities are scoped (tool, object class, later lineage), can be attenuated on delegation, and are snapshotted
    on each call.
  - The on-behalf-of chain prevents confused-deputy calls: the gateway authorizes against the end principal's
    rights *and* the agent's capabilities.
- **The credential gateway**, which is the blast radius of the product:
  - credentials live in a secret manager, never in the database;
  - short-lived, scoped credentials where the tool supports them (token exchange, workload identity);
  - per-tenant separation; rotation and revocation;
  - an egress policy that forbids other paths to the tools;
  - customer-supplied credentials are onboarded without staff access.

  Tools that only accept long-lived keys are a named risk.
- **Evidence access.** E-8 (reader policy, separate access sink, an auditor role separate from operators). pgAudit
  for database-level auditing ("detailed session and/or object audit logging") [EXT].
- **Integrity.** E-4 (per-agent chains, Merkle batcher, KMS-held signing key, external anchor), verification jobs,
  and PostgreSQL data checksums.
- **Retention and law** [EXT, secondary or summarised sources; verbatim text UNVERIFIED]:
  - AI Act Art. 12 requires automatic logging in high-risk systems.
  - Art. 19 requires *providers* to keep the logs under their control for at least six months, subject to other
    law.
  - Art. 26(6) puts a similar duty on *deployers*.
  - SNC would usually be infrastructure, with the customer as provider or deployer. SNC's job is to make their
    retention settable and provable, not to fix a floor.
  - GDPR storage limitation and erasure (Art. 5(1)(e), 17) apply, with carve-outs (Art. 17(3)(b), (e)) that need a
    documented legal basis per record class.
- **Sensitive data.**
  - The capture policy runs before anything is written.
  - Secrets are never captured raw.
  - Untrusted and third-party data follow 2.5.
  - "Track everything" is not the default (Codex section 4).

## 7. First proof of concept (proposal only; needs authorization)

### 7.1 AR-1: one agent lifecycle on PostgreSQL with DBOS

- **Scope:**
  - PostgreSQL 18: a primary with two standbys, one synchronous (`ANY 1`), under Patroni strict synchronous mode,
    on lab VMs (not the R640);
  - DBOS Transact **Python** (the mature SDK with transactional steps);
  - the evidence schema of section 2;
  - a filesystem blob store;
  - a stub model (deterministic, recorded outputs, configurable latency and context size), plus one real model call
    for manifest fidelity;
  - two fake tools: one that honours idempotency keys, one that does not;
  - the section 2.4 scenario, plus a refund-over-limit case that takes a real approval wait.
- **Baseline arm, for invariant 9:** the same scenario on a thin SNC journal (arm B), only for the gates where DBOS
  is questioned: determinism divergence and boundary latency.
- **Correctness gates:** seeded, deterministic fault injection (FoundationDB lesson) covering:
  - kill -9 of the app at every boundary, and between each external call and its commit;
  - primary restart while commits wait;
  - cancellation of a waiting commit;
  - loss of the synchronous standby;
  - Patroni failover;
  - a stale activation during a tool call;
  - a GC race on a deduplicated blob;
  - an approval racing a cancellation;
  - a double timer fire;
  - tenant-isolation and pooled-connection tests.

  Checks: E-1 to E-11 after each case; reconstruction (E-7); erasure by key destruction leaves the chain verifiable.
  Three-way review of the checker.
- **Measurements:**
  - commit latency p50/p99 per boundary type;
  - steps/s at 100, 1,000 and 10,000 concurrent agents, including a single hot tenant;
  - timer and event activation latency with 1M dormant agents;
  - memory-retrieval and provenance-query latency;
  - recovery time after termination;
  - evidence bytes per step with and without deduplication;
  - storage growth per 10,000 steps;
  - WAL volume, vacuum and transaction-ID age over a long run.

  Thresholds are preregistered before results. A timeout is UNRESOLVED.
- **Outcome:**
  - GO: build stage 1 on this.
  - REFINE: schema, deduplication, or arm B in place of DBOS.
  - STOP: re-open option C or the product question.

### 7.2 Product falsification gate (the vision's separate Agent-SNC gate)

Preregistered before any build, run alongside AR-1:
- At least three design partners name a consequential agent workflow and their audit question.
- At least two confirm, on the section 2.4-style reconstruction, that it answers a question they cannot answer with
  their current stack (framework + vault + policy engine + logs).
- At least one confirms the enforcement boundary (gateway-held credentials) is acceptable in their environment.

Failing any one is NARROW (SDK only) or STOP for the product, recorded as such [HYP thresholds; the owner sets
them].

## 8. Owner decisions required (exact)

1. **OD-P1 Product direction.** Accept SNC-ADR-PRODUCT-001 (the product decision only).
2. **OD-P2 Scope freeze.** Explicitly override `docs/EXPERIMENTAL_VALIDATION.md` section 3 for the agent runtime,
   multi-tenancy and the security subsystem. Suspend (do not conclude) general-SNC falsification under section 14.
   Defer the goals in section 5.
3. **OD-P3 Storage foundation.** PostgreSQL (strict synchronous replication, an HA manager with its DCS) plus a blob
   store, behind one SNC interface. This is the dependency authorization `docs/ENGINEERING_PRINCIPLES.md` requires.
   It revises the standing rule "take ideas, do NOT adopt or embed those systems" (cody, 2026-11-16, held in
   project memory and quoted in `docs/AFR_001_BUILD_VS_REUSE.md` section 7) for this product.
4. **OD-P4 Durable execution.** Reuse DBOS Transact for stage 1, with SNC owning the evidence schema. The 3.5
   triggers decide a switch to B.
5. **OD-P5 Evidence contract.** Accept section 2 (records, classes, E-1 to E-11, the commit/local split) as the
   direction for a new agent-runtime guarantees document. `docs/SNC_GUARANTEES.md` is not revised.
6. **OD-P6 Durability class "committed"**:
   - PostgreSQL `synchronous_commit = on`, with `synchronous_mode_strict` and at least two standbys (`ANY 1`), so
     writes stop rather than run on one copy;
   - `synchronous_commit` cannot be lowered per transaction (enforced by role setting);
   - external effects act only on replicated commits (E-1);
   - a client treats a cancelled or interrupted commit as in doubt.

   This is **not SNC's HA_DURABLE**. The differences: locally visible commits can be lost on failover, which is why
   the gateway waits for replication; recovery semantics are PostgreSQL's; there are no SNC terms or fencing below
   the database. HA_DURABLE is unchanged.
7. **OD-P7 Capture, retention and isolation defaults.** Keyed hashes and decisions are kept for the tenant-set
   retention period. Raw content follows the per-class policy. Secrets are never captured raw. Which shared
   components are split per tenant for regulated tenants is part of this decision.
8. **OD-P8 PoC and language.** Authorize AR-1 (7.1) and the product gate (7.2). AR-1 is in Python with DBOS Python,
   which needs no Rust exception. The product language is decided after AR-1. Whether SNC-ADR-RUST-001 applies to
   the runtime is decided then, explicitly.

## 9. Attempts to disprove this recommendation

1. **"This abandons SNC's thesis."** State-native *execution speed* is deferred. State-native *semantics* (authority,
   causes, activation on state, one owner) are the product. The vision lists this only as a fallback, so this is a
   pivot by owner decision. **Owner's call.**
2. **"Reusing DBOS contradicts owning the journal."** Corrected in this revision. The earlier draft rejected DBOS on
   Rust grounds, and the review showed that argument was circular.
3. **"PostgreSQL will not scale."** Possible. 3.3 lists the conditions. AR-1 measures, and C is preserved.
4. **"Evidence volume is unaffordable."** It is the real risk. Measured in AR-1. **Unresolved until measured.**
5. **"The product is just an SDK plus a vault plus OPA."** Possibly. That is the product gate (7.2). **Market
   question, unresolved.**
6. **"Strict synchronous mode trades availability."** Yes: with two standbys, one loss keeps writing and two stop
   it. The same trade as SNC's `m = 2`. **Accepted by design if OD-P6 is accepted.**
7. **"Tamper evidence is weak against operators."** It is evident up to the last external anchor, and only as strong
   as the KMS custody. Stated in E-4.
8. **"DBOS workflow determinism can silently diverge."** True in Rust ("Most divergence reports nothing"
   [EXT-SRC]). AR-1 tests it, and it is a 3.5 trigger.

## 10. Limits of this evidence

- Most external quotes came through a summarising fetch tool. That covers the DBOS papers and docs, Temporal, the
  AI Act (via the Commission's service desk), GDPR (via gdpr-info.eu) and crypto-shredding (Thoughtworks).
- DBOS source (Python 3.6.0, Rust 0.6.0) was read in package form.
- The Rust `(workflow_id, step_id)` claim comes from the correctness reviewer's reading of that crate.
- **UNVERIFIED:**
  - Stonebraker's 1981 CACM text;
  - DBOS limits pages, workflow size limits, and whether Conductor is needed in production;
  - Temporal's full event list;
  - OpenTelemetry GenAI convention status and content-capture defaults;
  - Restate's licence change terms;
  - AI Act and GDPR verbatim text;
  - OPA, Cedar and gateway product capabilities (not researched).
- Every performance number is HYP until AR-1.

## 11. Independent review

Three independent reviewers were asked to break the first draft (one on Opus for durable execution and databases,
one on Opus for strategy and consistency with SNC texts, one on Sonnet for security and governance). 40 defects were
raised; all are applied in this revision. The main changes:

- **Recommendation** (strategy 10, correctness 1-2): the first draft built its own journal and rejected DBOS on
  Rust-only grounds. That reasoning was circular, because Rust work is not authorized, and DBOS Rust can do the same
  app transaction with an idempotency row. The package now reuses DBOS (OD-P4), keeps arm B as the AR-1 baseline,
  and runs AR-1 in Python. Temporal is now judged on operations and waits, not on a false atomicity claim.
- **ADR scope** (strategy 1-3): the ADR now covers the product decision only. It names the override of the
  validation scope freeze and keeps the Agent-SNC falsification gate (7.2).
- **Durability** (correctness 3-4, strategy 12): OD-P6 now requires strict mode, two standbys, no per-transaction
  downgrade, and replication before effects. It is stated as not HA_DURABLE.
- **Fencing and crashes** (correctness 5-9, security 9): leases with epochs fenced at the gateway; activation-started
  records; blob GC leases; per-agent chains; re-checks inside the intent transaction; SERIALIZABLE isolation;
  timers, approvals and outbox handled.
- **PostgreSQL housekeeping** (correctness 10): freeze, WAL, partitioning, pooling.
- **Contract completeness** (strategy 5-6, 9): activation fields from the vision, the on-behalf-of chain, run
  events, capability snapshots, the candidate set, the commit/local split, and the missing measurements.
- **Prior art** (strategy 8): section 3.2 now covers the Stonebraker DBOS OS paper, Apiary, FoundationDB, Kafka and
  Raft.
- **Security** (security 1-8, strategy 16-17): RLS role model and pooling; keyed hashes with no existence oracle;
  erasure semantics; anchoring and key custody; the access-record stop rule and reader policy; untrusted content;
  the AI Act provider/deployer split; the credential gateway; competitor categories.
- **Accuracy** (strategy 4, 13-15, 18): vision citations corrected; the Rust ADR no longer silently retargeted; the
  example's approver corrected to Codex, relayed by cody; the merge facts confirmed (`c996ba7`); "the real run
  lacked enforcement" stated.

**Unresolved after review:**
- whether customers buy the enforcement layer (7.2);
- evidence volume and PostgreSQL capacity at scale (AR-1);
- whether DBOS will publish Rust transactional steps, and when;
- engineering effort for each arm (not estimated);
- whether a restart in place under Patroni exposes unreplicated commits beyond what E-1's replication check covers
  (untested);
- where the external anchor lives and who holds the signing key;
- erasure of third-party personal data found in retrieved content;
- SNC's role under the AI Act (infrastructure, provider or deployer);
- whether "suspend" is acceptable under EXPERIMENTAL_VALIDATION section 14, which is the owner's and Codex's call.
