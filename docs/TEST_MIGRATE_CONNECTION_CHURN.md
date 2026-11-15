# test_migrate connection churn (R4): measurement and guard (2026-11-15)

Assignment: cody, 2026-11-15 12:31Z ("Yes" to the connection-churn slice recommended in the PR #128 review,
`docs/handoff/PR128_Test_Migrate_Review_Approval.md`). Background: risk R4 in `docs/TEST_MIGRATE_ROOT_CAUSE.md`.

Labels: **Observed** (read from a log, counter or source), **Inferred** (follows from observations, not shown
directly), **Open** (not explained).

## Summary

1. Every control request in SNC opens its own TCP connection and closes it after one reply. That covers
   witness health polls, witness-to-witness Paxos, a node's placement and lease calls to the witnesses, peer
   health checks, and this test's probe. **Observed** (`tools/ha.h` `ha::ask`; `tools/admin_loop.h` and the
   witness's `serve()` close after the reply).
2. One full `test_migrate` run takes 42 to 53 s on hosted runners and adds 88k to 111k entries to the host's
   conntrack table (section 2.2). Locally, a traced run made about 59k IPv4 connects to the test's ports, and
   untraced runs counted 71k to 183k TCP active opens (a counter that includes refused attempts). A host that
   tracks loopback keeps each closed connection's entry for `nf_conntrack_tcp_timeout_time_wait` (120 s).
   Hosted runners do this, with a 262144-entry table on most of them and 1048576 on some.
3. On 6 hosted runners (run 43715274399), runs were made back to back:
   - one run from a drained table peaked at 88,857 to 106,587 entries and passed every time (12/12, no
     drops);
   - a second run straight after peaked at 182k to 217k and passed (12/12);
   - a third run straight after filled the table in 6 of the 10 runs at 262144, and all 6 failed
     mid-scenario. The other 4, all on AMD 7763, peaked at 249k to 257k and passed;
   - with the table lowered to 50000, every run failed (12/12).

   **Observed.**
4. CI runs `test_migrate` once. The tests just before it open fewer than 1,000 connections between them
   (local measurement, section 2.3), so today's CI fits with about 2.5× headroom. The hazard is repeating the
   test within about 2 minutes on one host, or running it on a smaller table. **Inferred** from 2 and 3.
5. **Change** (`tests/test_migrate.cpp`, `CMakeLists.txt`):
   - Before it starts, `test_migrate` reads the table's count and size. If 2,000 entries per scenario do not
     fit, it refuses with exit 1 and a message saying what is in the table and why. Before this, the run
     stalled mid-scenario with a failure that looked like a product fault.
   - It fixes no CI failure and reduces no churn. It turns a misleading mid-scenario failure into an
     immediate, named one.
   - No product code changed.

Reducing the churn itself means keeping control connections open across requests. That is a protocol change
across the control plane, not attempted here (section 5).

## 1. Classification

| Question | Answer |
|---|---|
| Migration correctness | Not involved. Failures under a full table come from dropped SYNs: nodes, witnesses and the test cannot reach each other |
| Request delivery, reconnection | Not an SNC defect: the host's table drops the connections, not SNC |
| State transitions | Not involved |
| Test infrastructure | **Yes.** The test's density of control traffic runs against a host-wide table, with no check that the run fits |
| Product | A design property, not a defect: one TCP connection per control request. It matters wherever loopback or host traffic is tracked (section 5) |

## 2. Evidence

### 2.1 Where the connections go (local, `strace -f -e connect`)

**Correction (2026-11-15, `docs/CONTROL_CONNECTIONS_DESIGN.md` 2.2 and 2.3):** an untraced count of every
`ha::ask` shows the test's `MOVE INFO` probe makes 81% of a run's control connections, not about a quarter.
The trace below throttled the probe's tight loop far more than the paced callers, so its caller attribution
is skewed; its destination counts describe the traced run only. In particular, the caller rows
"witness → node admin (health polls)" and "node → node admin (peer health)" have no real caller in this test:
its witnesses run without `--pair`, so they send no HEALTH polls.

The trace slows a run to about 95 s, from 45 s. One traced run made 59,289 IPv4 connects to ports of the
test's two lanes, classified exactly by destination below. The rest of its 71,447 connects went to other
addresses or families and were not classified.

| Destination | Connects | Share of classified |
|---|---|---|
| Node admin ports | 30,012 | 51% |
| Witness ports | 21,244 | 36% |
| Node client ports (790 refused while a node was down) | 5,609 | 9% |
| Node link ports | 1,784 | 3% |
| Replication and other ports in the range | 640 | 1% |

A second traced run (70,328 connects) attributed each connect to its process by thread ancestry. That
attribution is approximate.

| Caller → destination | Connects |
|---|---|
| test (probe, `node_info`) → node admin | 18,063 |
| witness → witness (Paxos) | 14,699 |
| witness → node admin (health polls) | 10,130 |
| node → witness (placement, leases) | 9,910 |
| node → node admin (peer health) | 6,443 |
| test → node client | 2,835 |
| other pairs | 8,248 |

Every admin and witness connection is a one-shot `ha::ask`. **Observed.**

### 2.2 Hosted (run 43715274399, diag commit 38e46f9)

The runners were Intel 8573C, Intel 6975P-C ×2 (table 1048576), AMD 7763 ×2 and AMD 9V74, with 2 iterations
each. The script set the table back to 262144 after iteration 1, so the two 6975P-C runners ran iteration 2
at 262144 rather than their native 1048576.

| Arm | Start entries | Peak | Result |
|---|---|---|---|
| A: from a drained table | 16 to 1,662 | 88,857 to 106,587 | 12/12 pass, no drops |
| B: straight after A | 89k to 107k | 182,141 to 217,284 | 12/12 pass, no drops |
| B2: straight after B | 182k to 217k | 262,144 in 6 of the 10 runs at that size (2 of the 10 on lowered 6975P-C tables) | Those 6 fail after 20 to 49 s, with 10 to 80 drop messages each. The other 4 at 262144 (AMD 7763, 249k to 257k) and both at 1048576 (317k to 318k) pass |
| C: max 50000, drained | under 1,600 | 50,000 | 12/12 fail mid-scenario after 27 to 55 s: `migrate::sync` refused, a restarted node's map, `no transfers acknowledged`, a node not up |
| D: max 50000, time-wait 10 s | under 700 | 50,000 | 3 of 12 fail. All 12 logged drops (10 to 63), the 9 that passed included |

**Open:** arm D. At about 2,000 new connections a second and a 10 s time-wait, about 20k entries would sit
in TIME_WAIT, yet the 50000 table still filled. Entries must be held in other states (SYN_SENT, refused
attempts, FIN_WAIT, CLOSE_WAIT), or the rate is much burstier than the average. No per-state count was
taken. So TIME_WAIT alone does not explain the table's use, and a fix that only shortens it (section 5)
is not shown to be enough.

Earlier evidence agrees. In run 43670006106, every spread 2-lane failure dropped packets for a full table
(`docs/TEST_MIGRATE_ROOT_CAUSE.md` 3.1). In the cold-table run 43671956993, no round dropped any.

### 2.3 What runs before it in CI

`ctest` order puts `test_net`, `test_transfer`, `test_replication` and `test_cluster` just before
`test_migrate`. Locally they opened 14, 0, 165 and 646 connections. **Observed.**

## 3. The change

- **`conntrack_room(scenarios)`** in `tests/test_migrate.cpp` is called once in `main`, before any
  scenario.
  - It reads `nf_conntrack_count` and `nf_conntrack_max`.
  - If either is missing or unreadable, or max is 0, it allows the run.
  - Otherwise it refuses when count + 2,000 × scenarios exceeds max (compared without overflow). It prints
    the count, the size, the need and what to do.
  - It reads only those two numbers, whatever traffic fills the table. A busy or small table therefore
    refuses even on a host that does not track loopback.
  - 2,000 per scenario is the hosted maximum with about 25% margin: 106,587 / 67 ≈ 1,591 by peak, or
    1,575 by peak minus start.
- **`SNC_MIGRATE_CONNTRACK_DIR`** replaces `/proc/sys/net/netfilter`, so the check can be tested without
  root.
- **Four ctest entries**, each on one scenario, with fixture tables in `tests/fixtures/conntrack`:
  - `test_migrate_conntrack_refuses`: 1,144 entries free, 856 short of the 2,000 needed. The run must exit
    non-zero.
  - `test_migrate_conntrack_says_why`: the same table. The message must name the count, size and need.
  - `test_migrate_conntrack_room`: 3,144 free. The scenario must run and print `1 scenarios ... ok`.
  - `test_migrate_conntrack_absent`: no conntrack files. Same requirement as `room`.

| Case | Result |
|---|---|
| **Before** (main's `test_migrate`, new ctest entries) | `refuses` and `says_why` fail: the scenario runs and passes on a table with no room |
| **After** | All four pass. The full test passes locally (67 scenarios, 2 lanes, 48 s) |
| Hosted | See section 4 |

**What it costs:** some runs that would have squeezed in are refused. In B2 above, 4 runs at 262144 started
at 182k to 184k and passed. The check refuses those, because it needs 134,000 free.

**What it does not do:**
- reduce the churn;
- wait for the table to drain;
- watch the table during the run;
- change any timeout or assertion.

A refusal is a failure (exit 1), not a skip.

## 4. Hosted verification

Run 43717077266 checked out commit 1405a57, with the check in place. Its runners were AMD 7763 ×2, AMD 9V45,
AMD 9V74 and Intel 6975P-C ×2: five of the classes above, with a 9V45 in place of the 8573C. As before, each
ran 2 iterations, and the 6975P-C runners ran iteration 2 at 262144.

| Arm | Before (43715274399) | After (43717077266) |
|---|---|---|
| A: from a drained table | 12/12 pass | 12/12 pass (peak 88,863 to 106,233); none refused |
| B: straight after A | 12/12 pass | 12/12 pass (start 89,108 to 106,649, peak 181,194 to 215,799) |
| B2: straight after B, table 262144 | 6 of 10 failed mid-scenario after 20 to 49 s, with 10 to 80 drop messages | 10 of 10 refused in 0 s with the message (start 181,568 to 216,081); no drops |
| B2: table 1048576 | 2/2 pass | 2/2 pass (peak 317,780 and 318,695) |
| C: table 50000 | 12/12 failed mid-scenario after 27 to 55 s | 12/12 refused in 0 s; no drops |

No packet was dropped for a full table in any arm after the change. A typical refusal:

```
test_migrate: the host's connection table has 182723 of 262144 entries in use; 67 scenarios need about
134000 more. Closed connections stay there for nf_conntrack_tcp_timeout_time_wait (120 s by default): wait
for them to expire, or run fewer scenarios (docs/TEST_MIGRATE_CONNECTION_CHURN.md).
```

## 5. Unresolved risks, alternatives, and the larger fix (reported, not started)

- **C1. One connection per control request (product).** Inferred from the code, not measured:
  - Steady state, a witness asks the current primary of each group it watches once every `--poll-ms`
    (100 ms by default). It asks the standby only after `failover_ms` of silence.
  - For N groups that is about 10 × N connections a second for polls, and about 1,200 × N entries held
    120 s.
  - Polls alone fill a 262144 table near N ≈ 218 groups per witness host.
  - Paxos rounds between witnesses, and the nodes' placement and lease calls, add to the same host's
    table. The total was not measured outside this test.
  - In production the nodes are remote. A stateful firewall tracks that traffic far more commonly than
    anything tracks loopback.
  - Persistent connections, a request/reply stream per peer reopened on error, would remove most of this.
    That touches `ha::ask`, `admin_loop.h`, the witness's `serve()` and watcher, Paxos, and the nodes'
    control calls. It changes how silence and failover are detected, so it needs its own design and review.
    (2026-11-15: the design, `docs/CONTROL_CONNECTIONS_DESIGN.md`, keeps HEALTH, every Paxos write and the
    rest of the HA path one-shot after review, so it does not reduce witness health polls.)
- **Alternatives considered for this slice:**
  - **Closing with RST** (`SO_LINGER {1, 0}` in `ha::ask` after the reply is read). The entry would expire
    in about 10 s instead of 120 s. Not done here: it is a product wire change, and arm D shows a short
    time-wait alone did not keep a 50000 table from filling (Open, section 2.2).
  - **Pacing the test's probe.** `Probe::run` asks in a tight loop and makes most of the connections
    (81% untraced; see the correction in 2.1). Not done here: it is the test's only continuous check for two serving owners, and how
    much detection a slower probe would lose was not measured. Its connections could only be shared
    through C1.
- **C2. The estimate is time-paced, not scenario-paced.** Much of the traffic runs on clocks: witness polls,
  placement polls, `node_info` loops and the unpaced probe. Entries per scenario therefore grow with each
  scenario's wall time. 2,000 per scenario was measured only on 4-vCPU hosted runners, Release builds,
  2 lanes and 42 to 53 s runs. On a slower host or build (sanitizers, loaded CPUs), or with more lanes, a run
  that passes the check may still fill the table. The peak is bounded by about rate × 120 s, not by the
  scenario count.
- **C3. A check, not a cap.** The table is read once, at start. Other processes, or a slow run, can still
  fill it during the run.
- **C4. Not counted:** other tools that call `ha::ask` in a loop (CLI and lab tools), and the Python tests
  that start SNC clusters. In CI those run after `test_migrate`.
- **C5. Outside this slice:** a filter that matches no scenario still prints `0 scenarios ... ok`, as on main.

## Classification

**REFINE**: `test_migrate` checks that its host can hold the run's connections.

**KEEP**: the control protocol, pending a design decision on C1.

**RECONSIDER** (for review, not started): one TCP connection per control request, at witness scale.

## 6. Independent QA

Three reviewers had no brief beyond their area: the code and tests, every claim against the hosted logs, and
scope and guarantees. None found a blocking defect. Each finding was re-checked against the source or the
logs before it was applied.

| Finding | Applied |
|---|---|
| C1 assumed a witness polls both nodes; it polls only the primary (`snc_witness.cpp` `watch()`) | C1 recomputed: about 10 × N a second, near N ≈ 218 for polls alone |
| "70k to 180k connections per run" was unsupported | Summary 2 states what each measurement counts |
| The 2.1 shares were of the wrong total; caller figures came from another trace | Shares of the 59,289 classified; caller table from its own trace |
| "A host that does not track loopback never refuses" was false as coded | Corrected in the summary, section 3 and the code comment |
| The "per scenario, so it scales" estimate ignored time-paced traffic | Restated as C2 |
| Arm D was not explained | Marked Open, with what it implies for a time-wait fix |
| The fixture is 1,144 free, not 1,144 short | Corrected in the doc and the CMake comment |
| B2's 262144 runs included two lowered 6975P-C tables; the after run's runner classes differed | Stated in 2.2 and 4 |
| `room` would pass vacuously if its scenario were renamed; the no-conntrack path had no test | `room` must print `1 scenarios ... ok`; new `absent` test |
| `count + need` could overflow on a corrupt count | Compared as `count <= max && need <= max - count` |
| Alternatives (RST close, probe pacing) were not on the record | Added to section 5 |
| Minor: arm A start, margin basis, run duration | Corrected |

Not changed: a crash would also satisfy `refuses` (WILL_FAIL accepts any non-zero exit); `says_why` covers the
message.

## Evidence index

| Item | Where |
|---|---|
| Diagnostic workflow | `.github/workflows/diag-churn.yml` in commits 38e46f9 and 1405a57, removed afterwards |
| Hosted runs | 43715274399 (measurement), 43717077266 (the check) |
| Local traces | `strace -f -e trace=connect,execve[,clone]` of two full runs (section 2.1) |
