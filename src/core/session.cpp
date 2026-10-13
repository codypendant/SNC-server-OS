#include "session.h"

#include <cstring>
#include <mutex>

namespace snc {

namespace {

Status refuse(OpCtx&, const ObjectHeader&, ByteView) { return Status::E_UNKNOWN_OP; }

bool check(const u8*, u32 len) { return len == sizeof(SessionV1); }

u32 extract(const u8* payload, u32 len, u8* key) {
  if (len < sizeof(u64)) return 0;
  std::memcpy(key, payload, sizeof(u64));  // client_id
  return sizeof(u64);
}

}  // namespace

void register_session_type() {
  static std::once_flag once;
  std::call_once(once, [] {
    Registry& r = registry();
    TypeDescriptor t;
    t.type_id = kSessionTypeId;
    t.name = "System.Session";
    t.initial_size = sizeof(SessionV1);
    t.max_size = sizeof(SessionV1);
    t.check = check;
    t.create_op = kSessionCreateOp;
    r.add_system_type(t);
    OpDescriptor op;
    op.op_id = kSessionCreateOp;
    op.type_id = kSessionTypeId;
    op.name = "System.Session.create";
    op.kind = OpKind::CREATE;
    op.validate = refuse;  // sessions are made by the runtime only
    r.add_op(op);
    IndexDescriptor idx;
    idx.index_id = kSessionIndexId;
    idx.type_id = kSessionTypeId;
    idx.name = "System.Session.client_id";
    idx.extract = extract;
    r.add_index(idx);
  });
}

}  // namespace snc
