> **INVALID FOR CRITERIA / INVALID FOR CAMPAIGN EVIDENCE.** Superseded by
> `docs/PILOT_PLAN.md`; see this directory's README.md.

# SNC-only pilot on the main R640: plan

Written before any sweep ran (EXPERIMENTAL_BASELINE 10.2). Run at cody's
direct instruction on 2026-11-06, before the CP-1A review that
CAMPAIGN_PREP section 8 orders. **This plan and its runner need that review
before the results feed `CRITERIA.md`.** No competitor runs here. No value
below is a campaign threshold.

## Fixed settings

- Host: `bench/manifests/r640-main-20261106c.txt`. Allocation:
  docs/R640_TOPOLOGY.md section 4. `kernel.numa_balancing=0`.
- Server: `numactl --cpunodebind=0 --membind=0`, CPUs 2,4,...: one physical
  core per shard. Client: `numactl --cpunodebind=1 --membind=1`, 8 threads,
  CPUs 1,3,...,15. TCP loopback, so every request crosses the socket link.
  **Loopback gives no physical-NIC evidence.**
- Workload A, 5,000,000 accounts, uniform keys, owner routing. Mix: 5%
  INDEX on email; of the rest 10% cross-object transfers; of the rest 70%
  reads, 30% deposits. Default depth 64 per client thread, 8 shards.
- Profile A (LOCAL_DURABLE, `/srv/snc-data`, `--checkpoint-s 60`) and Profile
  C (memory). Each command line passes `bench/profile_check.py`. Profile B
  is not run: this host has no partner. G4 (X4 tagging) is not built.
- Every run: new server, new data directory, load, then the measured window.
  Runs in a phase execute in a fixed-seed shuffled order, so drift is not
  mistaken for an effect.

## Phases

| Phase | Varies | Reps | Seconds (A / C) |
|---|---|---|---|
| duration | 30, 60, 120, 240 s | 1 | as listed |
| fixed | nothing | 5 | 120 / 60 |
| depth sweep | depth 1, 4, 16, 64, 128, 256, 512 | 3 | 90 / 30 |
| shard sweep | shards 1, 2, 4, 8, 16 | 3 | 90 / 30 |

Profile A runs last at least 90 s, so each includes a checkpoint.

## Knee and objective rule (exact)

For each depth in a profile's depth sweep, T = the median over its reps of
total ops/s (mix plus INDEX), and L = the median of its p99 (mix, not INDEX).

1. A depth is **stable** if no rep has errors and its throughput CV is at
   most 10%. Unstable depths are ignored below.
2. **Peak** P = the largest T among stable depths.
3. **Knee** = the smallest stable depth with T >= 0.90 P.
4. **Objective** = L at the smallest stable depth with T >= 0.70 P.
   The fraction 0.70 is PROPOSED here; the criteria review may change it, and
   recomputes from the same data.

CV = sample standard deviation / mean. Noise floor input, from the fixed
phase: 3 x max(CV_throughput, CV_p99). N_MIN is not chosen here.

## Known limits

- **Closed loop.** `net_bench` has no rate control, so the "offered load"
  sweep varies outstanding requests. Latency is subject to coordinated
  omission. An open-loop sweep would need a rate-limited driver.
- p99 values come from a bucketed histogram, so small changes can jump.
- "No filesystem or disk tuning": ext4 defaults. The manifest records BIOS
  and OS power state; frequency is OS-controlled (`schedutil`).
- CPU, write bytes and loopback bytes are counted over the client process's
  whole measured run, including its connect time (well under 0.1 s).
