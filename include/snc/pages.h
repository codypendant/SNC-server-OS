// Page size for the shard's hot memory (spec section 10, docs/HUGEPAGES.md):
// the local object map and the object heap. The rest of the process uses
// normal pages.
#pragma once

#include "snc/types.h"

namespace snc {

enum class PageMode : u8 {
  SMALL = 0,    // 4 KiB pages
  THP = 1,      // transparent huge pages: madvise(MADV_HUGEPAGE), 2 MiB when the kernel can
  HUGETLB = 2,  // reserved 2 MiB pages (vm.nr_hugepages); falls back to THP if none are free
};

const char* page_mode_name(PageMode m);

// Anonymous zeroed memory, `align`-aligned (a power of two, at least 2 MiB
// unless mode is SMALL). `bytes` is rounded up to 2 MiB unless mode is
// SMALL. Sets *got to the mode actually used. Aborts if no memory.
void* map_pages(std::size_t bytes, PageMode mode, std::size_t align, PageMode* got);
void unmap_pages(void* p, std::size_t bytes, PageMode mode);
// Size map_pages actually maps for `bytes`.
std::size_t page_round(std::size_t bytes, PageMode mode);

}  // namespace snc
