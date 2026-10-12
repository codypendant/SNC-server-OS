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
| 9. Secondary index | Not started | |
| 10. WAL/persistence | Not started | |
| 11. Checkpoint/recovery | Not started | |
| 12. Locality benchmarks | First micro-benchmark only | `bench/snc_bench.cpp` |

## Known deviations from the spec

- **No durability yet.** Every applied state reports `durable = true` because
  there is no WAL. Read-waits-for-durability (D7) is trivially satisfied.
- **Mint leases are not persisted.** Lease = shard + 1 per process, so IDs
  repeat across restarts. Spec rule C12 needs the superblock (step 10).
- **No delete tombstones.** Deletes erase at once, which is correct only
  while there is no WAL.
- **Local map growth rehashes in one step**, not incrementally. Engine
  pre-sizes maps from `expected_objects` to keep rehash out of runs.
- **Handler time budget** (20 µs, spec section 5) is not measured yet.
- **`BUF_RETURN` is sent per buffer**, not batched.
- **Ring stats are read racily while running** (benchmark only); they are
  exact after `stop()`.

## First benchmark numbers

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
