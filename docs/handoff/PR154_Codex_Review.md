# Codex review of PR #154 (relayed by cody, 2026-11-17 21:30 UTC)

Transcribed from three screenshots cody pasted into the project. PR #154 is approved for merge as documentation and
bounded model-verification evidence. No freeze, no implementation.

---

PR #154 — Review

PR #154 — Reconciliation Safety Verification — Approved for merge

Decision: APPROVE PR #154 as documentation and bounded model-verification evidence.

I reviewed the protocol changes, verification plan, and recorded TLA+ results.

Verification results

| Test | Result |
|---|---|
| C1 — Duplicate/delayed reconciliation replies | PASS — 929,059 states |
| C2 — Cross-term pending intents, four nodes | PASS — 5,675,611 states |
| C3 — Crash and self-repromotion | PASS — 23,744,032 states |
| MB — Remove reply binding | Expected safety violation |
| MI — Remove pending-intent blocking | Expected safety violation |
| MNH — Resume in old term | Expected log-identity violation |
| PS / PR — Reachability probes | Both reached |

All ten preregistered runs completed within their limits. No reruns were required.

These results directly address the two safety blockers identified in PR #153, within the model's stated bounds.

What remains unresolved

The verification is meaningful, but not a complete proof. In particular:

- C2 checks a narrow scenario, and the dangerous continuation under the corrected protocol has not been
  independently demonstrated reachable.
- The model abstracts request incarnation and sequence binding into a log-snapshot comparison.
- Timed leases, torn WAL tails, and silent storage loss remain outside this verification.
- R9-F, which prevents standby reads from exposing unacknowledged changes, is specified but **not model-checked**.
- The crash-consistent truncation procedure still has unresolved storage-format and metadata-layout details.

Architectural recommendation

I recommend **accepting R2′ as the preferred replication protocol and advancing it toward conditional freeze**,
while keeping the storage and consensus dependencies explicit.

For R9, retain the verified R9-A through R9-E rules as the candidate freeze set. Keep R9-F separate until its
read-visibility behavior is verified.

I would not authorize another broad formal-verification campaign. The next work should be a focused
protocol-acceptance review consolidating the evidence from PRs #151–154, identifying which guarantees are checked
versus argued, and recording the remaining obligations.

**Final verdict: PR #154 approved for merge. No C++ or Rust implementation authorized, and no final architectural
freeze yet.**
