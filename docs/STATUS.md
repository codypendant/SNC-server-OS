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
- All six tests pass in RelWithDebInfo, ASan+UBSan and TSan builds.

## In-memory benchmark numbers

These numbers are from a cloud VM: 4 vCPUs (Xeon @ 2.80GHz), 1 NUMA node,
4 shards on 4 cores with the benchmark's main thread sharing them. There is
no WAL, and the workload is 50% `get_balance` and 50% `deposit` with 32
outstanding ops per shard and 1M accounts. Treat them as a smoke test, not
evidence for the thesis.

| Remote targets | ops/s | p50 | p99 | p99.9 | Messages per op |
| --- | --- | --- | --- | --- | --- |
| 0% | 5.07M | 0.6 µs | 1.6 µs | 18 µs | 0 |
| 50% | 4.13M | 3.8 µs | 96 µs | 410 µs | 1.0 |
| 100% | 3.57M | 22.5 µs | 92 µs | 639 µs | 2.0 |

Latency includes queueing behind the 32 outstanding ops. Load ran at about
3.5M creates/s. The heap used 320 bytes per 248-byte object (mimalloc size
class), and the local maps used 16 MiB.

## Durable benchmark numbers

Same VM, `--data-dir` on the VM's virtio disk (ext4), O_DIRECT, writes
with `RWF_DSYNC`, 50 µs group commit, 1M accounts. Every op is DURABLE:
deposits wait for their WAL record, and reads wait until the state they
saw is durable (D7). Latency is dominated by the disk's sync write time
and queueing behind the outstanding ops, so the deeper queue is what lets
group commit batch.

| Workload | Depth/shard | ops/s | p50 | p99 | Records per flush |
| --- | --- | --- | --- | --- | --- |
| 50% reads, 0% remote | 32 | 240K | 213 µs | 3.8 ms | 28 |
| 50% reads, 0% remote | 256 | 1.24M | 475 µs | 4.2 ms | 136 |
| 50% reads, 50% remote | 256 | 1.06M | 754 µs | 5.1 ms | 115 |
| 100% deposits, 0% remote | 256 | 680K | 1.1 ms | 2.6 to 3.2 ms | 236 |
| 50% reads, no sync (dev only) | 256 | 1.88M | 311 µs | 1.1 ms | n/a |

WAL records are full after-images (56-byte header + 200-byte payload);
block padding added 1 to 5% at depth 256 and 21% at depth 32. Records per
flush counts the whole run, load included. Loading 1M
accounts took 1.5 to 3.3 s because each create waits for durability.

Restart of 1M accounts (`snc_bench` restarts after the run):

| Starting point | Total | Checkpoint load | WAL replay | Index rebuild |
| --- | --- | --- | --- | --- |
| Checkpoint (244 MiB) + WAL tail | 2.9 to 3.1 s | 0.95 s | 1.85 s | 0.17 s |
| WAL only (2.3M records, 598 MiB) | 5.5 s | n/a | 5.3 s | 0.18 s |

A checkpoint of 1M objects wrote 244 MiB in 1.1 to 1.6 s while shards kept
serving. In the first row, nearly all of the replay time is spent scanning
the segment that holds the checkpoint's start LSN; both replay costs are
the two recovery deviations above.
