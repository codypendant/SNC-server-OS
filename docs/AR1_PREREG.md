# AR-1 focused agent runtime proof of concept: preregistration (PROPOSED revision 2, NOT RUN)

## 0. Authority, status and labels

- **Authority.** cody's OD-P7, 2026-11-21 16:11 UTC (`docs/handoff/PR183_Owner_Decisions.md`): "Authorizes writing
  and reviewing the AR-1 preregistration only. No code until you approve that document."
- **Status.** Nothing has run, and no AR-1 code exists.
- **Revision 1.** A three-way review was told to break the first draft and raised 48 findings. All are applied
  (section 15).
- **Revision 2.** Codex's HOLD review of PR #184 (`docs/handoff/PR184_Codex_Review.md`) found two blockers: the
  durability oracle (7.1) and mutation M2's schedule (5.1 C, 7.3). Only those, and what they touch, change (section
  15).
- **Three separate gates:**
  - **G1:** approve this document;
  - **G2:** write the AR-1 code and run uncounted bring-up and the checker-adequacy runs;
  - **G3:** start counted runs.

  This document asks cody for G1 and G2 together (section 14). G3 is a separate GO, given after cody has seen the
  bring-up and checker-adequacy report.
- **Scope.** S1 of `docs/AGENT_RUNTIME_DECISION_PACKAGE.md` (the package) only. PostgreSQL 18 and DBOS Transact
  Python are experiment tools (OD-P3).
- **Freeze.** After G1, nothing here changes once the first counted run starts. A change before then is a new
  revision, and it needs review and approval again.
- **Labels:**
  - **FACT**: read in this repository;
  - **EXT**: external, cited;
  - **EXT-SRC**: read in source code;
  - **INF**: inference;
  - **HYP**: untested;
  - **UNVERIFIED**: not checked;
  - **PROPOSED**: a number for cody to set before any run.

## 1. Questions

1. **Phase 0 (OI-1 and the durability checks).** Do the PostgreSQL 18 mechanisms that package 2.6 relies on behave as
   assumed? These are the timeline id, the identity and bound of an unknown commit, the line between physical rows
   and authoritative state, and checks C-1, C-2, C-3 and C-7.
2. **Correctness.** Does an S1 runtime built on DBOS Python meet items 1-7 of package 7.1 at every crash point?
3. **Differentiation (item 8).** How much of items 1-7 does plain DBOS meet as an application would normally write
   it, and how much code does it need for the rest? What latency and bytes does SNC's layer add?

AR-1 does **not** decide:
- a durability contract (C-4 to C-6 are in S2);
- production dependencies;
- the language;
- the product gate (package 7.2).

## 2. Environment

- **Where.** **PROPOSED:** one isolated cloud VM (4 vCPU, 16 GB, local SSD), not the R640. cody chooses where the
  runs go (section 14).
- **Manifest.** A machine manifest goes under `bench/manifests/` before any run. That path and `experiments/ar1/`
  are the only paths AR-1 writes.
  - It records: CPU, RAM, kernel, filesystem and mount options, the PostgreSQL version and full configuration, the
    Python and DBOS versions, and every commit SHA (section 5.5).
  - Numbers are comparisons between two arms on one host, not benchmark claims.
- **PostgreSQL 18.** Two instances on the VM:
  - a primary and one physical streaming standby, with `synchronous_standby_names = 'FIRST 1 (s1)'`;
  - `synchronous_commit = on`, `fsync = on`, `full_page_writes = on`, data checksums, `wal_level = replica`;
  - the runtime role holds `pg_monitor`;
  - no Patroni.
- **Standby control.**
  - **Stop** means `SIGSTOP` of the standby's postmaster and walreceiver, so commits block inside COMMIT.
  - **Resume** means `SIGCONT`.
  - **Cut off** means restarting the standby with an empty `primary_conninfo` and no `restore_command`.
- **DBOS Transact Python**, pinned in the manifest (the package read 3.6.0 [EXT-SRC]).
  - Uses only workflows, steps, and transactional steps through a SQLAlchemy datasource (`@ds.transaction()`)
    [EXT: docs.dbos.dev, per review].
  - Isolation is pinned to READ COMMITTED for every SNC transaction, which E-10 step 1 needs.
  - `DBOS__APPVERSION` is pinned.
  - No Conductor and no Enterprise.
- **Stub model provider (local HTTP).**
  - It logs, durably before replying, the canonical request body (UTF-8 JSON with sorted keys), its hash, the SNC
    invocation id (sent as a metadata header) and the receive time.
  - It returns a deterministic response per (seed, input hash), and a request id in a header and in the first
    stream event.
  - It streams the response.
  - **PROPOSED latency:** 20 ms for crash runs and 500 ms for performance runs.
- **Fake tools (local HTTP).** Each logs every request durably before replying.
  - **T-I (idempotent):** a repeat key returns the first result. It offers a status query by key, which exercises
    E-10 step 11.
  - **T-F (fencing):** honours an (agent id, epoch) token per scope.
  - **T-N (non-idempotent):** every request is a new effect. It has no status query.
- **Blob store.** A content-addressed directory. Manifest items reference blobs by hash.
- **Clock.** All logs use one host monotonic clock (`CLOCK_MONOTONIC`, recorded with each entry).

## 3. System under test (S1 only)

All code lives in `experiments/ar1/` (Python, KISS, minimal comments).

| Component | Does |
|---|---|
| `snc_rt` | The S1 records of package 2.1, E-10 epochs and takeover, E-11 states, establishment (2.6 d), the state, acknowledgement and reconstruction APIs |
| Gateway | Runs in the app process. It is the only path to the stub and tools, and it holds their endpoints and credentials. Before any write of its own, it writes an **establishment decision log** entry: the bound, the `flush_lsn` and `sync_state` it read, and established, waiting or refused |
| Policy | A versioned static rule file. Policy decisions record the rule version |
| Approver (approval scenario only) | Scripted. It logs approve or deny to a file before answering |
| Owner (in-doubt resolver) | Scripted. It answers "happened" or "did not happen" by seed parity, after the escalation timeout. It logs its answer to a file |
| Recovery | Inside each send step: the step body first reads SNC's call record. If a dispatched attempt exists with no outcome, it does not send; it treats the call as outstanding (E-10 steps 9 and 11). DBOS's own recovery re-runs uncheckpointed steps at launch [EXT], so this guard lives in the step, not in a scan after launch |
| Fault injector | Named points. At a point it **blocks** for the seed's delay, writes an fsynced marker file (outside the database) naming the point and the run, then acts (`SIGKILL` of the app, pause, cancel or restart). A no-op when unset |
| Checker `ar1_check` | Written by a different implementer from `snc_rt`. Inputs: the stub, tool, approver, owner, gateway-decision and marker logs; the oracle's cuts (7.1); the database; `pg_waldump` of the run's WAL; and a common read interface both arms expose (section 7.4) |
| `plain_dbos` | The same scenario without `snc_rt`, by a third implementer (section 8) |

**Mapping to DBOS:**
- Task T is one DBOS workflow per epoch, with id `T:e<epoch>`.
- A takeover is the harness starting workflow `T:e<n+1>` for agent A while `T:e<n>` is paused at a point.
- Each consequential commit is a transactional step. Inside it, in order:
  1. lock the agent row first, with the epoch check;
  2. take `pg_current_xact_id()`;
  3. insert a commit token (UUID), with the activation's epoch and the commit kind (decision or outcome);
  4. check `current_setting('synchronous_commit')`.
- The commit happens when the step returns. The bound (`pg_current_wal_insert_lsn()` and
  `pg_walfile_name(pg_current_wal_insert_lsn())`) is read in the **next** step, the bound step.
- DBOS step checkpoints in its own system tables are not agent-visible authoritative state, and nothing gates on
  them [INF].
- DBOS step retries stay off for send steps. That is DBOS's default (`retries_allowed = False`) [EXT].

## 4. Scenarios

**Main scenario (S-M).** One agent A, one task T:

| Step | Action | Consequential commits |
|---|---|---|
| 1 | Accept T for A at epoch 1 | C-acc |
| 2 | Assemble three memory items (blobs) and call the stub | I-prep, I-disp, I-acc, I-out |
| 3 | Policy decision (no approval) | P-dec |
| 4 | Call T-I | TI-prep, TI-disp, TI-out |
| 5 | Call T-F | TF-prep, TF-disp, TF-out |
| 6 | Call T-N | TN-prep, TN-disp, TN-out |
| 7 | Write one memory object and complete T | M-done |

That is 16 commits.

**Approval scenario (S-AP).** As S-M, but step 3's rule requires an approval. Two commits are added after P-dec:
AP-req and AP-dec. The approver denies on odd seeds, and a denial ends T with no tool call.

**Allowed end states:**
- T completes; or
- T stops **only** for a T-N call with a dispatched attempt and no outcome, pending the owner.

Any other in-doubt stop is a violation of item 2.

**Timeouts (PROPOSED):**
- dispatch timeout 2 s, with a 2 s margin before a call becomes in doubt;
- escalation timeout 10 s before the owner answers.

## 5. Crash-point matrix (fixed)

### 5.1 Points (S-M)

**A. Commit points** for each of the 16 commits:

| Point | Where |
|---|---|
| a1 | At the end of the step body, before return (the row lock is held) |
| a2 | At the start of the bound step |
| a3 | The standby is stopped before COMMIT. Kill while COMMIT blocks, then resume the standby (when: 7.1) |
| a4 | After establishment, before the next action |

That is 64 points.

**B. Send points** for the model and the three tools:

| Point | Where |
|---|---|
| b1 | After establishment, before the send |
| b2 | After the receiver logged the request, before its reply arrives |
| b3 | After the reply arrives, before the outcome commits |

That is 12 points.

**C. Takeover.** `T:e<n+1>` starts while `T:e<n>` is paused. Where the new activation is held, it waits at named
point `post-takeover`: outside any transaction, after the takeover's bound step, so the new epoch is established and
the new activation holds no lock. A hold longer than 10 s (PROPOSED) makes the run UNRESOLVED.
- **C a1:** at a1 of each dispatched commit (I-disp, TI-disp, TF-disp, TN-disp), where the lock is held. The harness
  releases the old activation only after it sees the takeover waiting on that lock (`pg_locks`, marker
  `takeover-waiting`). This tests the lock and deadlock. That is 4 points;
- **C b2:** at b2 of each send, where the request is in flight. The seed decides whether the old reply is released
  before or after the takeover commits. That is 4 points;
- **C-p:** at a4 of each *-prep (I-prep, TI-prep, TF-prep, TN-prep). The takeover commits and should reject the
  prepared call. The new activation is held, and the old one is released and attempts its dispatch commit. That is
  4 points;
- **C-s (stale decision):** at a4 of each outcome (I-out, TI-out, TF-out, TN-out), in a fixed order:
  1. the old activation is alive, with its outcome committed under its current epoch and no open transaction;
  2. the takeover commits epoch n+1, and the harness writes marker `takeover-committed`;
  3. the new activation is held at `post-takeover`, so nothing it does can reject the old one first;
  4. the old activation is released and reaches named point `stale-decision` before its next consequential
     decision: P-dec after the model, TF-prep after T-I, TN-prep after T-F, M-done after T-N. It writes marker
     `stale-decision-attempt` (epoch, decision), then `stale-decision-rejected` with the reason the runtime
     reported, or `stale-decision-committed`;
  5. the new activation is released.

  That is 4 points. The row lock is never disabled at C-s. It is free by then, so only the epoch check can reject
  the decision. C-s sits at a4 of the outcome, not at b2: an outcome from a stale writer already shows the
  activation that it is stale, and a correct runtime may stop there, before any new decision (C b2 covers that
  path).

That is 16 points.

**D. Cancellation**, with no kill (the run continues to its end):
- at a4 of each *-prep, which is the E-1 race. That is 4 points;
- at b1, b2 and b3 of each tool. That is 9 points.

That is 13 points.

**E. PostgreSQL restart.** The standby is stopped and a dispatched COMMIT is blocking. Restart the primary with
`-m immediate` at the 4 dispatched commits, and with `-m fast` at TI-disp. Then resume the standby. That is 5 points.

**R. Recovery points:**
- a kill during the takeover commit itself;
- a kill during the recovery commits (result unknown at I-out, in doubt at TN-out, outstanding set at takeover).

That is 4 points.

### 5.2 Points (S-AP)

- a1 to a4 of AP-req and AP-dec (8 points);
- a cancellation between AP-req and AP-dec (1 point).

That is 9 points.

### 5.3 Totals and seeds

- **Total: 123 points.**
  - S-M: 64 + 12 + 16 + 13 + 5 + 4 = 114.
  - S-AP: 9.
- **Seeds. PROPOSED: 20 per point**, so 2,460 counted runs for SNC's arm.
- **What a seed varies:**
  - the block delay before the point acts (0-100 ms);
  - the release order in C b2;
  - the approver and owner answers by parity.

  For performance runs, stub and tool jitter is **off**.
- **Seed derivation.** Counted seeds are `sha256(frozen SHA set, point, i)`. Bring-up seeds use a different prefix,
  so the two never overlap.

### 5.4 Reached-point rule

- A run whose marker for its point is missing is **UNRESOLVED**.
- So is a run whose marker shows the wrong interleaving, for example in C the old epoch's send arriving at T-F
  before any newer-epoch request when the run needs the reverse.
- At C-s, `stale-decision-attempt` must follow `takeover-committed`, and the takeover's commit end LSN must be
  below that of any stale decision that committed.
- **Positive control (SNC's arm).** Without mutations, every C-s run must reach `stale-decision-rejected` with the
  epoch check as the reason, and `pg_waldump` must show no commit record for that transaction's xid.
  - Any other reason, such as a compare-and-set conflict or a lock timeout, means the schedule did not exercise the
    epoch check, and the run is UNRESOLVED.
  - `stale-decision-committed` without a mutation is not UNRESOLVED. It is an item 1 violation, judged as such.
  - In the plain arm, which has no epochs, C-s and C-p count as item 1 unmet (section 8).
- The report gives, for each clause of items 1-7, how many counted runs exercised it. A clause exercised by no run
  is UNRESOLVED.

### 5.5 Freeze before counted runs

- The SHAs of `snc_rt`, `ar1_check`, `plain_dbos` and the harness are pinned and recorded in the manifest.
- Any code change after that point voids the counted runs.

## 6. Phase 0: OI-1 and the durability checks (first, and a hard gate)

**Gate.** If any Phase 0 check fails or is UNRESOLVED, Phase 1 does not start. AR-1 ends with outcome **REFINE
(design)**, and package 2.6 is revised and reviewed again.

**Runs.** Each check runs 20 times (PROPOSED). Seeds do not change a deterministic mechanism, so each run varies:
- timing (load before the action, from 0 to 1,000 concurrent dummy writes);
- whether a checkpoint is forced before the action;
- the WAL position relative to a page boundary (padding writes).

`pg_promote` returning false is an **invalid** run, which is repeated, not counted.

| Check | Procedure | Pass condition |
|---|---|---|
| **OI-1a physical vs authoritative** | Create unestablished rows by C-1 (`SET LOCAL synchronous_commit TO off`, and separately `local`, standby stopped), C-2 (cancelled wait) and C-3 (restart). Then: query the state, acknowledgement and reconstruction APIs; run the policy gate; have the gateway decide a dispatch whose decision only *reads* those rows | The rows exist physically. No API returns or acknowledges them as authoritative. A cut taken while the standby is stopped, before the API queries, classifies them L (7.1). No stub or tool entry appears, and the WAL holds no dispatch commit for the gated attempt below the primary's insert LSN read just before resume. A gateway-decision log that disagrees (anything but "waiting" or "refused" before any write of its own) is a violation; one that agrees proves nothing. An `off` commit lost in C-3 has neither row nor token, which is accepted. After resume, they become authoritative and the dispatch proceeds. **Control run:** the same with the gate disabled must produce a violation, otherwise OI-1a is UNRESOLVED |
| **OI-1a-v establishment variants** | Revoke `pg_monitor` from a second role and read; set the standby's `application_name` so its `sync_state` is not `sync` | NULL LSNs count as not established. A non-`sync` standby's flush counts for nothing |
| **OI-1b timeline id** | Candidate: `pg_walfile_name(pg_current_wal_insert_lsn())`, first 8 hex digits, read in the same query as the bound by the runtime role. Cross-check: `IDENTIFY_SYSTEM` over a replication connection. `pg_control_checkpoint().timeline_id` is **rejected a priori**: promotion only requests a checkpoint, so the value lags by design [EXT-SRC: xlog.c, pg_controldata.c, per review]. Promote the standby (cut off first), with and without a checkpoint, and across a restart. Include a session opened on the standby before promotion | The candidate reports the new timeline on the first read after `pg_promote` returns, in every run. It agrees with `IDENTIFY_SYSTEM`. On the pre-promotion session it errors during recovery and is correct after promotion. Any lag or disagreement fails OI-1b |
| **OI-1c identity after promotion** | Stop the standby. Commit X on the primary with `SET LOCAL synchronous_commit TO local`, recording its xid and commit token. Stop the old primary. Start the standby cut off, then promote it. Run **writing** transactions until `pg_current_xact_id()` passes X's xid. Variant: X's xid assigned early and logged in a running-xacts record before the cut-off. Rebuild the pair with `pg_basebackup` after each run | Token lookup reports X not committed, every run. Record as results, not gates: `pg_xact_status(X)` before the counter passes X (an expected "in the future" error) and after it (committed, aborted or in progress), and whether the reissuing transaction committed |
| **OI-1d unknown commit** | **Deterministic case:** stop the standby, so COMMIT blocks; kill the client; then recover. Also random kills after COMMIT is sent and before the bound is read | Recovery waits for `pg_xact_status(xid)` to leave `in progress`, or terminates the backend and waits for it to exit, before the token lookup (package 2.6 d7). The lookup never says "not committed" for a transaction that later commits. Every recovered bound is ≥ the commit record's end LSN |
| **OI-1e oracle self-test** | (1) With the standby stopped, commit with `SET LOCAL synchronous_commit TO local` and take a kill cut; a commit with `on` made before the stop is in the same run. Kill the client, resume, take a final cut after catch-up. Then apply the old rule: `pg_last_wal_receive_lsn()` read after the catch-up. (2) Classify OI-1c's runs for X. (3) Freeze and thaw 1,000 times (PROPOSED) under streaming load, reading `flushed_lsn` twice 100 ms apart inside each freeze | (1) The `local` commit is L at the kill cut and R at the final cut. The `on` commit is D and passes the presence check. **Negative control:** the old rule calls the `local` commit established at the kill. (2) OI-1c's X commit is X, and every commit at or below the switchpoint survives. (3) The two reads inside each freeze are equal. Every load commit that returned before a freeze is D at that cut. At least one commit is L at a cut and R at the final cut. Every cut has the primary's reported flush at or below the standby's. After each thaw the walreceiver keeps its pid and replication resumes within 5 s (PROPOSED). Any failure, or a negative control that does not fire, is an instrument failure: the outcome is UNRESOLVED until the oracle is fixed and re-reviewed, not REFINE (design) |
| **C-1, C-2, C-3** | As in package 2.6 (e). C-3 runs with both `-m immediate` and `-m fast` | As defined there, including C-1's liveness. For C-2, the client's COMMIT success with only a WARNING is **not** treated as established |
| **C-7 bound** | 10,000 commits each under `on`, and under `off` with `wal_writer_delay = 10s`. Each run also reads `pg_current_wal_lsn()` in the same query as the bound. Find the commit record with `pg_waldump --rmgr=Transaction --xid=<xid>`, and its end as the next record's start in an unfiltered dump | The bound is ≥ the commit end in every case. **Negative control:** `pg_current_wal_lsn()` falls below the commit end in some `off` runs, which shows the risk was exercised; otherwise C-7 is UNRESOLVED. Report establishment latency, and the count of bounds that land past a page boundary |

OI-1 uses a manual `pg_promote` on one VM. It tests mechanisms, not Patroni (C-4 to C-6 stay in S2). A same-host
standby is sufficient for this [INF, per review].

## 7. Correctness criteria and checker (SNC's arm)

### 7.1 Established, defined independently (the durability-cut oracle)

The oracle is harness code by the checker's implementer, outside the app process. Its records are fsynced files.
It never reads the gateway-decision log. Establishment in package 2.6 means the sync standby's *reported* flush, and
that report comes from the standby's published flush position. So the oracle reads that position while it cannot
move.

- **Freeze** means `SIGSTOP` of the standby's walreceiver only. Its pid is read from `pg_stat_wal_receiver` at each
  freeze, since it changes after E. **Thaw** means `SIGCONT`. While frozen, the standby can neither receive nor flush
  WAL: the startup process writes none, and there is no `restore_command` [EXT-SRC: PostgreSQL 18, per review].
- **Connections.** The oracle has one connection to the standby and one to the primary, opened at run start with
  `pg_monitor`. They need no new backend, so they work while the standby is stopped (a3, E). After a primary
  restart they reconnect. Any read is retried for up to 10 s before it counts as failed.
- **Cut.** A record of, in this order:
  - `flushed_lsn` and `received_tli` from `pg_stat_wal_receiver` on the standby. The walreceiver sets this only after
    its fsync [EXT-SRC: walreceiver.c `XLogWalRcvFlush`, per review]. When no walreceiver runs, which happens for a
    few seconds after E, `pg_last_wal_receive_lsn()` is read instead. It cannot move then, and the oracle checks
    that no walreceiver pid appeared during the read;
  - the primary's `pg_stat_replication` `flush_lsn`, `sync_state` and walsender pid for the standby. A non-NULL
    `flush_lsn` above the standby's value makes the run UNRESOLVED. Counted runs never restart the standby, which is
    the one case where this can legitimately happen;
  - the time on the shared clock.
- **Kill cut**, at a kill or restart. At the fault point, after the marker:
  1. freeze, unless the standby is already stopped (a3, E);
  2. take the cut and fsync it;
  3. `SIGKILL` the app, or restart the primary in E;
  4. thaw; at a3 and E, resume the standby as the item 1 observation below says;
  5. once each of the killed app's backends has exited or waits in `SyncRep` (`pg_stat_activity`), and before the
     app restarts, read the primary's `pg_current_wal_insert_lsn()`: the **kill boundary**.

  Between steps 1 and 3 the standby cannot advance, so the cut is exactly what it had published at the kill. A
  later receive position is never used for this. Each kill in a run gets its own kill cut (R has two).
- **Takeover cut.** At each C point, a cut (freeze, cut, thaw) just before the takeover starts. It serves as the
  kill cut for item 1. The kill boundary is the insert LSN read at the same time.
- **Final cut.** After the run, with the app stopped, the oracle waits until the standby's `flushed_lsn` reaches the
  primary's `pg_current_wal_flush_lsn()`. Then it takes a cut. If catch-up takes more than 30 s, the run is
  UNRESOLVED.
- **Commit positions.** Each commit token records its xid, the activation's epoch, and its kind (decision or
  outcome). The checker maps the xid to its commit record's end LSN and timeline in `pg_waldump` of the primary's
  WAL (method as in C-7). `wal_keep_size` is 8 GB on both servers (PROPOSED), so no segment a run needs is recycled.
  A token whose commit record cannot be found makes the run UNRESOLVED.
- **Classes**, for every commit token:

| Class | Meaning | Rule |
|---|---|---|
| **D** | Durable on the standby before the kill | end LSN ≤ the kill cut's flushed position, on its timeline. Presence check: the record is in the standby's own WAL (`pg_waldump` of its `pg_wal` after the run) |
| **L** | Committed locally, not established at the kill | end LSN above the kill cut and at or below the kill boundary. The killed client's backend may have written it after the client died |
| **R** | Established only after recovery or once replication resumed | an L commit, or a commit above the kill boundary (made after the kill), whose end LSN is at or below a later cut on the same timeline |
| **X** | Lost across a timeline change | in the old timeline's WAL, end LSN above the switchpoint that the new timeline's `.history` file gives for the old one |

- In counted runs no timeline changes. A timeline change, or a commit not D or R at the final cut, makes the run
  UNRESOLVED. X is exercised only in Phase 0 (OI-1c, OI-1e).
- **Receiver-side read.** The stub and each fake tool, on receipt and before logging, read the primary's
  `pg_stat_replication` `flush_lsn`, `sync_state` and walsender pid for the standby, and the timeline. They use their
  own `pg_monitor` connection, with the same reconnect and retry, and log the read with the entry.
  - The reported flush never exceeds WAL that is durable on the standby. So a `sync` read at or above a commit's end
    LSN proves the commit was established before the read.
  - Within one walsender, the value only grows. A read below the end LSN is therefore a violation, unless the
    walsender pid changed during the run. Then the read cannot decide the order, and the run is invalid.
  - A NULL read, a failed read, or one with `sync_state` other than `sync` also makes the run invalid.
- **Item 1 observation.** Right after the first state-API answer after the last recovery returns, the oracle takes
  a cut. Nothing above that cut can have been established before the answer.
  - At a3 and E, the standby stays stopped until that answer returns, or until 5 s (PROPOSED) after recovery
    starts, whichever comes first. Otherwise it catches up in milliseconds, and an unestablished row would never be
    present at the answer.
  - Where it still never is, OI-1a exercises item 1's upper clause, and that clause is exempt from the 5.4
    unexercised rule.
- **Invalid runs.** A freeze that lasts over 1 s (PROPOSED), or an oracle read that fails after its retries, thaws
  at once (resumes, at a3 and E) and makes the run invalid. The same applies to a receiver-side read as stated
  above.
  - An invalid run is repeated **with the same seed**. Every invalid run is counted and reported.
  - A violation already observed in an invalid run still counts.
  - More than 1 invalid run among a point's 20 makes that point UNRESOLVED.

### 7.2 Items (zero violations across all counted runs)

| # | Pass condition |
|---|---|
| 1 | The first state-API answer after the last recovery (at C, the new activation's first answer; at D, the final state) is the state after a prefix, in commit-LSN order, of the run's commits. The prefix includes every commit that is D at the last kill or takeover cut (7.1), and no commit above the cut taken right after the answer. Between the two, R commits may be in or out. **SNC's arm:** no decision commit (E-10 step 3) carries a stale epoch. That is, by the token's kind and epoch and the end LSNs in `pg_waldump`, no decision commits with an epoch below that of a takeover whose commit is lower in the WAL. A stale-writer outcome is allowed (E-10 step 4), and the current epoch derives its dependent transitions |
| 2 | T reaches an allowed end state (section 4). No completed step's effect repeats in the tool logs, by logical call. No committed step output is lost |
| 3 | The whole-input hash equals the hash of the canonical body the stub logged for that invocation id. States follow E-11. After a b2 or b3 crash, the invocation is result unknown, or carries the late output marked late. A re-call is a new invocation id with `retry_of`. No uncommitted output is ever presented as received |
| 4 | No denied call appears in any tool log. Every stub and tool-log entry maps to a dispatched attempt, with its policy decision (and approval in S-AP), whose dispatched commit was established before the entry: the receiver-side read (7.1) is at or above that commit's end LSN on the same timeline. A gateway-decision log that disagrees is a violation; one that agrees proves nothing |
| 5 | **T-I:** one effect per logical call. Retries are new attempts with the same key. No re-send with a new key. **T-N:** never retried automatically. Each entry maps to exactly one attempt. When the owner answers "did not happen" and the call is re-dispatched, a late duplicate is recorded as a duplicate. **T-F:** a stale request after a higher epoch is rejected and recorded as rejected. No tool entry lacks a record. While an older call is outstanding, the new epoch sends nothing to that scope, except to T-F |
| 6 | Items 1-5 hold at every point. Takeover and cancellation reject prepared calls, abandon prepared invocations and record their outstanding sets. No deadlock occurs, especially at C a1. In-doubt calls have an owner and are escalated at the timeout. T-I's outstanding calls are resolved by its status query |
| 7 | For every effect and transition, reconstruction returns the fields in 7.4, each matching its source of truth |

### 7.3 Checker adequacy (before G3)

Each mutation runs only at the points where it is observable. All 5 seeds at each such point must be flagged.

| Mutation | Observable at |
|---|---|
| M1 send before establishment | a3 of each *-disp |
| M2 decision commits without the epoch check (the row lock stays) | C-s. At C-s of T-I and T-F the old activation goes on from its stale prepare to a stale dispatch and send, and the checker must report that dispatched record too |
| M3 lost output regenerated as the original | b2 and b3 of the model |
| M4 automatic T-N retry | b2 and b3 of T-N |
| M5 a policy read of an unestablished row | a3 of C-acc and of the memory reads, with P-dec following |
| M6 takeover without rejecting prepared calls | C-p (item 6: the takeover commit holds no rejection of the prepared call) |
| M7 reconstruction drops an attempt | b2 of T-I |
| M8 outcome blocked by the epoch check | C b2, on seeds that release the old reply after the takeover |
| M9 re-send with a new key | b2 of T-I |
| M10 epoch check removed from policy and memory decisions only (P-dec, M-done) | C-s of the model and of T-N |
| M11 no outstanding set recorded | C a1, C b2, and D b1 to b3 |
| M12 re-call without `retry_of`, or late output not marked late | b2 and b3 of the model |
| M13 dependent transitions not derived from a stale outcome | C b2, on seeds that release the old reply after the takeover |
| M14 conflict rule ignored (E-10 step 9) | C b2 of T-N, with the old reply held until the new activation commits TN-disp or reaches the escalation timeout |

Under M2 and M10 the C-s positive control turns into `stale-decision-committed`. The checker must flag the run from
the database and `pg_waldump` alone (item 1's stale-epoch clause), not from the markers. A mutation counts as flagged
only when the checker reports the clause it breaks. C a1 is judged on correct runs only; no mutation targets it.

A missed mutation makes AR-1 **UNRESOLVED** until the checker is fixed and re-reviewed. The checker gets a three-way
review before adequacy runs.

### 7.4 Common read interface and reconstruction fields

Both arms expose two calls:
- `state(agent)`;
- `reconstruct(effect)`, which also returns each consequential commit's xid, so the oracle classes apply to
  both arms.

Reconstruction returns these fields, each checked against its source of truth:

| Field | Source of truth |
|---|---|
| shown | Manifest items and whole-input hash, against the stub log and the blob store |
| authorized | Rule version, against the policy file; approval, against the approver log |
| executed | Every attempt and invocation state, against the tool and stub logs and their receiver-side reads (7.1). The gateway-decision log is a consistency check only |
| committed | Transitions, against commits that are D or R at the final cut (7.1) |

`plain_dbos`'s adapter for this interface counts toward its lines.

## 8. Item 8: the plain DBOS arm and differentiation

- **Implementer.** A third implementer works from section 4, items 1-7 and DBOS's documentation, using DBOS idioms:
  workflows, steps, transactional steps, workflow ids as keys, and an app table.
- **Step 1.** The arm as an application would normally write it. It is run over the full matrix and judged by the
  same checker.
- **Step 2.** Code is added until items 1-7 pass over the full matrix.
- **Missing points.** A matrix point that does not exist in the plain arm, such as C without epochs, means that item
  is **unmet** until code is added to meet it.
- **Lines.**
  - Format with one fixed formatter (`black`, pinned).
  - Count logical non-blank, non-comment lines of the whole diff from step 1 to step 2, once.
  - Reviewers may submit shorter correct versions, and the minimum counts (invariant 9).
  - `snc_rt`'s own size is reported alongside.
- **Differentiation threshold L.** **PROPOSED: 300 lines.**
- **Determinism divergence.** In both arms, with `DBOS__APPVERSION` pinned, reorder two steps between a b3 crash and
  its recovery. Record whether DBOS reports it, re-executes silently, or fails. This is reported, not gated.
- **Latency budget B.**
  - Runs: performance runs, jitter off, 1,000 tasks per arm, arms interleaved (A B A B), stub at 500 ms.
  - Net time per task is the task time minus stub and tool time measured client-side.
  - B at p50 is (SNC p50 net − plain-step-2 p50 net) ÷ SNC commits per task.
  - B at p99 is the same formula at p99.
  - **PROPOSED:** B at most 10 ms at p50 and at most 50 ms at p99.
  - Also reported: commits per task, WAL bytes per task (`pg_stat_wal`), and blob bytes per task.

## 9. Outcome

Violations are judged on SNC's arm. Plain step 1 failures are expected, and they are data. The outcome is the first
that applies, in this order:

1. **UNRESOLVED:**
   - any cap hit;
   - a missed mutation;
   - a missing marker or wrong interleaving;
   - an unexercised clause;
   - an unexplained run: a run the checker cannot classify, or one where the harness crashed;
   - any run or point declared UNRESOLVED in sections 5.4, 6 or 7.
2. **STOP:**
   - a correctness item fails on SNC's arm and package 3.5 (configuration, adapter, another engine) cannot fix it;
   - or B fails by more than a factor of 2.
3. **REFINE:**
   - Phase 0 fails (REFINE, design);
   - items 1-7 pass only after a schema or adapter change;
   - or B fails by at most a factor of 2.

   Each is a new revision, reviewed again.
4. **NARROW:** plain DBOS step 2 meets items 1-7 with zero violations over the full matrix, in L lines or fewer.
5. **GO:**
   - Phase 0 passes;
   - items 1-7 pass on SNC's arm;
   - B holds;
   - plain DBOS needs more than L lines.

   GO means S1 is worth building on these candidates. Nothing more.

## 10. Caps and time estimate (PROPOSED)

- **Per run:** 5 minutes. The package proposed 30 minutes; 5 is enough for the 20 ms stub and the 14 s timeouts
  [INF].
- **Per phase and arm.** About 15 to 20 s per run including the app restart and the oracle's cuts, and about 3 minutes per OI-1c rebuild [HYP]:

| Phase | Runs (about) | Cap |
|---|---|---|
| Phase 0 | 420, plus 40 rebuilds | 8 h |
| Checker adequacy | about 400 | 6 h |
| SNC arm | 2,460 | 16 h |
| Plain step 1 | 2,460 | 16 h |
| Plain step 2 | 2,460 | 16 h |
| Performance | 2 × 1,000 tasks | 4 h |

A phase that hits its cap reports what completed. The rest is UNRESOLVED.

## 11. Product-gate thresholds (package 7.2, OD-P5), proposed for cody before any results

| Gate step | PROPOSED threshold |
|---|---|
| 1. Working runtime on this project (needs its own authorization) | Two weeks. At least 90% of consequential actions in that window are answerable from records alone: who asked, what was shown, which rule allowed it, what happened |
| 2. Comparison | L applies only to the measured plain DBOS arm (section 8). Temporal, LangGraph, an OpenTelemetry trace stack and an OPA/MCP gateway are compared qualitatively, item by item |
| 3. Buyer interviews | At least 5 potential buyers in at least 3 organizations |
| 4. Function beyond the alternatives | At least 2 buyers name a need their current stack cannot meet and that S1 as built does. S3-only needs are recorded but do not count |

Failing any step is NARROW or STOP.

## 12. Out of scope

- **S2:**
  - Patroni and C-4 to C-6;
  - standby power loss;
  - concurrency beyond the takeover pair;
  - 1M timers;
  - long-run vacuum, freeze and WAL;
  - the extra fault cases in package 7.1.
- **S3:** anchors, multi-tenancy, retention, erasure and access audit.
- **Excluded entirely:**
  - building on Temporal;
  - any C++ or Rust;
  - any write outside `experiments/ar1/` and `bench/manifests/`;
  - the R640.

## 13. Assumptions and limits

- **AS-1.** The stub, tools, approver and owner log durably before acting.
- **AS-2.** The reached-point markers (5.4) replace the assumption that a point was reached.
- **AS-3.** DBOS Python allows custom SQL in `@ds.transaction()` steps at READ COMMITTED, and recovery re-runs
  uncheckpointed steps [EXT; verified during bring-up. If false, the result is REFINE].
- **AS-4.** A same-host standby is a valid mechanism test for Phase 0. It is not a durability claim.
- **AS-5.** Results from the VM compare the two arms. They are not benchmark claims.
- **Limits:** one agent, one task, single tenant, and an idle primary between steps. The page-boundary latency
  effect is measured in C-7, not hidden.
- **Oracle limits.**
  - The cut is the standby's *published* flush, which can trail its fsync slightly. A commit already on the
    standby's disk can be called L. That errs toward "not established", which is what the runtime could see.
  - The receiver-side read comes just after receipt. A send that precedes establishment by less than that read's
    latency is not caught by item 4. While the standby is stopped or frozen there is no such window.
  - Item 1's second cut comes just after the answer, so an answer that includes a commit established in that gap is
    not caught.
  - A stop can land while the walreceiver holds its spinlock. The oracle's read then spins until the thaw. That is
    why a read over 1 s thaws and invalidates the run.
  - E with `-m fast` and the standby stopped waits up to `wal_sender_timeout` (60 s) for the standby's flush
    confirmation [EXT-SRC, per review]. That costs time but stays inside the per-run cap.

## 14. What cody decides

1. **G1 and G2.**
   - Approve this document.
   - Allow AR-1 code under `experiments/ar1/`, plus the manifest.
   - Allow uncounted bring-up (at most 2 seeds per point, reported, then discarded) and the checker-adequacy runs.
   - No production dependency, durability contract, 7.2 demo, or S2 or S3 work.
2. **G3, later.** A separate GO for counted runs, after the bring-up and adequacy report.
3. **Set the PROPOSED numbers before G3:**
   - seeds (20);
   - caps (section 10);
   - stub latencies;
   - timeouts (section 4);
   - B (10 ms p50 / 50 ms p99);
   - L (300);
   - the section 11 thresholds;
   - the oracle's limits in 7.1 and 5.1 C: the 10 s retry, the 1 s freeze, the 10 s hold, the 5 s standby hold,
     1 invalid run per point, the 30 s catch-up, 8 GB `wal_keep_size`, and OI-1e's 1,000 freezes and 5 s resume.
4. **Where the runs go:**
   - the isolated cloud VM proposed in section 2;
   - a cloud session;
   - or cody's own computer.

## 15. Independent review

Three reviewers were told to break the first draft, and they raised 48 findings. All are applied in revision 1.

**Matrix and checker** (Opus, 22 findings plus 3 nits). Six were blocking:
- a3 and E were not real waits, because COMMIT already waits under FIRST 1. The standby is now stopped first.
- Nothing proved a point was reached. Markers were added (5.4).
- The E-10 step 9 conflict rule was untested. An owner, timeouts, the duplicate path and M14 were added.
- The GO clause "cannot meet at all" was removed.
- Missing plain-arm points now count as unmet.
- Mutation observability is now listed per point.

Others:
- block-then-kill;
- an independent definition of "established" (7.1);
- takeover at a1 and b2;
- cancellation at the prepare race;
- mutations M9 to M14;
- T-I retry scope;
- the shared clock;
- the canonical body;
- reconstruction fields and their sources;
- the common interface;
- a robust line count;
- B's formula;
- outcome precedence;
- caps per phase;
- pinned SHAs;
- the gateway in-process, and a status query on T-I.

**Phase 0 against PostgreSQL 18 source** (Opus, 11 findings). Three were blocking:
- A token lookup during a blocked COMMIT says "not committed" for a transaction that later commits. Recovery now
  waits for the transaction to finish (also fixed in package 2.6 d7).
- OI-1a would pass with no gate, because every commit blocks. A decision log before any write and a control run
  were added.
- OI-1c had no executable procedure. The cut-off, promote and rebuild sequence was added.

Others:
- writing transactions only, plus the running-xacts variant, and package 2.6 d5 made token-only;
- `pg_control_checkpoint` rejected a priori;
- the timeline must be readable atomically with the bound;
- the pre-promotion session case;
- C-7's negative control and `pg_waldump` method;
- page-boundary latency;
- the `pg_monitor` and `sync_state` variants;
- the restart modes;
- C-2's success with only a WARNING;
- what varies between runs.

**Scope, decisions and DBOS fit** (Sonnet, 15 findings):
- DBOS recovery re-runs uncheckpointed send steps, so the guard now lives in the step;
- `@ds.transaction()` with isolation pinned;
- the bound is read in the next step;
- the takeover and app-version mechanisms;
- the gate wording (G1, G2, G3);
- caps;
- the package status line;
- kill points during takeover and recovery;
- the manifest path;
- the policy-gate freeze note;
- the STATUS reading pointer;
- L applies only to the measured arm;
- B per commit;
- the run-cap note;
- labels;
- the cut-off method;
- the blob store.

The approval variant moved out of the main matrix into S-AP, so that every point of S-M keeps all 20 seeds.

**Revision 2** follows Codex's HOLD review of PR #184 (`docs/handoff/PR184_Codex_Review.md`). It changes only:
- **Oracle (7.1).**
  - The old rule read the standby's receive position after resuming it, which can overstate the cut.
  - The cut is now taken with the walreceiver frozen before the kill. A kill boundary from the primary sorts
    commits into D, L, R and X.
  - Item 4 uses a read by the receiver, instead of the gateway-decision log.
  - Item 1 has a prefix rule between two cuts.
  - OI-1a is decided by the oracle, and OI-1e tests the oracle, with a negative control.
- **M2 (5.1 C, 5.4, 7.3).**
  - At C a1 the old activation holds the row lock, so a takeover cannot commit first, and M2 could not show
    anything.
  - The new C-s schedule commits the takeover while the old activation is alive, then lets it attempt its next
    decision. It has named points, markers and a positive control.
  - C-p gives M6 a point with a prepared call, which no C point had.
  - M8, M10, M11, M13 and M14 now name the points where they show.
  - The totals become 123 points and 2,460 runs per arm.

A focused three-way review was told to break revision 2 (an oracle reviewer and an M2 reviewer on Opus, and a
checker-and-outcome reviewer on Sonnet). It raised 36 findings, 5 of them blocking. All are applied, except two that needed no
change (I-acc's role, and a dispatch timeout under a held reply, which C-s no longer has). The main ones:
- the stale outcome at b2 could end the old activation before its decision, so C-s moved to a4 of the outcome;
- nothing recorded the epoch of a policy or memory decision, so the commit token now carries its epoch and kind;
- the positive control would have turned a real stale commit into UNRESOLVED, and it would have blocked the plain
  arm;
- an OI-1e failure would have meant REFINE (design), so it is now an instrument failure;
- L mixed commits from before and after the kill, so a kill boundary was added;
- the reported flush restarts at every new walsender;
- E runs lost their connections;
- WAL retention was unset;
- OI-1e did not test WAL in flight;
- item 1 had no cut at C and D;
- the invalid-run rule could discard a seed;
- M2b was satisfied by M6 alone, so it was dropped, and M2 now continues to a stale dispatch.

**Unresolved:**
- how often a stop lands inside the walreceiver's spinlock (measured in OI-1e);
- whether the harness can keep every freeze under 1 s from the in-app marker (bring-up);
- whether a reconnecting walreceiver whose replay lags can report a value that crosses a commit's end LSN (an
  invalid run if it happens);
- whether DBOS surfaces an epoch rejection that the runtime can tell apart from a compare-and-set failure (AS-3);
- whether DBOS Python's `@ds.transaction()` allows everything section 3 needs (AS-3, checked during bring-up);
- the time estimates in section 10 (HYP);
- whether any provider-side id lookup exists for E-11 (not tested here; the stub only).
