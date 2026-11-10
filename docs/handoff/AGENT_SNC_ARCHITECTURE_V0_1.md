# Agent-SNC v0.1 Architecture and ACB Replacement Vision

**Status:** Architecture synthesis / design input for team review  
**Implementation direction:** Rust for Agent-SNC  
**Purpose:** Capture the current Agent-SNC design before implementation begins and preserve the reasoning behind the planned ACB replacement.

---

## 1. Core Vision

Agent-SNC should provide a complete autonomous development team as durable, inspectable state rather than as a collection of long-running model processes.

The target is a system in which SNC can track the observable execution history of every agent: what work it was given; which documents, messages, memory objects, artifacts, and versions it read; which model executed it; which tools it called; what tool results it received; what memory it created or changed; what decisions it made; what artifacts it produced; what work it delegated; which subscriptions fired; which state transitions occurred; which leases/fencing epochs were active; what failed or retried; and what humans approved or changed.

The goal is a highly efficient autonomous engineering organization that can operate with minimal human oversight while remaining auditable, reproducible, recoverable, controllable, and explainable.

### Fundamental principle

> **SNC durable state is the agent. The model is replaceable compute temporarily assigned to that agent.**

Claude, GPT/Codex, Kimi, Mercury, local models, or future models may execute the same logical agent. The model does not define the agent's identity.

---

## 2. Agent Identity and Durable State

An `Agent` is a persistent SNC object with a stable ObjectID.

An Agent should reference or own:

- identity and role;
- capabilities;
- project memberships;
- current tasks and historical work;
- authoritative memory;
- outstanding obligations;
- subscriptions;
- wait conditions;
- event cursors;
- project and policy/rule versions;
- activation history;
- current execution authority when active;
- provenance;
- model and tool permissions.

An agent can exist indefinitely without a running LLM. A dormant agent should require essentially no model compute.

---

## 3. SNC-Native Document/Object Layer

Agent-SNC should include the document-database-style layer previously discussed. This is **not MongoDB**, but an SNC-native structured document/object system.

Documents should support at least:

- strings;
- integers and floating point values;
- booleans;
- timestamps;
- arrays;
- maps/objects;
- ObjectID references;
- artifact/blob references;
- typed extensions where required.

Every durable document should have:

- stable ObjectID;
- document type;
- schema/version;
- authoritative current version;
- historical versions;
- provenance;
- ownership/authority metadata;
- timestamps;
- references to other SNC objects.

This lets agents exchange substantial semantic work rather than reducing collaboration to tiny state flags or transient broker packets.

---

## 4. Events Move Execution; Documents Move Knowledge

This should be a central Agent-SNC rule:

> **Events move execution. Documents move knowledge.**

Events should be small, durable, ordered, causal signals. Documents hold the rich semantic content.

Example:

```text
WorkOrder:2419
  objective: Implement D.2 cross-node follow-up exchange accounting
  requirements: ...
  constraints: ...
  acceptance_criteria: ...

Event:9001
  type: WorkAssigned
  target: Helper
  ref: WorkOrder:2419
```

The event makes Helper runnable. Helper then reads the detailed WorkOrder document.

---

## 5. WorkOrder as the Generic Delegation Primitive

`WorkOrder` should be a first-class document type for substantial agent-to-agent delegation.

Suggested fields:

- creator;
- assignee;
- project;
- objective;
- background/context;
- requirements;
- constraints;
- acceptance criteria;
- dependencies;
- allowed tools;
- required verification;
- references to designs, decisions, observations, artifacts, and prior work;
- expected deliverables;
- priority;
- deadline if applicable;
- status;
- provenance.

A WorkOrder must be domain-neutral. A PR or Git SHA is only one possible artifact reference. WorkOrders should be able to represent software engineering, research, network configuration, incidents, game development, plumbing estimating, document review, benchmark analysis, customer work, infrastructure maintenance, and other structured tasks.

---

## 6. Durable Messages

Agent-to-agent messages should also be durable SNC documents.

Example:

```text
Message:8803
  from: QA
  to: PM
  thread: D2-review
  subject: Possible generation-counter defect
  body: <detailed explanation>
  references:
    - WorkOrder:2419
    - BenchmarkResult:551
    - Decision:742
  priority: normal
```

A lightweight `MessageReceived` event references the Message document. The detailed message body does not need to fit inside the event.

---

## 7. Shared Team Board / Project Room

The current ACB shared message board should remain a first-class Agent-SNC capability.

Agent-SNC should provide a `Channel` or `CollaborationRoom` representing the shared working environment. All team members can see the durable board history.

Do **not** make the board one giant mutable document. Model it as:

- `CollaborationRoom`;
- `Channel`;
- `Thread`;
- individually addressable `Message` documents;
- an ordered channel event stream;
- durable per-agent cursors.

The board is the team's common room. Structured documents such as WorkOrders, Decisions, Policies, and Artifacts remain the authoritative project state.

---

## 8. Direct Message vs Channel Message vs WorkOrder

Agent-SNC should distinguish at least three communication modes:

1. **Direct Message** — Agent A communicates directly with Agent B.
2. **Channel Message** — Agent A posts into the shared team/project room.
3. **WorkOrder** — Agent A delegates authoritative structured work.

A conversational comment should not automatically become an authoritative requirement. A WorkOrder should.

---

## 9. Threads and Conversation Graphs

A conversation should be a graph of individually addressable objects instead of one giant chat transcript.

Example:

```text
Thread:500
  ├── Message:501
  ├── WorkOrder:502
  ├── Message:503
  ├── Observation:504
  ├── BenchmarkResult:505
  ├── Decision:506
  ├── Artifact:507
  └── Message:508
```

Activation context can retrieve only the relevant objects instead of loading the entire historical conversation.

---

## 10. Current State vs Historical State

Important objects should have a clearly authoritative current version while retaining the complete historical chain.

Normal execution resolves current state automatically. Historical state remains queryable for questions such as:

- Why was this decision made?
- Which benchmark caused the architecture to change?
- Which agent introduced a rule?
- What requirement existed before the current one?
- Why was an earlier approach rejected?

History should support explanation and audit without polluting every activation context.

---

## 11. SNC Event Fabric

The event/subscription layer should become an SNC-native subsystem rather than an agent-specific add-on.

Working name: **SNC Event Fabric**.

It should provide:

- durable events;
- ordered streams;
- subscriptions;
- structured filters/predicates;
- wait conditions;
- event cursors;
- wake signaling;
- coalescing;
- activation creation;
- live delivery to running agents;
- recovery/replay;
- cross-node event delivery semantics.

Agent-SNC would be a major consumer, but the Event Fabric should be generic enough for other SNC workloads.

---

## 12. Durable Event Model

Candidate event types include:

- `WorkAssigned`;
- `TaskSubmitted`;
- `WorkCompleted`;
- `ReviewRequested`;
- `ReviewCompleted`;
- `MessageReceived`;
- `ChannelMessagePosted`;
- `DocumentChanged`;
- `ArtifactCreated`;
- `DecisionAvailable`;
- `ApprovalGranted`;
- `TimerExpired`;
- `ToolCompleted`;
- `BuildFailed`;
- `AgentFailed`;
- `LeaseExpired`;
- `PolicyChanged`.

An Event should contain at least:

- EventID;
- type;
- source;
- target and/or scope;
- referenced ObjectIDs;
- project/channel;
- causal parent;
- timestamp;
- stream sequence/cursor;
- provenance;
- optional priority/coalescing key.

Events should be durable and append-oriented.

---

## 13. SNC-Native Pub/Sub

Subscriptions fill the same architectural role previously considered for MQTT or another message broker, but the internal abstraction should be SNC-native.

A Subscription should support structured predicates such as:

```text
event.type == ReviewRequested AND project == SNC
```

```text
event.type == WorkAssigned AND target == Helper
```

```text
Task.status changed to SUBMITTED AND Task.team == QA
```

```text
Artifact.kind == benchmark AND Artifact.result == FAILED
```

This creates:

> **Durable content-aware pub/sub over SNC objects and events.**

A topic-style syntax may exist for convenience, but should compile into structured predicates rather than defining the internal data model.

---

## 14. State-Transition Subscriptions

Subscriptions should not be limited to explicit messages.

QA should be able to subscribe directly to project state, for example:

```text
Task.status -> SUBMITTED
```

Then:

```text
Helper completes implementation
        ↓
Task becomes SUBMITTED
        ↓
QA subscription matches
        ↓
QA becomes runnable
```

PM does not have to remember to explicitly message QA.

This should also apply to builds, approvals, artifacts, dependencies, incidents, and other state transitions.

---

## 15. Wake Policies

Not every delivered event should consume model compute.

Subscriptions should support policies such as:

- `DELIVER` — visible to the agent but does not wake it;
- `WAKE` — make the logical agent runnable;
- `COALESCE` — combine repeated equivalent events before activation;
- `ONCE` — one-shot wait/subscription;
- `URGENT` — request interruption/yield at the next safe boundary.

Examples:

- TaskAssigned -> WAKE;
- ordinary FYI board message -> DELIVER;
- RepositoryChanged -> COALESCE + WAKE;
- ApprovalGranted -> ONCE + WAKE;
- emergency `@team STOP` -> URGENT.

This prevents activation storms and wasted token usage.

---

## 16. WaitCondition

An agent with no useful work should durably state what makes it runnable again.

`WaitCondition` should support:

- event waits;
- state-transition waits;
- timers;
- tool completion;
- dependency completion;
- approvals;
- compound predicates.

Examples:

```text
WAIT FOR MessageReceived(target=Helper)
```

```text
WAIT UNTIL Task:55.status == SUBMITTED
```

```text
WAIT UNTIL Task:A == COMPLETE AND Task:B == COMPLETE
```

```text
WAIT UNTIL Build:991.status != RUNNING
```

This removes large amounts of polling.

---

## 17. No-Lost-Wakeup Invariant

A critical Agent-SNC correctness invariant is that work can never arrive in the gap between checking for work and going to sleep.

Unsafe sequence:

```text
Agent checks inbox -> empty
Event arrives
Agent enters WAITING
```

The equivalent of these actions must be atomic:

```text
consume events through cursor N
+
register WaitConditions
+
Agent.state = WAITING
```

Guarantee:

- if an event committed before the wait transaction, the agent observes it;
- if the wait committed first, the later event matches the wait and makes the agent runnable.

There must be no lost-wakeup gap.

---

## 18. Logical Wake vs Model Wake

When an event or state transition matches a subscription/wait, SNC should first wake the **logical Agent**, not immediately launch a particular LLM.

The result is an `Activation`:

```text
Activation:551
  agent: Helper
  cause: Event:9001
  state: READY
  priority: normal
```

The scheduler then decides when and where it runs and which model executes it.

---

## 19. Activation Ownership, Leases, and Fencing

A worker executing an Activation must hold explicit authority.

The Activation should track:

- owner worker;
- authority epoch/fencing token;
- lease expiration;
- lifecycle state.

Only the current authority may commit resulting state.

If a worker/model dies:

1. the lease expires or is revoked;
2. another worker claims the Activation;
3. the authority epoch changes;
4. a stale old worker cannot commit later.

This reuses SNC's identity, ownership, epoch, and fencing philosophy.

---

## 20. Activation Context Instead of Giant Chat History

When an Activation is scheduled, Agent-SNC should construct a bounded context from authoritative state.

Potential contents:

- agent identity and role;
- activation cause;
- current WorkOrder/task;
- authoritative project state;
- current decisions;
- applicable policies and rules;
- relevant documents;
- relevant memory nodes;
- recent events;
- outstanding obligations;
- available tools;
- artifact references;
- prior activation summary where useful.

The runtime should not blindly inject the full historical transcript.

---

## 21. Model Independence

The scheduler may attach any compatible execution resource:

- Claude;
- GPT/Codex;
- Kimi;
- Mercury;
- local/open-weight models;
- future providers.

Model choice may depend on:

- task type;
- capability;
- cost;
- latency;
- context requirements;
- availability;
- policy;
- priority.

The persistent Agent remains unchanged when the model changes.

---

## 22. Full Observable Audit Trail

The long-term vision is an autonomous development team where every observable aspect of agent execution is traceable.

Persist at least:

- activation start/end;
- activation cause;
- model/provider/configuration;
- documents/objects retrieved;
- exact versions read;
- messages delivered;
- memory nodes retrieved;
- memory writes;
- runtime-supplied context packages where appropriate;
- model outputs;
- tool calls;
- tool parameters;
- tool outputs;
- files/artifacts read or written;
- state transitions;
- WorkOrders accepted/submitted/completed;
- decisions;
- events emitted;
- subscriptions fired;
- policy checks;
- leases/fencing changes;
- errors/retries;
- human approvals/interventions.

The objective is full **system-level provenance of observable inputs, outputs, and actions**. Hidden internal model reasoning is not assumed available or required.

---

## 23. Provenance Everywhere

Important durable state should identify where it came from.

A Decision, for example, may record:

```text
Decision:993
  created_by: Helper
  activation: 551
  work_order: 2419
  based_on:
    - Benchmark:901
    - Design:455
  model: Claude-...
  tools: GitHub
  created_at: ...
```

This supports audit, reproducibility, explanation, postmortems, enterprise compliance, and agent performance analysis.

---

## 24. Memory Architecture

Memory should remain a graph of durable documents/objects rather than one giant mutable memory blob.

Useful memory object types include:

- FactLearned;
- DecisionMade;
- TaskAccepted;
- PlanChanged;
- ArtifactCreated;
- WorkCompleted;
- DelegationMade;
- ImportantToolResult;
- Observation;
- Policy/ProjectRule.

Memory must distinguish:

- current authoritative state;
- historical state;
- agent-private memory;
- project/team memory.

Every memory write should have provenance.

---

## 25. Live Collaboration Requirement

Agent-SNC must support a genuinely live team environment in addition to dormant asynchronous execution.

The current ACB is often live-ish with tens of seconds of delay. SNC should move toward push-driven collaboration.

The shared Channel should be a durable ordered event stream. A newly committed channel event should signal relevant subscribed agents immediately rather than waiting for a polling interval.

On one SNC node, event propagation should be designed to be effectively immediate relative to model latency.

---

## 26. Authoritative Channel Ordering

If two agents post at nearly the same time, SNC should assign one authoritative order at commit.

Example:

```text
Helper -> seq 95001
QA     -> seq 95002
```

Every participant sees the same stream order.

Logical causality remains separately represented through fields such as `reply_to`, `caused_by`, and references. Stream order and causal relationships are related but not identical concepts.

---

## 27. Per-Agent Channel Cursors

Each agent should maintain durable stream cursors such as:

- delivered_through;
- seen_through;
- processed_through;
- acknowledged_through.

Important distinction:

> Being present in a model context is not equivalent to fulfilling the obligation represented by a message.

Critical messages or WorkOrders may require explicit acknowledgement or create a durable obligation.

---

## 28. Warm Collaboration Mode

Agent-SNC should support both cheap dormant agents and a live-team mode.

Candidate conceptual states:

- DORMANT;
- WAITING;
- WARM_IDLE;
- RUNNABLE;
- RUNNING.

A `WARM_IDLE` agent:

- is not performing inference;
- burns no tokens simply by waiting;
- retains live subscriptions/mailbox readiness;
- can become runnable immediately when relevant work arrives.

After a configurable idle period it can transition fully to DORMANT.

---

## 29. Activation Mailbox

A running Activation needs a live mailbox so that team communication can affect work in progress.

Do not mutate model context halfway through token generation. Instead inject live updates at safe boundaries such as:

- between model turns;
- before/after tool calls;
- after tool results;
- explicit yield/checkpoint points.

Example:

```text
PM: @Codex Do not modify replication.cpp. Helper owns it now.
```

The event enters Codex's Activation mailbox immediately and is injected before the next meaningful action.

Urgent events can request an early yield at the next safe boundary.

---

## 30. Mentions and Priority

The shared room should support stronger semantics than plain broadcast delivery.

Suggested behavior:

- normal message -> visible immediately, no automatic interrupt;
- `@Agent` -> immediate delivery, evaluate at next safe boundary;
- urgent mention -> scheduler requests early yield;
- `@team` -> all project participants receive it.

This preserves live collaboration without forcing every message to wake every LLM.

---

## 31. CollaborationRoom and Human Presence

A first-class `CollaborationRoom` should represent the live working environment.

It may reference:

- project;
- Channel;
- participants;
- presence;
- active WorkOrders;
- threads;
- shared documents;
- policies;
- active Activations.

Humans are authorized participants in the same room.

This allows durable live commands such as:

```text
@team STOP. Do not merge.
```

or:

```text
@QA verify this before Helper continues.
```

Human messages and approvals should be observable and attributable just like agent actions.

---

## 32. Build Our Own Semantics; Reuse Commodity Plumbing

Engineering rule:

> **Own the semantics; reuse commodity plumbing.**

Build SNC-native components when they participate in SNC correctness semantics:

- durable Event semantics;
- ordering;
- subscriptions;
- WaitConditions;
- no-lost-wakeup transactions;
- Channels/Messages;
- Activation creation;
- ownership/leases/fencing;
- replay/recovery;
- document references;
- scheduling;
- provenance.

Use mature libraries for implementation machinery that does not define SNC semantics:

- TLS;
- TCP/QUIC;
- WebSockets;
- HTTP;
- serialization;
- compression;
- cryptography;
- async I/O;
- metrics/export formats.

Do not spend engineering time recreating solved commodity infrastructure.

---

## 33. MQTT, NATS, Kafka, and External Systems

Do not put MQTT, NATS, Kafka, or another broker at the center of Agent-SNC.

They can be optional edge adapters:

```text
MQTT -> adapter -> SNC Event -> Subscription -> Activation
```

```text
SNC Event -> adapter -> MQTT/NATS/Kafka/webhook/Slack/email
```

If implementing a non-core integration ourselves would consume disproportionate time, use an established solution. But SNC remains authoritative for native event, subscription, activation, and recovery semantics.

---

## 34. Rust Direction

Agent-SNC v0.1 should be implemented in Rust unless a concrete integration constraint later proves otherwise.

Rust is a strong fit for:

- state machines;
- ownership and leases;
- fencing;
- event sequencing;
- concurrent schedulers;
- subscription processing;
- durable wait semantics;
- document versioning;
- tool execution tracking;
- recovery;
- cross-node protocols.

Possible initial modules:

```text
agent.rs
document.rs
event.rs
subscription.rs
wait.rs
activation.rs
scheduler.rs
lease.rs
provenance.rs
work_order.rs
message.rs
channel.rs
thread.rs
memory.rs
model.rs
tool.rs
audit.rs
adapter/
```

Exact crate boundaries should be decided after interfaces and transaction boundaries are reviewed.

---

## 35. Candidate Durable Types

Initial Agent-SNC types will likely include:

- Agent;
- Project;
- CollaborationRoom;
- Channel;
- Thread;
- Message;
- WorkOrder;
- Task/WorkItem;
- Event;
- Subscription;
- WaitCondition;
- Activation;
- Lease/Authority;
- Policy;
- Decision;
- Observation;
- Artifact;
- BenchmarkResult;
- MemoryNode;
- ToolExecution;
- ModelExecution;
- Approval;
- Timer.

All should use stable SNC ObjectIDs and explicit references rather than repeatedly copying context.

---

## 36. Candidate State Machines

### Agent

```text
DORMANT / WAITING
        ↓
RUNNABLE
        ↓
RUNNING
        ↓
WARM_IDLE / WAITING
```

### Activation

```text
READY
CLAIMED
RUNNING
COMPLETED
FAILED
LEASE_EXPIRED
REQUEUED
```

### WorkOrder

```text
DRAFT
ASSIGNED
ACCEPTED
IN_PROGRESS
BLOCKED
SUBMITTED
UNDER_REVIEW
COMPLETE
REJECTED
CANCELLED
```

Names are provisional. The important property is that transitions are durable, attributable, replay-safe, and subscription-visible.

---

## 37. Core Correctness Invariants

Agent-SNC v0.1 should explicitly prove/test at least:

1. No lost wakeups between cursor advancement, wait registration, and WAITING.
2. A stale Activation owner cannot commit after fencing/lease replacement.
3. One authoritative current state per durable object/version chain.
4. Successfully durable Events are not silently lost.
5. Subscription matching is replay-safe/idempotent.
6. Coalescing cannot discard an event that carries a required obligation.
7. Message/channel stream ordering is authoritative.
8. WorkOrder acceptance/completion transitions are idempotent.
9. Tool effects have explicit provenance and retry semantics.
10. Recovery recreates unfinished obligations without duplicating completed work.
11. A model crash cannot erase durable Agent identity/task/memory.
12. A running Agent can receive live mailbox updates without corrupting its Activation transaction.
13. Dormant Agents require no model process.
14. Human policy/stop actions can fence or interrupt future commits at defined safe boundaries.

---

## 38. First Implementation Vertical

Do not build the full vision in one pass.

Recommended first vertical:

1. Document + ObjectID references/versioning.
2. Agent object.
3. Durable Event.
4. Channel + Message.
5. Subscription.
6. WaitCondition.
7. Atomic event-cursor + wait transition.
8. Activation.
9. Activation lease/fencing.
10. WorkOrder.
11. Minimal scheduler.
12. One model adapter.
13. ToolExecution recording.
14. Provenance/audit.
15. Live Activation mailbox.
16. ACB compatibility/mirroring bridge.

This is enough to prove the central architecture without first implementing every future capability.

---

## 39. First Serious Demonstration

The first major demonstration should prove durable continuation across model replacement.

Scenario:

1. PM creates a WorkOrder for Helper.
2. WorkAssigned wakes Helper.
3. SNC schedules Helper on Model A.
4. Helper begins work and records intermediate durable state/tool results.
5. Kill Model A/process deliberately.
6. Its lease expires or is fenced.
7. SNC reassigns the same logical Helper work to Model B.
8. Model B reconstructs the required activation context from SNC durable state.
9. Helper completes the WorkOrder.
10. Task becomes SUBMITTED.
11. QA's state-transition subscription fires automatically.
12. QA reviews it.
13. ReviewCompleted wakes PM.
14. Messages, reads, tool calls, state transitions, artifacts, memory changes, decisions, and authority changes remain auditable.

If this works cleanly, the central Agent-SNC thesis is proven.

---

## 40. ACB Migration Strategy

Do not perform a flag-day replacement of the existing ACB.

Run Agent-SNC beside ACB initially.

Suggested progression:

1. mirror selected ACB events/messages into SNC;
2. mirror one agent/lane;
3. let SNC own one bounded low-risk WorkOrder;
4. validate replay/recovery and ownership semantics;
5. compare behavior with ACB;
6. expand to more agents;
7. move team board/channel authority into SNC;
8. make SNC authoritative;
9. retire superseded ACB components.

Natural mappings:

```text
ACB agent seat        -> Agent
lane                  -> Task / WorkItem
claim/fencing token   -> Activation lease / authority epoch
messages/inbox        -> Message + Event + Channel
event_seq             -> Event stream cursor
memory mirror         -> Memory/document graph
phase                 -> workflow/project state
Git SHA               -> Artifact + provenance
```

---

## 41. Development-Team Autonomy Target

The intended end state is a persistent autonomous development organization containing PMs, implementers, QA, specialists, temporary agents, one-shot agents, and human participants.

Agents should be able to:

- assign and accept work;
- communicate live;
- create detailed WorkOrders;
- subscribe to relevant state transitions;
- wake automatically when useful work arrives;
- become dormant when unnecessary;
- move between model providers;
- use tools;
- create artifacts;
- review one another;
- handle dependencies;
- escalate decisions;
- request human approval only where policy requires;
- query current authoritative state;
- query historical rationale;
- recover after crashes;
- continue across model/provider changes;
- remain deeply auditable.

The intended system is not merely "multiple LLM chats."

> **It is a durable, state-native, event-driven autonomous work system.**

---

## 42. Immediate Next Design Work

Before broad implementation begins, convert this architecture into concrete v0.1 interfaces, schemas, and invariants.

Priority design tasks:

- durable schema for Agent;
- Event;
- Subscription;
- WaitCondition;
- Activation;
- WorkOrder;
- Channel;
- Thread;
- Message;
- MemoryNode;
- Artifact;
- Decision;
- ToolExecution;
- ModelExecution;
- transaction boundaries;
- no-lost-wakeup algorithm;
- Activation claim/fencing protocol;
- stream ordering/cursor semantics;
- subscription matching/coalescing semantics;
- live mailbox safe points;
- document current/history/version behavior;
- provenance/audit record format;
- model adapter interface;
- tool adapter interface;
- ACB mirroring/migration boundary;
- single-node v0.1 vs cross-node responsibilities;
- recovery/failure test plan.

This architecture is **design input, not uncontrolled implementation authorization**.

Continue applying the SNC engineering rules:

> Always verify, never assume.  
> Fix confirmed defects on site.  
> Define semantics before implementation.  
> Make correctness structural.  
> Implement in bounded slices.  
> Test negative cases.  
> Independently verify before advancing.
