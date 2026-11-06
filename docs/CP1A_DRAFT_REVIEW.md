# CP-1A draft (from the R640): review

*Status: REVIEW of a DRAFT. Nothing here is authorized to run as the
pilot. The draft and this review go to cody/Codex together. The stopped
pilot's output is **not evidence**.*

## 1. What happened

On 2026-11-06 a Claude Code session running on the R640 was asked to
capture manifest c. Following cody's instruction to go ahead, it also:
- wrote a CP-1A pilot harness;
- set `kernel.numa_balancing=0`;
- started the full SNC-only pilot.

That was before the CP-1A review that CAMPAIGN_PREP section 8 orders.
cody stopped the pilot during its second run. One of 90 planned runs
finished, all in Profile A; Profile C never ran. The session pushed its
work as branch `r640-cp1a-draft` (18faa4d).

This PR carries that work, minus two files:
- `raw/A-duration-120s.ids`: 80 MB of binary account IDs left by the
  killed run;
- `uncommitted.diff`: a copy of the `net_bench` change that is already in
  the commit.

The rest is unchanged except that `tests/test_pilot.py` is now registered
in ctest.

| File | What it is |
|---|---|
| `bench/net_bench.cpp` | `--index PCT`: that share of operations are `INDEX` lookups of a random loaded account's email, reported apart from the mix (gap G5) |
| `bench/pilot.py` | the runner: phases, pinning (R640_TOPOLOGY section 4), per-run metrics, CV, knee rule |
| `tests/test_pilot.py` | arithmetic tests: CV, knee rule, per-op metrics, parsing |
| `bench/results/pilot-r640-20261106/` | `PILOT_PLAN.md` and the stopped pilot's output (one finished run). **Not evidence** |

## 2. Checked here (dev VM, not performance)

- `tests/test_pilot.py`: passes. It is now in ctest as `test_pilot`.
- **`net_bench --index` functional smoke:** 2 shards, 50K objects,
  `--index 20`, 2 s. 315,505 index lookups, all OK, 0 empty, 0 errors.
  Every generated email matches what `load()` created, using the same
  `user<thread>.<n>@example.com` scheme.
- **Full Release ctest with the change: 33/33 passed** on the dev VM
  (`bench/results/pilot-r640-20261106/review/ctest-release-devvm.log`).
  The ASan/TSan suites were not run.

## 3. Findings

**F1, blocking for the p99 objective: the sweep is closed-loop.**
EXPERIMENTAL_BASELINE 10.2 asks for an offered-load sweep. `net_bench`
keeps a fixed number of requests in flight (`--depth`) and has no rate
control, so the draft sweeps depth instead.
- In a closed loop a slow response delays the next send. Stalls then
  suppress the requests that would have measured them (coordinated
  omission), so p99 is under-reported exactly where the objective is
  read.
- A competitor driven at a fixed rate later would not be measured the
  same way.
- **Proposed:** an open-loop mode for `net_bench` (section 4) before
  the real pilot.

**F2, blocking: the frequency policy is unset.** After cody switched the
BIOS to "Performance", the OS took over frequency control, and the
governor is `schedutil`, which scales with load.
- **Proposed:** set the governor to `performance` on all CPUs for the
  campaign, record it in manifest c or d, and check it at the start of
  every run (the runner already records it).

**F3, the knee rule is exact but tied to depth.** The draft's rule meets
the deterministic requirement (EXPERIMENTAL_BASELINE 10.2) and its tests
pin it. It must move to the rate sweep (section 4). The 0.70 fraction
stays PROPOSED until the criteria review.

**F4, process and housekeeping.**
- `pilot.json` records the SHA-256 of an earlier manifest capture.
- Manifest c is now committed (`bench/manifests/r640-main-20261106c.txt`, R640_TOPOLOGY 2b).
- The ASan/TSan suites have not been run on the `net_bench` change.
- `kernel.numa_balancing=0` was set before manifest c; it is a planned
  run-time setting (R640_TOPOLOGY section 4), so it is kept and recorded.

**F5, metric definitions (minor, record only).**
- CPU per op uses the server's `utime + stime` from `/proc/<pid>/stat`.
  That covers all of its threads, including io_uring workers;
  `cutime`/`cstime` are 0 because the server has no children.
- Loopback bytes per op sums lo `rx` and `tx`, so every byte is counted
  twice. It is reported as loopback only, never as network cost
  (CAMPAIGN_PREP section 4), but the doubling must be stated or halved.
- p99 comes from a bucketed histogram. The bucket width bounds the
  resolution of N for p99.

**F6, things that are fine.**
- Every run starts a new server and a new data dir.
- Runs within a phase use a fixed-seed shuffled order.
- Server CPUs are 2,4,…; client CPUs are on node 1; both have explicit
  `--membind`.
- `profile_check` gates every command line.
- `pilot.json` is rewritten after each run, so a stopped pilot keeps
  what it measured.
- The `--seconds 0` load step runs a few mixed operations outside the
  measured window. That is harmless, and the plan states it.

## 4. Proposed fix for F1 and F3 (design; not implemented)

**`net_bench --rate R`** (operations per second, total across threads):
- Each thread sends on a fixed schedule at R/threads per second (a
  constant interval, seeded per thread).
- Latency is measured from each request's **intended** send time, so a
  stall is charged to every request it delays.
- If no slot is free when a send is due, the request waits, and its
  wait counts in its latency. `--depth` only bounds memory.
- It reports the offered rate, the achieved rate, and the late-send
  backlog at the end.

**Rate sweep and knee rule (proposed, exact):**
1. **Calibration:** one closed-loop run at depth 512 gives P0, which is
   used only to place the grid.
2. **Grid:** offered rates R_k = 0.05·k·P0 for k = 1…22 (5% to 110% of
   P0), 3 reps each, in fixed-seed shuffled order.
3. **Sustained:** a point counts as sustained if every rep has 0
   errors, achieved ≥ 0.99·offered, and an end backlog ≤ 1% of the
   operations sent.
4. **Stable peak** S = the largest offered rate whose point and all
   lower points are sustained.
5. **Objective** = the median p99 over the reps at the largest grid
   rate ≤ f·S, with f = 0.70 PROPOSED.
6. The knee is reported as the smallest rate whose median p99 exceeds
   2× the median p99 at R_1. It is reported only; the objective does not
   use it.

Given the same data, these steps always pick the same points.

## 5. Asks

1. **cody:** set the governor to `performance` (F2)? Claude recommends
   yes.
2. **cody/Codex:** authorize implementing `--rate` and the rate-sweep
   rule (section 4) as the CP-1A fix, or choose another method.
3. **Before any real pilot:**
   - manifest c (or d, after the governor change) is committed;
   - the ASan/TSan suites pass;
   - PILOT_PLAN.md is rewritten for the rate sweep and reviewed.
