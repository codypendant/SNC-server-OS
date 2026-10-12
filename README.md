# SNC: State-Native Compute

SNC explores whether persistent application objects can be directly
executable state: owned by per-core shards, addressed by stable 128-bit
ObjectIDs, and operated on in place through `execute(ObjectID, op, args)`,
on minimal Linux.

The design lives in the
[SNC v0.1 Object & Execution Specification](https://claude.ai/code/artifact/8a25e96e-09ae-43ef-9e42-d6763e1eebd9).
Progress against it, known deviations and first numbers are in
[docs/STATUS.md](docs/STATUS.md).

## Build and test

Requires Linux, a C++20 compiler, CMake ≥ 3.20, mimalloc and xxHash
(`apt install libmimalloc-dev libxxhash-dev`; liburing will be needed for the WAL).

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

## Layout

| Path | Contents |
| --- | --- |
| `include/snc/` | Public headers: ObjectID, object header, slot map, local map, registry, messages, runtime interface, application API |
| `src/core/` | Shard execution, dispatch, allocator, registry |
| `src/native/` | Native runtime backend (pinned threads, SPSC ring mesh) |
| `src/types/` | The `Account` benchmark type |
| `tests/` | Unit and end-to-end tests |
| `bench/` | Benchmarks |
