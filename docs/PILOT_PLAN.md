# SNC-only pilot on the main R640: plan (CP-1A)

*Status: **APPROVED** (PR #61 current-head review,
`docs/handoff/PR61_Final_Review_Handoff.txt`), with corrections C1-C3
applied:
- C1: the backlog denominator is the measured window's scheduled ops;
- C2: total p99 is the primary objective;
- C3: a client limit at the boundary invalidates the sweep.

Written before any data exists for it. Nothing here has run on the R640.
The values are the ones in `bench/pilot_plan.json`, which
`bench/pilot.py` reads and records by SHA-256. They must not change after
the pilot starts. **The real pilot still needs an explicit GO.***

This replaces the R640 draft's plan, which is kept under
`bench/results/INVALID-pilot-r640-20261106-stopped/` and is **invalid for
criteria and campaign evidence** (PR #61 review). No number from that run
was used to choose anything here.

## 1. What the pilot is for

It measures SNC alone on this host, for the criteria review:
- the noise inputs for N (EXPERIMENTAL_BASELINE 10.1): CV of throughput
  and CV of p99;
- the p99 objective, from an open-loop rate sweep with the exact rule in
  section 5 (EXPERIMENTAL_BASELINE 10.2);
- the shard-count scaling and the per-op costs (CAMPAIGN_PREP section 4).

It sets no threshold and runs no competitor.

## 2. Host and allocation

- Host: the R640 as in **manifest d**, accepted
  (`bench/manifests/r640-main-20261106d.txt`, R640_TOPOLOGY 2c). Before
  each evidence session, reapply the governor and `numa_balancing`
  (R640_TOPOLOGY 2c); neither survives a reboot. A real run refuses to start unless every CPU's governor is
  `performance` and `kernel.numa_balancing` is 0 (`pilot.py` checks both).
- **PILOT-L (the baseline allocation):** all on node 0, local to the data
  SSD's HBA and to eno1.
  - CPU 0 (and its sibling 36) is left to the OS and IRQs.
  - Server: 8 shards on physical cores 2, 4, …, 16.
  - Client: 8 `net_bench` threads on physical cores 18, 20, …, 32.
  - Core 34 is spare. Memory is bound to node 0 for both
    (`numactl --physcpubind … --membind=0`). Every SMT sibling (37-71) is
    idle.
- **PILOT-X (named, not the baseline):** server on node 0 (2, 4, …, 34),
  client on node 1 (1, 3, …, 15), each with its own node's memory. Every
  request crosses the socket link. It runs only when asked
  (`--allocation PILOT-X`) and is reported as cross-socket.
- **Core budget.** Node 0 has 18 physical cores; PILOT-L gives 8 to the
  server, 8 to the client and 1 to housekeeping. That caps PILOT-L's
  shard sweep at 8 shards. More shards need either the client off the
  socket (PILOT-X, classified separately) or a second client host.
- TCP loopback, so **no physical-NIC evidence**.

## 3. Workload

- Workload A, 5,000,000 accounts, uniform keys, owner routing.
- **Mix, as exact shares of total operations:** READ 70%, DEPOSIT 15%,
  TRANSFER 10%, INDEX 5% (`net_bench --mix read=70,deposit=15,transfer=10,index=5`).
  One draw per operation picks its kind. INDEX looks up a random loaded
  account's email. *(The draft's nested flags gave read 59.85%, deposit
  25.65%, transfer 9.50%, index 5.00%; `net_bench` now prints the
  effective shares on every run.)*
- Profile A (LOCAL_DURABLE: `/srv/snc-data`, ext4 defaults on the SAS SSD,
  `--checkpoint-s 60`) and Profile C (memory). Every server command line
  passes `bench/profile_check.py`. Profile B needs a second host.
- Every run: a new server, a new data directory, the load, then one
  measured `net_bench` run. The load step is a closed-loop `--seconds 0`
  run (it also runs a handful of operations outside any measured window).

## 4. The load model

**Open loop** (`net_bench --rate R`, `bench/rate_schedule.h`):
- R ops/s in total, divided over the 8 threads exactly (remainder to the
  lowest threads).
- Each thread sends on a fixed periodic schedule with a phase offset of
  i/8 of its period, so the 8 streams interleave. Arrivals are
  deterministic; Poisson arrivals would be a later, named dimension.
- **Latency runs from each operation's intended send time** to its answer.
  If all of a thread's slots (`--depth`, 1024 here) are busy when an op is
  due, the op is owed: it waits, and the wait counts in its latency.
  Scheduled work is never dropped.
- Per run, `net_bench` reports: offered rate; ops scheduled (warm-up and
  window), ops scheduled in the measured window, ops issued by the
  window's end and ops completed in the window; achieved rate; end backlog
  (scheduled but not yet issued at the window's end); ops in flight at the
  end; issue lateness (p50, p99, p99.9, max, count over 1 ms); whether
  everything drained, and what was left if not; errors and refusals; and
  latency for the total and for each kind.
- `tests/test_net_bench_rate.py` stops the server for 400 ms during a run:
  open loop, p99 about 370 ms and max about 400 ms; the same stall in the
  closed loop at depth 1 gives p99 about 65 µs. That is the coordinated
  omission the open loop removes.

**Closed loop** (`--depth D`, no `--rate`) is used only to measure
capacity (throughput), never to set a latency objective.

## 5. The exact algorithm

Per profile, in this order. Values are from `bench/pilot_plan.json`.

| Step | Value (APPROVED) |
|---|---|
| Calibration | 1 closed-loop run, depth 512 per thread, 60 s (A) / 30 s (C). P0 = its total ops/s. P0 is used only to place the grid |
| Grid | R_k = floor(k · P0 / 20), k = 1…22: 5% to 110% of P0 |
| Warm-up | 10 s, ops issued but not measured |
| Window | 60 s (A) / 30 s (C) after the warm-up. Each A sweep run lasts over 60 s from server start, so a checkpoint falls inside it. The duration check's 30 s A run may contain none |
| Reps | 3 per grid point |
| Order | all (rate, rep) cells shuffled with seed 20261106 |
| Drain limit | 10 s after the window |
| Early stop | **none.** Every cell runs. The drain limit bounds each overloaded run |

**Sustained rep.** A rep is sustained if all of these hold:
1. 0 errors (EXEC, INDEX, stale);
2. 0 refusals (E_OVERLOADED on EXEC or INDEX): no dropped work;
3. completed in the window ≥ 99/100 × offered rate × window;
4. end backlog ≤ 1/100 × the ops scheduled in the measured window
   (`scheduled_in_window`; C1). The warm-up's ops are left out, so they
   cannot dilute the 1% threshold. The backlog is all ops owed at the
   window's end, a lagging warm-up op included;
5. drained: every owed op was issued and answered within the drain
   limit. If not, the rep is unsustained and the remaining backlog is
   recorded.

Conditions 3 and 4 are exact integer comparisons.

A **point** is sustained if all 3 of its reps ran and are sustained.

**Stable peak** S = the largest grid rate whose point, and every lower
point, is sustained. If R_1 is not sustained there is no S. If S = R_1
there is no grid rate ≤ f · S. Either way there is no objective, and the
pilot aborts for review.

**Objective.** The objective rate is the largest grid rate ≤ f · S, with
f = 7/10 compared exactly. The objective p99 is the median of the 3 reps'
total p99 at that rate. With 3 reps the median is one measured value.
- f = 0.70 is APPROVED before any data exists. It does not move after
  the pilot starts.
- **The primary objective is the total-operation p99** (every kind
  together; C2). The READ, DEPOSIT, TRANSFER and INDEX p99 and p99.9 are
  secondary diagnostics. A per-kind primary may not be chosen after
  seeing results.

**Knee (reported only).** The smallest rate ≤ S whose median p99 is more
than 2 × the median p99 at R_1. The objective does not use it.

**Ties.** Grid rates are distinct integers. Each step picks the largest or
smallest qualifying rate, and the medians are of 3 values, so no step can
tie. The same data always gives the same answer; `tests/test_pilot.py`
pins each step, including the boundaries.

**Overload and failures.**
- A rep whose `net_bench` output cannot be parsed counts as failed, which
  makes its point unsustained.
- **Client limit (C3).** A point is flagged `client_limited_suspect` when
  all of these hold: it is unsustained, none of its reps had an error or
  a refusal, and its reps' median issue-lateness p99 is over 1 ms
  (strictly). Such a point means the load generator may have fallen
  behind, not SNC.
  - If the **boundary point** (the lowest unsustained point, which sets
    S) is flagged, the sweep is **INVALID**. S, the objective and the knee
    are not set. `invalid` says why, and the pilot aborts for review. A
    client limit is never counted as SNC unsustainability.
  - A flagged point above the boundary keeps its flag as a diagnostic and
    changes nothing.
  - `tests/test_pilot.py` covers both cases, R_1 as the boundary, errors
    (never client-limited), and the 1 ms edge.
- A server that exits during a run, a failed load, or a failed
  calibration aborts the pilot. So does a missing objective or an
  invalid sweep. The abort
  reason is written to `pilot.json`. Nothing after it runs, including
  the remaining profiles.

## 6. The other phases

- **Duration check:** open loop at the largest grid rate ≤ P0/2, windows of
  30/60/120/240 s (A) or 15/30/60/120 s (C), 1 rep each. It checks that
  the window is long enough: p99 and achieved rate should not drift with
  length. A drift sends the window back to review.
- **Fixed reps:** 5 open-loop reps at the objective rate. CV_p99 comes from
  the sustained ones. Memory is measured here (section 7).
- **Capacity reps:** 5 closed-loop runs at depth 512, which give CV_thr
  (throughput) and the median capacity.
- **Noise input:** 3 × max(CV_thr, CV_p99), for N = max(that, N_MIN)
  (EXPERIMENTAL_BASELINE 10.1). N_MIN is not chosen here.
  - CV_thr is taken from the 5 closed-loop capacity reps, because an
    open-loop run's throughput is the offered rate by construction.
    CV_p99 is taken from the 5 fixed open-loop reps at the objective
    rate. Both are APPROVED.
- **Shard sweep:** closed-loop capacity at 1, 2, 4 and 8 shards (PILOT-L's
  budget), 3 reps each, throughput only. Each shard count is reported as
  ops/s per core.

## 7. Metrics

- **Latency:** see section 4. The histogram reports the lower edge of a
  bucket that spans at most 1/32 (3.125%) of it (`bench/bench_common.h`).
  So a reported p99 r means r ≤ p99 < 1.03125 r. No claim is finer than
  that. Max is exact.
- **CPU per op:** the server's utime + stime (all threads, io_uring
  workers included), sampled before and after the whole `net_bench`
  run. It is divided by every op served in that run: EXEC served plus
  INDEX OK, warm-up and drain included (`served_all`). The denominator
  covers the same span as the samples.
- **Write bytes per op:** the server's `/proc/<pid>/io` `write_bytes`
  over the run, divided the same way.
- **Loopback RX and TX bytes per op:** reported separately. They are
  diagnostic only and are never compared with physical-network numbers.
- **Memory** (fixed reps only):
  - empty RSS and PSS after the empty server settles 5 s;
  - steady RSS and PSS 10 s after the load ends;
  - bytes per object = (steady − empty) / objects, for RSS and for PSS;
  - peak (VmHWM) is reported separately, after the run.
- Latency and the sustained test count ops by when they were due, in
  [warm-up end, window end), so the warm-up is excluded. Per-op costs
  instead use `served_all` over the whole run, as above. The only
  unmatched part is connect time: well under 0.1 s, with no ops.

## 8. Run discipline

A real pilot (`pilot.py` without `--quick`) refuses to start unless:
- the git tree is clean;
- `--manifest` is given;
- `bench/pilot_plan.json` is committed;
- the governors are all `performance`;
- `numa_balancing` is 0.

It also refuses unless:
- the manifest's SHA-256 equals `approved_manifest_sha256` in the plan
  (manifest d's, accepted in the PR #61 addendum);
- the build directory is a plain RelWithDebInfo build (no sanitizer
  flags).

Before it checks anything, it rebuilds `snc_server` and `net_bench` from
the tree. With a clean tree, the binaries then match HEAD.

It records the HEAD SHA, the plan's SHA-256 and values, the manifest's
SHA-256, the build type, and the SHA-256 of both binaries.

The runner never writes account-ID files under `--out`. It deletes each
run's data directory after the run. Raw client and server logs of every
run are kept.

The real pilot runs from the reviewed commit. Manifest d is accepted.
The pilot still needs this plan and the dev-VM evidence
(`bench/results/cp1a-devvm/`) to be accepted, and then cody's go.

## 9. Estimated duration

Per profile:
- sweep: 66 runs × (load + 10 s + window + up to 10 s);
- duration check: 4 runs;
- fixed reps: 5 runs;
- capacity reps: 5 runs;
- shard sweep: 12 runs.

That is about 2.5 hours for A and about 1.5 hours for C (inferred from the
run lengths, not measured).
