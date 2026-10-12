# Against a conventional app + database stack

The handoff's central question is whether persistent application objects
run as directly executable state can cut the overhead of the usual
application/database boundary. The thread-pool baseline in the other
reports is an in-process map, not a database, so it cannot answer that.
This report runs the same accounts workload on PostgreSQL and Redis on
the same VM, cores and disk.

## Method

- 1M accounts in each system. 50% reads (balance and op count of one
  account), 50% deposits (balance + 1, op count + 1, updated time set,
  new balance and op count returned). Keys uniform at random.
- Servers on cores 0-1, load generators on cores 2-3, loopback TCP: the
  same split as `run_net_matrix.py`.
- **PostgreSQL 16**: table with a primary key and an email index, 2 GB
  shared buffers, table prewarmed. `pgbench` with prepared statements, 16
  connections, one statement per round trip; and a pipelined script that
  sends 16 statements per round trip over 8 connections. Durable means
  `synchronous_commit` and `fsync` on; memory means both off.
- **Redis 7**: one hash per account, 2 I/O threads. Reads are `HMGET`;
  deposits are a Lua script (`EVALSHA`) so the three field updates are
  atomic, as a deposit is in SNC. Two `redis-benchmark` processes (reads
  on one client core, deposits on the other), 32 connections each,
  pipeline depth 16. Durable means AOF with `appendfsync always`.
- SNC numbers are from `net_matrix_lean.csv` (owner-routed `net_bench`,
  depth 64 in memory and 256 durable per client thread).
- `bench/conventional/run.py`, 3 runs, medians;
  `bench/results/conventional.csv`.

## Results

| System | In memory | Durable |
| --- | --- | --- |
| SNC, owner-routed | **2.38M ops/s** | **1.21M ops/s**, p99 1.2 ms |
| Redis, pipelined | 249K | 129K |
| PostgreSQL, pipelined | 46K | 35K |
| PostgreSQL, one statement per round trip | 39K | 23K, mean 0.69 ms |

SNC does about 10× Redis's throughput and 50× PostgreSQL's in memory, and
9× and 35× durable.

## What this says, and what it does not

1. **The boundary is most of the cost.** PostgreSQL spends its two cores
   parsing, planning, locking rows, logging pages and running a process
   per connection; Redis spends them parsing a text protocol and running
   a Lua interpreter on one thread. SNC runs a registered handler on the
   object in place, from a binary request, on the core that owns it.
2. **This is not a like-for-like database comparison.** PostgreSQL gives
   SQL, multi-row transactions, MVCC and ad-hoc queries; Redis gives a
   general data-structure server. SNC v0.1 gives single-object ops on
   registered types and one secondary index. The numbers measure what an
   application gives up and gains by keeping its state as SNC objects
   instead of behind one of these servers.
3. **Redis is single-threaded for execution.** Its I/O threads help with
   sockets, but scripts run on one core, so it uses only one of its two
   server cores for the work itself. Redis Cluster or several instances
   would scale that, at the cost of sharding the keyspace by hand, which
   is what SNC does internally.
4. **Load generators differ.** `pgbench` and `redis-benchmark` are the
   standard clients for each system but are not the same program as
   `net_bench`. A spot check of per-core CPU during the runs put the
   server cores at 99-100% (Redis) and 82-93% (PostgreSQL, durable, the
   rest waiting on fsync) and the client cores at 12-21%, so the servers,
   not the clients, set the rate.

## Limits

- 4 vCPUs, loopback only, one disk.
- PostgreSQL is untuned beyond memory and WAL size; settings such as
  `commit_delay` or unlogged tables would move its numbers, though not by
  an order of magnitude.
- No application server sits in front of either database. A real stack
  adds one more hop and its own serialization, which would widen the gap.
