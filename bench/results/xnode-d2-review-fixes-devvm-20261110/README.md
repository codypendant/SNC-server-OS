# D.2 review fixes (PR #95 review 1): validation, dev VM, 2026-11-10/11

This is the evidence for the fixes that review 1
(`docs/handoff/PR95_D2_Implementation_Review.md`) asked for. The earlier D.2
evidence stays in `../xnode-d2-validation-devvm-20261110/`, unchanged.

**Final head: 2bccdbe.** Engine code (src/, include/, tools/) is the same from
b77937e to 2bccdbe. Every later commit changes only tests, docs or evidence.

## Headline (all at 2bccdbe)

| Gate | Result | Where |
|---|---|---|
| Release suite, serial (`ctest -j1`) | 47 of 47 | `suites/release-04.log` |
| ASan+UBSan suite, serial | 47 of 47 | `suites/asan-03.log` |
| TSan suite, serial | 47 of 47, no TSan report | `suites/tsan-04.log` |
| Complete test_xnode (Release) | 33 of 33 cases (X1 to X35, X12 now with a, b, c; X19b; no X6, X7, X7b or X9, which are process tests) | `xnode/test_xnode-release-2bccdbe.log` |
| Process tests | x6, x7, x7b, x33 pass; x9 against the v1 build 33f7c6d passes | `proc/` |
| Negatives | **42 of 42 caught** | `negative/attempt2/summary.txt` |
| F8 (test_intake), accounting on and `SNC_XNODE_ACCOUNTING=off` | passes in Release, ASan and TSan, both settings | `f8/*-2bccdbe.log` |
| P2 (3 rounds of 5 s, on vs off) | on 113.8k/s, off 115.0k/s (mean; −1.0%, inside the ±7% spread between rounds); binding after a cut: median 102.4 ms (the 100 ms reconnect backoff included) | `p2/p2-02-2bccdbe.log` |

**Invariant counters.** Every test_xnode case ends with `check_zero_counters`.
It requires these to be 0 on every node and shard:

- xnode_end_underflow
- xnode_held_underflow
- xnode_debt_increases
- xnode_uncounted
- xnode_drops_to_resume
- xnode_handoff_drops

Under ND24, X12c saw 35 hand-off drops. That is the I2 check firing, as the
proof predicts.

## The four review items

1. **Generation-file lock (X33, process x33).**
   - An explicit `--links-generation-file` holds `flock(LOCK_EX|LOCK_NB)` on
     `<path>.lock` for the allocator's lifetime.
   - A second allocator, in-process and in another process, is refused with a
     message.
   - The process test: B refused with rc 1, A last 2, B last 4.
   - ND40 (no lock) is caught by X33.
2. **Blocked bit retried on both edges (X34).**
   - `blocked_sent` is −1/0/1. A HEARTBEAT is owed while it differs from the
     current state, and goes at the next liveness pass (T_hb/4).
   - X34 sets and clears blocked while P's Out queue takes no frame (hook
     `debug_full_incarnation`). A cleared the bit 110.6 ms after room came
     back, with T_hb 1000.
   - ND39 (a one-sided retry) is caught by X34.
3. **END v2 layout (X35).**
   - Byte for byte: n (u64) in source.hi, bytes 0..7; target incarnation in
     target.lo, bytes 24..31; kind 1 at byte 41; every other byte 0.
   - The receiver ignores an END with n = 0 or another target. A real END
     from B matched the table.
   - ND41 (the rev4 mapping) is caught by X35.
   - The table is in design 2.2.
4. **ND10/ND24.**
   - Proofs I1 and I2 are in design 2.9.1. Their branches are now invariant
     checks: logged, an abort in a debug build, fail-closed in a release one.
   - The negatives mutate premises:
     - ND10: the source half of `stale()` returns false. Caught by X12.
     - ND24: the run-time staleness test is set to false.
   - ND24 was NOT caught in attempt 1 (below). X12c was added for it and
     catches it.

**Requested evidence, from `xnode/test_xnode-release-2bccdbe.log`:**

- **X17 bound:** 20 churn cycles. At all 1356 samples live_s < X + max(E, Rr)
  = 1088 (peak 140), and physical ≤ 16320 (peak 411).
- **X28 barrier:** 7872 bytes queued ahead of the SYNC. At the SYNC the count
  went 3130 → 0 = k recorded at the barrier.
- **X29 incarnation:** 25 restarts under a faked clock, each incarnation
  above the last. 25 replayed old Hellos were refused. A crash at each of
  3 update steps never reused a value.

## Every run, in order

1. **Targeted at b77937e** (`targeted/`): X33, X34, X35 and the related cases
   pass in Release, ASan and TSan.
2. **Release run 1 (b77937e):** 47 of 47 (`suites/release-01.log`).
3. **ASan run 1 (b77937e):** 47 of 47 (`suites/asan-01.log`).
4. **TSan run 1 (b77937e):** 46 of 47 (`suites/tsan-01.log`). test_baseline_ha
   q4_normal failed: one summed sample was 280 > 272.
   - Probes (`q4-probe/`, 4 runs) showed the sum adds per-shard gauges
     published up to 84.5 ms apart, while coherent samples stayed ≤ 256
     (skew ≤ 14.1 ms).
   - Test defect, fixed in fb26462: the bound counts only samples with skew
     ≤ 20 ms.
   - Not verified: the skew may come from beda8a2's publish spacing. The
     product is unchanged.
5. **TSan run 2 (fb26462):** 46 of 47 (`suites/tsan-02.log`,
   `tsan-02-test_xnode.log`). X4 saw no SYNC that subsumed owed ENDs in
   60 cuts; it relied on timing.
   - Fixed in 2c0d1db: one forced cut, with B's links taking no frame and A's
     frames held at the proxy, so B's binding SYNC must subsume the ENDs.
   - While debugging, the setup was undone and retried when A had nothing at
     B: A's own admission refused all its transfers for hundreds of ms
     (E_OVERLOADED, count 0).
   - Release 30 of 30, TSan 8 of 8.
6. **Release run 2 (2c0d1db):** 45 of 47 (`suites/release-02.log`). The
   investigation is in `suites/release-02-investigation/NOTES.md`.
   - **b3_stalled_catchup:** low disk (6.8 GB free). It failed on main
     33f7c6d too. After cody approved deleting 699 leaked /tmp/snc-* dirs
     (about 17 GB), it passed 3 of 3 at 24 GB free.
   - **paxos c2b:** the proposer's deadline ran out. It passed 3 of 3 alone.
     **Cause not established** (fsync probes were clean).
7. **F8 at 2c0d1db** (`f8/*-2c0d1db.log`): accounting on, Release, failed once
   (`refused_exchange >= refused`). ASan and TSan passed, on and off.
   - Loops of the whole test_intake binary found G1 E_OVERLOADED 7 times in
     800 runs. The refusal came from d's S1 WAL buffer budget, every other
     reason 0.
   - Test defect, fixed in 1b086ce: G1 resends that one; F8 counts the
     refusals by reason.
   - In one of 400 runs, F8's links were not up after 10 s. **Cause not
     established**: not seen again in 600 runs. The wait now prints ports and
     connect counts.
8. **Release run 3 (1b086ce):** 47 of 47. **ASan run 2:** 47 of 47.
   **TSan run 3:** 47 of 47 (`release-03`, `asan-02`, `tsan-03`).
9. **Negatives attempt 1 (1b086ce):** 41 of 42 (`negative/attempt1/`).
   - ND24 was not caught by X8, X12 or X17. The routing-time test caught
     every stale item they made, so nothing sat routed and unrun across a
     move.
   - X12c (2bccdbe) pauses B's shard 1 follow-ups while the source slot
     moves. Unmutated it passes Release 10/10, TSan 3/3, ASan 2/2. Under
     ND24 there are no stale drops and the I2 check fires.
10. **Final gate at 2bccdbe:** Release run 4, ASan run 3 and TSan run 4,
    each 47 of 47. Negatives attempt 2: 42 of 42. Then F8 (all builds, on
    and off), P2, the complete test_xnode and the process tests.

## Disclosures

- **I2's proof:** its last step is an ordering argument (design 2.9.1).
  X12c shows that its run-time premise is load-bearing.
- **Two failures with no root cause:** paxos c2b in Release run 2, and the
  F8 links-up timeout seen once. Neither code path was changed by D.2's
  review fixes. Both are recorded above.
- **Disk:** the session's allowance is about 40 GB. Since 2026-11-11, leaked
  test dirs are deleted after every run (cody's standing OK).
