// An in-process TCP fault proxy for node links (D.2 tests,
// docs/CROSS_NODE_EXCHANGE_DESIGN.md 8.1). It accepts on one port and
// relays each connection to a target port. Each direction of each relayed
// connection has a mode:
//   PASS   relay;
//   HOLD   read and keep what arrives, relay nothing (released by PASS);
//   DROP   read and discard (forward: injected frames still go through);
//   STALL  read nothing (the sender's kernel buffer, then its user-space
//          queue, fill up).
// The forward direction (dialer -> target) is parsed as the v2 node-link
// stream (a 48-byte Hello, then 64-byte frames) as it is delivered, so a
// test can count what reached the target. The reverse direction carries
// only the 32-byte HelloAck.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/node_link.h"
#include "check.h"

namespace snc::test {

class LinkProxy {
 public:
  enum Mode : int { PASS = 0, HOLD = 1, DROP = 2, STALL = 3 };

  // What one relayed connection delivered forward, as parsed.
  struct Conn {
    u64 id = 0;
    bool hello = false;
    u16 member = 0;
    u64 incarnation = 0, conn = 0;
    u32 conn_seq = 0;
    std::string hello_bytes;
    u64 data = 0, counted_sender = 0, counted_receiver = 0, ends = 0, end_items = 0, syncs = 0, heartbeats = 0;
    // Kinds in delivery order (D data, E end, S sync, H heartbeat, R a
    // counted reply), the heartbeats left out; capped.
    std::string order;
    bool open = true;
    u64 undelivered = 0;  // forward bytes the target refused (its socket gone)
  };

  LinkProxy(u16 listen_port, u16 target_port, u32 rcvbuf = 0) : target_(target_port), rcvbuf_(rcvbuf) {
    lfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(lfd_ >= 0);
    int one = 1;
    ::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (rcvbuf_ != 0) ::setsockopt(lfd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf_, sizeof(rcvbuf_));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(listen_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
    CHECK_EQ(::listen(lfd_, 64), 0);
    th_ = std::thread([this] { loop(); });
  }
  ~LinkProxy() {
    stop_ = true;
    th_.join();
    for (auto& p : pairs_) close_pair(*p, false);
    ::close(lfd_);
  }

  // Modes for connections accepted from now on, and (all = true) for every
  // open one too.
  void set(Mode fwd, Mode rev, bool all = true) {
    std::lock_guard<std::mutex> g(mu_);
    def_fwd_ = fwd;
    def_rev_ = rev;
    if (all) {
      for (auto& p : pairs_) {
        p->fwd = fwd;
        p->rev = rev;
      }
    }
  }
  // Modes for the open connections only (new ones keep the default).
  void set_open(Mode fwd, Mode rev) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& p : pairs_) {
      p->fwd = fwd;
      p->rev = rev;
    }
  }
  // New connections are closed at once (the target unreachable).
  void refuse(bool on) { refuse_ = on; }
  // New connections go to another port from now on (X13: pin a link to
  // another instance).
  void retarget(u16 port) { target_ = port; }
  // Reset every open connection, both sides (SO_LINGER 0).
  void cut() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& p : pairs_) close_pair(*p, true);
    ++cuts_;
  }
  // A new connection to the target carrying `bytes` (a recorded Hello,
  // replayed), relayed in both directions like any other.
  void replay(const std::string& bytes) {
    std::lock_guard<std::mutex> g(mu_);
    auto p = std::make_unique<Pair>();
    p->id = ++next_id_;
    p->cfd = -1;  // no dialer: what comes back is read and dropped
    p->sfd = dial();
    if (p->sfd < 0) return;
    p->fwd = PASS;
    p->rev = DROP;
    p->fbuf.assign(bytes.begin(), bytes.end());
    p->info.id = p->id;
    p->replayed = true;
    pairs_.push_back(std::move(p));
  }
  // A forged frame (defense-in-depth tests, X30), put into the forward
  // stream of every open connection at the next frame boundary, as if its
  // dialer had sent it there. `then_hold`: it is the last frame delivered;
  // the forward stream is held from there (X32).
  void inject(const NodeLinks::Frame& f, bool then_hold = false) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& p : pairs_) {
      if (p->sfd < 0) continue;
      const u8* b = reinterpret_cast<const u8*>(&f);
      p->inject.insert(p->inject.end(), b, b + sizeof(f));
      p->hold_after_inject = then_hold;
    }
  }
  // Forward bytes held by inject(then_hold) once it was delivered.
  bool injected_and_held() const {
    std::lock_guard<std::mutex> g(mu_);
    for (const auto& p : pairs_) {
      if (p->sfd >= 0 && (p->hold_after_inject || !p->inject.empty() || p->fwd != HOLD)) return false;
    }
    return true;
  }
  std::vector<Conn> conns() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<Conn> v;
    for (const auto& p : pairs_) v.push_back(p->info);
    for (const Conn& c : closed_) v.push_back(c);
    return v;
  }
  // Totals over every connection so far.
  Conn total() const {
    Conn t;
    for (const Conn& c : conns()) {
      t.data += c.data;
      t.counted_sender += c.counted_sender;
      t.counted_receiver += c.counted_receiver;
      t.ends += c.ends;
      t.end_items += c.end_items;
      t.syncs += c.syncs;
      t.heartbeats += c.heartbeats;
    }
    return t;
  }
  u64 accepted() const { return accepted_; }
  u64 open_count() const {
    std::lock_guard<std::mutex> g(mu_);
    u64 n = 0;
    for (const auto& p : pairs_) n += p->cfd >= 0 || p->sfd >= 0;
    return n;
  }
  // Bytes held (HOLD) or waiting to be written, forward, over open pairs.
  u64 held_fwd() const {
    std::lock_guard<std::mutex> g(mu_);
    u64 n = 0;
    for (const auto& p : pairs_) n += p->fbuf.size();
    return n;
  }

 private:
  struct Pair {
    u64 id = 0;
    int cfd = -1, sfd = -1;
    int fwd = PASS, rev = PASS;
    std::vector<u8> fbuf, rbuf;
    u64 fwd_written = 0;  // bytes of the forward stream delivered (for parsing)
    u64 fwd_read = 0;     // bytes of the forward stream read from the dialer
    std::vector<u8> inject;  // forged frames waiting for a frame boundary
    bool hold_after_inject = false;
    u64 hold_at = 0;  // with hold_after_inject: hold once this much is delivered
    std::vector<u8> parse;
    Conn info;
    bool replayed = false;
    // The dialer hung up while forward bytes were held: they are kept,
    // with the target side open, until released (X25: an old instance's
    // frames delivered late).
    bool dialer_gone = false;
  };

  int dial() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(target_);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      ::close(fd);
      return -1;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
  }

  static void reset_close(int fd) {
    linger l{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    ::close(fd);
  }
  void close_pair(Pair& p, bool reset) {
    if (p.cfd >= 0) reset ? reset_close(p.cfd) : (void)::close(p.cfd);
    if (p.sfd >= 0) reset ? reset_close(p.sfd) : (void)::close(p.sfd);
    p.cfd = p.sfd = -1;
    p.info.open = false;
  }

  // Parses the forward bytes just delivered.
  void delivered(Pair& p, const u8* b, std::size_t n) {
    p.parse.insert(p.parse.end(), b, b + n);
    std::size_t at = 0;
    if (!p.info.hello) {
      if (p.parse.size() < 48) return;
      u32 magic = 0;
      std::memcpy(&magic, p.parse.data(), 4);
      p.info.hello = true;
      p.info.hello_bytes.assign(reinterpret_cast<const char*>(p.parse.data()), 48);
      if (magic == 0x324e4353) {  // "SNC2": u32 magic, u16 group, u16 shard, u32 crc, u32 version,
                                  // u64 epoch, u64 conn, u64 incarnation, u16 member, u16 flags, u32 conn_seq
        std::memcpy(&p.info.conn, p.parse.data() + 24, 8);
        std::memcpy(&p.info.incarnation, p.parse.data() + 32, 8);
        std::memcpy(&p.info.member, p.parse.data() + 40, 2);
        std::memcpy(&p.info.conn_seq, p.parse.data() + 44, 4);
      }
      at = 48;
    }
    for (; p.parse.size() - at >= sizeof(NodeLinks::Frame); at += sizeof(NodeLinks::Frame)) {
      NodeLinks::Frame f;
      std::memcpy(&f, p.parse.data() + at, sizeof(f));
      char k = '?';
      switch (NodeLinks::kind(f)) {
        case NodeLinks::DATA:
          ++p.info.data;
          if (NodeLinks::flags(f) & NodeLinks::COUNTED_BY_SENDER) {
            ++p.info.counted_sender;
            k = 'D';
          } else if (NodeLinks::flags(f) & NodeLinks::COUNTED_BY_RECEIVER) {
            ++p.info.counted_receiver;
            k = 'R';
          } else {
            k = 'd';
          }
          break;
        case NodeLinks::END:
          ++p.info.ends;
          p.info.end_items += f.source.hi;
          k = 'E';
          break;
        case NodeLinks::SYNC:
          ++p.info.syncs;
          k = 'S';
          break;
        case NodeLinks::HEARTBEAT:
          ++p.info.heartbeats;
          k = 0;
          break;
      }
      if (k != 0 && p.info.order.size() < 100000) p.info.order.push_back(k);
    }
    p.parse.erase(p.parse.begin(), p.parse.begin() + static_cast<std::ptrdiff_t>(at));
  }

  // One direction: read (unless STALL), then write (if PASS). False: closed.
  bool pump(Pair& p, bool fwd) {
    const int from = fwd ? p.cfd : p.sfd, to = fwd ? p.sfd : p.cfd;
    const int mode = fwd ? p.fwd : p.rev;
    std::vector<u8>& buf = fwd ? p.fbuf : p.rbuf;
    if (from >= 0 && mode != STALL && buf.size() < (8u << 20)) {
      u8 tmp[65536];
      const ssize_t n = ::recv(from, tmp, sizeof(tmp), MSG_DONTWAIT);
      if (n == 0 && fwd && mode == HOLD) {
        ::close(p.cfd);
        p.cfd = -1;
        p.dialer_gone = true;
        return true;
      }
      if (n == 0) return false;
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
      if (n > 0 && mode != DROP) buf.insert(buf.end(), tmp, tmp + n);
      if (n > 0 && fwd && mode != DROP) p.fwd_read += static_cast<u64>(n);
    }
    if (fwd && !p.inject.empty() && p.fwd_read >= 48) {
      // The last frame boundary read; usable if nothing past it has been
      // delivered yet (else wait for the next one).
      const u64 bnd = p.fwd_read - (p.fwd_read - 48) % sizeof(NodeLinks::Frame);
      if (bnd >= p.fwd_written) {
        buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(bnd - p.fwd_written), p.inject.begin(), p.inject.end());
        p.fwd_read += p.inject.size();
        p.hold_at = bnd + p.inject.size();
        p.inject.clear();
      }
    }
    // DROP keeps nothing it reads, so a forward buffer under DROP holds only
    // injected frames: those are delivered (X32b: forged heartbeats alone).
    if ((mode == PASS || (fwd && mode == DROP)) && !buf.empty()) {
      if (to < 0) {
        buf.clear();  // a replayed connection's answers
        return true;
      }
      const bool limit = fwd && p.hold_after_inject && p.inject.empty();
      const std::size_t len = limit ? static_cast<std::size_t>(p.hold_at - p.fwd_written) : buf.size();
      const ssize_t w = ::send(to, buf.data(), len, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        if (fwd) p.info.undelivered += buf.size();
        return false;
      }
      if (w > 0) {
        if (fwd) {
          delivered(p, buf.data(), static_cast<std::size_t>(w));
          p.fwd_written += static_cast<u64>(w);
        }
        buf.erase(buf.begin(), buf.begin() + w);
      }
    }
    if (fwd && p.hold_after_inject && p.inject.empty() && p.fwd_written >= p.hold_at) {
      p.fwd = HOLD;  // the forged frame was the last one delivered
      p.hold_after_inject = false;
    }
    if (fwd && p.dialer_gone && mode == PASS && buf.empty()) return false;
    return true;
  }

  void loop() {
    while (!stop_) {
      {
        std::lock_guard<std::mutex> g(mu_);
        while (true) {
          const int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (fd < 0) break;
          ++accepted_;
          if (refuse_) {
            reset_close(fd);
            continue;
          }
          int one = 1;
          ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          auto p = std::make_unique<Pair>();
          p->id = ++next_id_;
          p->cfd = fd;
          p->sfd = dial();
          if (p->sfd < 0) {
            reset_close(fd);
            continue;
          }
          p->fwd = def_fwd_;
          p->rev = def_rev_;
          p->info.id = p->id;
          pairs_.push_back(std::move(p));
        }
        for (auto it = pairs_.begin(); it != pairs_.end();) {
          Pair& p = **it;
          bool ok = (p.cfd >= 0 || p.replayed || p.dialer_gone) && p.sfd >= 0;
          if (ok) ok = pump(p, true);
          if (ok) ok = pump(p, false);
          if (!ok) {
            close_pair(p, false);
            closed_.push_back(p.info);
            it = pairs_.erase(it);
          } else {
            ++it;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  std::atomic<u16> target_;
  int rcvbuf_ = 0;
  int lfd_ = -1;
  std::thread th_;
  std::atomic<bool> stop_{false}, refuse_{false};
  mutable std::mutex mu_;
  std::vector<std::unique_ptr<Pair>> pairs_;
  std::vector<Conn> closed_;
  int def_fwd_ = PASS, def_rev_ = PASS;
  u64 next_id_ = 0, cuts_ = 0;
  std::atomic<u64> accepted_{0};
};

}  // namespace snc::test
