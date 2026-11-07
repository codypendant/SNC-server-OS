# Software validation of the corrected harness (dev VM, no R640 load)

Required by `docs/handoff/PR64_Reconciliation_Review.txt`, section 8. The C++
production code is unchanged since the reviewed build: this commit touches
only Python and docs. So no new sanitizer campaign is required.

| Check | Result | Log |
|---|---|---|
| `tests/test_pilot.py` (scratch resolver: partitions, dm, tmpfs with and without swap, btrfs, loop, missing device; isolation problems) | pass | `validation/test_pilot.log`; also `ctest -R test_pilot` on main |
| `test_diag3.py`: B and T commands identical except build and telemetry, for every proof and causal cell; ids under the one scratch; approved cells only | pass | `validation/test_diag3.log` |
| `test_diag3.py`: isolation fails closed (scratch or out on the data disk, swap on the data disk, unknown mapping, data on two disks) | pass | same |
| `test_diag3.py`: revised G0 (5% bounds at 4,148,143 / 4,584,789 exact, p99 bound 1,572,864, identity) and no zero-refusal clause | pass | same |
| `test_diag3.py`: revised G2 boundaries (median +0.0001 and per-rep max(B) + 0.0002 at the exact refusal count each side; INDEX refusals count) | pass | same |
| `test_diag3.py`: G1, G3, G4 unchanged at their boundaries; report parser; manifest d values | pass | same |
| `bench/pilot.py --quick` with `--scratch /dev/shm/snc-pilot-qv`: records scratch on memory, data and out disks, and the isolation problem | pass | `validation/quick-pilot-isolation.json` |
| `bench/pilot.py` real mode with scratch and out on the data disk: refuses before any run, listing both | pass | `validation/pilot-real-mode-refusal.log` |
| `diag3.py` real mode on the dev VM: refuses before any run (out on the data disk, identity checks fail as expected off the R640); `raw/` stays empty | pass | `validation/diag3-devvm-refusal.json` |

The dev VM has a single disk, so a passing isolation preflight can only be shown on the R640. Its
`--dry-run` prints the preflight and the commands without load.
