// Node links (internal, docs/CLUSTER.md): carry follow-ups between the
// shards of different replication groups. Each shard of a group's primary
// listens on its member's link_port + its id, and keeps one outbound TCP
// connection to every shard of every other group. A standby does not
// listen. An outbound link tries the group's members in turn until one
// accepts, so after a failover it finds the new primary by itself; the
// placement map is not consulted or changed (failover is not migration).
//
// Wire v2 (docs/CROSS_NODE_EXCHANGE_DESIGN.md 2.2): a 48-byte Hello names
// the sender's member, links instance (incarnation) and a random nonce for
// this connection; the accepting side writes one 32-byte HelloAck back on
// the accepted socket, the only bytes ever sent that way. Frames carry a
// kind (DATA, END, SYNC, HEARTBEAT) in pad[0] and flags in pad[1]. A v1 or
// mismatched peer is refused (fail-closed, 2.11).
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
  enum Kind : u8 { DATA = 0, END = 1, SYNC = 2, HEARTBEAT = 3 };
  enum : u8 { COUNTED_BY_SENDER = 1, COUNTED_BY_RECEIVER = 2 };  // DATA flags (pad[1])
  static u8 kind(const Frame& f) { return f.pad[0]; }
  static u8 flags(const Frame& f) { return f.pad[1]; }

  // Who sent a frame: the In connection's Hello (2.2, "the sending instance").
  struct From {
    u32 link;
    u16 member;
    u64 incarnation;
    u64 conn;
  };

  struct Options {
    u16 member = 0;
    u64 incarnation = 0;     // this links instance (2.1)
    bool accounting = true;  // Hello flag bit 0; a mismatch is refused (2.12 D)
  };

  // Opens the listener; call on the shard's thread.
  NodeLinks(const ClusterMap& map, u16 group, u16 shard, const Options& opt);
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
  using OnFrame = void (*)(void* ctx, const From& from, const Frame& f);
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
  u64 version_refusals() const { return version_refusals_; }
  u64 accounting_refusals() const { return accounting_refusals_; }
  u64 incarnation() const { return opt_.incarnation; }
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
  bool accept_hello(u32 i, const struct HelloV2& h);
  bool read_ack(u32 i, u64 now_ns);
  void close_in(u32 i);
  void try_listen(u64 now_ns);

  u16 group_, shard_;
  Options opt_;
  std::vector<u16> n_shards_;  // per group
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
  u64 version_refusals_ = 0, accounting_refusals_ = 0;
  u64 last_check_ns_ = 0;
};

}  // namespace snc
