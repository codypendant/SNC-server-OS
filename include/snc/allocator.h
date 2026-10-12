// ShardAllocator (spec section 10): one per shard, created on the pinned
// worker thread so first-touch keeps its pages NUMA-local. Backed by a
// mimalloc first-class heap. Only the owning shard frees what it allocated.
#pragma once

#include "snc/types.h"

typedef struct mi_heap_s mi_heap_t;

namespace snc {

struct AllocStats {
  u64 allocs = 0;
  u64 frees = 0;
  u64 resizes = 0;
  u64 live_bytes = 0;   // usable bytes currently allocated
  u64 limit_bytes = 0;  // 0 = unlimited
};

class ShardAllocator {
 public:
  explicit ShardAllocator(u64 limit_bytes = 0);
  ~ShardAllocator();
  ShardAllocator(const ShardAllocator&) = delete;
  ShardAllocator& operator=(const ShardAllocator&) = delete;

  // Returns nullptr when the shard limit would be exceeded.
  void* alloc(std::size_t bytes, std::size_t align);
  // May move. Returns nullptr (old block untouched) when over the limit.
  void* resize(void* p, std::size_t new_bytes, std::size_t align);
  void free(void* p);
  std::size_t usable(const void* p) const;
  // True if allocating `bytes` more would stay within the limit.
  bool can_reserve(std::size_t bytes) const;
  const AllocStats& stats() const { return stats_; }

 private:
  mi_heap_t* heap_;
  AllocStats stats_;
};

}  // namespace snc
