// Client sessions for safe retries (docs/RETRIES.md). A session is a small
// system object per (client, shard) holding the client's last sequence
// number applied there; it is logged in the same atomic WAL group as the
// op it records, so a standby holds a write exactly when it holds the
// session's record of it.
#pragma once

#include "snc/registry.h"
#include "snc/types.h"

namespace snc {

inline constexpr u32 kSessionTypeId = 1;           // reserved range
inline constexpr OpID kSessionCreateOp = 1;        // never run by clients
inline constexpr u32 kSessionIndexId = 0x80000001;  // client_id -> session

struct SessionV1 {
  u64 client_id;
  u64 last_seq;       // the client's last sequenced op applied on this shard
  ObjectID last_id;   // its target (or the created object)
  u64 last_version;   // the target's version after it
  u16 last_status;
  u16 last_app_code;
  u32 touched_s;      // wall clock, seconds: sessions idle past a TTL expire
};
static_assert(sizeof(SessionV1) == 48);

// Registers the Session type and index once per process.
void register_session_type();

}  // namespace snc
