# S6 R640 checkpoint regression: preregistration (final, for review; NOT GO)

Status: **FINAL TEXT FOR INDEPENDENT REVIEW. NOT GO.** No S6 cell has been
run.

Inputs:
- Decisions from [handoff/PR98_S2_Pacing_S6_Prereg_Final_Review.md](handoff/PR98_S2_Pacing_S6_Prereg_Final_Review.md)
  and [handoff/PR99_S6_Pacing_CLI_Final_Review.md](handoff/PR99_S6_Pacing_CLI_Final_Review.md);
- the plan of DESIGN-R section 9
  ([CHECKPOINT_BACKPRESSURE_DESIGN.md](CHECKPOINT_BACKPRESSURE_DESIGN.md));
- pacing evidence:
  [bench/results/r640-s2-pacing-20261111/](../bench/results/r640-s2-pacing-20261111/README.md);
- runner evidence:
  [bench/results/s6-runner-20261111/](../bench/results/s6-runner-20261111/README.md).

Nothing below changes after any S6 data exists. The runner
(`bench/s6.py`) implements this text exactly. It refuses a real run until
the freeze of section 1.3 is done and a GO is filed.

## 1. Identity

### 1.1 Code and binaries

| | |
|---|---|
| C++ source of every current binary | `main` at `91278d7` (merge of PR #100, after PR #99). This runner PR changes no C++, so its merge has the same sources. |
| Build | `build-s6/`, fresh, `cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`, GCC 13.3.0, on the R640 |
| Current `snc_server` SHA-256 | `69a51b695e2029942039d386abaa9755616f31327ff63e0420dd0c5c8ba25ee1` |
| Common `net_bench` SHA-256 (every cell, C0 included) | `9fc5494404747f8242680552588bd6f986e3e00b265e45ee67262c513a93c581` |
| `ckpt_walk_cost` SHA-256 (walk identity) | `4707dac135a6fc72c41a712075678aa24ccbf7ad74d546962623026584b5d2d2`, the binary characterized in PR #98 |
| Pre-S1 comparator commit | `389b6234855082a4591c6677d0a303c3add3319d` (merge of PR #71, base of PR #72) |
| Pre-S1 `snc_server` (`build-preS1/snc_server`) | `78f471efb7e7951bf8cf14637e03120d6460dc989317c5c32fb073af23806ff7`: RelWithDebInfo from a worktree of `389b623`. Only its server is used. |
| Manifest | e, `bench/manifests/r640-main-20261111.txt`, SHA-256 `768056bbee1d7f178ed0be1c195b821f0638a1de28a4aca1ea0afdf5444c27b6` |
| Runner | `bench/s6.py`, tests `tests/test_s6.py` |

The runner checks all three binary hashes against `bench/s6_frozen.json`
before the first cell and after the last. Any difference refuses the run,
or voids it (section 7).

### 1.2 Merged S6 HEAD

The merge of this runner PR is the S6 HEAD. It cannot be written inside
the PR it merges, so it is recorded by the freeze of 1.3. The runner then
requires:
- HEAD has that merge as an ancestor;
- HEAD differs from it only in `docs/`, `AGENTS.md`,
  `bench/s6_frozen.json` and `bench/results/`. No code may change after
  the freeze.

### 1.3 The freeze (after the review, before any cell)

One docs-only commit sets two things:
- in `bench/s6_frozen.json`: `runner_commit` (the merge SHA) and
  `go_record` (the filed GO handoff);
- in this section: the same merge SHA.

Until both are set, `bench/s6.py` refuses before starting any process.
This was checked: `bench/results/s6-runner-20261111/README.md`, run 4.

## 2. Pacing (approved, PR #98 review section 4)

| Field | Flag | Value |
|---|---|---|
| `ckpt_pacing` | `--ckpt-pacing on` | on |
| `ckpt_walk_batch` | `--ckpt-walk-batch 16` | 16 |
| `ckpt_walk_min_ns` | `--ckpt-walk-min-us 20` | 20 µs |
| `ckpt_walk_max_ns` | `--ckpt-walk-max-us 65` | 65 µs |
| `ckpt_walk_ceiling_ns` | `--ckpt-walk-ceiling-us 155` | 155 µs |
| `ckpt_walk_step_ns` | `--ckpt-walk-step-us 5` | 5 µs |
| `ckpt_pace_update_ns` | (default) | 1 ms |
| WAL high / low | (default) | 200% / 125%, inherited hypothesis |
| backlog high / low | (default) | 50% / 25%, inherited hypothesis |

Derivation (PR #98):
- T256 = 171.6 µs (p50 of the legacy 256-ID step), b = 16.4 µs (p99 of a
  batch);
- ceiling + b ≤ T256;
- max + b ≤ T256 / 2.

## 3. Checkpoint configuration (approved, PR #98 review decision B)

| | Value |
|---|---|
| `--checkpoint-s` | 60 (cells with a checkpoint) |
| `--ckpt-target-ms` | 30,000: a pacing goal, not a pass threshold |
| `--ckpt-max-age-ms` | **60,000**: the hard maximum, frozen; not widened after a failing run |
| `ckpt_warn_pct` | 80 (default) |
| `--ckpt-io` / `--ckpt-window` / `--ckpt-sync-tokens` | dsync / 4 / 0, the approved defaults |
| Everything else | default at the S6 HEAD |

Every current-server command line states the pacing and checkpoint flags,
N included, so N differs from P only by `--checkpoint-s 60`. Every current
server prints `ckpt_pacing: ...` at startup, so each server log records
the values in force. The pre-S1 server takes `--checkpoint-s 60` only: it
has no pacing flags.

## 4. Machine, allocation, isolation

- **Machine (before and after, `machine_identity`).** These must match
  manifest e:
  - kernel, 72 CPUs, CPU model, node 0 and node 1 CPU lists;
  - MemTotal within 0.1%;
  - governor `performance` on every CPU;
  - `kernel.numa_balancing = 0`;
  - on the data disk as the isolation resolver finds it (never a fixed
    device letter):
    - model `ARFX3840S5xnFTRI`;
    - active scheduler `[mq-deadline]`;
    - `write_cache` = `write back`;
    - `queue/fua` = `1` (exposed by this kernel; manifest e `fua=1`);
  - `/srv/snc-data`: ext4, the filesystem UUID
    `3bf857c8-0cb4-4c57-b9a5-14328e70b061`, with every manifest-e mount
    option present: `rw`, `relatime`, `stripe=2` (set containment, not a
    prefix);
  - the manifest's own SHA-256.

  Any check failing before the session refuses; failing after it, or
  changed values, is VOID. The DIMM population needs root, so the runner
  does not read it; MemTotal and the per-node CPU lists stand for it.
- **Nothing else running:** no `snc_server` or `net_bench` at the start.
- **PILOT-L:**
  - server: `numactl --physcpubind=2,4,…,16 --membind=0`,
    `--cpus 2,4,…,16`, 8 shards;
  - client: CPUs 18,…,32 on node 0, 8 threads;
  - the GAUGES/HEALTH sampler pinned to CPU 0, the disk observer to CPU 36
    (housekeeping).
- **Profile A:**
  - 5,000,000 objects (server `--objects 10000000`);
  - mix `read=70,deposit=15,transfer=10,index=5`, `--route owner`;
  - open loop: depth 1,024, warm-up 10 s, window 60 s, drain limit 10 s;
  - closed loop: depth 512, 60 s.
- **Isolation (`pilot.disks_of` / `isolation_problems`):**
  - data root `/srv/snc-data/s6` (data SSD only), a fresh directory per
    rep, removed after it;
  - ids files in scratch `/dev/shm/snc-s6`;
  - output on the boot disk;
  - scratch and output on no disk of the data root (the dry run resolved
    data `sdb`, scratch `sda` through swap, output `sda`); unknown
    mappings refuse.
- **The output directory** must not exist: one session, once.

## 5. Cells, order, sampling

| Cell | Server | Load | Reps | Role |
|---|---|---|---|---|
| **P** | current, `--checkpoint-s 60` | open loop, `--rate 436646` | 3 | **primary: pass/fail** |
| N | current, no checkpoint | open loop, `--rate 436646` | 3 | its median total p99 is the reference |
| H | current, `--checkpoint-s 60` | open loop, `--rate 2183233` | 3 | recorded, no pass/fail |
| C | current, `--checkpoint-s 60` | closed loop | 3 | capacity, numerator |
| C0 | pre-S1 `389b623`, `--checkpoint-s 60` | closed loop | 3 | capacity, denominator |

The rate 436,646 is kept exactly (PR #98 decision D). It is a regression
cell carried from CP-1A, not re-derived for manifest e.

**Order:** rep-major, N, P, H, C, C0 in each rep. Ports advance by 40 per
cell; the admin port is the cell's port + 30.

**Per rep** (diag3's discipline, `Driver.execute`):
1. Start the server; wait for its port and a HEALTH answer.
2. Start the sampler and the disk observer.
3. Load with `net_bench … --depth 64 --seconds 0`.
4. Make the measured run on the same server.
5. If a checkpoint is in progress, wait until HEALTH says `ckpt=idle`, up
   to the 60 s maximum age + 5 s.
6. Read the final GAUGES and HEALTH.
7. Stop the server (SIGTERM) and record its exit code.
8. Remove the data directory and the ids file.

**Sampling (PR #98 review section 6), 100 ms, every cell (C0 included):**
- GAUGES and HEALTH, each on its own admin connection, on fixed 100 ms
  deadlines.
- **No catch-up** (PR #101 review 1, item 2). After a cycle, the next
  deadline is one period later. If that deadline has already passed, the
  sampler moves to the first future deadline on the same grid
  (`next_deadline`). A missed period is a gap, not work owed, so a slow
  admin reply never produces back-to-back cycles. Skipped deadlines are
  counted and reported (`skipped_deadlines`).
- Every reply is kept verbatim with its monotonic time.
- A cut or failed reply is recorded as a failed sample, never dropped.
- Time above a threshold is computed from timestamps, sample-and-hold. An
  interval longer than 200 ms, or one that starts at a failed sample, is a
  gap: reported, and not attributed either way.
- A gauge the server does not publish is reported as absent (`null`),
  never 0. The pre-S1 server has no pacing, degraded, sync-token, intake
  or cross-node gauges.

**Checkpoint inside the measured window (P, H, C, C0;**
**`checkpoint_window`).**
- Every cell that is meant to measure a checkpoint must prove it did.
- **P and H (open loop):** the window is [t_run + 10 s, t_run + 70 s] on
  the runner's monotonic clock. t_run is read just before net_bench's run
  is spawned. net_bench's own window starts later by its startup, about
  0.01-0.02 s to read the ids file.
- **C and C0 (closed loop; PR #101 review 1, item 3):** the window is the
  measured process's own [start, end]. This is conservative: it includes
  net_bench's startup.
- Each begin is bounded by the two good samples between which the node's
  `ckpt_seq` advanced (`Engine::checkpoint` stores it at begin). A
  checkpoint already running at the first sample has no lower bound.
- The rep counts only if at least one begin has both bounds inside the
  window. Otherwise it is a checkpoint-window VOID (section 7).
- A C or C0 rep without a proven checkpoint is never used as checkpoint
  capacity.

## 6. Rules (exact integer and `Fraction` arithmetic; `bench/s6.py`)

"Total" is net_bench's run-section `total latency ns` line for the
measured window. Refusal and error counts are whole-run counts (warm-up,
window and drain), the stricter reading.

**Reference:** R = the median of N's three total p99 values: the same-SHA
no-checkpoint median, used for all three P reps. Not a paired reference.

**Each P rep must satisfy all seven rules:**
1. **No refusal:** `overloaded` = 0 and index `refused` = 0.
2. **Lateness:** `late_over_1ms` × 1000 ≤ `scheduled_in_window`.
3. **p99:** total p99 ≤ 3/2 × R.
4. **p99.9:** total p99.9 ≤ 10,000,000 ns.
5. **Backlog:** final `emits_peak` < 1,024 on all 8 shards. It is a
   high-water mark since server start, so it includes the load.
   - An absent shard fails the rule.
   - ≤ 512 is a target: reported, not judged.
6. **Checkpoint:**
   - at least one completion line `checkpoint: N objects in Xs`, each with
     X × 1000 < 60,000;
   - and final `ckpt_degraded_count` = 0 on all 8 shards. Absent fails.
7. **Correctness:**
   - the measured `net_bench` exited 0. Any other status, a signal, or no
     recorded status fails. This is a result, never a rerun (PR #101
     review 1, item 1);
   - `errors` = `stale` = index `errors` = 0;
   - `drained yes`;
   - the server alive at the end of the run, exiting 0 on SIGTERM;
   - no failure marker in its log (`fail-stop`, `terminate called`,
     `Aborted`, sanitizer reports, `CHECK failed`, segfault).

**Capacity guard:**
- median of C's three `total ops/s` ≥ 95/100 × median of C0's three;
- and rule 7 holds (without `drained`, closed loop) in all six C and C0
  reps.

**Verdict:**
- **PASS:** all three P reps satisfy all seven rules, and the capacity
  guard holds.
- **FAIL:** otherwise, when the session is not VOID.

**Reported, not judged:**
- H in full;
- per rep, from the samples:
  - WAL EWMA peak and time above WAL-high (ewma > 200% of the shard's
    learned baseline);
  - backlog peak and time above 25% and 50% of 1,024;
  - walk-budget range and trace;
  - catch-up time;
  - sample count, failed samples and the largest interval;
- from the final GAUGES: per-phase `ckpt_last_*_us`, `ckpt_step_max_*`,
  `ckpt_walk_steps`, `ckpt_catchup_steps`, `ckpt_past_target_count`,
  `refused_*`, `wal_direct`;
- disk rows from `/proc/diskstats`.

## 7. VOID, and the only reruns

| Cause | Action |
|---|---|
| A P, H, C or C0 rep in which no checkpoint begin is proven inside its measured window (section 5) | Rerun that rep once, at the end, in plan order. A **second** such rep in the session, or VOID again on the rerun, makes the session **VOID**. |
| Harness: server or admin port not up, or the load failed | Rerun once, at the end. VOID again: session **VOID**. |
| N reference unusable: any N rep with a refusal or a rule-7 failure | Session **VOID**. P is not judged against it. |
| Machine or build drift at postflight (section 4, section 1.1) | Session **VOID**. |

Nothing else is rerun. A rep that fails a rule is a result.
`tests/test_s6.py` V5 checks that a FAILing session executes exactly 15
reps.

## 8. Known risks, recorded before data

- **CPU sharing in the server's CPU set.** The PR #98 probe at one CPU
  showed steps of 0.2-1.0 ms from another thread or the I/O worker in
  every mode. Pacing cannot remove this. It is a hypothesis for review if
  P fails, not a reason to rerun.
- **The walk under load** is likely slower per ID than idle. The time
  bound holds; progress per step falls. This shows in `ckpt_walk_steps`,
  `ckpt_past_target_count` and `ckpt_last_walk_us`.
- **The rate** comes from manifest-d CP-1A (section 5). It is a regression
  cell, not a manifest-e capacity point.
- **The begin snapshot** (about 50 ms per shard, old measurement) is not
  bounded by S2 and is in P's latency.
- **The sampler** issues two admin requests per 100 ms to every server, in
  every cell (C0 included), so instrumentation is not applied only to the
  checkpoint arm.

## 9. Runner validation (`bench/results/s6-runner-20261111/`)

- `tests/test_s6.py` covers:
  - every boundary (p99 3/2, lateness 0.1%, p99.9 10 ms, backlog 1,024,
    capacity 95%, checkpoint 60 s);
  - the refusal, correctness and drained rules;
  - the four VOID causes; one rerun at most; no rerun for a FAIL;
  - medians;
  - timestamp-based durations with gaps;
  - old-server absent gauges;
  - commands (one net_bench everywhere; N vs P);
  - net_bench parsing;
  - the freeze refusal;
  - checkpoint-begin bounds.
- Review 1 additions:
  - measured-client exit status (K1);
  - sampler deadlines with no catch-up (S1);
  - the C and C0 checkpoint window (W1);
  - storage identity with faked sysfs and mounts (D1).
- **28 of 28 deliberate mutations** of `bench/s6.py` are caught: the
  original 19, rerun at the review-1 head, plus 9 for review 1's items.
- **Dry run:** every command of the 15 cells printed; preflight ok
  (machine, build, isolation); nothing started.
- **Real run before the freeze:** refused before any process started.
- **Compatibility (PR #99 review, required before GO). NOT S6 evidence.**
  The current `net_bench` drove the `389b623` server through the runner's
  own driver on the Profile-A shape (5,000,000 objects, the S6 mix, 8
  shards, 8 threads).
  - Load and all four op kinds: errors 0, stale 0.
  - Two checkpoints completed; the server exited 0.
  - 238 samples, 0 failed; absent gauges reported as absent.
  - It was run on the R640 host, not a dev VM (none was reachable from
    this session), shortened (5 s window, 8 s checkpoint interval) and on
    the boot disk.
  - The review may require a dev-VM repeat before GO.

## 10. Still not authorized

S6 cells before the GO; CP-1A rerun; comparator systems; CRITERIA freeze;
S5/ioprio; B2; Agent-SNC; Rust; DPDK/VPP.
