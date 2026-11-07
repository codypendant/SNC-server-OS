# Status of this pilot: diagnostic history, not campaign evidence

Recorded 2026-11-07 (`docs/handoff/PR64_Reconciliation_Review.txt`, section 11).

R640 step 1 (`../step1-r640-20261107-ids-shm/`) showed that this pilot's harness wrote each run's 80 MB
account-ID file onto the data SSD just before the run. Its writeback disturbed the storage being
measured. A BENCHMARK DEFECT, fixed by scratch isolation in `bench/pilot.py` (PR #64).

- Keep it. It is diagnostic and falsification history.
- Do not derive CRITERIA thresholds from it.
- Do not compare competitor numbers against it.
- Do not salvage individual cells as campaign evidence.

A clean SNC-only CP-1A rerun from the beginning is needed later, under the corrected harness. It is not
authorized yet.
