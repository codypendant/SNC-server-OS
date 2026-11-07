# Step 1, IDs file on /dev/shm: preregistration

Written, committed and pushed before any data. Authorized by `docs/handoff/PR62_Codex_Review.txt`
(on main), Step 1 only. Diagnostic only. Driver and evaluation code: `step1.py`, committed with this file.

## Arm

B only: `build-pilot` (51a5cb6; `snc_server` sha256 04030ccb..., `net_bench` sha256 a5f085d4..., the
hashes recorded in the pilot's `pilot.json`). Exactly the diag2 B-arm 2,183,233 ops/s w30 command: same
CPUs, binding, objects, mix, route, depth, warm-up, drain, data dir on /srv/snc-data, checkpoint interval.

**One difference only:** `--ids` points to a fresh file under `/dev/shm/`, deleted after each rep.

Necessarily different and not treated as a variation: the port number, and the rep's directory and file
names (`/srv/snc-data/step1/S-w30-rN`, `/dev/shm/step1-S-w30-rN.ids`). `step1.py --dry-run` compares the
commands with the logged diag2 B-w30 commands.

## Observer

/proc/diskstats for the data SSD (`sda`, verified as the device of /srv/snc-data) every 100 ms, pinned to
CPU 36, as in the T arm. No gauges, no buckets. The old B arm had no observer; this one is required here.

## Reps

3, run once each, no reruns. Any failure stops the driver; a stopped or failed rep is void and not rerun.

## Outcome rule (Codex's words)

- **ELIMINATED:** in ALL 3 reps, 0 EXEC + INDEX refusals, `late_over_1ms` = 0, and no storage event. A
  storage event is any 100 ms sample where the data SSD's average write await is above 1.0 ms. This means
  BENCHMARK DEFECT.
- **Anything else:** the ID-file hypothesis is REJECTED. Do not move other files or try variations.

A checkpoint logged inside a rep's measured window makes that rep CONFOUNDED. Report it; do not rerun.

## Definitions fixed before data

These settle details the rule leaves open. All use the stricter reading.

- **Refusals:** `overloaded` (EXEC) and INDEX `refused` from the run section of the client log, which
  count the whole run including warm-up and drain.
- **`late_over_1ms`:** from the run section's issue-lateness line, also the whole run.
- **Average write await for a sample:** delta `w_ms` / delta `w_ios` between consecutive samples. A sample
  with no completed writes has no await and is not an event.
- **Samples evaluated:** every sample the observer takes, from server port open to `net_bench` exit,
  including the load phase, not only the measured window. Old T-w30 load phases peaked at 0.07 ms.
- **Checkpoint:** any server log line beginning `checkpoint:`. These lines have no timestamp, so any such
  line marks the rep CONFOUNDED. The start-up `recovery: checkpoint before serving` line does not count.
- **A CONFOUNDED rep is not rerun.** The verdict rule is still applied to all 3 reps as measured, and the
  confound is reported beside it.

## Limits

No change to G0/G2. No proof, causal cell, or pilot rerun. No engine change. No storage or hardware
setting change. No push to main.
