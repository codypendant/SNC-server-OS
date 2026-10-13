// Node links (internal, docs/CLUSTER.md): carry follow-ups between the
// shards of different nodes. Each shard listens on link_port + its id and
// keeps one outbound TCP connection to every shard of every other node.
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
    u8 pad[7];
    u8 args[16];
  };
  static_assert(sizeof(Frame) == 64);

  // Opens the listener; call on the shard's thread.
  NodeLinks(const ClusterMap& map, u16 node, u16 shard);
  ~NodeLinks();

  u32 size() const { return static_cast<u32>(out_.size()); }
  u32 link_of(u16 node, u16 shard) const { return base_[node] + shard; }
  bool up(u32 link) const;
  // Queued bytes are below the cap: push may add another frame.
  bool room(u32 link) const;
  void push(u32 link, const Frame& f);

  // Connects, accepts, sends what is queued and hands each frame received
  // to on_frame. True if it did anything.
  using OnFrame = void (*)(void* ctx, const Frame& f);
  bool poll(u64 now_ns, OnFrame on_frame, void* ctx);
  // A link came up, outbound or inbound, since the last call.
  bool take_connected() {
    const bool r = connected_;
    connected_ = false;
    return r;
  }
  u64 connects() const { return connects_; }

 private:
  struct Out;
  struct In;
  void start_connect(u32 i, u64 now_ns);
  void drop_out(u32 i, u64 now_ns);
  bool flush(u32 i, u64 now_ns);
  void accept_all();
  bool read_in(u32 i, OnFrame on_frame, void* ctx);
  void close_in(u32 i);
  void try_listen(u64 now_ns);

  u16 node_, shard_;
  u32 crc_;
  int epfd_ = -1;
  int listen_ = -1;
  u16 listen_port_ = 0;
  u64 listen_retry_ns_ = 0;
  std::vector<u32> base_;
  std::vector<std::unique_ptr<Out>> out_;
  std::vector<std::unique_ptr<In>> in_;  // null = free entry
  bool connected_ = false;
  u64 connects_ = 0;
  u64 last_check_ns_ = 0;
};

}  // namespace snc
