#include "snc/registry.h"

#include <cstdio>
#include <cstdlib>

namespace snc {

namespace {
[[noreturn]] void die(const char* what, u32 id) {
  std::fprintf(stderr, "snc registry: %s (%u)\n", what, id);
  std::abort();
}
constexpr u32 kMaxOpId = 1u << 16;
}  // namespace

void Registry::add_type(const TypeDescriptor& t) {
  if (t.type_id < kFirstUserTypeId) die("type id in reserved range", t.type_id);
  if (type(t.type_id)) die("duplicate type id", t.type_id);
  if (t.max_size == 0 || t.max_size > kMaxPayload || t.initial_size > t.max_size) {
    die("bad type sizes", t.type_id);
  }
  if (t.create_op == 0) die("type has no create op", t.type_id);
  types_.push_back(t);
}

void Registry::add_op(const OpDescriptor& op) {
  if (op.op_id == 0 || op.op_id >= kMaxOpId) die("op id out of range", op.op_id);
  if (!type(op.type_id)) die("op for unknown type", op.op_id);
  if (op.validate == nullptr) die("op has no validate", op.op_id);
  if (op.max_args > kMaxArgs || op.max_result > kMaxResult) die("op limits too large", op.op_id);
  if (ops_.size() <= op.op_id) ops_.resize(op.op_id + 1);
  if (ops_[op.op_id].op_id != 0) die("duplicate op id", op.op_id);
  ops_[op.op_id] = op;
}

const TypeDescriptor* Registry::type(u32 type_id) const {
  for (const auto& t : types_) {
    if (t.type_id == type_id) return &t;
  }
  return nullptr;
}

const OpDescriptor* Registry::op(OpID op_id) const {
  if (op_id >= ops_.size() || ops_[op_id].op_id == 0) return nullptr;
  return &ops_[op_id];
}

Registry& registry() {
  static Registry r;
  return r;
}

}  // namespace snc
