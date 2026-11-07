# CP-1A R640 pilot abort: for Codex review

Status: **awaiting review.** No R640 run happens until this is answered
(cody, 2026-11-07). Written by Claude from the evidence pushed by the R640
session. Nothing below changes the plan, the gates or the engine.

## Evidence (branches, not merged)

| Branch / commit | What |
|---|---|
| `r640-pilot-cp1a` 1837358 | Pilot results: `bench/results/pilot-r640-20261106-cp1a/` |
| `r640-pilot-cp1a` 5ffe278 | First diagnosis: `bench/results/diag-r640-20261106-cp1a/REPORT.md` |
| `r640-diag-telemetry` 395418b, 6fdc74c | Diagnostic telemetry (off by default) and the preregistered proof (`PREREG.md`) |
| `r640-diag-telemetry` 50092d0 | `DIAGNOSIS.md` rev 2, 12 completed proof cells, `gates.json` |

The R640 Claude session ended between 6fdc74c and 50092d0. The host did
not reboot (up since 2026-11-06 17:51 UTC, before the pilot), and all 12
proof cells are complete, so no run is void.

## What happened

1. The pilot aborted correctly under the approved rule: P0 = 4,366,466
   (closed loop, depth 512); R_1 = 218,323 sustained in 3/3; R_2 = 436,646
   refused in 3/3, so S = R_1 and no grid rate is at or below 0.70 x S.
   C3 did not fire: `client_limited_suspect` was false at the boundary.
2. The proof stopped on G0 and G2, so the causal cells never ran.
   G1, G3 and G4 passed. G0/G2 failed only on their zero-refusal clauses:
   the unchanged pilot build at 2,183,233 w30 refused 0, 256 and 1,016.
   The pilot's single w30 zero was a favourable draw.

## Mechanism (measured closed-loop; open-loop link is one A/B rep)

- Checkpoint walk, about 1.6 s: costs CPU on the shard threads, throughput
  about 2.0M ops/s.
- Checkpoint `fdatasync`, about 1.1 s: all 8 shards on the WAL's SSD at
  once, about 1,090 MB/s at 96% utilisation. WAL write await goes from
  about 0.1 ms to 7-9 ms, and durable capacity falls to 0.35-0.57M ops/s.
- 436,646 sits inside that range, which explains the transition and the
  spread of refusals across reps (598 to 23,726).
- Refusals come from the per-shard emit backlog gate
  (`emits_in_flight() > 1024`, checked before the op is looked up, so reads
  are refused too) and, at high rates, from SRI ring overflow (INDEX).
- First diagnosis A/B at 436,646: 26,770 refusals with a checkpoint, 0
  without (one rep, perturbed harness).
- One further short stall per run without a checkpoint is UNRESOLVED.
  The suspect is `net_bench` writing its 80 MB `--ids` file buffered to
  the WAL's SSD just before the run; the timing does not fully fit.

## Decisions requested

1. **Step 1 (about 4 min):** pilot build, 2,183,233 w30, `--ids` on
   `/dev/shm`, 3 reps. If the stall goes, it is a benchmark defect (fix the
   harness, add a test, re-preregister). Approve?
2. **G0/G2:** they assumed zero refusals in w30, which is not true of this
   harness. Keep them and stop, revise them (re-preregistered before any
   run), or waive them for the causal cells only?
3. **Causal cells (about 15 min):** 436,646 with and without checkpoint,
   2,183,233 w60, T arm, 3 reps. Approve, given (2)?
4. **Classification.** The R640 session says EXPECTED ARCHITECTURAL LIMIT
   (no document promises paced checkpoints). Claude's view: the result is
   valid falsification evidence either way, but two behaviours look like
   design gaps rather than limits and should be recorded as such:
   - backpressure on durable writes refuses reads, which need no WAL;
   - checkpoint writes and `fdatasync` run unpaced on the WAL's device.
   Neither should be fixed on the strength of this result alone, and
   neither fix is authorized.
5. **Telemetry branch.** `r640-diag-telemetry` touches engine code
   (`include/snc/diag.h`, `src/core/shard.cpp`, `tools/snc_server.cpp`),
   off by default and diagnostic only. It was not in the authorized scope.
   Keep it off main until you decide whether it may stay as a
   diagnostic tool.

## Not done, not authorized

No pilot rerun, no Profile C, no competitors, no CP-1B, no gate, grid,
threshold, engine, storage or hardware change.
