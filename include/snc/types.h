// Basic fixed-width types and status codes (spec: Conventions, section 6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace snc {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

static_assert(sizeof(void*) == 8, "SNC v0.1 targets 64-bit platforms only");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "SNC v0.1 is little-endian only");

// v0.1 limits (spec: Conventions). Named constants, not format fields.
inline constexpr u32 kMaxShards = 256;
inline constexpr u32 kMaxPayload = 1u << 20;   // 1 MiB
inline constexpr u32 kMaxArgs = 64u << 10;     // 64 KiB
inline constexpr u32 kMaxResult = 64u << 10;   // 64 KiB
inline constexpr u32 kMaxIndexKey = 256;

enum class Status : u16 {
  OK = 0,
  E_NOT_FOUND,
  E_TYPE_MISMATCH,
  E_UNKNOWN_OP,
  E_BAD_ARGS,
  E_VERSION_CONFLICT,
  E_APP,            // validate rejected; app code travels separately
  E_TOO_LARGE,
  E_NO_MEMORY,
  E_OVERLOADED,
  E_SHUTTING_DOWN,
  E_STANDBY,        // this engine is a replication standby and serves no ops
  E_STALE,          // a standby serving reads has no current lease from its primary
  E_REPLAYED_OLD,   // sequenced: an older request, already decided; its outcome is not kept
  E_NOT_OWNER,      // sequenced: sent to a shard that does not own the target
  E_WRONG_NODE,     // clustered: another node owns the target (docs/CLUSTER.md)
};

const char* status_name(Status s);

// Non-owning view of bytes.
struct ByteView {
  const u8* data = nullptr;
  u32 len = 0;

  ByteView() = default;
  ByteView(const void* d, u32 n) : data(static_cast<const u8*>(d)), len(n) {}
};

}  // namespace snc
