// Cross-shard message (spec section 7): fixed 128-byte slots on SPSC rings.
#pragma once

#include "snc/object_id.h"

namespace snc {

enum class MsgKind : u8 {
  EXEC_REQ = 1,
  CREATE_REQ = 2,
  DELETE_REQ = 3,
  RESP = 4,
  INDEX_QUERY = 5,  // reserved for step 9
  INDEX_RESP = 6,   // reserved for step 9
  BUF_RETURN = 7,
  CTRL = 8,
  // One-way follow-up (OpCtx::emit): no response. req_id and
  // expected_version carry the emitting object's ID, status and aux the
  // fail_op (low, high 16 bits); args are always inline.
  EMIT = 9,
};

enum MsgFlags : u8 {
  kMsgArgsInline = 1u << 0,
  kMsgArgsExternal = 1u << 1,
  kMsgAllowUndurable = 1u << 2,
  kMsgDurApplied = 1u << 3,
};

inline constexpr u32 kMsgInlineBytes = 16;

struct alignas(64) Msg {
  MsgKind kind;          //  0
  u8 flags;              //  1
  u16 src_shard;         //  2
  u32 op_id;             //  4  or type_id for CREATE_REQ
  u64 req_id;            //  8  unique per source shard
  ObjectID target;       // 16
  u64 expected_version;  // 32  or, in RESP: resulting version
  u32 len;               // 40  args (REQ) or result (RESP) length
  u16 status;            // 44  RESP only
  u16 aux;               // 46  RESP: app code from validate
  union {                // 48
    u8 inline_data[kMsgInlineBytes];
    struct {
      u8* ptr;  // buffer in the sender's heap; returned with BUF_RETURN
    } ext;
  };

  const u8* data() const { return (flags & kMsgArgsExternal) ? ext.ptr : inline_data; }
};

static_assert(sizeof(Msg) == 64, "Msg must be exactly one cache line");
static_assert(offsetof(Msg, inline_data) == 48);

}  // namespace snc
