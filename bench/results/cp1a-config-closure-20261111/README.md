# CP-1A configuration closure: validation (2026-11-11)

Scope: [docs/handoff/PR106_S6_Refine1_Final_Review.md](../../../docs/handoff/PR106_S6_Refine1_Final_Review.md),
"NEXT AUTHORIZED TASK".
- **No benchmark run** and no CP-1A.
- No product algorithm change, and no change to any other pilot
  parameter.

## Change

| File | Change |
|---|---|
| `bench/pilot_plan.json` | `profiles.A.checkpoint` = {checkpoint_s 60, ckpt_pacing on, ckpt_walk_batch 16, ckpt_walk_min_us 20, ckpt_walk_max_us 65, ckpt_walk_ceiling_us 155, ckpt_walk_step_us 5, ckpt_target_ms 30000, ckpt_max_age_ms 60000, ckpt_io dsync, ckpt_window 1, ckpt_sync_tokens 0}. The top-level `checkpoint_s` (its only user was `pilot.py`) is folded in. Nothing else changed. |
| `bench/pilot.py` | `checkpoint_args(cfg)` maps the twelve keys to flags in a fixed order and refuses a missing or unknown key. `server_args(...)` builds the server command (Profile A: data dir + all twelve; Profile C: none). `Pilot.one()` uses it. |
| `bench/profile_check.py` | `PROFILE_REQUIRED["A"]`: the twelve flags at exact values, refused if missing or different, with no deviation route. For those flags it replaces the FIXED `--checkpoint-s` check on A. C is unchanged (the flags stay foreign to it); B and D get no R640 values. |
| `tests/test_pilot.py` | Profile-A command carries all twelve exact values and passes the guard; Profile C command unchanged; plan provenance (8 mutated plan values each change the command and are refused; a missing or unknown key raises) |
| `tests/test_profile_check.py` | `a_node` carries the configuration. New case: each of the 12 flags missing is refused, and 13 wrong values are refused: window 4 and 2, max 100, ceiling 250, min 10, step 10, batch 32, target 60000, max age 300000, pacing off, io legacy, tokens 4, checkpoint 30. The old walk defaults are refused as a set; a declared deviation does not open a route; C and B/D behave as before. |

## Plan hash

| | SHA-256 |
|---|---|
| before | `b62e75552b3188e10586a97f8f5d2a05d4b02bc4b0ce94f8bd8adcc961eaed49` |
| **after** | **`568e9874e61480647c600fe6c52f44965331081d1d82f7a5f01b2875a87968c7`** |

No file in the repository pinned the plan's hash. `pilot.py` records it
in every result. The manifest-e hash (`approved_manifest_sha256`) is
unchanged. The new hash is recorded here and in `docs/PILOT_PLAN.md`
before any CP-1A data.

## Validation

| | Result | File |
|---|---|---|
| `test_pilot`, `test_profile_check`, `test_s6`, `test_ckpt_cli` | 4/4 | `ctest-python-fast.log` |
| plus `test_lab_witness_guard`, `test_baseline_ha` (profile guard users) | 6/6 | `ctest-python.log` |
| `test_net_bench_rate` (imports `pilot`) | 1/1 | `ctest-python.log` (appended) |
| Mutations of the closure: pilot drops the window flag, falls back to `--checkpoint-s` only, skips the key check, or puts checkpoint flags on C; guard expects window 4, allows a missing flag, allows a deviation route, imposes values on B, or expects max age 300000 | **9/9 caught** | `negative/summary.txt` |
| Command-construction proof: `server_args` with the committed plan; the guard accepts A and C; the real `build-s6/snc_server` started with the generated Profile-A flags (temporary data dir, 1,000 objects, no load) and printed exactly the approved values | ok | `command-proof.txt` |

The server's startup line shows pacing, batch, min, max, ceiling, step,
update, target and max age. It does not print `--ckpt-io`,
`--ckpt-window` or `--ckpt-sync-tokens`. The binary accepted those flags,
and REFINE #1's final GAUGES (`ckpt_window=1`) showed the window taking
effect.
