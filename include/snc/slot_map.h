// Slot map (spec section 3): snc_hash64(id) picks one of 4,096 slots; the
// slot map assigns slots to shards. Immutable while running; each shard keeps
// its own copy in local memory.
#pragma once

#include <array>

#include "snc/object_id.h"

namespace snc {

inline constexpr u32 kSlots = 4096;  // format constant

inline u16 slot_of(const ObjectID& id) { return static_cast<u16>(snc_hash64(id) >> 52); }

struct SlotMap {
  u32 epoch = 0;  // +1 whenever the assignment changes (only at boot in v0.1)
  u16 n_shards = 0;
  std::array<u16, kSlots> owner{};

  // Default assignment: owner[s] = s % n_shards.
  static SlotMap make_default(u16 n_shards, u32 epoch = 1) {
    SlotMap m;
    m.epoch = epoch;
    m.n_shards = n_shards;
    for (u32 s = 0; s < kSlots; ++s) m.owner[s] = static_cast<u16>(s % n_shards);
    return m;
  }

  u16 owner_of(const ObjectID& id) const { return owner[slot_of(id)]; }
};

}  // namespace snc
