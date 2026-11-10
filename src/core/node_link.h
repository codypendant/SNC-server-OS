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
    u32 conn_seq;
    u32 serial;  // the entry's serial when the frame was read (2.3, per item tag)
  };

  struct Options {
    u16 member = 0;
    u64 incarnation = 0;     // this links instance (2.1)
    bool accounting = true;  // Hello flag bit 0; a mismatch is refused (2.12 D)
    u64 hb_ns = 100'000'000;      // T_hb (2.10)
    u64 dead_ns = 1'000'000'000;  // T_dead (2.10)
    // Per (link, member): the highest incarnation seen (2.12 C). Owned by
    // the shard so it outlives this instance; sized here.
    std::vector<u64>* highest = nullptr;
    // An older incarnation of (link, member) was fenced and is retired
    // (2.7): its sockets are already reset. Called on the shard's thread.
    void (*on_retire)(void* ctx, u32 link, u16 member, u64 incarnation) = nullptr;
    // An Out connection became BOUND (2.4): resume what was purged.
    void (*on_bound)(void* ctx, u32 link) = nullptr;
    void* ctx = nullptr;
  };

  // ---- Accounting entries (2.3): one per (link, member), for that
  // member's current incarnation. Memory only; they go with this instance.
  enum class EntryState : u8 { ACTIVE, QUARANTINED };
  struct Entry {
    u64 inc = 0;     // 0: no instance seen
    u32 serial = 1;  // bumps at retirement: an older tag never matches
    // As the origin: our counted items pushed to it, not yet ended or
    // back; part of live only while `gates` (2.8).
    u64 count = 0;
    bool gates = true;
    EntryState state = EntryState::ACTIVE;
    // As the peer: its items here, ENDs owed to it, a SYNC owed (2.5).
    u64 held_for = 0;
    u64 end_owed = 0;
    bool sync_owed = false;
  };
  const Entry& entry(u32 link, u16 m) const { return entries_[mbase_[link] + m]; }
  // Counted DATA may go on `link` (2.4): BOUND, or up when accounting is
  // off (rollback: no binding). Not ready: down for queueing.
  bool ready(u32 link) const;
  bool bound(u32 link) const;
  int bound_member(u32 link) const;  // -1: none
  // Push our own exchange's follow-up, counted by us (COUNTED_BY_SENDER,
  // 2.6). The link must be bound.
  void push_counted(u32 link, Frame f);
  // An item here tagged (link, member, serial) leaves us other than by its
  // counted reply (2.6): one more END owed to that entry, if the tag is
  // still current (false: a retired instance's, nothing owed).
  bool dispose(u32 link, u16 member, u32 serial);
  // Push the counted reply of an item tagged (member, serial) on `link`
  // (COUNTED_BY_RECEIVER, 2.6): only if the link is bound to exactly that
  // entry. False: not pushed, the caller drops it and disposes of it.
  bool push_reply(u32 link, u16 member, u32 serial, Frame f);
  // A COUNTED_BY_RECEIVER frame from `from`: our exchange returns (2.6).
  // False: its entry has no count for it (xnode_end_underflow, or a
  // retired entry's): the caller drops it.
  bool take_reply(const From& from);
  // Σ count over gating entries (xnode_remote_live, 2.8).
  u64 xnode_live() const { return xnode_live_; }
  u64 debt() const;
  bool accounting() const { return opt_.accounting; }
  // Counters (section 7).
  struct Counters {
    u64 bindings = 0, identified = 0, bind_mismatch = 0, syncs_sent = 0, syncs_exact = 0, syncs_stale = 0;
    u64 sync_correction = 0, ignored_frames = 0, sync_r_anomalies = 0;
    // END (2.6): frames and the items they carried, each way; replies
    // pushed and returned; dispositions; stale tags; underflows (must
    // stay 0: an END or a counted reply with no count for it).
    u64 ends_sent = 0, end_items_sent = 0, ends_received = 0, end_items_received = 0;
    u64 replies_pushed = 0, replies_returned = 0, dispositions = 0, stale_tags = 0;
    u64 end_underflow = 0, held_underflow = 0;
  };
  const Counters& counters() const { return ctr_; }

  // Tests only (X3, X4, X28): the values at the SYNC barrier (peer, when
  // the SYNC is appended) and at its apply (origin).
  struct SyncProbe {
    bool barrier;  // true: appended at the peer; false: applied at the origin
    u64 conn;
    u32 conn_seq;
    u64 k, r;               // held_for and rx at the barrier, as carried
    u64 pushed;             // origin: cur pushed when applied
    u64 count_before, count_after;  // origin
    u64 end_owed_cleared;   // peer: ENDs subsumed by k
  };
  static void (*debug_sync_probe)(const SyncProbe& p);

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
  u64 m_conflicts() const { return m_conflicts_; }
  u64 fenced() const { return fenced_; }
  u16 members(u32 link) const { return link_members_[link]; }
  // 2.12 A: this instance's primary is blocked (sync replication with no
  // standby acknowledging). A change is announced at once in a HEARTBEAT.
  void set_blocked(bool on);
  bool blocked() const { return blocked_; }
  // What member m of `link` last said in its heartbeats (2.8 trigger 2):
  // blocked since (0: not blocked), and whether its reads are paused.
  u64 peer_blocked_since(u32 link, u16 m) const { return peer_[mbase_[link] + m].blocked_since_ns; }
  u64 heartbeats_sent() const { return heartbeats_sent_; }
  u64 silence_closes() const { return silence_closes_; }
  u64 out_resets() const { return out_resets_; }
  u64 steers() const { return steers_; }
  // The placement epoch announced on connections made from now on.
  void set_epoch(u64 e) { epoch_ = e; }

 private:
  struct Out;
  struct In;
  void start_connect(u32 i, u64 now_ns);
  void drop_out(u32 i, u64 now_ns, bool reset = false);
  bool flush(u32 i, u64 now_ns);
  void accept_all();
  bool read_in(u32 i, OnFrame on_frame, void* ctx, u32& budget);
  bool accept_hello(u32 i, const struct HelloV2& h);
  bool read_ack(u32 i, u64 now_ns);
  void close_in(u32 i, bool reset = false);
  void on_heartbeat(u32 in_index, const Frame& f, u64 now_ns);
  void push_heartbeat(u32 link, u64 now_ns);
  void liveness(u64 now_ns);  // 2.10 silence and Out-side checks, 2.12 B steering
  Entry& ent(u32 link, u16 m) { return entries_[mbase_[link] + m]; }
  void set_count(Entry& e, u64 v);
  void push_syncs();
  void push_ends();
  void on_end(u32 in_index, const Frame& f);
  void underflow(const char* what, u32 link, u16 member, u64 have, u64 n);
  void on_sync(u32 in_index, const Frame& f);
  void try_bind(u32 link);
  void identified_out(u32 link);
  u64& highest(u32 link, u16 member) { return (*opt_.highest)[mbase_[link] + member]; }
  // Invariant M at the peer (2.12 C): may member m's instance `inc` be
  // accepted on `link`? Fences and retires an older incarnation first.
  bool admit_incarnation(u32 link, u16 member, u64 inc, u64 now_ns, int except_in);
  void try_listen(u64 now_ns);

  u16 group_, shard_;
  Options opt_;
  std::vector<u16> n_shards_;  // per group
  std::vector<u16> link_members_;  // per link: the remote group's member count
  std::vector<u32> mbase_;         // per link: its first (link, member) index
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
  u64 version_refusals_ = 0, accounting_refusals_ = 0, m_conflicts_ = 0, fenced_ = 0;
  std::vector<u64> own_highest_;  // when the shard keeps none
  struct Peer {
    u64 blocked_since_ns = 0;
    bool paused = false;
  };
  std::vector<Peer> peer_;  // per (link, member)
  std::vector<Entry> entries_;  // per (link, member)
  u64 xnode_live_ = 0;
  Counters ctr_;
  bool blocked_ = false;
  u64 heartbeats_sent_ = 0, silence_closes_ = 0, out_resets_ = 0, steers_ = 0;
  u64 last_liveness_ns_ = 0;
  u64 now_ns_ = 0;  // the poll's time, for push stamps
  u64 last_check_ns_ = 0;
};

}  // namespace snc
