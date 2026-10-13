#include "slot_state.h"

#include <cstring>
#include <mutex>

namespace snc {

namespace {

Status refuse(OpCtx&, const ObjectHeader&, ByteView) { return Status::E_UNKNOWN_OP; }

bool check(const u8* p, u32 len) {
  if (len != sizeof(SlotStateV1)) return false;
  SlotStateV1 s;
  std::memcpy(&s, p, sizeof(s));
  return s.slot < 4096 && s.phase <= static_cast<u8>(SlotPhase::STAGED);
}

u32 extract(const u8* payload, u32 len, u8* key) {
  if (len < sizeof(u16)) return 0;
  std::memcpy(key, payload, sizeof(u16));  // slot
  return sizeof(u16);
}

}  // namespace

void register_slot_state_type() {
  static std::once_flag once;
  std::call_once(once, [] {
    Registry& r = registry();
    TypeDescriptor t;
    t.type_id = kSlotStateTypeId;
    t.name = "System.SlotState";
    t.initial_size = sizeof(SlotStateV1);
    t.max_size = sizeof(SlotStateV1);
    t.check = check;
    t.create_op = kSlotStateCreateOp;
    r.add_system_type(t);
    OpDescriptor op;
    op.op_id = kSlotStateCreateOp;
    op.type_id = kSlotStateTypeId;
    op.name = "System.SlotState.create";
    op.kind = OpKind::CREATE;
    op.validate = refuse;  // written by the runtime only
    r.add_op(op);
    IndexDescriptor idx;
    idx.index_id = kSlotStateIndexId;
    idx.type_id = kSlotStateTypeId;
    idx.name = "System.SlotState.slot";
    idx.extract = extract;
    r.add_index(idx);
  });
}

}  // namespace snc
