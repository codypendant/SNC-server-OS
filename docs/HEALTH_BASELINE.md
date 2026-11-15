# HEALTH and failover baseline (before K0)

Status: **approved by cody (PR #132 review, transcribed in
[handoff/PR132_Health_Baseline_Review_Approval.md](handoff/PR132_Health_Baseline_Review_Approval.md)).** This is the "Before K0: HEALTH baseline" step of
[CONTROL_CONNECTIONS_DESIGN.md](CONTROL_CONNECTIONS_DESIGN.md) section 6. It
measures failure detection and promotion on main as it is, before any kept
connection exists. K0, K1, K2 and K3 are not started. The figures below are
what each later slice must reproduce.

## What this PR adds

The PR changes no request handling. One-shot `ha::ask`, HEALTH, the witness
loop and promotion all behave as on main. The additions are:

- **Per-request trace line in `ha::ask` (A2(a)).** With `SNC_TRACE=1`, each
  control request writes a line naming the caller path, the verb, the port,
  `conn=oneshot` and the outcome. An example is
  `caller=watch/propose verb=PREPARE ha/0`. A thread-local `ha::Caller` scope
  builds the path. Paxos broadcast threads and promotion adopt their parent's
  path, so a READ inside `propose` is counted apart from the PLACEMENT GET
  handler's READ.
- **`DEBUG ACCEPT-HOLD <ms>`** (needs `--debug-admin`). The admin loop stops
  accepting for `<ms>`. Connections it accepted earlier are still answered,
  and `0` ends the hold. This simulates a stalled accept loop without
  stopping the process.
- **`adm_accept_fail` in HEALTH.** It counts accept failures on EMFILE,
  ENFILE, ENOBUFS and ENOMEM. It is appended at the end of the line, per the
  test_gauges_admin T8a contract.
- **`tests/test_admin_loop.cpp` H1 and H2 (A6, in-process loop).**
  - H1: the heavy worker is held, its queue is full, and 200 half-read
    connections are open. HEALTH is still answered inline, worst case under
    500 ms over 20 probes, and the next heavy request gets `NO busy`.
  - H2: the accept hold. A connection accepted before the hold is answered
    during it. A new one is answered only after the hold, and `0` ends a
    60 s hold early.
- **`tests/test_health_baseline.py` (A2(a), A2(b)).** It runs a clustered
  harness: three witnesses (failover 1000 ms, poll 100 ms), one replication
  group with placement and leases (`--control`), and `SNC_TRACE=1`. Each
  process writes its trace to its own file. It runs on the slow label in
  ctest.

## Method

- **Fault time** is `time.monotonic_ns()` in the test, taken just before
  injection. The trace uses `CLOCK_MONOTONIC`, the same clock.
- **Detection** is the first `witness_suspect` line from any witness minus
  the fault time.
- **Promotion** is the moment the standby first reports `role=p` in HEALTH,
  minus the fault time.

The fault scenarios are:

| Scenario | Fault |
|---|---|
| kill | SIGKILL the primary |
| stop | SIGSTOP the primary, then SIGCONT after promotion; the old primary's role is recorded |
| partition | the witnesses reach the primary's admin port through test_health's `Proxy`, which is then cut |
| hold | `DEBUG ACCEPT-HOLD` for failover_ms + 10 s |
| hold_short | a 400 ms hold, which must cause no promotion and no HA state change |
| emfile | the primary's soft RLIMIT_NOFILE is set to its lowest free descriptor, then restored; HEALTH must show `adm_accept_fail > 0` |

Raw results: `bench/results/health-baseline-20261115/` (JSON and log).
Run: `SNC_BASELINE_TRIALS=10 SNC_BASELINE_OUT=<file>.json python3
tests/test_health_baseline.py <build>`. ctest uses 2 trials.

## Results (10 trials each, 4 vCPU cloud container, kernel 6.18, not sanitized)

Times are in ms from the fault: min / median / max.

| Scenario | Detection | Promotion | Outcome |
|---|---|---|---|
| kill | 1055 / 1069 / 1084 | 1083 / 1111 / 1120 | promoted, every trial |
| stop | 1238 / 1255 / 1276 | 1266 / 1286 / 1308 | promoted; after SIGCONT the old primary reports `role=p term=0` |
| partition | 1087 / 1253 / 1272 | 1125 / 1287 / 1308 | promoted, every trial |
| hold | 1245 / 1268 / 1271 | 1283 / 1308 / 1316 | promoted, every trial |
| emfile | 1248 / 1257 / 1273 | 1284 / 1291 / 1333 | promoted; `adm_accept_fail` 120 to 122, `adm_evicted` 0 |
| emfile, corrected harness (K0 PR) | 1028 / 1046 / 1056 | 1075 / 1090 / 1106 | see the correction below |
| hold_short | none | none | no promotion; HA state unchanged; HEALTH answered after the hold |

- The full suite passed on this build (56 of 56, ctest `-j1`).
- **A2(a) one-shot check.** The kill run's trace has no unlabeled request.
  Every request kind on the HA path (design 4.5) appears and is one-shot:
  - witness `watch` HEALTH, STATUS and PROMOTE;
  - `watch/propose` READ, PREPARE, ACCEPT and COMMIT on `ha/0`;
  - `serve_LEASES_TAKE/propose`;
  - `serve_PLACEMENT_SYNC/read_linear`;
  - the server's `ask_witness` READY and NOTREADY;
  - `promote/placement_sync` and `promote/lease_blocks`.

  The test prints the request table, and the harness asserts this coverage.
- **Kill is faster** than stop, hold and emfile because a refused connect
  fails at once. A silent primary costs the witness a full poll timeout on
  every iteration.
- **The partition proxy usually black-holes.** One trial in ten (1087 ms)
  timed like kill, because the cut reset a connection.

## Findings and decisions

1. **The design's detection bound was too tight. Decided: split, not loosened.**
   A2(b) said suspect within failover_ms + 2 × poll_ms, which is 1200 ms here.
   Main measures up to 1276 ms, so main itself would fail that bound.
   - **Cause (`tools/snc_witness.cpp` `watch`).** `primary_ok` is set at the
     top of the last answered iteration. Suspicion is checked only at the end
     of an iteration. A silent iteration costs a poll_ms timeout, plus a
     poll_ms sleep, plus the witness's per-iteration state read.
   - **cody's decision:** accept the measured baseline, but do not replace
     the detection contract with a loose timeout. The design's new section
     5.1 separates three terms: the measured behavior (this record), the
     regression tolerance (same outcomes, each max within one poll_ms of
     main's), and the intended guarantee (failover_ms + one silent
     iteration, including the state read).
   - **The test's bound is only a backstop:** failover_ms + 3 × poll_ms +
     500 ms for detection, failover_ms + 8000 ms for promotion, skipped
     under sanitizers. It catches gross breakage on any host.
2. **A second silent path in the admin loop.** If RLIMIT_NOFILE is below the
   poll set's size, `poll()` returns EINVAL. The loop then answers nothing,
   and `adm_accept_fail` stays 0. It was found while setting the soft limit
   to 0. The EMFILE scenario therefore sets the limit to the lowest free
   descriptor. Recorded, not fixed (outside this slice).
3. **Runtime trace instead of a test-build counter. Decided: accepted,
   provided the overhead stays measured and controlled.** See "Trace
   overhead" below.
4. **`conn=oneshot` is a literal today.** `check_oneshot` is a tripwire that
   becomes meaningful once K0 adds a kept path that writes `conn=kept`.
5. **A stopped primary still claims primary after SIGCONT. Tracked as open
   item O1 below.** It reports `role=p` at its old term. cody's review: a
   potentially important correctness concern, to be investigated, not
   dismissed as a monitoring issue.
6. **Possible flake.** With the soft limit at the lowest free descriptor, a
   descriptor closing below it leaves one usable slot. Accept then succeeds
   once. The test asserts `adm_accept_fail > 0`, not a count.

**Correction (K0 PR, for cody's review).** Finding 6 was real and larger
than recorded. The harness read the lowest free descriptor once; when a
transient descriptor was open at that instant, the limit sat above a hole
that stayed usable, so the primary stayed partly reachable and detection
waited for a later silent iteration. The fixed harness takes the minimum of
100 samples. Rerun on this same commit (28733cf), same host, 10 trials, EMFILE
detection is 1028 / 1046 / 1056 ms and promotion 1075 / 1090 / 1106 ms
(`bench/results/health-baseline-20261115/emfile_fixed_harness.json`). The row
above with 1248 to 1333 ms is kept as recorded; K0 and later slices are judged
against the corrected row. Details: [K0_TEST_PROBE.md](K0_TEST_PROBE.md).

## Trace overhead (measured and controlled)

**Control.** With `SNC_TRACE` unset (the default), an `ha::Caller` or
`Caller::Adopt` scope does one load and branch on `trace::enabled()` and
nothing else: no string append, no copy. The witness's per-request label
(`"serve " + verb`) is built only when tracing is on. `test_ha_caller` runs
twice in ctest, once with tracing off, where the path must stay empty, and
once on, where the paths must nest, unwind and be adopted correctly.

**Measurement.** `bench/results/health-baseline-20261115/ha_trace_overhead.cpp`, built against `tools/ha.h` with
`-O2`, on the same 4 vCPU container, 3 runs each:

| | Tracing off | Tracing on (stderr to /dev/null) |
|---|---|---|
| one `ha::Caller` scope | 0.6 to 0.9 ns | 2.9 to 4.9 ns |
| one loopback `ha::ask` (connect, send, reply, close), 20,000 asks | 54 to 58 µs | 52 to 58 µs |

A scope costs under 1 ns with tracing off. That is under 0.002% of the
cheapest control request, which is a TCP connection of about 55 µs. With
tracing on, the per-request trace line is within run-to-run noise.

## Open items

- **O1. A resumed old primary reports `role=p` at its previous term.**
  - **Seen:** in all 10 stop trials, after SIGSTOP, promotion of the
    standby, and SIGCONT. The old primary's HEALTH said `role=p term=0`
    while the group's decided term was 1.
  - **Not yet known:**
    - whether it serves reads or accepts writes in that state, and for how
      long;
    - whether the existing withdrawal paths (DEGRADE answered "term
      replaced", `sender_->fenced()`; EVIDENCE.md, the cut experiment) apply
      when no write traffic reaches it, as in this test.
  - **Owner:** to be investigated as its own slice, with cody's
    authorization. This baseline only records it.

## Coverage of A2 and A6

| Criterion | This PR | Notes |
|---|---|---|
| A2(a) per-verb, per-caller one-shot count | yes | the trace and the request table; `HA_KINDS` coverage assertion |
| A2(b) SIGKILL, SIGSTOP, partition, stalled accept, EMFILE | yes | clustered, 10 trials recorded; ctest runs 2 |
| A2(b) EMFILE "with the idle set populated" | N/A | there is no idle set before K0; K0 reruns it |
| A2(c) no false promotion with idle, evicted or reset kept connections | N/A | needs kept connections (K0, K1). hold_short is the present-day form: a short stall does not promote |
| A2(d) near the descriptor limit with the idle set full | not covered | needs the idle set (K1) |
| A6 heavy worker stalled, queue full, HEALTH inline | yes, in-process | H1; the real server is covered by test_health T11 |
| A6 a new witness polling an old node | N/A | no version difference exists until K0 or K1 |
| Lone-pair partition (no placement) | existing | test_health T6 |

## Classification

**KEEP.** Measurement and test hooks only. Request handling, HEALTH,
failover timing and fencing are unchanged.
