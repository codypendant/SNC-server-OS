# Baseline readiness: pre-baseline test results

**Status: RESULTS FOR REVIEW. Three Profile B BLOCKERS were reproduced
(B1, B2, B3), and none is fixed here.** This is step 2 of the order in
[EXPERIMENTAL_BASELINE.md](EXPERIMENTAL_BASELINE.md) section 13 (the
review of PR #45, `handoff/PR45_Baseline_Review.txt`). Following that
review, each blocker is characterized, its evidence is kept, and the
smallest fix is proposed for a separate reviewed slice. The baseline
cannot be frozen until the fixes land and their tests pass.

- **Host:** the 4-vCPU development VM. These are correctness and
  characterization results only, never performance claims (section 12
  of the baseline document).
- **Evidence:**
  [`bench/results/baseline-readiness-20261105/`](../bench/results/baseline-readiness-20261105/).
  It holds one JSON file per scenario, plus the logs and stacks for the
  blockers.
- **Code:** test, harness and instrumentation only. Production behaviour
  is unchanged unless a `--debug-admin` hook is used (section 5).

## 1. Results

| Test | What ran (real processes) | Result |
| --- | --- | --- |
| T-Q1 | Seven resend paths through the shipped `net::Client` (section 4) | **PASS** on every failure path: the oldest resend was 10.2 s. **FINDING F2:** while a sync primary is blocked by a control-plane outage, the age grows with the outage (35.3 s for a 40 s outage). That is not the 35 s bound section 7 states |
| T-Q2 | Lost answers, then a slot move, then resends through `ClusterClient` (`test_baseline_move.py q2_lost_move`) | **PASS** |
| T-Q3 (run time) | Checkpoint every 1 s, then SIGKILL the standby under load (`test_baseline_ha.py q3_runtime`, 3 trials) | **BLOCKER B1**, reproduced in 1 of 3 trials |
| T-Q3 (restart) | Standby gone, then the primary crashes and restarts with another shard count (`q3_restart`) | **PASS** (section 2, B1 note) |
| T-Q4 | Standby SIGSTOPped at full write load; witness grant at once, after 10 s, after 60 s (`q4_normal`, `q4_delayed10`, `q4_delayed60`) | **PASS**. **FINDING F1:** replication retention fills to its cap (shards x 1 GiB) after the grant |
| T-Q5 | Two consecutive failovers during a resend storm, with exact balances (`test_baseline_retry.py q5_kill_kill`, `q5_kill_stop`) | **PASS** once the rejoined standby has caught up (release, ASan). **BLOCKER B2** if the second failure comes while it is still catching up (`q5_catchup_race`, 3 of 3). **BLOCKER B3** under TSan: the slowed standby keeps missing the ack timeout during catch-up and the promoted primary's RSS grows until the OOM killer ends it (section 3b; reproduced in release) |
| T-Q10 | Checkpoint during a move under load, then every node killed and restarted, at copied / sealed / staged / committed / activated (`test_baseline_move.py q10_*`) | **PASS**, 5 of 5 |
| T-H | `bench/profile_check.py`, used by `bench/lab/lab.py` (`test_profile_check.py`, 10 cases) | **PASS**: each refusal is tested, and a valid Profile B (also as `lab.py` builds it) passes |

**Suites** (this branch, 4-vCPU VM): release 26/26. ASan+UBSan 26/26
after the `q4_normal` memory criterion took sanitizer shadow memory into
account. TSan 24/26 on the first run: `q4_normal` (the same shadow-memory
criterion; TSan measured 3.6x RSS per retained byte, so TSan builds allow
6x and the rerun passed) and `q5_kill_kill` (BLOCKER B3, section 3b: it
stays red under TSan until B3 is fixed). No TSan data-race report.

**T-Q2 detail:** the source's answers to sequenced requests were
discarded (`DEBUG DROP-ANSWERS`) while the slot moved.

- The move then completed (DONE, epoch 2), and the answers flowed again.
- The client timed out and resent with the same session and sequence. The
  source's session answered all four from the session (`replayed 4`),
  not as `E_MOVED`, and nothing ran twice.
- New requests got `E_MOVED`, refreshed, and went to the destination.
- Every transfer settled (AUDIT pending 0 on both groups). The check was
  exact, with nothing in doubt: 400 accounts, each served once.

**T-Q4 detail:** while the primary was blocked:

- RSS grew 3-9 MB in total; heap stayed flat.
- Held answers peaked at 256, the client's window (4 threads x 64).
- Ring overflow and held follow-ups stayed at 0.
- Unacknowledged WAL reached 18-39K records.

At the grant, the held answers drained. The 60 s delay grew no faster
than the 10 s delay (RSS 87 KB/s against 902 KB/s while blocked).
At the blocked rates of the 10 s and 60 s runs, reaching 4 GiB would
take more than an hour.

**T-Q10 detail:** after the restart, both primaries hold the slot state
they checkpointed. Once the placement is committed, they adopt it at the
start barrier: the destination becomes ACTIVE and serving, and the source
stays sealed, so the slot never has two serving owners. A new driver
finishes the move, and the check is exact.

## 2. BLOCKER B1: a checkpoint wedges a sync primary whose standby dies (T-Q3, matrix row 13)

**Observed** (`q3_runtime.json` trial 2, `q3_runtime_t2_stack.txt`):

- The witness never granted DEGRADE.
- HEALTH `main_ms` reached 14762 at 15 s and kept rising.
- STATUS showed `blocked=2`, and every write after the kill failed.
- The main thread was in `std::latch::wait` inside `Engine::checkpoint`
  (`engine.cpp:882`), called from the main loop of `snc_server`.

Trials 0 and 1 passed only because no checkpoint was in flight when the
standby died.

**Cause:**

- `Shard::checkpoint_step` (`src/core/shard.cpp:1629`) waits for
  `durable(c.max_lsn)`.
- In a sync group, `durable()` also needs the standby's ack
  (`shard.h:411-415`: `repl_hold_` and `acked_lsn`). The standby is dead,
  so that ack never comes.
- The same main thread is the only one that asks the witnesses for
  DEGRADE (`snc_server.cpp`, main loop). So it never asks, and the
  primary stays blocked for good.
- No timeout ends this, and checkpoints cannot be turned off to avoid
  it (review condition 3).

**Proposed smallest fix (for review; not implemented):** make the
checkpoint's wait local. In `checkpoint_step`, wait for
`c.max_lsn <= wal_->durable_lsn()` instead of `durable(c.max_lsn)`.

- Spec C5 asks that a checkpoint part hold only state whose WAL record is
  durable on this node. Recovery replays this node's own WAL, so a part
  can never hold state that the local WAL cannot already recover.
- The standby's ack decides when a client gets its OK. It does not decide
  what this node may write to its own disk.
- Divergence after a failover is unchanged: a node that rejoins is
  re-seeded (`--rejoin` moves its directory aside).

Defense in depth (optional, a larger change): ask DEGRADE from a thread
other than the one running the checkpoint.

The fix's tests:

- `q3_runtime` passes 20 of 20 trials.
- The restart case asserts that the step-8 recovery checkpoint actually
  ran: today's `q3_restart` passes, but it does not prove that path ran
  (matrix row 19c).
- `test_persistence` and `test_crash` still pass.

## 3. BLOCKER B2: the witnesses promote a standby that is still catching up (T-Q5, matrix rows 5 and 12)

**Observed** (`q5_catchup_race.json`, `q5_catchup_race_logs.txt`;
reproduced 3 of 3):

1. Node 0 (primary, term 0) is SIGKILLed, and node 1 is promoted (term 1)
   in 0.93 s.
2. Node 0 restarts with its own command line. `--rejoin` finds node 1 as
   primary and moves its directory aside. It is seeded from node 1's
   checkpoint 1 (3 files, 0.0 MiB, an early checkpoint) and starts as
   node 1's standby 0.02 s later. It still has to catch up about 830K WAL
   records.
3. Node 1 is SIGKILLed. Node 1 had never degraded: its standby was back
   inside `--replica-timeout-ms`. So the witness state was still
   `degraded=false`, and node 0 met every eligibility check: it is
   standby, with the same space UUID and the same term.
4. The witnesses promoted node 0 to term 2 in 0.99 s.
5. Node 0 served an empty space: all 2,000 accounts were `E_NOT_FOUND`.
   Every acknowledged write of terms 0 and 1 was lost, on a node that
   reported healthy.

The two controls isolate the cause:

- `q5_catchup_race_late`: node 0 rejoins 2 s after the promotion,
  after the replica timeout. Node 1 first runs alone under a DEGRADE
  grant, so the witness state is `degraded=true` and nobody is promoted.
  The group is unavailable, not wrong: correct fail-closed behaviour.
- `q5_kill_kill` and `q5_kill_stop`: the second failure waits until node
  0 has caught up (node 1's unacknowledged WAL down to the client's
  window). Both pass with exact balances. Each client thread followed both
  failovers, and the oldest resend was 5.0 s. After `q5_kill_stop`, the
  stopped old primary is fenced.

**Cause:** a witness reads `degraded=false` as "the standby holds every
acknowledged write" (`snc_witness.cpp`, promotion check). That is true
for a standby that was in sync all along. It is not true for a standby
attached in this term that has not caught up yet. A promotion copies the
old term's `degraded=false` into the new term, although the new primary
has no in-sync standby. The rejoin also makes the window real: a
supervisor restart is fast, the seed is quick, and the WAL catch-up is
long. Matrix row 12 inferred that a seeding node cannot be promoted
because it is still in `Engine::start`. That holds only during the seed.
During the catch-up that follows, the node answers STATUS as a normal
standby.

**Proposed smallest fix (for review; not implemented):**

1. **Witness:** a promotion records the new term as degraded
   (`want.degraded = true`). The new primary has no standby known to hold
   its writes.
2. **Server:** the promoted primary starts as if it had a grant on
   record. Its shards begin degraded, and the main loop owes SYNCED.
   - It sends SYNCED through the existing path (`snc_server.cpp`,
     `owe_synced`). That happens only once every shard has cleared
     degraded, and the existing rule clears it only when the standby is
     live and `acked_lsn >= durable_lsn` (`shard.cpp:1523`).
   - Until then the witnesses promote nobody: the group fails closed, as
     the late-rejoin control already does.

This reuses the tested DEGRADE/SYNCED path, adds no new protocol, and
changes nothing for a group that never failed over. The cost: between a
promotion and the new standby catching up, OKs are LOCAL_DURABLE. That is
already what Profile B's contract says for "after a failover until a new
standby is seeded", so section 9 run tagging must use the witness state,
not only `degraded` in STATUS.

The fix's tests:

- `q5_catchup_race` passes: no promotion while node 0 catches up, then a
  promotion with exact balances once it has.
- `q5_kill_kill`, `q5_kill_stop`, `test_witness.py`, `test_health.py`
  and `test_fail_closed.py` still pass.
- A test of the clustered (`--control`) path does the same.

## 3b. BLOCKER B3: a cancelled catch-up reader keeps reading into memory (T-Q5 under TSan, matrix row 12)

**Observed.** Under TSan, `q5_kill_kill` failed twice in a row: node 0
"never caught up". Sampling showed why. After node 0 rejoined, node 1 (the
promoted primary) logged `standby shard N stopped acking: treating it as
down` again and again, and its RSS grew about 260 MB/s while its
replication retention stayed small (37 MB at 5.9 GB RSS). The host's OOM
killer ended it at about 12.7 GB
(`b3_tsan_q5_rss_samples.txt`). A smaller retention cap
(`--replica-retain-mib 256`) changed nothing: the memory is not retention.

**Reproduced in release** (`b3_repro.py`; retention capped at 16 MiB per
shard so the rejoin catches up from disk; node 0 SIGSTOPped 0.7 s of every
1 s for 30 s, so it misses the 500 ms ack timeout):

| Run | `stopped acking` | Node 1 peak RSS | Retained |
| --- | --- | --- | --- |
| Standby stalled (`b3_release_standby_stalled.json`) | 58 | **2,059 MiB** | 31 MiB |
| Control, no stalls (`b3_release_control.json`) | 0 | 283 MiB | 31 MiB |

In release the memory comes back once the readers finish (RSS falls back to
about 195 MiB); under TSan, with a slower reader and shadow memory, it
reached the host's limit first.

**Cause** (`src/core/replication.cpp`, `Sender::start_disk` /
`stop_disk`). Catch-up from WAL files runs on a detached reader thread that
queues frames for the shard, at most `kDiskQueued` (16 MiB) ahead.
`stop_disk()` (every `close_link`: ack timeout, write error, reconnect)
sets `cancel` and drops the shard's reference. But the reader only looks
at `cancel` as a reason to stop *waiting* for queue space: it then keeps
reading the rest of its range and queues all of it, with no limit and no
consumer, until the range ends. Every reconnect during catch-up starts a
new reader from the standby's acknowledged LSN, so each failed attempt
adds another copy of the remaining range to memory, read concurrently.

**Impact on Profile B.** Any standby that catches up from disk and drops
its link while doing it (slow host, network flap, a standby that is still
applying when `--replica-timeout-ms` passes) makes the primary hold a
copy of the remaining catch-up range per attempt. A long outage means a
large range, so a primary under write load can run out of memory: an
availability failure on the healthy node, caused by the standby. The
data stays correct (the shard never sends the abandoned frames).

**Smallest fix proposed** (a separate, reviewed slice; not applied):

- In the reader, treat `cancel` as stop: after the wait in `push()`,
  drop the frame and stop if `cancel` is set, and make the record callback
  return at once once stopped (or give `read_stream_range` a stop
  predicate so it also stops reading the files).
- Its tests: `b3_repro.py` with the standby stalled stays within the
  control's peak RSS (plus the 16 MiB read-ahead per live reader), and
  `q5_kill_kill` passes under TSan; `test_replication`, `test_health`
  and the T-Q5 scenarios still pass.

## 4. T-Q1: resend age per path (R11)

The client is `net_bench --session` (the shipped `net::Client`:
`failover_ms` 30000, `request_timeout_ms` 5000) against the sync pair
with three HEALTH witnesses. The **age** is the time from a request's
first send to its resend.

| Path | Scenario | Oldest resend | Notes |
| --- | --- | --- | --- |
| Primary SIGKILL | `q1_kill` | 0.93 s | Promotion 0.93 s; exact |
| Primary SIGSTOP (hung) | `q1_stop` | 10.2 s | Request timeout 5 s, plus the hung node's dial and topology timeout of 5 s; old primary fenced |
| Partition, every path of the primary | `q1_partition` | 10.1 s | As `q1_stop`; old primary fenced after the heal |
| Flap: 5 cuts of 0.5 s | `q1_flap` | 0.58 s | No promotion (term 0); reconnect and resend to the same primary |
| Shard stall 7 s (no promotion: HEALTH) | `q1_stall` | 7.0 s | Two request timeouts; resent to the same primary |
| Two failovers (T-Q5) | `q5_kill_kill`, `q5_kill_stop` | 1.0 s, 5.0 s | |
| No primary at all | `q1_failed` | n/a | The client gives up after 30.0 s (its failover deadline); the application gets an error; nothing is resent later |
| **Control-plane outage while blocked** (standby and every witness stopped 40 s) | `q1_blocked` | **35.3 s** | See F2 |

Not run here: a one-way partition (it needs iptables on real hosts; the
lab's `oneway` fault covers it on the R640/VPS runs) and flaps longer
than `failover_ms`.

**Every failure path is far below the TTL** (3600 s; more than 350x
margin), and every run was exact.

**FINDING F2:** section 7's statement that a request is "resent within
about 35 s or never" is wrong in one case.

- A sync primary that is blocked (standby gone, no grant possible) keeps
  its connections and answers the client's reconnect probe.
- So each request timeout leads to a reconnect to the same primary and
  another resend. `fail_over` succeeds every time and starts a fresh
  deadline.
- The age therefore follows the outage. It was 35.3 s for a 40 s outage
  here, and it would reach the TTL only with an outage of about an hour.
- Meanwhile no checkpoint, and so no expiry, can complete on that
  primary while it is blocked (B1), and the session's time comes from
  the request's own apply.

**Proposal for R11 (for review):** make R11 non-blocking, with the bound
restated as follows:

> the larger of `failover_ms + request_timeout_ms` and the longest
> control-plane outage during which a primary stays blocked, plus one
> request timeout

Under that bound:

- A campaign run whose control-plane outage passes 10 minutes is invalid
  (section 9); that is 6x margin.
- No TTL change.

If a hard client-side bound is wanted instead, the smallest change is a
maximum in-doubt age in `net::Client` (give up past it, as on a failed
failover). That would be a separate slice.

## 5. Findings that are not blockers

**F1. Replication retention after a grant (T-Q4, matrix row 14).**

- After a DEGRADE grant, the primary keeps shipping-ready WAL for the
  absent standby up to `--replica-retain-mib` per shard (default 1 GiB).
- At this VM's write rate (about 71 MB/s of WAL after the grant),
  retention reached its 2 GiB cap (2 shards) within about 30 s, and RSS
  reached 2.4 GB. Both then plateaued.
- On the R640 with one shard per core, that is tens of GiB.
- Profile B must therefore declare `--replica-retain-mib` and count it in
  memory reporting. The proposal: T-H requires it to be named in Profile
  B. It is not yet enforced, pending this review.
- Growth while blocked is bounded by the client window (F1 is about the
  grant, not the block).

**F3. The T-Q3 restart case does not prove its path.** `q3_restart`
passes: the admin port opened in 0.5 s, DEGRADE was granted, and a write
succeeded. But it does not show that the shard-count change ran the
recovery checkpoint with the standby gone (row 19c). B1's fix should
assert that.

## 6. T-H: the profile guard

[`bench/profile_check.py`](../bench/profile_check.py) checks every
`snc_server` and `snc_witness` command line and the `snc_ctl init` before
the harness starts them.

**Always refused,** even if declared as a deviation:

- async with witness HA;
- a witness polling STATUS;
- a data directory that already exists at its first start in the run;
- a different `--shards` when the run restarts a directory;
- `--standby-reads` in Profile B;
- each profile's own shape:
  - B: every node is a sync primary or standby; every primary has
    `--witness`; the placement is written as v2;
  - A and C: no replication;
  - C: no data directory;
  - D: no witness HA.

**Refused unless declared:**

- a fixed setting at another value than section 1 gives:
  `--group-commit-us 50`, `--pages 4k`, `--move-units on`,
  `--checkpoint-s 60`, `--replica-timeout-ms 2000` (an absent flag counts
  as the binary's default);
- any flag outside the profile's shape (for example `--debug-admin`).

A deviation is declared in the `[lab]` table as `deviations =
["--flag", "--flag=value"]`. `result.json` records the profile, the
deviations made, and any declared but unused.

`lab.py` with `profile = "B"`:

- builds its command lines through the same functions the test checks;
- adds `--checkpoint-s 60`;
- writes the placement as v2 for `init` and for moves.

A loopback P5 run (a slot move and back under load) with `profile = "B"`
passed its check exactly, with no deviations.

Limits, for review:

- `lab.py` builds Profile B clusters only. It refuses another declared
  profile, and refuses `p1`/`p2` under a profile, because those start
  their nodes outside the guard.
- The guard reads command lines exactly as the binaries do: a flag takes
  the next token as its value, and a repeated flag is refused.
- A directory is tracked per host, and a second node on the same
  directory is refused.
- The campaign scripts for profiles A, C and D (step 7) must call the
  same guard. None exists yet.

## 7. Instrumentation added (for these tests)

| What | Where | Effect when unused |
| --- | --- | --- |
| `RESOURCES` admin verb (heavy queue): `rss_kb`, `hwm_kb`, `heap_live`, `waiters`, `overflow`, `emits_in_flight`, `emits_held`, `repl_retained`, `wal_unacked`, `wal_undurable`, `blocked`, `degraded` | `tools/snc_server.cpp`; gauges in `ShardStats` (`waiters_now`, `overflow_now`, `wal_next_lsn`); `sri_overflow_queued()` in both runtimes | Read-only, answered on request |
| `net::Client::resends()`, `max_resend_age_ms()` | `include/snc/net.h`, `src/net/client.cpp` (a send time per unanswered request) | One timestamp per session request |
| `DEBUG DROP-ANSWERS 1\|0` (`--debug-admin` only): answers to sequenced requests are discarded after they ran | `src/net/server.cpp`, `tools/snc_server.cpp` | One relaxed atomic load per answer |
| `net_bench` prints `resends` and `max_resend_age_ms`; `snc_load` prints `replayed`, `resends` and `max_resend_age_ms` | `bench/` | Output only |

## 8. How to rerun

```
python3 tests/test_baseline_retry.py build [q1_kill ... q5_catchup_race_late] [--json DIR]
python3 tests/test_baseline_move.py  build [q2_lost_move q10_copied ... q10_activated] [--json DIR]
python3 tests/test_baseline_ha.py    build [q3_runtime q3_restart q4_normal q4_delayed10 q4_delayed60] [--json DIR]
python3 tests/test_profile_check.py
```

`ctest` runs the passing scenarios that are short enough:

- `test_baseline_retry`: `q1_kill`, `q1_stop`, `q1_stall`, `q5_kill_kill`;
- `test_baseline_move`: `q2_lost_move`, `q10_sealed`, `q10_committed`;
- `test_baseline_ha`: `q3_restart`, `q4_normal`;
- `test_profile_check`.

The blocker scenarios (`q3_runtime`, `q5_catchup_race`) and the long ones
run by name. They move into `ctest` with their fixes.

## 9. What this changes for the freeze

Section 11 of the baseline document still holds. In addition:

- **B1, B2 and B3 must be fixed** in separate reviewed slices, and their
  scenarios must pass, before the freeze.
- **R11 needs a decision** on the restated bound (section 4), or the
  client-side cap.
- **F1:** Profile B declares `--replica-retain-mib`. Should T-H enforce
  it?
- **X1** is implemented as its own slice (EXPERIMENTAL_BASELINE section
  4). It does not depend on B1, B2 or B3.
