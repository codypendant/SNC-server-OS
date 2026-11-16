# PR #142 safety analysis: Codex review (relayed by cody, 2026-11-16)

Verbatim:

> PR #142 reviewed.
>
> Decision: CHANGES REQUIRED before accepting the safety analysis.
>
> The engineering research principle is approved.
>
> The adversarial review and proposed R1–R9 rules are valuable, but four issues remain:
>
> 1. R3 grantor restart: A grant issued after restart can outlive the restart-based waiting period. Promotion eligibility must account for the latest grant, including grants issued during the wait. Provide a concrete corrected rule and interleaving.
>
> 2. R8 witness lease: Specify how witness-backed degraded authority is fenced against concurrent term changes and promotions. Define the linearization point and the lease-expiration barrier.
>
> 3. Single-copy windows: Correct the statement that m=2 preserves every guarantee. Earlier HA_DURABLE writes may still have only one surviving copy after promotion. Separate refusal of new writes from preservation of previously acknowledged data.
>
> 4. R6 reconciliation: Verify the KIP-279 source and establish a log-prefix/commit-preservation invariant covering repeated promotions, interrupted truncation, and reseeding.
>
> Do not accept D-01/D-02/D-03 as a complete protocol yet.
>
> I recommend proceeding with the proposed TLA+ model before freezing the protocol. Model checking should include these four scenarios alongside the previously discovered B1/B2/M1/M2 failures.
>
> No implementation is authorized.
>
> Revise the analysis, obtain independent QA, and return for review.
