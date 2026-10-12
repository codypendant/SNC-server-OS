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
`bench/results/matrix_rerun.csv` and `net_matrix.csv`; "after" is
`matrix_prefetch.csv` and `net_matrix_prefetch.csv`.

In process, in memory (4 shards, depth 32):

| Flags | SNC before | SNC after | Baseline |
| --- | --- | --- | --- |
| `--remote 0 --reads 50` | 5.03M, p50 0.6 µs | **9.03M**, p50 0.3 µs | 4.09M |
| `--remote 0 --reads 95` | 5.68M | **9.95M** | 3.88M |
| `--remote 50 --reads 50` | 4.23M, p50 3.8 µs | **6.41M**, p50 2.6 µs | 3.58M |
| `--remote 100 --reads 50` | 3.37M, p50 22.5 µs | **5.18M**, p50 15.4 µs | 3.51M |
| `--shards 1 --remote 50` | 1.33M | **2.19M** | 0.94M (1 thread) |

Single runs separating the two changes (`--remote 0 --reads 50`): the
tagged map alone took 5.0M to 7.6M; prefetch took 7.6M to 9.3M. At 100%
remote, ring prefetch took 4.4M to 5.4M.

Over TCP (2 shards on cores 0-1, client on 2-3):

| Workload | SNC before | SNC after | Baseline |
| --- | --- | --- | --- |
| memory, owner-routed, 50% reads | 1.78M, p99 148 µs | **2.08M**, p99 109 µs | 1.42M |
| memory, owner-routed, 95% reads | 1.71M | **2.19M** | 1.44M |
| memory, any shard, 50% reads | 1.36M | 1.59M | 1.42M |
| durable, owner-routed, 50% reads | 1.12M, p99 1.28 ms | 1.18M, p99 1.21 ms | 0.82M (2 logs) |
| durable, owner-routed, writes only | 0.64M | 0.67M | 0.57M (2 logs) |

So routed SNC is now 1.46× the baseline over sockets in memory (it was
1.30×) and 2.2× in process with every op local. Durable runs move little:
they are bound by the disk.

## Limits

- The baseline keeps `std::unordered_map` per lock stripe and does not
  prefetch. A tuned conventional engine could use a flatter map and
  prefetch as well, though looking ahead there means touching stripes
  other threads lock. Part of SNC's new lead is the map, not the
  architecture.
- Prefetch helps only when requests arrive in batches. A lightly loaded
  server sees one request per `recv` and gains nothing; it also loses
  nothing measurable.
