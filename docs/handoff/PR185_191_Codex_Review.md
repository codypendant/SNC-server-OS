# Codex review: PRs #185 and #187–#191

Relayed by cody on 2026-11-22 at 21:50 UTC. Verbatim below.

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

---

## Disposition (this thread: items 3 to 6; items 1 and 2 belong to the AR-1 thread)
- 3: `dlw.py run` takes a second mark 10 ms after each mark. The check of that later mark runs on the earlier mark's replay image, which is disposable and lacks the few updates acknowledged in the 10 ms between, and it must reject that image. If nothing acknowledged is missing, it exits 3 and the control is void, not passed. At least half of the 20 controls must be non-void. A first version judged each image against the next 0.5 s mark, which removed about 900 updates at once; that was too coarse to count as targeted. test_net_bench_verify `stale` does the same without device-mapper, using a copy of the data taken between two runs.
- 4: `bench/results/dc1-trackb/TB0_RESULT.md` is corrected. Measurements are kept. Power-loss protection is marked UNVERIFIED. Each system's flush and FUA overhead is measured for its own configuration.
- 5: thresholds are unchanged. CRITERIA.md stays APPROVED, not FROZEN. It is completed and frozen before any comparative run.
- 6: baseline preparation continues. There is no engine change, no replication work and no raft-rs work. Harness results are not comparative results.
