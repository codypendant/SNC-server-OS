// Slot states (docs/MIGRATION.md): what a replication group knows about a
// slot that has taken part in a move. One small system object per such slot,
// on the shard that owns the slot, logged and replicated like any object, so
// a seal or an activation survives a crash and a failover exactly as an
// acknowledged write does. A slot with no state object is in its initial
// state: the group serves it if the placement map gives it the slot.
#pragma once

#include "snc/registry.h"
#include "snc/types.h"

namespace snc {

inline constexpr u32 kSlotStateTypeId = 2;           // reserved range
inline constexpr OpID kSlotStateCreateOp = 2;        // never run by clients
inline constexpr u32 kSlotStateIndexId = 0x80000002;  // slot -> its state object

enum class SlotPhase : u8 {
  NONE = 0,    // no state object: serving iff the map gives this group the slot
  ACTIVE = 1,  // activated by a move here (or unsealed): as NONE
  SEALED = 2,  // moving away: no write is ever applied here again, whatever the map says
  STAGED = 3,  // moving here: a complete copy of the sealed source, not yet authoritative
};

struct SlotStateV1 {
  u16 slot;
  u8 phase;      // SlotPhase
  u8 prior;      // SEALED: the phase before it (NONE or ACTIVE), restored by an unseal
  u16 group;     // SEALED: destination; STAGED, ACTIVE: source
  u16 flags;     // kSlotUnsealed: move_id's seal was undone (its move aborted)
  u64 move_id;   // the last move that sealed, staged or activated the slot here
  u64 epoch;     // SEALED, STAGED: the placement epoch the move commits at; ACTIVE: that epoch
  u64 count;     // SEALED, STAGED: objects in the slot
  u64 digest;    // ... and their slot_digest
};
static_assert(sizeof(SlotStateV1) == 40);
inline constexpr u16 kSlotUnsealed = 1;

// The serving cache (Shard::phases_) holds a SlotPhase, or this: the map
// gives the slot to this group but the local evidence does not prove the
// arrival it records (docs/MIGRATION_FAIL_CLOSED.md 4). Volatile: never in
// a record; recomputed whenever the slot's state or the map changes.
inline constexpr u8 kCacheUnproven = 4;

// Registers the type and its index once per process.
void register_slot_state_type();

}  // namespace snc
