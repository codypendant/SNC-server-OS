// Cluster map (docs/CLUSTER.md, docs/CONTROL_PLANE.md): which replication
// group owns each of the 4,096 slots. A group is one space (one UUID, one
// WAL lineage, one term) held by one or more member nodes, of which one is
// primary at a time; which one is HA state, not part of this map. Fixed
// while running (placement epoch 1) and the same on every node; a node
// checks a peer's map by its crc and epoch before taking follow-ups from
// it. Within a group, the local slot map still assigns slots to shards
// (slot % n_shards).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "snc/slot_map.h"

namespace snc {

inline constexpr u32 kMaxGroups = 255;

// One node holding a copy of a group's space.
struct ClusterMember {
  std::string host;     // where the other groups reach this member
  u16 link_port = 0;    // its shard i listens for follow-ups on link_port + i, while primary
  u16 client_port = 0;  // its shard i serves clients on client_port + i (0: not given)
};

struct ClusterGroup {
  u16 n_shards = 0;  // every member has this many (replication is stream to stream)
  std::vector<ClusterMember> members;
};

// The commit that last gave a slot to its current group
// (docs/MIGRATION_FAIL_CLOSED.md): the move's ID and the epoch it committed
// at. {0, 0}: none on record (the slot never moved, or it moved before
// placement v2).
struct SlotArrival {
  u64 move = 0;
  u64 epoch = 0;
  bool operator==(const SlotArrival&) const = default;
};

struct ClusterMap {
  std::vector<ClusterGroup> groups;
  std::array<u8, kSlots> group_of{};
  // Per slot, the arrival on record (placement v2). Not in crc(): it says
  // which local evidence proves a slot, not who owns it.
  std::array<SlotArrival, kSlots> arrived{};
  // Placement epoch (docs/CONTROL_PLANE.md section 5). Changes only when
  // placement does (a slot move, docs/MIGRATION.md); a failover inside a
  // group never changes it.
  u64 epoch = 1;

  // Contiguous ranges of slots, each group's in proportion to its shards.
  static ClusterMap make(const std::vector<ClusterGroup>& groups);
  // An explicit assignment (from the control plane, docs/CONTROL_PLANE.md):
  // every slot's group must exist. Throws std::invalid_argument otherwise.
  static ClusterMap assign(const std::vector<ClusterGroup>& groups, const std::array<u8, kSlots>& group_of,
                           u64 epoch);
  // Over the assignment and shard counts, not the members or addresses.
  u32 crc() const;
  // Over the groups' shard counts only: what two nodes must agree on to
  // link, whatever their placement epochs (docs/MIGRATION.md).
  u32 shape_crc() const;
  // The shard, in the owning group, that owns `slot` (its local slot map).
  u16 shard_of(u16 slot) const { return slot % groups[group_of[slot]].n_shards; }
  u16 group_of_id(const ObjectID& id) const { return group_of[slot_of(id)]; }
};

// The result of an op refused with E_MOVED (docs/CONTROL_PLANE.md section
// 8): the refusing node's placement epoch and the group that owns the
// target by its map. A client whose map is older refreshes it; one whose
// map is newer keeps it (the node is behind) and retries.
struct MovedResult {
  u64 epoch;
  u16 group;
  u16 reserved[3];
};
static_assert(sizeof(MovedResult) == 16);

// Groups separated by ',', a group's members by '|', each member
// "host:link_port:shards[:client_port]" (its group's shard count, repeated
// for every member), groups in group-id order. Throws std::invalid_argument
// on a malformed list or members of one group with different shard counts.
std::vector<ClusterGroup> parse_cluster(const std::string& text);

}  // namespace snc
