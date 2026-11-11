# S6 R640 checkpoint regression: preregistration (for review, no data)

Status: **DRAFT FOR REVIEW. NOT GO.** No S6 run has been made, and none may
be made until this record has been reviewed and an explicit GO given.
Written 2026-11-11 under
[handoff/PR97_Manifest_Closure_Final_Review.md](handoff/PR97_Manifest_Closure_Final_Review.md)
(section E). It implements the plan of DESIGN-R section 9
([CHECKPOINT_BACKPRESSURE_DESIGN.md](CHECKPOINT_BACKPRESSURE_DESIGN.md)).
The pacing evidence is in
[bench/results/r640-s2-pacing-20261111/](../bench/results/r640-s2-pacing-20261111/README.md).

Once GO is given, nothing below changes after any S6 data exists.

## 1. Code identity

| | |
|---|---|
| Merged post-PR97 `main` | `0dbd4c77b993572fb7b771fa44e7520c3197fe1b` |
| **S6 SHA** | **Not yet fixed. See section 2.** `0dbd4c7` cannot run the derived pacing values. The S6 SHA is the merge of the flags-only change of section 2 (or of whatever the review decides instead), recorded here before GO. |
| Build | RelWithDebInfo, fresh build directory, `cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`, GCC 13.3.0; no sanitizer, no telemetry (PILOT_PLAN.md) |
| Binaries | `snc_server` and `net_bench` SHA-256 recorded before the first cell and checked after the last |
| Manifest | e, `bench/manifests/r640-main-20261111.txt`, SHA-256 `768056bbee1d7f178ed0be1c195b821f0638a1de28a4aca1ea0afdf5444c27b6` |
| Pre-S1 comparator build | `389b623` (Merge PR #71, the tip of `main` immediately before S1, PR #72, merged). See section 6. |

## 2. R640 pacing values, and the change they need

Derived in the evidence README, section D, before any S6 data.

| Config field | S6 value | Compiled default at `0dbd4c7` |
|---|---|---|
| `ckpt_pacing` | on | on |
| `ckpt_walk_batch` | 16 | 16 |
| `ckpt_walk_min_ns` | 20,000 | 20,000 |
| `ckpt_walk_max_ns` | **65,000** | 100,000 |
| `ckpt_walk_ceiling_ns` | **155,000** | 250,000 |
| `ckpt_walk_step_ns` | **5,000** | 10,000 |
| `ckpt_pace_update_ns` | 1,000,000 | 1,000,000 |
| WAL high / low | 200% / 125% (inherited hypothesis, not tuned) | same |
| backlog high / low | 50% / 25% (inherited hypothesis, not tuned) | same |

Reference measurements:
- legacy 256-ID step, T256 = 171.6 µs p50 (p99 187.7);
- batch-16 b = 13.3 µs p50, 16.4 p99;
- about 670 ns per ID.

Worst routine step against T256: min 0.21×, max 0.47×, ceiling 1.00×.

**Required change (blocker, not made).** `snc_server` exposes no flag for
any walk field. Proposed, for separate review:
- Add `--ckpt-walk-batch N`, `--ckpt-walk-min-us N`, `--ckpt-walk-max-us N`,
  `--ckpt-walk-ceiling-us N` and `--ckpt-walk-step-us N` to
  `tools/snc_server.cpp`.
  - Values are range-checked and exit 2 on error, as the existing
    `--ckpt-*` flags do.
  - `Engine::Engine` already refuses an inconsistent pacing config
    (`std::invalid_argument`, P7). `snc_server` does not catch that today,
    so the change must turn it into a clean exit 2.
- The compiled defaults are unchanged, so dev-VM tests and behaviour stay
  as approved in PR #75.
- No file under `src/` or `include/` changes.
- Evidence that the walk is untouched: `git diff 0dbd4c7..S6 -- src include`
  is empty, and the S6 build's `ckpt_walk_cost` SHA-256 equals
  `4707dac1…5d2d2`. If either check fails, section C of the evidence
  README is re-measured at the S6 SHA before GO.

The alternative is to change the compiled defaults to the R640 values.
That is not proposed: it would change the approved dev-VM behaviour and
test baselines for a hardware-specific reason.

## 3. Checkpoint configuration (explicit, all cells with a checkpoint)

| | Value | Why |
|---|---|---|
| `--checkpoint-s` | 60 | DESIGN-R section 9 |
| `--ckpt-target-ms` (`ckpt_target_ms`) | 30,000 | The S2 default, stated rather than implied: a pacing goal, not a pass criterion |
| `--ckpt-max-age-ms` (`ckpt_max_age_ms`) | **60,000** | Proposed in place of the 300,000 placeholder. The server checkpoints every 60 s; a checkpoint older than its interval means the schedule cannot be kept and retained WAL grows. CP-1A measured about 3 s (walk 1.6 s + sync 1.1 s) on the old build, so 60 s is not tight. If the review prefers the 300,000 default, it is changed here before GO. |
| `ckpt_warn_pct` | 80 (warn at 48 s) | default |
| `--ckpt-io` | dsync (default) | S3, as approved |
| `--ckpt-window` | 4 (default) | S3 |
| `--ckpt-sync-tokens` | 0 = unlimited (default) | S4: K is a hypothesis, left at the approved default |
| Every other `Engine::Config` field | default at the S6 SHA | |

Every server command line of section 5 states the checkpoint and pacing
flags, defaults included, so that the logs carry the full configuration.

## 4. Machine state, allocation, isolation

- **Host:**
  - manifest e hardware;
  - before the first cell and after the last, re-record the governor
    (`performance` on 72 CPUs), `kernel.numa_balancing = 0`, the DIMMs,
    the SSD model `ARFX3840S5xnFTRI` and the `/srv/snc-data` mount
    options;
  - any difference voids the run (section 7).
- **No other load:** `snc_server` and `net_bench` not running at the
  start (as diag3 checks).
- **Allocation PILOT-L** (`bench/pilot_plan.json`):
  - server: `numactl --physcpubind=2,4,6,8,10,12,14,16 --membind=0`,
    `--cpus 2,4,6,8,10,12,14,16`, 8 shards;
  - client: `numactl --physcpubind=18,20,22,24,26,28,30,32 --membind=0`,
    8 threads, `--cpus 18,…,32`;
  - CPUs 0 and 36 for housekeeping; SMT siblings idle.
- **Profile A:**
  - 5,000,000 objects (server `--objects 10000000`, as diag3);
  - mix `read=70,deposit=15,transfer=10,index=5`, `--route owner`;
  - warm-up 10 s, window 60 s, drain limit 10 s;
  - open loop depth 1,024, closed loop depth 512.
- **Isolation (PR #62 / #64 rules):**
  - data under `/srv/snc-data/s6/<cell>-r<k>` (the data SSD), one fresh
    directory per rep, removed after the rep;
  - ids files under scratch `/dev/shm/snc-s6`;
  - output under `bench/results/s6-r640-<date>/` (boot disk);
  - scratch and output are checked to be on no physical disk holding the
    data root, with `bench/pilot.py`'s resolver. Unknown mappings refuse.

## 5. Cells (exact)

| Cell | Server checkpoint | Load | Reps | Role |
|---|---|---|---|---|
| **P** `r436646-ckpt` | `--checkpoint-s 60` + section 3 flags | open loop `--rate 436646` | 3 | **primary, pass/fail** |
| N `r436646-nockpt` | none (no `--checkpoint-s`) | open loop `--rate 436646` | 3 | comparator; its median total p99 is the p99 reference |
| H `r2183233-ckpt` | `--checkpoint-s 60` + section 3 | open loop `--rate 2183233` | 3 | recorded, no pass/fail |
| C `closed-ckpt` (S6 SHA) | `--checkpoint-s 60` + section 3 | closed loop, depth 512, `--seconds 60` | 3 | capacity guard, numerator |
| C0 `closed-ckpt-preS1` (`389b623`) | `--checkpoint-s 60` (that build has no pacing flags) | closed loop, depth 512, `--seconds 60` | 3 | capacity guard, denominator |

Per rep, as diag3's B arm:
- start the server;
- `net_bench … --depth 64 --seconds 0` loads the objects;
- the measured run follows on the same server;
- the server is stopped only after any checkpoint it started has logged
  completion, or after `ckpt_max_age_ms` has passed since its begin;
- at the end of each rep, before the stop, `GAUGES <shard>` and `HEALTH`
  are read on the admin port (`--admin-port`) for all 8 shards.

**Order (fixed before data):** rep-major, the same within each rep:
N, P, H, C, C0. Ports advance per cell.

**Checkpoint timing check.** The server's first checkpoint begins 60 s
after server start, so after the load (about 6 s) and the warm-up (10 s)
it falls inside the 60 s window. If in a P or H rep no checkpoint *begins*
inside the measured window (server log against the client's window
times), that rep is VOID and rerun once (section 7).

## 6. Pass/fail rules

All arithmetic is exact (integers and `Fraction`). "Total" is net_bench's
run-section `total latency ns` line for the measured window. Counts are
whole-run (warm-up, window and drain), the stricter reading.

**P passes if, in each of its 3 reps:**
1. **No refusal:** `overloaded` = 0 and index `refused` = 0.
2. **Lateness:** `late_over_1ms` × 1000 ≤ `scheduled_in_window`.
3. **p99 against the reference:** total p99 ≤ 3/2 × R, where R is the
   **median of N's three total p99 values** (same SHA, same session).
   - This is the reference resolved in the PR97 handoff: the same-SHA
     no-checkpoint median, not a paired reference.
   - It is fixed now, applies to all three P reps, and is not changed
     after S6 data.
4. **p99.9:** total p99.9 ≤ 10,000,000 ns.
5. **Backlog:** for every shard, `emits_peak` < 1,024 (`kEmitBacklog`).
   - `emits_peak` is a high-water mark since server start, so this
     includes the load phase.
   - At or below 512 (50%) is a *target*: reported, not a pass criterion
     (DESIGN-R section 9).
6. **Checkpoint completion:**
   - the rep's checkpoint logs completion (`checkpoint: N objects in Xs`)
     with X × 1000 < `ckpt_max_age_ms` (60,000);
   - and `ckpt_degraded_count` = 0 on every shard.
7. **Correctness and durability:**
   - `errors` = 0, `stale` = 0 and index `errors` = 0;
   - `drained yes`;
   - the server exits 0 with no abort or fail-stop message in its log.

**Capacity guard:**
- median of C's three closed-loop `total ops/s` ≥ 95/100 × median of C0's
  three;
- each rep must also satisfy rule 7, or it counts as a failed rep.

**Verdict:**
- **PASS:** all three P reps pass all seven rules, and the capacity guard
  holds.
- **FAIL:** otherwise, unless the run is VOID.

**Reported, not pass/fail** (DESIGN-R section 9):
- for H: everything above;
- per P, H and C rep, from GAUGES and the server log:
  - `wal_flush_ewma_us` and `wal_flush_max_us`;
  - `emits_peak` against 256 and 512;
  - `ckpt_last_*_us` (snapshot, walk, drain, durable wait, sync, total);
  - `ckpt_step_max_ns` and `ckpt_step_max_objects`;
  - `ckpt_walk_steps` and `ckpt_catchup_steps`;
  - `ckpt_past_target_count`;
  - `refused_*` counts and `wal_direct`;
- device await from `/proc/diskstats` before and after each rep;
- the WAL-EWMA time above the high threshold and the budget trace are not
  available from end-of-rep gauges. They are reported only if the runner
  samples gauges during the rep, which the review decides (section 9).

## 7. VOID (rerun, not a result)

A VOID run is not a FAIL and not a PASS. It is reported with its raw
output.
- **Machine or build drift:** a host check of section 4 differs before
  versus after, or a binary hash changes. The whole session is VOID.
- **Reference unusable:** any N rep with a refusal or a rule-7 failure.
  The p99 reference is then not a no-checkpoint baseline, so the session
  is VOID and reported. P is not judged against it.
- **Checkpoint outside the window:** a P or H rep in which no checkpoint
  began inside the measured window. Rerun that rep once, at the end, in
  the same order. A second such rep makes the session VOID.
- **Harness failure:** a client or server failed to start, or a port was
  taken. Rerun that rep once.

Nothing else is a reason to rerun. A rep that fails a rule is a result.

## 8. Known risks, recorded before data

- **CPU sharing inside the server's CPU set.** The walk-cost probe on a
  single CPU (L0) showed steps of 0.2-1.0 ms in every mode, pacing off
  included: another thread of the process, or its dsync I/O worker,
  preempting the walker. In S6, the 8 shards, the server's other threads
  and the kernel's I/O workers share the 8 server CPUs. Latency in P may
  include this.
  - Pacing cannot remove it.
  - It is not a reason to change the allocation now: PILOT-L is the
    approved baseline.
  - If P fails, this is a hypothesis to test in review, not a reason to
    rerun.
- **The walk under load** is likely slower per ID than measured idle. The
  time bound holds; progress per step falls. `ckpt_past_target_count` and
  `ckpt_last_walk_us` show it.
- **The rate.** 436,646 = R_2 of the CP-1A grid, from P0 = 4,366,466,
  measured on manifest d hardware (five channels per socket). It is
  carried forward unchanged as instructed (DESIGN-R section 9; PR97
  handoff E). It is not a re-derived rate for manifest e, and S6
  comparisons are same-machine and same-SHA, so the regression stays
  valid.
- **One-step begin snapshot** (about 50 ms per shard measured on the old
  build) is unchanged by S2 (B2 deferred). It is in P's latency.

## 9. Open for the review (decide before GO)

1. **Flags-only change of section 2.** Accept it (then fill the S6 SHA in
   section 1), or decide otherwise.
2. **`ckpt_max_age_ms` = 60,000** (proposed) or 300,000 (the default).
3. **Pre-S1 comparator** `389b623`, or another build (for example the
   CP-1A pilot build used by diag3's B arm).
4. **Runner.** No S6 runner exists on `main`; diag3 lives on
   `r640-diag-telemetry`. Proposed: a new `bench/s6.py`, built from
   diag3's B arm and `pilot.py`'s isolation and parsing, plus the admin
   GAUGES/HEALTH reads and the rule checks above, with deterministic tests
   of every rule boundary (as `test_diag3.py`). Reviewed with this record,
   before GO.
   - Whether it should also sample gauges during each rep (WAL-EWMA time
     above threshold, the budget trace) is for the review. Sampling every
     100 ms through the admin port is light but not zero.
