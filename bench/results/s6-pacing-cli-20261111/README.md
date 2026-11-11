# S6 pacing CLI plumbing: validation (R640 host, 2026-11-11)

Scope: [handoff/PR98_S2_Pacing_S6_Prereg_Final_Review.md](../../../docs/handoff/PR98_S2_Pacing_S6_Prereg_Final_Review.md)
section 7. No S6 cell was run. No performance data is in this directory.

## Change

- **`tools/snc_server.cpp`.** Five new flags set the existing
  `Engine::Config` walk fields:
  - `--ckpt-walk-batch N` (1 to 4,294,967,295);
  - `--ckpt-walk-min-us N`, `--ckpt-walk-max-us N`,
    `--ckpt-walk-ceiling-us N`, `--ckpt-walk-step-us N` (1 to 60,000,000 µs,
    the Engine's 60 s bound; converted to ns).

  Parsing and validation:
  - Digits only: zero, negative, `+`, leading space, decimal, malformed,
    empty and overflow are all refused.
  - `min <= max <= ceiling` is checked on the effective values (flags over
    defaults) before `Engine` is built.
  - Every refusal exits 2 with a message. Without this check, the same
    inputs reached `Engine::Engine`'s `std::invalid_argument` uncaught
    (abort, rc -6; see the negative checks below).

  Startup output: one line with the effective configuration, after
  `snc_server shards=...`:
  `ckpt_pacing: pacing= batch= min_us= max_us= ceiling_us= step_us= update_us= target_ms= max_age_ms=`.
- **`tests/test_ckpt_cli.py`** (C1-C5), registered in `CMakeLists.txt`.
- **`docs/S2_PACING.md`:** the "as built" row lists the flags.
- **Not changed:** anything under `src/` or `include/`, the compiled
  defaults, the pacing algorithm, thresholds, checkpoint order, the
  storage path. `git diff 0dbd4c7 HEAD -- src include` is empty.
- **Walk identity:** the rebuilt `ckpt_walk_cost` SHA-256 is
  `4707dac135a6fc72c41a712075678aa24ccbf7ad74d546962623026584b5d2d2`,
  identical to the binary characterized in
  `bench/results/r640-s2-pacing-20261111` (S6_PREREG section 2 criterion).
  The walk measurements therefore still apply.

## Runs (`runs.txt`)

| # | What | Code | Result | File |
|---|---|---|---|---|
| 1 | `test_ckpt_cli` alone | b9ec45e | C1-C5 pass: defaults unchanged; S6 set 20/65/155/5/16, target 30000, max age 60000; each flag alone; 56 refusals with exit 2; the 60 s and u32 bounds accepted | (console) |
| 2 | Negative checks: 5 mutations of the change, each must fail the test | b9ec45e | 5/5 fail | `negative/` |
| 3 | **Full suite, RelWithDebInfo**, `ctest -j1` | b9ec45e | **48/48** | `relwithdebinfo-j1.log` |
| 4 | **Full suite, ASan+UBSan** (Debug, `-fsanitize=address,undefined`, `detect_leaks=1`, `halt_on_error=1`, `log_path` set for server children), `ctest -j1` | b9ec45e | **48/48, 0 reports** (no `log_path` file written; 0 report lines in the log) | `asan-j1.log`, `asan-build.log` |

TSan was not run. The change is argument parsing and one `printf` on the
main thread before the Engine (and any of its threads) exists. Say if
TSan is wanted.

## Negative checks (run 2)

| Mutation | Fails at | How |
|---|---|---|
| order check removed | C4 `min 70, max 65` | rc -6: uncaught `std::invalid_argument` from Engine |
| zero accepted | C4 `--ckpt-walk-batch 0` | rc -6, the same |
| 60 s bound removed | C4 ceiling 60000001 | rc -6, the same |
| `--ckpt-walk-max-us` parsed but not applied | C2 | startup line shows max_us=100 |
| digits-only check removed | C4 `--ckpt-walk-batch " 5"` | accepted: server started (timeout) |
