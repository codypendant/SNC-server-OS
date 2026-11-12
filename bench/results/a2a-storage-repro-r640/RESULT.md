# A2-a storage-state reproducibility validation: RESULT (for review)

**Verdict: STOP.** The run was complete and valid (K = 5 sessions, 60 of 60 runs, every guard, trim, sync,
settle and A1 check passed; nothing was INCOMPLETE). The single `why` line:

> `multimodal thr: session-median gap 322958 > 2 x widest within-session range 108520`

`a2a.py judge` on the out directory reproduces the verdict and the `why` line. Facts for the reviewer; no
rulings, no re-judging, no new session.

- Run: 2026-11-12 16:04:48 to 19:57 UTC, one invocation, python PID 4131, from `main` = `d5c07ca` (PR #116
  merge, GO), tree clean. Frozen binaries / plan / manifest e as recorded in `out/pilot.json`.
- Host setup before the run (`evidence/host-setup.txt`): 72 governors `performance`, `numa_balancing` 0. Sudoers
  installed, `visudo -c` OK, 4 of 4 `sudo -n -l` OK (`evidence/sudoers-install.txt`). Pre-data `test_a2a.py` ok.
  Console: `evidence/console.log`. Raw output, unchanged: `out/` (`diff -r` and `cmp pilot.json` identical to
  `/home/solo/a2a-storage-repro-r640`).
- Device identity (persistent, revision 4): UUID `3bf857c8-0cb4-4c57-b9a5-14328e70b061`, Samsung ARFX3840S5xnFTRI,
  serial S48LNA0M700325, WWN 0x5002538b097f68a0; kernel names `sda` / `sda1` in every session (unchanged during
  the invocation). Recorded in `out/pilot.json` (`device_identity_at_start`) and per session.

## Per session

| k | trim bytes | discard delta (sectors) | settle written / discarded | guard | syncs rc | SSD temp pre-trim / start / end (C) | background scan progress pre-trim / start / end | units started (start..end) |
|---|---|---|---|---|---|---|---|---|
| 1 | 3,779,301,535,744 | 7,381,448,312 | 0 / 0 | ok | 0 / 0 | 34 / 34 / 37 | 79.89 / 79.96 / 80.00 % (active) | fwupd-refresh, sysstat-collect |
| 2 | 185,471,426,560 | 362,248,880 | 0 / 0 | ok | 0 / 0 | 37 / 35 / 38 | 80.00 / 80.08 / 80.12 % (active) | fwupd-refresh, sysstat-collect |
| 3 | 195,851,280,384 | 382,522,032 | 0 / 0 | ok | 0 / 0 | 38 / 35 / 38 | 80.12 / 80.19 / 80.24 % (active) | sysstat-collect |
| 4 | 184,920,182,784 | 361,172,232 | 0 / 0 | ok | 0 / 0 | 38 / 34 / 37 | 80.24 / 80.31 / 80.35 % (active) | fwupd-refresh, sysstat-collect |
| 5 | 184,524,406,784 | 360,399,232 | 0 / 0 | ok | 0 / 0 | 37 / 34 / 37 | 80.35 / 80.42 / 80.46 % (active) | fwupd-refresh, sysstat-collect |

Session 1 trimmed about 3.78 TB (the last trim was 2026-11-11 15:09); sessions 2 to 5 about 185 to 196 GB each,
the data written by the preceding session's 12 runs.

## Medians and CVs (sample CV, `pilot.cv`)

| k | median p99 (F1..F5), ns | CV p99 | range p99, ns | median throughput (C1..C5), ops/s | CV thr | range thr, ops/s |
|---|---|---|---|---|---|---|
| 1 | 966,656 | 2.75 % | 65,536 | 4,021,146 | 0.94 % | 92,827 |
| 2 | 983,040 | 1.91 % | 49,152 | **4,356,828** | 0.28 % | 29,849 |
| 3 | 999,424 | 2.17 % | 49,152 | 4,010,715 | 0.67 % | 63,883 |
| 4 | 966,656 | 4.16 % | 98,304 | 4,033,870 | 0.38 % | 39,122 |
| 5 | 983,040 | 3.97 % | 98,304 | 3,996,693 | 1.22 % | 108,520 |

- Between-session CV: p99 **1.40 %**, throughput **3.75 %**. Max within-session CV: p99 **4.16 %**, throughput
  **1.22 %**. All four are below the 10 % STOP limit.
- **N_raw = 3 x max(CVs) = 0.1249** (a result only; it judged nothing).
- Anchor reps (S0, F1..F5, S1): all sustained, 0 errors. Capacity reps: 0 errors, 0 stale, 0 index_errors.

## Sentinels (limit 1.10, fixed)

| k | S0 p99 | S1 p99 | ratio |
|---|---|---|---|
| 1 | 999,424 | 999,424 | 1.0000 |
| 2 | 933,888 | 933,888 | 1.0000 |
| 3 | 983,040 | 933,888 | 1.0526 |
| 4 | 1,032,192 | 1,015,808 | 1.0161 |
| 5 | 983,040 | 1,032,192 | 1.0500 |

All five are within 1.10.

## Multimodal signal (supplementary automatic rule)

- **Throughput: FLAG.** The sorted session medians are 3,996,693 / 4,010,715 / 4,021,146 / 4,033,870 /
  4,356,828. The largest adjacent gap is 322,958 (session 2 vs the next lowest, session 4), against
  2 x the widest within-session range, 2 x 108,520 = 217,040. 322,958 > 217,040, so the rule flags it. This is
  the only reason for STOP.
- **p99: no flag.** Max gap 16,384 against widest within-session range 98,304.
- Descriptive only (not a re-judgment, and not a rule of the preregistration): session 2's capacity median is
  about 8.0 % above the mean of the other four, which lie within 37,177 ops/s of each other (CV 0.39 %). The
  five capacity reps of session 2 are all between 4,331,074 and 4,360,923. Session 2's p99 median (983,040)
  is not unusual. The session-2 settle, trim, guard, background-scan and temperature records are of the same
  kind as the other sessions; session 2's trim (185 GB) is about the same as sessions 4 and 5.

## Historical anchor (not the objective)

`anchor_median_of_session_medians_p99_ns` = **983,040** ns (median of the five session p99 medians). CP-1A's
Profile A p99 at the same rate was 933,888 ns. Labelled as a historical anchor only.

## Not done, per the review

No stable-peak bracket, duration Latin square, CRITERIA, competitor run or REFINE #2. No session was replaced
or rerun. `fstrim.timer` stays disabled / inactive; the only trims were the driver's five explicit ones.
