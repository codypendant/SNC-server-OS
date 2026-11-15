# PR #132 HEALTH baseline: review and approval (cody, 2026-11-15)

Transcribed from cody's screenshots of the review (2026-11-15, about 18:47 UTC). The opening paragraph is
partly cut off at the top of the first screenshot; the visible part is given as seen.

---

[... partly cut off] One important finding: the existing witness detection timing exceeds the previously
proposed design bound. That means we need to distinguish a correction to our documented acceptance
criteria from an actual regression in failover behavior.

**Review decision**

Approve PR #132 for merge

The baseline is sufficiently documented to support the next implementation stage.

| Scenario | Maximum detection | Maximum promotion |
|---|---|---|
| SIGKILL | 1,084 ms | 1,120 ms |
| SIGSTOP | 1,276 ms | 1,308 ms |
| Network partition | 1,272 ms | 1,308 ms |
| Accept-path stall | 1,271 ms | 1,316 ms |
| Descriptor exhaustion | 1,273 ms | 1,333 ms |

These are measurements from ten trials per fault on the documented test environment, not universal timing
guarantees.

**Two decisions:**

1. **Accept the measured baseline**, but do not silently replace the original detection contract with a
   loose timeout. Update A2 to distinguish the existing implementation's measured behavior, the regression
   tolerance, and the intended timing guarantee.
2. **Accept runtime tracing** instead of a test-only counter, provided its overhead remains measured and
   controlled.

The resumed old primary reporting `role=p` at its previous term is a separate, potentially important
correctness concern. It should remain explicitly tracked for investigation rather than being dismissed as
a monitoring issue.

**PR #132: Approved for merge.** The next authorized step is K0, subject to preserving these baseline
guarantees and completing independent QA.

---

## How it was carried out before merge

- **Decision 1.** `docs/CONTROL_CONNECTIONS_DESIGN.md` gains section 5.1. A2(b) now refers to three terms:
  - measured behavior: `docs/HEALTH_BASELINE.md`;
  - regression tolerance: the same outcomes, with each scenario's max within one `poll_ms` of main's
    recorded max;
  - intended guarantee: `failover_ms` + one silent watch iteration, including the state read the old
    bound left out.

  The revision 4 wording is kept and quoted. The test's own bound is labelled a backstop only.
- **Decision 2.**
  - Controlled: with tracing off, `ha::Caller` and `Caller::Adopt` do no string work, and the witness
    builds its label only when tracing is on. `test_ha_caller` is checked both off and on.
  - Measured: under 1 ns per scope off, about 3 to 5 ns on; one loopback request is about 55 µs.
    Details are in `docs/HEALTH_BASELINE.md` and `bench/results/health-baseline-20261115/`.
- **Stale old primary.** Tracked as open item O1 in `docs/HEALTH_BASELINE.md` and in `docs/STATUS.md`. It
  is not investigated in this PR.
