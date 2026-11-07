# HA test-harness race fixes: validation (dev VM, 2026-11-07)

Required by `docs/handoff/PR70_S01_Review.txt` sections 5 and 6. The fixes
are test-only: no engine, server or witness code changed. The R640 was not
used. All runs below are on 2e5319a.

## The fixes

**A. `bench/lab/lab.py` p3** (exposed by `test_lab_witness_guard`, case
`clean`, `KeyError: None`).
- Before: p3 sampled `lab.primary(0)` once. Under load a STATUS could time
  out, so the sample returned `None` and `lab.nodes[None]` raised.
- After: `Lab.wait_primary(g, timeout=30)` asks every member of the group
  each round. It returns only when exactly one member answers as primary.
  - A round with no primary answer (a timed-out STATUS) is not an answer.
  - A round with two claimants (an old primary not yet withdrawn) is not
    an answer either, so a stale node is never chosen silently.
  - It compares with `None`, never truthiness, so node id 0 is valid.
    (The lab numbers nodes from 1 today, but the helper does not rely on
    that.)
  - On timeout it raises `RuntimeError` naming every member's last role.
- Only p3 changes. Other one-shot `primary()` callers (`connect_arg`, p4,
  the move scenario, soak) have not shown the race. They are left as is
  (review section 13: the two known races only).

**B. `tests/test_ready_order.py` c2_restart_held** (exposed under ASan:
`grounded: false` with READY already chosen).
- Before: after `wait_ready` saw READY in the witnesses, the test read the
  primary's output once. The witnesses commit READY before their reply
  reaches the primary, and only then does the primary print
  `READY after v=N`.
- After: a bounded wait (30 s) for the new process's first READY line,
  taken only once READY was observed.
- The process instance is keyed: `pr.nodes[0]` is the new process (the old
  one is in `pr.old_nodes`), so no line from before the restart can match.
- The grounding rule is unchanged: the first READY line of the new
  process decides, and `grounded` requires its `after` value to be at
  least the new process's own NOT READY version. A refused line is
  ignored, and a stale or older one fails.
- `grounding_self_test()` checks that rule on fixed lines at every run.

## Results

| Check | Result |
|---|---|
| `test_lab_witness_guard`, RelWithDebInfo ×10 | 10/10 (each: `primary`, `clean`, `violation` PASS) |
| `test_ready_order c2_restart_held`, RelWithDebInfo ×10 | 10/10 |
| `test_lab_witness_guard`, ASan+UBSan ×5 | 5/5, 0 sanitizer reports |
| `c2_restart_held`, ASan+UBSan ×5 | 5/5, 0 sanitizer reports |
| `test_lab_witness_guard`, TSan ×5 | 5/5, 0 sanitizer reports |
| `c2_restart_held`, TSan ×5 | 5/5, 0 sanitizer reports |
| Stressed, ×3 rounds: lab guard (Release), c2 (ASan) and `test_sync_readiness` (Release) at once, plus two busy-loop CPU burners on 4 vCPUs | 3/3 rounds, all three tests pass; no stray processes after |
| Full RelWithDebInfo suite, `ctest -j1` | 37/37 |

**The race, reproduced deterministically, and fixed:**
- **A.** The new `primary` case in `test_lab_witness_guard` scripts STATUS
  answers, with no processes:
  - One-shot `primary()` returns `None` when the first STATUS times out.
    That is the original failure.
  - `wait_primary` then returns the primary.
  - With two claimants it waits until one withdraws.
  - Node id 0 is returned correctly.
  - With no primary it raises within its 0.5 s timeout, naming the last
    roles.
- **B.** `c2_negative.py late` delays the READY line's visibility by 3 s,
  which is the race in deterministic form. Read once, the line would be
  missing. With the wait, c2 passes and is grounded (`LATE_OK`).

**The test still fails when its condition is absent** (`c2_negative.py`,
real processes; the result must be `pass: false`, `grounded: false`):
- `absent`: the new process's READY line never appears. Fails with
  `new_ready_line: ""` after the bounded wait. Run under both Release and
  ASan.
- `stale`: the first READY line names an older NOT READY (`after v=1`
  < base). Fails as not grounded.

The stressed rounds did not reproduce the original failures on the old
code either; the races are rare under ordinary load. They show the
fixed tests stay green under contention. The proof that each race is
handled is the deterministic reproduction above.

## Files

- `repeat-summary.txt`, `harness_matrix.sh`: the repeated runs.
- `negative-stress-full-summary.txt`, `phase2.sh`: negatives, stress and
  the full suite.
- `c2_negative.py`, `neg-*.log`, `late-rel.log`: the negative and late
  drivers and their output.
- `full-release-j1.log`: the full suite.
