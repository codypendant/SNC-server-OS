# Non-perturbation proof and targeted cells: preregistration

Written and committed before any data. Telemetry as specified in the accepted report
(`bench/results/diag-r640-20261106-cp1a/REPORT.md`, "Minimal proposed next experiment"). No separate
work-order text was found in the repo.

Arms: **B** = `build-pilot` (51a5cb6), exact pilot command, no telemetry. **T** = `build-diag`, with
`--diag-gauges` (50 ms, sampler on CPU 0), `--diag-buckets` (50 ms), and /proc/diskstats at 100 ms
(CPU 36). Same CPUs, binding, objects, mix, route and depths as the pilot's Profile A.

Proof cells, 3 reps, order B,T repeating: closed loop depth 512 w60; 2,183,233 ops/s w30.

Gates, using medians over 3 reps. All must pass, or no causal cell runs:
- G0 machine unchanged: B closed within 5% of 4,366,466; B w30 p99 <= 1.5 x 1.05 ms; B w30 has 0 refusals.
- G1 throughput: T closed >= 0.97 x B closed.
- G2 refusals: T w30 has 0 refusals in every rep.
- G3 latency: T w30 p99 and p99.9 each <= max(1.25 x B median, B max).
- G4 achieved: T w30 >= 0.995 x B w30.

Causal cells, T only, 3 reps, only if all gates pass: 436,646 with checkpoint (w60); 436,646 without
checkpoint (w60, diagnostic only); 2,183,233 with checkpoint (w60). Closed loop and 2,183,233 w30 data
come from the proof's T reps.

Each run happens once, with no reruns or changes after a result. Any failure stops the driver.
No full pilot rerun, no competitors, no criteria or threshold change, no engine optimization, no disk
configuration change.
