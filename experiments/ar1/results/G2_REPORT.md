# AR-1 G2 evidence report: bring-up and checker adequacy (uncounted)

## Status and authority

- **Authority:** cody's "go" on 2026-11-21 at 19:52 UTC for G2 (`docs/handoff/PR184_G2_Owner_Decision.md`). It
  covers the AR-1 code, uncounted bring-up and the checker-adequacy runs of `docs/AR1_PREREG.md` revision 2.
- **Counted runs:** none. G3 stays on HOLD.
- **Discarded data:** the bring-up and adequacy rows are uncounted, so only the summaries below are kept (prereg
  section 14). Phase 0 output is kept in `results/phase0/` because it is small and is what the Phase 0 gate reads.
- **Code:** `experiments/ar1/` contains:
  - `ar1/`: the SNC runtime, harness, services, Phase 0 and batch;
  - `check/`: the oracle and checker, written by a separate implementer;
  - `plain1/`: the plain DBOS arm, step 1, written by a third implementer.
- **Location deviation.** The accepted default was a dedicated cloud VM, but these runs used this cloud session
  (prereg section 14 lists a cloud session as an option). The container:
  - 4 vCPU, 15 GiB, one virtual disk;
  - PostgreSQL 18.7, Python 3.12, DBOS 3.6.0, psycopg 3.3.9 and SQLAlchemy 2.1.9 from conda-forge;
  - manifest in `bench/manifests/ar1-g2-cloud-20261121.txt`.

  G3 counted runs belong on the dedicated VM, with their own manifest and the SHA freeze of prereg 5.5. Nothing here
  measures performance.

## Verdict

**Not ready for G3.** Phase 0 passes at bring-up scale. SNC's arm passes all 246 correct bring-up runs. The checker
flags every mutation that the schedules exercise, at every seed (12 of 14 mutations at all their points). Three items are still UNRESOLVED and must be
settled before counted runs can mean anything:

1. M5 is never exercised;
2. M13 is not exercised at T-F and T-N;
3. four item clauses are exercised by no correct run.

The prereg also needs two amendments, and cody or Codex must accept them.

## Phase 0 (prereg section 6), bring-up scale

These are 2 to 8 runs per check, not the 20 the prereg proposes. The 20-run Phase 0 is part of the counted
campaign on the dedicated VM. Every check below passed on the final code.

| Check | Runs | Result |
|---|---|---|
| OI-1a (off, local, cancel, restart-immediate, restart-fast) | 5 | Pass. In every variant the rows exist physically, and no API, acknowledgement, policy read or gateway decision treats them as authoritative before resume. The kill cut classes them L. The dispatch commit for the gated attempt lies above the insert LSN read before resume. After resume everything proceeds, including the gateway send. In restart-fast, the `off` commit survives with both its row and its token. In restart-immediate, both are lost, which is accepted. |
| OI-1a control (gate off, same 5 variants) | 5 | Pass: the violation fires in every variant. The dispatch commit lands below the pre-resume insert LSN. The read gate is what this exercises; the send gate is masked by the synchronous commit blocking, as prereg line 594 already states. |
| OI-1a-v | 2 | Pass. NULL LSNs without `pg_monitor`, and a standby whose `sync_state` is not `sync`, both count as not established. |
| OI-1b | 8 | Pass. The candidate timeline id is 2 on the first read after `pg_promote` and agrees with `IDENTIFY_SYSTEM`. The pre-promotion session errors during recovery and is correct afterwards. The variants cover: a restartpoint before promotion, a restart after it, and a standby cut off before promotion. `pg_control_checkpoint().timeline_id` read 1 in all 8 runs, which confirms the a priori rejection; it read 2 only after an explicit CHECKPOINT. |
| OI-1c | 2 | Pass. Token lookup says not committed in every run. As a result, not a gate: with a late xid, `pg_xact_status` says "in the future" before the counter passes X, and afterwards "committed" by the reissuing transaction. With an early xid logged in a running-xacts record, it says "aborted". X's commit lies above the switchpoint in both runs. |
| OI-1d | 2 | Pass. The deterministic case finds the commit and its bound is at or above the commit end. The negative control fires: before d7 the token is invisible and the xact is "in progress". There are 20 random kills per run, with no violations. All 39 of 40 random-kill tokens that committed were found. The random case never produced "not committed", so only the deterministic case carries the d7 evidence (UNRESOLVED U-6). |
| OI-1e | 2 runs, 1,000 freezes | Pass on all three sub-checks, and the negative control fires. |
| C-7 | 10,000 commits, `on` and `off` | Pass: the bound is never below the commit end. The `off` negative control fires, with `pg_current_wal_lsn()` below the commit end in 10,000 of 10,000. Establishment under `on`: p50 0.16 ms, p99 0.46 ms, max 4.9 ms. Bounds where the commit ends on a page start: 7 (`on`) and 9 (`off`). Their worst wait was 0.23 ms after the fix described below. |

### Finding F-1: page-boundary stall in the bound (fixed, needs amendment A-2)

- **What happened.** When a commit record ends exactly at an 8 KiB page start, `pg_current_wal_insert_lsn()`
  returns that position plus the page header (24 bytes, or 40 at a 16 MiB segment start). The standby cannot report
  a flush there until some other WAL is written.
- **Effect before the fix.** In C-7, 23 of 10,000 waits took 4.7 to 24.4 s on an idle server.
- **Fix.** `rt.BOUND` subtracts the header in exactly those two cases. An independent review checked it against
  short and long page headers, records that cross pages, and XLOG_SWITCH, and found it never below a commit's end
  (the constants rely on initdb's 8 KiB pages and 16 MiB segments).

### Amendments the prereg needs (PROPOSED, not accepted)

- **A-1, commit end.** The prereg defines a commit's end as "the next record's start in an unfiltered dump". The
  checker instead uses the record's own end, XactLastRecEnd: start plus length, advanced over page headers. That is
  the position SyncRep waits for.

  The two differ exactly at page starts. By the prereg's definition, C-7 would count 7 and 9 bounds "below the end",
  and the only bound that satisfies it is one that can wait forever on an idle server. All three reviewers agree
  the checker's definition is the right one. It needs cody's or Codex's acceptance as a revision before G3.
- **A-2, the bound** of F-1, as a design note for package 2.6.

## Bring-up (prereg 5.3, at most 2 seeds per point), both arms

The run totals 492: 123 points, 2 seeds and 2 arms. All rows come from code hash `ca5fa715825816d1`. Adequacy ran
on `41328fcc698a2a44`, and the only difference is the M11 seed selection in `ar1/batch.py` (U-3). Summaries:
`results/bringup_summary.json`, `results/adequacy_score.json`.

| Arm | Runs | Pass | Violation | Point not reachable | Unresolved | Harness error |
|---|---|---|---|---|---|---|
| SNC | 246 | 246 | 0 | 0 | 0 | 0 |
| plain DBOS, step 1 | 246 | 0 | 148 | 98 | 0 | 0 |

**Plain arm, step 1.** These results are data, not gates (prereg section 9).
- **Clauses that fired, counted over runs:**
  - 4.mapped 473 (the tool sends have no dispatch record);
  - 7.executed 168;
  - 2.no_repeat 66;
  - 5.conflict 40;
  - 3.recall 26;
  - 5.TI_one_effect 22;
  - 2.end_state 22;
  - 7.reconstructed 20;
  - 5.recorded 18;
  - 5.TN_no_retry 8;
  - 1.stale_epoch 6;
  - 3.crash_unknown 4.
- **Unreachable points:** the 98 runs that never reached their point are at points that have no plain equivalent: every `*-prep` and `*-disp` of a tool, `I-disp`, `I-acc`, `I-unknown` and `TN-indoubt`. Its implementer chose not to add SNC-style records to fake them (`plain1/TAKEOVER.md`). Each counts as item unmet.
- **Lines:** plain step 1 is 367 logical lines. SNC's `rt.py`, `app.py` and `iface.py` together are 907 lines.

**SNC's arm: clauses no correct run exercised.** Per prereg 5.4, these are UNRESOLVED for counted runs:
- 1.upper;
- 4.policy_read;
- 5.TF_stale_rejected;
- 5.TN_duplicate_recorded.

Some of them are exercised under mutations, for example 5.TF_stale_rejected under M8. A correct run that exercises
them still needs a schedule.

## Checker adequacy (prereg 7.3)

250 mutation runs, 5 seeds at each point where 7.3 says the mutation is observable, one code hash. A mutation counts
as flagged only when a clause it breaks reports it (`check/score.py`, `MUTATION_CLAUSES` in `check/check.py`). No
adequacy run was unresolved, invalid or a harness error.

| Mutation | Points | Runs | Flagged | Flagging clauses |
|---|---|---|---|---|
| M1 send before establishment | a3 of I/TI/TF/TN-disp | 20 | 20 | 4.gateway |
| M2 no epoch check on decisions | C-s of I/TI/TF/TN-out | 20 | 20 | 1.stale_epoch |
| M3 lost output regenerated | b2, b3 of model | 10 | 10 | 3.crash_unknown, 3.recall |
| M4 automatic T-N retry | b2, b3 of T-N | 10 | 10 | 2.no_repeat, 5.TN_no_retry |
| M5 policy read of an unestablished row | a3 of C-acc, I-out | 10 | **0, not exercised** | none (U-1) |
| M6 takeover keeps prepared calls | C-p of I/TI/TF/TN-prep | 20 | 20 | 6.prepared_rejected |
| M7 reconstruction drops an attempt | b2 of T-I | 5 | 5 | 7.executed |
| M8 outcome blocked by the epoch check | C-b2, "after" seeds | 20 | 20 | 6.outcome_exempt, 5.recorded |
| M9 re-send with a new key | b2 of T-I | 5 | 5 | 5.TI_same_key, 5.TI_one_effect, 2.no_repeat |
| M10 no epoch check on P-dec, M-done | C-s of I-out, TN-out | 10 | 10 | 1.stale_epoch |
| M11 no outstanding set | C-a1, C-b2 ("after" seeds), D b1-b3 | 85 | 85 | 6.outstanding |
| M12 re-call without retry_of | b2, b3 of model | 10 | 10 | 3.recall |
| M13 no derivation from a stale outcome | C-b2, "after" seeds | 20 | **10** (model 5/5, T-I 5/5; T-F, T-N not exercised) | 6.derive (U-2) |
| M14 conflict rule ignored | C-b2 of T-N, "after" seeds | 5 | 5 | 5.conflict |

So 12 of 14 mutations are flagged at every seed of every listed point. M5 and half of M13's points are never
exercised: the checker does not miss them, the schedule never creates the situation. Under prereg 7.3 that still
leaves adequacy UNRESOLVED until the schedules or the table are revised and reviewed. One more limit from the
review: 3.no_regenerate cannot tell a regenerated reply apart, because the stub's request id is derived from the
invocation id alone, so M3 is caught only through 3.crash_unknown and 3.recall.

## What was fixed on site, and reviewed

The runtime, harness, checker and Phase 0 each had an independent review that was told to break them, three in
all. A fourth review then went over the fixes. Each defect was fixed with the smallest change. The main ones:

- **Runtime:**
  - state reads now lock (FOR UPDATE), which closes an I-acc race where a compare-and-set passed on changed state;
  - each prepare and dispatch re-checks authorization;
  - late tool attempts are recorded as attempts, with no state change;
  - there is an in-doubt escalation timer;
  - derive excludes only originals whose re-call was dispatched;
  - cancel reads its bound after COMMIT, no longer inside its own transaction;
  - the page-boundary bound (F-1).
- **Harness:**
  - the C-b2 schedule was redesigned: the old activation is SIGSTOPped with its reply in flight, and the release order comes from a seed bit independent of the owner and approver answers;
  - T-F and T-N wait for the new activation, and become UNRESOLVED if it never arrives;
  - receivers log `delivered` after flushing a reply, and the harness waits for it;
  - b2 kill releases;
  - a pre-resume oracle cut for M5;
  - batch isolation (each run in its own session, killed by session).
- **Checker:**
  - received() uses `delivered` entries;
  - kills are executor-aware;
  - the M6 epoch comes from the prepared token;
  - T-F stale rejection is checked in response order;
  - only decision history counts;
  - error answers are unresolved;
  - separate oracle and invariant verdicts, plus a scorer;
  - the scorer skips unresolved and invalid runs;
  - the plain arm's stub entries map to its `I-prep` commit;
  - the release-order seed bit is fixed.
- **Phase 0:**
  - controls run every variant and pass on specific evidence;
  - liveness requires the gateway send to succeed;
  - C-7 runs both modes and gates on its negative control;
  - OI-1d gates on its negative control;
  - padding is incompressible;
  - OI-1b adds the cut-off and post-checkpoint variants;
  - OI-1c gates X's commit above the switchpoint;
  - a predicate bug that made OI-1a fail on correct runs is fixed.

## UNRESOLVED

- **U-1, M5 is not exercisable** at A/a3:C-acc or A/a3:I-out. Recovery's earlier commits are synchronous, so they
  block while the standby is stopped, and P-dec never commits before resume. This needs a schedule decision: a point
  where P-dec has no synchronous commit before it, or a statement that M5 is unobservable in S1.
- **U-2, M13 is not exercised** at C-b2/T-F and C-b2/T-N.
  - At T-F, the old reply arrives after the new epoch raised the fence, so it is `rejected_stale` and no stale
    outcome exists to derive from.
  - At T-N, the new activation has already settled the call.
  - M13 is caught at every seed where it can happen (C-b2/model and C-b2/T-I). Prereg 7.3 lists all four tools, so
    either the schedule or the table must change.
- **U-3, the M11 seed restriction.** At C-b2 of the tools, M11 is observable only when the old reply is released
  after the takeover. With "before" seeds there is no outstanding call, and 7 of 40 runs in a first adequacy pass were
  not exercised. The batch now selects "after" seeds for M11 at C-b2, as it already does for M8 and M13, and the
  rerun above flags all 85. This is a reading of 7.3's "observable at" and should be confirmed.
- **U-4, unexercised clauses** of the SNC arm, listed under bring-up.
- **U-5, `received()` is a heuristic.** It means delivered with no kill of the requester within 1 s. A kill cut
  longer than 1 s at b3 could make 5.TN_duplicate_recorded flag a correct run. No run did.
- **U-6, OI-1d's random kills** never hit "not committed".
- **U-7, model output at a1-a3 of I-out** is replayed from DBOS's step checkpoint. Is that consistent with prereg
  section 3 and E-11?
- **U-8, REPEATABLE READ snapshot order vs LSN-prefix order.** The policy and gateway reads take a snapshot, then a
  bound. Under concurrent commits, a snapshot can include a commit above an earlier one it lacks. No run showed it;
  no proof either.
- **U-9, the pre-resume cut** reuses the kill cut's standby read. That is sound only while the walreceiver stays
  stopped, and nothing asserts that it did.
- **U-10, the plain arm's takeover model** (a new workflow per epoch) is its implementer's choice. Step 2 decides
  whether it changes.
- **U-11, setup reproducibility.** `setup.sh` installs from conda-forge without a lock file or hashes. G3 needs
  pinned hashes on the VM.
- **U-12, run caps.** RUN_CAP does not cover setup and check time.
- **U-13, location** (see the top of this report).

## Scope notes

- `docs/handoff/` and `docs/STATUS.md` are project bookkeeping, outside prereg section 12's code paths.
- No S2 or S3 work, no custom replication, no R640, no production dependency.
