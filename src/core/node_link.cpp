#include "node_link.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "snc/sri.h"

namespace snc {

void (*NodeLinks::debug_sync_probe)(const NodeLinks::SyncProbe& p) = nullptr;

// Wire v2 Hello (docs/CROSS_NODE_EXCHANGE_DESIGN.md 2.2).
struct HelloV2 {
  u32 magic;  // "SNC2"
  u16 group, shard;
  u32 crc;      // cluster shape
  u32 version;  // 2
  u64 epoch;    // informational
  u64 conn;     // this connection's random nonce
  u64 incarnation;
  u16 member;
  u8 flags;  // bit 0: accounting on
  u8 zero;
  u32 conn_seq;  // 1, 2, 3, ... per (instance, link)
};
static_assert(sizeof(HelloV2) == 48);

namespace {
constexpr u32 kMagicV1 = 0x4c434e53;  // "SNCL" (wire v1)
constexpr u32 kMagic = 0x324e4353;    // "SNC2"
constexpr u32 kAckMagic = 0x41434e53; // "SNCA"
constexpr u32 kVersion = 2;
constexpr u8 kFlagAccounting = 1;
constexpr u64 kRetryNs = 100'000'000;
constexpr u64 kConnectTimeoutNs = 1'000'000'000;
constexpr std::size_t kOutCap = 1u << 20;
constexpr std::size_t kInBuf = 64u << 10;

enum : u32 { kTagListen = 1, kTagIn = 2, kTagOut = 3 };

// Written once by the accepting side, back on the accepted socket (2.4).
struct HelloAck {
  u32 magic;  // "SNCA"
  u32 zero;
  u64 conn;         // the Hello's nonce, echoed
  u64 incarnation;  // the acceptor's
  u16 member;       // the acceptor's
  u16 zero2[3];
};
static_assert(sizeof(HelloAck) == 32);

u64 nonce() {
  u64 v = 0;
  while (v == 0) {
    if (::getrandom(&v, sizeof(v), 0) != sizeof(v)) v = 0;
  }
  return v;
}

u64 tag(u32 kind, u32 i) { return (u64{kind} << 32) | i; }

void watch(int epfd, int fd, u32 events, u64 t, int op = EPOLL_CTL_ADD) {
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = t;
  ::epoll_ctl(epfd, op, fd, &ev);
}

// A reset, not a graceful close: unsent bytes are discarded and the peer
// reads no more from this connection (2.8, 2.12 C).
void linger_reset(int fd) {
  linger l{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
}

void nodelay(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}
}  // namespace

struct NodeLinks::Out {
  enum class State : u8 { DOWN, CONNECTING, UP } state = State::DOWN;
  // The group's members, at this shard's port on each; `at` is the one
  // tried now. Whichever accepts is the primary: a standby does not listen.
  std::vector<std::pair<std::string, u16>> members;
  std::size_t at = 0;
  int fd = -1;
  u64 retry_ns = 0, since_ns = 0;
  std::vector<u8> buf;
  std::size_t off = 0;
  // This connection (2.2): nonce, and its order among this instance's
  // connections on this link.
  u64 conn = 0;
  u32 conn_seq = 0, next_seq = 1;
  // The HelloAck read back (2.4): who accepted this connection.
  u8 ack[sizeof(HelloAck)];
  std::size_t ack_have = 0;
  bool identified = false;
  u16 ack_member = 0;
  u64 ack_incarnation = 0;
  // Liveness (2.10): when this connection last pushed a frame; what the
  // peer's heartbeats say about it.
  u64 last_push_ns = 0;
  u64 mismatch_since_ns = 0;  // heartbeats name another conn, or none
  u64 r_stuck_since_ns = 0;   // R not advancing, peer not paused, frames outstanding
  u64 last_r = 0;
  u64 pushed = 0;  // counted frames pushed on this connection (D.2)
  u64 steer_since_ns = 0;  // 2.12 B
  // Binding (2.4): IDENTIFIED (HelloAck), SYNCED (the exact SYNC naming
  // this conn and conn_seq), BOUND once both name the same instance.
  bool synced = false;
  u16 sync_member = 0;
  u64 sync_incarnation = 0;
  bool is_bound = false;
};

struct NodeLinks::In {
  int fd = -1;
  bool hello = false;
  u16 group = 0, shard = 0;
  From from{};
  u64 last_read_ns = 0;  // bytes last read (2.10, 2.12 C: silence)
  u64 rx = 0;            // counted frames received on it (R)
  std::vector<u8> buf = std::vector<u8>(kInBuf);
  std::size_t have = 0;
  // Whole frames left in buf for want of budget (docs/FOLLOWUP_INTAKE_DESIGN.md
  // 3.6): handed on before anything more is read.
  bool backlog = false;
  bool muted = false;  // paused with a backlog and a hang-up pending: no events
};

NodeLinks::NodeLinks(const ClusterMap& map, u16 group, u16 shard, const Options& opt)
    : group_(group), shard_(shard), opt_(opt), crc_(map.shape_crc()), epoch_(map.epoch) {
  const u16 member = opt.member;
  for (const ClusterGroup& g : map.groups) n_shards_.push_back(g.n_shards);
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  sri_watch_fd(epfd_);
  base_.assign(map.groups.size(), 0);
  for (u16 g = 0; g < map.groups.size(); ++g) {
    base_[g] = static_cast<u32>(out_.size());
    if (g == group) continue;  // its own group: replication, not links; link_of is never asked for it
    for (u16 s = 0; s < map.groups[g].n_shards; ++s) {
      link_members_.push_back(static_cast<u16>(map.groups[g].members.size()));
      mbase_.push_back(mbase_.empty() ? 0 : mbase_.back() + link_members_[link_members_.size() - 2]);
      auto o = std::make_unique<Out>();
      for (const ClusterMember& m : map.groups[g].members) {
        o->members.emplace_back(m.host, static_cast<u16>(m.link_port + s));
      }
      out_.push_back(std::move(o));
    }
  }
  const u32 slots = mbase_.empty() ? 0 : mbase_.back() + link_members_.back();
  if (opt_.highest == nullptr) opt_.highest = &own_highest_;
  if (opt_.highest->size() < slots) opt_.highest->resize(slots, 0);
  peer_.assign(slots, Peer{});
  entries_.assign(slots, Entry{});
  listen_port_ = static_cast<u16>(map.groups[group].members[member].link_port + shard);
  try_listen(sri_now_ns());
}

NodeLinks::~NodeLinks() {
  for (auto& o : out_) {
    if (o->fd >= 0) ::close(o->fd);
  }
  for (auto& in : in_) {
    if (in && in->fd >= 0) ::close(in->fd);
  }
  if (listen_ >= 0) ::close(listen_);
  if (epfd_ >= 0) ::close(epfd_);
}

void NodeLinks::try_listen(u64 now_ns) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(listen_port_);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, 64) != 0) {
    if (listen_retry_ns_ == 0) {
      std::fprintf(stderr, "snc: node link: cannot listen on port %u (%s), retrying\n", listen_port_,
                   std::strerror(errno));
    }
    ::close(fd);
    listen_retry_ns_ = now_ns + kRetryNs;
    return;
  }
  listen_ = fd;
  watch(epfd_, listen_, EPOLLIN, tag(kTagListen, 0));
}

bool NodeLinks::up(u32 i) const { return out_[i]->state == Out::State::UP; }

bool NodeLinks::room(u32 i) const {
  const Out& o = *out_[i];
  return o.state == Out::State::UP && o.buf.size() - o.off < kOutCap;
}

void NodeLinks::push(u32 i, const Frame& f) {
  Out& o = *out_[i];
  const u8* p = reinterpret_cast<const u8*>(&f);
  o.buf.insert(o.buf.end(), p, p + sizeof(f));
  o.last_push_ns = now_ns_;
}

void NodeLinks::start_connect(u32 i, u64 now_ns) {
  Out& o = *out_[i];
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const auto& [host, p] = o.members[o.at];
  const std::string port = std::to_string(p);
  o.retry_ns = now_ns + kRetryNs;
  if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
    o.at = (o.at + 1) % o.members.size();
    return;
  }
  o.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  nodelay(o.fd);
  const int r = ::connect(o.fd, res->ai_addr, res->ai_addrlen);
  ::freeaddrinfo(res);
  if (r != 0 && errno != EINPROGRESS) {
    ::close(o.fd);
    o.fd = -1;
    o.at = (o.at + 1) % o.members.size();
    return;
  }
  o.state = Out::State::CONNECTING;
  o.since_ns = now_ns;
  watch(epfd_, o.fd, EPOLLOUT | EPOLLIN | EPOLLRDHUP, tag(kTagOut, i));
}

// Down, by failure or refusal: the next attempt tries the group's next
// member (the primary may have moved within the group).
void NodeLinks::drop_out(u32 i, u64 now_ns, bool reset) {
  Out& o = *out_[i];
  o.at = (o.at + 1) % o.members.size();
  if (o.fd >= 0) {
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, o.fd, nullptr);
    if (reset) linger_reset(o.fd);
    ::close(o.fd);
  }
  o.fd = -1;
  o.state = Out::State::DOWN;
  o.retry_ns = now_ns + kRetryNs;
  o.buf.clear();
  o.off = 0;
  o.conn = 0;
  o.ack_have = 0;
  o.identified = false;
  o.mismatch_since_ns = o.r_stuck_since_ns = o.last_r = o.pushed = o.steer_since_ns = 0;
  // No record of this connection is kept (2.3): its count stays,
  // conservatively, until the next binding's SYNC.
  o.synced = o.is_bound = false;
}

// ---- Liveness (2.10, 2.12 B) ----------------------------------------------------------

// What the sender sees of its In connection from the receiver's instance:
// conn (0: none), R on it, and whether its reads are paused or it is
// blocked. Pushed FIFO like any frame (2.5: no frame is ever put ahead of
// bytes already in the buffer).
void NodeLinks::push_heartbeat(u32 l, u64 now_ns) {
  (void)now_ns;
  Out& o = *out_[l];
  if (o.state != Out::State::UP || !room(l)) return;
  Frame f{};
  f.pad[0] = HEARTBEAT;
  if (o.identified) {
    for (const auto& in : in_) {
      if (in && in->hello && in->from.link == l && in->from.member == o.ack_member &&
          in->from.incarnation == o.ack_incarnation) {
        f.source.hi = in->from.conn;
        f.source.lo = in->rx;
        break;
      }
    }
  }
  f.op = (paused_ ? 1u : 0u) | (blocked_ ? 2u : 0u);
  push(l, f);
  ++heartbeats_sent_;
}

void NodeLinks::set_blocked(bool on) {
  if (blocked_ == on) return;
  blocked_ = on;
  // At once, not after T_hb of idleness (2.12 A).
  for (u32 l = 0; l < out_.size(); ++l) {
    if (out_[l]->identified) push_heartbeat(l, now_ns_);
  }
}

void NodeLinks::on_heartbeat(u32 i, const Frame& f, u64 now_ns) {
  const In& in = *in_[i];
  Peer& p = peer_[mbase_[in.from.link] + in.from.member];
  const bool blocked = (f.op & 2u) != 0;
  if (blocked && p.blocked_since_ns == 0) p.blocked_since_ns = now_ns;
  if (!blocked && p.blocked_since_ns != 0) {
    p.blocked_since_ns = 0;
    try_bind(in.from.link);
  }
  p.paused = (f.op & 1u) != 0;
  // About our Out connection on this link, if this instance acknowledged it.
  Out& o = *out_[in.from.link];
  if (o.state != Out::State::UP || !o.identified || o.ack_member != in.from.member ||
      o.ack_incarnation != in.from.incarnation) {
    return;
  }
  if (f.source.hi != o.conn) {
    if (o.mismatch_since_ns == 0) o.mismatch_since_ns = now_ns;
    return;
  }
  o.mismatch_since_ns = 0;
  const u64 r = f.source.lo;
  if (r != o.last_r || p.paused || o.pushed <= r) {
    o.last_r = r;
    o.r_stuck_since_ns = 0;
  } else if (o.r_stuck_since_ns == 0) {
    o.r_stuck_since_ns = now_ns;
  }
}

void NodeLinks::liveness(u64 now_ns) {
  // Heartbeats on every connection idle for T_hb.
  for (u32 l = 0; l < out_.size(); ++l) {
    Out& o = *out_[l];
    if (o.state == Out::State::UP && now_ns - o.last_push_ns >= opt_.hb_ns) push_heartbeat(l, now_ns);
  }
  // In side: silent for T_dead, but only while reads are not paused and
  // nothing is left unread (a paused or backlogged connection is
  // backpressure, not death).
  for (u32 j = 0; j < in_.size(); ++j) {
    if (!in_[j] || paused_ || in_[j]->backlog || now_ns - in_[j]->last_read_ns < opt_.dead_ns) continue;
    int unread = 0;
    if (::ioctl(in_[j]->fd, FIONREAD, &unread) == 0 && unread > 0) continue;
    close_in(j, /*reset=*/true);
    ++silence_closes_;
  }
  for (u32 l = 0; l < out_.size(); ++l) {
    Out& o = *out_[l];
    if (o.state != Out::State::UP) continue;
    // Out side: the peer does not see this connection, or reads nothing of
    // it while not paused, for T_dead. (No TCP_USER_TIMEOUT.)
    if ((o.mismatch_since_ns != 0 && now_ns - o.mismatch_since_ns >= opt_.dead_ns) ||
        (o.r_stuck_since_ns != 0 && now_ns - o.r_stuck_since_ns >= opt_.dead_ns)) {
      drop_out(l, now_ns, /*reset=*/true);
      ++out_resets_;
      continue;
    }
    // Member steering (2.12 B): a live In connection from another member
    // of that shard, and none from the one this link is attached to.
    const u16 at = static_cast<u16>(o.at);
    int other = -1;
    bool from_at = false;
    for (const auto& in : in_) {
      if (!in || !in->hello || in->from.link != l || now_ns - in->last_read_ns >= opt_.dead_ns) continue;
      if (in->from.member == at) from_at = true;
      else other = in->from.member;
    }
    if (other < 0 || from_at) {
      o.steer_since_ns = 0;
      continue;
    }
    if (o.steer_since_ns == 0) {
      o.steer_since_ns = now_ns;
    } else if (now_ns - o.steer_since_ns >= opt_.dead_ns) {
      drop_out(l, now_ns, /*reset=*/true);
      o.at = static_cast<std::size_t>(other);
      o.retry_ns = now_ns;
      ++steers_;
    }
  }
}

// The acceptor's HelloAck (2.4): read once, up to its 32 bytes. Anything
// after it is a protocol error; a hang-up closes as before.
bool NodeLinks::read_ack(u32 i, u64 now_ns) {
  Out& o = *out_[i];
  u8 b[sizeof(HelloAck) + 1];
  const std::size_t want = o.identified ? 1 : sizeof(HelloAck) - o.ack_have;
  const ssize_t r = ::recv(o.fd, b, want, MSG_DONTWAIT);
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
  if (r <= 0 || o.identified) {
    if (r > 0) std::fprintf(stderr, "snc: node link: bytes after the HelloAck on link %u, closing\n", i);
    drop_out(i, now_ns);
    return false;
  }
  std::memcpy(o.ack + o.ack_have, b, static_cast<std::size_t>(r));
  o.ack_have += static_cast<std::size_t>(r);
  if (o.ack_have < sizeof(HelloAck)) return true;
  HelloAck a;
  std::memcpy(&a, o.ack, sizeof(a));
  if (a.magic != kAckMagic || a.conn != o.conn) {
    std::fprintf(stderr, "snc: node link: a bad HelloAck on link %u, closing\n", i);
    drop_out(i, now_ns);
    return false;
  }
  if (a.member >= link_members_[i] || !admit_incarnation(i, a.member, a.incarnation, now_ns, -1)) {
    drop_out(i, now_ns, /*reset=*/true);
    return false;
  }
  o.identified = true;
  o.ack_member = a.member;
  o.ack_incarnation = a.incarnation;
  identified_out(i);
  return true;
}

// 2.12 C. Lower than the member's highest: a retired instance's delayed
// connection (or a restored data directory): refused. Higher: a new
// instance of the member, accepted only once the older one's In
// connection is closed or silent for T_dead; it is then fenced (its In
// sockets reset and destroyed, with whatever they had buffered; an Out
// connection it acknowledged reset) and only then retired, before the new
// one is accepted.
bool NodeLinks::admit_incarnation(u32 l, u16 m, u64 inc, u64 now_ns, int except_in) {
  u64& hi = highest(l, m);
  if (inc == hi) return true;
  if (inc < hi) {
    ++m_conflicts_;
    std::fprintf(stderr,
                 "snc: node link: refused link %u member %u incarnation %llu: below the highest seen (%llu): a "
                 "retired instance, or a restored data directory (raise it with --links-generation-min)\n",
                 l, m, (unsigned long long)inc, (unsigned long long)hi);
    return false;
  }
  if (hi != 0) {
    for (u32 j = 0; j < in_.size(); ++j) {
      if (static_cast<int>(j) == except_in || !in_[j] || !in_[j]->hello) continue;
      const From& f = in_[j]->from;
      if (f.link != l || f.member != m || f.incarnation >= inc) continue;
      if (now_ns - in_[j]->last_read_ns < opt_.dead_ns) {
        ++m_conflicts_;
        std::fprintf(stderr,
                     "snc: node link: refused link %u member %u incarnation %llu while incarnation %llu is live "
                     "(two instances of one member?)\n",
                     l, m, (unsigned long long)inc, (unsigned long long)f.incarnation);
        return false;
      }
    }
    for (u32 j = 0; j < in_.size(); ++j) {
      if (static_cast<int>(j) == except_in || !in_[j] || !in_[j]->hello) continue;
      const From& f = in_[j]->from;
      if (f.link == l && f.member == m && f.incarnation < inc) {
        close_in(j, /*reset=*/true);
        ++fenced_;
      }
    }
    Out& o = *out_[l];
    if (o.identified && o.ack_member == m && o.ack_incarnation < inc) {
      drop_out(l, now_ns, /*reset=*/true);
      ++fenced_;
    }
    const u64 old = hi;
    hi = inc;
    // Retire the old entry (2.7): its items are purged by the shard (no
    // END: its instance is gone), its END debt is discarded, its count
    // zeroed. The slot's serial bumps, so no older tag matches.
    if (opt_.on_retire) opt_.on_retire(opt_.ctx, l, m, old);
    Entry& e = ent(l, m);
    set_count(e, 0);
    const u32 serial = e.serial + 1;
    e = Entry{};
    e.serial = serial;
    e.inc = inc;
    return true;
  }
  hi = inc;
  ent(l, m).inc = inc;
  return true;
}

// ---- Accounting (2.4-2.6) ---------------------------------------------------------------

bool NodeLinks::bound(u32 l) const { return out_[l]->state == Out::State::UP && out_[l]->is_bound; }

bool NodeLinks::ready(u32 l) const { return opt_.accounting ? bound(l) : up(l); }

int NodeLinks::bound_member(u32 l) const { return bound(l) ? out_[l]->ack_member : -1; }

void NodeLinks::set_count(Entry& e, u64 v) {
  if (e.gates) xnode_live_ = xnode_live_ - e.count + v;
  e.count = v;
}

u64 NodeLinks::debt() const {
  u64 d = 0;
  for (const Entry& e : entries_) {
    if (!e.gates) d += e.count;
  }
  return d;
}

void NodeLinks::push_counted(u32 l, Frame f) {
  Out& o = *out_[l];
  f.pad[1] = COUNTED_BY_SENDER;
  Entry& e = ent(l, o.ack_member);
  set_count(e, e.count + 1);
  ++o.pushed;
  push(l, f);
}

// The HelloAck named an instance (2.4 step 3). A SYNC already read for this
// connection (step 4 first) must name the same one. The peer owes us a
// SYNC on its own Out connection; here, ours to it becomes possible (2.5).
void NodeLinks::identified_out(u32 l) {
  Out& o = *out_[l];
  ++ctr_.identified;
  if (o.synced && (o.sync_member != o.ack_member || o.sync_incarnation != o.ack_incarnation)) {
    ++ctr_.bind_mismatch;
    std::fprintf(stderr, "snc: node link: link %u: HelloAck and binding SYNC name different instances, resetting\n", l);
    drop_out(l, now_ns_, /*reset=*/true);
    return;
  }
  for (const auto& in : in_) {
    if (in && in->hello && in->from.link == l && in->from.member == o.ack_member &&
        in->from.incarnation == o.ack_incarnation) {
      ent(l, o.ack_member).sync_owed = true;
      break;
    }
  }
  try_bind(l);
}

void NodeLinks::try_bind(u32 l) {
  Out& o = *out_[l];
  if (!opt_.accounting || o.state != Out::State::UP || o.is_bound || !o.identified || !o.synced) return;
  if (o.sync_member != o.ack_member || o.sync_incarnation != o.ack_incarnation) return;
  if (peer_blocked_since(l, o.ack_member) != 0) return;  // never bound to a blocked instance (2.12 A)
  o.is_bound = true;
  Entry& e = ent(l, o.ack_member);
  if (!e.gates) {
    e.gates = true;
    xnode_live_ += e.count;
  }
  e.state = EntryState::ACTIVE;
  ++ctr_.bindings;
  connected_ = true;  // resume_all, re-sending what was purged while unbound
  if (opt_.on_bound) opt_.on_bound(opt_.ctx, l);
}

// Owed SYNCs, each on an Out connection identified with its target, FIFO
// after every byte already in Out::buf (the accounting barrier, 2.5): k,
// R and the ENDs it subsumes are read now, as it is appended.
void NodeLinks::push_syncs() {
  for (u32 l = 0; l < out_.size(); ++l) {
    Out& o = *out_[l];
    if (o.state != Out::State::UP || !o.identified) continue;
    Entry& e = ent(l, o.ack_member);
    if (!e.sync_owed || e.inc != o.ack_incarnation || !room(l)) continue;
    const In* cur = nullptr;
    for (const auto& in : in_) {
      if (in && in->hello && in->from.link == l && in->from.member == o.ack_member &&
          in->from.incarnation == e.inc) {
        cur = in.get();
        break;
      }
    }
    if (cur == nullptr) continue;  // no connection of its to name
    Frame f{};
    f.pad[0] = SYNC;
    f.source.hi = cur->from.conn;
    f.op = cur->from.conn_seq;
    f.source.lo = cur->rx;
    f.target.hi = e.held_for;
    f.target.lo = e.inc;
    const u64 cleared = e.end_owed;
    e.end_owed = 0;
    e.sync_owed = false;
    push(l, f);
    ++ctr_.syncs_sent;
    if (debug_sync_probe) {
      debug_sync_probe(SyncProbe{true, cur->from.conn, cur->from.conn_seq, e.held_for, cur->rx, 0, 0, 0, cleared});
    }
  }
}

// A SYNC from instance f (2.5): applied only if it names our current Out
// connection to that link, conn and conn_seq, and f is the instance it is
// identified or synced with (or none yet). Every other SYNC is stale.
void NodeLinks::on_sync(u32 i, const Frame& fr) {
  const From& f = in_[i]->from;
  if (fr.target.lo != opt_.incarnation) {
    ++ctr_.ignored_frames;
    return;
  }
  Out& o = *out_[f.link];
  const bool names_cur = o.state == Out::State::UP && o.conn != 0 && fr.source.hi == o.conn && fr.op == o.conn_seq;
  const bool same = o.identified ? (o.ack_member == f.member && o.ack_incarnation == f.incarnation)
                    : o.synced    ? (o.sync_member == f.member && o.sync_incarnation == f.incarnation)
                                  : true;
  if (!names_cur || !same) {
    ++ctr_.syncs_stale;
    return;
  }
  u64 r = fr.source.lo;
  if (r > o.pushed) {  // cannot have received more than we pushed on it
    ++ctr_.sync_r_anomalies;
    r = o.pushed;
  }
  Entry& e = ent(f.link, f.member);
  const u64 before = e.count;
  const u64 after = fr.target.hi + (o.pushed - r);
  set_count(e, after);
  ctr_.sync_correction += before > after ? before - after : after - before;
  ++ctr_.syncs_exact;
  if (debug_sync_probe) {
    debug_sync_probe(SyncProbe{false, o.conn, o.conn_seq, fr.target.hi, r, o.pushed, before, after, 0});
  }
  if (!o.synced) {
    o.synced = true;
    o.sync_member = f.member;
    o.sync_incarnation = f.incarnation;
    try_bind(f.link);
  }
}

bool NodeLinks::flush(u32 i, u64 now_ns) {
  Out& o = *out_[i];
  if (o.state != Out::State::UP || o.off == o.buf.size()) return false;
  const ssize_t n = ::send(o.fd, o.buf.data() + o.off, o.buf.size() - o.off, MSG_NOSIGNAL | MSG_DONTWAIT);
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) drop_out(i, now_ns);
    return false;
  }
  o.off += static_cast<std::size_t>(n);
  if (o.off == o.buf.size()) {
    o.buf.clear();
    o.off = 0;
  } else if (o.off >= kOutCap) {
    o.buf.erase(o.buf.begin(), o.buf.begin() + static_cast<std::ptrdiff_t>(o.off));
    o.off = 0;
  }
  return true;
}

void NodeLinks::accept_all() {
  while (true) {
    const int fd = ::accept4(listen_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    nodelay(fd);
    u32 i = 0;
    while (i < in_.size() && in_[i]) ++i;
    if (i == in_.size()) in_.emplace_back();
    in_[i] = std::make_unique<In>();
    in_[i]->fd = fd;
    in_[i]->last_read_ns = sri_now_ns();
    watch(epfd_, fd, paused_ ? EPOLLRDHUP : EPOLLIN | EPOLLRDHUP, tag(kTagIn, i));
  }
}

void NodeLinks::pause_reads(bool on) {
  if (paused_ == on) return;
  paused_ = on;
  for (u32 i = 0; i < in_.size(); ++i) {
    if (!in_[i]) continue;
    const int op = in_[i]->muted ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    in_[i]->muted = false;
    watch(epfd_, in_[i]->fd, on ? EPOLLRDHUP : EPOLLIN | EPOLLRDHUP, tag(kTagIn, i), op);
  }
}

void NodeLinks::close_in(u32 i, bool reset) {
  if (!in_[i]->muted) ::epoll_ctl(epfd_, EPOLL_CTL_DEL, in_[i]->fd, nullptr);
  if (reset) linger_reset(in_[i]->fd);
  ::close(in_[i]->fd);
  in_[i].reset();
}

// A v2 Hello on In connection i: refused (false) or accepted, with the
// HelloAck written back on the same socket (2.4).
bool NodeLinks::accept_hello(u32 i, const HelloV2& h) {
  In& in = *in_[i];
  // Placement epochs may differ (a slot moved and one side has not
  // refreshed yet): each frame carries its sender's, and the receiver
  // deals with a difference (Shard::on_link_frame). The groups and their
  // shard counts must match.
  const char* why = nullptr;
  if (h.magic != kMagic) why = "not a node link";
  else if (h.version != kVersion) why = "another wire version";
  else if (h.crc != crc_) why = "another cluster";
  else if (h.group >= n_shards_.size() || h.group == group_ || h.shard >= n_shards_[h.group]) why = "no such peer";
  else if (h.member >= link_members_[link_of(h.group, h.shard)]) why = "no such member";
  if (why != nullptr) {
    std::fprintf(stderr,
                 "snc: node link: refused group %u shard %u: %s (its cluster shape crc %08x, ours %08x)\n",
                 h.group, h.shard, why, h.crc, crc_);
    if (h.magic == kMagic && h.version != kVersion) ++version_refusals_;
    return false;
  }
  if (((h.flags & kFlagAccounting) != 0) != opt_.accounting) {
    std::fprintf(stderr,
                 "snc: node link: refused group %u shard %u member %u: cross-node accounting %s there, %s here "
                 "(SNC_XNODE_ACCOUNTING must match on every node)\n",
                 h.group, h.shard, h.member, (h.flags & kFlagAccounting) ? "on" : "off",
                 opt_.accounting ? "on" : "off");
    ++accounting_refusals_;
    return false;
  }
  const u32 l = link_of(h.group, h.shard);
  if (!admit_incarnation(l, h.member, h.incarnation, sri_now_ns(), static_cast<int>(i))) return false;
  HelloAck a{};
  a.magic = kAckMagic;
  a.conn = h.conn;
  a.incarnation = opt_.incarnation;
  a.member = opt_.member;
  // A fresh socket's send buffer is empty: 32 bytes go at once, or the
  // connection is no good.
  if (::send(in.fd, &a, sizeof(a), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(sizeof(a))) return false;
  in.hello = true;
  // A new connection: the sender's link was down, and what it sent here
  // meanwhile is lost. Resume here too, so this shard's objects ask again
  // for what they were owed (a settle, a forget).
  connected_ = true;
  in.group = h.group;
  in.shard = h.shard;
  in.from = From{l, h.member, h.incarnation, h.conn, h.conn_seq, ent(l, h.member).serial};
  ent(l, h.member).sync_owed = true;  // a new Hello from it (2.5)
  // Its earlier connection, if still open, goes with whatever it has not
  // delivered: frames from one instance must not run out of the order it
  // sent them in (a credit overtaking its own forget would credit twice,
  // docs/TRANSFERS.md), and the sender re-emits what was lost. Per member
  // (2.7): another member's connection is another stream, kept.
  for (u32 j = 0; j < in_.size(); ++j) {
    if (j != i && in_[j] && in_[j]->hello && in_[j]->from.link == l && in_[j]->from.member == h.member) close_in(j);
  }
  return true;
}

// False once the connection is done with (closed, or a peer with another map).
bool NodeLinks::read_in(u32 i, OnFrame on_frame, void* ctx, u32& budget) {
  In& in = *in_[i];
  while (true) {
    // Frames already read first, as many as the budget allows; more is
    // read only once none are left over.
    if (in.hello) {
      std::size_t at = 0;
      for (; in.have - at >= sizeof(Frame) && budget != 0; at += sizeof(Frame)) {
        Frame f;
        std::memcpy(&f, in.buf.data() + at, sizeof(f));
        if (kind(f) == HEARTBEAT) {
          on_heartbeat(i, f, now_ns_);
          continue;
        }
        if (kind(f) == SYNC) {
          if (opt_.accounting) on_sync(i, f);
          continue;
        }
        if (kind(f) != DATA) {
          ++ctr_.ignored_frames;
          continue;
        }
        if (opt_.accounting && (flags(f) & COUNTED_BY_SENDER)) {
          ++in.rx;
          ++ent(in.from.link, in.from.member).held_for;
        }
        --budget;
        on_frame(ctx, in.from, f);
        if (!in_[i]) return false;  // the frame's handling closed it
      }
      if (at) {
        std::memmove(in.buf.data(), in.buf.data() + at, in.have - at);
        in.have -= at;
      }
      in.backlog = in.have >= sizeof(Frame);
      if (in.backlog) return true;
    }
    const ssize_t n = ::recv(in.fd, in.buf.data() + in.have, in.buf.size() - in.have, MSG_DONTWAIT);
    if (n == 0) return false;
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    in.have += static_cast<std::size_t>(n);
    in.last_read_ns = sri_now_ns();
    std::size_t at = 0;
    if (!in.hello) {
      // A v1 Hello is 24 bytes: enough to tell it apart.
      u32 magic = 0;
      if (in.have >= sizeof(magic)) std::memcpy(&magic, in.buf.data(), sizeof(magic));
      if (in.have >= sizeof(magic) && magic == kMagicV1) {
        std::fprintf(stderr, "snc: node link: refused a peer: wire v1, this node speaks v2 (upgrade every node)\n");
        ++version_refusals_;
        return false;
      }
      if (in.have < sizeof(HelloV2)) continue;
      HelloV2 h;
      std::memcpy(&h, in.buf.data(), sizeof(h));
      if (!accept_hello(i, h)) return false;
      at = sizeof(HelloV2);
    }
    if (at) {
      std::memmove(in.buf.data(), in.buf.data() + at, in.have - at);
      in.have -= at;
    }
  }
}

bool NodeLinks::poll(u64 now_ns, OnFrame on_frame, void* ctx, u32 budget) {
  bool busy = false;
  now_ns_ = now_ns;
  if (now_ns - last_liveness_ns_ >= opt_.hb_ns / 4) {
    last_liveness_ns_ = now_ns;
    liveness(now_ns);
  }
  // Frames left over from earlier polls go first, oldest connection data
  // before anything newly readable.
  for (u32 i = 0; i < in_.size() && budget != 0; ++i) {
    if (in_[i] && in_[i]->backlog) {
      busy = true;
      if (!read_in(i, on_frame, ctx, budget) && in_[i]) close_in(i);
    }
  }
  if (listen_ < 0 && now_ns >= listen_retry_ns_) try_listen(now_ns);
  for (u32 i = 0; i < out_.size(); ++i) {
    Out& o = *out_[i];
    // Invariant M (2.1): no Out connection before this instance holds its
    // listener.
    if (o.state == Out::State::DOWN && now_ns >= o.retry_ns && listen_ >= 0) start_connect(i, now_ns);
    if (o.state == Out::State::CONNECTING && now_ns - o.since_ns >= kConnectTimeoutNs) drop_out(i, now_ns);
  }
  epoll_event evs[64];
  const int n = ::epoll_wait(epfd_, evs, 64, 0);
  for (int k = 0; k < n; ++k) {
    const u32 kind = static_cast<u32>(evs[k].data.u64 >> 32);
    const u32 i = static_cast<u32>(evs[k].data.u64);
    const u32 e = evs[k].events;
    busy = true;
    if (kind == kTagListen) {
      accept_all();
    } else if (kind == kTagIn) {
      if (i >= in_.size() || !in_[i]) continue;
      In& in = *in_[i];
      if (budget == 0 && in.backlog) {
        // Out of budget for this poll: its frames wait (level-triggered,
        // it is reported again). Paused, the only event is a hang-up, which
        // would be reported on every poll: no events from it until reads
        // resume (pause_reads re-adds it).
        if (paused_ && !in.muted) {
          ::epoll_ctl(epfd_, EPOLL_CTL_DEL, in.fd, nullptr);  // HUP is reported even with no events
          in.muted = true;
        }
        continue;
      }
      if (!read_in(i, on_frame, ctx, budget) && in_[i]) close_in(i);
    } else if (kind == kTagOut && i < out_.size()) {
      Out& o = *out_[i];
      if (o.state == Out::State::CONNECTING) {
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(o.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0 || (e & (EPOLLERR | EPOLLHUP))) {
          drop_out(i, now_ns);
          continue;
        }
        if (!(e & EPOLLOUT)) continue;
        o.state = Out::State::UP;
        watch(epfd_, o.fd, EPOLLIN | EPOLLRDHUP, tag(kTagOut, i), EPOLL_CTL_MOD);
        o.conn = nonce();
        o.conn_seq = o.next_seq++;
        o.ack_have = 0;
        o.identified = false;
        HelloV2 h{};
        h.magic = kMagic;
        h.group = group_;
        h.shard = shard_;
        h.crc = crc_;
        h.version = kVersion;
        h.epoch = epoch_;
        h.conn = o.conn;
        h.incarnation = opt_.incarnation;
        h.member = opt_.member;
        h.flags = opt_.accounting ? kFlagAccounting : 0;
        h.conn_seq = o.conn_seq;
        const u8* p = reinterpret_cast<const u8*>(&h);
        o.buf.assign(p, p + sizeof(h));
        o.off = 0;
        // Resume runs at binding with accounting (2.4); without it, at connect.
        if (!opt_.accounting) connected_ = true;
        ++connects_;
      } else if (o.state == Out::State::UP) {
        // The peer sends one HelloAck and nothing else: anything more,
        // or a hang-up, closes the connection.
        read_ack(i, now_ns);
      }
    }
  }
  if (opt_.accounting) push_syncs();
  for (u32 i = 0; i < out_.size(); ++i) busy |= flush(i, now_ns);
  return busy;
}

}  // namespace snc
