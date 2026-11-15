# SNC-ADR-RUST-001: Rust-Native Production Architecture

- **Status:** Accepted
- **Decision date:** November 15, 2026
- **Decided by:** cody, with Codex/ChatGPT (co-planner)
- **Recorded:** 2026-11-15 by Claude (Claude Code), at cody's request ("file this away in the repository").
  Transcribed from two screenshots cody posted in the project thread. The text above the decision card
  was cut off at the top of the first screenshot; only its visible fragment is kept below.

## Decision

> **Decision:** SNC's production implementation will be designed natively in Rust, using the existing C++
> system as an independently maintained behavioral reference.
>
> **Implementation strategy:** Parallel Rust development with differential testing, not source-to-source
> translation.
>
> **Timing:** Begin after the architecture stabilization and Rust readiness review.
>
> **Design principle:** Rust's ownership, type, and concurrency systems must inform architectural decisions
> from the beginning.
>
> **Compatibility:** Preserve validated external contracts and guarantees, not necessarily C++
> implementation details.
>
> **Verification:** The specification is authoritative. C++ and Rust discrepancies must be investigated
> independently.
>
> **Scope control:** Continue C++ development only where necessary for architectural validation,
> correctness fixes, or reference-system maintenance.

## Additional principle

> One additional principle I would establish is **correctness before performance, with performance-aware
> architecture.**
>
> We should design for NUMA locality, per-core execution, and low-overhead state access from the beginning,
> but we shouldn't sacrifice Rust's safety guarantees just to reproduce a particular C++ optimization. Where
> `unsafe` is justified, it should have a documented invariant and targeted verification.

## Context (visible fragment)

The discussion above the decision card was cut off in the screenshot. Its last visible lines read:

> ... [Rust does not] automatically guarantee distributed correctness, crash consistency, or fencing safety.
> Those still require explicit protocols, proofs where practical, and rigorous testing. But we can [use
> R]ust to enforce more of our local invariants structurally.

(Words in brackets are partly hidden in the screenshot and are reconstructed; the rest is verbatim.)

## Recording note from the same message

> I also want to distinguish between accepting this decision in our engineering discussions and recording
> it in the repository. The latter still needs to happen.
>
> At the next suitable handoff to Project Claude, I recommend adding this ADR to the repository without
> interrupting the current control-plane work. That gives the entire agent team an authoritative record of
> our intended direction and prevents continued C++ development from becoming the default long-term
> strategy.
>
> This is an important decision for SNC. We're defining the production architecture around the system we
> actually want to build, rather than allowing the experimental implementation to dictate its future.

## What this record does and does not change

- It sets the intended production direction: Rust-native, with the C++ system as the behavioral reference.
- It authorizes no Rust implementation now. By its own Timing clause, Rust work begins only after the
  architecture stabilization and a Rust readiness review, neither of which has happened. Earlier
  instructions that Rust work stays out of scope until explicitly authorized (`AGENTS.md`, `docs/STATUS.md`
  scope freeze) remain in force until then.
- Current C++ work continues under its Scope control clause: architectural validation, correctness fixes
  and reference-system maintenance. The control-plane connection slices (`docs/CONTROL_CONNECTIONS_DESIGN.md`)
  are architectural validation and correctness work on the reference system.
