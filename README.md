# SNC: State-Native Compute

SNC explores whether persistent application objects can be directly
executable state: owned by per-core shards, addressed by stable 128-bit
ObjectIDs, and operated on in place through `execute(ObjectID, op, args)`,
on minimal Linux.

The design lives in the
[SNC v0.1 Object & Execution Specification](https://claude.ai/code/artifact/8a25e96e-09ae-43ef-9e42-d6763e1eebd9).
Progress against it and known deviations are in
[docs/STATUS.md](docs/STATUS.md); the v0.1 benchmark comparison is in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Build and test

Requires Linux, a C++20 compiler, CMake ≥ 3.20, mimalloc and xxHash
and liburing (`apt install libmimalloc-dev libxxhash-dev liburing-dev`).

```sh
cmake -S . -B build
cmake --build build -j
(cd build && ctest --output-on-failure)
```

Sanitizer builds:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread"
```

## Locality micro-benchmark

```sh
./build/snc_bench --shards 4 --objects 1000000 --seconds 5 --remote 50
```

`--remote` is the percentage of ops whose target is owned by another shard.
Other flags: `--reads PCT`, `--depth N` (outstanding ops per shard),
`--no-pin`, `--spin` (never sleep).

With `--data-dir DIR` (a new or empty directory) every op is durable: the
WAL is on, and after the run the benchmark takes a checkpoint and restarts
the engine to time recovery. `--no-checkpoint` restarts from the WAL alone,
`--group-commit-us N` sets the group-commit window, `--no-direct` turns off
O_DIRECT and `--no-sync` drops the sync (development only).

```sh
./build/snc_bench --objects 1000000 --seconds 5 --depth 256 --data-dir /tmp/snc-data
```

## Server and routing benchmark

`snc_server` runs the engine behind a TCP front end, one port per shard
(`--port P` is shard 0, shard i listens on P + i). Clients fetch the slot
map and send each request to the shard that owns its target; a request
sent elsewhere is forwarded, so routing is an optimization, never a
requirement. Design and numbers: [docs/ROUTING.md](docs/ROUTING.md).

```sh
./build/snc_server --cpus 0,1 --port 7400 &              # 2 shards on cores 0-1
./build/net_bench --cpus 2,3 --threads 2 --port 7400 --route owner
```

`net_bench --route any` sends each request to a random shard instead
(ownership-blind ingress), and `--misroute PCT` sends PCT% to a non-owner.
`baseline_server` puts the thread-pool baseline behind the same front end.
`bench/run_net_matrix.py` runs both under the same workloads and writes
`bench/results/net_matrix.csv`; `bench/summarize.py bench/results/net_matrix.csv --markdown`
summarizes it.

## Layout

| Path | Contents |
| --- | --- |
| `include/snc/` | Public headers: ObjectID, object header, slot map, local map, registry, messages, runtime interface, application API |
| `src/core/` | Shard execution, dispatch, allocator, registry |
| `src/native/` | Native runtime backend (pinned threads, SPSC ring mesh) |
| `src/net/` | TCP front end (per-shard listeners), wire protocol, owner-aware client |
| `tools/` | `snc_server` |
| `src/types/` | The `Account` benchmark type |
| `tests/` | Unit and end-to-end tests |
| `bench/` | Benchmarks |

## Baseline and benchmark matrix

`baseline_bench` runs the same workload on a conventional design (thread
pool, shared striped hash map, shared or partitioned WAL). It takes the
same flags as `snc_bench`, with `--threads` instead of `--shards` and
`--logs K` for the number of WAL partitions.

```sh
bench/run_matrix.py --reps 3 --seconds 4   # writes bench/results/matrix.csv
bench/summarize.py --markdown              # medians per configuration
```

