# Witness hardening evidence (2026-11-05)

Work orders: `docs/handoff/PR51_Review_Handoff.txt` and `PR52_Review_Handoff.txt`.
Write-up: `docs/PAXOS_BALLOTS.md` section 9 (suites in 9.5).

- `ctest-release-clean.log` (+ `.header`): commit b622963, RelWithDebInfo,
  `SNC_OLD_BUILD` = e3e0500 build from the start: **32/32**.
- `ctest-asan.log`, `ctest-tsan.log`: commit 482958a (last production-code
  change), 31/31 each, 0 sanitizer reports.
- `test_witness_hardening.log`, `hardening-json/`: H1-H9 (H12 inside H2,
  H8, H9). H10: `tests/test_profile_check.py`; G2 lab boundaries:
  `tests/test_lab_witness_guard.py` (both in ctest).
- `history/`: release 30/31 (wrong `SNC_OLD_BUILD`; `test_ready_order-v10.log`
  is the diagnostic rerun) and release 31/32 (b3, disk full from leaked
  test directories). Kept as history, not counted.
