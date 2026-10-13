#include "node_link.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "snc/sri.h"

namespace snc {

namespace {
constexpr u32 kMagic = 0x4c434e53;  // "SNCL"
constexpr u64 kRetryNs = 100'000'000;
constexpr u64 kConnectTimeoutNs = 1'000'000'000;
constexpr std::size_t kOutCap = 1u << 20;
constexpr std::size_t kInBuf = 64u << 10;

enum : u32 { kTagListen = 1, kTagIn = 2, kTagOut = 3 };

struct Hello {
  u32 magic;
  u16 group, shard;
  u32 crc;
  u32 pad;
  u64 epoch;  // the sender's placement epoch when it connected (informational)
};
static_assert(sizeof(Hello) == 24);

u64 tag(u32 kind, u32 i) { return (u64{kind} << 32) | i; }

void watch(int epfd, int fd, u32 events, u64 t, int op = EPOLL_CTL_ADD) {
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = t;
  ::epoll_ctl(epfd, op, fd, &ev);
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
};

struct NodeLinks::In {
  int fd = -1;
  bool hello = false;
  u16 group = 0, shard = 0;
  std::vector<u8> buf = std::vector<u8>(kInBuf);
  std::size_t have = 0;
};

NodeLinks::NodeLinks(const ClusterMap& map, u16 group, u16 member, u16 shard)
    : group_(group), shard_(shard), crc_(map.shape_crc()), epoch_(map.epoch) {
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  sri_watch_fd(epfd_);
  base_.assign(map.groups.size(), 0);
  for (u16 g = 0; g < map.groups.size(); ++g) {
    base_[g] = static_cast<u32>(out_.size());
    if (g == group) continue;  // its own group: replication, not links; link_of is never asked for it
    for (u16 s = 0; s < map.groups[g].n_shards; ++s) {
      auto o = std::make_unique<Out>();
      for (const ClusterMember& m : map.groups[g].members) {
        o->members.emplace_back(m.host, static_cast<u16>(m.link_port + s));
      }
      out_.push_back(std::move(o));
    }
  }
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
void NodeLinks::drop_out(u32 i, u64 now_ns) {
  Out& o = *out_[i];
  o.at = (o.at + 1) % o.members.size();
  if (o.fd >= 0) {
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, o.fd, nullptr);
    ::close(o.fd);
  }
  o.fd = -1;
  o.state = Out::State::DOWN;
  o.retry_ns = now_ns + kRetryNs;
  o.buf.clear();
  o.off = 0;
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
    watch(epfd_, fd, EPOLLIN | EPOLLRDHUP, tag(kTagIn, i));
  }
}

void NodeLinks::close_in(u32 i) {
  ::epoll_ctl(epfd_, EPOLL_CTL_DEL, in_[i]->fd, nullptr);
  ::close(in_[i]->fd);
  in_[i].reset();
}

// False once the connection is done with (closed, or a peer with another map).
bool NodeLinks::read_in(u32 i, OnFrame on_frame, void* ctx) {
  In& in = *in_[i];
  while (true) {
    const ssize_t n = ::recv(in.fd, in.buf.data() + in.have, in.buf.size() - in.have, MSG_DONTWAIT);
    if (n == 0) return false;
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    in.have += static_cast<std::size_t>(n);
    std::size_t at = 0;
    if (!in.hello) {
      if (in.have < sizeof(Hello)) continue;
      Hello h;
      std::memcpy(&h, in.buf.data(), sizeof(h));
      // Placement epochs may differ (a slot moved and one side has not
      // refreshed yet): each frame carries its sender's, and the receiver
      // deals with a difference (Shard::on_link_frame). The groups and
      // their shard counts must match.
      if (h.magic != kMagic || h.crc != crc_) {
        std::fprintf(stderr,
                     "snc: node link: refused group %u shard %u: %s (its cluster shape crc %08x, ours %08x)\n",
                     h.group, h.shard, h.magic != kMagic ? "not a node link" : "another cluster", h.crc, crc_);
        return false;
      }
      in.hello = true;
      // A new connection: the sender's link was down, and what it sent
      // here meanwhile is lost. Resume here too, so this shard's objects
      // ask again for what they were owed (a settle, a forget).
      connected_ = true;
      in.group = h.group;
      in.shard = h.shard;
      at = sizeof(Hello);
      // Its earlier connection, if still open, goes with whatever it has not
      // delivered: frames from one shard must not run out of the order it
      // sent them in (a credit overtaking its own forget would credit twice,
      // docs/TRANSFERS.md), and the sender re-emits what was lost.
      for (u32 j = 0; j < in_.size(); ++j) {
        if (j != i && in_[j] && in_[j]->hello && in_[j]->group == h.group && in_[j]->shard == h.shard) close_in(j);
      }
    }
    for (; in.have - at >= sizeof(Frame); at += sizeof(Frame)) {
      Frame f;
      std::memcpy(&f, in.buf.data() + at, sizeof(f));
      on_frame(ctx, f);
    }
    if (at) {
      std::memmove(in.buf.data(), in.buf.data() + at, in.have - at);
      in.have -= at;
    }
  }
}

bool NodeLinks::poll(u64 now_ns, OnFrame on_frame, void* ctx) {
  bool busy = false;
  if (listen_ < 0 && now_ns >= listen_retry_ns_) try_listen(now_ns);
  for (u32 i = 0; i < out_.size(); ++i) {
    Out& o = *out_[i];
    if (o.state == Out::State::DOWN && now_ns >= o.retry_ns) start_connect(i, now_ns);
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
      if (i < in_.size() && in_[i] && !read_in(i, on_frame, ctx)) close_in(i);
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
        Hello h{kMagic, group_, shard_, crc_, 0, epoch_};
        const u8* p = reinterpret_cast<const u8*>(&h);
        o.buf.assign(p, p + sizeof(h));
        o.off = 0;
        connected_ = true;
        ++connects_;
      } else if (o.state == Out::State::UP) {
        // The peer never sends: readable means closed or reset.
        u8 b;
        const ssize_t r = ::recv(o.fd, &b, 1, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) drop_out(i, now_ns);
      }
    }
  }
  for (u32 i = 0; i < out_.size(); ++i) busy |= flush(i, now_ns);
  return busy;
}

}  // namespace snc
