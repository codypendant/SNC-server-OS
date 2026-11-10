// Node links (internal, docs/CLUSTER.md): carry follow-ups between the
// shards of different replication groups. Each shard of a group's primary
// listens on its member's link_port + its id, and keeps one outbound TCP
// connection to every shard of every other group. A standby does not
// listen. An outbound link tries the group's members in turn until one
// accepts, so after a failover it finds the new primary by itself; the
// placement map is not consulted or changed (failover is not migration).
//
// There are no acks. A follow-up is lost if its connection drops or is not
// up yet. Every time a connection comes up, in either direction, the shard
// re-runs its objects' resume hooks, which re-emit whatever has not settled
// and ask peers again for answers owed (transfer follow-ups are idempotent,
// docs/TRANSFERS.md).
#pragma once

#include <memory>
#include <vector>

#include "snc/cluster.h"

namespace snc {

class NodeLinks {
 public:
  struct Frame {
    ObjectID source, target;
    u32 op, fail_op;
    u8 len;
    u8 pad[3];
    u32 epoch;  // the sender's placement epoch (low 32 bits) when it routed this
    u8 args[16];
  };
  static_assert(sizeof(Frame) == 64);

  // Opens the listener; call on the shard's thread.
  NodeLinks(const ClusterMap& map, u16 group, u16 member, u16 shard);
  ~NodeLinks();

  u32 size() const { return static_cast<u32>(out_.size()); }
  u32 link_of(u16 group, u16 shard) const { return base_[group] + shard; }
  bool up(u32 link) const;
  // Queued bytes are below the cap: push may add another frame.
  bool room(u32 link) const;
  void push(u32 link, const Frame& f);

  // Connects, accepts, sends what is queued and hands each frame received
  // to on_frame. True if it did anything.
  // At most `budget` frames are handed on per call (whole frames left over
  // wait in the connection's buffer, docs/FOLLOWUP_INTAKE_DESIGN.md 3.6).
  using OnFrame = void (*)(void* ctx, const Frame& f);
  bool poll(u64 now_ns, OnFrame on_frame, void* ctx, u32 budget = ~u32{0});
  // Paused: inbound connections are watched for hang-ups only, so nothing
  // more is read from them (nor does the poll spin on them).
  void pause_reads(bool on);
  // A link came up, outbound or inbound, since the last call.
  bool take_connected() {
    const bool r = connected_;
    connected_ = false;
    return r;
  }
  u64 connects() const { return connects_; }
  // The placement epoch announced on connections made from now on.
  void set_epoch(u64 e) { epoch_ = e; }

 private:
  struct Out;
  struct In;
  void start_connect(u32 i, u64 now_ns);
  void drop_out(u32 i, u64 now_ns);
  bool flush(u32 i, u64 now_ns);
  void accept_all();
  bool read_in(u32 i, OnFrame on_frame, void* ctx, u32& budget);
  void close_in(u32 i);
  void try_listen(u64 now_ns);

  u16 group_, shard_;
  u32 crc_;
  u64 epoch_;
  int epfd_ = -1;
  int listen_ = -1;
  u16 listen_port_ = 0;
  u64 listen_retry_ns_ = 0;
  std::vector<u32> base_;
  std::vector<std::unique_ptr<Out>> out_;
  std::vector<std::unique_ptr<In>> in_;  // null = free entry
  bool connected_ = false;
  bool paused_ = false;
  u64 connects_ = 0;
  u64 last_check_ns_ = 0;
};

}  // namespace snc
