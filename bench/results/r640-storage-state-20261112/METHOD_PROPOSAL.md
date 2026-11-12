# Proposed campaign-method revision: storage state, between-session noise, duration order (for review)

A proposal only (PR #112 review, "Also analyze campaign methodology"). **Nothing here is authorized or
run.** The evidence is in [REPORT.md](REPORT.md). Every number marked *proposed* is the reviewer's to
accept, change or reject before any data. Each choice is fixed once accepted.

The aim is that a SNC number and later a competitor number are measured from the **same defined device
state**, with a noise model that includes **between-session** movement and a duration test that cannot
confound window length with session position.

---

## A0. Immediate, state-preserving (needs a ruling, not a measurement)

`fstrim.timer` fires **Mon 2026-11-16 00:56:44 UTC** and would trim `/srv/snc-data`. That would destroy
the state the review says to preserve, and campaign sessions would then depend on a weekly timer nobody
controls.

**Proposed:**
- `systemctl stop fstrim.timer` and `systemctl disable fstrim.timer` for the campaign's duration.
- Every trim of the data SSD then becomes an explicit, logged step of a session procedure (A2).
- This changes no device state; it only prevents an uncontrolled change.

Not done: it is a host change the review did not name.

## A. Storage state at session start

### A1. Record the state of every session (proposed: always, whatever A2 is chosen)

At the start and end of every physical session the runner records, read only:
- `/proc/diskstats` for the data disk (writes, discards, in-flight);
- `smartctl -x`: temperature, background-scan status and progress, error counters, bytes written;
- the last fstrim from the journal;
- `tune2fs -l` lifetime writes;
- uptime and boot ID.

This costs seconds and needs no state change. It would have told us whether a background scan ran during
CP-1A or during the validation. **Proposed:** a session whose start and end snapshots show a background
scan's progress advancing is flagged in its evidence. It is not discarded.

### A2. Defined start state: three options

| Option | Procedure before each session | Cost | Represents |
|---|---|---|---|
| **A2-a fresh** | `fstrim /srv/snc-data`, then a fixed idle settle (*proposed* 30 min), then the A1 snapshot | ~1–2 min trim + settle | a freshly trimmed drive: best case, easy to reproduce |
| **A2-b steady state** | A2-a, then a fixed preconditioning write (*proposed* 2 × user capacity, 7.7 TB, sequential fill then the campaign's own write pattern) until a steady-state test holds, then the snapshot | hours per session (at ~1–2 GB/s, 1–2 h per capacity) and real wear (~2 drive writes per session) | a long-running production drive: worst/steady case, closest to "LOCAL_DURABLE in production" |
| **A2-c bounded** | A2-a, plus a hard cap on host writes per session since the trim (*proposed* ≤ 0.5 × capacity, 1.9 TB, from diskstats); a session that would exceed it is split, with a new A2-a in between | low | a fresh-to-moderately-used drive, within a measured bound |

**Recommendation: A2-a, plus A1, plus a between-session check (B).** It is cheap, repeatable and the same
for every system. A2-b is the more production-like state, but it costs hours and wear per session, and it
needs its own steady-state criterion. Either way, **the same state procedure applies to SNC and to every
competitor session**, or the comparison is not fair.

Open question for the reviewer: whether CP-1A's numbers, measured about 6–8 h after a trim, count as
"A2-a-like" evidence or need a re-measurement under the accepted procedure. This proposal does not
pre-judge that.

## B. Between-session noise (before any competitor data)

The current noise input N = 3 × max(CV_thr, CV_p99) uses within-session reps only. PR #112 showed a
session-to-session p99 shift of about 1.58 ×, so that N is incomplete.

**Proposed procedure, SNC only, deterministic:**
1. **K sessions** (*proposed* K = 5). Each starts from the accepted A2 state, with a fresh server per rep.
   No other experiment runs in between.
2. Each session runs the same **anchor block**: 5 fixed open-loop reps at the CP-1A objective rate,
   654,569 ops/s, with a 60 s window and the unchanged Profile-A configuration. Each session also runs
   5 closed-loop capacity reps. This is the CP-1A fixed and capacity phase, unchanged.
3. Per session: S_p99 = median total p99 and S_thr = median capacity.
4. **CV_p99_between** = `pilot.cv()` of the K session medians, and likewise CV_thr_between. The
   within-session CVs are pooled as the median of the K session CVs.
5. **New noise input:** N = 3 × max(CV_thr_within, CV_p99_within, CV_thr_between, CV_p99_between).
6. **Stop rule** (*proposed*): if CV_p99_between exceeds 10%, or one session median is more than
   1.5 × the median of the others, the state procedure is not controlling the device. STOP for
   methodology review. Do not widen N to absorb it.
7. **Sentinels inside every later session**, competitor sessions included: one anchor rep at the start
   and one at the end. If the end sentinel's p99 exceeds the start's × (1 + N), the session is flagged
   non-stationary and returned for review. It is never silently used.

This needs no change to `bench/pilot.py`. It would be a separate driver in `bench/results/` that imports
`pilot.Pilot.one` and `pilot.sustained`, as the duration driver did. The A1 snapshot would be added in the
driver.

## C. Duration validation without order confounding

The PR #112 validation used a seeded shuffle of 9 runs. It happened to put 2 of the 3 60 s references last,
after the state step (PR #112 review, Finding 1).

**Proposed design: a balanced Latin square in blocks, with paired comparison.**
- **3 blocks**, each containing one 60 s, one 120 s and one 240 s rep. Across blocks the positions rotate,
  so every window appears once in every position:

  | Block | position 1 | position 2 | position 3 |
  |---|---|---|---|
  | 1 | 60 | 120 | 240 |
  | 2 | 120 | 240 | 60 |
  | 3 | 240 | 60 | 120 |

  The order is fixed in the preregistration, not drawn from a seed.
- **Sentinels:** a 60 s anchor rep at the start of the session and after the last block. These are
  stationarity sentinels as in B.7, not part of the judged cells.
- **Paired judgement within each block:** r_w,b = p99(w, block b) / p99(60, block b) for w = 120 and 240.
  - PASS for w needs the median over the 3 blocks of r_w,b ≤ 1 + D, with every rep sustained.
  - This compares each long window with the 60 s rep measured minutes away in the same block, not with a
    reference that may sit on the other side of a state change.
- **Write balance:** a 240 s rep writes about 4 × as much as a 60 s rep. The Latin square balances time
  position but not cumulative writes. The A1 snapshot records the per-rep write offset, and the blocks
  stay within the A2 write bound.
- **D:** the accepted N from B, not the within-session-only 0.0579, once B exists. Until then the reviewer
  decides which D applies.
- **Same start state (A2)**, A1 snapshots at start and end, 11 runs, about 40 min.

## Order of work, if accepted (each a separate reviewed step, nothing run yet)

1. A0 ruling (timer).
2. Accept or adjust A2, B and C, with numbers fixed.
3. Preregister and run B (SNC only).
4. Preregister and run C under the B noise model.
5. Then the CRITERIA review.

Competitor runs, the CRITERIA freeze and REFINE #2 remain unauthorized.
