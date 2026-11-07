# Step 1, IDs file on /dev/shm: result

Preregistration: `PREREG.md`, commit 7977258, pushed before any data. Driver and evaluation: `step1.py`.
Each rep ran once. No reruns. Raw data: `raw/`; computed values: `summary.json`.

## Verdict

**ELIMINATED. Under the preregistered rule, this means BENCHMARK DEFECT.**

In all 3 reps: 0 EXEC + INDEX refusals, `late_over_1ms` = 0, and no 100 ms sample with average write
await above 1.0 ms. No rep is CONFOUNDED: no server log has a `checkpoint:` line.

## Per-rep results

All at 2,183,233 ops/s offered, w30, pilot build, `--ids` on /dev/shm.

| Rep | EXEC refused | INDEX refused | p99 | p99.9 | Achieved ops/s | `late_over_1ms` | Max write await | Storage events | Checkpoints |
|---|---|---|---|---|---|---|---|---|---|
| r0 | 0 | 0 | 1.016 ms | 1.245 ms | 2,183,210 | 0 | 0.072 ms | 0 of 462 samples | 0 |
| r1 | 0 | 0 | 0.999 ms | 1.245 ms | 2,183,207 | 0 | 0.100 ms | 0 of 460 samples | 0 |
| r2 | 0 | 0 | 1.016 ms | 1.245 ms | 2,183,209 | 0 | 0.071 ms | 0 of 460 samples | 0 |

## Comparison with the diag2 reps

`--ids` on /srv/snc-data (the SSD holding the data dir) in both old arms. Values from
`../diag2-r640-20261106-telemetry/`. The old B arm had no disk observer. Event time is seconds after
`run_start`.

| Arm, rep | `--ids` on | Refused (EXEC + INDEX) | p99 | p99.9 | `late_over_1ms` | Samples above 1.0 ms: time, await |
|---|---|---|---|---|---|---|
| Step 1 r0 | /dev/shm | 0 | 1.016 ms | 1.245 ms | 0 | none |
| Step 1 r1 | /dev/shm | 0 | 0.999 ms | 1.245 ms | 0 | none |
| Step 1 r2 | /dev/shm | 0 | 1.016 ms | 1.245 ms | 0 | none |
| Old B r0 | SSD | 0 | 1.049 ms | 40.894 ms | 203,443 | not observed |
| Old B r1 | SSD | 256 | 0.999 ms | 1.245 ms | 203,881 | not observed |
| Old B r2 | SSD | 1,016 | 1.049 ms | 37.749 ms | 213,409 | not observed |
| Old T r0 | SSD | 5,945 | 1.049 ms | 29.884 ms | 189,964 | 26.84 s, 1.88 ms |
| Old T r1 | SSD | 169 | 1.049 ms | 33.554 ms | 204,464 | 34.02 s, 4.16 ms |
| Old T r2 | SSD | 34 | 1.049 ms | 34.603 ms | 198,616 | 30.32 s, 1.82 ms |

## Differences from the old B arm

- Required by the prereg: `--ids` location, port and rep paths, and the 100 ms disk observer on CPU 36.
- Run order: these 3 reps ran consecutively. Each old B-w30 rep ran right after a B-closed rep, and old
  T reps ran between them.

## Compliance

Step 1 only. No change to G0/G2. No proof, causal cell or pilot rerun. No engine, storage or hardware
setting change. Nothing pushed to main.
