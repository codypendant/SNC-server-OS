# S6 REFINE round 1 (of 2): W=1 configuration rescue: preregistration

Committed **before any rescue data**, under
[docs/handoff/PR105_S6_Write_Window_Review.md](../../../docs/handoff/PR105_S6_Write_Window_Review.md).

- The original S6 result stays **FAIL (rule 4 p99.9)**. Its evidence
  (`bench/results/s6-r640-20261111/`) is read here, never edited.
- The W1/W2 characterization does not count as a rescue round. This rerun
  is round 1.

## 1. The fix (one bounded change)

Every current-server checkpoint cell adds `--ckpt-window 1`. This is the
existing S3 C3 mechanism, configurable and covered by tests at W=1.
Unchanged:
- no engine, algorithm or compiled-default change;
- no adaptive window;
- no pacing, threshold or criterion change.

## 2. Cells (rep-major P1, H1, C1; 3 reps each)

| Cell | Definition | Role |
|---|---|---|
| **P1** | S6 P (436,646 ops/s open loop, checkpoint 60 s) + `--ckpt-window 1` | **primary: judged** |
| H1 | S6 H (2,183,233 open loop, checkpoint) + `--ckpt-window 1` | reported only, not judged |
| C1 | S6 C (closed loop depth 512, checkpoint) + `--ckpt-window 1` | capacity numerator |

`test_refine1.py` A1 asserts that each command is the S6 command plus
`--ckpt-window 1`, with ports, data directory and ids file masked.

**Reused, not rerun** (the S6 session, read-only):
- **N:** the p99 reference R = median(655,360, 638,976, 638,976) =
  **638,976 ns**, so the p99 limit is **958,464 ns**. All three N reps
  are usable.
- **C0:** the capacity denominator, median **4,016,161 ops/s**. The guard
  is median(C1) ≥ 95% × 4,016,161, i.e. at least 3,815,352.95, and C1 rule
  7 must hold in all reps.

`refine1.references()` recomputes both from `summary.json` and refuses to
run if either differs.

**Unchanged from S6 (bench/s6.py, frozen at 63440b1, imported unchanged):**
- binaries: `snc_server` `69a51b69…`, `net_bench` `9fc54944…`; the pre-S1
  hash is checked too, though not run;
- the R640, manifest e, storage state and isolation: S6's preflight and
  postflight;
- PILOT-L CPUs and node 0; the sampler on CPU 0, the observer on CPU 36;
- Profile A, 5,000,000 objects, the rates, depths, warm-up, window and
  drain;
- checkpoint 60 s, pacing 20/65/155/5 µs with batch 16, target 30 s, hard
  maximum 60 s;
- 100 ms sampling with no catch-up;
- the checkpoint-window proof for P1, H1 and C1;
- the VOID and rerun policy.

New paths only: data under `/srv/snc-data/s6r1` (the same SSD), scratch
`/dev/shm/snc-s6r1`, output `run/` here.

## 3. Rules (the original S6 rules, unchanged: `s6.p_rules`, `s6.capacity_guard`)

**Each P1 rep:**
- zero EXEC and INDEX refusals;
- `late_over_1ms` × 1000 ≤ `scheduled_in_window`;
- total p99 ≤ 958,464 ns;
- total p99.9 ≤ 10,000,000 ns;
- `emits_peak` < 1,024 on every shard;
- checkpoint completion < 60,000 ms, and no degraded checkpoint;
- client rc 0; errors, stale and index errors 0; drained; server alive
  through the run with a clean exit; no failure marker.

**Capacity:** median(C1) ≥ 95/100 × 4,016,161, and rule 7 in every C1 rep.

**REFINE #1:**
- **PASS:** all three P1 reps pass every rule, the capacity guard holds,
  no session VOID, and identities stay valid (postflight).
- **FAIL:** otherwise, unless VOID. A genuine FAIL is not rerun, W is not
  changed, and no criterion is weakened.

H1 is reported only.

## 4. Mechanics

- `refine1.py run run --scratch /dev/shm/snc-s6r1`, **once**.
- VOIDs follow S6 section 7: a window or harness VOID is rerun once, at the
  end; a second window VOID, or VOID again on a rerun, makes the session
  VOID.
- Whatever the result, it is committed and goes to a result-review PR.
- No CP-1A.

## 5. Validation before data

- `test_refine1.py`:
  - A1: commands, order and window proof;
  - A2: the references reproduce R = 638,976 and the C0 median 4,016,161;
  - A3: verdict boundaries at p99 958,464/958,465, p99.9 10 ms, every
    other P rule, capacity 3,815,353/3,815,352 (median, not mean), C1 rule
    7, H1 not judged, VOID, incomplete.
- `negative/`: 7/7 mutations of `refine1.py` caught (window 2, no window
  flag, H1 judged, wrong R, wrong C0 median, capacity ignored, VOID
  ignored).
