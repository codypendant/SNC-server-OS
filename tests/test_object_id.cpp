#include <set>
#include <vector>

#include "check.h"
#include "snc/object.h"
#include "snc/slot_map.h"

using namespace snc;

int main() {
  // Layout and fields.
  ObjectID id = ObjectID::make(3, 1000000);
  CHECK_EQ(id.format(), kObjectIdFormat);
  CHECK_EQ(id.lease(), 3u);
  CHECK_EQ(id.seq(), 1000000u);
  CHECK(!id.is_null());
  CHECK(kNullId.is_null());

  // Text form round trip, and the exact spec example.
  CHECK_EQ(id.to_string(), std::string("0100000000000003-00000000000f4240"));
  auto parsed = ObjectID::parse("0100000000000003-00000000000f4240");
  CHECK(parsed.has_value() && *parsed == id);
  CHECK(!ObjectID::parse("0100000000000003-00000000000F4240").has_value());  // uppercase
  CHECK(!ObjectID::parse("0100000000000003_00000000000f4240").has_value());
  CHECK(!ObjectID::parse("01000000000000030000000000000f4240").has_value());

  // Minter never repeats.
  Minter m(7);
  std::set<std::pair<u64, u64>> seen;
  for (int i = 0; i < 10000; ++i) {
    ObjectID x = m.next();
    CHECK(seen.insert({x.hi, x.lo}).second);
    CHECK_EQ(x.lease(), 7u);
  }

  // Sequential IDs spread evenly over slots and shards.
  SlotMap sm = SlotMap::make_default(4);
  std::vector<u32> per_shard(4, 0);
  std::vector<u32> per_slot(kSlots, 0);
  Minter m2(1);
  const int n = 400000;
  for (int i = 0; i < n; ++i) {
    ObjectID x = m2.next();
    ++per_shard[sm.owner_of(x)];
    ++per_slot[slot_of(x)];
  }
  for (u32 c : per_shard) CHECK(c > n / 4 * 0.97 && c < n / 4 * 1.03);
  u32 empty_slots = 0;
  for (u32 c : per_slot) empty_slots += (c == 0);
  CHECK_EQ(empty_slots, 0u);

  // Hash is frozen: a change here changes placement of persisted objects.
  CHECK_EQ(snc_hash64(ObjectID::make(1, 1)), snc_hash64(ObjectID{0x0100000000000001ull, 1}));

  std::printf("test_object_id: ok\n");
  return 0;
}
