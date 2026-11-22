# AR-1 G2 evidence report: bring-up and checker adequacy (uncounted), revision 3

## Status and authority

- **Authority.** cody's "go" on 2026-11-21 at 19:52 UTC for G2 (`docs/handoff/PR184_G2_Owner_Decision.md`).
  Codex's HOLD review of this PR (`docs/handoff/PR185_Codex_Review.md`, pasted by cody on 2026-11-22 at 02:00 UTC)
  says that authority also covers the corrective work and the uncounted reruns reported here.
- **Counted runs.** None. G3 stays on HOLD until cody authorizes it separately.
- **Prereg.** These runs follow `docs/AR1_PREREG.md` revision 3, which cody ACCEPTED on 2026-11-22 with the U-7 ruling (`docs/handoff/PR185_Rev3_Owner_Decision.md`).
  - Revision 2 is preserved in `docs/AR1_PREREG_REV2.md`.
  - Section 16 of revision 3 lists amendments A-1 to A-12, with what changed and why.
- **Discarded data.** Bring-up and adequacy rows are uncounted, so only their summaries are kept (prereg section 14):
  `results/bringup_summary.json` and `results/adequacy_score.json`. Phase 0 output is in `results/phase0/`.
- **Code.** `experiments/ar1/` contains:
  - `ar1/`: the SNC runtime, harness, services, Phase 0 and batch;
  - `check/`: the oracle and checker, from a separate implementer;
  - `plain1/`: the plain DBOS arm, step 1, from a third implementer.

  Every bring-up and adequacy row in this report comes from one code hash, `1e385c68af4e12a0`.
- **Location (U-13, unchanged).** These runs used this cloud session, not the dedicated VM:
  - 4 vCPU and 15 GiB;
  - PostgreSQL 18.7, Python 3.12 and DBOS 3.6.0, now installed from hash-pinned locks (A-12).

  Counted runs belong on the dedicated VM, installed from the same locks. This step did not need the VM. Nothing
  here measures performance.

## Verdict

**G2 is complete at bring-up scale. It now waits on cody's decision on the revision 3 amendments.**

- **Phase 0:** every check passes, including C-7b, the new focused check for the page-boundary bound.
- **SNC's arm:** 256 of 256 correct runs pass. All 128 fault points are reached, and every item clause of 7.1 and
  7.2 is exercised by at least one correct run.
- **Adequacy:** 14 of 14 mutations are flagged at every seed of every listed point, across 265 runs.
- **M1:** now detected without relying on the gateway's own claim. The receiver logs the premature request, and
  4.established flags it from that log in all 20 of the new Ac runs.
- **Clean runs:** no adequacy run was unresolved, invalid or over the run cap.

## Refreshed verification after merging main (2026-11-22)

Codex's review of #185 and #187-#191 asked for verification after integration. PR #185 merges main cleanly, and the
AR-1 code is byte-identical afterwards (same code hash). The whole set was rerun on the merged tree anyway:
- the checker's 74 unit tests pass;
- Phase 0 passes every check, including C-7b;
- SNC passes 256 of 256 bring-up runs, reaches all 128 points and exercises every clause;
- the plain arm again has 148 violations and 108 unreachable points;
- 14 of 14 mutations are flagged at every seed over 265 runs, with M1 flagged by 4.established in all 20 Ac runs.

The longest run took 91 s. Every number matches the summaries in `results/`.

## What changed since the HOLD, item by item

**2A, M5.**
- **Root cause.** In S1 the policy reads only memory, and only two things write memory:
  - setup, before the activation starts;
  - M-done, after P-dec.

  So the activation's own waits always cover an unestablished row before the policy reads it, and the revision 2
  points cannot expose M5 (A-5). This was observed in 10 of 10 runs.
- **Correction.** New point P:
  - just before the policy phase, a foreign writer commits a memory row locally while the standby is stopped;
  - a correct runtime waits for that row to be established;
  - M5 skips both the state-read wait and the policy-read wait.
- **Evidence.** Correct runs at P pass and exercise 4.policy_read. 4.policy_read flags M5 in 5 of 5 runs.

**2B, M13 at T-F and T-N.**
- **Root cause.** In revision 2's "after" order, the old request reaches the receiver only after the new epoch has
  acted. Its reply is then a rejection (T-F) or a late attempt (T-N), not a completed outcome. M13's obligation is
  to derive from a stale *completed* outcome, so it never arises (A-4).
- **Correction.** A new "after-outcome" release order: the receiver answers before the takeover, and the reply
  reaches the old activation only afterwards.
  - The "after" order stays, because it is what exercises 5.TF_stale_rejected, 5.TN_duplicate_recorded and M14.
  - A-4 justifies the change to the mutation table.
- **Evidence.** 6.derive flags M13 in 20 of 20 runs, 5 at each of the four tools.

**2C, unexercised clauses.**
- **Root causes:**
  - 1.upper: for a correct runtime, the cut taken after the answer can never show it (A-9);
  - 4.policy_read: the same cause as 2A;
  - 5.TF_stale_rejected and 5.TN_duplicate_recorded: bring-up never selected correct runs with the "after" order at
    those points.
- **Corrections:**
  - A-9 states when 1.upper counts as exercised, and adds a detector that compares against the pre-resume cut;
  - bring-up now selects seeds for every release order at every C-b2 point.
- **Evidence.** Correct runs exercise all four: 1.upper 46, 4.policy_read 2, 5.TF_stale_rejected 1 and
  5.TN_duplicate_recorded 1. The last three each have a single schedule (see "Remaining limits").

**2D, M1 independent detection.**
- **Root cause.** Synchronous commits block while the standby is stopped. So an unestablished dispatch commit never
  let the activation run on, and only the gateway's own log could show M1.
- **Correction.** New points Ac, which use C-2:
  - cancelling the SyncRep wait leaves the commit local, and the client sees success;
  - a correct runtime still waits on its own establishment read.
- **Evidence.** At Ac, M1 is flagged in 20 of 20 runs by 4.established, which reads the receiver's log, and by
  4.gateway as well.

**2E, request ids.**
- **Root cause.** The stub derived its request id from the invocation id.
- **Correction (A-6):**
  - each physical request gets a fresh random id, and the stub's output depends on that id;
  - 3.no_regenerate requires the recorded output to carry the first request's id and the hash of its reply.
- **Evidence.** M3 is flagged in 10 of 10 runs by 3.no_regenerate, and also by 3.crash_unknown.

**Section 3, amendments A-1 and A-2.**
- **Root cause.** See the Phase 0 section.
- **Corrections:**
  - A-1 rewrites prereg 7.1 and C-7;
  - A-2 adds a correction note to package 2.6 d1;
  - C-7b is a new focused check.
- **Evidence.** C-7 and C-7b pass, and both negative controls fire.

**Section 4, U-7 to U-12.** See "Open issues from the first report".

## Phase 0 (prereg section 6), bring-up scale

Each check ran 2 to 8 times, not the 20 the prereg proposes. The 20-run Phase 0 is part of the counted campaign.
Every check passed on the final code.

| Check | Runs | Result |
|---|---|---|
| OI-1a (off, local, cancel, restart-immediate, restart-fast) | 5 | Pass, with the same evidence as before: no API, policy read or gateway decision treats a row as authoritative before resume. |
| OI-1a control (gate off) | 5 | Pass: the violation fires in every variant. |
| OI-1a-v | 2 | Pass: NULL LSNs and a non-`sync` standby both count as not established. |
| OI-1b | 8 | Pass: the timeline id is 2 on the first read after promotion, in every variant. |
| OI-1c | 2 | Pass. |
| OI-1d | 2 | Pass, and the negative control fires. In 37 of 40 random kills the token had committed; there were no violations. |
| OI-1e | 2 runs, 1,000 freezes | Pass on all three sub-checks, and the negative control fires. |
| C-7 | 2 × 10,000 commits, `on` and `off` | Pass. The bound is never below the commit end as A-1 defines it. The `off` negative control fires in 10,000 of 10,000. Establishment under `on`: p50 0.16 ms, p99 0.54 ms, max 5.5 ms. 7 bounds fall at a page start, and their worst wait is 0.21 ms. The 18 s maximum under `off` is the single final wait under `wal_writer_delay` = 10 s, by design. |
| **C-7b** (new) | 2 runs, 26 landed commits each | Pass; details below. |

### C-7b: the focused check Codex asked for

Each case writes a commit sized to land on the boundary under test, using transactional `pg_logical_emit_message`
and a feedback loop. A case passes only if the commit really landed where intended, and in both runs every case did.

| Case | Per run | What it requires | Result |
|---|---|---|---|
| Ends exactly at a page start (short header, 24 bytes) | 10 | raw − end = 24 and bound = end | 20 of 20 ok. Waits are at most 5.6 ms. |
| Ends exactly at a segment start (long header, 40 bytes) | 2 | raw − end = 40 and bound = end | 4 of 4 ok. |
| Crosses a page | 10 | the record spans the page, and bound = raw = end | 20 of 20 ok. |
| Crosses a segment | 1 | the record spans the segment, and bound = raw = end | 2 of 2 ok. |
| Followed by `pg_switch_wal()` | 3 | bound ≥ end, on a segment start | 6 of 6 ok. Waits are at most 21 ms. |

- **In every case** the bound is at or above the commit's end, and establishment takes under 1 s.
- **Negative control:** waiting on the raw insert position at a page start stalls for the full 2 s.
  - This happened in 9 of 10 page cases per run.
  - In the tenth, unrelated WAL released the wait.
  - The control fired in both runs.
- **Assumption:** the bound's constants assume initdb's 8 KiB pages and 16 MiB segments, and `rt.setup_db` now
  asserts both.

## Bring-up (prereg 5.3), both arms

The bring-up covers 128 points, 2 seeds per point and 2 arms: 512 runs. At each C-b2 point, the seeds cover every
release order that point uses:
- model and T-I use "before" and "after";
- T-F and T-N use "after" and "after-outcome".

| Arm | Runs | Pass | Violation | Point not reachable | Unresolved |
|---|---|---|---|---|---|
| SNC | 256 | 256 | 0 | 0 | 0 |
| plain DBOS, step 1 | 256 | 0 | 148 | 108 | 0 |

**SNC: every fault point is reached, and every clause is exercised.** All 128 points were reached at both seeds.
The table gives the number of correct runs that exercise each clause (also in `results/bringup_summary.json`, whose
`unexercised` list is empty). Bold marks the four clauses Codex named.

| Item | Clauses and correct runs exercising each |
|---|---|
| 1 | 1.lower 214, 1.prefix 254, 1.stale_epoch 32, 1.state 254, **1.upper 46** |
| 2 | 2.end_state 256, 2.no_lost_output 254, 2.no_repeat 233 |
| 3 | 3.crash_unknown 6, 3.hash 254, 3.late_marked 2, 3.no_regenerate 254, 3.recall 16, 3.states 256 |
| 4 | 4.denied 9, 4.established 254, 4.gateway 254, 4.mapped 254, **4.policy_read 2** |
| 5 | **5.TF_stale_rejected 1**, 5.TI_one_effect 243, 5.TI_same_key 211, **5.TN_duplicate_recorded 1**, 5.TN_no_retry 208, 5.conflict 2, 5.recorded 226 |
| 6 | 6.TI_status 7, 6.derive 12, 6.in_doubt_owner 17, 6.no_deadlock 8, 6.outcome_exempt 30, 6.outstanding 34, 6.prepared_rejected 14 |
| 7 | 7.authorized 243, 7.committed 256, 7.executed 256, 7.reconstructed 256, 7.shown 256 |

**Plain arm, step 1.** These results are data, not gates (prereg section 9).
- **Clauses that fired, counted over runs:**
  - 4.mapped 474;
  - 7.executed 168;
  - 2.no_repeat 68;
  - 5.conflict 42;
  - 3.recall 26;
  - 5.TI_one_effect 22;
  - 2.end_state 22;
  - 7.reconstructed 19;
  - 5.recorded 18;
  - 5.TN_no_retry 8;
  - 1.stale_epoch 5;
  - 3.crash_unknown 4.
- **Unreachable:** 108 runs never reached their point. These points have no plain equivalent, and they now include
  Ac and P, which need SNC's dispatch and establishment records. Each counts as item unmet.

## Checker adequacy (prereg 7.3, as revised by A-3, A-4, A-5 and A-8)

The adequacy runs total 265 mutation runs, with 5 seeds at each point. A mutation counts as flagged only when a
clause it breaks reports it.

| Mutation | Points | Runs | Flagged | Flagging clauses (runs) |
|---|---|---|---|---|
| M1 send before establishment | a3 of I/TI/TF/TN-disp; **Ac** of the same four | 40 | 40 | 4.gateway 40; **4.established 20 (all Ac runs)** |
| M2 no epoch check on decisions | C-s of I/TI/TF/TN-out | 20 | 20 | 1.stale_epoch 20 |
| M3 lost output regenerated | b2, b3 of model | 10 | 10 | **3.no_regenerate 10**, 3.crash_unknown 10 |
| M4 automatic T-N retry | b2, b3 of T-N | 10 | 10 | 2.no_repeat 10, 5.TN_no_retry 10 |
| M5 policy read of an unestablished row | **P/pre-policy** | 5 | 5 | **4.policy_read 5** |
| M6 takeover keeps prepared calls | C-p of I/TI/TF/TN-prep | 20 | 20 | 6.prepared_rejected 20 |
| M7 reconstruction drops an attempt | b2 of T-I | 5 | 5 | 7.executed 5 |
| M8 outcome blocked by the epoch check | C-b2 of the four tools, seeds not "before" | 20 | 20 | 6.outcome_exempt 15, 5.recorded 5 |
| M9 re-send with a new key | b2 of T-I | 5 | 5 | 5.TI_same_key, 5.TI_one_effect, 2.no_repeat (5 each) |
| M10 no epoch check on P-dec, M-done | C-s of I-out, TN-out | 10 | 10 | 1.stale_epoch 10 |
| M11 no outstanding set | C-a1 of the four tools; C-b2 of the four tools, seeds not "before"; D b1-b3 of T-I, T-F and T-N | 85 | 85 | 6.outstanding 85 |
| M12 re-call without retry_of | b2, b3 of model | 10 | 10 | 3.recall 10 |
| M13 no derivation from a stale outcome | C-b2 of the four tools: "after" for model and T-I, **"after-outcome" for T-F and T-N** | 20 | 20 | **6.derive 20** |
| M14 conflict rule ignored | C-b2 of T-N, "after" seeds | 5 | 5 | 5.conflict 5 |

The adequacy table is complete: 14 of 14 mutations are flagged at every seed of every listed point.

## Open issues from the first report (Codex section 4)

**U-7: model output replayed from the DBOS step checkpoint.** Ruled by cody on 2026-11-22 (ACCEPTED).
- The output is replayed, not regenerated: no second physical request goes out.
- A-6 checks that the recorded output carries the first request's id and hash.
- The ruling: a replay of a checkpointed output is the same output, consistent with prereg section 3 and E-11.

**U-8: snapshot order versus LSN-prefix order.** Classified as safe for establishment; the checker now handles it.
- A snapshot can include a commit above an earlier commit that was still in progress, and that earlier commit is
  invisible to the snapshot.
- Establishment still holds, because the bound is taken after the snapshot and the standby's flush is a prefix.
- The state answer now carries its snapshot (`pg_current_snapshot()`).
- The 1.prefix gap check exempts xids that were in progress in that snapshot, and lists each exemption in the
  result. No bring-up run needed one.

**U-9: the pre-resume cut assumed a stopped walreceiver.** Fixed (A-10).
- The cut reads the standby through the oracle's open connection.
- A run is invalid unless the walreceiver's pid is unchanged and its process state is stopped (T).
- No run was invalid.

**U-11: dependency reproducibility.** Fixed (A-12).
- `conda-linux-64.lock` (43 packages) and `requirements.lock` (16 wheels) pin every package by sha256.
- `setup.sh` checks the micromamba binary's hash and installs only from the locks.
- A clean reinstall reproduced the environment exactly.

**U-12: run-cap accounting.** Fixed (A-11).
- The 5-minute cap covers setup through the end of the check.
- Each run records its wall time, and a run over the cap is unresolved.
- The longest of the 777 runs took 89 s.

These items from the first report remain open:
- **U-5:** received() means delivered with no kill of the requester within 1 s. It now rests on `delivered` log
  entries (A-7). No run was affected.
- **U-6:** OI-1d's random kills never produced "not committed".
- **U-10:** the plain arm's takeover model is its implementer's choice; step 2 revisits it.

## Independent review of the corrections

A separate reviewer was told to break the corrections and their interaction with the checker. It found the issues
below, and each was fixed on site before the final reruns above.
- **1.upper had no real detector.** Fixed with the "answer-returned" marker plus a comparison against the
  pre-resume cut.
- **4.policy_read could count as exercised too easily.** It now counts only after `pre-policy` is reached.
- **Scoring did not enforce the run cap.** Fixed (A-11).
- **U-8 was unhandled.** Fixed with the snapshot exemption.
- **The canceller could cancel a SyncRep wait twice.** It now cancels once per backend and state change. It also
  covers DBOS's system database connections, whose commits block on SyncRep too.
- **The geometry was assumed.** It is now asserted.
- **C-7b's checks were too loose.** Tightened to the exact header arithmetic above.

The checker's 64 unit tests pass.

## Remaining limits

- **The foreign writer at P is outside S1.** S1 has no writer of its own that can put unestablished state in front
  of the policy (A-5). So M5 is shown against a schedule that S1's own workload never produces.
- **M3's first-request branch never fires.** M3 is detected through the output hash and request id. No run had a
  second request reach the stub before the first reply.
- **Thin correct-run coverage at bring-up scale.** 4.policy_read, 5.TF_stale_rejected and 5.TN_duplicate_recorded
  each have one schedule and 1 or 2 runs. The counted campaign runs 20 seeds per point.
- **Geometry.** The bound's constants assume 8 KiB pages and 16 MiB segments (asserted at setup).
- **Location:** cloud session (U-13). Counted runs go on the dedicated VM, installed from the locks.
- **Approval:** A-1 to A-12 and the U-7 ruling are ACCEPTED. G3 still needs cody's separate GO for counted
  execution.

## Previous round (kept as history)

The first G2 round ran on code hash `ca5fa715825816d1` and the revision 2 matrix, with 123 points and 492 bring-up
runs:
- **SNC:** 246 of 246 correct runs passed.
- **Plain arm, step 1:** 148 violations, and 98 runs at points it cannot reach.
- **Adequacy:** 250 runs. 12 of 14 mutations were fully flagged. M5, and M13 at T-F and T-N, were not exercised.
- **Coverage:** four clauses were exercised by no correct run, and only 4.gateway flagged M1.

Codex's review of those results is in `docs/handoff/PR185_Codex_Review.md`. That version of this report is in
commit `ed4de49`.

## Scope notes

- `docs/handoff/` and `docs/STATUS.md` are project bookkeeping, outside prereg section 12's code paths.
- This step did no S2 or S3 work, no custom replication, no R640 work and no performance work, and added no
  production dependency.
