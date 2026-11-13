# PR #122 independent QA report (third verifier)

A fresh agent that had not seen the implementer's reasoning verified PR #122 against code and by running
programs. It reviewed head c03e902 (commits abb2a88 and c03e902). Its report follows as delivered, lightly
reformatted. The two issues it raised are fixed in 98588d1 (see "Disposition" at the end).

---

Summary: one docs overstatement that should be fixed before merge, and one freeze consequence the PR does
not document.

Outside its scratch copy it changed nothing in the repo except one `git fetch origin m1-0-minimal-ci`, which
updated a remote-tracking ref. It built and ran tests in a clone under the scratchpad, then deleted it.

## 1. ci.yml: CONFIRMED (two nits)

- The YAML parses. Triggers are pull_request (any base), push to main, cron "17 7 * * *" and
  workflow_dispatch. `fast` has no `if`; `full` is skipped on pull_request.
- Nothing lets a step pass while tests fail:
  - There is no continue-on-error, `|| true` or `set +e`.
  - Proven on GitHub: run 43369215185 (abb2a88, -j nproc) failed. test_net_bench_rate failed, 39/40 passed,
    the ctest step failed, the job was red.
  - Upload ran anyway (`if: always()`): "Artifact ctest-fast has been successfully uploaded", 3 files.
  - Run 43370027787 (c03e902, -j1) passed: 40/40, 357.85 s of tests, job 7m15s. The `full` job has never run
    on a hosted runner.
- fetch-depth 0 is required:
  - In a depth-1 clone, test_pilot fails with "runner_commit 4250b32… is not a commit in this repository".
  - In an orphan-history copy it fails with "…is not an ancestor of HEAD". Full history passes.
- ctest 3.28.3 flags:
  - `-LE` is a regex. `slow` is the only LABELS value in the repo, so it matches only those tests.
  - `--timeout` sets the default only. The TIMEOUT 2400 property (test_s3_writeback, test_lifecycle,
    test_s4_sync_tokens, test_intake, test_xnode) overrides it, which is fine.
  - `--output-junit junit-fast.xml` with `--test-dir build` wrote build/junit-fast.xml, matching the upload
    path.
- Nits: actions are pinned by tag, not SHA. GitHub warns that checkout@v4 and upload-artifact@v4 target the
  deprecated Node 20.

## 2. slow label: CONFIRMED

- `ctest -N` lists 49 tests and `ctest -N -LE slow` lists 40. The difference is exactly the 9 labelled tests;
  `-L slow` returns the same 9.
- The full job has no `-L`/`-LE`, so all 49 run there.
- No add_test was removed, and nothing in the diff disables, skips or sets WILL_FAIL on a test.
- The 9 are exactly the tests at 60 s or more in docs/audit-20261113-working-notes/qa/ctest_serial.log
  (test_s1 60.2 s up to test_sync_readiness 285.7 s). test_xnode_proc (50.4 s) is the largest fast test.
- witness_state is an unregistered file, but that predates this PR.

## 3. test_pilot.py: CONFIRMED as a correction, not a weakening; one plausible risk

- bench/cp1a_frozen.json, pilot.py, pilot_plan.json, bench/results/, bench/manifests/ and bench/manifest.sh
  are identical to origin/main (`git diff --quiet`). The manifest SHA-256 768056bb… matches.
- On origin/main, test 20 of 24 (test_freeze_gate_quick_is_not_evidence) asserted, so tests 21 to 24 never
  ran. On the PR, `python3 tests/test_pilot.py` prints ok and a counting harness shows all 24 run.
- Mutations in the scratch copy. Every one failed the test:
  - go_record set to null → AssertionError: None
  - go_record pointing at a missing path → fail
  - JSON unchanged but the GO file deleted → isfile assert
  - plan_sha256 changed → fail
  - manifest_sha256 changed → fail
  - pilot_plan.json edited → caught by "plan SHA-256 … is not the frozen"
  - runner_commit changed to HEAD, to origin/main, to a non-commit, or to null → all fail
- Risk (no CI defect; it is the "freeze decision" the audit's hidden-coupling note calls for):
  - On origin/main the freeze gate allows a real pilot: the list of disallowed paths changed since 4250b32 is
    empty.
  - After merging, main HEAD changes .github/workflows/ci.yml, CMakeLists.txt and tests/test_pilot.py. So a
    real CP-1A pilot from main would be refused until runner_commit is re-frozen, or the pilot is run from a
    checkout at or before 86a5668.
  - The new test comment says the test does not pin this, but docs/CI.md never states the consequence. This
    should be decided or documented explicitly.

## 4. docs/CI.md: ISSUES

- Inaccuracy (should fix): "Hosted runners are enough for both tiers today (verified in PR #122)" is
  overstated. Only the fast tier has run on a hosted runner, and only one of its two runs passed (the -j nproc
  run failed). The full tier has never run there, so its 90-minute limit and its runner requirements are
  untested.
- Nit: "cost is wall time (measured in PR #122)" gives no figure. Measured: fast -j1 took 284.9 s locally and
  357.9 s on the hosted runner.
- Why serial: the claims match the evidence.
  - The net_bench_rate failure in the first hosted run matches the log exactly: "end_backlog 1" and issue
    lateness p99 301989888 ns (302 ms).
  - "Passed in the next hosted run" is now true (run 43370027787, at -j1).
  - "Passed alone": it passed at -j1 locally (15.68 s), and it passed in the QA serial log.
  - ctest_j4.log line 85: q5_catchup_race failed with "standby cannot listen on 0.0.0.0:25577: Address already
    in use"; test_baseline_retry was the only failure, 48/49 passed.
- Plausible but not proven: that the port was taken by another test. free_block (tests/test_health.py:79)
  picks a random base in 20000 to 31000 and closes its probe sockets, so a parallel test can grab a port while
  a node restarts. The log shows only the bind failure.
- Correct: the tier lists, the triggers, the cron time, the dependencies, the artifact names, "no
  continue-on-error" and the timings source.

## 5. Scope: CONFIRMED

`git diff --name-only origin/main...origin/m1-0-minimal-ci` lists only .github/workflows/ci.yml,
CMakeLists.txt (a 5-line label block), docs/CI.md and tests/test_pilot.py. There are no production changes.

## Local test run

In its own build at abb2a88 (c03e902 changes only ci.yml and CI.md, so binaries and tests are the same),
`ctest -LE slow -j1` passed 40/40, 0 failed, 284.92 s.

## Verdict

No confirmed defects. Before merge, fix the "verified in PR #122 / both tiers" overstatement in CI.md and record
a decision on the post-merge freeze consequence for CP-1A pilots.

---

## Disposition (implementer, verified)

- Overstatement: fixed in 98588d1. CI.md now says only the fast tier is verified hosted; the full tier's
  stand-in is a local serial run (49/49, `local-full-j1-98588d1.log`). Measured costs are stated.
- Port-race cause: CI.md now says plausible, not proven.
- Freeze consequence: re-checked with `bench/pilot.py`'s own `post_freeze_allowed`: the disallowed list is
  empty at 86a5668 and is exactly `.github/workflows/ci.yml`, `CMakeLists.txt`, `tests/test_pilot.py` on this
  branch. Recorded in CI.md ("Consequence for the frozen campaigns") for the reviewer to accept or decide
  otherwise. `bench/s6.py` already refuses on main (code changed after 63440b1). `bench/cp1a_frozen.json` is
  not changed.
- Nits (actions pinned by tag; Node 20 deprecation warning): left as is; pinning by SHA is a reasonable
  follow-up, not required for M1-0.
