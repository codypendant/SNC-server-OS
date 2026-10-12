# Hot keys: Zipf workloads

The uniform-key matrix ([PREFETCH.md](PREFETCH.md)) spreads ops evenly over
1M objects, so the baseline's 4,096 lock stripes almost never contend. Real
workloads have hot keys, and they are the case shard-per-core is meant to
handle: SNC runs every op on an object from one core with no lock, while the
baseline's threads queue on the hot stripe's mutex and pass its cache lines
between cores.

## Method

`--zipf THETA` on `snc_bench`, `baseline_bench` and `net_bench` draws each
target's rank with probability proportional to 1/rank^θ (YCSB's generator,
`bench/zipf.h`). Ranks map to objects through one list shuffled with a fixed
seed, so all drivers agree on the hot set and hot objects land on owners in
proportion to how many each owns. In process, `--remote` no longer applies:
a driver's target is local only when its shard happens to own it (about 25%
with 4 shards). Over TCP, the SNC client sends each request to the owner.

The hottest object gets 6.5% of ops at θ 0.99, 19% at θ 1.2 and 38% at
θ 1.5. YCSB's default is 0.99.

Same VM as before (4 vCPUs), 1M objects, 3 runs, medians;
`bench/results/matrix_skew.csv` and `net_matrix_skew.csv`. Run with
`bench/run_matrix.py --groups skew,skew-durable` and
`bench/run_net_matrix.py --groups skew,skew-durable`.

## Results

These runs predate the prefetch fix in
[PREFETCH.md](PREFETCH.md#fixing-sncs-object-prefetch); after it, SNC over
TCP also leads at θ 0.99 (2.68M vs 2.53M) and by 1.35× at θ 1.5
(`net_matrix_lean.csv`). In process, θ 0.99 is still a baseline win
(8.11M vs 12.63M), because three quarters of SNC's ops cross cores.

In process, in memory (4 shards or threads, depth 32):

| θ | Reads | SNC | Baseline | Baseline stripe contention |
| --- | --- | --- | --- | --- |
| 0.99 | 50% | 7.64M | **12.85M** | 0.8% |
| 0.99 | 0% | 7.49M | **11.39M** | 0.8% |
| 1.2 | 50% | 8.56M | **10.39M** | 5.3% |
| 1.2 | 0% | 8.46M | **9.74M** | 5.8% |
| 1.5 | 50% | **8.04M** | 6.95M | 14.0% |
| 1.5 | 0% | **8.13M** | 6.30M | 14.2% |

For comparison, uniform keys gave SNC 10.25M (all local) and 5.28M (all
remote), and the baseline 13.95M. In-process latencies are not comparable:
the baseline runs each op inline, while SNC's include queueing behind 32
outstanding ops per shard.

In process, durable (depth 256, baseline with 4 logs):

| θ | SNC | Baseline |
| --- | --- | --- |
| 0.99 | **1.61M**, p99 2.0 ms | 1.11M, p99 2.6 ms |
| 1.5 | **1.25M**, p99 2.6 ms | 0.84M, p99 2.9 ms |

Over TCP (2 server cores, 2 client cores; SNC owner-routed):

| Workload | SNC | Baseline |
| --- | --- | --- |
| memory, θ 0.99, 50% reads | 2.36M, p99 102 µs | **2.50M**, p99 92 µs |
| memory, θ 0.99, writes only | 2.31M | **2.36M** |
| memory, θ 1.5, 50% reads | **3.09M**, p99 86 µs | 2.58M, p99 96 µs |
| memory, θ 1.5, writes only | **3.19M**, p99 82 µs | 2.56M, p99 96 µs |
| durable, θ 1.2, 50% reads | **0.97M**, p99 1.38 ms | 0.75M (2 logs), p99 2.56 ms |

## What this says

1. **Skew is where the curves cross.** As θ rises the baseline loses 46% of
   its in-memory throughput (12.85M to 6.95M) as stripe contention climbs
   to 14%. SNC holds 7.5-8.6M at every θ: hot objects stay in the owning
   core's cache and nothing waits on a lock. SNC passes the baseline
   between θ 1.2 and 1.5 in process, and by θ 1.5 over TCP (1.20-1.25×).
2. **At YCSB's default (0.99) the baseline still wins in memory**, by 1.68×
   in process and 1.06× over TCP. Moderate skew does not make its stripes
   contend enough to offset SNC's longer per-op path.
3. **Durable, SNC leads at every skew**: 1.45-1.49× in process and 1.29×
   over TCP, with lower p99. Skew costs both engines throughput here
   because hot objects are rewritten often, so their WAL records queue
   behind each other's flushes.
4. **SNC's load does not spread with the hot set.** At θ 1.5 the owner of
   the hottest object handles over a third of all ops, yet SNC does not
   slow down: one core serving a hot object from cache with no lock is
   faster than four cores fighting over it. A wider machine would expose
   that one core as the ceiling; shard rebalancing (v0.1 non-goal) or
   splitting hot objects would be the answer there.

## Limits

- 4 vCPUs. Lock contention and cache-line transfers cost more across
  sockets and with more cores, so the baseline's losses here are likely an
  underestimate; SNC's single-owner ceiling would also show sooner.
- The baseline's mutexes are `std::mutex` (futex, no spin tuning).
- One op type (deposit/get balance). Ops that touch several objects are
  not tested.
