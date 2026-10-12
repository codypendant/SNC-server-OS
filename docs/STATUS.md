# Prototype status

What the code covers today, measured against the
[SNC v0.1 Object & Execution Specification](https://claude.ai/code/artifact/8a25e96e-09ae-43ef-9e42-d6763e1eebd9)
and the handoff's development sequence (§24).

## Handoff §24 steps

| Step | State | Where |
| --- | --- | --- |
| 1. Development Linux | Done: Ubuntu 24.04, GCC 13, CMake | `CMakeLists.txt` |
| 2. Runtime abstraction | Done for the native backend; Seastar adapter not started | `include/snc/sri.h`, `src/native/runtime.cpp` |
| 3. Dedicated per-core workers | Done: pinned threads, run-to-completion loop, eventfd idle | `src/native/runtime.cpp` |
| 4. Stable ObjectID | Done | `include/snc/object_id.h` |
| 5. In-memory object directory | Done: slot map + per-shard local map | `include/snc/slot_map.h`, `include/snc/local_map.h` |
| 6. Shard ownership | Done | `src/core/shard.cpp` |
| 7. `execute(ObjectID, op)` | Done: two-phase validate/apply, create, delete | `src/core/shard.cpp`, `include/snc/snc.h` |
| 8. Cross-shard dispatch | Done: 128-byte `Msg`, SPSC ring mesh, external buffers, `BUF_RETURN` | `include/snc/msg.h`, `src/core/shard.cpp` |
| 9. Secondary index | Done: shard-local `Account.email`, broadcast lookup, durable-only results | `src/core/shard.cpp`, `include/snc/registry.h` |
| 10. WAL/persistence | Done: per-shard streams, after-images, group commit over io_uring, durable acks | `include/snc/wal_format.h`, `src/core/storage.cpp` |
| 11. Checkpoint/recovery | Done: fuzzy per-shard checkpoint, A/B superblock, replay, remap on shard-count change | `src/core/engine.cpp`, `src/core/shard.cpp` |
| 12. Locality benchmarks | Done: SNC vs a thread-pool baseline, in memory and durable; report in `docs/BENCHMARKS.md` | `bench/`, `docs/BENCHMARKS.md` |

## After v0.1

| Step | State | Where |
| --- | --- | --- |
| Object lookup cost | Done: local map entries carry their hash; batch prefetch on socket and ring ingress (`snc::prefetch`). Measured against a baseline with the same map and prefetch: SNC leads when ops reach the owner and durable, trails on cross-core ops in process. Ring messages are one cache line | `include/snc/local_map.h`, `src/core/shard.cpp`, [PREFETCH.md](PREFETCH.md) |
| Durable tail latency | Done: shards yield every 50 µs while disk I/O is in flight so io_uring workers can complete WAL writes (`RuntimeConfig::io_yield_us`) | `src/native/runtime.cpp`, [ROUTING.md](ROUTING.md#durable-p99-the-cause) |
| Hot-key (Zipf) workloads | Done: `--zipf THETA` in all benchmarks; SNC overtakes the baseline at high skew (θ 1.5), and over TCP at θ 0.99 too; leads durable at every skew | `bench/zipf.h`, [SKEW.md](SKEW.md) |
| Comparison with a conventional stack | Done: same workload on PostgreSQL 16 and Redis 7, same cores; SNC about 10× Redis and 35-50× PostgreSQL | `bench/conventional/`, [CONVENTIONAL.md](CONVENTIONAL.md) |
| Two-object ops (transfers) | Done: durable follow-ups (`OpCtx::emit`, `resume`) across shards, one op with an atomic WAL group (`OpCtx::local`) within a shard; `create_near` places related objects in one slot; conserved across restart, remap, torn groups and SIGKILL. Random pairs: 1.8x slower than the locked baseline in memory, 1.2x durable; all pairs on one shard: 1.6x faster in memory, 1.1x durable. 7-9x Redis, 14-35x PostgreSQL | [TRANSFERS.md](TRANSFERS.md) |
| Request routing to the owning shard | Done: per-shard TCP listeners, wire protocol v0, owner-aware client, owner-local minting for creates; measured against the baseline behind the same front end | `src/net/`, `include/snc/wire.h`, `tools/snc_server.cpp`, [ROUTING.md](ROUTING.md) |

## Known deviations from the spec

- **Recovery after a remap routes records on the control thread.** With an
  unchanged slot map, every shard reads its own checkpoint part and WAL
  stream in parallel. After a shard-count change (or before the first
  checkpoint exists) records are read on the control thread and routed to
  their new owners; a checkpoint follows, so this happens once per remap.
- **Recovery reads the whole segment that holds the checkpoint's start
  LSN**, CRC-checking records it then skips. There is no seek index.
- **Segments are allocated ahead, not zeroed ahead.** A preparer thread
  keeps one `fallocate`d, synced spare per stream; the shard renames it
  into place at rollover and writes the header with its first flush. The
  rename is not directory-synced on the shard; recovery adopts a spare
  whose header is valid instead.
- **A hole inside a segment ends it.** Anything after the first invalid or
  out-of-sequence record in a segment is treated as an unacknowledged tail,
  which is right for crashes (one flush in flight per stream) but cannot
  tell mid-segment media corruption apart. A gap *between* segments still
  stops recovery (C4).
- **Creates always wait for durability**, whatever `ExecOptions` says, so a
  new ID is never handed out before its record is on disk.
- **New registry types are accepted** at recovery even when the manifest's
  type catalog does not list them; only types that disappeared or changed
  schema stop recovery.
- **Local map growth rehashes in one step**, not incrementally. Engine
  pre-sizes maps from `expected_objects` to keep rehash out of runs.
- **Handler time budget** (20 µs, spec section 5) is not measured yet.
- **`BUF_RETURN` is sent per buffer**, not batched.
- **Ring stats are read racily while running** (benchmark only); they are
  exact after `stop()`.

## How durability is tested

- `test_persistence`: create, deposit, re-key email, delete; clean restart
  from the WAL alone; checkpoint then more writes; restart with 3 shards
  (slot remap, orphan stream folded into a forced checkpoint) and back to 4;
  a garbage block written after the last record of a segment (torn tail).
  Every phase checks every object's version and balance and samples the
  email index from all shards. Also checks that IDs minted after a restart
  use fresh leases (C12) and that a missing superblock without `init` is
  refused (C13).
- `test_crash`: a child process creates accounts and deposits into them,
  reporting each acknowledged result to the parent, which kills it with
  SIGKILL at a different point each round (4 rounds on one directory).
  A fresh process recovers and checks that every acknowledged version
  survived and that every account's balance matches its version. A process
  kill does not tear writes the kernel already accepted, so torn tails are
  covered by `test_persistence`, not here; power loss is not tested.
- `test_net`: the TCP front end in memory and durable: topology, creates
  that stay on their entry shard, requests to the owner and forwarded
  requests (flagged), reads, unknown targets and ops, a client that
  disconnects with 2,000 requests in flight, and a malformed frame.
- All seven tests pass in RelWithDebInfo, ASan+UBSan and TSan builds.

## Benchmark numbers

Routing over TCP, and the cause of the durable p99 gap, are in
[ROUTING.md](ROUTING.md). See [BENCHMARKS.md](BENCHMARKS.md) for the step 12 comparison against a
conventional thread-pool baseline, including durable throughput, latency,
memory and restart time.
