# Codex review of PRs #185 and #187-#191

Pasted by cody in the project chat on 2026-11-22 at 21:50 UTC. It is filed verbatim below.

---

SNC — Codex Review: PRs #185 and #187–#191
Reviewed the latest repository changes, relevant code, evidence reports, and available GitHub Actions results.
PR #185: G2 evidence acceptable, but merge HOLD until the integration conflict is resolved and verification is refreshed.
PRs #187–#190: Already merged. No rollback requested.
PR #191: Results accepted, interpretation correction required.
Required Next Actions

1. Resolve PR #185's reported merge conflict. Preserve the corrected G2 evidence, including 256/256 passing bring-up runs, 14/14 detected mutations and full 128-point reachability. Rerun applicable verification after integration.
2. Present preregistration revision 3 amendments A-1 through A-12 and the U-7 DBOS checkpoint-replay ruling for the owner's explicit acceptance. Do not initiate counted AR-1 runs without separate G3 authorization.
3. Strengthen the Track B durability checker with a targeted negative control that removes an acknowledged durable update from a disposable replay image. The independent checker must reject the corrupted result. Keep this inside the existing authorized harness scope.
4. Correct PR #191's interpretation: identical hardware does not imply identical flush or FUA overhead. Retain the measurements and preserve the unverified status of power-loss protection.
5. Preserve the approved Track B thresholds. `CRITERIA.md` must be completed and frozen before comparative benchmarking.
6. Continue the authorized Track B baseline preparation. Do not modify SNC's engine, resume custom replication, or begin raft-rs integration.

Track A still requires completion of the AR-1 prerequisites and its own authorization.
Engineering Assessment
The device-mapper controls and SNC's 20-mark durability CI provide useful evidence that the test harness is functioning.
They do not yet establish SNC's superiority over Garnet, Restate or PostgreSQL.
Do not treat completed harness work as a comparative performance result.
Next priority: close PR #185 cleanly, strengthen the durability checker, then complete the comparative baselines under the approved criteria.
Always verify, never assume. Fix on site. Preserve independent verification and original evidence.
