// Client: one TCP connection per shard and the server's slot map.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "snc/net.h"

namespace snc::net {

namespace {
constexpr std::size_t kReadChunk = 256u << 10;

i64 now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// timeout_ms > 0 bounds each blocking send and recv on the socket (the
// handshake), so a hung node fails the step instead of stalling it.
int dial(const std::string& host, u16 port, int timeout_ms = 0) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (timeout_ms > 0) {
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

bool write_all(int fd, const void* p, std::size_t n) {
  const u8* b = static_cast<const u8*>(p);
  while (n > 0) {
    ssize_t w = ::send(fd, b, n, MSG_NOSIGNAL);
    if (w <= 0) return false;
    b += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

bool read_all(int fd, void* p, std::size_t n) {
  u8* b = static_cast<u8*>(p);
  while (n > 0) {
    ssize_t r = ::recv(fd, b, n, 0);
    if (r <= 0) return false;
    b += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}
}  // namespace

Client::~Client() { close_links(); }

void Client::close_links() {
  for (Link& l : links_) {
    if (l.fd >= 0) ::close(l.fd);
    l.fd = -1;
  }
  if (epfd_ >= 0) ::close(epfd_);
  epfd_ = -1;
}

bool Client::connect(const std::string& host, u16 base_port) {
  if (endpoints_.empty()) endpoints_.push_back({host, base_port});
  else endpoints_[0] = {host, base_port};
  current_ = 0;
  return open(host, base_port);
}

void Client::add_endpoint(const std::string& host, u16 base_port) {
  if (endpoints_.empty()) endpoints_.push_back({host, base_port});  // replaced by connect()
  endpoints_.push_back({host, base_port});
}

void Client::enable_session(u64 client_id, int failover_ms, int request_timeout_ms) {
  client_id_ = client_id;
  failover_ms_ = failover_ms;
  request_timeout_ms_ = request_timeout_ms;
}

bool Client::open(const std::string& host, u16 base_port) {
  auto fail = [&](const char* what) {
    error_ = std::string(what) + ": " + std::strerror(errno);
    return false;
  };
  const int tmo = client_id_ != 0 ? request_timeout_ms_ : 0;
  int fd0 = dial(host, base_port, tmo);
  if (fd0 < 0) return fail("connect shard 0");

  wire::ReqHeader q{};
  q.len = sizeof(q);
  q.kind = wire::Kind::TOPOLOGY;
  wire::RespHeader h;
  wire::TopologyBody b;
  if (!write_all(fd0, &q, sizeof(q)) || !read_all(fd0, &h, sizeof(h))) {
    ::close(fd0);
    return fail("topology");
  }
  if (h.kind != wire::Kind::RESP || h.len != sizeof(h) + sizeof(b) || !read_all(fd0, &b, sizeof(b))) {
    ::close(fd0);
    errno = EPROTO;
    return fail("topology");
  }
  if (client_id_ != 0) {
    // A standby answers ops with E_STANDBY: not the primary (yet).
    wire::ReqHeader p{};
    p.len = sizeof(p);
    p.kind = wire::Kind::EXEC;
    wire::RespHeader r;
    if (!write_all(fd0, &p, sizeof(p)) || !read_all(fd0, &r, sizeof(r))) {
      ::close(fd0);
      return fail("probe");
    }
    std::vector<u8> body(r.len > sizeof(r) ? r.len - sizeof(r) : 0);
    if (!body.empty() && !read_all(fd0, body.data(), body.size())) {
      ::close(fd0);
      return fail("probe");
    }
    if (static_cast<Status>(r.status) == Status::E_STANDBY) {
      ::close(fd0);
      errno = EAGAIN;
      return fail("a standby");
    }
  }
  if (!links_.empty() && b.n_shards != map_.n_shards) {
    ::close(fd0);
    errno = EPROTO;
    return fail("another shard count");
  }
  map_.epoch = b.map_epoch;
  map_.n_shards = b.n_shards;
  std::memcpy(map_.owner.data(), b.owner, sizeof(b.owner));

  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) return fail("epoll_create1");
  links_.resize(map_.n_shards);
  links_[0].fd = fd0;
  for (u16 s = 1; s < map_.n_shards; ++s) {
    links_[s].fd = dial(host, static_cast<u16>(base_port + s), tmo);
    if (links_[s].fd < 0) return fail("connect shard");
  }
  for (u16 s = 0; s < map_.n_shards; ++s) {
    Link& l = links_[s];
    int flags = 1;
    ::ioctl(l.fd, FIONBIO, &flags);
    l.in.assign(kReadChunk, 0);
    l.in_len = 0;
    l.out.clear();
    l.out_off = 0;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = s;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, l.fd, &ev) != 0) return fail("epoll_ctl");
  }
  return true;
}

void Client::send(u16 shard, const wire::ReqHeader& h, ByteView args) {
  Link& l = links_[shard];
  wire::ReqHeader q = h;
  const bool seq = client_id_ != 0 && (h.kind == wire::Kind::EXEC || h.kind == wire::Kind::CREATE);
  wire::SeqPrefix sp{client_id_, 0};
  if (seq) {
    q.flags |= wire::kReqSequenced;
    sp.seq = l.next_seq++;
  }
  q.len = static_cast<u32>(sizeof(q) + (seq ? sizeof(sp) : 0)) + args.len;
  const std::size_t at = l.out.size();
  const auto* p = reinterpret_cast<const u8*>(&q);
  l.out.insert(l.out.end(), p, p + sizeof(q));
  if (seq) {
    const auto* x = reinterpret_cast<const u8*>(&sp);
    l.out.insert(l.out.end(), x, x + sizeof(sp));
  }
  if (args.len) l.out.insert(l.out.end(), args.data, args.data + args.len);
  if (client_id_ != 0) {
    // Kept until answered, to resend after a failover.
    l.order[h.req_id] = l.base + l.unanswered.size();
    l.unanswered.emplace_back(l.out.begin() + static_cast<std::ptrdiff_t>(at), l.out.end());
    l.sent_ns.push_back(now_ns());
  }
}

bool Client::fail_over() {
  if (client_id_ == 0 || endpoints_.empty()) return false;
  ++failovers_;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(failover_ms_);
  while (std::chrono::steady_clock::now() < deadline) {
    current_ = (current_ + 1) % endpoints_.size();
    close_links();
    const Endpoint& e = endpoints_[current_];
    if (open(e.host, e.port)) {
      // Resend what was not answered, in its original order.
      const i64 now = now_ns();
      for (Link& l : links_) {
        for (std::size_t i = 0; i < l.unanswered.size(); ++i) {
          const auto& req = l.unanswered[i];
          if (req.empty()) continue;
          l.out.insert(l.out.end(), req.begin(), req.end());
          ++resends_;
          max_resend_age_ns_ = std::max(max_resend_age_ns_, now - l.sent_ns[i]);
        }
      }
      progress_ns_ = now_ns();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  error_ = "failover: no primary answered: " + error_;
  return false;
}

bool Client::flush_all() {
  for (Link& l : links_) {
    if (l.out_off == l.out.size()) continue;
    ssize_t n = ::send(l.fd, l.out.data() + l.out_off, l.out.size() - l.out_off,
                       MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      error_ = std::string("send: ") + std::strerror(errno);
      return false;
    }
    if (n > 0) l.out_off += static_cast<std::size_t>(n);
    if (l.out_off == l.out.size()) {
      l.out.clear();
      l.out_off = 0;
    }
  }
  return true;
}

int Client::read_link(Link& l, OnResponse on, void* ctx) {
  if (l.in.size() - l.in_len < kReadChunk / 2) l.in.resize(l.in.size() * 2);
  ssize_t n = ::recv(l.fd, l.in.data() + l.in_len, l.in.size() - l.in_len, MSG_DONTWAIT);
  if (n == 0) {
    error_ = "server closed the connection";
    return -1;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    error_ = std::string("recv: ") + std::strerror(errno);
    return -1;
  }
  l.in_len += static_cast<std::size_t>(n);
  int got = 0;
  std::size_t pos = 0;
  while (l.in_len - pos >= sizeof(wire::RespHeader)) {
    wire::RespHeader h;
    std::memcpy(&h, l.in.data() + pos, sizeof(h));
    if (h.len < sizeof(h)) {
      error_ = "bad response frame";
      return -1;
    }
    if (l.in_len - pos < h.len) break;
    if (client_id_ != 0) {
      if (static_cast<Status>(h.status) == Status::E_STANDBY ||
          static_cast<Status>(h.status) == Status::E_FENCED) {
        error_ = "the node is no longer the primary";
        return -1;  // fail over; the request is resent
      }
      auto it = l.order.find(h.req_id);
      if (it != l.order.end()) {
        l.unanswered[it->second - l.base].clear();
        l.order.erase(it);
        while (!l.unanswered.empty() && l.unanswered.front().empty()) {
          l.unanswered.pop_front();
          l.sent_ns.pop_front();
          ++l.base;
        }
      }
    }
    if (h.flags & wire::kRespForwarded) ++forwarded_;
    on(ctx, h, ByteView(l.in.data() + pos + sizeof(h), h.len - static_cast<u32>(sizeof(h))));
    pos += h.len;
    ++got;
  }
  std::memmove(l.in.data(), l.in.data() + pos, l.in_len - pos);
  l.in_len -= pos;
  return got;
}

int Client::poll(OnResponse on, void* ctx, int timeout_ms) {
  if (!flush_all()) return fail_over() ? 0 : -1;
  const bool timed = client_id_ != 0 && request_timeout_ms_ > 0;
  if (timed && timeout_ms > request_timeout_ms_) timeout_ms = request_timeout_ms_;
  epoll_event evs[64];
  int n = ::epoll_wait(epfd_, evs, 64, timeout_ms);
  int got = 0;
  for (int i = 0; i < n; ++i) {
    int r = read_link(links_[evs[i].data.u32], on, ctx);
    if (r < 0) return fail_over() ? got : -1;
    got += r;
  }
  if (timed) {
    const i64 now = now_ns();
    bool waiting = false;
    for (const Link& l : links_) waiting |= !l.unanswered.empty();
    if (got > 0 || !waiting || progress_ns_ == 0) {
      progress_ns_ = now;
    } else if (now - progress_ns_ > i64(request_timeout_ms_) * 1000000) {
      ++timeouts_;
      error_ = "no answer in " + std::to_string(request_timeout_ms_) + " ms";
      return fail_over() ? got : -1;
    }
  }
  return got;
}

}  // namespace snc::net
