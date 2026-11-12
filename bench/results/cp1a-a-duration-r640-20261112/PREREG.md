# CP-1A Profile-A objective-rate duration validation: preregistration (before data)

Authorized by [docs/handoff/PR111_CP1A_Result_Final_Review.md](../../../docs/handoff/PR111_CP1A_Result_Final_Review.md)
("NEXT AUTHORIZED TASK"). This is a **pilot-method validation, not REFINE round 2**.
No product code change, no tuning, no threshold change, no competitor.

Committed before any validation data exists. Nothing here changes after the first run starts.

## Question

Is the 60 s Profile-A measurement window long enough at the CP-1A objective rate? Or do p99 and
sustainability drift when the window grows to 120 s and 240 s, with 2 and 4 periodic checkpoints?

The CP-1A duration check could not answer this. It ran at 2,181,897 ops/s, the largest grid rate
≤ P0/2, which is about 2 × A's stable peak S = 1,090,948 (PR #111 review).

## Cells

| | |
|---|---|
| Rate | **654,569 ops/s** exactly: the CP-1A A objective, from the preregistered algorithm (`results.A.sweep.objective_rate`) |
| Windows | **60 s × 3, 120 s × 3, 240 s × 3** = 9 runs |
| Warm-up / drain | 10 s / 10 s (`pilot_plan.json` `warmup_s`, `drain_s`) |
| Per rep | fresh server and data dir, load, one measured open-loop `net_bench` run (`--depth 1024`) |
| Order | `random.Random(order_seed = 20261106)` shuffle of (window, rep), as the pilot shuffles: **120s-r1, 120s-r0, 120s-r2, 60s-r2, 240s-r2, 240s-r0, 240s-r1, 60s-r1, 60s-r0** |
| Data root | `/srv/snc-data/aduration` (the data SSD), removed afterwards |
| Scratch | `/dev/shm/snc-aduration`, isolated from the data SSD (pilot isolation check) |
| Out | `/home/solo/cp1a-a-duration-r640-20261112` (outside the tree, so it stays clean), copied in afterwards unchanged |

All else is the accepted CP-1A Profile-A configuration, unchanged, and comes from `bench/pilot_plan.json`
through `bench/pilot.py`, both frozen at runner commit `4250b32`. That is: 5 M objects; mix
read 70 / deposit 15 / transfer 10 / index 5; owner routing; 8 shards; 8 client threads; PILOT-L
CPUs and NUMA binding; manifest e; Profile-A checkpoint `--checkpoint-s 60 --ckpt-pacing on
--ckpt-walk-batch 16 --ckpt-walk-min-us 20 --ckpt-walk-max-us 65 --ckpt-walk-ceiling-us 155
--ckpt-walk-step-us 5 --ckpt-target-ms 30000 --ckpt-max-age-ms 60000 --ckpt-io dsync --ckpt-window 1
--ckpt-sync-tokens 0`; and `profile_check` guard.

## Driver: `aduration.py` (here, beside its test)

`bench/pilot.py` is imported **unchanged**. The driver lives under `bench/results/`, as the S6
write-window and REFINE #1 drivers did (PRs #105, #106), so no frozen file changes and the CP-1A freeze
gate still applies.

- **Preflight:** `pilot.Pilot(...)` runs the CP-1A real-mode preflight as it is. That covers the freeze
  gate (runner-commit ancestry, post-freeze allowlist, plan and manifest SHA, GO record), clean tree,
  rebuild of `build-s6`, RelWithDebInfo with no flags, governors `performance`, `numa_balancing` 0,
  manifest e, scratch/data/out isolation and the committed plan.
- **Extra refusal:** the driver also refuses unless the rebuilt `snc_server` and `net_bench` SHA-256 and the
  plan and manifest SHA-256 equal the CP-1A record (`69a51b69…`, `9fc54944…`, `568e9874…`, `768056bb…`).
  It asserts that the rate and CV_p99 reproduce `bench/results/cp1a-r640-20261111/pilot.json`.
- **Runs:** every run goes through `Pilot.one(..., rate, depth=rate_depth, window=(10, w))`. That is the
  same call the pilot's duration check makes (`memory=False`).
  - Design point: the CP-1A fixed reps used `memory=True`, which adds the memory settle pauses before the
    run. These runs do not. Otherwise the server and client commands are byte-identical to CP-1A
    `A-fixed-r0` except `--seconds`, ports, data dir and ids path (`test_aduration.py` A2).
- **Abort:** a server exit, a failed load or an unparsable report aborts the session (pilot's `Abort`).
  The verdict is then INCOMPLETE. No rerun, no replacement rep.

## Decision (fixed before data; PR #111 review)

**Sustained:** each rep is judged by the existing `pilot.sustained()`, unchanged. A rep is sustained only
with zero errors (EXEC, INDEX, stale), zero refusals (EXEC `overloaded`, INDEX `refused`),
completed-in-window ≥ 99/100 of offered × window, end backlog ≤ 1/100 of scheduled-in-window, and drained.

**Drift bound:**
- D = 3 × CV_p99_fixed_A = 3 × 0.019286005545956553 = **0.0578580166** (exactly 3 × the recorded float,
  as a `Fraction`). D is not changed after observing the runs.
- M60 = the median total p99 of the three 60 s reps, a measured value.
- For 120 s and for 240 s, the median total p99 of that cell must be ≤ M60 × (1 + D). The comparison is
  exact, and equality passes.

**PASS** if all 9 reps are sustained **and** the 120 s median p99 ≤ M60 × (1 + D) **and** the 240 s
median p99 ≤ M60 × (1 + D). Then the 60 s Profile-A window is validated, the CP-1A results stand, and the
next step is the formal `CRITERIA.md` review. No full CP-1A rerun.

**FAIL** if any rep is unsustained or failed, or either bound is exceeded. Then STOP and return for
review. No longer window is selected after seeing the result. A revised window would need a methodology
revision and an affected Profile-A pilot rerun. This is not automatically REFINE round 2.

**INCOMPLETE** (an abort): STOP and return for review. Not a PASS.

**Report only:** p99.9, max, the per-kind latencies, issue lateness, and the periodic checkpoints per run
(counted from the server log, excluding the recovery checkpoint). They do not enter the verdict.

## Commands

```
cd /home/solo/SNC-server-OS      # HEAD = this preregistration commit, tree clean
PYTHONDONTWRITEBYTECODE=1 python3 -B bench/results/cp1a-a-duration-r640-20261112/test_aduration.py
python3 -B bench/results/cp1a-a-duration-r640-20261112/aduration.py run \
    /home/solo/cp1a-a-duration-r640-20261112 --scratch /dev/shm/snc-aduration \
    --manifest bench/manifests/r640-main-20261111.txt --build build-s6
python3 -B bench/results/cp1a-a-duration-r640-20261112/aduration.py judge <copied-in out dir>
```

Expected wall time is about 30 min: 1,260 s of windows plus 9 × (load, warm-up, drain, restart).

## Validation before data

- `test_aduration.py` passes (A1 constants, A2 run path through `Pilot.one` with stubbed processes, A3
  verdict boundaries). It starts no process.
- **Mutations:** `negative/summary.txt` covers 12 mutants of `aduration.py` (rate, D, windows, reps,
  inclusive bound, bound ignored, mean and max for median, unsustained ignored, incomplete ignored,
  warm-up, order). **12 / 12 are caught.**
  - The first pass caught 10. `bound-strict` and `order-unshuffled` survived. The test then gained an
    exactly integral bound case and the pinned order, and the suite was rerun.
- No file outside `bench/results/cp1a-a-duration-r640-20261112/` changes in this commit.
