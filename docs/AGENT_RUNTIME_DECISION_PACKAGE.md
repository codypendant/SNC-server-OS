# SNC Agent Runtime: Strategic Architecture Decision Package (PROPOSED, revision 2)

Status: **PROPOSED, for Codex and the owner.** Docs only.
- Revision 1 answered section 11 of Codex's review of PRs #181/#182 (`docs/handoff/PR181_PR182_Codex_Review.md`).
- Revision 2 answers Codex's HOLD review of PR #183 (`docs/handoff/PR183_Codex_Review.md`).

What it does **not** do:
- accept a product pivot;
- authorize implementation, AR-1, a dependency, C++ or Rust, or an override of the scope freeze;
- revise any SNC guarantee (HA_DURABLE included) or freeze anything.

The product direction is `docs/adr/SNC-ADR-PRODUCT-001.md` (revision 2), which needs cody's explicit yes. AFR-001,
L-1, LiveGrant, D-02, R-23, O-FLOOR, the #181 research and every model result are preserved unchanged.

Labels (`docs/ENGINEERING_PRINCIPLES.md`):
- **FACT**: read in this repository.
- **EXT**: an external source. Most external quotes came through a fetch tool that summarises pages, so they are
  not byte-verified.
- **EXT-SRC**: read in package source (the DBOS Transact Python 3.6.0 wheel and the `dbos` 0.6.0 crate).
- **INF**: inferred here.
- **HYP**: untested.

## 0. Recommendation in one page

**Make SNC's proposed first product a focused state-native runtime for persistent AI agents.** For the first
experiment only:
- **PostgreSQL** as the authoritative store;
- **DBOS Transact Python** for durable execution;
- both behind SNC interfaces, so either can be replaced.

**Stages.** Every part of this package is labelled with one:

| Stage | Contents | Status |
|---|---|---|
| **S1: focused runtime** | Persistent agent identity; durable tasks, events and recovery; authoritative memory; model-independent execution state; basic policy-controlled tool execution; lightweight provenance and causal history; lifecycle inspection and reconstruction | Proposed product. AR-1 (7.1) tests it, after authorization |
| **S2: verification** | Patroni HA and the failover checks (2.6), scale (10,000 agents, 1M timers), long-run PostgreSQL housekeeping | Only if S1 and the product gate justify it |
| **S3: enterprise governance expansion** | Comprehensive evidence capture; per-tenant cryptographic anchoring; retention and erasure infrastructure; enterprise approval and delegated authorization; access auditing and compliance; regulated multi-tenant deployment | A separate design candidate. Not part of the first release. Needs the product gate (7.2) and its own decision |

**What SNC owns in S1:**
- the agent, memory and task model;
- the provenance records (2.1) and the fencing and invocation rules (E-10, E-11);
- a basic policy gate on tool calls;
- the inspection and reconstruction API.

**What the first experiment would reuse** (candidates only, not approved dependencies) is PostgreSQL and DBOS
(section 3). Temporal is the alternative. Codex's preference applies: "prefer reusing ... durable workflow execution
unless benchmarks justify otherwise".

**Correctness corrections in this revision:**
- **E-10:** an epoch check cannot fence a network call. The contract now states the weaker guarantee that holds:
  - every send has an epoch-checked dispatched-attempt record, committed and established before it goes out;
  - outcomes are recorded even after a takeover;
  - calls in flight across an epoch change are recorded as completed, rejected or in doubt;
  - real fencing applies only where the tool checks a fencing token.
- **E-11:** a client-generated SNC invocation id is committed before the call. The provider's request id is
  attached when it arrives. Six invocation states are distinguished.
- **OD-P6:** PostgreSQL lets any transaction lower `synchronous_commit`. So durability is *established by observing*
  the standby's flush position on one timeline, never assumed from the setting. The bypass is gated, not prevented.
  External effects, model dispatch and client acknowledgements wait for establishment. Seven checks (C-1 to C-7) must pass before the class is accepted.

**Said plainly** [INF]:
- If OD-P1 and OD-P2 are accepted, SNC's first product stops being a high-throughput distributed object database.
- The per-core runtime and the custom replication become retained research (section 5).
- S1 is deliberately small. Whether anyone needs more than S1, or S1 at all, beyond an SDK over DBOS or Temporal is
  the product question (7.2), and it is open.

## 1. Product mission and differentiation

**Mission (proposed, S1).** SNC is a runtime where a persistent agent's identity, memory, tasks and execution state
live in authoritative, recoverable state, independent of any one model. For each consequential step, the runtime
can show:
- what the agent was shown;
- which policy allowed the action;
- what it did, including retries and failures;
- how state changed.

**Expansion (S3, candidate only):**
- what the agent *could* have seen (the candidate set);
- full delegation and on-behalf-of chains;
- who has read the evidence since;
- tamper evidence;
- regulated retention and erasure.

**Where this sits** [INF; the positioning is HYP until the product gate]:

| Category | Gives | Leaves to the user |
|---|---|---|
| Durable execution (Temporal, DBOS, Restate) | Resumable code, retries, timers | The agent and memory model, policy records, provenance, reconstruction |
| Agent frameworks with checkpoints (LangGraph) | Resumable graph state; but replay "re-executes nodes" and calls "fire again and may return different results" [EXT] | Exactly-once effects, provenance, governance |
| Policy engines (OPA, Cedar), credential and MCP gateways | Policy evaluation; credential brokering [INF, not researched here] | Durable state, transactional provenance, reconstruction |
| Observability (OpenTelemetry GenAI conventions) | Traces | Authority: a trace is not a transactional record of state |
| **SNC S1** | Agent state, memory, provenance records and the policy decision committed together with each step; recovery; reconstruction | Model internals (E-6); enterprise governance (S3) |

**Atomicity is not the moat** (section 3.4). Any durable engine can commit state and evidence together. What S1 adds
is the agent model and provenance by construction, for every agent on the runtime. Whether that is worth a product
is tested by 7.2.

The vision frames this as a fallback. The relevant text [FACT: `docs/handoff/SNC_Future_Architecture_Vision_v2.txt`
lines 22-28]:
- "If SNC is not compelling as a general enterprise substrate, evaluate a specialized persistent-agent platform
  before STOP".
- Its value list: "continuity, auditability, policy enforcement, recovery, context reconstruction, token
  efficiency, dormant-agent efficiency and multi-agent coordination".

The vision's model-independence principle, used here, is "The persistent state is the agent" (line 32).

## 2. Agent-state and provenance contract

### 2.1 Record types

| Record | Holds | Class | Stage |
|---|---|---|---|
| **Principal** | Humans, services and agents; role | Authoritative | S1 (tenant and project fields S3) |
| **Agent** | Persistent id; role; capability set (versioned); runtime configuration; memory root; lease epoch; version | Authoritative, versioned | S1 |
| **Memory object** | A typed fact, document or artifact; provenance (the producing activation); visibility | Authoritative, versioned, history kept | S1 |
| **Task** | Requester; goal; inputs; deadline; parent; status | Authoritative | S1. The full on-behalf-of chain (principal, then delegating agent, then this agent) is S3 |
| **Activation** | One step of an agent; holds its model invocations (E-11), code version, skill and tool-set versions, memory snapshot id, causal event | Immutable per state | S1 |
| **Model invocation** | SNC invocation id (client-generated); provider; model requested; parameters; manifest; state (E-11); provider request id and model id/version as returned, when available | Immutable per state | S1 |
| **Evidence manifest** | The ordered items delivered to the model: kind, id, version, content hash; and a whole-input hash | Immutable | S1. Trust levels and capture modes (redacted, quarantined, erased) are S3 |
| **Retrieval record** | The query and the candidate set (ids, versions, scores), including what was *not* delivered | Immutable | S3 |
| **Policy decision** | Policy id and version; evaluated inputs (by hash); result; obligations, such as approval required | Immutable | S1 (basic) |
| **Approval** | Approver; source (direct, or relayed by whom); hash of the approved action; decision; time | Immutable | S1, as a basic human approve/deny. Enterprise approval and delegated authorization are S3 |
| **Tool call** | Call id; idempotency key; tool and version; capability snapshot; epoch; argument hash; state (E-10); each attempt; result, rejection or in-doubt marker | Immutable per state | S1 |
| **Run event** | Cancellation, failure, timeout, recovery, epoch change (with its outstanding-call set, E-10) | Immutable | S1 |
| **State transition** | Object; prior version; new version; causing record | Immutable | S1 |
| **Wait / timer** | A condition or due time; owning task; fired-by | Authoritative until fired | S1 |
| **Evidence access** | Reader, item, purpose, time, in a separate append-only sink | Immutable | S3 |

**Classes:**
- **Authoritative**: the current truth, with one writer.
- **Immutable**: append-only. A correction is a new record that references the old one.
- **Reconstructable**: derived and rebuildable. This covers memory views, indexes, summaries, and state "as of" a
  time.
- **Not captured**: the model's internal reasoning. Reasoning text a provider *returns* is output.

### 2.2 Invariants (proposed)

**S1:**
- **E-1 Decide before acting.**
  - An external effect runs only through SNC's tool gateway.
  - It runs only after the policy decision and any approval have committed.
  - It also needs the call's dispatched record (E-10) to have committed **and to be established** (2.6).
  - The dispatch transaction locks the agent row first. It then re-checks that the task is not cancelled and that
    the policy inputs are unchanged.
  - This fences the *record*, not the send. A cancellation that commits after dispatch cannot recall the call
    (E-10 step 8).
  - The gateway is the only path to a tool. That is a deployment requirement, not something the runtime can prove.
- **E-2 One commit per consequential boundary.** A step outcome commits in one transaction together with:
  - its state transitions;
  - its provenance records;
  - its outbox entries;
  - its timer changes.

  Either all of them commit or none does. Which operations are boundaries is in 2.3. One exception exists: when the
  writer of an outcome is stale, the outcome commits alone and the current epoch derives the dependent transitions
  (E-10 step 4).
- **E-3 Manifest (S1, lightweight).**
  - Every item delivered to the model is listed by content hash and version.
  - In S1 the bytes are kept under one simple retention setting.
  - S3 adds completeness rules: every byte is listed by keyed hash, and for each item either the bytes are
    retrievable for the retention period or its capture mode says why not (redacted, quarantined, erased).
  - S3 also adds per-class retention.
- **E-5 Effects at least once, visible once.**
  - One idempotency key per tool call, the same across retries and epochs.
  - It removes duplicates of *the same call* only. It does not prevent a different action (E-10).
  - An outstanding call with no outcome is retried only if the tool honours keys. The retry is a new attempt under
    the current epoch, with the same key (E-10 step 5).
  - Otherwise the call is marked **in doubt** for its owner (E-10 step 9).
  - Outbox consumers deduplicate.
- **E-6 Observable boundary only.** The record claims what crossed the runtime boundary, nothing about what the
  model internally "considered".
- **E-7 Reconstruction.** For any tool call or state transition, the system returns its chain:
  - transition;
  - tool call (with every state);
  - approval;
  - policy decision;
  - model invocation (with every state);
  - manifest;
  - items.

  In S3 the chain also includes the retrieval record, and an erased item is returned as erased, not as an
  error.
- **E-10 One writer per agent; in-flight effects recorded, fenced only where the tool can fence.** See below.
- **E-11 Model invocations identified before dispatch.** See below.
- **Secrets are never captured raw** (S1 minimum of E-9).

**S3 candidates** (kept from revision 1, not first-release requirements):
- **E-4 Tamper evidence:**
  - per-agent hash chains. They are per agent, not per tenant, so they add no tenant-wide serial point;
  - a per-tenant Merkle batcher (RFC 9162 style [EXT]);
  - tree heads signed with a KMS- or HSM-held key and anchored externally;
  - verification by a party the database operator does not control.

  This is tamper-*evident* up to the last anchor.
- **E-8 Controlled and recorded evidence reads:**
  - a reader policy;
  - access records in a separate anchored sink, with bulk reads aggregated;
  - an auditor role separate from operators.
- **E-9 Minimization and erasure:**
  - a capture policy applied before writing;
  - keyed hashes (HMAC, per tenant or per subject);
  - erasure by destroying the subject key, after which the chain stays verifiable over the hashes as stored;
  - a documented legal basis per record class;
  - legal holds win.

#### E-10 One writer per agent (corrected)

Revision 1 said a stale activation "cannot make a call after a newer epoch exists". That is false, because no
database check can be atomic with a later network call. Revision 2 states option B from Codex's review: a weaker,
accurate guarantee. Option A (real fencing) is added only per tool, in step 10.

1. **Epoch and lock order.**
   - The agent row carries a lease epoch.
   - Every transaction that touches an agent's records locks the agent row **first**. This avoids lock-order
     deadlocks.
   - A takeover runs at READ COMMITTED. Its first statement is `SELECT ... FOR UPDATE` on the agent row, followed by
     `epoch = epoch + 1`. It reads the agent's calls only after it holds the lock, so it sees every transaction that
     committed before it [INF from PostgreSQL row locking; tested in AR-1].
2. **One current-state row per call.**
   - Each tool call and each model invocation has a single row holding its current state.
   - Every transition is a compare-and-set on that state, such as `UPDATE ... WHERE state = 'prepared'`.
   - History is appended separately.
   - Two transitions from the same state therefore cannot both succeed.
3. **Epoch-checked commits.** The activation's own decisions carry the epoch check (agent row `FOR SHARE`, with
   `epoch` equal to its own, taken first):
   - prepare;
   - dispatch;
   - policy and approval links;
   - state transitions;
   - memory writes;
   - outbox entries.

   A stale activation's decision commit fails.
4. **Outcome records are facts, not decisions, and are exempt from the epoch check.**
   - A tool's result, a rejection, a model output and a provider request id are written by compare-and-set on the
     call's own row, whatever the current epoch.
   - So an effect that happened is always recordable, even after a takeover.
   - The transitions that depend on an outcome are **not** exempt. If the outcome's writer is stale, the current
     epoch derives them from the recorded outcome. This is the one exception to E-2's single commit.
5. **Tool call states:**
   - **prepared**: the intent, with the policy decision;
   - **dispatched (attempt n)**: committed under the epoch check, then established (2.6). This is the last commit
     before the send. Every retry is a new attempt, committed under the then-current epoch with the same
     idempotency key;
   - **completed**, **rejected** (by the tool or by fencing) or **in doubt**.

   The gateway sends only after the attempt's dispatched record is established. Every send that can reach a tool
   therefore has its own epoch-checked record.
6. **What is guaranteed:**
   - No dispatched record commits for a stale epoch.
   - A dispatched call **may still reach the tool after a newer epoch exists**, because the send happens after the
     commit and the network can delay it. It is an **in-flight effect** of the older epoch: authorized when
     dispatched, and recorded (step 4).
7. **Takeover**, in the same transaction:
   - older-epoch tool calls still **prepared** become **rejected (superseded, never sent)**, and older prepared
     model invocations become **abandoned**. This is definite: a dispatch's compare-and-set and the takeover's
     compare-and-set contend on the same row, and the dispatch also needs the epoch it has lost;
   - older **dispatched** calls and invocations with no outcome form the **outstanding set**, recorded with the
     epoch change. The new activation sees that set in its manifest.
8. **Cancellation has the same window.**
   - A cancellation takes the same lock first and rejects prepared calls. It records its own outstanding set.
   - Calls already dispatched may still land after the cancel. They are recorded under step 4.
9. **Conflict rule (S1 default).** A new epoch may not dispatch to the same tool and resource scope while an older
   call there is outstanding.
   - **Resolution.** An outstanding call is resolved by a tool status query where one exists, matched to *this*
     call (step 11). Otherwise, after the tool's dispatch timeout plus a margin, it becomes **in doubt**.
   - **Who resolves in doubt.** An in-doubt call has an owner: the task's requester, or a configured human. It also
     has an escalation timeout.
   - **Liveness.** Until someone resolves it, the scope stays blocked. That trades liveness for safety, and it is
     stated as such.
   - **Admitted duplicate.** A human may resolve a call as "did not happen" and let the new epoch re-dispatch. A late
     old arrival then becomes a **duplicate effect**. It is recorded against the old call, and only an idempotency
     key or a fencing token can prevent it.
10. **Real fencing, per tool (option A).** A tool may accept the token (agent id, epoch) and reject, for that agent
    and resource scope, any epoch lower than the highest it has seen. This is the fencing-token pattern [EXT:
    Kleppmann, "How to do distributed locking", 2016].
    - At such a tool, a stale call that arrives **after the resource has seen a higher epoch for this agent** is
      rejected by the resource itself and recorded as rejected.
    - A stale call that arrives first is accepted. That is consistent, because it is ordered before the newer
      epoch's effects at that resource.
    - Because the resource itself orders calls, fencing-token tools are **exempt from step 9**.
11. **Gateway crash and status queries.**
    - On recovery, a dispatched attempt with no outcome is outstanding (step 9). A prepared call was never sent.
    - A status query must identify this call by its idempotency key, or by an effect that only this call can
      produce. A state change someone else could have made is not proof. In that case the call is in doubt.
    - A call is never re-sent with a new idempotency key.

What the idempotency key does: it makes retries of *one* call safe at tools that honour keys. It does not stop an
older epoch from performing a *different* action. That action is an in-flight effect under step 6, recorded and
subject to step 9.

#### E-11 Model invocation identity (corrected)

Revision 1 put a provider request id in the activation-started record. A server-generated provider id cannot exist
before the provider receives the request.

| State | Recorded how | Meaning after a crash |
|---|---|---|
| **prepared** | Epoch-checked commit. Holds the SNC invocation id (client-generated and unique), manifest, provider, model requested, parameters and epoch | The input has not left. The invocation is abandoned. A re-call is a new invocation |
| **dispatched** | Epoch-checked commit, established (2.6), immediately before the send | The input **may** have reached the provider. From here, data sent to the provider is on record |
| **provider accepted** | Optional and non-blocking. An epoch-exempt commit (E-10 step 4) of the provider's request id as soon as it is observable: from response headers or a first stream event | The provider is known to have received it. Its id survives a crash mid-stream |
| **response received** | Not durable. The evidence model records only that it is held in memory, and it is absent after a crash | Lost on a crash |
| **output committed** | Epoch-exempt commit. Holds the output as returned, the provider request id, the model id and version as returned, and usage | The original response is on record |
| **result unknown** | Commit, by recovery, timeout or takeover, for a dispatched invocation with no committed output | The provider may have produced a response that SNC does not have |

Rules:
- The SNC invocation id is sent to the provider as **metadata** where the provider accepts it [HYP, per provider].
  It is not an idempotency key, because every re-call gets a new id.
- A committed output is never regenerated.
- An output that was not committed is **never reconstructed and presented as the original response**. A re-call is a
  new invocation, with a new id and a `retry_of` link.
- A response retrieved from the provider is recorded as **retrieved from provider**, with that provenance, not as the
  original receipt. Retrieval works by a provider id recorded as accepted, or by the SNC id where the provider
  supports a metadata lookup [HYP].
- A genuine output that arrives after *result unknown* may still commit, by compare-and-set. It is the original
  receipt, arriving late, and it is marked as late.

Cost: three or four commits per model call. That is small against model latency [HYP; measured in AR-1].

### 2.3 What needs a database transaction, and what runs locally (Codex section 6, option B)

| Needs a consequential commit (E-2) | Runs locally, with no durable boundary |
|---|---|
| Task acceptance; model invocation prepared, dispatched and output; policy decision; approval; tool call prepared, dispatched and outcome; state transitions; timer arm or fire; cancellation; epoch takeover; memory writes that later decisions may cite | Context assembly and ranking (its *result* is the manifest); token streaming; caches; draft scratch; read-only memory queries; UI state |

Rule [INF]: anything that can authorize, cause or explain a consequential action commits. Anything that can be
recomputed from committed records does not.

### 2.4 End-to-end example: this project merging a PR

This project's own workflow is a natural first use case [INF]. It has:
- an owner (cody);
- an external reviewer (Codex);
- AI workers (Claude threads);
- standing rules (today held in memory files);
- consequential actions (merges, contract changes).

Case: PR #181 merged on 2026-11-21 [FACT: merge `c996ba7`, called with `expectedHeadSha 1009b83`]. Each **C#** is
one transaction. All records are S1.

| Step | What happens | Commit | Records |
|---|---|---|---|
| 1. Task acceptance | The coordinator assigns "merge #181 as evidence, then write the package" to agent `thread-f0vhs6` | C1 | Task T1 (requester: coordinator, for cody); agent v12 → v13, epoch 4 |
| 2. Evidence capture | Context: the Codex review text (relayed by cody); rules `merge-rules v7` ("Claude may merge when tests pass"; "contract changes need cody's explicit yes"; both held in project memory today, not the repo); PR #181 at `1009b83`; the check-run result | C2 | Manifest X1; invocation I1 *prepared* |
| 3. Model execution | Send, after the dispatched record is established; then the output | C3, send, C4 | I1 *dispatched*; I1 *output committed* (with the provider request id) |
| 4. Policy check | `merge-rules v7` on {docs only, Codex approval relayed by cody, CI green, no contract change}: allowed | C5 | Policy decision P1. A frozen-contract change would instead have created a wait for cody's explicit yes |
| 5. Approval | Codex's verdict "APPROVED FOR DOCUMENTATION MERGE", relayed by cody | C5 | Approval AP1 (approver Codex, relayed by cody, action hash) |
| 6. Tool action | Tool call TC1 (key `T1-merge-181`, epoch 4): prepared, then dispatched and established, then the GitHub merge with the head sha | C5, C6, send, C7 | TC1 prepared; dispatched; completed with `c996ba7` |
| 7. State transition | In the same transaction as the outcome | C7 | PR #181 open → merged; memory M41 "#181 merged as evidence, HYBRID on hold" (provenance I1, TC1); outbox: notify the coordinator |
| 8. Audit retrieval | Later: "on whose authority was #181 merged, and did anyone accept raft-rs?" | read | merge → TC1 → AP1 (Codex, via cody) → P1 (rules v7) → I1 → X1 (the text shown). M41 answers the second question |

**What the real run lacked** [FACT]:
- the agent held GitHub credentials directly;
- its rules were memory text;
- the only record of the decision is this paragraph.

S1 closes the first two gaps at a basic level. S3 would add delegation chains, anchoring and access records.

Crash cases [INF]:
- **Kill after C6 and before C7.** TC1 is outstanding.
  - Recovery queries the PR. TC1 is completed only if the merge commit's actor and head sha match TC1.
  - A merge by someone else proves nothing about TC1, so it is in doubt (E-10 step 11).
- **Kill after C3 and before C4.** I1 becomes *result unknown*. A re-call is invocation I2, `retry_of` I1. I1's output
  is never presented as received.
- **Takeover by epoch 5 after C5 and before C6.** TC1 is rejected (superseded, never sent).
- **Takeover by epoch 5 after C6.** TC1 is in the outstanding set.
  - Epoch 5 cannot dispatch another merge of #181 until TC1 resolves.
  - If epoch 4's send lands late, its result is still recorded against TC1 (E-10 step 4).
  - A second merge call on an already merged PR returns an error rather than repeating the merge. That makes it
    safe, not idempotent [INF from GitHub's merge API behaviour; not tested].
- **Failover after C6 commits locally but before it is established.** The send never happens, because the gateway
  waits for establishment (2.6).

### 2.5 Cost and sensitivity of capturing exact inputs

**S1, single tenant:**
- Manifests store content hashes. Bytes are stored once, by hash, in segments: instructions, each policy version,
  each memory version, each document version, each tool result.
- Most of a step's context repeats from the previous step, so it costs a reference, not a copy [HYP; measured in
  AR-1].
- Blob GC uses mark epochs, with a grace period longer than the longest step. Pending blobs are leased from write
  until commit.
- The model-visible input is the ordered segments plus versioned framing code. The exact bytes can therefore be
  rebuilt and checked against the whole-input hash.
- Secrets are never captured raw.

**S3 candidates** (kept from revision 1):
- Keyed hashes (HMAC per tenant or subject), so that low-entropy values cannot be brute-forced.
- Per-tenant deduplication with no existence oracle, and never across tenants or subject keys.
- Trust tags for untrusted content. Untrusted content cannot supply arguments to a consequential call unless policy
  gates that call.
- A quarantine mode.
- Third-party personal data captured as hash-only, because no per-subject key exists at capture time.
- A blob store with its own durability class (two copies).
- Retention tiers: raw content short; hashes and decisions kept for the tenant's period.

**Volume** [HYP]:
- Context runs 20-200 KB per model call. At 1,000 steps/s that is 1.7-17 TB a day raw, before deduplication.
- That is an S2 scale question, but the S1 schema should not preclude deduplication.

### 2.6 Durability class "committed" (corrected; the basis of OD-P6)

Revision 1 said `synchronous_commit` "cannot be lowered per transaction (enforced by role setting)". That is wrong.
PostgreSQL lets any session or transaction change `synchronous_commit`: "This parameter can be changed at any time;
the behavior for any one transaction is determined by the setting in effect when it commits" [EXT: PostgreSQL 18,
runtime-config-wal.html#GUC-SYNCHRONOUS-COMMIT]. A role-level setting is only a default, and startup options
override it too.

**This revision does not prevent the bypass. It makes the bypass harmless.** Effects, model dispatch and client
acknowledgements are gated on durability that is *observed*, never on the setting. Accepting that substitution is
part of OD-P6.

The revision separates four layers.

**(a) Desired PostgreSQL configuration (candidate):**
- `synchronous_commit = on` as the cluster default;
- a synchronous standby named in `synchronous_standby_names`;
- `fsync = on` on primary and standbys, on storage that honours flushes;
- for S2: Patroni `synchronous_mode` with `synchronous_mode_strict`. Strict mode blocks writes "until at least one
  synchronous replica comes up" [EXT: Patroni, replication_modes].

**(b) Application-enforced configuration:**
- The runtime's database layer never lowers the setting.
- Inside each consequential transaction it checks `current_setting('synchronous_commit')` before COMMIT and aborts
  if the value is lower.
- This covers the runtime's own code only. It is **not a prohibition**.

**(c) Database-enforced restrictions:** **none is known** [INF].
- `synchronous_commit` is a user-context parameter, so `GRANT SET ON PARAMETER` cannot restrict it.
- Core PostgreSQL has no generic hook on built-in parameters. `ProcessUtility_hook` sees `SET`, but not
  `set_config()` or startup options.
- A deferred constraint trigger that rejects lowered commits has known bypasses:
  - `SET CONSTRAINTS ALL IMMEDIATE` followed by a later `SET LOCAL`;
  - a table owner disabling the trigger;
  - a transaction that touches only tables without the trigger.

The guarantee in (d) does not rely on (b) or (c) [INF].

**(d) Actual guarantee: durability is established by observation.**

1. **Bound.** Immediately after COMMIT returns, the session reads in one query:
   - `pg_current_wal_insert_lsn()`, which is the logical end of WAL and so at or past the end of the commit record;
   - the current timeline id;
   - the transaction id (`pg_current_xact_id()` is taken before COMMIT).

   `pg_current_wal_lsn()` (the *write* position) is not used. Under asynchronous commit it can sit below the commit
   record [EXT: PostgreSQL 18, functions-admin and wal-async-commit].
2. **Established.** A commit is *established* when, on the same timeline, the standbys the commit rule requires
   report `flush_lsn` at or past the bound:
   - S1: the single standby, with `sync_state = 'sync'` read in the same query;
   - S2: FIRST k or ANY k members of Patroni's recorded sync or quorum set.

   The reader needs `pg_monitor`. Without it the LSN columns read NULL, and NULL counts as not established.
3. **Reads count too.** A transaction that reads data and leads to a dispatch or an acknowledgement takes
   `pg_current_wal_insert_lsn()` at read time, and waits for that to be established. This covers a gateway decision
   made from rows that are locally visible but not established.
4. **Prefix property** [INF from WAL being one ordered stream on a timeline, with standby flush a prefix of it].
   Establishing a commit also establishes every earlier commit **on the same timeline**. That includes
   cancelled-wait and restart-marked commits.
5. **Timeline change.** Any check still pending when the timeline changes becomes **in doubt**. Comparing LSNs
   across timelines is wrong: a promoted standby's new writes can pass an old bound while the old commit was lost.
   The check is resolved on the new primary by transaction id (`pg_xact_status`) or a row lookup. It is never
   resolved by an LSN comparison.
6. **What waits for establishment:**
   - external effects (each dispatched attempt, E-10);
   - model dispatch (E-11);
   - every "committed" acknowledgement the runtime API gives a client.

   A transaction that bypasses the setting therefore cannot cause an effect or an acknowledgement on unestablished
   data. Commits whose establishment lags beyond the expected synchronous latency are logged, so a bypass is
   visible, not silent.
7. **Lost connection.** If the connection drops after COMMIT is sent but before the bound is read, the outcome is
   unknown. Recovery looks up the transaction id or the row, then applies steps 2 and 5.
8. **What S1 establishes, and what it does not:**
   - S1 establishes a second durable copy before any effect.
   - It establishes **nothing about failover**. Survival of the primary's loss depends on promoting a standby that
     holds every established commit, which is C-6 in S2.
   - Locally visible commits that are not established can be read and then lost. Patroni documents that a
     cancelled wait makes "transaction changes become visible for other backends" [EXT: Patroni]. PostgreSQL
     documents that after a restart, waiting transactions "will be marked fully committed" [EXT: PostgreSQL 18,
     warm-standby.html#SYNCHRONOUS-REPLICATION].
   - Recovery semantics are PostgreSQL's and Patroni's. There are no SNC terms or fencing below the database.

   **This is not SNC's HA_DURABLE**, which is unchanged.

**(e) Required checks before the class is accepted** (specified, not run; each needs authorization):

| Check | Scenario | Pass condition | Where |
|---|---|---|---|
| C-1 bypass | A transaction runs `SET LOCAL synchronous_commit TO off` and commits while the standby is stopped | It commits locally (expected; this documents the gap). No acknowledgement, and no effect on it or on anything that read it, until it is established. **Liveness:** once the standby returns, it is established and the dependent dispatch proceeds | AR-1 environment |
| C-2 cancelled commit | Cancel a backend waiting for synchronous acknowledgement. This tests PostgreSQL's cancel behaviour; Patroni's version is covered in S2 | Other sessions, including the gateway, can see the change (expected). The gateway itself reads the row and does not dispatch until it is established | AR-1 environment |
| C-3 primary restart | Restart the primary while commits wait | Waiting commits show as committed after the restart (expected). Nothing is acknowledged or dispatched on them until they are established | AR-1 environment |
| C-7 bound | Thousands of commits with `synchronous_commit` set to `on`, `local` and `off`, with a large `wal_writer_delay` | The bound is ≥ the commit record's **end** LSN (the next record's start, from `pg_waldump`) in every case | AR-1 environment |
| C-4 failover before establishment | A local commit, then loss of the primary before the standby flushes, including a promoted timeline that later passes the old bound | The commit is lost. No effect was dispatched and no acknowledgement given. The pending check went in doubt at the timeline change and resolved as "not committed" | S2 (Patroni) |
| C-5 standby loss, strict mode | Stop the synchronous standby; separately, power-fail a standby just after it acknowledges | Writes block and nothing is acknowledged. Writes recover when a standby returns. An acknowledged flush survives the standby's power loss | S2 |
| C-6 promotion | Under the chosen Patroni mode, including a partitioned old primary | The promoted standby holds every commit that had been established | S2 |

The class is a proposed direction until C-1 to C-7 pass. AR-1 runs C-1, C-2, C-3 and C-7 only.

## 3. Build versus reuse

### 3.1 Candidates, against the contract and Codex's criteria

These are **candidates for the first experiment**, not production dependencies.

| Criterion | A1: PostgreSQL + Temporal (alternative) | A2: PostgreSQL + DBOS Transact Python (first-experiment candidate) | B: SNC-owned journal on PostgreSQL | C: SNC substrate on raft-rs/OpenRaft |
|---|---|---|---|---|
| Atomic state + provenance (E-2) | Yes, if each consequential step is an Activity that writes state, provenance and an idempotency row in one app-database transaction. Waits and timers live in Temporal and must be mirrored [INF] | Yes. Python/TS transactional steps commit "atomically in the same database transaction" [EXT]. Rust 0.6 has none public; a step can do the same with an idempotency row keyed by `(workflow_id, step_id)` [EXT-SRC, per review] | Yes, by construction | Yes, if built |
| Effects (E-5) | Activities "may be executed multiple times" [EXT] | Steps "tried at least once but ... never re-executed after they complete" [EXT] | Ours | Ours |
| Long-lived agents | 51,200 events / 50 MB per run, 2 MB payload [EXT]; continue-as-new | No stated cap found (UNVERIFIED) | No cap | No cap |
| Provenance | History is a replay log; provenance lives in the app database | `operation_outputs` is a recovery cache [EXT-SRC]; provenance lives in our schema, same database | Ours | Ours |
| Multi-tenancy (S3) | Namespaces; "A single Namespace is still multi-tenant" [EXT] | Ours (6.2) | Ours | Ours |
| Operational burden | Four services plus persistence plus visibility store [EXT] | A library plus PostgreSQL. Cross-executor recovery uses Conductor (proprietary) or our own recovery scan | PostgreSQL only | Highest (#181) |
| Maturity | Server mature; Rust SDK 0.6 "Public Preview" [EXT] | Python/TS in production use (HYP for scale); Rust 0.6 "pre-production" [EXT-SRC] | New code | New code |
| Engineering cost [INF, not estimated in hours] | Adapter plus service operations | Adapter | Journal, recovery, queues, timers | Largest |
| Throughput (S2) | Not the constraint at S1 scale [HYP] | Bounded by PostgreSQL (3.3) [HYP] | Same | Highest, unneeded so far |
| Latency per boundary | One app transaction plus a Temporal round trip | One transaction | One transaction | Lowest |
| Time to a usable S1 | Medium | **Shortest** | Medium | Longest |
| Licence | MIT | MIT core; Enterprise proprietary [EXT-SRC] | PostgreSQL License | Apache-2.0 / MIT |

**D** (staged) is A2 for the first experiment, with storage and durable execution behind SNC interfaces.

### 3.2 Prior art and what it settles

- **Stonebraker's DBOS OS work** [EXT, PVLDB 15(1) 2021]:
  - Claim: "a distributed transactional DBMS should be the basis for a scalable cluster OS".
  - It built the scheduler, files and IPC as tables (FIFO scheduling at about 750K tasks/s).
  - It measured about 40 µs per DBMS invocation, against about 1 µs per syscall.
  - The 1981 CACM text is UNVERIFIED here.
  - *Settles:* scheduling state in a DBMS works, and its per-call cost is negligible against model latency.
- **Apiary** [EXT, arXiv 2208.13068]:
  - Exactly-once functions run as DBMS transactions.
  - Provenance "records for each data item all function executions that accessed or modified it", at up to 15%
    throughput cost.
  - *Settles:* transactional provenance is affordable, and it gives a budget for AR-1.
- **DBOS Transact** [EXT, EXT-SRC]:
  - steps are checkpointed in PostgreSQL;
  - transactional steps commit together with the application's writes;
  - recovery re-enqueues pending workflows;
  - workflow code must be deterministic, and "Most divergence reports nothing" in Rust.
  - *Settles:* it is the first-experiment candidate. Divergence is tested in AR-1 item 8.
- **Temporal** [EXT]:
  - event history with replay;
  - a strong engine, but its history is not a provenance record.
  - *Settles:* it stays the alternative.
- **PostgreSQL 18 and Patroni** [EXT, #181 4.4]:
  - synchronous commit, which a transaction can override;
  - quorum `ANY n`;
  - checksums on by default;
  - strict mode;
  - the documented cancelled-wait and restart cases.
  - *Settles:* 2.6.
- **Raft, raft-rs, OpenRaft** (#181):
  - Patroni relies on a Raft- or Paxos-based DCS (etcd, Consul or ZooKeeper) [EXT].
  - *Settles:* S1 writes no consensus.
- **FoundationDB** (#181 4.5):
  - its lesson is deterministic simulation: "It seems unlikely that we would have been able to build FoundationDB
    without" it [EXT].
  - *Settles:* AR-1's fault injection is seeded and deterministic.
- **Kafka KIP-497/903/966** (#181):
  - eligibility is checked at the decider, at the moment of the change.
  - *Settles:* E-1's re-checks happen inside the dispatch transaction.
- **Fencing tokens** [EXT, Kleppmann 2016]:
  - a lease alone cannot fence a delayed request; only the resource checking a token can.
  - *Settles:* E-10 step 10.

### 3.3 Re-examining "every mutation leaves the owning core" (Codex section 8)

[INF; every number HYP until measured]
- **#181 compared operations.** SNC sync-replicated ran at 640K ops/s against PostgreSQL's 29K [FACT:
  `docs/REPLICATION.md`]. That compares object operations, not agent steps.
- **An agent step** is one model call (0.3-30 s), some tools, and about 6-9 consequential commits (2.3, with E-10 and
  E-11's states).
- **Latency.** With commits of about 1-5 ms against a synchronous standby on a LAN, persistence is on the order of a
  few percent of step latency. That is a latency statement, not a capacity one.
- **Capacity (S2).** 10,000 concurrent agents stepping every 10 s is about 1,000 steps/s, or 6,000-9,000 commits/s.
  Whether that is reasonable depends on:
  - partitioning;
  - manifest rows;
  - no tenant-wide serial point;
  - no LISTEN/NOTIFY on the commit path;
  - pooling;
  - transaction-ID freeze (roughly 500-800 M a day against the default `autovacuum_freeze_max_age` of 200 M);
  - WAL volume;
  - queue table bloat.
- **The dominant cost at scale is evidence bytes** (2.5).
- **Where SNC's core-local runtime could still matter:** very large dormant-agent populations, tight-latency memory
  retrieval, and provenance traversal at scale. Each is an S2 measurement, not a premise.

### 3.4 Can the differentiator be a thin library over an existing engine?

**Atomicity, yes** [INF from EXT]. Over DBOS or Temporal, a step writes state, provenance and an idempotency row in
one app transaction.

What a library alone does not give:
- the gateway as the only path to tools;
- provenance for every agent, by construction, not only for code that calls the library;
- in S3, tenant-level integrity, minimization and reader control.

If the market wants only a library, the product is an SDK over DBOS or Temporal. AR-1 item 8 and the product gate
measure exactly that (7.1, 7.2).

### 3.5 If a candidate fails a test

1. **First**, decide whether the limitation can be addressed safely through configuration.
2. **Then** through an adapter.
3. **Then** through another established implementation: DBOS TypeScript, or Temporal.
4. **Only then** consider option B (an SNC-owned journal). That needs its own owner decision. One failed test does
   not authorize it.

Other triggers:
- **DBOS's divergence risk cannot be contained** (AR-1 item 8): apply steps 1-4 above.
- **The product language is Rust and DBOS Rust is still pre-production**: wait, or apply steps 1-4 above.
- **PostgreSQL capacity is the measured bottleneck** at a target scale (S2): evaluate C, or the SNC core, behind the
  storage interface, starting from #181.
- **A customer already runs Temporal**: an adapter. The provenance schema does not change.

## 4. Mapping of existing SNC technology

| SNC asset | Use in the agent runtime | Status |
|---|---|---|
| Stable 128-bit ObjectIDs, location-independent (invariant 2) | Agent, memory, invocation and provenance ids | Reuse the concept (S1) |
| One owner per object, terms and fencing (invariant 3, G-31, AFR-001) | E-10 epochs and takeover. AFR-001's lesson, that a stale writer acts between check and effect, is exactly the E-10 correction | Reuse the lessons (S1) |
| Obligations and outbox in one WAL group (CORE_CONTRACTS 13, R14) | E-2 outbox entries in the step transaction | Reuse the design (S1) |
| Sessions and dedup; invariant 18 | Idempotency keys (E-5) | Reuse the design (S1) |
| Full post-op images | State transitions with prior and new versions | Reuse the design (S1) |
| Checkpoints as snapshot roots; space UUID as branch identity (FUTURE_ARCHITECTURE 3.1, 3.3) | Memory snapshot ids; later, agent sandboxes | Later stage |
| FM-1..16 and the G-14 fail-closed method | 2.6's layered analysis and the in-doubt rules | Reuse the method (S1) |
| #181 research; the C++ engine, per-core shards, NUMA work; state-native object and scheduling work | Option C if triggered; a future specialized backend | Preserved, not extended |

### 4.1 AGENTS.md invariants and the runtime [INF]

- **Carry over:**
  - 2 (identity independent of location);
  - 3 (one owner, here per agent);
  - 8 (correctness before benchmark wins);
  - 9 (benchmark competent alternatives: AR-1 item 8 compares against plain DBOS);
  - 15 (persistent state authoritative, delivery recoverable);
  - 16 (no externally visible computation before recovery completes: the gateway refuses during an agent's
    recovery);
  - 18 (a duplicate is resolved before any refusal that invites a resend).
- **Do not apply to S1:**
  - 4, 5 and 14 (core locality and the hot path);
  - 7 (native runtime by default);
  - 10-13 and 17 (placement, slots, networking OS).
- **Conflict for the owner:** `docs/ENGINEERING_PRINCIPLES.md` says "maintaining SNC's independent architecture". A
  production PostgreSQL dependency would need its own decision. An experiment dependency does not change that rule
  (OD-P3).

## 5. Goals that would become deferred or optional

**Deferred research**, preserved and not expanded while the product decision is open:
- the general SNC database;
- the custom replication work: AFR-001, L-1, LiveGrant, A-1 and the LG gaps;
- D-02, R-23, O-FLOOR and D-06;
- the raft-rs/OpenRaft research (#181);
- the per-core and NUMA architecture;
- the state-native object and scheduling work;
- the R640 campaigns (CP-1A, Workload A, X-2);
- cross-node exchange and slot migration;
- the Seastar, DPDK and VPP comparisons;
- the networking-OS integration;
- kernel work;
- the 1M-object benchmark programme;
- the C++ reference's role under SNC-ADR-RUST-001.

**S2:**
- Patroni HA;
- C-4 to C-6;
- scale and housekeeping campaigns.

**S3:** the enterprise governance expansion (section 6.2).

**Optional:**
- branches and sandboxes;
- PITR and backup beyond PostgreSQL's own;
- a specialized backend (C);
- token-efficiency, dormant-agent and multi-agent features.

General-SNC falsification under `docs/EXPERIMENTAL_VALIDATION.md` section 14 is **suspended, not concluded**, and
only if OD-P2 says so. Its evidence stays valid if it is resumed.

## 6. Security, governance and evidence retention

### 6.1 S1 minimum (single tenant, lab)

- TLS in transit and disk encryption at rest.
- Secrets are never captured raw.
- Tool credentials are held by the gateway's configuration, not by agents, so the policy gate cannot be bypassed by
  an agent's own code.
- One database role for the runtime, NOSUPERUSER and NOBYPASSRLS, that does not own the tables.
- PostgreSQL data checksums (on by default in PG 18 [EXT]).
- Basic capability sets per agent, snapshotted on each call.

### 6.2 S3 enterprise governance candidate (kept from revision 1, by stage)

- **Tenant isolation:**
  - separate migration-owner, runtime, audit-reader and anchor roles;
  - `FORCE ROW LEVEL SECURITY` on every table, because owners and `BYPASSRLS` roles bypass policies [EXT];
  - `SET LOCAL` tenant context;
  - no SECURITY DEFINER functions, and `security_invoker` views;
  - RLS as defence in depth only;
  - a database per tenant for regulated tenants, which still leaves the control plane, KMS, blob credentials, HA
    manager and gateway shared;
  - isolation tests, including pooled-connection leaks and constraint-error side channels.
- **Encryption:** envelope encryption under per-tenant or per-subject keys in a KMS; keyed hashes.
- **Authorization:**
  - workload identities;
  - capabilities scoped (tool, object class, later lineage) and attenuated on delegation;
  - on-behalf-of chains against confused-deputy calls: the gateway authorizes against the end principal's rights
    *and* the agent's capabilities.
- **Credential gateway as the blast radius:**
  - a secret manager;
  - short-lived scoped credentials;
  - per-tenant separation;
  - rotation;
  - an egress policy;
  - onboarding without staff access.

  Tools that accept only long-lived keys are a named risk.
- **Evidence access:** E-8, plus pgAudit ("detailed session and/or object audit logging") [EXT].
- **Integrity:** E-4, plus verification jobs.
- **Retention and law** [EXT, summarised; verbatim UNVERIFIED]:
  - AI Act Art. 12 (automatic logging for high-risk systems).
  - Art. 19 (providers keep logs for at least six months).
  - Art. 26(6) (deployers).
  - SNC would usually be infrastructure, so its job is to make the customer's retention settable and provable.
  - GDPR Art. 5(1)(e) and 17, with the carve-outs in 17(3)(b) and (e), which need a documented basis per record
    class.
- **Sensitive data:**
  - a capture policy applied before writing;
  - untrusted and third-party data handled as in 2.5 S3;
  - "track everything" is not the default (Codex review of #181/#182, section 4).

## 7. First proof of concept and product validation (proposal only; needs authorization)

### 7.1 AR-1, rescoped to Codex's eight items

**Environment:**
- one lab VM, not the R640;
- PostgreSQL 18, one primary plus one synchronous streaming standby, with no Patroni;
- DBOS Transact Python;
- a filesystem blob store;
- a stub model provider: deterministic, records the exact bytes it receives, configurable latency, returns a
  request id;
- three fake tools that log every call they receive:
  - one honours idempotency keys;
  - one honours fencing tokens (E-10 step 10);
  - one does neither.

**Crash-point matrix**, enumerated in the preregistration before any run. The crash points are:
- before and after each commit in 2.3;
- between dispatched and send;
- between send and outcome;
- during a takeover;
- during a cancellation;
- during an establishment wait.

Each crash point is a kill -9 of the app, run with a fixed number of seeds (proposed: 20 per point [HYP]), seeded and
deterministic. PostgreSQL restarts where a check calls for one. Each run has a time cap (proposed: 30 minutes
[HYP]).

**Acceptance criteria**, fixed before any run:
- Correctness criteria need **zero violations** across the whole matrix.
- A run that hits its cap, or does not complete, is **UNRESOLVED**, not PASS.

| # | Item (Codex) | Pass condition |
|---|---|---|
| 1 | One persistent agent with authoritative state | After every crash point, the agent's state and version equal the last established commit. No stale-epoch *decision* (E-10 step 3) ever commits. Outcomes may commit from a stale writer, as E-10 step 4 allows |
| 2 | One durable task spanning several steps (at least three) | The task resumes after every crash point. No completed step re-executes its effects. No committed step output is lost |
| 3 | One model call with a manifest | The whole-input hash equals the hash of the bytes the stub received. Invocation states follow E-11. A crash after dispatch and before output commit yields *result unknown*, and any re-call is a new invocation. No uncommitted output is ever presented as received |
| 4 | One policy-gated tool action | A denied call never appears in a tool's log. Every call in a tool's log has an established dispatched-attempt record, committed before it, with its policy decision |
| 5 | Idempotent and non-idempotent effects | **Idempotent tool:** exactly one effect per logical call across the matrix. **Non-idempotent tool:** every logged call maps to exactly one dispatched attempt, and for each logical call every logged call beyond the first has an in-doubt or duplicate record. No logged call lacks a record. **Fencing-token tool:** a stale call arriving after the tool has seen a higher epoch for the agent is rejected and recorded as rejected |
| 6 | Crash and restart recovery at key boundaries (the whole matrix) | Items 1-5 hold at every crash point. Takeover marks older prepared calls rejected and prepared invocations abandoned, and records the outstanding set. No transaction deadlocks under the lock order. Durability checks C-1, C-2, C-3 and C-7 (2.6) pass |
| 7 | Historical reconstruction | For every effect and state transition, the reconstruction query returns what was shown (manifest), authorized (policy, approval), executed (every attempt and invocation state) and committed (transitions). Each answer is checked against the tool and stub logs |
| 8 | Direct comparison with plain DBOS | The same scenario is written in DBOS Python without SNC's layer, using steps, transactions and an idempotency row. Report which of items 1-7 plain DBOS meets as written, and the lines of code it needs to meet each one it does not. Report SNC's added latency per step (p50, p99) and bytes per step. Report whether DBOS detects deliberately changed workflow code between a crash and its recovery (determinism divergence) |

**Performance** is reported, not gated, except for one budget. The owner sets the maximum added latency per step
over plain DBOS before the run. A proposed figure is 10 ms at p50 [HYP].

**Outcome**, tied to item 8:
- **GO**: items 1-7 pass, the latency budget holds, and plain DBOS fails at least one of items 1-7 without
  substantial extra code. The owner sets the line count that counts as substantial before the run.
- **REFINE**: items 1-7 pass only after a schema or adapter change (3.5).
- **NARROW**: plain DBOS meets items 1-7 within that line count. That is evidence for an SDK, not a runtime.
- **STOP**: a correctness item fails and 3.5 cannot fix it, or the budget fails by more than twice.

**Deferred to S2** (all kept from revision 1):
- the complete Patroni HA setup and C-4 to C-6;
- the 1M-agent timer and event activation test;
- the 100, 1,000 and 10,000 concurrent-agent campaigns, including a single hot tenant;
- memory-retrieval and provenance-query latency at scale;
- recovery time after termination at scale;
- storage growth per 10,000 steps;
- WAL volume, vacuum and transaction-ID age over a long run;
- fault cases: a GC race on a deduplicated blob, an approval racing a cancellation, a double timer fire, and a
  refund-over-limit case with a real approval wait.

**Deferred to S3:**
- the full cryptographic anchor infrastructure;
- regulated multi-tenant rollout;
- tenant-isolation and pooled-connection tests.

### 7.2 Product validation gate (the vision's separate Agent-SNC gate), before any enterprise expansion

The owner preregisters the thresholds before results [HYP figures below]. **Failing any step is NARROW (an SDK) or
STOP, recorded as such.**

1. **Working runtime on this project.**
   - Run S1 on this project's own multi-agent engineering environment (coordinator, threads, Codex, cody) for a
     defined period. This goes beyond AR-1 and **needs its own authorization** (OD-P8).
   - Pass: it answers the 2.4 audit questions from records, which the current setup cannot.
2. **Comparison** of the same scenario against:
   - DBOS;
   - Temporal;
   - existing agent frameworks (LangGraph at least);
   - an observability stack (OpenTelemetry GenAI traces);
   - a policy gateway (OPA or an MCP gateway).

   Use AR-1 item 8's method, and report what each needs to match items 1-7.
3. **Buyer interviews** about decision reconstruction and governance.
   - At least five potential enterprise buyers.
   - Record the question each needs answered, and what they use today.
   - Record whether gateway-held credentials are acceptable in their environment.
4. **Function beyond an SDK, a workflow engine, an observability system and a policy gateway.**
   - At least two buyers must name a need their current stack cannot meet and that **S1 as built** does.
   - Needs that only S3 would meet are recorded, but they do not count toward a pass.

Demand for agent auditing in general is not counted as demand for SNC. **S3 design work starts only after this gate
passes, and only by its own decision.**

## 8. Owner decisions required (exact)

Each needs cody's explicit yes. None is requested as a bundle.

1. **OD-P1 Product direction.** Accept SNC-ADR-PRODUCT-001 revision 2: a focused state-native agent runtime (S1),
   with enterprise governance as an optional later expansion (S3).
2. **OD-P2 Scope freeze.**
   - Decide whether to override `docs/EXPERIMENTAL_VALIDATION.md` section 3 for the focused agent runtime only.
     Multi-tenancy and the security subsystem stay frozen with S3.
   - Decide whether general-SNC falsification under section 14 is suspended.

   OD-P1 alone does neither.
3. **OD-P3 First-experiment candidates.**
   - PostgreSQL and DBOS Transact Python, for AR-1 only, behind SNC interfaces. They are not production
     dependencies. Temporal stays the alternative.
   - The prior-art rule stays unchanged until a separate production decision. It reads "take ideas, do NOT adopt
     or embed" (cody, 2026-11-16, in project memory, quoted in `docs/AFR_001_BUILD_VS_REUSE.md` section 7).
4. **OD-P4 S1 provenance contract.** Accept, as direction for a future agent-runtime guarantees document:
   - the S1 records (2.1);
   - E-1, E-2, E-3 (lightweight), E-5, E-6 and E-7;
   - E-10 and E-11 as corrected;
   - the commit/local split (2.3).

   `docs/SNC_GUARANTEES.md` is not revised.
5. **OD-P5 Product validation gate.** Accept 7.2 and set its thresholds before results.
6. **OD-P6 Durability class "committed".** Accept 2.6 as direction:
   - Durability is established by observation, on one timeline.
   - Effects, model dispatch and acknowledgements wait for it.
   - A `synchronous_commit` bypass is **not prevented**, and the gating takes the place of a prohibition.

   It becomes a contract only after C-1 to C-7 pass. S1 establishes nothing about failover. It is **not
   HA_DURABLE**.
7. **OD-P7 AR-1.** Authorize AR-1 as rescoped in 7.1, with these set before any run:
   - the matrix;
   - the seed count;
   - the time cap;
   - the latency budget;
   - the GO line count.
8. **OD-P8 Later decisions.** Nothing is requested now. Each of the following is a separate later decision:
   - running S1 on this project (7.2 step 1);
   - S2 verification;
   - S3 enterprise design, including capture, retention and isolation defaults and which shared components are
     split per tenant;
   - the product language;
   - whether SNC-ADR-RUST-001 applies to the runtime;
   - any custom journal (3.5).

## 9. Attempts to disprove this recommendation

1. **"This abandons SNC's thesis."**
   - State-native execution *speed* is deferred.
   - State-native *semantics* are the product: authority, causes, activation on state, and one owner.
   - It would be a pivot if accepted, and that is the owner's call.
2. **"S1 is just an SDK over DBOS."** Possibly. AR-1 item 8 and the gate's step 4 test this directly.
   **Unresolved.**
3. **"E-10 still allows a stale effect."** Yes, by design.
   - An in-flight call can land after a takeover or a cancellation.
   - It is recorded, and it blocks conflicting new calls in its scope until resolved.
   - A resolved-then-redispatched call can duplicate.
   - Only tools that check tokens are fenced.

   All of this is stated as such.
4. **"Reusing DBOS contradicts owning the journal."** Revision 1 corrected this. DBOS's divergence risk is tested in
   AR-1 item 8.
5. **"Three or four commits per model call is heavy."** Small against model latency [HYP]. It is measured in AR-1.
6. **"Observation-based durability depends on the primary's view of standby flush."** Yes. 2.6 adds the timeline
   rule and the sync-set rule, and C-6 tests promotion. **Unresolved until C-6.**
7. **"Strict synchronous mode trades availability."** Yes. With two standbys, losing one keeps writing and losing
   two stops writes. It is the same trade as SNC's `m = 2`, accepted by design if OD-P6 is accepted (S2).
8. **"Tamper evidence is weak against operators."** It is evident only up to the last external anchor, and only as
   strong as the KMS custody (S3, E-4).
9. **"PostgreSQL will not scale."** That is an S2 question. 3.3 lists the conditions, and C is preserved.
10. **"Evidence volume is unaffordable."** That is an S2 and S3 risk. **Unresolved until measured.**

## 10. Limits of this evidence

- Most external quotes came through a summarising fetch tool. That covers:
  - the DBOS papers and docs;
  - Temporal;
  - the PostgreSQL and Patroni docs;
  - the AI Act and GDPR;
  - the fencing-token article.
- The 2.6 reviewer could not fetch live pages (proxy refusal). Its PostgreSQL function and parameter claims come
  from the documentation as it knows it, checked against this project's earlier quote notes.
- DBOS source was read in package form.
- The Rust `(workflow_id, step_id)` claim comes from a reviewer's reading of that crate.
- **UNVERIFIED:**
  - Stonebraker's 1981 CACM text;
  - DBOS limits, and whether Conductor is needed in production;
  - Temporal's full event list;
  - OpenTelemetry GenAI conventions;
  - Restate's licence terms;
  - AI Act and GDPR verbatim text;
  - OPA, Cedar and gateway capabilities;
  - Patroni's promotion behaviour for C-6;
  - GitHub's response to a repeated merge;
  - provider support for client-supplied metadata and for response retrieval (E-11).
- Every performance number is HYP.

## 11. Independent review

### 11.1 Revision 1: three-way review (40 defects, all applied)

Three reviewers broke the first draft:
- correctness, on Opus;
- strategy and SNC-text consistency, on Opus;
- security and governance, on Sonnet.

Their main changes:
- DBOS reused instead of an own journal (revision 1 wording, now "first-experiment candidate");
- the ADR limited to the product decision;
- durability settings;
- epochs and activation records;
- PostgreSQL housekeeping;
- contract completeness;
- prior art;
- security;
- the example's accuracy.

Codex's review of PR #183 then found three issues the three-way review had missed: E-10, E-11 and OD-P6.

### 11.2 Revision 2: focused review of the corrections (three-way)

Three independent reviewers were asked to break revision 2's first draft:
- E-10 and E-11, on Opus;
- 2.6/OD-P6 against PostgreSQL and Patroni, on Opus;
- faithfulness to Codex's PR #183 review and preservation of revision 1, on Sonnet.

They raised 31 defects. All are applied:

- **E-10, 11 defects.**
  - *Blocking:* the epoch check made late outcomes unrecordable. Outcomes are now epoch-exempt facts.
  - *Blocking:* "rejected, never sent" was definite only under some isolation levels. Each call now has one
    current-state row with compare-and-set transitions, and a takeover locks first at READ COMMITTED.
  - Also fixed:
    - lock-order deadlock (lock the agent row first);
    - takeover now covers model invocations;
    - provider id commits early;
    - fencing-token scope and its exemption from the conflict rule;
    - in-doubt owner, liveness and the admitted duplicate;
    - one dispatched record per attempt;
    - the cancellation window;
    - two overclaims in the 2.4 crash cases;
    - the SNC id is metadata, not an idempotency key.
- **2.6, 10 defects.**
  - *Blocking:* `pg_current_wal_lsn()` is the write position and fails under asynchronous commit. Now
    `pg_current_wal_insert_lsn()`.
  - *Blocking:* C-7 checked a record's start in the wrong regime. It now checks the end LSN, under on, local and
    off.
  - *Blocking:* the prefix property breaks across timelines. Now timeline-bound, with resolution by transaction id.
  - *Blocking:* "a synchronous standby" was under-specified. Now members of the commit rule's sync set, with
    `pg_monitor`.
  - Also fixed:
    - OD-P6 states that the bypass is gated, not prevented;
    - known bypasses of the trigger idea are listed;
    - reads that lead to a dispatch are gated;
    - standby fsync and power loss;
    - the lost-connection path;
    - C-1 liveness;
    - "S1 establishes nothing about failover";
    - labels and citation anchors.
- **Faithfulness, 10 defects.**
  - Revision 1 content dropped rather than reclassified: restored here by stage, and revision 1 is kept verbatim in
    `docs/AGENT_RUNTIME_DECISION_PACKAGE_REV1.md` (section 12).
  - Also fixed:
    - the dangling 11.3 reference;
    - AR-1 criteria made measurable (matrix, seeds, cap, "logical call", outcome thresholds tied to item 8);
    - DBOS divergence added to item 8;
    - gate gaps (own authorization for step 1, S1 as built, any failure is NARROW or STOP, observability and policy
      baselines);
    - the reuse and pivot wording;
    - the ADR's scope line and its mention of S2.

### 11.3 Unresolved after both reviews

- whether customers want more than an SDK over DBOS or Temporal (7.2);
- evidence volume and PostgreSQL capacity at scale (S2);
- Patroni promotion under the chosen mode (C-6);
- whether a database-enforced restriction on `synchronous_commit` exists that is worth its cost;
- whether DBOS will publish Rust transactional steps, and when;
- engineering effort for each arm (not estimated);
- where an S3 anchor would live and who holds the signing key;
- erasure of third-party personal data in retrieved content (S3);
- SNC's role under the AI Act (S3);
- whether "suspend" is acceptable under EXPERIMENTAL_VALIDATION section 14 (OD-P2, the owner's and Codex's call);
- provider-by-provider support for invocation metadata and response retrieval (E-11).

## 12. Revision 1, retained by stage

Revision 1 is kept verbatim in `docs/AGENT_RUNTIME_DECISION_PACKAGE_REV1.md` and marked superseded. Its E-10, E-11 and
OD-P6 text is wrong, and sections 2.2 and 2.6 here replace it. The rest maps by stage as follows:

| Revision 1 content | Stage now |
|---|---|
| Mission sentences 1-7, positioning table | S1 (sentences 2, 4, 5, 6) and S3 (1, 3 in full, 7) |
| Records: retrieval record, evidence access, tenant and project fields, on-behalf-of chain, trust and capture modes | S3 |
| E-4, E-8, E-9; keyed hashes; erasure; dedup without an existence oracle; quarantine; retention tiers | S3 |
| Section 6 (tenant isolation, KMS, credential gateway, pgAudit, AI Act, GDPR) | S3 (6.2), with an S1 minimum (6.1) |
| OD-P6 strict mode with two standbys; Patroni failover | S2 |
| AR-1 scale measurements and extra fault cases | S2 (7.1) |
| Prior art, options table, 3.3 performance re-examination | Kept, labelled |
| Product gate thresholds | Revised in 7.2 |
