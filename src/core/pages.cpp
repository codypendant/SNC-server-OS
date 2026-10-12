#include "snc/pages.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>

namespace snc {

namespace {
constexpr std::size_t kHuge = 2u << 20;
}

const char* page_mode_name(PageMode m) {
  switch (m) {
    case PageMode::SMALL: return "4k";
    case PageMode::THP: return "thp";
    case PageMode::HUGETLB: return "hugetlb";
  }
  return "?";
}

std::size_t page_round(std::size_t bytes, PageMode mode) {
  const std::size_t unit = mode == PageMode::SMALL ? 4096 : kHuge;
  return (bytes + unit - 1) & ~(unit - 1);
}

void* map_pages(std::size_t bytes, PageMode mode, std::size_t align, PageMode* got) {
  bytes = page_round(bytes, mode);
  if (mode != PageMode::SMALL && align < kHuge) align = kHuge;
  if (align < 4096) align = 4096;
  // Reserve address space with room to align, then place the pages inside.
  const std::size_t span = bytes + align;
  void* r = ::mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (r == MAP_FAILED) {
    std::fprintf(stderr, "snc: cannot reserve %zu bytes of address space\n", span);
    std::abort();
  }
  const auto base = reinterpret_cast<std::uintptr_t>(r);
  const std::uintptr_t start = (base + align - 1) & ~(std::uintptr_t{align} - 1);
  void* p = reinterpret_cast<void*>(start);
  // Trim the unused head and tail of the reservation.
  if (start > base) ::munmap(r, start - base);
  const std::uintptr_t end = start + bytes;
  if (base + span > end) ::munmap(reinterpret_cast<void*>(end), base + span - end);

  if (mode == PageMode::HUGETLB) {
    void* q = ::mmap(p, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_HUGETLB, -1, 0);
    if (q != MAP_FAILED) {
      *got = PageMode::HUGETLB;
      return q;
    }
    mode = PageMode::THP;  // no reserved pages free: the range is still PROT_NONE
  }
  void* q = ::mmap(p, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (q == MAP_FAILED) {
    std::fprintf(stderr, "snc: cannot map %zu bytes\n", bytes);
    std::abort();
  }
  if (mode == PageMode::THP) ::madvise(q, bytes, MADV_HUGEPAGE);
  *got = mode;
  return q;
}

void unmap_pages(void* p, std::size_t bytes, PageMode mode) {
  if (p != nullptr) ::munmap(p, page_round(bytes, mode));
}

}  // namespace snc
