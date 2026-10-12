// Object header and layout (spec section 2). An object is one allocation:
// a 48-byte header followed by its payload at offset 48. Payload bytes are
// plain data, relocatable, and identical in memory, WAL and checkpoints.
#pragma once

#include <cstddef>

#include "snc/object_id.h"

namespace snc {

enum ObjectFlags : u16 {
  kFlagDeleted = 1u << 0,  // tombstone waiting for its delete record to be durable
  kFlagDirty = 1u << 1,    // reserved for incremental checkpoints, unused in v0.1
  kFlagIndexed = 1u << 2,  // has an entry in the type's secondary index
};

struct ObjectHeader {
  ObjectID id;     //  0  identity
  u64 version;     // 16  1 at create, +1 per successful mutation
  u64 last_lsn;    // 24  WAL LSN that produced this version (runtime only)
  u32 type_id;     // 32  registered TypeID, never 0
  u32 length;      // 36  payload bytes in use
  u32 capacity;    // 40  payload bytes available without reallocating
  u16 flags;       // 44
  u16 schema_ver;  // 46

  u8* payload() { return reinterpret_cast<u8*>(this) + sizeof(ObjectHeader); }
  const u8* payload() const { return reinterpret_cast<const u8*>(this) + sizeof(ObjectHeader); }
  bool deleted() const { return flags & kFlagDeleted; }
};

static_assert(sizeof(ObjectHeader) == 48);
static_assert(offsetof(ObjectHeader, version) == 16);
static_assert(offsetof(ObjectHeader, last_lsn) == 24);
static_assert(offsetof(ObjectHeader, type_id) == 32);
static_assert(offsetof(ObjectHeader, length) == 36);
static_assert(offsetof(ObjectHeader, capacity) == 40);
static_assert(offsetof(ObjectHeader, flags) == 44);
static_assert(offsetof(ObjectHeader, schema_ver) == 46);

// Payload starts at offset 48; objects are allocated 16-aligned so the
// payload is 16-aligned too.
inline constexpr std::size_t kObjectAlign = 16;

}  // namespace snc
