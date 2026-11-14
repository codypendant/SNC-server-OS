<!-- cody's review of PR #125, posted in the project thread 2026-11-14 05:33 UTC; filed verbatim. -->

SNC Server OS — PR #125 Approved / F1 Authorized

PR #125 is approved for merge.

The diagnostic findings are accepted as the baseline for the next engineering phase. Preserve the distinction between confirmed defects, suspected root causes, and unresolved observations.

Proceed with merging PR #125.

Next Assignment: F1 — Primary Liveness Failure

Your next task is to investigate and correct the q10 primary liveness failure.

Confirmed behavior

- q10 failed 3 of 12 runs on GitHub AMD runners.
- Intel runners passed 12/12; local runs passed 38/38.
- The standby repeatedly missed its 300 ms acknowledgment deadline.
- The primary disconnected from its standby but failed to obtain witness authorization to continue independently.
- Writes remained blocked for approximately 95 seconds in one observed run.
- The checkpoint loop is a suspected contributor, but the causal mechanism has not been established.

Engineering requirements

1. Establish the root cause before implementing a fix.

Trace the complete failure path, including standby acknowledgment timeouts, connection state transitions, checkpoint execution, witness authorization, and primary write availability.

Determine whether checkpoint execution can block the control-plane progress required for failover.

Do not assume the checkpoint hypothesis is correct.

2. Implement the smallest architecturally correct fix.

The correction must preserve:

- Replication safety
- Witness authorization and fencing guarantees
- Durable acknowledgment semantics
- State consistency
- Correct failover and recovery behavior

Never sacrifice safety to improve availability.

Do not simply increase timeouts or suppress failures to make tests pass.

3. Establish regression coverage.

Create targeted tests that reproduce the original failure mechanism and verify the correction.

Test both normal operation and failure conditions.

Include AMD and Intel execution environments where practical.

The regression must detect the underlying defect rather than merely depend on favorable timing.

4. Independent verification.

Follow our established three-way verification process.

Require independent QA to examine the root-cause evidence, implementation, and regression results.

Personally verify significant QA findings before requesting merge approval.

Scope boundaries

Keep the following issues separate:

- F2: Correct the load checker's treatment of unanswered or in-doubt transfers. Do not assume the account balances were corrupted.
- F3: Investigate and correct the suspected X16 test timing issue.
- F4: Continue diagnosis of X3 and X26, including the cross-node reconnection failure and unanswered requests.

Do not combine F2, F3, or F4 into the F1 implementation.

Do not weaken test assertions, conceal failures, or introduce arbitrary timeout increases.

Deliverables

Open the next PR containing:

1. Root-cause analysis with source and runtime evidence.
2. Minimal corrective implementation.
3. Targeted regression tests.
4. Verification results, including failure-injection coverage.
5. Independent QA findings.
6. Assessment of downstream architectural consequences.

If the root cause cannot be established confidently, stop at a documented diagnosis rather than implementing a speculative fix.

Stop after opening the PR. Do not merge or begin F2 until I have reviewed the results.

Always verify, never assume. Fix on site within authorized scope. Preserve the complete system architecture and its correctness guarantees.
