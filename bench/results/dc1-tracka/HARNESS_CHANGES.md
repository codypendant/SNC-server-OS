# Track A harness behaviour changes after the criteria freeze

CRITERIA.md is frozen at `8a0f787` and is byte-identical on this branch (`git diff 8a0f787 HEAD --
bench/results/dc1-tracka/CRITERIA.md` is empty). Neither change below touches a threshold, cap, outcome row, matrix row,
guarantee or seed. Both are harness behaviours that CRITERIA.md does not specify, they apply identically to every arm
including SNC, and both were made before any counted run. They are reported next to the results.

## HC-1, 2026-11-28: the owner answers truthfully
- **Change:** `GET /owner` answers `happened` exactly when T-N's log holds an effect for that `call_id` at the time
  of asking, otherwise `did_not_happen`. Before, the draft contract (inherited from AR-1) answered by seed parity.
- **Reason:** with seed parity, an arm that recorded a T-N attempt but was killed before sending it was told
  `happened` on odd seeds, completed with `"owner:happened"` for a T-N that never ran, and failed O-2 through no fault
  of its own (seen with the test arm at A07 seed 5). The plan (3.2) defines the owner as the party that knows the
  outcome; it does not define a seed rule.
- **Commits:** contract `94f6314` (`experiments/tracka/INTERFACES.md`); implementation `abf93ab`
  (`experiments/tracka/tracka/services.py`); checker `abf93ab` (an owner answer that contradicts the T-N log at its
  time makes the run invalid).

## HC-2, 2026-11-28: a worker that exits on its own is restarted
- **Change:** when the live worker exits non-zero and the harness did not kill it, the harness writes `exited`
  (`{worker, code}`) and starts a new worker through the arm's documented recovery entry point, as a process
  supervisor (systemd `Restart=on-failure`) would; at most 20 workers per run.
- **Reason:** plan 3.4 restarts workers after every harness kill but says nothing about a worker that dies by itself.
  Without a supervisor, LangGraph's worker died on a closed connection after the PostgreSQL restart at A12, and the run
  sat until the 5-minute cap, which CRITERIA makes UNRESOLVED. A supervisor turns that into a real test of recovery
  for every arm. It favours no arm: any arm can crash, and each restart is visible in the markers.
- **Commit:** `505fe15` (`experiments/tracka/tracka/run.py`, `experiments/tracka/INTERFACES.md`).

## HC-3, 2026-11-28: the approver posts once per run
- **Change:** the harness runs `deliver` at most once per `(task, action)` per run. A request the arm repeats after
  a restart is logged but not answered again.
- **Reason:** the adequacy author's L3 mutant that drops the delivered approval event on restart passed at A09. The
  harness had posted again on the repeated request, which healed the loss: the event was durable only because the
  harness answered twice, the failure plan section 3 names for AR-1. Plan 3.2 says the approver "posts its decision",
  once.
- **Commit:** `e56a759` (`experiments/tracka/tracka/run.py`, `experiments/tracka/INTERFACES.md`).

## HC-4, 2026-11-28 (checker): a capped run is UNRESOLVED, not invalid
- **Change:** on a `run-end` with status `cap` the harness collects no answers. The checker no longer counts the
  missing `quiesced` marker or answers as invalid, and reports a failure of any clause that reads the answers as
  `unresolved`. Failures of the log-only clauses still count.
- **Reason:** before, every capped run was invalid with answer clauses failing, and "a violation seen in an invalid
  run counts" turned a cap into a violation. That contradicted the frozen rule that a run reaching the 5-minute cap is
  UNRESOLVED (CRITERIA "Run" and "A cap hit"). This change restores the frozen rule; it does not change it.
- **Commit:** `e56a759` (`experiments/tracka/check/check.py`, `CLAUSES.md`, tests).

## HC-5, 2026-11-28 (checker): stale-rejected T-F requests are outside the used-reply set
- **Change:** `U` leaves out T-F requests answered `rejected_stale`, as `O-2.single_exec` already did.
- **Reason:** plan 3.3 exempts "a T-F request the tool rejected". A rejected stale request carrying another reply
  id failed `O-1.one_reply`, `O-2.memory` and `O-2.single_exec` on a correct arm (adequacy L1 false positive on 21
  bases).
- **Commit:** `e56a759`.

HC-3 to HC-5 come from the checker adequacy author's development-scale report (DBOS step 1 and the test arm). None of
them changes a CRITERIA threshold, cap, row, matrix entry, guarantee or seed; CRITERIA.md is still byte-identical
to `8a0f787`.
