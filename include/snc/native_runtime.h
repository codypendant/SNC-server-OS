// Native backend for the SNC Runtime Interface (spec D2): pthreads pinned with
// sched_setaffinity, a full mesh of SPSC rings, eventfd idle wakeup.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "snc/pages.h"
#include "snc/sri.h"

namespace snc {

struct RuntimeConfig {
  u16 n_shards = 1;
  bool pin = true;              // pin worker i to cpus[i] (or core i)
  std::vector<int> cpus;        // optional explicit core list
  u32 ring_slots = 256;         // power of two
  u32 overflow_limit = 256;     // per destination, for requests
  u32 batch_in = 32;            // messages drained per ring per iteration
  u32 idle_spin_us = 50;        // spin before sleeping; 0 = sleep at once
  bool spin_forever = false;    // benchmarks: never sleep
  u64 heap_limit_bytes = 0;     // per shard, 0 = unlimited
  // Pages for each shard's object heap and local map (docs/HUGEPAGES.md).
  // With huge pages the heap is an arena of heap_reserve_bytes per shard;
  // allocations beyond it use normal pages.
  PageMode pages = PageMode::SMALL;
  u64 heap_reserve_bytes = 0;
  // While file I/O is in flight, give the core up (a brief sleep) at most
  // this often so the kernel threads that complete that I/O (io_uring
  // workers, ksoftirqd, dio kworkers) can run on it. 0 = never. See
  // docs/ROUTING.md, "Durable p99: the cause", and
  // docs/F1_Q10_LIVENESS_ROOT_CAUSE.md (why a sleep, not sched_yield).
  u32 io_yield_us = 50;
  // Cores for the kernel's io_uring workers; empty = kernel default.
  std::vector<int> io_worker_cpus;
};

struct RingStats {
  u64 msgs = 0;
  u64 bytes_inline = 0;
  u64 bytes_external = 0;
  u64 full_events = 0;   // ring full, message went to overflow
  u64 refused = 0;       // overflow full, request refused
};

class NativeRuntime {
 public:
  NativeRuntime(const RuntimeConfig& cfg, const ShardHooks& hooks);
  ~NativeRuntime();
  NativeRuntime(const NativeRuntime&) = delete;
  NativeRuntime& operator=(const NativeRuntime&) = delete;

  // Starts workers and returns once every shard has run its init hook.
  void start();
  // Stops all loops, runs fini hooks, joins threads.
  void stop();

  // Control path (not hot): run `fn` on shard `shard`'s worker thread
  // during its next loop iteration. Safe from any thread.
  void submit(u16 shard, std::function<void()> fn);
  // Runs `fn` on every shard and waits until all have run it.
  void run_on_all(const std::function<void(u16)>& fn);

  u16 shard_count() const;
  const ShardTopology& topology(u16 shard) const;
  // Shard progress (docs/HEALTH_DECOUPLING.md 3.2): the number of loop
  // iterations shard `shard` has started. Safe from any thread; never waits
  // on the shard. False if this backend does not count them (Seastar).
  bool progress(u16 shard, u64& iterations) const;
  // Stats for ring (src, dst); read after stop() or accept racy values.
  RingStats ring_stats(u16 src, u16 dst) const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace snc
