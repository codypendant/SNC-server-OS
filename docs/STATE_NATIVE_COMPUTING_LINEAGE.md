# SNC State-Native Computing: Architecture Lineage, Prior Art, and the Minimal Durable State-Machine Primitive

**Status:** Research / architecture exploration  
**Date:** 2026-11-07  
**Scope:** SNC Server OS and future runtimes built on SNC  
**Purpose:** Record the architectural lineage behind SNC's emerging state-native model, distinguish it from adjacent systems, and frame the next core design question.

---

## 1. Executive summary

SNC began from a database-native operating-system idea, but the architecture is increasingly better described in a different way:

> **SNC is a state-native operating environment for durable computational objects.**

A more conceptual phrasing is:

> **An operating system for evolving state machines.**

And the most rigorous formulation currently available is:

> **A runtime/OS substrate in which persistent application state is the primary unit of identity, placement, recovery, and computation.**

This framing is not without precedent. Persistent operating systems, orthogonally persistent systems, virtual actors, durable execution engines, stateful serverless platforms, and modern agent runtimes have all explored pieces of the same design space.

The research does **not** support a claim that SNC is the first system to combine persistence and computation, the first durable state-machine runtime, or the first system to provide replay or branching. Those ideas have substantial prior art.

What remains unusual is the intended combination:

1. durable state is primary rather than an external backing store;
2. execution is modeled as transitions over durable objects;
3. object identity, ownership, locality, scheduling, persistence, recovery, and replication are managed by one substrate;
4. dormant objects need no resident process;
5. history, provenance, replay, and eventually branching can attach to state itself;
6. vertical runtimes such as agents, MMO backends, workflows, and control planes can share the same primitive.

The most important question coming out of this research is therefore:

> **What is the minimal state-machine primitive SNC needs such that Agent-SNC, MMO workloads, durable workflows, and ordinary backend services all fall out naturally without contaminating SNC Core with vertical-specific features?**

This document records the relevant prior art, the likely differentiators, and a proposed way to reason about that primitive.

---

## 2. The conceptual shift: from database OS to state-native OS

The original database-native framing remains useful because it identifies a problem with the conventional stack:

```text
application process
    |
runtime / framework
    |
database client / ORM
    |
database
    |
filesystem / block layer
    |
kernel
```

Application state is represented multiple times: live process memory, serialized records, database rows/documents, WAL/log entries, cache state, recovery metadata, and orchestration/workflow state. Each layer invents its own identity, lifecycle, consistency, and recovery model.

SNC is moving toward a more direct abstraction:

```text
durable object / state machine
    |
scheduled transition
    |
new durable state
```

The important change is not merely that data persists. The change is that **durable state becomes the thing the system schedules and manages**.

In a conventional OS, the fundamental abstraction is approximately:

> process + address space + files

In a state-native SNC model, the fundamental abstraction could become:

> durable object + transition + ownership + history

### 2.1 Dormancy becomes normal

A durable state machine can exist indefinitely without consuming a CPU thread or resident process. An MMO player offline for six months, an agent waiting for approval, a workflow sleeping until next quarter, or a control-plane object waiting for an event can all remain durable while consuming no active compute.

The state remains. Execution appears only when a transition is required.

### 2.2 Scheduling and state placement become one problem

SNC already uses owner-per-core execution. This suggests a stronger principle:

> **Place computation where authoritative state is owned.**

That connects CPU scheduling, NUMA locality, object placement, shard ownership, replication, migration, and resource isolation.

### 2.3 Recovery moves below application code

If state transitions are native to the substrate, application code should not have to rebuild its own retry ledger, replay engine, idempotency store, checkpoint table, workflow journal, lease state, or durable timer store. Those can become platform properties.

---

## 3. Historical lineage: orthogonally persistent operating systems

The oldest important lineage is **orthogonal persistence**: object lifetime should be independent of type and access mechanism, and programmers should not have to flatten live object graphs into files or relations merely to make them survive.

### 3.1 Grasshopper

The 1994 Grasshopper operating system is especially relevant. Grasshopper argued that implementing orthogonal persistence above a conventional OS repeatedly forced researchers to build a second abstract machine above the OS. Its answer was to design the operating system itself around persistence.

Grasshopper centered three abstractions:

- **containers** — storage;
- **loci** — agents of change;
- **capabilities** — access and protection.

The SNC analogue is not exact, but the lesson is important:

> If persistence is fundamental, forcing it through a conventional volatile-process abstraction may create an unnecessary second machine.

### 3.2 EROS / CapROS / KeyKOS lineage

The capability-OS lineage explored persistent object spaces, authority, checkpointing, and recovery as system-level concerns. SNC should study this work for persistent identity, authority, crash recovery boundaries, checkpoint publication, and capability-based access to durable state.

### 3.3 Why SNC is still different

Orthogonally persistent systems generally focused on making ordinary computation persistent. SNC's direction is more explicit: durable objects are not merely memory that survives; they are the native units whose transitions are scheduled. That distinction becomes larger once owner-per-core locality, distributed replication, migration, timers, branching, and provenance enter the model.

---

## 4. Commercial evidence: IBM i and persistent object systems

IBM i / AS/400 matters because it demonstrates that persistent, object-centric system abstractions can survive commercially for decades. The lesson is not that SNC should emulate IBM i; it is that the Unix split between process memory, files, and database records is not the only viable machine model.

A state-native system can be commercially practical if object identity is stable, persistence is predictable, operations are disciplined, and lifecycle tooling is excellent.

---

## 5. Modern OS research: Aurora and persistent execution

Aurora (SOSP 2021) explores transparent persistence of entire applications. Its philosophical move is to reduce the distinction between volatile application state and persistent state by continuously checkpointing execution.

Aurora-style model:

```text
ordinary process
    |
system persistently preserves process execution
```

SNC direction:

```text
durable object/state
    |
system schedules explicit transitions over that state
```

Aurora remains highly relevant to checkpoint engineering, crash consistency, and the cost of transparent persistence.

---

## 6. Persistent objects: Twizzler

Twizzler is a data-centric OS design in which persistent objects and persistent references are native abstractions. It overlaps directly with stable durable identity, cross-object references, sharing, long-lived namespaces, and protection.

The difference is computational: Twizzler is primarily an object/data-centric OS; SNC is heading toward an object **plus transition scheduler** model.

The useful lesson is the separation between **object identity** and **whichever process happens to be touching that object right now**.

---

## 7. The explicit state-machine framing

Modern OS research has explicitly used a state-machine interpretation: an application can be modeled as a state machine while the OS coordinates scheduling, interrupts, snapshots, restart, and recovery around it.

That validates the phrase:

> **Operating system for evolving state machines.**

The stronger SNC move would be to expose durable state transitions as an explicit application-state abstraction rather than merely noting that processes are mathematically state machines.

---

## 8. DBOS and database-centered operating-system designs

DBOS is the obvious neighbor to SNC's original database-OS framing. DBOS makes database abstractions central to OS/runtime state and durable execution.

DBOS:

```text
computation
    |
database-backed durable state
```

SNC direction:

```text
durable state
    |
scheduled computation / transition
```

In SNC, owner, locality, recovery stream, migration state, and execution scheduling could all be properties of the same durable object. That places the state abstraction below the application runtime instead of merely providing a database underneath it.

---

## 9. Durable execution: Temporal

Temporal's core model is event-history replay: deterministic workflow code, nondeterministic activities, persisted history, and recovery by replay.

Important lessons for SNC include durable timers, explicit nondeterminism boundaries, event histories, idempotent external effects, and replay-based debugging.

But Temporal's primary abstraction is a **workflow execution**. SNC could make the state object itself primary, which matters for huge populations of dormant entities whose computation is sporadic rather than continuously workflow-shaped.

---

## 10. Durable execution beyond workflows: Restate

Restate is one of the closest current systems to the runtime side of SNC. It provides durable execution, Virtual Objects, keyed state, single-writer-per-key semantics, reliable RPC, queues, timers, signals, flow control, and agent support.

Restate validates several SNC assumptions:

1. stateful keyed entities are useful;
2. single-writer ownership simplifies concurrency;
3. durable RPC and timers belong near the state model;
4. workflows and actor-like objects can share one substrate;
5. agents benefit from the same durable primitives;
6. flow control belongs in the runtime.

The SNC differentiation cannot simply be "durable objects"—Restate already has those. A stronger distinction is:

> **ownership, placement, transition execution, durability, recovery, and potentially version history are one native object model.**

---

## 11. Virtual actors: Microsoft Orleans

Orleans implements the virtual actor model. A grain has stable identity, behavior, isolated mutable state, runtime-managed activation and placement, and optional durable persistence.

The key lesson is that developers benefit when they address a logical entity without manually managing which host owns it, whether it is resident, or how it is activated.

Possible distinction:

**Orleans grain:** stable identity + activation + behavior + optional persistence.  
**SNC durable object:** stable identity + ownership + durability + recovery + transition scheduling + locality.

---

## 12. Cloudflare Durable Objects

Cloudflare Durable Objects combine globally unique identity, single-threaded execution, colocated durable storage, runtime-managed lifecycle, placement, migration, and hibernation.

This strongly validates the intuition:

> **state should have identity, and computation should happen where that state lives.**

Important lessons for SNC: dormant state should not consume compute, lifecycle should be transparent, compute/storage locality matters, and object identity can be more useful than process identity.

The SNC distinction would be deeper integration: instead of durable storage attached to an object, the object state itself can become the durable machine state.

---

## 13. Stateful serverless: Cloudburst and related systems

Cloudburst's principle of **logical disaggregation with physical colocation** maps closely to SNC's owner/locality direction: applications should not manually manage location, while the system should physically colocate state and compute for performance.

Stateful serverless research also illustrates how difficult it is to retrofit transactions, durability, fault tolerance, and migration onto systems originally designed around stateless functions. That supports putting these semantics lower in the architecture.

---

## 14. Netherite and Durable Functions

Microsoft's Netherite work is useful implementation prior art for partition ownership, durable logs, asynchronous snapshots, high-throughput persistence, state movement, and recovery.

Even where the programming model differs, it addresses many of the same engineering problems SNC is already encountering: checkpointing without stopping foreground work, replay, ownership, and state movement.

---

## 15. Smalltalk images: the persistent object-world intuition

Smalltalk images provide a useful old analogy. Instead of a program that starts, reads data, computes, writes, and exits, the environment feels like a long-lived object world that continues evolving.

SNC can extend that intuition with distributed ownership, replication, NUMA locality, fault isolation, HA, migration, and horizontal partitioning.

---

## 16. Agent runtimes: durable state machines are becoming mainstream

Long-running agents need durable execution, tool-call idempotency, timers, approvals, event waiting, replay, checkpoints, memory, human-in-the-loop pauses, and model replacement.

This supports a central Agent-SNC principle:

> **The model is an execution resource; the durable state is the agent.**

Agent identity should not depend on one particular model session.

---

## 17. Division Swarm and agent state machines

Division Swarm overlaps significantly with Agent-SNC semantics: durable state machines, persistent entities, deterministic transitions, replay, crash recovery, model independence, agent workspaces, and run forking.

That means Agent-SNC is not unique merely because it uses durable agent state machines.

The possible SNC distinction is architectural depth: those semantics can emerge from the same native durable-state substrate used by ordinary backend, MMO, workflow, and control-plane workloads.

---

## 18. State branching / StateFork / Waypoint direction

Recent work on agent execution increasingly treats branchable execution state as a first-class capability: restore an intermediate system state and explore multiple futures.

Branching itself is therefore not enough to differentiate SNC.

The stronger SNC version would be:

> **branch the native durable state graph rather than snapshotting an external collection of processes, files, shells, and databases.**

Potential semantics:

```text
namespace @ version N
        |
        +---- branch A
        |
        +---- branch B
```

Applications include agent plan exploration, game simulation, migration testing, schema/state transformation, incident reproduction, policy testing, and AI-generated change review.

Branching should be considered a generic state primitive, not an agent-only feature.

---

## 19. Replay, history, and provenance

Replay is common in durable execution and should not be treated as novel by itself.

The more interesting SNC direction is to make history structural:

```text
object/version
    |
transition metadata
    |
new object/version
```

Potential history metadata includes previous version, transition type, causal parent, principal/model, input/event, tool result, provenance, policy decision, code version, owner, and durability epoch.

This can support debugging, audit, causal explanation, rollback, selective branch, security forensics, and agent-memory provenance.

History policy should be configurable; SNC Core should not assume infinite history for every object.

---

## 20. The state-machine interpretation

A clean abstract model is:

```text
(State, Event, Policy, CodeVersion)
        |
        v
   Transition
        |
        v
(NewState, Effects, NewEvents)
```

The substrate is responsible for making transitions isolated, owned, durable where required, observable, resource-bounded, and recoverable. External effects require an explicit durable boundary.

---

## 21. A possible SNC machine model

A future SNC state-machine object could contain:

### Identity
- ObjectID
- type
- namespace
- tenant / authority domain

### State
- durable version
- payload
- metadata
- owner/shard
- placement policy

### Transition interface
- operation ID
- validated input
- execution function
- resource class
- side-effect declaration

### Durability
- WAL position
- durability status
- replication status
- checkpoint membership

### Time
- durable timers
- deadline/expiration
- wake conditions

### Events
- subscriptions
- signals
- durable messages
- wait conditions

### History policy
- none
- WAL only
- bounded version chain
- full provenance
- branchable

### Security
- capability / role / policy
- operation authorization
- namespace isolation

The design constraint is:

> **SNC Core should expose only what is required by multiple workloads.**

Agent concepts such as memory, plans, and tool calls stay in Agent-SNC. MMO concepts such as players and zones stay in an MMO runtime.

---

## 22. Mapping vertical workloads onto the same primitive

### Agent-SNC
Objects: agent, task, project, memory document, approval, timer, tool invocation.  
Transitions: accept task, record fact, revise plan, wait, wake, delegate, commit artifact.

### MMO runtime
Objects: player, character, guild, zone, market listing, raid, creature, inventory.  
Transitions: move, trade, equip, craft, join, attack, spawn, expire.

### Durable workflows
Objects: workflow instance, job, approval, timer, external-call state.  
Transitions: start, complete step, retry, pause, signal, cancel, compensate.

### Ordinary backend services
Objects: account, order, subscription, cart, session, device, entitlement.  
Transitions: create, update, approve, charge, cancel, renew.

### Network/control plane
Objects: configuration, virtual service, pool, policy, route, peer, tunnel, health state.  
Transitions: apply, withdraw, fail over, reconcile, promote, degrade.

The shared pattern is:

> **stable durable identity + serialized ownership + validated transition + durability + events/time.**

---

## 23. Candidate minimal state-machine primitive

The minimal SNC primitive should probably **not** begin with branching, provenance, graph memory, AI, workflow DSLs, or arbitrary distributed transactions.

A plausible minimum is:

### Primitive 1: durable identity
A globally or namespace-stable ObjectID.

### Primitive 2: authoritative owner
At any moment, exactly one execution owner for a mutable object/shard domain.

### Primitive 3: atomic transition
An operation receives object identity, current version/state, input/event, and execution context, and produces a new state/version plus zero or more durable messages/effects.

### Primitive 4: durable event delivery
Transitions can emit messages/events that survive failure.

### Primitive 5: durable waiting
An object can become dormant until a timer, event, signal, or condition without consuming compute.

### Primitive 6: recovery identity
After restart or migration, the same object resumes from the same durable identity and authoritative state.

### Primitive 7: explicit resource bounds
Every transition class is bounded in memory, queueing, durability backlog, execution quantum, and fan-out.

Everything else can be layered.

---

## 24. What should remain outside the minimal primitive

Do not immediately put these into SNC Core:

- LLM/model concepts;
- agent memory types;
- task-plan semantics;
- MMO player/zone semantics;
- SQL query language;
- arbitrary distributed transactions;
- full branch/merge semantics;
- full provenance graph;
- universal event sourcing;
- long-term version retention.

The minimal core should remain small enough to reason about formally.

---

## 25. Future primitive: state branch / fork

A generic branch primitive might eventually look like:

```text
BranchID fork(NamespaceID, VersionPoint)
```

Open questions include branch consistency point, copy-on-write semantics, branch-local WAL, effect isolation, timer behavior, message behavior, merge/promotion, security, and garbage collection.

The hardest issue is external effects. A branch must not silently send real email, charge cards, alter production infrastructure, or mutate external databases. Effects need a policy: blocked, simulated, redirected, or explicitly authorized.

---

## 26. Future primitive: selective promotion

Selective promotion could support:

1. fork production state;
2. execute migration or agent plan;
3. validate invariants;
4. compute a delta;
5. approve;
6. promote allowed changes.

This is valuable for enterprise change control, agent sandboxing, game-event preparation, infrastructure policy testing, and migrations.

Promotion is much harder than fork because authoritative state can diverge concurrently, so it should be a separate research problem.

---

## 27. Future primitive: deterministic replay

Replay should capture the minimum nondeterministic boundary: transition input, current state/version, logical time, random seed, external-call results, and code version.

But replay can be a policy rather than universal overhead:

- none;
- transition-level;
- workflow-level;
- full forensic.

---

## 28. Security implications

A state-native system changes the valuable security object: the durable state machine itself.

Eventually security must answer who may invoke transitions, read current state, inspect history, fork, promote, alter retention, migrate ownership, and authorize effects.

Capability systems such as Grasshopper/EROS/CapROS are useful prior art for this problem.

---

## 29. Commercial significance

The state-native model is a clearer product story than "database OS":

> **Build long-lived stateful applications without rebuilding persistence, recovery, scheduling, timers, locality, and lifecycle management for every service.**

Potential layering:

### SNC Core
- durable objects
- ownership
- transitions
- persistence
- replication
- migration
- timers/events
- resource bounds

### Agent-SNC
- agents
- memory/provenance
- approvals
- durable tools
- branching
- model independence

### MMO runtime
- entities
- zones
- persistence
- timers
- inventory/economy
- migration
- massive dormant population

### Workflow runtime
- jobs
- timers
- signals
- retries
- compensation

This keeps SNC Core reusable instead of vertical-specific.

---

## 30. What is not unique

SNC should not claim novelty merely for persistent objects, orthogonal persistence, durable workflows, actors, keyed state, single-writer entities, replay, durable timers, stateful serverless, or branchable agent execution.

The stronger thesis is the architectural composition:

> **A low-level state-native substrate where durable object identity, ownership, locality, scheduling, persistence, recovery, and transition execution are unified.**

Whether that is valuable remains an empirical question and should be falsified rather than assumed.

---

## 31. Research hypotheses

### H1 — state-native programming reduces infrastructure duplication
Applications built directly on SNC require less custom persistence/recovery/timer/idempotency machinery than comparable conventional services.

### H2 — ownership-local execution improves predictable latency
Owner-per-core execution plus state locality provides better tail behavior for stateful workloads than stateless workers repeatedly reaching remote persistence.

### H3 — dormancy is a density advantage
Large populations of long-lived entities can exist durably without equivalent populations of active processes/tasks.

### H4 — one substrate can support multiple vertical runtimes
Agent, MMO, workflow, and backend workloads can share the same core primitive without vertical semantics leaking into SNC Core.

### H5 — native history/branching is simpler than external snapshot orchestration
If implemented later, branching native state should be operationally simpler and more coherent than snapshotting a multi-service conventional stack.

These are hypotheses, not conclusions.

---

## 32. Immediate architecture question: the minimum durable state-machine contract

A useful first draft:

### DSM-1 Identity
Every durable machine has stable identity.

### DSM-2 Ownership
Mutable transition execution has one authoritative owner at a time.

### DSM-3 Versioned transition
Every accepted mutation advances a version and has a durable ordering point.

### DSM-4 Atomic local visibility
A transition is not partially visible.

### DSM-5 Durable causality
Any emitted durable follow-up/event is causally tied to the transition that created it.

### DSM-6 Dormancy
A machine can exist with zero active execution resources.

### DSM-7 Wake
Timers/events can activate the machine.

### DSM-8 Recovery
After failure, the machine recovers to a state consistent with its acknowledged durable transition prefix.

### DSM-9 Migration
Ownership can move without changing identity.

### DSM-10 Resource bounds
Admission and execution are bounded by explicit resources.

This is deliberately smaller than an agent runtime or workflow engine.

---

## 33. How current SNC already maps to this

Current SNC work already supplies pieces of the proposed substrate:

- durable ObjectID identity;
- shard ownership;
- owner routing;
- per-shard WAL;
- acknowledged durability semantics;
- checkpoint/recovery;
- replication;
- migration;
- failover;
- backpressure;
- explicit refusal;
- resource observability;
- differential/falsification testing.

The checkpoint/backpressure work is directly foundational. A state-native system fails if background durability machinery can unpredictably destroy foreground service. Durable state machines need bounded transition admission, bounded durability backlog, bounded checkpoint interference, and predictable progress.

---

## 34. Engineering philosophy carried forward

### Always verify, never assume
Architecture claims require code inspection, tests, physical evidence, and reproducible measurement.

### Falsification before feature growth
Do not build state branching simply because it sounds compelling. Prove the core substrate first.

### Correctness contracts before optimization
The checkpoint CC-1..CC-5 approach should be repeated for future state-machine primitives.

### Explicit resource bounds
No unbounded queue should hide inside a "durable" abstraction.

### Differential testing
Preserve behavioral reference points wherever possible.

### Fix on site
Do not accumulate known correctness, harness, or measurement ambiguity.

---

## 35. Recommended next research artifacts

1. **Minimal Durable State-Machine Contract** — formal DSM-1..DSM-10 architecture.
2. **State Branching Design** — fork semantics, effect isolation, promotion, GC.
3. **Durable Event/Timer Model** — guarantees for wait, wake, timer, cancellation, signals.
4. **State History / Provenance Policy** — what belongs in Core vs Agent-SNC.
5. **Object Security / Authority Model** — capabilities, roles, namespaces, tenant isolation.
6. **Vertical Mapping** — concrete mappings for Agent-SNC, MMO, workflow, backend, control plane.

---

## 36. Recommended terminology

### Preferred technical description

> **SNC is a state-native execution substrate in which durable application objects are the primary units of identity, ownership, recovery, and computation.**

### Preferred conceptual description

> **An operating system for evolving state machines.**

### Preferred short phrase

> **State-native computing.**

### Terms to use carefully

- **Database OS** — historically useful, now too narrow.
- **Persistent process OS** — too process-centric.
- **Actor runtime** — meaningful overlap, but SNC aims lower and persistence-native.
- **Workflow engine** — too vertical.
- **Agent OS** — appropriate for Agent-SNC, not SNC Core.

---

## 37. Bottom line

The SNC direction has substantial intellectual ancestry, which is useful because it lets the project learn from decades of persistent-system, actor, workflow, and stateful-runtime research.

The research also narrows the novelty claim. Persistence is not new. Actors are not new. Replay is not new. Branching is not new.

The stronger SNC thesis is:

> **Durable state itself can be the primary computational substrate, with identity, locality, ownership, transition scheduling, persistence, recovery, and resource control unified below vertical runtimes.**

If that thesis holds, Agent-SNC, MMO servers, durable workflows, backend services, and control planes are not separate architectures. They are different applications of the same machine model.

The immediate architectural question is:

> **What is the smallest, rigorously specified durable state-machine primitive that can support all of those workloads without dragging any one vertical's semantics into SNC Core?**

That question should guide the next conceptual phase of SNC.

---

## References / systems to study

### Persistent operating systems / object systems

- Grasshopper: Alan Dearle et al., *Grasshopper: An Orthogonally Persistent Operating System* (1994)  
  https://research-portal.st-andrews.ac.uk/en/publications/grasshopper-an-orthogonally-persistent-operating-system/
- CapROS  
  https://www.capros.org/
- Aurora, SOSP 2021  
  https://rcs.uwaterloo.ca/~ali/papers/sosp21-aurora.pdf
- Twizzler  
  https://www.ssrc.us/pub/bittman-tos21.html
- IBM i documentation  
  https://www.ibm.com/docs/en/i/

### Durable execution / stateful runtimes

- Temporal  
  https://docs.temporal.io/
- Restate  
  https://restate.dev/
- Restate examples / Virtual Objects / state machines  
  https://github.com/restatedev/examples
- Microsoft Orleans  
  https://dotnet.github.io/orleans/docs/overview/
- Cloudflare Durable Objects  
  https://developers.cloudflare.com/durable-objects/concepts/what-are-durable-objects/
- Cloudburst  
  https://rise.cs.berkeley.edu/projects/cloudburst/
- Netherite / Durable Functions  
  https://www.microsoft.com/en-us/research/publication/serverless-workflows-with-durable-functions-and-netherite/

### Database / OS convergence

- DBOS  
  https://dbos.dev/
- Stanford MAST DBOS research lineage  
  https://mast.stanford.edu/

### Agent / branchable-state directions

- Division Swarm  
  https://github.com/division-sh/swarm
- StateFork / Waypoint: track as part of the branchable-agent execution research landscape.

---

## Research note

This document is an architecture-lineage and hypothesis document, not a claim of proven uniqueness.

Future external positioning should distinguish:

- established prior art;
- SNC implementation already demonstrated;
- SNC architecture under active development;
- speculative future primitives.

The project should continue to prefer falsifiable technical claims over novelty rhetoric.
