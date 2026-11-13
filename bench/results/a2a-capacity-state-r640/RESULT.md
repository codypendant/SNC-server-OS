# A2-a capacity-state characterization: RESULT (for review)

Preregistration: `bench/results/a2a-capacity-state-prereg-20261112/PREREG.md` (revision 2, GO). Ran 2026-11-12 22:59 UTC to
2026-11-13 about 02:10 UTC from `main` `c86e43c`. K=5 sessions x 5 runs (`S0 C1 C2 C3 S1`) = 25 of 25 runs, `complete: true`,
every run returncode 0, no errors / overloaded / stale. Nothing rerun, nothing tuned, nothing hand-edited. `characterization.json`
was produced by `capstate.py analyze` (written and tested before any data). The A2-a STOP (PR #117) is untouched.

## Outcome: one state only (STOP per PREREG section 6), plus an instrument-validity flag on every run

**All 15 capacity runs are large-batch** (server-log frames/recv 15.4 to 15.9, frames/send 17.3 to 18.0; the frozen boundary is
15.0). **0 are normal-batch**, in all five sessions. Per the preregistration the separation analysis is not evaluated (it needs
at least 2 runs of each state) and **nothing is to be run again** until the other state is observed.

| | min | max |
|---|---|---|
| capacity, ops/s | 4,256,242 | 4,338,623 |
| frames/recv | 15.4 | 15.9 |
| frames/send | 17.3 | 18.0 |
| server effective MHz (P1a) | 3391.8 | 3391.8 |
| package power W (P1b) | 180.0 | 184.3 |
| server instructions/op (P2a) | 4,448 | 4,523 |
| server ctx switches/s (P2b) | 133.9 k | 139.3 k |
| client sendto/op, recvfrom/op (P3) | 0.273 to 0.280, 0.059 to 0.062 | |
| server pages on node 1 (P4a) | 0.00136 | 0.00137 |
| AnonHugePages kB (P4b) | 0 | 0 |
| lo bytes/packet (P5a) | 387 | 396 |
| WAL extents/file (P6a) | 1.03 | 1.19 |
| SSD temp at start (N1), C | 34 | 35 |

Anchors (S0/S1, 654,569 ops/s): all 10 held 654,567 ops/s, p99 0.95 to 1.03 ms.

## What this means, and what it does not

- The large-batch state that appeared in one of five A2-a sessions (session 2) appeared in **5 of 5** sessions here, with the same
  throughput band (A2-a session 2: 4.357 M median; here 4.26 to 4.34 M, vs 4.00 to 4.03 M in the other four A2-a sessions and 4.03 M in CP-1A).
- The runs here differ from A2-a in one known way: the **instrumentation** (3 `perf stat`, `sar`, snapshots on CPUs 34/70). PREREG
  section 0 item 3 accepted this as a potential observer effect. This experiment cannot separate "the instrumentation selects the
  large-batch state" from "the host is now in the large-batch state". No causal claim is made either way.
- The same-day sequence (A2-a mostly normal, this run all large-batch, same binaries, same resets) means the state is not set by the
  storage reset alone; it was not observed to vary within this experiment, so none of H1 to H6 can be separated by it.

## Instrument validity: defect flagged in all 15 runs (PREREG section 3 check)

The server-set `sys_enter_recvfrom` count over the window divided by `requests / (frames/recv)` is **0.81 to 0.82** in every run
(allowed: within 3%). By the preregistered rule this is **an instrument defect: the tracepoint columns are flagged, the runs are kept**.
The gap is consistent (not run-to-run noise), so the recvfrom-derived columns (P3b and the server `recvfrom` rate) should be read as
relative, not absolute. The cause is not investigated here (it could be the denominator definition in the server log or the
tracepoint scope; not checked). The non-tracepoint metrics (frequency from aperf/mperf, energy, instructions, ctx switches,
numa_maps, nstat, sar) are not covered by this check.

## Files

- `pilot.json`, `raw/`, `state/` (A1 snapshots per session), `instr/<run>/` per-run samplers, `characterization.json`.
- **`instr/*/sar.sa` (25 binary files, about 14 MB each, 350 MB total) are NOT committed** (size). They remain in
  `/home/solo/capstate-r640/instr/*/sar.sa` on the R640; their hashes are in `SHA256SUMS` (which covers all 962 original files).
  The `sar`-derived columns in `characterization.json` (P5a and the `sar`-based context) were computed from them.

## Not authorized, not done

Stable-peak bracket, duration Latin square, CRITERIA, competitor performance, REFINE #2, any rerun or second K.
