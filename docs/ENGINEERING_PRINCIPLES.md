# SNC engineering principles (standing)

Standing principles set by the owner (cody). They apply to every agent working on SNC. Text is recorded verbatim.

## Research Before Architectural Reinvention (cody, 2026-11-16 01:29 UTC)

SNC Engineering Principle — Research Before Architectural Reinvention

Effective immediately, incorporate external technical research into SNC's architecture and engineering workflow.

Whenever a significant architectural problem, correctness defect, scalability limitation, or expensive-to-reverse decision is encountered:

1. Define the underlying problem, affected invariants, and failure conditions.
2. Search existing research, production systems, open-source projects, and documented engineering failures for comparable problems.
3. Prioritize peer-reviewed research, original technical papers, official design documents, source code, and production incident analyses.
4. Identify multiple viable approaches where available. Record the assumptions, guarantees, limitations, and tradeoffs of each.
5. Evaluate compatibility with SNC's state-native object model, ownership architecture, execution semantics, durability requirements, and planned Rust-native implementation.
6. Recommend an SNC-specific architecture based on the strongest evidence. Do not mechanically copy another system's implementation or adopt its assumptions without verification.
7. Define the invariants, fault-injection scenarios, differential tests, and performance experiments necessary to validate the proposed design.
8. Record the research sources and rationale in the relevant design document or ADR.

Research must inform architectural decisions, not substitute for engineering proof.

Distinguish established facts, external-system guarantees, SNC-specific inferences, and unverified hypotheses.

Do not introduce external dependencies, adopt source code, or expand implementation scope without authorization. Respect applicable licenses.

Apply this principle proportionally: a localized, well-understood implementation defect does not require a literature survey. Foundational architecture decisions and unresolved correctness problems do.

The objective is to build SNC correctly from the beginning by learning from established engineering work while maintaining SNC's independent architecture and state-native design philosophy.

Always verify, never assume.
