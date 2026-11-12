# Proposed campaign-method revision: storage state, between-session noise, duration order

**Revision 2 (2026-11-12): updated for the PR #113 review**
([docs/handoff/PR113_Storage_State_Final_Review.md](../../../docs/handoff/PR113_Storage_State_Final_Review.md)).

| Item | Ruling |
|---|---|
| A0 | **AUTHORIZED and done** 2026-11-12 03:54 UTC: `fstrim.timer` disabled and inactive (`bench/results/a2a-storage-repro-r640/a0-fstrim-timer/`). No trim executed. |
| A1 | **Approved:** start/end snapshots in every controlled session. |
| A2 | **A2-a selected** for the first comparative campaign: explicit logged trim, 30 min idle settle, A1 snapshot, then the benchmark. Chosen for repeatability. Claims are scoped to this storage-start regime. A2-b may be a later, separate campaign. |
| CP-1A objective | Measured under the old, uncontrolled storage history. Its S 1,090,948, rate 654,569 and p99 933,888 ns do **not** automatically become the CRITERIA objective; they must be revalidated under A2-a. |
| B | Approved with corrections B1–B4 (section B, rewritten below). |
| C | Accepted in principle, to be used only after A2-a reproducibility, the stable-peak revalidation and the final noise input (section C). |

Nothing beyond A0 has been run. The next step is the A2-a reproducibility preregistration
(`bench/results/a2a-storage-repro-r640/PREREG.md`), returned for review before the first trim.

The original proposal follows, revised where the review changed it.

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

**Selected (PR #113 review): A2-a, plus A1, plus the between-session check (B).** The original recommendation follows. It is cheap, repeatable and the same
for every system. A2-b is the more production-like state, but it costs hours and wear per session, and it
needs its own steady-state criterion. Either way, **the same state procedure applies to SNC and to every
competitor session**, or the comparison is not fair.

Open question for the reviewer: whether CP-1A's numbers, measured about 6–8 h after a trim, count as
"A2-a-like" evidence or need a re-measurement under the accepted procedure. This proposal does not
pre-judge that.

## B. Between-session noise (before any competitor data): revised per the PR #113 review

The current noise input N = 3 × max(CV_thr, CV_p99) uses within-session reps only. PR #112 showed a
session-to-session p99 shift of about 1.58 ×, so that N is incomplete.

**B1. Method validation comes first, then the final N.** The first step validates that A2-a controls
session-to-session movement. 654,569 ops/s is used **only as a historical anchor rate** in that step. Its
result is not called the campaign p99 objective.

**Procedure, SNC only, deterministic** (made concrete in the A2-a preregistration):
1. K = 5 sessions. Each is A2-a: an explicit logged trim, a 30 min idle settle, then the A1 snapshot. The
   A1 snapshot is repeated at the end.
2. Each session runs the CP-1A Profile-A fixed phase (5 open-loop reps at 654,569 ops/s, 60 s) and capacity
   phase (5 closed-loop reps, depth 512, 60 s), unchanged, in a fixed order, with start and end sentinels.
3. Per session k: S_p99,k = median fixed-rep total p99; S_thr,k = median capacity throughput;
   CV_p99,k and CV_thr,k are the within-session `pilot.cv()`.
4. CV_p99_between = `pilot.cv()`(S_p99,1..K); CV_thr_between likewise.

**B2. A conservative within-session CV: the maximum, not the median.**

    N_raw = 3 × max(CV_p99_between, CV_thr_between, max_k CV_p99,k, max_k CV_thr,k)

N = max(N_raw, N_MIN) is set only in the later CRITERIA review. A noisy session cannot disappear inside a
median.

**B3. Stop conditions (before any competitor data).** STOP for methodology review if:
- CV_p99_between > 10%; or
- CV_thr_between > 10%; or
- any session's CV_p99,k > 10%; or
- any session's CV_thr,k > 10%; or
- the distribution is visibly or statefully multimodal despite CV ≤ 10%.

N is never widened to absorb an uncontrolled state.

**B4. Symmetric sentinels.** For a session's start and end sentinel p99 values P_start and P_end:

    max(P_end / P_start, P_start / P_end) ≤ 1 + N

A large improvement is also nonstationarity.

**For the A2-a method-validation experiment itself** (PR #114 review 1, item 1), the sentinel limit is a
**fixed 1.10**, not 1 + N: max(P_end/P_start, P_start/P_end) ≤ 1.10. That experiment's own N_raw must not
widen the threshold that judges its stationarity. N_raw remains a result that may later inform CRITERIA.
Review 1 also requires a per-session environment guard, a proven trim, a judged clean settle (0 sectors
written or discarded on `sdb`), stricter capacity validity and fail-closed A1 snapshots. See
`bench/results/a2a-storage-repro-r640/PREREG.md` (revision 2).

**After A2-a stability is shown (later steps, each preregistered and reviewed):**
- **Stable-peak revalidation under A2-a.** A balanced, interleaved bracket of 872,759 × 3, 1,090,948 × 3
  and 1,309,138 × 3.
  - If the old bracket reproduces (872,759 and 1,090,948 sustain 3/3, and 1,309,138 is unsustained), then
    S = 1,090,948 is validated under A2-a and the objective rate stays 654,569.
  - Otherwise STOP: a new Profile-A sweep is needed. S is not extrapolated.
- **The final objective p99** comes from the controlled A2-a sessions at the validated rate. Recommended:
  the median of the K session medians, fixed before any competitor result is seen.

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
- **D:** the accepted campaign noise guard from B, not the CP-1A-only D = 0.057858 (PR #113 review).
- **Prerequisites (PR #113 review):** use C only after (1) A2-a is shown reproducible, (2) the stable peak
  and objective rate are revalidated, and (3) the final SNC-only noise input exists.
- **Same start state (A2)**, A1 snapshots at start and end, 11 runs, about 40 min.

## Order of work (revision 2; each a separate reviewed step)

1. ~~A0: hold the timer.~~ **Done** 2026-11-12 03:54 UTC.
2. ~~Rule on A2, B and C.~~ **Ruled** (PR #113 review): A2-a selected; B approved with B1–B4; C in principle.
3. **Now:** preregister the A2-a reproducibility validation (K = 5) and return it for review **before the
   first trim**. Revision 1 drew CHANGES REQUIRED (PR #114 review 1); revision 2 applies them.
4. Run it once approved. STABLE or STOP.
5. Preregister and run the stable-peak bracket under A2-a.
6. Fix the final objective p99 and N_raw from the A2-a sessions.
7. Preregister and run C under the accepted noise guard.
8. Then the CRITERIA review.

Competitor runs, the CRITERIA freeze and REFINE #2 remain unauthorized.
