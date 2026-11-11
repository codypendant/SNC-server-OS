# CP-1A freeze interlock: validation (2026-11-11)

Scope: [docs/handoff/PR107_CP1A_Config_Closure_Final_Review.md](../../../docs/handoff/PR107_CP1A_Config_Closure_Final_Review.md),
"NEXT AND ONLY AUTHORIZED SLICE".
- **No CP-1A run**, and no `--quick` run on the R640.
- No change to the plan, the guard, the workload or C++.

## Change

**`bench/cp1a_frozen.json`:**
- `plan_sha256` `568e9874…68c7` and `manifest_sha256` `768056bb…27b6`, as
  approved;
- `runner_commit` **null** and `go_record` **null**.

**`bench/pilot.py`:**
- `freeze_problems()`, `freeze_gate()` and `identity_record()`.
- In real mode, `Pilot.environment()` checks the freeze **first**, before
  the rebuild and every other check, and refuses unless all of these hold:
  1. `runner_commit` is set and is a commit;
  2. it is an ancestor of HEAD;
  3. nothing changed after it except `docs/`, `AGENTS.md`,
     `bench/cp1a_frozen.json` and `bench/results/` (exact path matching);
  4. the plan's SHA-256 is the frozen one;
  5. the manifest's SHA-256 is the frozen one;
  6. `go_record` is set;
  7. the named GO file exists.
- The existing clean-tree, manifest, build, governor, NUMA and isolation
  checks follow, unchanged.
- `--quick` does not check the freeze. It records `freeze_checked=false`
  and `evidence=false`.
- Every run records the actual HEAD, plan, manifest and binary SHA-256
  (`identity_record`), and the freeze file and its problems.

**Why `runner_commit` is null and not PR #107's merge `85cc9b0`.** This PR
itself changes `bench/pilot.py`. With `85cc9b0` as the runner, rule 3
would refuse every later real pilot. As with S6's `runner_commit`, it
becomes **this PR's merge SHA**, recorded in a coordination-only commit
after review. Then come the GO record and `go_record`, then an independent
verification of the GO closure. The review's own sequence anticipates
this: "record exact freeze-interlock merge SHA if required by its design".

## Validation

| | Result | File |
|---|---|---|
| `tests/test_pilot.py` `test_freeze_interlock` against a real temporary git repo | see below | |
| — complete, correct freeze with docs, freeze-file, AGENTS and results commits after the runner | passes | |
| — each alone: runner unset; runner not a commit; runner a non-ancestor (side branch); wrong plan SHA; wrong or missing manifest SHA; GO unset; GO file absent | each refused | |
| — post-freeze change to `bench/pilot.py`, `bench/pilot_plan.json`, `bench/profile_check.py`, `tests/test_pilot.py`, C++ or a workload file | each refused | |
| — allowed-path matching is exact (`docsx/`, `AGENTS.md.bak`, `bench/results.py`, `bench/cp1a_frozen.jsonx` not allowed) | ok | |
| `test_freeze_gate_quick_is_not_evidence` | quick is unchecked and usable; real is refused; the committed freeze file refuses only for the missing runner and GO | |
| `test_identity_record_is_actual` | actual HEAD, plan, manifest and binary SHA-256 recorded | |
| Gate: test_ckpt_cli, test_profile_check, test_lab_witness_guard, test_pilot, test_s6, test_net_bench_rate | **6/6** | `ctest-python.log` |
| Mutations of the interlock: runner check, ancestor check, pilot.py allowed, prefix matching, plan check, manifest check, GO check, GO file check, quick checking the freeze, real skipping it | **10/10 caught** | `negative/summary.txt` |
| **Real-mode preflight at `850ad0a`** (`pilot.py --out … --manifest manifest-e --scratch /dev/shm/snc-pilot --build build-s6`) | **refused before any build or process**, for exactly `runner_commit` and `go_record` (plan and manifest SHA matched); no binary rebuilt, no data or scratch directory, nothing started | `real-mode-refused.txt` |
