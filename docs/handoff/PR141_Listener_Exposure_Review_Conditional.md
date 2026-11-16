# PR #140 / #141 independent review (Codex, relayed by cody, 2026-11-16 01:28 UTC)

Posted by cody in the project chat; copied verbatim. The PR #141 part applies to this slice (AFR-S-2,
`docs/LISTENER_EXPOSURE.md`); the PR #140 part is recorded here for completeness and is handled in its own
thread.

> SNC PR #140 and #141 — Independent Review
>
> PR #140: APPROVED FOR DOCUMENTATION MERGE ONLY.
>
> The architectural analysis is sound enough to retain as a proposed decision record. Do not treat D-01, D-02 or D-03 as fully accepted contracts.
>
> The owner's selection of term-scoped leases as the default is acknowledged.
>
> Before final architectural acceptance, produce a focused safety analysis covering:
>
> 1. Lease validity and handoff across N-member set changes, restarts, pauses, clock drift and operator promotion.
> 2. Proof that every acknowledged write survives promotion and log reconciliation, including unacknowledged suffixes and concurrent membership changes.
> 3. Explicit guarantees for single-copy windows, including the limits of G-14(b).
> 4. Dependencies and acceptance criteria for experiments X-1, X-2 and X-3.
>
> Preserve the distinction between choosing the architectural direction and proving the complete protocol.
>
> PR #141: CONDITIONALLY APPROVED.
>
> The security mitigation and regression coverage are acceptable.
>
> Before merge:
>
> 1. Run the complete test suite on the final PR head after the test-harness corrections.
> 2. Verify the mixed-version x9 scenario, or document why it cannot run and obtain explicit approval for that coverage gap.
> 3. Confirm the final test results and commit SHA in the PR.
> 4. Keep authentication, authorization and cluster identity explicitly open for AFR-D-07.
>
> Do not begin additional architectural implementation based on either approval.
>
> Continue the existing review gates and independent QA requirements.
>
> Always verify, never assume.

## How the PR #141 conditions were met

Code and tests verified at **a12f4c11237aed935f3de643792860b54fdfe124** (PR head after the test-harness
corrections and the merge of main at 15d3cf4). The commit that adds this record changes documentation only.

1. **Complete suite on the final head:** `ctest -j1 --timeout 2400`, RelWithDebInfo build of a12f4c1 (configured fresh this session, rebuilt incrementally), this
   container (4 CPUs): **63 of 63 passed**, 1833 s. GitHub Actions fast job on a12f4c1: green.
2. **Mixed-version x9:** run, **pass**. `python3 tests/test_xnode_proc.py BUILD x9 --old OLD`, OLD = a clean build of
   D.1 commit 33f7c6d (the build PR #95's review used). Mixed phase: the new side refused v1 links 160 times with
   "wire v1, this node speaks v2", the old side refused too, clients served (93,065 acknowledged, 0 unknown),
   cross-group transfers held pending; after group 0 was upgraded: "OK: 64 accounts each served once; balances =
   acknowledged transfers + 0 of 0 in doubt". The old nodes take no `--audit-dir` and are asked AUDIT with a path,
   as before; no coverage gap to approve.
3. **Results and SHA in the PR:** PR #141's description carries the above.
4. **Authentication, authorization and cluster identity stay open for AFR-D-07:** `docs/LISTENER_EXPOSURE.md`
   section 6 says so; nothing in this slice designs or implements them.

**Re-verified after main moved.** Before merge, main gained PR #140 (docs) and PR #137 (D4, which also edits
`tools/snc_server.cpp` startup). Both were merged into the branch (conflicts in `AGENTS.md`, `CMakeLists.txt`,
`docs/STATUS.md` only, resolved by keeping both sides). Because D4 brought code, conditions 1 and 2 were run
again on the merged head **e5c9561cd059a213154a2151a9dc0f8434ce7d33**: full suite **64 of 64 passed** (1832 s;
the 64th is D4's `test_startup_errors`), and x9 against 33f7c6d **passed**. CI fast job on e5c9561: green. The
commit that updates this record changes documentation only.

No further architectural implementation starts from this approval.
