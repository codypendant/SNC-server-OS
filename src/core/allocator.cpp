#include "snc/allocator.h"

#include <mimalloc.h>

#include <cstdlib>

namespace snc {

ShardAllocator::ShardAllocator(u64 limit_bytes) : heap_(mi_heap_new()) {
  if (heap_ == nullptr) std::abort();
  stats_.limit_bytes = limit_bytes;
}

ShardAllocator::~ShardAllocator() {
  // Frees everything still allocated from this heap.
  mi_heap_destroy(heap_);
}

bool ShardAllocator::can_reserve(std::size_t bytes) const {
  return stats_.limit_bytes == 0 || stats_.live_bytes + bytes <= stats_.limit_bytes;
}

void* ShardAllocator::alloc(std::size_t bytes, std::size_t align) {
  if (!can_reserve(bytes)) return nullptr;
  void* p = mi_heap_malloc_aligned(heap_, bytes, align);
  if (p == nullptr) return nullptr;
  ++stats_.allocs;
  stats_.live_bytes += mi_usable_size(p);
  return p;
}

void* ShardAllocator::resize(void* p, std::size_t new_bytes, std::size_t align) {
  std::size_t old_usable = mi_usable_size(p);
  if (new_bytes > old_usable && !can_reserve(new_bytes - old_usable)) return nullptr;
  void* q = mi_heap_realloc_aligned(heap_, p, new_bytes, align);
  if (q == nullptr) return nullptr;
  ++stats_.resizes;
  stats_.live_bytes = stats_.live_bytes - old_usable + mi_usable_size(q);
  return q;
}

void ShardAllocator::free(void* p) {
  if (p == nullptr) return;
  ++stats_.frees;
  stats_.live_bytes -= mi_usable_size(p);
  mi_free(p);
}

std::size_t ShardAllocator::usable(const void* p) const { return mi_usable_size(p); }

}  // namespace snc
