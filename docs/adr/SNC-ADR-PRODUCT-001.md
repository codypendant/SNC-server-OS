# SNC-ADR-PRODUCT-001: First product is a focused state-native runtime for persistent AI agents

- **Status:** PROPOSED, revision 2. Not accepted until cody says yes. No product pivot is accepted, and nothing here
  authorizes implementation, a proof of concept, a dependency, or an override of the scope freeze.
- **Proposed:** 2026-11-21, by Claude. Revision 1 came from Codex's review of PRs #181/#182
  (`docs/handoff/PR181_PR182_Codex_Review.md`). Revision 2 follows Codex's HOLD review of PR #183
  (`docs/handoff/PR183_Codex_Review.md`).
- **Analysis:** `docs/AGENT_RUNTIME_DECISION_PACKAGE.md` (revision 2). Revision 1 is kept as superseded research in
  `docs/AGENT_RUNTIME_DECISION_PACKAGE_REV1.md`.
- **Scope:** the product decision. Storage, durable execution, the durability class, the proof of concept and
  language are separate owner decisions (package section 8) that depend on this one.
- **Stages** (package section 0): S1 is the focused runtime; S2 is HA, scale and housekeeping verification; S3 is
  the enterprise governance expansion.

## 1. Proposed product: a focused agent runtime

SNC's proposed first product is a **focused state-native runtime for persistent AI agents**. Its foundational
capabilities (stage 1 in the package) are:

- persistent agent identity;
- durable tasks, events and recovery;
- authoritative agent memory;
- model-independent execution state;
- basic policy-controlled tool execution;
- lightweight decision provenance and causal history;
- agent lifecycle inspection and reconstruction.

Enterprise-first development is **not** assumed. The owner has questioned whether a full enterprise governance
platform is commercially justified, and this ADR does not decide that.

## 2. Optional enterprise governance expansion

These are a separate design candidate (stage 3 in the package). They are not part of the first release, and they
need their own decision once product validation (section 4) supports it:

- comprehensive decision-time evidence capture;
- per-tenant cryptographic anchoring;
- complex retention and erasure infrastructure;
- enterprise approval and delegated-authorization systems;
- detailed access auditing and compliance support;
- regulated multi-tenant deployments.

The research already written for them is kept and labelled by stage, not deleted.

## 3. Initial build-versus-reuse candidates

These candidates are **for the first experiment only**. None is an approved production dependency.

- **PostgreSQL** as the authoritative store.
- **DBOS Transact Python** for durable execution. It has transactional steps.
  - DBOS Rust 0.6 is pre-production.
  - Temporal remains an alternative.
- Storage and durable execution sit behind SNC interfaces, so these choices can be replaced.
- A custom journal is not authorized because a library fails one test. Configuration, an adapter, or another
  established implementation is examined first.

The proposed PostgreSQL durability class "committed" is **not** SNC's HA_DURABLE. It is accepted only after the
checks in package section 2.6 pass.

## 4. Product validation requirements

The vision calls Agent-SNC "a separate hypothesis with its own falsification gate"
(`docs/handoff/SNC_Future_Architecture_Vision_v2.txt` lines 10-11). That gate is kept, and it comes before any
enterprise expansion (package section 7.2):

- a working runtime demonstrated on this project's own multi-agent engineering environment;
- its value compared against DBOS, Temporal and existing agent frameworks;
- interviews with potential enterprise buyers about decision reconstruction and governance;
- evidence that SNC offers meaningful function beyond an SDK, an existing workflow engine, an observability system
  and a policy gateway.

Demand for agent auditing in general is not taken as demand for SNC.

## 5. Explicit owner decisions

The exact list is OD-P1 to OD-P8 in package section 8. Each needs cody's explicit yes. Two of them matter most here:

- Accepting this product direction (OD-P1) does not by itself override the scope freeze in
  `docs/EXPERIMENTAL_VALIDATION.md` section 3, which froze "the Agent Runtime". That override is a separate decision
  (OD-P2).
- Accepting it does not by itself conclude general-SNC falsification either.

## 6. Research retained but deferred

The following stay available for future specialized storage or execution needs:

- AFR-001 and its model evidence;
- L-1 and LiveGrant;
- D-02, R-23 and O-FLOOR;
- the raft-rs/OpenRaft build-versus-reuse research (#181);
- the per-core and NUMA architecture;
- the state-native object and scheduling work.

The custom replication work is not expanded while the product decision is open. `docs/SNC_GUARANTEES.md` and
HA_DURABLE are not revised. The AGENTS.md invariants keep governing the SNC core. Which of them carry over to the
runtime is package section 4.1. SNC-ADR-RUST-001 is not changed by this ADR. Whether it applies to the runtime is a
later decision (OD-P8).
