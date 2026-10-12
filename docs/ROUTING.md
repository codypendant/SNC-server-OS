# Routing requests to the owning shard

The v0.1 benchmark ([BENCHMARKS.md](BENCHMARKS.md)) showed that SNC wins
when an op runs on the shard that owns its object and loses most of that
lead when it does not. The in-process driver picked locality directly.
Real requests arrive on sockets, and the shard that reads a request is the
one that owns the connection, which has nothing to do with the object the
request targets. With N shards and ownership-blind ingress, (N-1)/N of
requests cross shards: 50% at 2 shards, 75% at 4, 98% at 64. This change
puts SNC behind a TCP front end and lets clients send each request to the
owner, then measures what that buys.

## Options considered

| Option | Routes per | Verdict |
| --- | --- | --- |
| Ingress forwarding only (the v0.1 path) | request | Always correct; costs a ring hop each way for (N-1)/N of requests. Kept as the fallback. |
| **Client-side routing** (owner-aware client) | request | **Chosen.** Client fetches the slot map once, holds one connection per shard, sends each request to the owner's port. No hop when the map is current. Same idea as shard-aware drivers in ScyllaDB and token-aware drivers in Cassandra. Needs a client library. |
| Kernel steering (`SO_REUSEPORT` + BPF reading the ObjectID) | datagram (UDP) or connection (TCP) | For TCP it can only pick a socket per connection, so it cannot route requests that share a connection. Per-datagram UDP steering is possible but needs its own reliability layer. Deferred. |
| NIC flow steering (aRFS, Flow Director, DPDK) | flow | Same per-flow limit; DPDK is a v0.1 non-goal. |
| Placement (co-locate related objects) | object | Complements routing for multi-object ops. v0.1 ops are single-object. Later. |

## Design

**One port per shard, polled by the shard.** Shard *i* listens on
`base_port + i` and polls its own sockets from its run loop (non-blocking
epoll, level-triggered, `TCP_NODELAY`). Nothing else touches the socket, so
a request read by its owner runs to completion on that core and the
response is written from the same core. The runtime interface gained one
call, `sri_watch_fd(fd)`, so an idle shard also wakes when its epoll fd is
readable. It stays backend-neutral: a Seastar backend would map it to a
reactor poller.

**Wire protocol v0** (`include/snc/wire.h`). Length-prefixed binary frames,
little-endian like every SNC format: a 48-byte request header (kind, flags,
op or type id, client-chosen `req_id`, target ObjectID, expected version)
followed by the args, and a 48-byte response header (status, app code,
owner, map epoch, `req_id`, ID, version, flags) followed by the result.
Responses can arrive out of order; `req_id` pairs them. Kinds are EXEC,
CREATE, DELETE and TOPOLOGY.

**Routing is a hint, never a requirement.** Any shard accepts any request
and runs it through the ordinary API, so a request for another shard's
object takes the existing cross-shard path. The response then carries
`kRespForwarded`, the owner, and the slot map epoch, so a client can tell
its map is stale and refetch it. In v0.1 the map changes only across a
restart, which drops every connection, so a reconnecting client always
starts from the current map. Online rebalance would reuse the same signal.

**TOPOLOGY** returns the map epoch, the shard count and the 4,096 slot
owners (8 KiB). The client (`snc::net::Client`) fetches it from shard 0,
connects to every shard and routes with `owner_of(id)`, the same function
the server uses.

**Creates mint local IDs** (`Engine::Config::mint_local`). A create runs on
the shard that mints the ID, but the new ID's owner is decided by its hash,
so a create used to cross shards (N-1)/N of the time and a client cannot
route it, because the ID does not exist yet. With `mint_local` the entry
shard skips sequence numbers until the ID hashes to a slot it owns: about N
hashes per create. IDs stay unique and are never reused (C12), placement
still depends only on the hash, and the ID still encodes no location: after
a remap the object moves like any other. Clients spread creates round-robin
and none of them cross shards. `snc_server` turns it on; the engine default
stays off so the existing tests keep exercising remote creates.

**Durability is unchanged.** The response goes out when the op's
completion runs, which for a DURABLE op is after its WAL record is durable
(D7). A connection that closes with requests in flight drops their
responses; the ops themselves still complete.

**Not covered yet:** online rebalance, multi-object ops, index lookups over
the wire (they still broadcast inside the server), auth and TLS, flow
control beyond an 8 MiB per-connection output cap, and clients in other
languages.

## Measurements

Same VM as the step 12 rerun below: 4 vCPUs (Xeon @ 2.10GHz), Linux 6.18,
ext4 on virtio, GCC 13.3, RelWithDebInfo. The server gets cores 0-1
(2 shards or 2 baseline workers), `net_bench` gets cores 2-3 (2 client
threads, each with a connection to every shard), so the load generator
never shares a core with the server. Loopback TCP. 1M Account objects
created over the wire first, then 4 s of `get_balance`/`deposit` against
uniformly random accounts, 3 runs per configuration, medians (min–max of
throughput). Depth is per client thread: 64 in memory, 256 durable.
`baseline_server` is the step 12 thread-pool baseline behind the same
front end: every worker runs whatever it receives against the shared
striped map, so routing changes nothing for it. Raw runs:
`bench/results/net_matrix.csv`; rerun with `bench/run_net_matrix.py`.

| Group | Engine | Routing | reads | ops/s | p50 | p99 | p99.9 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| memory | snc | owner | 50% | **1.78M** (1.72–1.89) | 56 µs | 148 µs | 0.70 ms |
| memory | snc | any (50% forwarded) | 50% | 1.36M (1.30–1.43) | 80 µs | 234 µs | 0.97 ms |
| memory | snc | all to non-owner | 50% | 1.22M (1.21–1.24) | 88 µs | 225 µs | 1.05 ms |
| memory | baseline | any | 50% | 1.37M (1.35–1.43) | 74 µs | 213 µs | 0.85 ms |
| memory | snc | owner | 95% | **1.71M** (1.61–1.81) | 59 µs | 156 µs | 0.72 ms |
| memory | snc | any | 95% | 1.37M (1.34–1.42) | 78 µs | 242 µs | 0.93 ms |
| memory | baseline | any | 95% | 1.42M (1.41–1.45) | 72 µs | 193 µs | 0.84 ms |
| durable | snc | owner | 50% | **1.12M** (1.07–1.15) | 418 µs | 1.28 ms | 3.21 ms |
| durable | snc | any | 50% | 0.97M (0.96–0.99) | 516 µs | 1.51 ms | 4.33 ms |
| durable | snc | all to non-owner | 50% | 0.84M (0.83–0.85) | 573 µs | 1.77 ms | 4.85 ms |
| durable | baseline, 2 logs | any | 50% | 0.91M (0.75–0.92) | 459 µs | 2.29 ms | 5.51 ms |
| durable | baseline, 1 log | any | 50% | 0.49M (0.48–0.51) | 885 µs | 3.08 ms | 8.26 ms |
| durable | snc | owner | 0% | 0.64M (0.63–0.68) | 705 µs | 1.87 ms | 4.33 ms |
| durable | snc | any | 0% | 0.65M (0.60–0.66) | 705 µs | 2.00 ms | 5.90 ms |
| durable | baseline, 2 logs | any | 0% | 0.58M (0.56–0.62) | 770 µs | 2.82 ms | 7.60 ms |

Creates over the wire (load phase, in memory): 1.07M/s with `mint_local`,
0.67M/s without (1.6×). Durable creates are disk-bound (0.59M/s SNC,
0.50M/s baseline with 2 logs).

### What the numbers say

1. **Routing recovers the locality win over real sockets.** In memory,
   owner-routed SNC does 1.78M ops/s against 1.37M for the baseline (1.30×)
   and 1.36M for ownership-blind SNC (1.31×), with a 25-30% lower p50 and
   p99. Durable with half reads, it does 1.12M against 0.91M for the
   strongest baseline (1.23×) and 0.97M unrouted (1.15×), with p99 1.28 ms
   against 2.29 ms.
2. **Unrouted SNC is no better than the baseline over sockets.** At 50%
   forwarded it ties the baseline in memory (1.36M vs 1.37M) and stays a
   little ahead durable. Forwarding everything costs 31% of throughput
   in memory and 25% durable against routing everything. So the shard-per-
   core design needs owner-aware routing to pay off; it is not optional.
3. **The network dominates the per-op cost.** Per shard, routed SNC does
   about 0.89M ops/s over loopback against about 1.26M in process
   (`--remote 0` rerun below). The server reads about 16 requests per
   `recv` and writes about 14 responses per `send`; syscalls, not the
   objects, are now the largest cost, so batching and io_uring sockets are
   where the next in-memory gains are.
4. **Write-only durable load is disk-bound,** so routing does not move it
   (0.64M vs 0.65M); SNC stays 1.1× ahead of the 2-log baseline there.
5. **Owner-local minting matters for creates**: 1.6× faster loads, since
   no create crosses a shard.

### Limits

- 2 server cores and 2 client cores on one 4-vCPU VM, over loopback.
  Loopback runs the TCP stack on the sending core, so this understates
  what a NIC with per-core queues would give and says nothing about NUMA.
- One client thread drives every shard; real clients with fewer outstanding
  requests per connection would batch less, and pay more per request.
- The baseline front end is ours and as simple as SNC's.

## Durable p99: the cause

The step 12 report found SNC's durable p99 at 5-6 ms against about 3.5 ms
for the baseline in process, and suspected one WAL flush in flight per
shard. The rerun on this VM reproduces the gap (`bench/results/matrix_rerun.csv`:
6.7-7.7 ms against 2.4-3.0 ms), and new counters in `WalStream::Stats`
(flush latency histogram, rollover time) point elsewhere:

- **Not rollover.** The longest segment switch on a shard was under 300 µs
  and no segment was ever created on a shard thread (spares were ready).
- **Not SNC's flush structure.** The baseline also has one flush in flight
  per log, and with `--no-sync` both engines' p99 falls under 1.5 ms
  (SNC 0.85 ms, baseline 1.5 ms), so the tail comes from the sync writes.
- **CPU starvation of the kernel's I/O workers.** O_DIRECT writes with
  `RWF_DSYNC` cannot complete inline, so io_uring hands them to `iou-wrk`
  kernel threads (visible in `ps` during a run). With 4 shards pinned to
  4 cores and spinning under load, those threads wait for a time slice on
  a busy core. The baseline's workers call `yield()` while they wait, which
  lets its log writers run. With one core left free (3 shards on 4 cores)
  SNC's durable p99 falls from 3.7-4.8 ms to 1.1-1.4 ms and throughput rises
  from 1.53-1.74M to 1.66-1.84M ops/s. Over sockets (2 shards, client on
  other cores, shards idle in `epoll` between batches) SNC's p99 is already
  below the baseline's.

### The fix: yield while disk I/O is in flight

A shard with WAL records buffered never idles, so it never gives up its
core on its own. `RuntimeConfig::io_yield_us` (default 50) makes a shard
call `sched_yield()` at most every 50 µs while it has file I/O in flight,
which lets an `iou-wrk` thread waiting on that core complete the write. It
costs nothing in memory (no I/O in flight) and one syscall per 50 µs
otherwise. Binding the I/O workers to a core that also runs a shard
(`io_worker_cpus`, via `io_uring_register_iowq_aff`) did not help; that
option is for machines with a spare housekeeping core.

5 runs each, 4 shards, `--remote 0 --reads 50 --depth 256`, durable:

| Setting | ops/s | p99 |
| --- | --- | --- |
| no yield (v0.1) | 1.23–1.57M | 3.3–7.2 ms |
| yield every 20 µs | 1.68–1.80M | 1.47–1.87 ms |
| yield every 50 µs | 1.69–1.84M | 1.54–1.77 ms |
| yield every 100 µs | 1.68–1.83M | 1.47–1.77 ms |
| no yield, 3 shards (1 core free) | 1.52–1.74M | 1.18–1.34 ms |

The durable in-process matrix with the 50 µs default
(`bench/results/matrix_io_yield.csv`, 3 runs, medians) against the rerun
without it (`matrix_rerun.csv`) and the 4-log baseline:

| Flags | SNC before | SNC after | Baseline, 4 logs |
| --- | --- | --- | --- |
| `--remote 0 --reads 50` | 1.30M, p99 6.68 ms | **1.91M, p99 1.44 ms** | 1.39M, p99 2.36 ms |
| `--remote 50 --reads 50` | 1.16M, p99 7.73 ms | **1.86M, p99 1.97 ms** | 1.30M, p99 2.75 ms |
| `--remote 100 --reads 50` | 1.01M, p99 7.73 ms | **1.51M, p99 2.69 ms** | 1.32M, p99 2.36 ms |
| `--remote 0 --reads 0` | 0.78M, p99 2.36 ms | **0.84M, p99 2.23 ms** | 0.60M, p99 10.5 ms |

Durable SNC now leads the strongest baseline by 1.37× with every op local
and by 1.14× with every op remote, and its all-local p99 is lower than the
baseline's. Over sockets (2 shards with free client cores) the change makes
no measurable difference, as expected: those shards were not starving the
I/O workers.

## In-process rerun on this VM

The step 12 numbers in BENCHMARKS.md came from a different VM (2.80GHz).
For a like-for-like reference, the same matrix was rerun here
(`bench/results/matrix_rerun.csv`, 3 runs each, medians):

| Group | Engine | Flags | ops/s | p50 | p99 |
| --- | --- | --- | --- | --- | --- |
| memory | snc | `--remote 0 --reads 50` | 5.03M | 0.6 µs | 1.7 µs |
| memory | snc | `--remote 50 --reads 50` | 4.23M | 3.8 µs | 96 µs |
| memory | snc | `--remote 100 --reads 50` | 3.37M | 22.5 µs | 109 µs |
| memory | baseline | `--remote 0 --reads 50` | 4.26M | 0.8 µs | 1.4 µs |
| memory | baseline | `--remote 100 --reads 50` | 3.64M | 1.0 µs | 1.7 µs |
| durable | snc | `--remote 0 --reads 50` | 1.30M | 336 µs | 6.68 ms |
| durable | snc | `--remote 100 --reads 50` | 1.01M | 639 µs | 7.73 ms |
| durable | baseline, 4 logs | `--remote 0 --reads 50` | 1.18M | 573 µs | 2.69 ms |
| durable | snc | `--remote 0 --reads 0` | 0.78M | 967 µs | 2.36 ms |
| durable | baseline, 4 logs | `--remote 0 --reads 0` | 0.65M | 1.28 ms | 4.19 ms |

On this VM the all-local in-memory lead is 1.18× (it was 1.47×): the
baseline gained more from this machine than SNC did.
