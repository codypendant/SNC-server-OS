// Type and op registration (spec sections 2 and 6). Types are compiled in and
// registered before the runtime starts; the registry is read-only afterwards.
#pragma once

#include <vector>

#include "snc/object.h"

namespace snc {

using OpID = u32;

enum class OpKind : u8 { READ, MUTATE, CREATE, DELETE };

class OpCtx;

struct OpDescriptor {
  OpID op_id = 0;
  u32 type_id = 0;  // op runs only on objects of this type
  const char* name = "";
  OpKind kind = OpKind::READ;
  u32 max_args = 0;
  u32 max_result = 0;
  // Phase 1: read-only. May reject. Must not modify the object.
  // For CREATE the object is freshly allocated with zeroed payload.
  Status (*validate)(OpCtx& ctx, const ObjectHeader& obj, ByteView args) = nullptr;
  // Phase 2: in-place mutation through ctx.obj(). Must succeed once validate
  // returned OK. May be null for ops with nothing to apply.
  void (*apply)(OpCtx& ctx, ByteView args) = nullptr;
  // MUTATE only: apply never changes the type's indexed key, so the runtime
  // skips index maintenance for this op. Checked in debug builds.
  bool keeps_index_key = false;
  // MUTATE only: the change may be lost in a crash, so it is not logged and
  // does not bump the version; it reaches the WAL with the object's next
  // logged change or checkpoint. Only for changes a type's resume hook
  // redoes after recovery (docs/TRANSFERS.md: dropping a finished inbox
  // entry). May not emit, use OpCtx::local, or change the indexed key.
  bool unlogged = false;
  // Follow-ups (docs/FOLLOWUP_INTAKE_DESIGN.md 3.3, 3.9.1). max_emits: the
  // most follow-ups one apply may emit (a hard ceiling; 0 = may not emit).
  // A client-run apply emits at most what its validate reserved with
  // OpCtx::reserve_emits. `followup`: the op runs only as a follow-up
  // (OpCtx::source() set); clients get E_BAD_ARGS, and only such ops may
  // be emitted. A follow-up apply emits at most once, and only to its
  // source (the reply-only contract), so a follow-up op has max_emits 0 or 1.
  u16 max_emits = 0;
  bool followup = false;
};

// Secondary index (spec section 11): shard-local, non-unique, equality only.
struct IndexDescriptor {
  u32 index_id = 0;
  u32 type_id = 0;
  const char* name = "";
  // Returns key length (0 = object not indexed); writes at most kMaxIndexKey
  // bytes to key_out.
  u32 (*extract)(const u8* payload, u32 len, u8* key_out) = nullptr;
};

struct TypeDescriptor {
  u32 type_id = 0;  // stable registry number; 0..255 reserved for the runtime
  const char* name = "";
  u16 schema_ver = 1;
  u32 initial_size = 0;  // payload bytes allocated at create
  u32 max_size = 0;      // <= kMaxPayload
  // Structural validation, used at recovery and in debug builds.
  bool (*check)(const u8* payload, u32 len) = nullptr;
  OpID create_op = 0;  // OpKind::CREATE descriptor, required
  OpID delete_op = 0;  // OpKind::DELETE descriptor, optional
  u32 index_id = 0;    // v0.1: at most one secondary index per type, 0 = none
  // Runs on every object of the type once recovery finishes, so it can
  // re-emit follow-ups its state says are pending (OpCtx::emit). Optional.
  void (*resume)(OpCtx& ctx) = nullptr;
  // The most follow-ups one object's resume hook may emit (enforced; the
  // scan pages resume work against the exchange budget per object).
  u32 max_resume_emits = 0;
};

inline constexpr u32 kFirstUserTypeId = 256;

class Registry {
 public:
  // Registration aborts on duplicates or invalid descriptors: these are
  // programming errors in compiled-in code.
  void add_type(const TypeDescriptor& t);
  void add_system_type(const TypeDescriptor& t);  // the runtime's own (reserved ids)
  void add_op(const OpDescriptor& op);
  void add_index(const IndexDescriptor& idx);  // also sets the type's index_id

  const TypeDescriptor* type(u32 type_id) const;
  const OpDescriptor* op(OpID op_id) const;
  const IndexDescriptor* index(u32 index_id) const;

  const std::vector<TypeDescriptor>& types() const { return types_; }

 private:
  std::vector<TypeDescriptor> types_;
  std::vector<OpDescriptor> ops_;  // dense by op_id for O(1) lookup
  std::vector<IndexDescriptor> indexes_;
};

// Process-wide registry. Populate before starting the runtime.
Registry& registry();

}  // namespace snc
