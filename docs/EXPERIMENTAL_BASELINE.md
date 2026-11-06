# Experimental Baseline: readiness review

**Current status (2026-11-06, review of PR #53,
`handoff/PR53_Review_Handoff.txt`):**

- **CORRECTNESS BASELINE: READY FOR CAMPAIGN PREPARATION.** The fixes
  and dispositions are approved: B1, B2 and B3 (PR #49), the delayed
  READY/SYNCED window (PR #50), F8/F9 (PR #51), the witness hardening
  (PR #52) and its corrections (PR #53).
- **FULL EXPERIMENTAL BASELINE: NOT YET FROZEN.** Section 11 still
  needs: the main R640's manifest (and the remote R640's before any WAN
  run), the first-wave drivers and harnesses, the within-system pilot,
  `CRITERIA.md`, and the exact frozen SHA.
- **Next:** campaign preparation
  ([CAMPAIGN_PREP.md](CAMPAIGN_PREP.md)). No comparative benchmarks.

The record below is kept as written at each review.

**Status: APPROVED WITH CORRECTIONS** (review of PR #45,
`handoff/PR45_Baseline_Review.txt`). The corrections are applied below.

- **Authorized next:** the pre-baseline tests (T-Q1, T-Q2, T-Q3, T-Q4,
  T-Q5, T-Q10), the harness guards (T-H), the instrumentation they need,
  and X1 as its own minimal slice.
- **Not authorized:** the comparative campaign. It waits for the freeze
  (section 11).
- **A blocker found by these tests is not fixed automatically.** It is
  characterized and its evidence kept, and the smallest fix is proposed
  for review.
- **Results (2026-11-05): [BASELINE_READINESS.md](BASELINE_READINESS.md).**
  T-Q1, T-Q2, T-Q4, T-Q10 and T-H pass. Three Profile B BLOCKERS were
  reproduced (**fixed 2026-11-05 and approved: BASELINE_READINESS section
  10**): B1, a checkpoint wedges the primary when
  its standby dies (T-Q3); B2, the witnesses promote a standby that is
  still catching up after a rejoin (T-Q5); B3, a catch-up that keeps
  dropping its link makes the primary read the remaining WAL range into
  memory once per attempt (T-Q5 under TSan). T-Q1 also found that section
  7's 35 s bound does not hold while a sync primary is blocked by a
  control-plane outage (F2; the R11 contract and its run-validity guard
  are in section 7).

- **Requested by:** cody's review of PR #44
  (`handoff/PR44_S1_and_Experimental_Baseline_Handoff.txt`).
- **Part of:** [EXPERIMENTAL_VALIDATION.md](EXPERIMENTAL_VALIDATION.md).
- **Purpose:** define exactly which SNC configurations and correctness
  contracts the falsification campaign may use, and what must be true
  before the baseline is frozen.

Labels as in [CORE_CONTRACTS.md](CORE_CONTRACTS.md):

- **MEASURED** / **OBSERVED:** a test or run shows it.
- **INFERRED:** read from code (EL0).
- **PROPOSED:** for review.

Triage classes, as the work order defines them:

| Class | Meaning |
| --- | --- |
| **BLOCKER** | Must be fixed before the baseline: it invalidates a declared experimental contract |
| **EXCLUSION** | Configuration exclusion: no fix is needed if that mode is not used |
| **TEST REQUIRED** | Likely safe, but must be tested before the campaign relies on it |
| **NON-BLOCKING** | A known limit that may remain if it is explicitly excluded from claims |

**Summary.** As reviewed, no item is a BLOCKER for the accepted profiles
A, B and C (D is optional and may be omitted). Several items become
BLOCKERS if their tests reproduce a hazard:

- R08 is handled by excluding automatic async failover (section 6).
- R11 is conditionally non-blocking. T-Q1 must prove that the shipped
  client's largest resend age stays well below session expiry on every
  campaign retry and failover path. If any path can resend near or past
  expiry, R11 is a BLOCKER (section 7).
- Seven tests are mandatory before the freeze (section 5).
  - T-Q3 is a Profile B BLOCKER if checkpoint or recovery keeps the
    primary from obtaining DEGRADE.
  - T-Q4 is a resource-safety defect if a plausible control-plane outage
    can exhaust the process.
- **X1 is approved (option b):** a minimal secondary-index query on the
  wire, as validation-enabling work (section 4). **Implemented
  2026-11-05:** wire kind `INDEX` (section 4, X1).

## 1. Candidate baseline commit and configuration

**Candidate code:** `fd0ba42` (main after PR #44). It includes:

- placement v2 with fail-closed serving (PR #40);
- legacy-role hardening (PR #41);
- HEALTH witnesses (PR #37);
- S1 bounded staging (PR #44).

**The frozen commit will differ from the candidate** in only two ways:

- the pre-baseline tests of section 5, which are test, harness and
  instrumentation only;
- any BLOCKER fix that those tests force.

Each of these is a separately reviewed slice. The frozen commit's SHA is
recorded in `bench/results/<campaign>/CRITERIA.md` (section 11).

**Fixed configuration** (every profile, unless the profile says
otherwise):

| Setting | Value | Why |
| --- | --- | --- |
| Build | RelWithDebInfo, the repository's CMake defaults | What every published number used |
| `--shards` | Physical cores given to SNC on the host, one per core. The exact list is recorded from the manifest (`lscpu -e`), never guessed | Owner-per-core is the thesis |
| `--group-commit-us` | 50 (default) | Default; a sweep may vary it only as a named parameter |
| `--pages` | 4 KiB (default) | THP gains apply to the baselines equally (HUGEPAGES.md) |
| `--move-units` | `on` (default since PR #44); `off` only as a named comparison | S1 is part of the baseline |
| Session TTL | 3600 s (the built-in default; no flag changes it) | Not raised to hide R11 (section 7) |
| `--checkpoint-s` | A named profile parameter. Default for the campaign: **60** | Recovery-time results need checkpoints, and sessions expire only at checkpoints (section 7) |
| Data directories | Fresh for every run; never a directory from another build or run | Removes legacy-role and wrong-directory cases (Q9) |
| Clocks | NTP-disciplined, slew only after boot (no step), recorded in the manifest | R11 horizon (section 7); standby read lease (C5) |
| Witnesses | 3, polling `HEALTH` (the default `--health-verb health`), each on a different host where the topology allows | B-2/B-3; a `STATUS` witness reintroduces the false failover (matrix row 11) |
| Client | The shipped `net::Client` / `ClusterClient`, sessions on, `failover_ms` 30000, `request_timeout_ms` 5000 (the bench defaults) | The retry contract is stated for this library (section 7) |

## 2. Benchmark profiles and their semantics

Each profile is a contract. A result is reported under exactly one
profile, and is valid only if the run stayed inside that profile's
contract (section 9's run validity rules).

### Profile A: LOCAL_DURABLE

**Configuration:** `--data-dir`, no standby, no witnesses. Optional
`--cluster` or `--control` for multi-group runs without replication.

| Property | Contract |
| --- | --- |
| What OK means | Applied and crossed this node's persistence boundary (`RWF_DSYNC` group commit): **LOCAL_DURABLE**. A cross-shard or cross-group transfer's OK is the durable debit plus a durable obligation, **not** SETTLED |
| Process crash | Survives: every OK is recovered (`test_crash`, EL3) |
| Host loss or disk loss | **Not survived**: the only copy is on that host |
| Power loss | **UNPROVEN** (H3): depends on the storage honoring `RWF_DSYNC`. The manifest records the device and its write-cache setting |
| Replication | None |
| Failover | None. Recovery is a restart; its duration is a measured result |
| Retry | At most once for sequenced mutations resent within the retry horizon (section 7). Deletes, reads and `unlogged` ops are not deduplicated (CORE_CONTRACTS 7) |
| Read consistency | A default single-object read is answered only once the object's last write is durable, and reflects every write to that object acknowledged before it was sent (R-1, R-2, AS-BUILT). Nothing across objects; a transfer's debit can be seen before its credit |
| Migration | Not used (no replication groups to move between) unless the run is a named migration run on unreplicated groups |
| Known exclusions | Host or disk loss; power loss; latent media corruption (Q8); `allow_undurable` reads; `Durability::APPLIED` writes |

### Profile B: SYNC HA

**Configuration:**

- every group is a primary with one sync standby (`--replica-of`,
  without `--async-replica`; `--replica-timeout-ms` 2000, the default);
- 3 `HEALTH` witnesses deciding by Paxos;
- `--control` with placement v2 (`snc_ctl --placement-v2 init`);
- `--move-units on`;
- `--checkpoint-s` 60.

**Witness requirements** (PAXOS_BALLOTS.md section 9; F9 is excluded
for this profile only under all of them):

- three unique witness endpoints in `--peers` (normalized: one host
  under two names counts once), with each witness's `--id` naming its own
  endpoint; `snc_witness` and the profile guard refuse otherwise; one
  address per witness host (a remote host named by two addresses cannot
  be detected);
- persistent witness state paths (plain files, not symlinks) that last
  the whole run;
- `--new-state 1` only at a witness's initial creation, never on a
  restart, never to recreate a member of an existing quorum;
- no individual witness rollback or restore (state file, directory,
  disk, VM, container or hypervisor snapshot), and no reuse of a witness
  identity after its state is lost: stop the run and rebuild the witness
  set (PAXOS_BALLOTS.md 9.3);
- witness persistence and directory-durability errors are fatal (the
  witness stops before replying);
- protected READY/NOT READY protocol versions: every witness of a group
  runs a binary with the READY causal order (SYNC_READINESS.md section 7);
- the campaign witness-state guard is active (`bench/lab/lab.py` with
  `bench/profile_check.py`), checking at the run's campaign boundaries; a
  run it marks invalid is not evidence. It is a point check, not
  continuous anti-rollback protection (PAXOS_BALLOTS.md 9.1).

| Property | Contract |
| --- | --- |
| What OK means | **HA_DURABLE while the group is healthy**: durable on the primary and acknowledged durable by its standby. While the group is degraded (grant on record), or after a failover until a new standby is seeded and caught up, an OK is **LOCAL_DURABLE** and the run must record it as such (section 9: STATUS `degraded=1` or `role=n`). The group is promotable again only once its primary has said READY, naming the NOT READY its proof follows (SYNC_READINESS.md section 7; every witness of the group must run a binary with it) |
| Process crash | Survives |
| Loss of one member's host or disk | Survives while healthy: the other member holds every HA_DURABLE OK (`test_replication`, `test_witness.py`, Vultr P3: EL2-EL4). A second loss before re-seeding is not covered |
| Replication | Synchronous; the standby is acked by LSN before the client's OK |
| Failover | Automatic, by a witness majority, on silence of the primary's health path. Measured: about 0.5-1.5 s for a kill and about 8 s for a silent partition (EVIDENCE 5; the gap is not attributed). A shard stall alone never promotes (B-3). With a witness majority lost: no promotion, no DEGRADE grant (Q6) |
| Retry | At most once for sequenced mutations resent within the retry horizon, across crash, restart and a single failover (EL3-EL4). A second failover during resends needs a test (Q5) |
| Read consistency | As Profile A at HA_DURABLE, **except** that a cut-off old primary can answer default reads of already-durable objects until it learns the new term (Q7). Standby reads (`--standby-reads`) are excluded |
| Migration | `snc_ctl move` with placement v2. A slot is never served from two groups, nor from a partial copy (MS-1..MS-4, D1/D7: `test_fail_closed`, `test_migrate`, `test_s1`). Acknowledged writes and follow-ups survive a move, including source or destination failover mid-move. Clients see `E_MIGRATING` for a window (S1: about 2.5-7 s at 100K objects on loopback) |
| Known exclusions | Async standbys; `STATUS` witnesses; standby reads; a shard-count change within a run; resends that bypass the library; two member losses in one group; witness-majority loss (except as a named fault); legacy or reused data directories; any witness state loss, rollback or restore (F9, PAXOS_BALLOTS.md 9.3) |

### Profile C: MEMORY CEILING

**Configuration:** no `--data-dir`: the engine runs in memory, and every
mutation is acknowledged at APPLIED.

| Property | Contract |
| --- | --- |
| What OK means | Applied in memory only (**APPLIED**) |
| Process crash, host loss | Nothing survives |
| Replication, failover | None |
| Retry | Deduplicated only within the process's life |
| Read consistency | Single-object; reads see the applied state |
| Migration | Not used |
| Use | Execution and locality ceiling only: the cost of SNC's execution path without durability. It **may never be compared with a durable competitor configuration**. A comparison with an in-memory competitor (Redis with persistence off, for example) is allowed only with the mismatch table of section 9 |

### Profile D: ASYNC LOCAL_DURABLE (optional)

**Configuration:**

- `--replica-of` with `--async-replica`;
- **no witnesses for the group**;
- promotion only by the harness, after it has killed the old primary.

| Property | Contract |
| --- | --- |
| What OK means | **LOCAL_DURABLE** on the primary. The standby receives the WAL best-effort |
| Host loss | **Not survived**: OKs not yet on the standby are lost on promotion (`test_async_failover`: 500 of 500, EL2) |
| Failover | Manual only (F-1). The harness must kill or fence the old primary before promoting: without witnesses nothing fences it, and two primaries could serve |
| Retry | After a promotion, sessions are as old as the standby (F-4). Resends may apply again, which is correct for lost writes, and earlier OKs may be lost. The run reports this as data loss, not as a retry success |
| Use | Only for comparison with competitors in their async-replication modes (Redis replicas, PostgreSQL async streaming). **Never reported as HA** |

Profile D is optional. The campaign need not run it, because Profile A
already gives the LOCAL_DURABLE comparison.

## 3. Known-issue triage

Every known issue and high-risk interaction, with its class per profile.
Matrix row numbers refer to [INTERACTION_MATRIX.md](INTERACTION_MATRIX.md).

| ID | Item | Evidence today | A | B | C | D | Why |
| --- | --- | --- | --- | --- | --- | --- | --- |
| R08 | An async standby is auto-promoted and acknowledged writes are lost (row 10, C7) | MEASURED, EL2 | n/a | **EXCLUSION** | n/a | **EXCLUSION** (no witnesses) | Section 6 |
| R11 | Resend after session expiry runs again (row 7, W2) | MEASURED, EL2 | **TEST REQUIRED → BLOCKER if T-Q1 fails** | **TEST REQUIRED → BLOCKER if T-Q1 fails** | NON-BLOCKING | TEST REQUIRED | Section 7 |
| Q1 | The retry horizon of the shipped client is far below the TTL | INFERRED | TEST REQUIRED | TEST REQUIRED | n/a | TEST REQUIRED | Section 7 |
| Q2 | Resend to the new owner after `E_MOVED` (row 3, D-3) | INFERRED: unreachable by the shipped library while the source holds the session | n/a | **TEST REQUIRED** | n/a | n/a | The library resends an in-doubt request to the same group, which deduplicates before `E_MOVED` [INV 18]. It resends to the new group only on an `E_MOVED` answer, which an applied request cannot get while its session exists. So a double apply needs R08 or R11 as well. A test must show it |
| Q3 | Checkpoint × lost standby with a witness, at run time and at restart (rows 13, 19c) | INFERRED: the main thread waits for durability that needs the standby, and it is also the thread that asks for DEGRADE | n/a | **TEST REQUIRED → BLOCKER if reproduced** | n/a | n/a | Profile B's failure runs kill standbys under load with checkpoints on. A wedged primary would make every such run measure the wedge, not SNC |
| Q4 | Blocked sync × load and memory growth (row 14) | INFERRED: waiters, held follow-ups and pipelining grow without a bound until the DEGRADE grant | n/a | **TEST REQUIRED, measured as a time series → resource-safety defect if a plausible control-plane outage can exhaust the process** | n/a | n/a | The growth is normally bounded by the grant latency (about 1-2 s). The test measures RSS and queue high-water across a standby loss at full load |
| Q5 | Second failover during resends (row 5) | INFERRED: same path as row 4, two terms | n/a | **TEST REQUIRED** | n/a | n/a | The failure program does repeated failovers |
| Q6 | Witness-majority loss during a move or a failure (rows 19, 26; C6) | INFERRED: no decisions without a majority; a sync primary that also loses its standby blocks | n/a | NON-BLOCKING (named fault only) | n/a | n/a | By design no promotion or grant without a majority. A run that injects it reports it as an availability result, not a safety claim |
| Q7 | Stale reads from a cut-off old primary (row 19a) | INFERRED, KNOWN GAP | n/a | **NON-BLOCKING**, excluded from claims, characterized | n/a | n/a | It affects read consistency during a partition, not acknowledged durability. No read-linearizability claim is made (section 12) |
| Q8 | WAL bit rot mid-segment silently drops acknowledged records (row 19b, S6) | INFERRED, KNOWN GAP | NON-BLOCKING | NON-BLOCKING | n/a | NON-BLOCKING | Latent media corruption is excluded from every durability claim. In Profile B a sync standby still holds the records. A characterization run is part of the failure program |
| Q9 | Wrong data directory, or control-plane startup with a stale directory (row 32, C5) | INFERRED | EXCLUSION | EXCLUSION | n/a | EXCLUSION | Fresh directories every run, scripted by the harness. Operator error is not under test |
| Q10 | Checkpoint × migration (row 27) | INFERRED: slot state is an ordinary object | n/a | **TEST REQUIRED** | n/a | n/a | Workload B moves ownership with checkpoints on |
| Q11 | Shard-count change × sessions (row 8) | INFERRED; only guard is the client refusing another shard count | EXCLUSION | EXCLUSION | EXCLUSION | EXCLUSION | No run changes `--shards` across a restart |
| Q12 | Lost standby leaves the group acknowledging LOCAL_DURABLE silently (H-1; `role=n` after a failover) | MEASURED (STABILIZATION_REPORT D) | n/a | **NON-BLOCKING with a run rule** | n/a | n/a | STATUS shows `role=n` and `degraded=1`. The harness tags those intervals and reports them as LOCAL_DURABLE (section 9) |
| Q13 | Async cut-off primary keeps acknowledging until it learns the new term (C3 exception) | INFERRED | n/a | n/a (sync) | n/a | EXCLUSION (harness kills before promoting) | Profile D's promotion procedure |
| Q14 | Other O(map) tasks still hold a shard for about 100 ms (HAVE, `INFO`, seal, export, activation, checkpoint snapshot, `rebuild_index`) (W1) | MEASURED (S1 evidence) | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | A tail-latency cost to report, not a correctness issue. Tails during moves and checkpoints are measured and reported |
| Q15 | Network flap × failover, no hysteresis (row 15, N7) | INFERRED | n/a | NON-BLOCKING (named fault) | n/a | n/a | Each flap can fail over and leave a group without a replica (then Q12) |
| Q16 | Clock jump × standby read lease (row 17) | INFERRED | n/a | EXCLUSION (no standby reads) | n/a | n/a | Standby reads are excluded |
| Q17 | Allocation failure in the session path aborts after apply; disk full or EIO is fail-stop (rows 36, 37, W3, S3) | INFERRED | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | Fail-stop by design. A disk-full run is a characterization, and memory is sized so allocation never fails |
| Q18 | Index results hide objects with a write in flight; not a snapshot (R-5) | AS-BUILT | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | Stated index semantics (section 9); a competitor's index is compared with them stated |
| Q19 | Obligation stalls and hot retry: a follow-up dropped until the next trigger; a full inbox re-emits at once (rows 34, 35) | INFERRED | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | NON-BLOCKING | No loss. Settlement lag is a measured metric. Fan-in to one inbox is a named workload, not a default |
| Q20 | Promotion × index lookup, no client barrier (row 33) | INFERRED | n/a | NON-BLOCKING | n/a | n/a | Since X1, index lookups are reachable over the wire (`INDEX`); the promotion window stays untested (matrix row 33) |
| X1 | **No secondary-index query on the wire**: `index_lookup` was in-process only (`wire.h` kinds: EXEC, CREATE, DELETE, TOPOLOGY, CLUSTER) | **RESOLVED 2026-11-05** | n/a | n/a | n/a | n/a | Wire kind `INDEX` (section 4, X1): today's semantics, tested by `test_net` and `test_index_wire.py` |

## 4. Workload-readiness gaps (not correctness)

These do not invalidate a contract, but they decide what the campaign
can measure. Each needs a reviewed decision before the workload that
needs it runs. None is authorized by this document.

- **X1. Secondary-index query over the wire** (workload A). **APPROVED,
  option (b):** the smallest wire kind that exposes the existing
  `index_lookup`. It is a separate, minimal, validation-enabling slice.
  - It exposes today's semantics only: durable-only results, filtered to
    served slots, not a snapshot, with objects whose write is in flight
    hidden (R-5).
  - It makes no index redesign, adds no snapshot semantics, and does no
    unrelated protocol work.
  - It needs compatibility tests: an old client or server meeting the
    new kind refuses it cleanly, and the existing kinds are unchanged.
  - **Implemented (2026-11-05):** wire kind `INDEX = 6`
    (`include/snc/wire.h`, [ROUTING.md](ROUTING.md)). `op` is the index
    id; the body is `IndexQueryBody` (max_results 1..4096) and the key.
    The node gathers from all its shards through the unchanged
    `index_lookup` and answers up to max_results IDs. A standby answers
    E_STANDBY, any shard's error answers that status with no IDs, and
    the sequenced flag and a non-zero `reserved` are refused (a read). In
    a cluster the client asks every group's primary and merges; during a
    slot move, between the seal and the activation, neither group lists
    that slot's objects (not a snapshot). Tests: `test_net` (wire format,
    bounds, refusals, a 40-byte key sent to the other shards out of line,
    equality with the in-process lookup, an unknown kind keeps the
    connection) and `tests/test_index_wire.py`:
    - `durable`: a create applied but unacknowledged by a SIGSTOPped
      standby is hidden, then shown once acknowledged;
    - `serving`: across a slot move, each account is listed once, by the
      group serving its slot;
    - `compat` (needs `SNC_OLD_BUILD`, so not in `ctest`): a server from
      before the kind answers E_BAD_ARGS and keeps the connection;
      TOPOLOGY and CREATE are unchanged. Run 2026-11-05 against a build of
      `dea7ba5` (before X1): passed. An old client never sends the kind, and responses
      keep the RESP format.
    No benchmark driver uses it yet.
- **X2. Drivers for workloads B and C** (EXPERIMENTAL_VALIDATION section
  5 table): region movement as slot moves; dormancy approximated by an
  external waker.
- **X3. Competitor harnesses:** SpacetimeDB, Orleans, Restate and DBOS.
  The PostgreSQL, Redis and ScyllaDB harnesses exist
  (`bench/conventional/`).
- **X4. Run tagging (required for Profile B):** the harness samples
  STATUS to tag every interval as one of:
  - healthy (HA_DURABLE);
  - degraded or lost standby (`degraded=1`, `blocked=1` or `role=n`:
    LOCAL_DURABLE);
  - failover and recovery (from the fault injection until the group is
    healthy again).

  Each is reported separately. **Degraded intervals, which are faster,
  are never pooled into healthy sync-HA performance.** `lab.py` already polls every node's
  STATUS in the background; the tagging rule is new.

## 5. Required pre-baseline tests

All are test, harness or instrumentation work (no semantic change). Each
is the smallest test that settles its item. If a test reproduces a
hazard that section 3 marks "BLOCKER if reproduced", the fix is a
separate reviewed slice, and the baseline waits for it.

| Test | Settles | Smallest test | Pass condition |
| --- | --- | --- | --- |
| T-Q1 | R11 horizon | Instrument `net::Client` to report the age of each resent request at resend (first send → this resend). Run every campaign retry and failover path: primary kill, silent partition, one-way partition, flap, request timeout, and a failover that fails | The largest resend age is reported per path, and every one is below `failover_ms` + `request_timeout_ms` (35 s), far below the 3600 s TTL. A client whose failover fails reports an error and resends nothing later. **Any path that can keep or resend an in-doubt sequence near or past expiry makes R11 a BLOCKER** |
| T-Q2 | Row 3 | Through `ClusterClient`: apply a sequenced transfer whose credit crosses groups, drop its answer, move the source slot, then resend | Applied once; the resend is answered from the source session (replayed OK); the final state at the destination is exact; no duplicate obligation (one follow-up settled, money conserved) |
| T-Q3 | Rows 13, 19c | `test_witness.py` with `--checkpoint-s 1`; SIGKILL the standby mid-checkpoint under load; also a restart that forces a recovery checkpoint with the standby gone | The witness grants DEGRADE, writes resume, and `main_ms` returns to normal. **If checkpoint or recovery keeps the primary from obtaining DEGRADE because the same control path is blocked on durability, that is a Profile B BLOCKER:** evidence is kept and a fix proposed. Checkpoints are never disabled to benchmark around it |
| T-Q4 | Row 14 | Standby SIGSTOP at full client load, with the grant as normal; then the same with the grant delayed (witness majority stopped for 10 s, then 60 s) | Reported as time series, not a yes/no: RSS delta and rate; pending client requests; held follow-ups; pipeline and backlog; must-deliver overflow; WAL and replication backlog, where available. **With the normal grant, growth must stop and drain.** With the delayed grant, the exhaustion risk is quantified (time to a stated memory limit at the measured rate). A plausible control-plane outage that could exhaust the process is a resource-safety defect |
| T-Q5 | Row 5 | `test_replication`: two consecutive primary kills (re-seeding between) during a resend storm | Each sequenced mutation is applied once, and money is conserved |
| T-Q10 | Row 27 | `test_migrate`: `checkpoint()` between seal and activate, and between stage and commit, then restart each node | Single owner, every acknowledged write present, all settled |
| T-H | Harness guards | One profile checker used by `lab.py` and the campaign scripts. It rejects automatically: async with witness HA; a `STATUS` witness; a reused data directory; a shard-count change across a restart within a profile; standby reads in Profile B; any deviation from the declared profile that is not named in the run's configuration | Each rejection is checked by a test, and a valid profile passes |

Already evidence, so no new test is needed (the classification is
updated in INTERACTION_MATRIX):

- **Row 20** (failover during a move): `test_migrate` with sync groups
  (EL3); `test_fail_closed` F1-F17; `test_s1` dst_promote, src_promote
  and fence (EL2).
- **Row 22:** crash points now include S1's chain (`test_s1` crash
  matrix).

## 6. R08 baseline decision

**Decision (ACCEPTED): EXCLUSION. R08 does not need implementing before
the campaign.**

- **Profile B uses synchronous replication only.** Its HA_DURABLE claim
  rests on sync replication, which is MEASURED at EL2-EL4. Async with
  witnesses is the only path that loses acknowledged writes on an
  automatic promotion, and Profile B forbids it (T-H checks this).
- **Async is allowed only as Profile D:** no witnesses, manual
  promotion after killing the old primary, and every OK described as
  LOCAL_DURABLE. Profile D never claims HA_DURABLE, and its results are
  never pooled with Profile B's.
- **What the exclusion costs:** the campaign cannot show "automatic
  failover with async replication". The competitors' async-HA modes are
  then compared only with Profile D (manual) or with Profile B (sync),
  and the mismatch is stated.
- **Why not implement F-a now:** the witness `mode` field changes the HA
  state's format. It is a contract change with its own rollout risk
  (CORE_CONTRACTS 9), and it buys nothing a correctly configured run
  needs. It is a candidate for after the GO/REFINE/NARROW/STOP decision.
  It becomes a BLOCKER only if a future profile needs automatic async
  failover.
- **Is this scientifically valid?** Yes. The claim is restricted to the
  configuration that provides it. The forbidden configuration cannot
  enter a run, because the harness refuses it, not because people
  remember not to.

## 7. R11 baseline decision

**Decision (ACCEPTED, conditional): non-blocking only once T-Q1 proves
the bound.** T-Q1 must show that the shipped client's largest resend age
stays well below session expiry on every campaign retry and failover
path. If any path can keep or resend an in-doubt sequence near or past
expiry, R11 is a BLOCKER. The TTL is not raised to make this pass.
Long-idle in-doubt resends are excluded from every claim.

**Can the campaign meet it? INFERRED from the code: no, with the shipped
client.** The double apply needs all three of these:

1. A request is applied and its answer is lost.
2. Its session is then left untouched for the TTL (3600 s, built-in;
   `snc_server` has no flag to change it).
3. A checkpoint runs (expiry happens only in `Engine::checkpoint`)
   before the same sequence is resent.

The shipped `net::Client` resends unanswered requests only inside
`fail_over`, right after reconnecting:

- That happens within `failover_ms` (30 s) of the failure, after at most
  `request_timeout_ms` (5 s) without progress.
- If no primary answers within `failover_ms`, it returns an error to
  its caller and keeps nothing to resend later. A campaign client must
  treat that as a failed run (T-H).

~~So an in-doubt request is resent within about 35 s or never.~~
**Withdrawn (T-Q1, BASELINE_READINESS 4, F2; cody's review of PR #47):**
that bound does not hold while a sync primary is blocked by a
control-plane outage. It keeps answering the client's reconnect, so the
client resends to it after every request timeout, and the age follows
the outage (35.3 s for a 40 s outage).

**The R11 contract for the campaign:**

> For any run claiming session deduplication, every in-doubt resend must
> occur before session expiry.

**Run validity (the guard).** A run that makes a retry claim passes
`net_bench --retry-claim-ttl-s TTL` (the server's `session_ttl_s`,
default 3600). The run is valid only if `max_resend_age_ms` is at most a
tenth of the TTL (360 s at the default); otherwise net_bench prints
`retry claim: INVALID` and exits 3, and the run's retry results are not
used. The margin is 10x because the real expiry horizon is longer than
the TTL and the margin must absorb what is not modelled:

- expiry is timed from the session's last applied request, not from the
  first send of the request being resent;
- it runs only at a checkpoint, and none completes on a blocked primary;
- server clocks may slew (the manifest records the NTP setup).

Long-outage retry stays excluded: a run with a control-plane outage long
enough to break the guard is an R11 characterization, not a retry claim.
The TTL is not raised to hide this, and session incarnation is not
required for the initial campaign. (`test_baseline_retry.py r11_guard`
shows the guard refusing a run.)
A client that goes idle for an hour and then sends
its next sequence finds no session and runs it as new work. That is
correct: it is a new request.

**What could still reach it, and how each is excluded:**

| Path | Exclusion |
| --- | --- |
| A wall-clock step forward of about an hour on a server (expiry compares wall-clock times) | Clocks slew only (manifest records the NTP setup); campaign runs are shorter than the TTL. The R-4 / G-4 clock dependence remains a stated gap |
| A harness that rebuilds a client and resends with an old `client_id` and sequence | Out of contract today (FAILURE_MODEL: `client_id` reuse). T-H forbids it in campaign scripts |
| A run longer than the TTL whose clients hold in-doubt requests across an outage longer than the TTL | No campaign fault lasts that long. A run that injects one is a named R11 characterization, not a retry claim |

**Why not "just raise the TTL":** the TTL stays at its default. Each
claiming run proves its own resend ages against it (the guard), not a
large number chosen to make the defect unreachable.

**What the campaign may not claim** (section 12):

- exactly-once across arbitrary idle periods;
- deduplication of resends by other clients;
- deduplication after a TTL-length outage.

**Is session incarnation (G-1..G-7) a blocker?** Not for the proposed
profiles. It becomes one if a workload needs retries across long
dormancy as a stated guarantee. Workload C's dormant entities are server
objects, not client sessions; a waker that resends after dormancy must
use application idempotency keys, not sessions. That is stated in the
workload C design before it runs.

## 8. Interaction-matrix updates

Applied to [INTERACTION_MATRIX.md](INTERACTION_MATRIX.md) in the same
change.

| Row | Was | Now | Evidence |
| --- | --- | --- | --- |
| 20 Migration × failover | UNTESTED | **TESTED** (EL3 sync, EL2 lossy async) | `test_migrate` (sync groups, standby promoted at each step); `test_fail_closed` F1, F3, F6, F7, F13, F17 (PR #40); `test_s1` dst_promote, src_promote, fence (PR #44) |
| 21 Migration × replication | PARTIALLY TESTED | PARTIALLY TESTED (unchanged class) | Standby promotion after moves (F-tests) shows staged and sealed state replicated; still no data comparison of standby against primary after a move |
| 22 Migration × recovery | TESTED | TESTED, extended | `test_s1` crash at admit, check, put, scan, stage and reply, including a restart with units off |
| 26 Control-plane loss × migration | PARTIALLY TESTED | PARTIALLY TESTED | F17 (offline restart without the control plane does not start); witness-majority loss mid-move still untested (Q6, NON-BLOCKING) |
| 3 Resend to the new owner after `E_MOVED` | UNTESTED | UNTESTED; analysis refined: INFERRED unreachable by the shipped library while the source holds the session (Q2, T-Q2) | Section 3 |
| 11 Long shard task × witness health | TESTED (EL3) | TESTED; the final stage is now bounded (W1) | S1 evidence |
| 5, 13/19c, 14, 27 | UNTESTED | UNTESTED, with pre-baseline tests T-Q5, T-Q3, T-Q4, T-Q10 | Section 5 |

## 9. Hardware and manifests plan

These are the hosts the campaign intends to use. **No CPU, core or NUMA
count is assumed: each is read from the host's manifest before its first
run.**

| Host | Known | To capture (manifest) | Role |
| --- | --- | --- | --- |
| **Main local bare-metal host:** Dell PowerEdge R640 | Dual socket, Intel Xeon Gold, about 384 GB RAM, Linux | Exact CPU model, cores and threads, NUMA nodes and distances, memory per node, disks and their write cache, NICs, BIOS power profile, kernel, governor, THP | All single-host profiles; NUMA classes of the locality envelope; one member or witness in LAN runs |
| **Remote physical host:** the partner's Dell PowerEdge R640 | Linux, separate physical site | As above, plus measured RTT, jitter and loss to the main host (method below) | The WAN class: replication, cross-group operations, failover, migration across sites. Reported separately from LAN |
| **VPS fabric** | Disposable, any provider or region | Provider, instance type, region and zone, the manifest, measured RTT, jitter and loss between every pair | Same-region and WAN topologies; fault injection that would be unsafe on the physical hosts; a third witness site |
| **R740xd** | TrueNAS Scale on bare metal | n/a | Not an SNC benchmark host |
| **This project's cloud VM** (4 vCPU) | `bench/manifests/dev-vm-4vcpu-20261104.txt` | n/a | Development and regression checks only |

**Network measurement, per host pair, recorded with the manifest:**

- **RTT:** `ping -c 1000 -i 0.01`, reporting p50, p95, p99 and max. The
  standard deviation may be recorded too, but it is not the only jitter
  measure: the p99-p50 spread and max are.
- **Loss:** the same `ping`.
- **Throughput:** `iperf3` in both directions, where available.

Every path is measured at the start and the end of each campaign
session.

**Allocation rule:** within a comparison, SNC and the competitor get
the same cores (by `lscpu -e` CPU list), the same memory limit, the
same storage device and the same network path. This is recorded in each
run's configuration.

**Run validity rules** (a run that breaks one is discarded, not
reported):

1. The profile's configuration matches section 2, checked by the
   harness guards (T-H).
2. STATUS is sampled at least once a second on every primary. Profile
   B results are reported in three separately tagged classes (X4), and
   never pooled:
   - healthy (HA_DURABLE);
   - degraded or lost standby (LOCAL_DURABLE: `degraded=1`, `blocked=1`
     or `role=n`);
   - failover and recovery.
3. The manifests of every host are committed before the run, and named
   in the run's directory.
4. The semantics table (durability, replication, ack point, consistency,
   operation and index semantics, batching, client concurrency) is
   filled for every system in the comparison. A mismatch is stated next
   to the result, not in a footnote.

## 10. Pre-registered falsification decision rules

These rules are **ACCEPTED with the review's corrections**. Where a number is not yet defensible,
this section gives the qualitative rule and the pilot that sets the
number. Every number is committed in `CRITERIA.md` **before any
comparative result exists**. The pilot measures only the variance of
each system on its own, never one system against another.

### 10.1 Pilot (before any comparison): SNC only

*Reconciled 2026-11-06 (reviews of PRs #54 and #55,
`handoff/PR55_Review_Handoff.txt`). The earlier rule piloted every system
and took N and the p99 objective from competitor runs, which would have
observed competitor performance before pre-registration. It is
superseded.*

- **The pilot runs SNC only**, on the host it will run on. No competitor
  performance is observed before `CRITERIA.md` is committed.
- It repeats workload A at fixed settings **at least 5 times** and records
  SNC's coefficient of variation of throughput (CV_throughput_SNC) and
  of p99 (CV_p99_SNC).
- **Noise floor**, fixed in `CRITERIA.md`:

  ```text
  N = max( 3 × max(CV_throughput_SNC, CV_p99_SNC), N_MIN )
  ```

  **N_MIN** is a fixed minimum materiality and noise floor. It is
  chosen at the formal criteria-setting review, after the SNC pilot and
  before any competitor performance run.
- **Competitor variance** is governed by **V_MAX**, also fixed in
  `CRITERIA.md`. In the comparative repetitions:
  - a competitor CV of at most V_MAX: the cell is valid;
  - a competitor CV above V_MAX: the cell is **invalid (too noisy)**. It
    is rerun, or its environment is corrected, and then rerun.
  - N never grows after competitor variance is seen, and no materiality
    threshold is weakened.
- **A difference smaller than N is "no difference".** PR #37's T9
  measured 8-16% host noise on the cloud VM; bare metal is expected to be
  lower, but this is not assumed.
- **N is a pre-registered noise and materiality guard, not a statistical
  confidence interval**, and not a per-competitor interval. If SNC's
  pilot variance turns out unexpectedly high or non-normal (for example
  bimodal runs, or a CV above 10%), the pilot method is revised before
  `CRITERIA.md`, and the revision is committed with its reason.
- **Before any competitor performance run**, `CRITERIA.md` holds the
  final values: N, N_MIN, V_MAX, the p99 objective (10.2), M_B and each
  Path B threshold (10.3). None is chosen in this document.

### 10.2 The primary metric

The primary metric is **throughput per core at a p99 latency objective**:

- **The objective comes from the SNC-only pilot, by a rule declared
  before the pilot runs.** The pilot sweeps SNC's offered load and
  finds SNC's stable knee and operating region. The objective is SNC's
  p99 at a documented fraction of its stable peak load. The fraction
  and the knee rule are written in the pilot plan before the sweep, and
  the resulting objective is recorded in `CRITERIA.md` before any
  competitor performance run. *(Superseded rule: "the p99 the slower
  system reaches at 50% of its own peak".)*
- Each competitor is then measured on how much throughput per core it
  sustains while meeting that fixed objective. The objective never moves
  because of competitor results.
- It is measured under matched semantics in the same profile.

Secondary metrics:

- memory per object;
- WAL and network bytes per operation;
- failover and recovery interruption;
- the E_MIGRATING window;
- the operational complexity count of EXPERIMENTAL_VALIDATION section 12.

### 10.3 Materiality

An SNC result in a cell is a **material win** by either of two
pre-registered paths. There is no subjective or post-hoc scoring.

**Path A, strong performance win:**

- at least **2x** on the primary metric (throughput per core at the
  matched p99 objective);
- beyond the noise floor N;
- with no material regression in tails (p99.9) or in the complexity
  inventory.

**Path B, multi-dimensional win:** all of:

- a smaller but substantial primary-metric advantage beyond N, of at
  least **M_B**. M_B is fixed in `CRITERIA.md` from the SNC pilot before any
  comparison: at least N, and no lower than a value cody and Codex set
  there;
- at least one **independent** material advantage, each with its own
  threshold fixed in `CRITERIA.md` beforehand:
  - memory per object;
  - p99.9 latency;
  - recovery or failover interruption;
  - network or storage amplification (bytes per operation);
  - operational component count;
- **no critical dimension materially worse.** The critical dimensions
  are the declared contract (durability, retry, read semantics), p99.9,
  and recovery/failover interruption. "Materially worse" means beyond N,
  or beyond the dimension's own threshold.

Each secondary dimension is scored only by the rule fixed for it in
`CRITERIA.md`.

### 10.4 Decision rules

| Decision | Rule |
| --- | --- |
| **GO** | SNC shows a material win (path A or B) in a durable profile (B, or A with B at least at parity) on at least one of workloads A, B, C. The win holds over a contiguous region of the locality envelope that includes a realistic share of cross-owner work (≥ 10% cross-owner, or the workload's natural share), and the failure semantics hold in every failure-program run. Operational complexity is **inventoried as a cost**, not required to be "no worse": the advantage must be large enough, across that region, to justify it |
| **REFINE** | No material win, but a measured profile (perf, traces) attributes the gap to one named SNC bottleneck, and a fix fits one bounded slice (for example S2 if the migration window decides a workload B cell). **At most two bounded rescue rounds.** Each names one measured bottleneck and reruns only the affected cells under unchanged criteria. A third rescue means NARROW or STOP |
| **NARROW** | A material win exists only in a narrow class: ≥ 95% owner-local, one workload, Profile C only, or one hardware class. The claim and the product narrow to that class, and STOP applies outside it |
| **STOP** | Any one of these: no material win (by path A or B) in any durable profile on any workload; the win disappears under realistic coordination (below 90% owner-local on every workload); a failure-program run violates a declared contract that cannot be fixed within one bounded slice; or a matched competitor reaches parity (within N) with materially lower complexity |

Ties and conflicts:

- If GO and STOP conditions hold on different workloads, the decision
  is NARROW to the GO workloads.
- A result that only Profile C shows never supports GO.

## 11. Requirements to freeze the Experimental Baseline

The baseline is frozen when all of these hold. The freeze is recorded
as one commit plus `bench/results/<campaign>/CRITERIA.md`.

1. **Review:** this corrected document is accepted.
2. **Tests:** T-Q1, T-Q2, T-Q3, T-Q4, T-Q5, T-Q10 and T-H are written and
   pass. Any that reproduces a hazard has its BLOCKER fixed in a
   separate reviewed slice, and the test passes after the fix.
3. **Full suites** pass at the frozen commit: RelWithDebInfo, ASan+UBSan
   and TSan, with sanitizer-scale tests noted as correctness coverage,
   not scale evidence.
4. **Workloads:** X1 is implemented for workload A, and the drivers and
   competitor harnesses that the first campaign wave needs exist (X2,
   X3).
5. **Manifests and network:** the main R640's manifest is committed,
   and the remote R640's too before any WAN run. Every VPS has one at
   creation. Network measurements are recorded per host pair.
6. **CRITERIA.md** is committed before the first comparative run. It
   names:
   - the frozen SHA;
   - the profiles;
   - the competitors' versions and configurations;
   - the semantics tables;
   - N, M_B, each Path B dimension's threshold, and the p99 objective
     from the pilot;
   - the decision rules of section 10.
7. **Pilot:** the within-system pilot ran, and its raw output is
   committed.
8. **Freeze:** no feature work lands on the baseline branch during the
   campaign. A fix that the campaign forces is a REFINE round, rerun
   under the same criteria.

## 12. Claims the baseline is NOT allowed to make

- **Automatic failover with async replication**, or HA_DURABLE for any
  async OK (R08).
- **Exactly-once for retries:** beyond the shipped client's retry
  horizon; across idle periods longer than the session TTL; across
  client restarts or rebuilt clients; or for deletes, reads and
  `unlogged` ops (R11, D-4).
- **Linearizable reads**, or any read guarantee across a partition: a
  cut-off old primary can serve stale reads (Q7). There is no
  multi-object snapshot, and transfers can be seen half-settled.
- **Durability against** power loss, latent media corruption, or loss of
  both members of a group (H3, S5, Q8).
- **HA_DURABLE during degraded or lost-standby intervals** (Q12).
- **Bounded latency for** the other O(map) operations (Q14), or a
  bounded migration window: S1 bounds the work unit, not the window.
- **Anything from Profile C** about durable systems, or any comparison
  that pairs a Profile C number with a durable competitor.
- **Loopback or 4-vCPU dev-VM numbers** as campaign evidence.
- **Security, multi-tenancy, or Byzantine tolerance** (FAILURE_MODEL
  "Out of contract").
- **Consensus safety with a non-SNC or Byzantine proposer** (C2x: the
  witness Paxos assumes every peer runs this implementation and sends an
  ACCEPT only after a successful phase 1), or **safety across a witness
  restored, rolled back or recreated** in its identity (F9). Witness
  replacement and recovery are not solved; no enterprise claim of
  consensus-member restore or replacement (PAXOS_BALLOTS.md 9).
- **Index semantics stronger than as built** (Q18): not a snapshot;
  objects with writes in flight are hidden.
- **WAN results presented as LAN results**, or the reverse.
- **Generality beyond the measured envelope**: a workload, locality
  share, or hardware class not run is not claimed.
- **Standby-read semantics**, which are excluded from every profile.
- **Safety under multiple simultaneous losses** that no test has
  exercised.

## 13. Order of work (from the review of PR #45)

1. Apply the PR #45 corrections to this document.
2. Implement and run the pre-baseline tests and harness guards (section
   5).
3. Implement X1 as its own small slice.
4. Full suites, independent review and QA.
5. Return the results for review.
6. If clear, capture the main R640's manifest.
7. Build the first-wave workload and competitor harnesses.
8. Run the within-system pilot only.
9. Commit `CRITERIA.md` with the final pre-registered thresholds.
10. Freeze the baseline SHA.
11. Only then begin the comparative falsification.

A blocker that steps 2-4 find stops this branch of work. It is
characterized, its evidence kept, and the smallest fix is proposed for
review; it is not fixed automatically.
