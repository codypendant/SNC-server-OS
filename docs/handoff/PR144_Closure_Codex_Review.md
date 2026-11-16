# PR #144 verification closure: Codex review (relayed by cody as a screenshot, 2026-11-16)

Received in three parts and transcribed from the screenshots. The review is complete.

> **PR #144 — Formal Verification Closure** — *Conditional approval*
>
> I've reviewed the verification-closure document, the TLA+ model changes, and the recorded TLC results.
>
> **Decision: Approve as provisional verification evidence, but do not declare formal verification complete or
> accept D-01/D-02/D-03 yet.**
>
> **What passed**
>
> - **Combined authority and durability model:** 15,151,183 states, exhaustive at MaxTerm 3.
> - **Operator promotion fencing gate:** 30,970,751 states, exhaustive for one fence and one return.
> - **R3 simplified lease rule:** Passed in completed Authority configurations.
> - **Log model:** Previous verification defect corrected; smaller configurations rerun successfully.
> - **Mutation testing:** Successfully exposed several safety violations, including lease-barrier, membership, and
>   reconciliation defects.
>
> Claude also disclosed that the original Log model contained a defect that invalidated part of its earlier
> verification coverage. Correcting and rerunning that model was essential.
>
> **Remaining concerns**
>
> 1. **The combined model is not yet a complete protocol model.** It excludes operator promotion, standby leases,
> and detailed storage recovery. Those interactions are checked separately, so their combined safety remains
> unestablished.
>
> 2. **Authority coverage remains incomplete.** No exhaustive run has completed with restarts, fencing, and all
> authority scenarios enabled together.
>
> 3. **The combined model's authority abstraction needs further scrutiny.** Its `Auth` predicate relies on lease
> state and local primary status without explicitly checking that the node's term matches the current group record.
> Other actions provide indirect fencing, but I want that relationship expressed as an invariant and challenged
> through mutation testing.
>
> 4. **The corrected Log model's larger configuration did not finish.** The completed [smaller] configurations provide
> bounded evidence, not unrestricted correctness.
>
> **My next instruction to Claude**
>
> I recommend one focused follow-up: **verify that the abstractions used to split Authority, Log, and Combined
> preserve the safety properties we intend to prove.** Prioritize cross-model invariants and counterexamples over
> simply increasing TLC state limits.
>
> PR #144 can merge as a documentation-and-model checkpoint, provided its status remains explicitly **PROPOSED /
> PARTIAL VERIFICATION**.
>
> **Do not authorize implementation from this PR.** We are making measurable progress toward architectural
> acceptance, but the remaining proof obligations must stay visible.

The word "[smaller]" is partly hidden in the screenshot. Before merge, `docs/AFR_001_VERIFICATION_CLOSURE.md` was
marked PROPOSED / PARTIAL VERIFICATION and given a list of remaining proof obligations (section 6).
