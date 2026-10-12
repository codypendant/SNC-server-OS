# Finding objects faster: hash-tagged map and batch prefetch

After routing ([ROUTING.md](ROUTING.md)), a profile of `snc_server` under
owner-routed load put 38% of server CPU in `Shard::run_exec`, nearly all of
it on two instructions: the load of a local map entry and the compare
against the ID stored in the object it points to. Both are cache misses
with 1M objects (about 500 MB) touched at random. Socket syscalls were a
smaller share than expected (the server reads about 16 requests per
`recv`).

## Changes

**Hash-tagged entries.** A local map entry was one pointer, so linear
probing dereferenced every object it passed to compare IDs: a miss per
probe. Entries now hold the full 64-bit hash next to the pointer, and an
object is read only when the hashes match. Erase and growth reuse the
stored hash. Cost: 8 more bytes per table slot, measured at +17 B per
object of resident memory (483 to 499 B).

**Batch prefetch.** `snc::prefetch(ids, n)` warms the map entries of every
ID this shard owns, then (once the whole batch has been issued) the
objects those entries point to, so a batch pays its misses in parallel
instead of one op at a time. It is called from:

- the socket front end, with the targets of every whole frame in a `recv`
  buffer before any is dispatched (`ConnSet::set_on_batch`);
- the runtime's ring drain, with the targets of each batch of requests
  from another shard (`ShardHooks::on_batch`, `Engine::Config::prefetch`);
- the in-process benchmark driver, which now picks a batch's targets
  before issuing them (`snc_bench --no-prefetch` turns that and the ring
  prefetch off).

Shard-local ownership is what makes this cheap: the shard can look ahead
across a batch without taking any lock, because nothing else touches its
map.

## Results

Same VM and method as ROUTING.md, 3 runs, medians. "Before" is
`bench/results/matrix_rerun.csv` and `net_matrix.csv` (v0.1 map, baseline
with `std::unordered_map`); "SNC after, old baseline" is
`matrix_prefetch.csv` and `net_matrix_prefetch.csv`. A single-run check
separated the two SNC changes (`--remote 0 --reads 50`): the tagged map
alone took 5.0M to 7.6M ops/s, prefetch took 7.6M to 9.3M, and at 100%
remote, ring prefetch took 4.4M to 5.4M.

## The baseline gets the same map and prefetch

Part of that gain was the map, not the architecture, so the baseline now
gets the same treatment: each lock stripe holds a flat open-addressing
table with the hash in every entry (`FlatMap` in `bench/baseline_core.h`),
and both `baseline_bench` and `baseline_server` prefetch each batch's
stripes, entries and objects before running it (`--no-prefetch` turns it
off). SNC also lost two per-op costs found while profiling: it now hashes a
target once per op instead of twice, and ops declared `keeps_index_key`
(deposit, withdraw) skip index-key extraction, which debug builds verify.
All runs below are `bench/results/matrix_tuned.csv` and
`net_matrix_tuned.csv`.

In process (4 shards or threads, depth 32):

| Flags | SNC | Tuned baseline | Old baseline |
| --- | --- | --- | --- |
| memory `--remote 0 --reads 50` | 10.25M | **13.95M** | 4.26M |
| memory `--remote 0 --reads 95` | 10.84M | **18.45M** | 4.19M |
| memory `--remote 100 --reads 50` | 5.28M | **12.35M** | 3.64M |
| scaling, 1 shard or thread | 2.27M | **3.29M** | 0.92M |
| durable `--remote 0 --reads 50` | **1.85M**, p99 1.54 ms | 1.30M (4 logs), p99 2.29 ms | 1.18M |
| durable `--remote 100 --reads 50` | **1.77M**, p99 1.97 ms | 1.23M, p99 2.62 ms | 1.21M |
| durable `--remote 0 --reads 0` | **0.85M**, p99 2.23 ms | 0.64M, p99 8.26 ms | 0.65M |

Over TCP (2 shards or workers on cores 0-1, client on 2-3):

| Workload | SNC, owner-routed | SNC, any shard | Tuned baseline |
| --- | --- | --- | --- |
| memory, 50% reads | 2.07M, p99 121 µs | 1.64M | **2.25M**, p99 113 µs |
| memory, 95% reads | 2.26M | 1.62M | **2.58M** |
| durable, 50% reads | **1.18M**, p99 1.34 ms | 1.09M | 0.99M (2 logs), p99 3.34 ms |
| durable, writes only | **0.70M**, p99 1.70 ms | 0.67M | 0.63M, p99 3.01 ms |

Resident memory: SNC 499 B per object, baseline 452 B.

### What this says

1. **In memory, the tuned baseline is faster.** With the same map and
   prefetch, it beats SNC by 1.36× in process with every op local, by
   2.3× with every op remote, and by 1.09-1.14× over TCP. The v0.1 claim
   that SNC wins in memory came from the baseline's slower map.
2. **The durable path is where SNC still leads**: 1.42× in process and
   1.19× over TCP with half reads, with p99 latency 30-60% lower. One
   WAL stream per shard with no shared lock beats a shared or partitioned
   log behind a mutex, and that holds with every op remote too.
3. **The two engines do different amounts of work per op.** With 10K
   objects (everything in cache) SNC does 22.2M ops/s and the baseline
   25.2M. The baseline's deposit is a hard-coded increment under a lock;
   SNC looks up the op descriptor and runs a generic two-phase
   validate/apply with version checks, result buffers and completion
   callbacks. The longer path also hides less of each cache miss behind
   prefetch: with 1M objects SNC loses about 205 ns per op to memory
   against the baseline's 140 ns (a single-thread microbenchmark shows the
   same effect for any long per-op path). Part of SNC's deficit is this
   generic machinery, not shard-per-core.
4. **Uniform random keys flatter the baseline.** Its 4,096 stripes are
   almost never contended when keys are spread evenly. Hot keys, which
   real workloads have, would contend on stripe locks and bounce cache
   lines between cores, and they are what shard-per-core is meant to
   handle. This matrix does not test them yet.

## Limits

- Prefetch helps only when requests arrive in batches. A lightly loaded
  server sees one request per `recv` and gains nothing; it also loses
  nothing measurable.
- The baseline's prefetch reads entries without the stripe lock, as a hint
  (relaxed atomic loads); the run phase never grows a table, so a stale
  read only costs a wasted prefetch.
