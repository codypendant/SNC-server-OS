# PR #130 review: persistent control-plane connections design (cody, 2026-11-15 16:30 UTC)

Posted by cody in the project thread as text; transcribed verbatim below (headings kept, bullets kept).

> **SNC Server OS — PR #130 Review and Required Design Corrections**
>
> PR #130 is approved in principle as a documentation-only design. Implementation authorization is on hold
> pending two corrections.
>
> The persistent control-plane connection design is technically sound in its overall direction. The
> independent QA process identified important compatibility and correctness issues, and revision 2 addresses
> most of them appropriately.
>
> However, two architectural concerns must be resolved before implementation.
>
> **1. Preserve Existing HEALTH Semantics**
>
> The proposed five-second maximum lifetime for persistent HEALTH connections changes the failure-detection
> contract.
>
> Currently, every HEALTH request opens a new connection. This exercises the server's accept path and can
> expose descriptor exhaustion or a stalled accept loop.
>
> A persistent connection could continue answering HEALTH while the server is unable to accept new
> connections.
>
> Decision: Keep HEALTH requests one-shot initially.
>
> Requirements:
>
> - Preserve existing witness liveness detection behavior.
> - Preserve the current failover timing guarantees.
> - Do not introduce an additional five-second detection window.
> - Keep HEALTH on its existing dedicated fast path.
> - Do not allow connection pooling to delay witness decisions.
> - Establish regression coverage for stalled accept paths, descriptor exhaustion, SIGSTOP, SIGKILL, and
>   network partitions.
>
> Persistent HEALTH connections may be reconsidered later through a separate evidence-based proposal.
>
> **2. Correct the Late-Delivery Guarantee**
>
> The current A3 acceptance criterion overstates what TCP can guarantee.
>
> "TCP_USER_TIMEOUT" and RST can terminate a connection, but they cannot guarantee that a timed-out request
> will never execute if the peer has already received it.
>
> Revise the design to explicitly recognize ambiguous request outcomes.
>
> Requirements:
>
> - A timeout must not be interpreted as proof that the request was never executed.
> - Preserve existing term, epoch, ballot, and fencing validation.
> - Never automatically retry non-idempotent operations.
> - Keep retries restricted to explicitly classified safe operations.
> - Preserve sequence-number validation to prevent stale replies from being associated with subsequent
>   requests.
> - Test late replies, half-open connections, partitions, reconnections, and requests that execute before
>   their replies are lost.
>
> Acceptance criteria must describe guarantees the implementation can actually establish.
>
> **3. Preserve the Implementation Strategy**
>
> The overall staged implementation approach remains appropriate:
>
> K0: Reduce "test_migrate" probe connection churn.
>
> K1: Introduce negotiated persistent connections, request sequencing, bounded pools, and server-side
> connection lifecycle management.
>
> K2: Convert eligible production control-plane callers incrementally, preserving existing safety and
> liveness semantics. HEALTH remains one-shot.
>
> K3: Conduct hosted measurements and verify connection reduction, latency, resource consumption, and
> correctness.
>
> Each implementation slice requires its own PR and independent verification.
>
> Do not combine these into one large implementation.
>
> **4. Required Actions**
>
> 1. Update "docs/CONTROL_CONNECTIONS_DESIGN.md" with the two corrections.
> 2. Update the affected acceptance criteria, especially A2, A3, and A8.
> 3. Verify that the changes remain consistent with the existing witness, Paxos, migration, and fencing
>    contracts.
> 4. Obtain independent QA verification of the revised design.
> 5. Commit the documentation corrections to PR #130.
> 6. Post a concise summary of the changes and QA conclusions.
>
> Do not modify production code in PR #130.
>
> Do not begin K0, K1, K2, or K3 yet.
>
> Stop after updating PR #130 and notify me for final review.
>
> The objective is to reduce control-plane connection overhead without weakening SNC's distributed
> correctness, failure detection, or fencing guarantees.
>
> Always verify, never assume. Preserve system-wide architectural consistency.

Applied in: `docs/CONTROL_CONNECTIONS_DESIGN.md` revision 3 (section 9).
