# Release run 2 (2c0d1db) investigation, in order

Release run 2 (`../release-02.log`): 45 of 47. Engine code was identical to
b77937e, where Release run 1 passed 47 of 47.

1. **test_baseline_retry b3_stalled_catchup.**
   - Alone at 6.8 GB free: failed 3 of 3 (`b3-alone.log`). The first two
     entries there are invalid (wrong argv) and are kept.
   - Main 33f7c6d (before D.2), at 6.8 GB free: failed 2 of 2
     (`b3-main-33f7c6d.log`; runs 1-2 are invalid, the build had no net_bench).
   - cody then approved deleting 699 leaked /tmp/snc-* dirs, about 17 GB;
     free space went to 24 GB. b3 then passed 3 of 3.
   - Cause: the session's disk allowance being nearly used. The same thing
     happened in D.1 (intake-d1-validation README). How low space slows
     catch-up was not traced.
2. **test_paxos_ballots c2b.**
   - The proposer's 1 s deadline ran out after 5 ballots (`../release-02.log`).
     The test took 24 s against 2 s in run 1.
   - Alone it passed 3 of 3 (`paxos-alone.log`).
   - fsync probes at the time showed no stall (`fsync-probe-*.txt`, p999 3 ms;
     with a du of /tmp/snc-* running, no change).
   - Cause NOT established. A host stall during that window is inferred,
     not shown. Witness code was not changed by D.2.
3. **test_intake (F8 run outside the suite).** One accounting-on Release run
   failed `as.refused_exchange >= refused`
   (`../../f8/test_intake-accounting-on-build-2c0d1db.log`).
   - Loops of the whole binary: loop 1 (300 runs) and loop 2 (500) failed G1
     7 times with E_OVERLOADED.
   - Loop 3 printed the refusal reasons: d's S1 WAL buffer budget
     (refused_wal_buffer 1), with every other reason 0. This is a legitimate
     refusal when d's WAL write is slow; G1 is D.1's test and is unchanged
     by D.2.
   - The F8 failure was not reproduced in 1,600 runs. Its check assumed that
     every refusal at A comes from X; the same S1 limit can refuse there too.
   - Fix (test only):
     - G1 resends a transfer that only the WAL budget refused.
     - F8 checks that the client's refusals equal A's overloaded delta, that
       X refused at least one, and prints the breakdown by reason.
   - Loop 4 (400 runs): one other F8 failure. Its links were not up after
     10 s (`test_intake.cpp` "links up" wait). It was not reproduced in loop 5
     (600 runs, 0 failed).
   - Cause NOT established. A suspect: the test's ports, 45000-59950, lie in
     the ephemeral range 32768-60999. The wait now prints the ports and
     connect counts if it recurs.
