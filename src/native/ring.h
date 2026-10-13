// SPSC ring of Msg slots, shared by the runtime backends (native, and the
// Seastar backend's rings mode). Private to src/.
#pragma once

#include <atomic>
#include <cstdlib>

#include "snc/native_runtime.h"

namespace snc {

// Single-producer, single-consumer ring. The producer batches writes and
// publishes its tail once per loop iteration; the consumer advances head
// once per drain. Head and tail live on separate cache lines.
struct Ring {
  alignas(64) std::atomic<u32> tail{0};  // published by producer
  alignas(64) std::atomic<u32> head{0};  // published by consumer
  alignas(64) u32 mask = 0;
  Msg* slots = nullptr;
  // Producer-private.
  alignas(64) u32 p_tail = 0;
  u32 p_published = 0;
  bool p_unwoken = false;  // published early (sri_publish): wake check still due
  u32 p_head_cache = 0;
  RingStats stats;
  // Consumer-private.
  alignas(64) u32 c_head = 0;

  explicit Ring(u32 n_slots) : mask(n_slots - 1) {
    slots = static_cast<Msg*>(std::aligned_alloc(64, sizeof(Msg) * n_slots));
    if (slots == nullptr) std::abort();
  }
  ~Ring() { std::free(slots); }

  bool try_push(const Msg& m) {
    if (p_tail - p_head_cache == mask + 1) {
      p_head_cache = head.load(std::memory_order_acquire);
      if (p_tail - p_head_cache == mask + 1) return false;
    }
    slots[p_tail & mask] = m;
    ++p_tail;
    return true;
  }

  // Returns true if new messages became visible to the consumer.
  bool publish() {
    if (p_tail == p_published) return false;
    tail.store(p_tail, std::memory_order_release);
    p_published = p_tail;
    return true;
  }

  bool has_pending_for_consumer() const {
    return tail.load(std::memory_order_acquire) != c_head;
  }
};

}  // namespace snc
