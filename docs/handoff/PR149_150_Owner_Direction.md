# Owner direction on PRs #149 and #150 (cody, 2026-11-17 00:16 UTC)

Transcribed verbatim from cody's message in the project. This is the owner's decision record.

---

SNC AFR-001 — Owner Direction on PRs #149 and #150

I approve the following direction, subject to the existing engineering and verification gates.

1. R2 — Verified survivors

Approve R2′ as the preferred design candidate: retain surviving members of the committed in-sync set after promotion, subject to durable term fencing, log reconciliation, positioning, and lease requirements.

Authorize the bounded TLA+ validation plan in PR #150: maximum eight runs, two hours per run, no R640. Any unexplained safety counterexample is a blocker. A timeout is NOT COMPLETED, not PASS.

Do not freeze R2′ until the model evidence and independent QA are reviewed.

2. Promotion eligibility

Approve P1–P7 and S1–S5 as the proposed replacement for E1–E11, subject to verification against the selected R2 protocol. Keep promotion eligibility distinct from permission to begin serving.

3. Single-copy recovery

Approve monotonic LSN reservation as the preferred standalone-node candidate.

For a restarted degraded-group primary, approve witness-coordinated re-promotion into a new term as the preferred candidate. It must never resume serving under its old term. Verify the implications for fencing, leases, log matching, and recovery before freezing this rule.

D-02 and the silent-loss durability boundary remain unresolved.

4. X-2 performance thresholds

Set provisional limits of:

- Maximum 10% throughput reduction.
- Maximum 10% p99 acknowledgment-latency increase.

Apply the preregistered confidence-interval and control requirements. Report any inconclusive result as UNRESOLVED; do not adjust thresholds after observing results.

Any mechanism permitting silent loss under a covered fault is disqualified, regardless of performance.

5. X-2 execution authorization

Approve the preregistration in PR #149 as the planning baseline, subject to final independent QA.

Do not start the approximately 9.5-hour R640 benchmark.

The harness implementation, offline correctness execution, and R640 benchmark remain separate approval gates.

Read-only blktrace access is not authorized at this stage. Request it separately only if needed to resolve an otherwise unanswerable measurement question.

6. QA and scope

Complete independent re-review of the corrected PR #149 and #150 documents. Record any unresolved findings.

The targeted R9 verification remains authorized under the previous scope.

No C++ or Rust product implementation is authorized. No D-02 selection or final R2 freeze is authorized.

Proceed with the bounded R2 model verification, complete QA, and return with the results and any required owner decisions.

---

## Where each item is handled

- Sections 1, 2, 3 and 6 for PR #150: `docs/AFR_001_R2_AND_SINGLE_COPY.md` and the bounded R2 model runs (follow-up PR).
- Sections 4, 5 and 6 for PR #149: the X-2 preregistration thread.
