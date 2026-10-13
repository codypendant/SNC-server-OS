// SNC wire protocol v0 (docs/ROUTING.md): length-prefixed binary frames over
// ordinary TCP, one listening port per shard (base_port + shard). Frames are
// little-endian, like every SNC format. A client may send any request to any
// shard; the shard forwards it to the owner when needed, so routing is a
// performance hint and never a correctness requirement.
#pragma once

#include "snc/object_id.h"
#include "snc/slot_map.h"

namespace snc::wire {

enum class Kind : u8 {
  EXEC = 1,      // execute(target, op, args)
  CREATE = 2,    // create(type = op, args); a non-null target asks for create_near(target)
  DELETE = 3,    // remove(target)
  TOPOLOGY = 4,  // returns a TopologyBody: slot map and shard count
  CLUSTER = 5,   // returns a ClusterBody (docs/CLUSTER.md); n_nodes 0 when not clustered
  RESP = 16,
};

enum ReqFlags : u8 {
  kReqAllowUndurable = 1u << 0,
  kReqApplied = 1u << 1,  // Durability::APPLIED
  // Safe retries (docs/RETRIES.md): the body starts with a SeqPrefix.
  kReqSequenced = 1u << 2,
};
struct SeqPrefix {
  u64 client_id;
  u64 seq;
};

enum RespFlags : u8 {
  kRespDurable = 1u << 0,
  kRespForwarded = 1u << 1,  // the target is owned by another shard: re-route
  kRespReplayed = 1u << 2,   // a sequenced request already applied: answered from its session
};

// Every frame starts with its total length (header + body) in bytes.
struct ReqHeader {
  u32 len;
  Kind kind;
  u8 flags;
  u16 reserved0;
  u32 op;  // op id, or type id for CREATE
  u32 reserved1;
  u64 req_id;  // chosen by the client, echoed in the response
  ObjectID target;
  u64 expected_version;
};
static_assert(sizeof(ReqHeader) == 48);

struct RespHeader {
  u32 len;
  Kind kind;  // RESP
  u8 flags;
  u16 status;
  u16 app_code;
  u16 owner;      // shard that owns `id` under map_epoch
  u32 map_epoch;  // slot map the server routed with
  u64 req_id;
  ObjectID id;  // target, or the new ID for CREATE
  u64 version;
};
static_assert(sizeof(RespHeader) == 48);

// Body of a TOPOLOGY response.
struct TopologyBody {
  u32 map_epoch;
  u16 n_shards;
  u16 reserved;
  u16 owner[kSlots];
};

// Body of a CLUSTER response: the header, then n_nodes ClusterEntry.
struct ClusterBody {
  u32 crc;
  u16 n_nodes;
  u16 node_id;  // the node answering
  u8 node_of[kSlots];
};
struct ClusterEntry {
  char host[64];  // NUL-terminated
  u16 client_port;
  u16 n_shards;
  u32 reserved;
};
static_assert(sizeof(ClusterEntry) == 72);

inline constexpr u32 kMaxFrame = sizeof(ReqHeader) + kMaxArgs;

}  // namespace snc::wire
