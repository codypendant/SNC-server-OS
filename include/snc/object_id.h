// ObjectID (spec section 1): 8-bit format + 56-bit mint lease + 64-bit sequence.
// Unique forever, never reused, carries no location. Placement uses only
// snc_hash64(id); nothing may route on the lease or sequence fields.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "snc/types.h"

namespace snc {

inline constexpr u8 kObjectIdFormat = 0x01;
inline constexpr u64 kLeaseMask = (u64{1} << 56) - 1;

struct ObjectID {
  u64 hi = 0;  // bits 63..56: format; bits 55..0: mint lease
  u64 lo = 0;  // sequence within the lease, starts at 1

  static constexpr ObjectID make(u64 lease, u64 seq) {
    return ObjectID{(u64{kObjectIdFormat} << 56) | (lease & kLeaseMask), seq};
  }

  constexpr bool is_null() const { return hi == 0 && lo == 0; }
  constexpr u8 format() const { return static_cast<u8>(hi >> 56); }
  constexpr u64 lease() const { return hi & kLeaseMask; }
  constexpr u64 seq() const { return lo; }

  friend constexpr bool operator==(const ObjectID& a, const ObjectID& b) {
    return a.hi == b.hi && a.lo == b.lo;
  }
  // Ordering exists for sorting and determinism only; it is not creation order.
  friend constexpr bool operator<(const ObjectID& a, const ObjectID& b) {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
  }

  // Text form: 32 lowercase hex digits, hi then lo, one dash.
  std::string to_string() const;
  static std::optional<ObjectID> parse(std::string_view text);
};

static_assert(sizeof(ObjectID) == 16 && alignof(ObjectID) == 8);

inline constexpr ObjectID kNullId{};

// Frozen as part of the format: XXH3-64 over the 16 ID bytes, seed 0.
u64 snc_hash64(const ObjectID& id);

// Hands out IDs from one lease. Owned by a single shard; not thread-safe.
class Minter {
 public:
  explicit Minter(u64 lease) : lease_(lease) {}
  ObjectID next() { return ObjectID::make(lease_, ++seq_); }
  u64 lease() const { return lease_; }
  u64 minted() const { return seq_; }

 private:
  u64 lease_;
  u64 seq_ = 0;
};

}  // namespace snc
