# Final review of PR #137 and #141 (Codex, relayed by cody, 2026-11-16 11:07 UTC)

Posted by cody in the project chat as three screenshots of Codex's review; transcribed verbatim.

> **Final review — PR #137 and #141**
>
> **PR #137 — Startup errors** — Conditional approval
>
> Claude has reproduced a plausible mechanism for the x7 CI timeout: the primary can be killed while its HA
> group is NOT READY, in which case promotion is correctly refused.
>
> The investigation is credible and explains why the timeout can occur without a startup-code regression.
> However, the original failing CI logs are unavailable, so this is a reproduced mechanism rather than proof
> of that run's exact cause.
>
> **Approve merge once the final-head CI suite is green.** The latest material I retrieved does not
> establish that this final condition has been met.
>
> **PR #141 — Listener hardening** — Approved
>
> Claude has satisfied the conditions from my previous review:
>
> - Complete suite: **64/64 passed** on `e5c9561`.
> - Mixed-version x9: **passed** against the older D.1 implementation.
> - CI fast job: reported green.
> - Remaining authentication and authorization gaps: explicitly documented.
>
> The subsequent commit is documentation-only.
>
> **PR #141 is approved for merge**, subject to normal repository checks.
>
> The remaining x7 issue should be tracked separately as a test-readiness race. It should not be confused
> with the startup error handling in #137.
>
> My recommendation is to merge #141, finish the final CI gate for #137, and keep the architecture
> safety-analysis work moving independently.

## Record: #137's final-head CI gate was met before the merge

| | |
|---|---|
| #137's final head | `5010bec5a014ad8d0a980541eade656aeb679fac` (main with #140, docs only, merged in) |
| Run | 43793041721 (`workflow_dispatch`, `ci.yml`), the one rerun cody chose on the decision card |
| build + fast tests | success, 53/53, completed 2026-11-16 02:41:57Z |
| build + full suite | success, 63/63, completed 2026-11-16 03:06:12Z |
| Merge | 2026-11-16 03:06:36Z, merge commit `e1594bd`, parents `5338688` (main) and `5010bec` |

The x7 and x7b scenarios both passed in that run. The earlier red run on the previous head 085cec3
(43783722134: x7b liveness, `test_xnode` X5) is recorded in
`PR137_D4_Startup_Errors_Review_Approval.md` and in `docs/STATUS.md`.

## Follow-through

- x7 is tracked on its own as **T-X7, a test-readiness race**, in `docs/STATUS.md`, separate from #137's
  startup error handling. The x7b liveness failure is **T-X7B**; the test-tooling fixes are listed with
  them. None is started.
- D4-b, D4-c and D5 stay unstarted. All of these wait for cody's go.
