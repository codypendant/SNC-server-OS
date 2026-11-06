// Diagnostic-only telemetry (CP-1A abort diagnosis, branch r640-diag-telemetry).
// Each shard thread stores its own gauges with relaxed atomics; a sampler
// reads them from another CPU and never submits work to a shard. Off unless
// snc_server is started with --diag-gauges. Changes no behaviour.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace snc::diag {

struct alignas(64) ShardGauge {
  std::atomic<uint64_t> polls{0}, emits{0}, emit_wait{0}, waiters{0}, undurable{0}, overloaded{0}, overflow{0};
  std::atomic<uint64_t> ckpt_phase{0};  // 0 idle, 1 walking, 2 walked, 3 fdatasync
  // CLOCK_MONOTONIC ns of the latest checkpoint's phases.
  std::atomic<uint64_t> ph_begin{0}, ph_walked{0}, ph_sync{0}, ph_done{0};
};

inline constexpr unsigned kMaxShards = 64;
inline ShardGauge gauges[kMaxShards];
inline std::atomic<bool> enabled{false};

inline uint64_t mono_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

inline void mark_phase(unsigned shard, uint64_t phase) {
  if (!enabled.load(std::memory_order_relaxed) || shard >= kMaxShards) return;
  ShardGauge& g = gauges[shard];
  const uint64_t now = mono_ns();
  switch (phase) {
    case 1: g.ph_begin.store(now, std::memory_order_relaxed); break;
    case 2: g.ph_walked.store(now, std::memory_order_relaxed); break;
    case 3: g.ph_sync.store(now, std::memory_order_relaxed); break;
    default: g.ph_done.store(now, std::memory_order_relaxed); phase = 0; break;
  }
  g.ckpt_phase.store(phase, std::memory_order_relaxed);
}

}  // namespace snc::diag
