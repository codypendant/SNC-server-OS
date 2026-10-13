// Huge-page memory (docs/HUGEPAGES.md): map_pages, the local map on huge
// pages, and the shard heap's arena spilling to normal pages when full.
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "snc/allocator.h"
#include "snc/local_map.h"

using namespace snc;

namespace {

void test_map_pages(PageMode mode) {
  PageMode got;
  void* p = map_pages(3u << 20, mode, 64u << 20, &got);
  CHECK(reinterpret_cast<std::uintptr_t>(p) % (64u << 20) == 0);
  // HUGETLB falls back to THP when no reserved pages are free.
  CHECK(got == mode || (mode == PageMode::HUGETLB && got == PageMode::THP));
  const auto* b = static_cast<const unsigned char*>(p);
  for (std::size_t i = 0; i < page_round(3u << 20, got); i += 4096) CHECK_EQ(b[i], 0);
  std::memset(p, 0xab, 3u << 20);
  unmap_pages(p, 3u << 20, got);
}

void test_map(PageMode mode) {
  const int n = 300000;  // table grows past 2 MiB, onto huge pages
  std::vector<ObjectHeader> objs(n);
  for (int i = 0; i < n; ++i) objs[i].id = ObjectID::make(1, i + 1);
  LocalMap map(16, mode);
  CHECK(map.page_mode() == PageMode::SMALL);  // small tables stay on 4 KiB pages
  for (auto& o : objs) map.upsert(&o);
  CHECK(map.memory_bytes() >= (2u << 20));
  if (mode == PageMode::SMALL) CHECK(map.page_mode() == PageMode::SMALL);
  else CHECK(map.page_mode() != PageMode::SMALL);
  for (auto& o : objs) CHECK_EQ(map.find(o.id), &o);
  for (int i = 0; i < n; i += 2) CHECK_EQ(map.erase(objs[i].id), &objs[i]);
  for (int i = 0; i < n; ++i) CHECK_EQ(map.find(objs[i].id), (i % 2) ? &objs[i] : nullptr);
}

void test_arena_spill(PageMode mode) {
  // A 64 MiB arena; allocate well past it.
  ShardAllocator a(0, mode, 64u << 20);
  CHECK(a.stats().arena_bytes == (64u << 20));
  CHECK(a.stats().arena_pages != PageMode::SMALL);
  std::vector<void*> blocks;
  for (int i = 0; i < 120000; ++i) {
    void* p = a.alloc(1000, 16);
    CHECK(p != nullptr);
    std::memset(p, i & 0xff, 1000);
    blocks.push_back(p);
  }
  CHECK(a.stats().arena_misses > 0);
  // Grow a few blocks: arena-resident ones must move to the spill heap intact.
  for (int i = 0; i < 1000; ++i) {
    void* q = a.resize(blocks[i], 4000, 16);
    CHECK(q != nullptr);
    const auto* b = static_cast<const unsigned char*>(q);
    for (int j = 0; j < 1000; ++j) CHECK_EQ(b[j], static_cast<unsigned char>(i & 0xff));
    blocks[i] = q;
  }
  for (void* p : blocks) a.free(p);
  CHECK_EQ(a.stats().live_bytes, 0u);
}

}  // namespace

int main() {
  for (PageMode m : {PageMode::SMALL, PageMode::THP, PageMode::HUGETLB}) test_map_pages(m);
  for (PageMode m : {PageMode::SMALL, PageMode::THP, PageMode::HUGETLB}) test_map(m);
  test_arena_spill(PageMode::THP);
  std::printf("test_pages: ok\n");
  return 0;
}
