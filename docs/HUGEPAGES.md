# Huge pages

The handoff's memory section (§10) asks to benchmark 4 KiB pages, 2 MiB huge
pages and THP for the hot structures instead of assuming one wins, and to keep
huge pages to selected structures rather than the whole process. This round
does that for the two structures every op touches: the shard's local object
map and its object heap.

## What changed

`RuntimeConfig::pages` picks the page size for both. The default is still
4 KiB, so earlier results stay comparable.

| Mode | How | When it is not available |
| --- | --- | --- |
| `SMALL` (default) | Ordinary anonymous memory | n/a |
| `THP` | `madvise(MADV_HUGEPAGE)` on 2 MiB-aligned regions; works with the usual `enabled=madvise` setting | The kernel uses 4 KiB pages where it cannot find 2 MiB |
| `HUGETLB` | `MAP_HUGETLB` from the reserved pool (`vm.nr_hugepages`) | Falls back to `THP` if the pool is short, and `ShardStats` says so |

- **Local map** (`include/snc/local_map.h`): the table is mapped with
  `map_pages` (`include/snc/pages.h`) instead of `std::vector`. Tables under
  2 MiB stay on 4 KiB pages so small maps don't waste a whole huge page.
- **Object heap** (`src/core/allocator.cpp`): each shard's mimalloc heap
  allocates from its own exclusive arena (`mi_manage_os_memory_ex`,
  `mi_heap_new_in_arena`) of `heap_reserve_bytes`. The arena is mapped on the
  pinned worker thread, so first touch stays local. When the arena is full,
  allocations continue in a normal heap (`heap_arena_misses` counts them).
  `Engine` sizes the arena from `expected_objects` (512 B each, at least
  256 MiB per shard). mimalloc keeps arenas for the life of the process, so
  this assumes one `Engine` per process, as `snc.h` already does.
- **Flags:** `snc_bench --pages 4k|thp|hugetlb` and the same flag on
  `snc_server`. `run_matrix.py --groups pages` runs the comparison; it
  reserves the HugeTLB pool and sets system THP for the baseline itself
  (needs root).

Rings, pending tables and the WAL buffers stay on normal pages. They are
small and touched sequentially, so the TLB is not their problem.

## Results

4 vCPU VM, one NUMA node, 1M accounts, 4 shards, mean of 3 runs
(`bench/results/matrix_pages.csv`). The baseline has no page option, so it
runs once with the default system setting (`madvise`, which gives it 4 KiB
pages) and once with `enabled=always`.

| Workload | SNC 4k | SNC thp | SNC hugetlb | Baseline 4k | Baseline THP |
| --- | --- | --- | --- | --- | --- |
| Memory, all local, depth 256 | 15.88M | 16.25M | 17.56M | 12.99M | 14.05M |
| Memory, all remote, depth 128 | 10.47M | 10.65M | 11.14M | 11.52M | 12.79M |
| Transfers, depth 32 | 3.77M | 3.88M | 3.87M | 6.10M | 6.51M |
| Durable, all local, depth 256 | 2.02M | 2.15M | 2.00M | 1.47M | 1.71M |

In the matrix, THP and HugeTLB differ by up to 8%. When the same binary
alternates between them six times, they come out level: 4k 17.12M, thp
18.53M, hugetlb 18.51M. The gap in the matrix is run-to-run noise on this
VM, not a property of either mode.

- **Huge pages are worth 3-10% in memory.** With 1M objects the heap is
  about 80 MiB per shard. That is far more than the 4 KiB TLB covers, so
  random lookups miss the TLB as well as the cache. With 2 MiB pages, about
  40 entries cover a shard's heap.
- **They do not change who leads.** The baseline gains as much from
  system-wide THP: 8% local, 11% remote, 16% durable. With huge pages on
  both sides, SNC leads all-local work 1.25x (1.22x on 4k) and trails
  all-remote work 0.87x (0.91x). Huge pages remove a cost both designs pay
  per random access. They are not an advantage of state-native placement.
- **SNC needs only its own memory on huge pages.** `THP` mode gets the
  whole gain with the system left at `madvise`. That is the handoff's
  "selected critical structures" rule, and it leaves the rest of the
  machine alone.
- **Durable runs** are bound by the WAL, not lookups, so they move only
  within noise.

## Choice

Use `thp` on a stock kernel. It needs no setup, falls back on its own, and
gives the same gain here. Use `hugetlb` where memory is reserved at boot, as
the handoff describes for a dedicated machine. Then the pages exist before
the first object does and cannot be split or compacted later. The default
stays 4 KiB until the minimal-Linux image decides how memory is reserved.

1 GiB pages are not tried: a shard's heap at this size fits in 40 pages of
2 MiB, which the second-level TLB already holds.

## Not measured here

NUMA. This VM has one node, so worker, heap and map are always local. The
arena is mapped from the pinned worker thread, so first touch already
places it on the worker's node. A multi-socket machine is needed to measure
the cross-node cost.
