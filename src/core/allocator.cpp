#include "snc/allocator.h"

#include <mimalloc.h>

#include <cstdlib>
#include <cstring>

namespace snc {

namespace {
// mimalloc arenas hand out whole segments; keep the arena aligned to them.
constexpr std::size_t kArenaAlign = 64u << 20;
}  // namespace

ShardAllocator::ShardAllocator(u64 limit_bytes, PageMode pages, u64 reserve_bytes) {
  stats_.limit_bytes = limit_bytes;
  heap_ = nullptr;
  if (pages != PageMode::SMALL && reserve_bytes > 0) {
    const std::size_t bytes = (reserve_bytes + kArenaAlign - 1) & ~(kArenaAlign - 1);
    PageMode got;
    // Called on the pinned worker thread; pages are touched there first.
    void* p = map_pages(bytes, pages, kArenaAlign, &got);
    mi_arena_id_t arena;
    if (mi_manage_os_memory_ex(p, bytes, /*is_committed=*/true, /*is_large=*/got == PageMode::HUGETLB,
                               /*is_zero=*/true, /*numa_node=*/-1, /*exclusive=*/true, &arena)) {
      heap_ = mi_heap_new_in_arena(arena);
      spill_ = mi_heap_new();
      stats_.arena_bytes = bytes;
      stats_.arena_pages = got;
    } else {
      unmap_pages(p, bytes, got);  // mimalloc refused it: plain heap below
    }
    // mimalloc keeps the arena registered for the life of the process, so
    // its memory is never unmapped. One Engine per process (snc.h).
  }
  if (heap_ == nullptr) heap_ = mi_heap_new();
  if (heap_ == nullptr) std::abort();
}

ShardAllocator::~ShardAllocator() {
  // Frees everything still allocated from these heaps.
  mi_heap_destroy(heap_);
  if (spill_) mi_heap_destroy(spill_);
}

bool ShardAllocator::can_reserve(std::size_t bytes) const {
  return stats_.limit_bytes == 0 || stats_.live_bytes + bytes <= stats_.limit_bytes;
}

void* ShardAllocator::alloc(std::size_t bytes, std::size_t align) {
  if (!can_reserve(bytes)) return nullptr;
  void* p = mi_heap_malloc_aligned(heap_, bytes, align);
  if (p == nullptr && spill_) {
    ++stats_.arena_misses;
    p = mi_heap_malloc_aligned(spill_, bytes, align);
  }
  if (p == nullptr) return nullptr;
  ++stats_.allocs;
  stats_.live_bytes += mi_usable_size(p);
  return p;
}

void* ShardAllocator::resize(void* p, std::size_t new_bytes, std::size_t align) {
  std::size_t old_usable = mi_usable_size(p);
  if (new_bytes > old_usable && !can_reserve(new_bytes - old_usable)) return nullptr;
  void* q = mi_heap_realloc_aligned(heap_, p, new_bytes, align);
  if (q == nullptr && spill_) {
    // Arena full: move the block to the normal heap by hand.
    ++stats_.arena_misses;
    q = mi_heap_malloc_aligned(spill_, new_bytes, align);
    if (q == nullptr) return nullptr;
    std::memcpy(q, p, old_usable < new_bytes ? old_usable : new_bytes);
    mi_free(p);
  }
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
