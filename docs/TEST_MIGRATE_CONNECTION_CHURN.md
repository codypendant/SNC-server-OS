# test_migrate connection churn (R4): measurement and guard (2026-11-15)

Assignment: cody, 2026-11-15 12:31Z ("Yes" to the connection-churn slice recommended in the PR #128 review,
`docs/handoff/PR128_Test_Migrate_Review_Approval.md`). Background: risk R4 in `docs/TEST_MIGRATE_ROOT_CAUSE.md`.

Labels: **Observed** (read from a log, counter or source), **Inferred** (follows from observations, not shown
directly), **Open** (not explained).

## Summary

1. Every control request in SNC opens its own TCP connection and closes it after one reply: witness health
   polls, witness-to-witness Paxos, a node's placement and lease calls to the witnesses, peer health checks,
   and this test's probe. **Observed** (`tools/ha.h` `ha::ask`; `tools/admin_loop.h` closes after the reply).
2. One full `test_migrate` run therefore opens about 70k to 180k connections in 45 to 50 s (section 2).
   A host that tracks loopback connections keeps each one in its conntrack table for
   `nf_conntrack_tcp_timeout_time_wait` (120 s) after it closes. Hosted runners do, with a 262144-entry
   table on most of them and 1048576 on some.
3. On 6 hosted runners (run 43715274399), one run from a drained table peaked at 88,857 to 106,587 entries
   and passed every time (12/12, no drops). A second run straight after peaked at 182k to 217k and passed
   (12/12). A third straight after filled the table on 6 of the 10 runs with a 262144 table, and all 6 failed
   mid-scenario; the other 4 (all on AMD 7763) peaked at 249k to 257k and passed.
   With the table lowered to 50000, every run failed (12/12). **Observed.**
4. CI runs `test_migrate` once, after tests that open fewer than 1,000 connections between them (local
   measurement, section 2.3), so today's CI fits with about 2.5× headroom. The hazard is repeating the
   test within about 2 minutes on one host, or a smaller table. **Inferred** from 2 and 3.
5. **Change** (`tests/test_migrate.cpp`, `CMakeLists.txt`): before it starts, `test_migrate` reads the
   table's count and size. If 2,000 entries per scenario do not fit, it refuses with exit 1 and a message
   saying what is in the table and why, instead of stalling mid-scenario. A host without conntrack, or
   one that does not track loopback, never refuses. No product code changed.

Reducing the churn itself means keeping control connections open across requests. That is a protocol change
across the control plane, not attempted here (section 5).

## 1. Classification

| Question | Answer |
|---|---|
| Migration correctness | Not involved. Failures under a full table are dropped SYNs: nodes, witnesses and the test cannot reach each other |
| Request delivery, reconnection | Not a defect: connections are refused by the host's table, not by SNC |
| State transitions | Not involved |
| Test infrastructure | **Yes**: the test's density of control traffic against a host-wide table, with no check that the run fits |
| Product | A design property, not a defect: one TCP connection per control request. It matters wherever loopback or host traffic is tracked (section 5) |

## 2. Evidence

### 2.1 Where the connections go (local, one full run, `strace -f -e connect`)

The trace slows the run (95 s instead of 45 s); counts by destination are exact, the caller column is
attributed from thread ancestry and is approximate.

| Destination | Connects | Share | Main callers |
|---|---|---|---|
| Node admin ports | 30,012 | 42% | the test's probe and `node_info` (about 18k); witness health polls (about 10k); peer health from nodes (about 6k) |
| Witness ports | 21,244 | 30% | witness-to-witness Paxos (about 15k); nodes' placement and lease calls (about 10k) |
| Node client ports | 5,609 | 8% | the test's workload clients and probes; 790 refused while a node was down |
| Node link ports | 1,784 | 3% | cross-node links |
| Other | about 640 | 1% | replication and transient ports |

Every admin and witness connection is a one-shot `ha::ask`. **Observed.**

### 2.2 Hosted (run 43715274399, diag commit 38e46f9)

6 runners (Intel 8573C, Intel 6975P-C ×2, AMD 7763 ×2, AMD 9V74), 2 iterations each:

| Arm | Start entries | Peak | Result |
|---|---|---|---|
| A: from a drained table | 13 to 1,662 | 88,857 to 106,587 | 12/12 pass, no drops |
| B: straight after A | 89k to 107k | 182,141 to 217,284 | 12/12 pass, no drops |
| B2: straight after B | 182k to 217k | full (262,144) in 6 of the 10 runs with that size | those 6 fail (10 to 80 drop messages each); the other 4 at 262144 (AMD 7763, 249k to 257k) and both at 1048576 (317k to 318k) pass |
| C: table max 50000, drained | under 1,600 | 50,000 | 12/12 fail mid-scenario (`migrate::sync` refused, a restarted node's map, `no transfers acknowledged`, a node not up) |
| D: max 50000 and time-wait 10 s | under 700 | 50,000 | 3 of 12 fail; the table still fills, at about 2,000 new connections a second |

Earlier evidence agrees: in run 43670006106 every spread 2-lane failure dropped packets for a full table
(`docs/TEST_MIGRATE_ROOT_CAUSE.md` 3.1), and in the cold-table run 43671956993 no round dropped any.

### 2.3 What runs before it in CI

`ctest` order puts `test_net`, `test_transfer`, `test_replication` and `test_cluster` just before
`test_migrate`. Locally they opened 14, 0, 165 and 646 connections. **Observed.**

## 3. The change

- `conntrack_room(scenarios)` in `tests/test_migrate.cpp`, called once in `main` before any scenario:
  reads `nf_conntrack_count` and `nf_conntrack_max`. If either is missing or unreadable, it allows the
  run. Otherwise it refuses when count + 2,000 × scenarios exceeds max, printing the count, the size, the
  need and what to do. 2,000 per scenario is the hosted maximum (106,587 / 67 ≈ 1,591) with about 25%
  margin.
- `SNC_MIGRATE_CONNTRACK_DIR` replaces `/proc/sys/net/netfilter`, so the check can be tested without root.
- Three ctest entries on one scenario, with fixture tables in `tests/fixtures/conntrack`:
  - `test_migrate_conntrack_refuses`: a table 1,144 short; the run must exit non-zero;
  - `test_migrate_conntrack_says_why`: the same; the message must name the count, size and need;
  - `test_migrate_conntrack_room`: 3,144 free; the scenario runs and passes.

| Case | Result |
|---|---|
| **Before** (main's `test_migrate`, new ctest entries) | `refuses` and `says_why` fail: the scenario runs and passes on a table with no room |
| **After** | all three pass; the full test passes locally (67 scenarios, 2 lanes, 48 s) |
| Hosted | see section 4 |

What it costs: a run that would have squeezed in is refused. In B2 above, 4 runs at 262144 started at
182k to 184k and passed; the check refuses those (it needs 134,000 free). That is the margin.

What it does not do: reduce the churn, wait for the table to drain, or change any timeout or assertion.
A refusal is a failure (exit 1), not a skip.

## 4. Hosted verification

Run 43717077266 (commit 1405a57, the guard in place), the same 6 runner classes as above (AMD 7763 ×2, AMD
9V45, AMD 9V74, Intel 6975P-C ×2), 2 iterations each:

| Arm | Before (43715274399) | After (43717077266) |
|---|---|---|
| A: from a drained table | 12/12 pass | 12/12 pass (peak 88,863 to 106,233); no run refused |
| B: straight after A | 12/12 pass | 12/12 pass (start 89k to 107k, peak 181k to 216k) |
| B2: straight after B, table 262144 | 6 of 10 failed mid-scenario after 20 to 49 s, with 10 to 80 drop messages | 10 of 10 refused in 0 s with the message (start 181,568 to 216,081); no drops |
| B2: table 1048576 | 2/2 pass | 2/2 pass (peak 318k) |
| C: table 50000 | 12/12 failed mid-scenario after 27 to 55 s | 12/12 refused in 0 s; no drops |

No packet was dropped for a full table in any arm after the change. The refusal reads, for example:
`test_migrate: the host's connection table has 182723 of 262144 entries in use; 67 scenarios need about
134000 more. Closed connections stay there for nf_conntrack_tcp_timeout_time_wait (120 s by default): wait
for them to expire, or run fewer scenarios (docs/TEST_MIGRATE_CONNECTION_CHURN.md).`

The check reads the table once, at start. Another process filling the table during the run is not covered.

## 5. Unresolved risks and the larger fix (reported, not started)

- **C1. One connection per control request (product).** A witness polls each node of each group it
  watches every `--poll-ms` (100 ms by default) with a fresh connection, and Paxos rounds between witnesses
  do the same. On a host that tracks connections, each lasts 120 s after closing. **Inferred:** one witness
  watching N groups opens about 20 × N connections a second for polls alone, so about 2,400 × N table
  entries; at the default 262144, polls alone fill it near N = 100 groups per witness host. Persistent
  connections (a request/reply stream per peer, reopened on error) would remove most of this. That touches
  `ha::ask`, `admin_loop.h`, the witness watcher, Paxos and the nodes' control calls, and changes how
  silence and failover are detected, so it needs its own design and review.
- **C2. The test's own probe** opens about a quarter of the connections. It could share connections only
  with C1; slowing it would weaken what it checks, so it is unchanged.
- **C3. The guard's estimate** (2,000 per scenario) comes from 4-vCPU hosted runners at 2 lanes. A faster
  host with more lanes opens connections faster; the estimate is per scenario, so it still scales, but it
  was not measured above 2 lanes.
- **C4.** Other tests that start SNC clusters (Python tests) were not measured; in CI they run after
  `test_migrate` or far enough before it.

## Classification

**REFINE**: `test_migrate` checks that its host can hold the run's connections. **KEEP**: the control
protocol, pending a design decision on C1. **RECONSIDER** (for review, not started): one TCP connection per
control request, at witness scale.

## Evidence index

| Item | Where |
|---|---|
| Diagnostic workflow | `.github/workflows/diag-churn.yml` in commits 38e46f9 and 1405a57, removed afterwards |
| Hosted runs | 43715274399 (measurement), 43717077266 (the guard) |
| Local trace | `strace -f -e trace=connect,execve,clone` of one full run (section 2.1) |
