# Witness hardening evidence (2026-11-05)

Work order `docs/handoff/PR51_Review_Handoff.txt`; write-up `docs/PAXOS_BALLOTS.md` section 9.
Code commit: 482958a (fresh builds; Release, Debug ASan+UBSan, Debug TSan).

- `ctest-release.log`: 30/31; `test_ready_order` m1/m2 failed only because `SNC_OLD_BUILD`
  pointed at a PR #39 build. `test_ready_order-v10.log`: rerun with a build of e3e0500, all 6 scenarios pass.
- `ctest-asan.log`, `ctest-tsan.log`: 31/31 each, `SNC_OLD_BUILD` = e3e0500 build, 0 reports.
- `test_witness_hardening.log`, `hardening-json/`: H1-H9 (H12 inside H2, H8, H9).
- H10: `tests/test_profile_check.py` (in ctest).
