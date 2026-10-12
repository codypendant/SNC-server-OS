#include <map>
#include <random>
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

  std::printf("test_local_map: ok\n");
  return 0;
}
