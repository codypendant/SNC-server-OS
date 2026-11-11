# S6 runner and final preregistration: validation (2026-11-11)

Authorized by [handoff/PR99_S6_Pacing_CLI_Final_Review.md](../../../docs/handoff/PR99_S6_Pacing_CLI_Final_Review.md),
"Post-merge next authorized task". **No S6 cell was run.** Nothing here is
S6 evidence.

## Runs, in order

| # | What | Result | File |
|---|---|---|---|
| 1 | `tests/test_s6.py` (R1-R8, V1-V5, M1, T1, O1, C1, P1, F1, B1) | all pass | `ctest-python.log` (with `test_pilot`, `test_profile_check`, `test_ckpt_cli`: 4/4) |
| 2 | 19 mutations of `bench/s6.py`, each must fail run 1 | **19/19 caught**, each at the check it targets | `negative/summary.txt`, `negative/*.log` |
| 3 | `bench/s6.py OUT --scratch /dev/shm/snc-s6 --dry-run` at ba0089f | every command of the 15 cells; preflight **ok** (13 machine checks, 3 build hashes, isolation: data `sdb`, scratch and out `sda`); nothing started | `dry-run.txt` |
| 4 | `bench/s6.py OUT --scratch /dev/shm/snc-s6` (real mode) before the freeze | **REFUSED before any process**: `runner_commit` and `go_record` not set; no server or client ran | `real-run-refused.txt`, `real-run-refused.preflight.json` |
| 5 | Compatibility: current net_bench drives the 389b623 server (and the current one), through `Driver.execute` | compatible (below) | `compat/` |

**A finding during run 2.** The first mutation batch reused stale Python
bytecode (`bench/__pycache__`). Two mutated files had the same size and
were written in the same second, so some mutations ran the previous
mutation's code. That batch was discarded. Run 2 is the rerun with
`PYTHONDONTWRITEBYTECODE=1` and `-B`, the cache removed first.

## Compatibility check (run 5): NOT S6 EVIDENCE

`compat/compat_check.py` ran C0 then C, one rep each, through the runner's
own driver (commands, sampler, parsing).
- **Shape:** the Profile-A workload (5,000,000 objects, the S6 mix, owner
  routing, 8 shards, 8 client threads).
- **Shortened:** a 5 s closed-loop window and a checkpoint every 8 s, so a
  checkpoint happens.
- **Data on the boot disk**, not the data SSD.
- **Host:** the R640, because no dev VM was reachable from this session.
  The review may ask for a dev-VM repeat.

| | C0: pre-S1 server `78f471ef…` | C: current server `69a51b69…` |
|---|---|---|
| net_bench | `build-s6/net_bench` `9fc54944…` (the same binary) | the same |
| load 5,000,000 objects | ok | ok |
| run: errors / stale / index errors | 0 / 0 / 0 | 0 / 0 / 0 |
| refusals | 0 EXEC, 59 INDEX (a valid status of the old server, which has no S1 admission) | 0 / 0 |
| checkpoints completed | 2 (the first during the load) | 2 |
| server exit on SIGTERM | 0 (rule 7 holds) | 0 (rule 7 holds) |
| 100 ms samples | 238, 0 failed, max interval 100.01 ms | 233, 0 failed, max interval 100.01 ms |
| pacing / degraded / catch-up / WAL-above-high | **absent** (`null`), not 0 | present; walk budget 20,000-65,000 ns |
| startup pacing line | none (that server predates it) | `ckpt_pacing: pacing=on batch=16 min_us=20 max_us=65 ceiling_us=155 step_us=5 update_us=1000 target_ms=30000 max_age_ms=60000` |

Latency and throughput from this check mean nothing: boot disk, an 8 s
checkpoint interval, 5 s windows. They are not reported.

**Cleanup:** the compat data directory, `/dev/shm/snc-s6-compat`,
`/dev/shm/snc-s6` and the empty `/srv/snc-data/s6` the dry run created
were removed.

## Binaries (frozen in `bench/s6_frozen.json`)

| | SHA-256 |
|---|---|
| `build-s6/snc_server` (main 91278d7, fresh RelWithDebInfo) | `69a51b695e2029942039d386abaa9755616f31327ff63e0420dd0c5c8ba25ee1` |
| `build-s6/net_bench` | `9fc5494404747f8242680552588bd6f986e3e00b265e45ee67262c513a93c581` |
| `build-s6/ckpt_walk_cost` | `4707dac135a6fc72c41a712075678aa24ccbf7ad74d546962623026584b5d2d2`, identical to PR #98's |
| `build-preS1/snc_server` (389b623) | `78f471efb7e7951bf8cf14637e03120d6460dc989317c5c32fb073af23806ff7` |
