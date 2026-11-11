# D.2 cross-node exchange accounting: validation on the dev VM (2026-11-10)

D.2 implements `docs/CROSS_NODE_EXCHANGE_DESIGN.md` (rev4, approved in
`docs/handoff/PR94_D2_Design_Rev4_Review.md`). This directory is its
evidence. It is returned for independent review: **D.2 is not complete
until that review approves it.**

Dev VM numbers are regression checks only (4 vCPUs, loopback). Every
headline suite is a complete serial run (`ctest -j1`) in its own build
directory. Every attempt is kept, in the order it ran, including the
failed ones.

## Headline

Code head of the evidence: **66d5145** (full suites at beda8a2; 66d5145
changes only X4, and test_xnode passed at 66d5145 in all three builds,
`suites/xnode-only-*-66d5145.log`).

| Suite (complete serial run, `ctest -j1`) | Head | Result |
|---|---|---|
| Release (RelWithDebInfo), `suites/release-02.log` | beda8a2 | **47 of 47 pass** |
| ASan+UBSan (Debug), `suites/asan-03.log` | beda8a2 | **47 of 47 pass**, no sanitizer report |
| TSan (Debug), `suites/tsan-02.log` | beda8a2 | **47 of 47 pass**, no race report |

Earlier suite runs, kept in order (all failures root-caused below):
`suites/release-01.log` (f7dde4e, 47 of 47), `suites/asan-01.log`
(f7dde4e, 45 of 47: X12, x7b), `suites/asan-02.log` (6489cec, 47 of 47),
`suites/tsan-01.log` (6489cec, 44 of 47: test_s1, test_gauges_admin,
X29). Each suite ran with `TMPDIR` in a scratch directory, and the
`/tmp/snc-*` count was the same before and after every run (666, none of
them from these runs).

| Evidence | Result |
|---|---|
| X1-X32 with X19b (`test_xnode`, all cases) | all pass in every headline suite and at 66d5145 in all three builds |
| Process tests x6, x7, x7b, x9 (`test_xnode_proc`) | all pass; x9 against the v1 build of 33f7c6d (`x9/x9-01.log` at c64330d, `x9/x9-02.log` at 66d5145) |
| Negatives ND1-ND38 plus ND2b, run 6 (beda8a2) | 36 of 39 caught; ND10 and ND24 change unreachable branches; ND7 missed by chance, then X4 fixed to force it and ND7 caught 3 of 3 at 66d5145 (below) |
| F8 cross-node sender gate (`test_intake`) | passes with accounting on and with `SNC_XNODE_ACCOUNTING=off` (`f8/`, at 66d5145) |
| P2 (`test_xnode P2`, 3 rounds of 5 s, accounting on vs off) | no measurable throughput cost (`p2/p2-01.log` at c64330d, `p2/p2-02.log` at 66d5145) |
| D.1 tests | pass in every headline suite. Two D.1-era tests changed: test_intake F8 (it now requires the sender's gate to refuse across nodes, as the design's 8.3 asks, and still runs D.1's way with accounting off) and test_gauges_admin (the D.2 gauge names) |

## What D.2 changes for a reader

- Wire v2: a 48-byte Hello and a 32-byte HelloAck. A v1 peer, or a peer
  with accounting set the other way, is refused with a message and a
  gauge.
- A follow-up of this shard's exchange pushed to another node stays live
  at its origin until that node ends it (coalesced END frames) or returns
  it as a counted reply. So X bounds a shard's work across nodes as well.
- A connection binds only on the HelloAck plus the SYNC naming that exact
  connection: count = k + pushed - R at the accounting barrier.
- Quarantine (In side down, blocked, or not bound, for `T_release`) and
  retirement on the member's next incarnation.
- A durable links generation (`<data-dir>/links_generation`) gives every
  links instance a strictly higher incarnation, with no clock involved.
- A blocked primary announces the blocked bit at once, leaves the links
  after `T_block`, and rejoins as a new instance.
- Heartbeats and silence detection, without TCP timeouts.

## Defects found and fixed on site

Product defects (1-2 found by the step 10 tests and fixed in 8ef83d3; 3
found by the per-commit run, fixed in f7dde4e; 4 found by the TSan
suite, fixed in beda8a2):

1. **A rejoining links instance never bound.** A blocked primary
   rejoining (2.12) met a peer incarnation equal to the highest the shard
   kept. `admit_incarnation` returned early, leaving the new entry's
   incarnation at 0, so no SYNC was ever sent. Found by X19
   (`attempts/X19-02.log`). Fixed: an equal incarnation with an empty
   entry takes it.
2. **The blocked bit could arrive late under traffic.** `set_blocked`
   pushed the bit only on identified Out connections that had room at
   that instant. Under steady traffic no idle heartbeat followed, so the
   bit waited. Found by X19 with a 100 ms bit delay once in 5 runs. Fixed:
   each connection records the bit it last carried, and liveness sends it
   as soon as the connection can take it. After the fix, 6 of 6 runs
   showed 0-2 ms.
3. **One stale heartbeat reset a healthy Out connection.** The Out-side
   rule (2.10) stamped "the peer does not see this connection" from one
   heartbeat and reset the connection T_dead later, even when no
   heartbeat came after it. In X15, B's Out link visited member 1 during
   member 0's restart, sent one heartbeat, and left; member 1's own
   connection to B was then reset (`out_resets` +1; 6 of 30 and 7 of 20
   runs at c64330d, `attempts/x15rep-01`, `attempts/x15rep-02`; the debug
   run `attempts/x15rep-03` showed one heartbeat and nothing after it).
   Fixed: the condition must be shown by a heartbeat at least T_dead
   after the first, and a gap of more than T_dead starts it over (design
   2.10 clarified). X15 0 of 40 after the fix (`attempts/x15rep-04`). New
   **X32** shows it deterministically (3 of 3 fail without the fix) and
   that persistent evidence still resets the connection after T_dead.
4. **Step 9's gauges made each publish slower than its interval (TSan).**
   test_s1 failed every run under TSan, also with accounting off. Bisected
   to step 9 (a69da05; 95afd55, 936fa4f and eafa9c6 pass,
   `attempts/bisect-s1*`). The gauge set grew from 118 to 179. Under TSan
   a publish took about 60 us, more than the 50 us interval, so a shard
   published on almost every loop iteration (measured 7,346 publishes/s
   at 61 us on the move's destination shard, from temporary
   instrumentation; the node logs with those lines were not kept, only
   the test output `attempts/exp4-rate.log` and `attempts/exp5-fail.log`,
   and the numbers are in commit beda8a2's message). Skipping D.2's gauge block made test_s1 pass 4 of 4
   (`attempts/exp-s1-nogauges1.log`, `attempts/exp2-nox-*`). Fixed: the
   interval is at least 4x the last publish's duration, counted from the
   publish's end. Release is unchanged (a publish takes about 1 us). The
   same slowness starved GAUGES reads (test_gauges_admin: shard 0
   consistent = 0, all zeros): the reader's 64 retries ran inside one
   publish. It now yields, then retries 100 us apart for up to about
   10 ms (`attempts/tsan-ga-*`, 7 of 7 pass).

Test defects (harness bugs, fixed in the tests):

- X29: the Load thread raced an engine restart and segfaulted
  (`attempts/x29-01.log`).
- X21, X13: two engines claiming one member listened on the same port.
- X21: A's Out reached the second instance; it is now pinned to the
  first.
- X16: the setup order was wrong.
- X25 (first version): the proxy's undelivered counter is not evidence.
- X19b: a stopped primary's requests and quarantined debt were treated
  as losses.
- x9: the wrong gauge was checked.
- x7: required `xnode_end_owed` = 0 at the end. ENDs owed to the killed
  primary's quarantined entry legitimately wait for a binding or its
  retirement (2.6, 2.7), and that instance never returns in x7. ENDs may
  now be owed only on a shard holding a quarantined entry, and must not
  move over 3 s. It failed at 8ef83d3, cdfca8d and c64330d in the
  per-commit run.
- X19: a bindings baseline that started at 0 made a bit seen on the
  first sample count an earlier binding as "bound while blocked". This
  is a false failure, seen about once in 30 runs; it most likely also
  explains `attempts/X19-01.log`, whose cause was not recorded at the
  time.

- X12 (ASan suite 1): the wire to B is held across a slot move. A hold
  beyond T_dead (200 ms in the tests) lets B close its In as silent, so
  the held credits never arrive and there is no stale drop to show. X12
  now runs with T_dead 5 s and checks the hold stayed below it
  (`attempts/x12-asan`, ND2 and ND11 still caught,
  `negative/attempt5-x12-recheck`).
- x7b (ASan suite 1): group 1 never became READY before its primary was
  stopped, so the witnesses could not promote. A standby catching up
  under constant writes may not get READY (docs/SYNC_READINESS.md,
  liveness under load). x7 and x7b now wait for both groups READY before
  the load (`suites/asan-01-x7b-logs`).
- X29 (TSan suite 1): it required the link bound at the instant after a
  refused replay; under TSan with T_dead 200 ms the link can be reset and
  rebound meanwhile. It now waits for the binding and checks that the
  incarnation and serial did not change.
- X4: see ND7 below.

Details and order of the step 10 ones are in `attempts/NOTES-step10.md`.

## Negatives (8.2): three runs, in order

`negative/run_negatives.sh` patches a copy of HEAD for each mutation, does
one Release build of `test_xnode`, and runs the named cases. A mutation is
caught when any named case fails.

1. **Run 1 (8ef83d3): 28 of 38 caught.** Uncaught: ND3, ND10, ND12,
   ND18, ND20, ND24, ND29, ND30, ND34, ND35. ND36 and ND37 were caught
   (X28, X29), as the review requires. Each miss was traced to a test
   that never reached the guarded path:
   - **ND3, ND12** guard defence-in-depth checks. Only the acceptor
     knows a connection's nonce, and fencing closes the old sockets, so
     no correct peer sends those frames. New **X30** injects forged END
     and SYNC frames into live streams through the proxy.
   - **ND18**: no test had tagged items queued at a retirement. New
     **X31** holds B's replies for A (WAL held) while A restarts; the
     purge must happen at once.
   - **ND20**: X15 never checked member 1's own connection. It now must
     survive member 0's restarts.
   - **ND29**: in X24, A stopped admitting work while unbound, so there
     was nothing to push. X24 now runs with a larger X and more accounts,
     and requires refused follow-ups in each unbound window.
   - **ND30** was masked twice: the In silence close and the per-member
     In replacement both closed the old In before the fence was needed.
     The fence is the only guard when reads are paused and the new
     incarnation arrives by its HelloAck. **X25** is rebuilt that way.
   - **ND34**: X19's bound (T_hb + 30 ms with T_hb 20) let an
     idle-heartbeat bit through. X19 now runs T_hb 80 with a bound of
     T_hb/2; the mutant's bit took 98 ms.
   - **ND35**: X19 never reconnected while blocked. It now cuts A's Out
     connection to the blocked primary once; it must not rebind.
2. **Run 2 (cdfca8d): 34 of 38 caught.** The 8 above were caught.
   ND2b and ND22, caught in run 1, were missed: their detection depended
   on timing.
   - **ND2b**: misroutes occur in X8 only by a race, in about 4 of 10
     runs. X8 now moves the slot back and forth until one occurs, and
     requires it.
   - **ND22**: masked when B's Out connection happened to reset after
     A's binding, so A resumed from a new In. X22 now runs a long T_dead
     and requires A's own resume at binding with no new In accepted.
3. **Run 3 (c64330d): 36 of 38 caught.** The 2 others (ND10, ND24):
   - **ND10** (adopt on a re-route) and **ND24** (hand off a tagged reply
     untagged) mutate branches that cannot be reached with the account
     type.
   - Every emit older than a move of its source's or target's slot is
     dropped as stale, at routing and at execution (`Shard::stale`), and
     a reply targets its own source. So no follow-up of another shard's
     exchange reaches a node link after a move (ND10). For the same
     reason, a moved source makes a tagged reply stale before it could be
     handed on (ND24).
   - Every run reports drops to resume 0 and hand-off drops 0.
   - The branches stay as written (fail-closed) for op types that could
     reach them.

4. **Run 4 (f7dde4e, after the X15 fix): 37 of 39 caught** (ND38 added,
   caught by X32; ND10 and ND24 as above). ND14's pattern had moved with
   the fix and was not applied in the full run; with the pattern updated
   it was run alone from the working tree and caught by X3
   (`negative/attempt4`).
5. **ND2, ND11, ND24 recheck (after the X12 change):** ND2 and ND11
   caught by X12, ND24 not (unreachable) (`negative/attempt5-x12-recheck`).
6. **Run 6 (beda8a2, final product code): 36 of 39 caught.** ND10 and
   ND24 as above. **ND7 was missed:** X4 caught it in runs 1-4, but only
   when a SYNC was pushed while ENDs were owed, and X4's 10 cuts did not
   always produce one. Alone, the mutant failed X4 6 of 6 times. X4 now
   cuts until such a SYNC happened (up to 60 cuts) and requires it
   (66d5145).
7. **ND7 recheck (66d5145):** caught by X4 3 of 3
   (`negative/attempt7-nd7-recheck`).

Test mapping that differs from the design's table (section 8.2):

- ND38 (new, implementation finding) is caught by X32.
- ND3 and ND12 are caught by X30.
- ND18 is caught by X31.
- ND22 is caught by X22.
- ND12, ND19 and ND20 were mapped to the process test X7b. The negatives
  run only the in-process `test_xnode`, so they are caught there (X30,
  X15/X16, X15).
- ND26 is a stand-in: the rev2-draft split-counter SYNC rule, applied as
  "a SYNC during quarantine does not touch the count".

## P2 (`p2/p2-01.log` at c64330d, `p2/p2-02.log` at 66d5145)

Two nodes, 3 rounds of 5 s each, accounting on, then off.

| Transfers/s | Round 0 | Round 1 | Round 2 |
|---|---|---|---|
| c64330d, accounting on | 104,765 | 105,805 | 108,791 |
| c64330d, accounting off | 104,563 | 105,908 | 104,932 |
| 66d5145, accounting on | 120,529 | 124,229 | 121,327 |
| 66d5145, accounting off | 117,910 | 120,547 | 115,647 |

The second run is higher in both modes. It was not investigated; the
gauge spacing fix (beda8a2) changes the publish schedule, and VM noise
is likely too. On and off stay within the same range in both runs.

With accounting on:

- END frames carry about 176-186 items each.
- There are 0.0018-0.0019 control frames per DATA frame.
- No heartbeat is sent under load; idle connections send about 9.5
  heartbeats per second, at T_hb 100 ms.
- No SYNC is sent in steady state. There are 0 drops to resume and 0
  unbound purges.

Binding latency over 20 cuts, idle and under load: median about 102-103
ms, maximum about 202 ms in the first run and 103 ms in the second. This
is dominated by the 100 ms reconnect backoff.

## Disclosures

- **Not every intermediate commit is green.** The review asked for each
  commit to be green. Steps 1-9 passed the per-commit subset
  (`attempts/percommit/summary.txt`). The step 10 commits do not, in
  every build: 8ef83d3, cdfca8d and c64330d fail x7 (a test defect) and,
  from cdfca8d, X15 intermittently (product defect 3); f7dde4e and
  6489cec carry the TSan regression of step 9 (product defect 4), which
  is in a69da05 too, though a69da05 passed the Release subset. The fixes
  are later commits rather than rewritten history: 8ef83d3 was already
  pushed, and every attempt is kept in order. Only beda8a2 and 66d5145
  pass everything.

- X7's "retires on restart" is shown by X6 (kill -9 and restart). X7
  itself shows the failover and the quarantine.
- X12's in-node hand-off branch is unreachable (ND24 above). X12 shows
  the stale-drop path with ENDs on the tag.
- X14's first-quarantine physical baseline can be 0 when nothing was
  held at that moment; the bound is still checked at every sample.
- **Upgrade:** a pre-D.2 data directory has no `links_generation` file,
  so the engine refuses to start. That is fail-closed, as approved.
  Upgrading needs `--links-generation-min 1` once (x9 does this).

## Every run, in order

All in `attempts/` unless another folder is named. In order:

1. Per-step subsets while implementing: `step5_subset_1`, `step5_subset_2`,
   `step6_subset_1`, `step7_subset_1`, `step8_subset_1`, `step9_subset_1`,
   `step10_subset_1`.
2. Step 10 case attempts, each case's own numbering in order: `X1-01`,
   `X2-01`, `X8-01`, `X11-01`, `X12-01`, `X13-01..02`, `X15-01`,
   `X16-01..03`, `X17-01`, `X19-01..03`, `X19b-01..03`, `X20-01`,
   `X21-01..03`, `X25-01..02`, `x29-01..02`; process tests `proc-x6-01`,
   `proc-x7-01`, `proc-x7b-01`, `proc-x9-01..02`; whole runs
   `xnode-all-01..03` (notes: `NOTES-step10.md`).
3. Negatives run 1 (8ef83d3) `negative/attempt1`, run 2 (cdfca8d)
   `negative/attempt2`, run 3 (c64330d) `negative/attempt3`.
4. P2 at c64330d `p2/p2-01.log`; x9 against the v1 build `x9/x9-01.log`.
5. Per-commit subset, attempt 1: stopped by a full disk
   (`percommit-attempt1-diskfull`). Attempt 2 (`percommit`): steps 1-9
   pass; 8ef83d3, cdfca8d, c64330d fail (x7, X15).
6. X15 repetitions at c64330d: `x15rep-01` (6 of 30 fail), `x15rep-02`
   (7 of 20, with counter deltas), `x15rep-03` (debug output); after the
   fix `x15rep-04` (0 of 40).
7. At the fix: `xnode-all-04`; `proc-x6-x7-x7b-02..04`;
   `suites/release-01.log`; negatives run 4 `negative/attempt4`.
8. `suites/asan-01.log` (2 failures); `x12-asan` (16 runs, X12 passes);
   `xnode-asan` (6 whole runs, pass); `proc-asan-x7-x7b-01..02`;
   `negative/attempt5-x12-recheck`.
9. `suites/asan-02.log` (6489cec, pass); `suites/tsan-01.log` (3
   failures; node logs in `suites/tsan-01-kept-logs`).
10. TSan root cause: `s1-tsan-normal-01..02` (fail),
    `s1-tsan-normal-acctoff-01` (fail), `s1-tsan-normal-d1base-01..02`
    (95afd55, pass), bisection `bisect-s1*`, experiments `exp-s1-*`,
    `exp2-*` (3 each: unchanged, no D.2 gauge block, block without the
    state and counter reads), `exp3-costgap-1..2`, `exp4-rate`, `exp5-*`;
    after the fix `tsan-fix-subset-01` (gauges_admin still failing),
    `tsan-ga-01..06`, `tsan-fix-subset-02`, `tsan-x29-01..03`.
11. `suites/tsan-02.log`, `suites/asan-03.log`, `suites/release-02.log`
    (beda8a2, all pass); negatives run 6 `negative/attempt6`; ND7 recheck
    `negative/attempt7-nd7-recheck`.
12. At 66d5145: `suites/xnode-only-{build,build-asan,build-tsan}-66d5145.log`; `x9/x9-02.log`; `f8/`;
    P2 `p2/p2-02.log`.
