// Placement state (docs/CONTROL_PLANE.md section 3): the cluster's nodes,
// its replication groups and which group owns each slot, under a placement
// epoch. Held by the control plane (the `placement` machine in the
// witnesses), fetched by nodes at start and stored in their data
// directories. It never names a group's primary or term: that is HA state.
//
// Encoded as one line of text, the form the control plane stores:
//   v1 cluster=HEX32 epoch=E nodes=ID@HOST:ADMIN,...
//      groups=SHARDS/UUIDHEX/NODE:LINK:CLIENT+NODE:LINK:CLIENT,... slots=G*N,G*N,...
//      [move=IDHEX:SLOT:FROM:TO:EPOCH:STATE]
// Version 2 (docs/MIGRATION_FAIL_CLOSED.md) is the same line starting "v2",
// with one more optional key, last:
//      [arrived=SLOT:MOVEHEX:EPOCH,...]   (sparse, ascending slots)
// the commit that last gave each listed slot to its group. A v1 decoder
// refuses v2, so an older writer cannot rewrite it and drop `arrived`; a
// placement read as v2 is always written back as v2.
// Groups are numbered by their place in the list; slots are runs from 0.
// The move, when present, is the one slot move in progress
// (docs/MIGRATION.md); it changes the machine's version, never the epoch,
// until it commits.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "snc/cluster.h"
#include "snc/storage.h"

namespace snc {

struct PlacementNode {
  u32 id = 0;  // NodeID: assigned once, never reused, not derived from the address
  std::string host;
  u16 admin_port = 0;
};

struct PlacementMember {
  u32 node = 0;
  u16 link_port = 0;
  u16 client_port = 0;
};

struct PlacementGroup {
  u16 n_shards = 0;
  Uuid space{};  // the group's data identity: every member's space UUID
  std::vector<PlacementMember> members;
};

// A slot move in progress (docs/MIGRATION.md). Its states only advance,
// each by a compare-and-set on the placement machine: PREPARED -> SEALED
// (the source's seal is durable) -> STAGED (the destination holds the
// sealed state, durably) -> COMMITTED (in the same change: the slot is
// the destination's and the epoch is `epoch`). Removing it before COMMITTED
// aborts the move; after, finishes it.
struct PlacementMove {
  enum class State : u8 { NONE = 0, PREPARED, SEALED, STAGED, COMMITTED };
  // The placement machine's version that records the PREPARED move: grows
  // with every move, so groups refuse a late request from an older one.
  u64 id = 0;
  u16 slot = 0;
  u16 from = 0, to = 0;
  u64 epoch = 0;  // the epoch the move commits at: the placement's at PREPARED, plus 1
  State state = State::NONE;

  static const char* name(State s);
};

struct Placement {
  // Encoding version: 1, or 2 once v2 writes are enabled (snc_ctl
  // --placement-v2) or a v2 placement was read. Never lowered by a writer.
  u8 version = 1;
  Uuid cluster_id{};
  u64 epoch = 0;
  std::vector<PlacementNode> nodes;
  std::vector<PlacementGroup> groups;
  std::array<u8, kSlots> slots{};
  PlacementMove move;
  // v2 only: per slot, the commit that last gave it to its group. Set by a
  // move's commit (and the backfill); never by a failover.
  std::array<SlotArrival, kSlots> arrived{};

  // A first placement: epoch 1, slots in ClusterMap::make's contiguous
  // ranges. Throws std::invalid_argument on an unknown or repeated node.
  static Placement initial(const Uuid& cluster_id, std::vector<PlacementNode> nodes,
                           std::vector<PlacementGroup> groups);
  // Throws std::logic_error for a v1 placement with arrivals: they would be
  // dropped (a writer must raise the version, never lose them).
  std::string encode() const;
  // Checks the form and the references (nodes, groups, slots).
  static std::optional<Placement> decode(const std::string& text);

  const PlacementNode* node(u32 id) const;
  // The group and member index node `id` serves as; false if it is in none.
  bool find(u32 id, u16& group, u16& member) const;
  // The data plane's view: groups with addresses, slots, epoch.
  std::vector<ClusterGroup> cluster_groups() const;
  ClusterMap map() const;
};

}  // namespace snc
