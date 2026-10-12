// Native backend for the SNC Runtime Interface (spec D2): pthreads pinned with
// sched_setaffinity, a full mesh of SPSC rings, eventfd idle wakeup.
#pragma once

#include <functional>
#include <memory>
#include <vector>

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
  // Stats for ring (src, dst); read after stop() or accept racy values.
  RingStats ring_stats(u16 src, u16 dst) const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace snc
