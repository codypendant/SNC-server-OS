// Cluster map (docs/CLUSTER.md): which node owns each of the 4,096 slots.
// Fixed while running and the same on every node; a node checks a peer's
// map by its crc before taking follow-ups from it. Within a node, the
// local slot map still assigns slots to shards (slot % n_shards).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "snc/slot_map.h"

namespace snc {

inline constexpr u32 kMaxNodes = 255;

struct ClusterNode {
  std::string host;     // where the other nodes reach this one
  u16 link_port = 0;    // shard i listens for follow-ups on link_port + i
  u16 n_shards = 0;
};

struct ClusterMap {
  std::vector<ClusterNode> nodes;
  std::array<u8, kSlots> node_of{};

  // Contiguous ranges of slots, each node's in proportion to its shards.
  static ClusterMap make(const std::vector<ClusterNode>& nodes);
  // Over the assignment and shard counts, not the addresses.
  u32 crc() const;
  // The shard on the owning node that owns `slot` (its local slot map).
  u16 shard_of(u16 slot) const { return slot % nodes[node_of[slot]].n_shards; }
  u16 node_of_id(const ObjectID& id) const { return node_of[slot_of(id)]; }
};

// "host:link_port:shards,host:link_port:shards,...", one entry per node in
// node-id order. Throws std::invalid_argument on a malformed list.
std::vector<ClusterNode> parse_cluster(const std::string& text);

}  // namespace snc
