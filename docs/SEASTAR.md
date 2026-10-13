# Seastar evaluation

Handoff §24 step 2 asks for a runtime abstraction, with Seastar evaluated as
the first candidate behind it. §15 adds that Seastar is "an implementation
candidate, not SNC's public abstraction". The accepted spec builds the
native runtime first and evaluates Seastar second, behind the same
interface. This is that evaluation.

## What was built

`src/seastar/runtime.cc` implements the SNC Runtime Interface (`sri.h`) and
the `NativeRuntime` class on Seastar. SNC core is unchanged: the same
`shard.cpp`, `engine.cpp`, storage, types and benchmark compile against
either backend, and the linker picks one. `sri_backend()` reports which.

- **Shards** are Seastar reactor threads (`--smp N --cpuset ...`), pinned to
  the same cores as the native backend.
- **Loop:** each shard's iteration (poll hook, flush, I/O completions) runs as
  a Seastar poller (`seastar::internal::poller`), so it runs once per reactor
  loop, between tasks.
- **Messages, two modes:**
  - `smp` (default): Seastar's own inter-core queues. Messages for one
    destination collected during an iteration go as one `smp::submit_to`,
    which runs them on the owner as a task.
  - `rings` (`SNC_SEASTAR_MSG=rings`): SNC's own SPSC rings
    (`src/native/ring.h`, now shared with the native backend), drained by the
    poller. This mode isolates the cost of Seastar's reactor from the cost
    of its messaging.
- **File I/O:** a per-shard io_uring polled by the same poller, as in the
  native backend, so durable runs compare runtimes and not I/O paths.
  Seastar's reactor itself uses its epoll backend.
- **Build:** `-DSNC_SEASTAR=ON -DCMAKE_PREFIX_PATH=<seastar prefix>` builds
  `snc_bench_seastar`. Seastar master needs GCC 14 and fmt 10 or newer, so
  that build uses `g++-14` and fmt 11.0.2 built from source. The default
  build does not need Seastar.
- **Limits:**
  - One runtime per process, because a Seastar reactor cannot restart. So
    `snc_bench --no-restart` skips the timed recovery, and the unit tests
    (which restart engines) stay on the native backend.
  - Shards never sleep.
  - In `smp` mode, sends are never refused.

## Results

4 vCPU VM, 1M accounts, 4 shards, mean of 3 runs
(`bench/results/matrix_seastar.csv`, `run_matrix.py --groups seastar`):

| Workload | Native | Seastar, smp | Seastar, SNC rings |
| --- | --- | --- | --- |
| Memory, all local, depth 256 | 15.35M | 14.12M (0.92x) | 14.54M (0.95x) |
| Memory, all remote, depth 32 | 8.26M | 3.70M (0.45x) | 7.32M (0.89x) |
| Memory, all remote, depth 128 | 11.16M | 6.65M (0.60x) | 9.74M (0.87x) |
| Memory, all remote, depth 512 | 11.89M | 9.32M (0.78x) | 10.83M (0.91x) |
| Transfers, depth 32 | 3.68M | 3.26M (0.89x) | 3.36M (0.91x) |
| Durable, all local, depth 256 | 2.04M | 2.14M (1.05x) | 2.12M (1.04x) |
| Durable, all remote, depth 256 | 2.06M | 1.63M (0.79x) | 1.83M (0.89x) |

Remote p50 at depth 32: native 9.3 us, Seastar smp 23.2 us, Seastar with
rings 9.8 us.

- **Seastar's inter-core messaging is the big cost.** With `smp::submit_to`,
  all-remote work runs at 0.45x to 0.78x of native, and p50 latency is 2.5x
  higher. Swapping in SNC's rings on the same reactor recovers most of it:
  0.87x to 0.91x. Most of the gap is messaging; what remains is the
  reactor. `submit_to` turns each batch into a heap-allocated task, and it
  flushes and polls its queues on Seastar's own schedule. SNC's rings write
  fixed 64-byte messages in place.
- **The reactor costs 5-13%.** That holds even with SNC's rings and with no
  cross-core work. SNC's loop runs as one poller among Seastar's (timers,
  smp queues, epoll), between task-queue runs, instead of being the whole
  loop.
- **Durable all-local is the exception: Seastar is 4-5% ahead, with lower
  p99.** The native loop has to yield every 50 us so the kernel's io_uring
  workers get the core (`docs/ROUTING.md`). Seastar's reactor makes enough
  syscalls of its own that those workers get the core without yielding. This
  is inferred, not traced: native with yields off drops to 1.59M with a
  5.8 ms p99. The native yield interval may have room to tune.
- **What Seastar would bring but this run does not use:** its TCP/DPDK
  network stack, `seastar::file` with its I/O scheduler, and futures for
  application code. SNC already has a routed TCP front end and its own WAL
  path. The handoff also warns against stacking Seastar, DPDK and VPP
  without benchmarking each overlap.

## Decision

Keep the native backend as SNC's runtime. On every in-memory workload,
Seastar is slower in both of its forms. Its cross-core primitive, the one
SNC uses most, costs up to 2.2x. The interface held up, though: SNC core
moved to Seastar with no change, which is the property handoff rule 3
("keep SNC abstractions independent of Seastar") asks for. The backend
stays in the tree, off by default, so the comparison can be rerun when
either side changes. It also serves as a starting point if the DPDK phase
wants Seastar's network stack.
