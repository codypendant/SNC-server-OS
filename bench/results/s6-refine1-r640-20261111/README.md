# S6 REFINE round 1 (W=1): result (2026-11-11), for independent review

The preregistration ([PREREG.md](PREREG.md)), the overlay and the tests
were committed at `3acad8c` before any data.
- **Run:** once, `refine1.py run run --scratch /dev/shm/snc-s6r1`, at
  `3acad8c`, 19:40:25 to 19:51:38 UTC, exit 0.
- **Files:** `run/` holds the runner's files, unedited, and
  `run/console.log` was copied in afterwards. This README is a reading of
  them.

## Verdict (`run/verdict.json`): **REFINE #1 PASS**

- **Session:** preflight no problems; postflight every machine and build
  check true; 9 reps, **no VOID, no rerun**. Every P1, H1 and C1 rep proved
  a checkpoint began inside its window.
- **All three P1 reps pass all seven original S6 rules:**

| P1 rep | p99 (limit 958,464) | p99.9 (limit 10,000,000) | max | late / scheduled | refusals | emits_peak max | checkpoint | rc client/server |
|---|---|---|---|---|---|---|---|---|
| 0 | 901,120 | 9,961,472 | 49.4 ms | 0.0247% | 0 / 0 | 262 | 1.52 s | 0/0 |
| 1 | 950,272 | 9,961,472 | 49.3 ms | 0.0252% | 0 / 0 | 306 | 1.58 s | 0/0 |
| 2 | 917,504 | 9,961,472 | 49.0 ms | 0.0252% | 0 / 0 | 268 | 1.54 s | 0/0 |

All reps drained, with errors, stale and index errors 0, no degraded
checkpoint and no failure marker.

- **Capacity guard: holds.** Median C1 = **4,353,886** ops/s, at least
  0.95 × 4,016,161 (the frozen S6 C0 median) = 3,815,353. The ratio is
  1.084. Rule 7 holds in all three C1 reps.

### The margins are thin, and stated plainly

- **p99.9:** 9,961,472 ns in every P1 rep, against 10,000,000. That is a
  0.39% margin; it is a net_bench histogram bucket edge, and the next edge
  is above the limit. In the diagnostic W1 characterization the same cell
  read 9,699,328, one bucket lower.
- **p99:** rep 1 is 950,272 ns, against 958,464: a 0.85% margin.

The rules are met as preregistered. The review may weigh how close they
were.

## Everything measured (P1 vs the frozen S6 P = W4)

| | S6 P (W4) | **P1 (W1)** | S6 N (no checkpoint) |
|---|---|---|---|
| total p99.9 | 14.16-14.68 ms | **9.96** (×3) | 0.90-0.93 |
| deposit/transfer p99.9 | 17.3-17.8 | **12.1-12.6** | 1.15-1.18 |
| read p99.9 | 2.9-3.4 | 2.8-3.2 | about 0.34 |
| index p99.9 | 6.4-7.6 | 6.6-6.9 | about 0.8 |
| max | 48.8-49.3 | 49.0-49.4 | 4.5-8.6 |
| WAL EWMA peak | 6.9-7.4 | **4.1-4.3** | 0.44-0.84 |
| time above WAL-high (worst shard) | 0.8-1.1 s | **0** | 0 |
| chunk write EWMA, per shard | 9.7-35.0 | **4.1-8.5** | n/a |
| chunk write max, per shard | 30.1-52.9 | **11.9-16.9** | n/a |
| checkpoint completion | 1.49-1.56 s | 1.52-1.58 s | none |
| begin snapshot (shard 0) | 27.0 ms | 26.9-27.0 ms | none |

The 100 ms samples: 767-768 per P1 rep, 0 failed, 0 skipped. No catch-up
in P1; backlog never above 25%.

## H1 (reported only, not judged) and C1

| | S6 H (W4) | H1 (W1) |
|---|---|---|
| total p99 / p99.9 | 2.10-2.36 / 8.65-9.70 ms | 2.10-2.16 / 8.91-9.44 ms |
| index p99.9 | 25.2-27.3 ms | 24.1-27.8 ms |
| lateness | 0.108-0.110% | 0.108% |
| EXEC refusals | 6,868-7,956 | 7,000-8,649 |
| checkpoint | 3.33-3.47 s | 3.36-3.53 s |

So W=1 shows no pathology at the high-load diagnostic point: H1 sits
within or beside H's range.

C1 per rep:
- 4,353,886, 4,361,579 and 4,342,799 ops/s;
- checkpoint 4.82-5.09 s;
- EXEC refusals 35.5-38.9 M per rep, against S6 C's 38.3-40.3 M: the
  closed-loop `refused_followup` pattern noted in the S6 result review is
  unchanged by W.

## Still open (unchanged from the characterization; not part of this verdict)

- The max is about 49 ms in every P-shaped rep at every window.
- Mutation p99.9 is about 12 ms against N's about 1.2 ms.
- The 27 ms begin snapshot (B2) is not addressed by this round.

## Accounting

- REFINE round 1 of at most 2: **PASS**, pending independent review.
- The original S6 result stays FAIL as recorded.
- No CP-1A, no default change, no adaptive window, no further tuning.
