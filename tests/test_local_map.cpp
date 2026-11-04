#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "check.h"
#include "snc/local_map.h"

using namespace snc;

int main() {
  // Randomized comparison against std::map, with growth and backward-shift
  // deletion exercised heavily.
  const int n = 200000;
  std::vector<ObjectHeader> objs(n);
  for (int i = 0; i < n; ++i) objs[i].id = ObjectID::make(1, i + 1);

  LocalMap map(16);
  std::map<std::pair<u64, u64>, ObjectHeader*> ref;
  std::mt19937_64 rng(42);
  for (int step = 0; step < 1000000; ++step) {
    ObjectHeader* o = &objs[rng() % n];
    auto key = std::make_pair(o->id.hi, o->id.lo);
    switch (rng() % 3) {
      case 0:
      case 1:
        map.upsert(o);
        ref[key] = o;
        break;
      case 2:
        CHECK_EQ(map.erase(o->id), (ref.count(key) ? ref[key] : nullptr));
        ref.erase(key);
        break;
    }
  }
  CHECK_EQ(map.size(), ref.size());
  for (auto& o : objs) {
    auto key = std::make_pair(o.id.hi, o.id.lo);
    CHECK_EQ(map.find(o.id), (ref.count(key) ? &o : nullptr));
  }
  std::size_t visited = 0;
  map.for_each([&](ObjectHeader*) { ++visited; });
  CHECK_EQ(visited, ref.size());
  CHECK(map.size() * 5 <= map.capacity() * 4);

  // Erase everything; table must be empty and still usable.
  for (auto& o : objs) map.erase(o.id);
  CHECK_EQ(map.size(), 0u);
  map.upsert(&objs[0]);
  CHECK_EQ(map.find(objs[0].id), &objs[0]);

  // The bounded walk (docs/MIGRATION_STAGING.md 3.4, test L): steps of
  // random length, each ending just after an empty bucket, with random
  // inserts and erases of other entries between steps (and growth). Every
  // entry present for the whole walk is visited exactly once; a growth is
  // always seen through generation() and the walk restarted.
  {
    std::mt19937_64 r(7);
    const int pool = 60000;
    std::vector<ObjectHeader> all(pool);
    for (int i = 0; i < pool; ++i) all[i].id = ObjectID::make(2, i + 1);
    u64 walks = 0, restarts = 0, checked = 0;
    for (int round = 0; round < 400; ++round) {
      LocalMap m(16);
      std::set<int> in;
      const int start = 200 + static_cast<int>(r() % 4000);
      for (int k = 0; k < start; ++k) {
        const int i = static_cast<int>(r() % pool);
        m.upsert(&all[i]);
        in.insert(i);
      }
      const bool allow_growth = round % 4 == 0;
      LocalMap::Walk w;
      bool restart = true;
      std::set<int> present_all;          // present at the start and never removed
      std::set<int> touched;              // inserted or erased during the walk
      std::unordered_map<const ObjectHeader*, int> seen;
      while (true) {
        if (restart) {
          m.walk_begin(w);
          present_all = in;
          touched.clear();
          seen.clear();
          restart = false;
          ++walks;
        }
        int budget = 1 + static_cast<int>(r() % 64);
        const bool done = m.walk_step(w, [&](const ObjectHeader* o) { ++seen[o]; }, [&] { return --budget <= 0; });
        if (done) break;
        // Churn between steps: erase or insert other entries.
        const int ops = static_cast<int>(r() % 40);
        for (int k = 0; k < ops; ++k) {
          const int i = static_cast<int>(r() % pool);
          if (in.count(i) && r() % 2) {
            m.erase(all[i].id);
            in.erase(i);
            touched.insert(i);
          } else if (!in.count(i) && (allow_growth || (m.size() + 2) * 5 <= m.capacity() * 4)) {
            m.upsert(&all[i]);
            in.insert(i);
            touched.insert(i);
          }
        }
        if (m.generation() != w.gen) {
          restart = true;
          ++restarts;
        }
      }
      for (int i : present_all) {
        if (touched.count(i)) continue;
        CHECK_EQ(seen[&all[i]], 1);
        ++checked;
      }
      for (const auto& [o, c] : seen) CHECK(c <= 1 || touched.count(static_cast<int>(o - all.data())));
    }
    CHECK(restarts > 0);
    std::printf("test_local_map: walk: %llu walks, %llu growth restarts, %llu entries checked exactly once\n",
                (unsigned long long)walks, (unsigned long long)restarts, (unsigned long long)checked);
  }

  std::printf("test_local_map: ok\n");
  return 0;
}
