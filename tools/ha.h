// Small line protocol shared by snc_server's admin port and snc_witness
// (docs/REPLICATION.md, "Automatic failover"). One request line, one reply
// line, then the connection closes. ha::Conn (K0,
// docs/CONTROL_CONNECTIONS_DESIGN.md 4.1, 4.2) keeps one connection to an
// admin port for an eligible caller (today only test_migrate's probe).
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "snc/trace.h"

namespace snc::ha {

// Who is asking, for the SNC_TRACE=1 line every ask writes ("ha_ask",
// docs/CONTROL_CONNECTIONS_DESIGN.md 5, A2): a path of the Caller scopes
// open on this thread, outermost first ("watch/propose/read_latest"), "-"
// with none. It only labels trace lines; nothing reads it otherwise.
inline std::string& caller_path() {
  thread_local std::string p;
  return p;
}
// With tracing off (the default) a scope does nothing: one load and branch
// on trace::enabled(), no string work (docs/HEALTH_BASELINE.md, overhead).
struct Caller {
  static constexpr std::size_t kOff = static_cast<std::size_t>(-1);
  std::size_t prev = kOff;
  explicit Caller(const char* name) {
    if (!::snc::trace::enabled()) return;
    prev = caller_path().size();
    if (prev != 0) caller_path() += '/';
    caller_path() += name;
  }
  explicit Caller(const std::string& name) : Caller(name.c_str()) {}
  // Another thread's path, carried into a helper thread (Paxos broadcast).
  struct Adopt {
    std::string prev;
    bool on = ::snc::trace::enabled();
    explicit Adopt(const std::string& path) {
      if (!on) return;
      prev = caller_path();
      caller_path() = path;
    }
    ~Adopt() {
      if (on) caller_path() = prev;
    }
    Adopt(const Adopt&) = delete;
    Adopt& operator=(const Adopt&) = delete;
  };
  ~Caller() {
    if (prev != kOff) caller_path().resize(prev);
  }
  Caller(const Caller&) = delete;
  Caller& operator=(const Caller&) = delete;
};

// The verb of a request line for a trace: its first word, and its second
// when that is short (a sub-verb or a Paxos machine: "PLACEMENT GET",
// "READ ha/0"; not a space UUID), at most 40 bytes (never a payload).
inline std::string verb_of(const std::string& line) {
  std::size_t e = line.find(' ');
  if (e != std::string::npos) {
    const std::size_t e2 = line.find(' ', e + 1);
    const std::size_t len2 = (e2 == std::string::npos ? line.size() : e2) - (e + 1);
    e = len2 <= 12 ? e2 : e;
  }
  std::string v = line.substr(0, e == std::string::npos ? line.size() : e);
  if (v.size() > 40) v.resize(40);
  for (char& c : v) if (c == '\n' || c == '\r') c = ' ';
  return v;
}

inline bool parse_hostport(const std::string& v, std::string& host, unsigned& port) {
  auto colon = v.rfind(':');
  if (colon == std::string::npos) return false;
  host = v.substr(0, colon);
  port = static_cast<unsigned>(std::atoi(v.c_str() + colon + 1));
  return port != 0;
}

inline int listen_on(unsigned port, int backlog = 16) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, backlog) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Waits up to timeout_ms for fd to be ready for events.
inline bool wait_fd(int fd, short events, int timeout_ms) {
  pollfd p{fd, events, 0};
  int r;
  do r = ::poll(&p, 1, timeout_ms);
  while (r < 0 && errno == EINTR);
  return r > 0;
}

// Reads one line (without the newline) within timeout_ms. One request or
// reply per connection, so whatever follows the line is not kept. Lines
// carry a placement map (a few KiB) or a slot's objects (docs/MIGRATION.md).
inline bool read_line(int fd, std::string& out, int timeout_ms) {
  out.clear();
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[65536];
  while (out.size() < (std::size_t{1} << 28)) {
    int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    end - std::chrono::steady_clock::now()).count());
    if (left <= 0 || !wait_fd(fd, POLLIN, left)) return false;
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) return false;
    if (const void* nl = std::memchr(buf, '\n', static_cast<std::size_t>(r))) {
      out.append(buf, static_cast<std::size_t>(static_cast<const char*>(nl) - buf));
      return true;
    }
    out.append(buf, static_cast<std::size_t>(r));
  }
  return false;
}

inline bool write_all(int fd, const std::string& s) {
  std::size_t done = 0;
  while (done < s.size()) {
    ssize_t w = ::send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return false;
    }
    done += static_cast<std::size_t>(w);
  }
  return true;
}

// The SNC_TRACE=1 line of one request (A2(a)): caller path, verb, port,
// conn=oneshot or conn=kept, result.
inline void trace_ask(const std::string& line, unsigned port, const char* conn, bool ok) {
  if (!::snc::trace::enabled()) return;
  std::string c = caller_path().empty() ? "-" : caller_path();
  for (char& ch : c) if (ch == ' ') ch = '_';  // one token: "serve PLACEMENT SYNC" -> serve_PLACEMENT_SYNC
  ::snc::trace::emit("ha_ask caller=%s verb=%s port=%u conn=%s ok=%d", c.c_str(), verb_of(line).c_str(), port,
                     conn, ok ? 1 : 0);
}

// One connection: connect, send `line`, read one reply line (ask's body).
inline bool ask_once(const std::string& host, unsigned port, const std::string& line,
                     std::string& reply, int timeout_ms);

// Sends `line` to host:port and reads the reply, all within timeout_ms. One
// connection per request (docs/CONTROL_CONNECTIONS_DESIGN.md). With
// SNC_TRACE=1, one "ha_ask" line per call: caller path, verb, port, result.
inline bool ask(const std::string& host, unsigned port, const std::string& line,
                std::string& reply, int timeout_ms) {
  const bool ok = ask_once(host, port, line, reply, timeout_ms);
  trace_ask(line, port, "oneshot", ok);
  return ok;
}

inline bool ask_once(const std::string& host, unsigned port, const std::string& line,
                     std::string& reply, int timeout_ms) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
    ::close(fd);
    return false;
  }
  int r = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  if (r != 0 && errno != EINPROGRESS) {
    ::close(fd);
    return false;
  }
  int err = 0;
  socklen_t len = sizeof(err);
  if (r != 0 && (!wait_fd(fd, POLLOUT, timeout_ms) ||
                 ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)) {
    ::close(fd);
    return false;
  }
  int fl = ::fcntl(fd, F_GETFL);
  ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  bool ok = write_all(fd, line + "\n") && read_line(fd, reply, timeout_ms);
  ::close(fd);
  return ok;
}

// ---- Kept connections (K0) ------------------------------------------------
//
// Off with SNC_HA_KEEP=0 in the environment: every request is one-shot.
inline bool keep_enabled() {
  static const bool on = [] {
    const char* v = std::getenv("SNC_HA_KEEP");
    return v == nullptr || std::strcmp(v, "0") != 0;
  }();
  return on;
}

// Verbs the library may resend once, one-shot, after a kept request failed
// with no reply byte on a reused connection (design 4.2, 4.7). Read-only
// only; no other verb, idempotent or not, is ever resent.
inline bool read_only_verb(const std::string& line) {
  for (const std::string v : {"MOVE INFO", "MOVE ROLE", "MOVE HAVE", "READ", "PLACEMENT GET"}) {
    // The verb alone ("MOVE ROLE") or followed by its arguments.
    if (line.rfind(v, 0) == 0 && (line.size() == v.size() || line[v.size()] == ' ')) return true;
  }
  return false;
}

// Closes with RST (SO_LINGER {1, 0}): ends the client's side at once and
// stops its retransmissions. Hygiene only: a request the server already
// received may still run (design 4.5).
inline void close_rst(int fd) {
  linger l{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
  ::close(fd);
}

// One kept connection to one admin port, for an eligible caller only
// (design 4.2's closed list; in K0 the test_migrate probe and node_info).
//
//   - Never on a request's path: a request with no confirmed connection
//     goes one-shot (ask), and a background thread runs the "KEEP 1"
//     handshake; only later requests use the result. A failed handshake
//     (old server, --no-keep, NO busy, timeout) is retried at most once per
//     60 s, and nothing prefixed is ever sent to a server that did not
//     answer "OK keep" on that connection.
//   - One request at a time: a caller that finds it in use goes one-shot.
//   - A request is "K <seq> <line>"; the reply must be "<seq> <reply>" with
//     nothing after it. On timeout, short reply, EOF, error, a wrong
//     sequence number or stray bytes, the connection is closed with RST and
//     never reused, and the request returns false: false means "unknown",
//     never "not executed".
//   - One resend, one-shot within the same deadline, only for a read-only
//     verb, on a connection that had answered before, when no reply byte
//     arrived (a server's idle close crossing the request).
//   - Idle 20 s: closed by the client, before the server's 30 s.
class Conn {
 public:
  static constexpr int kIdleMs = 20000;
  static constexpr int kRetryMs = 60000;
  static constexpr int kHandshakeMs = 1000;

  Conn(std::string host, unsigned port) : host_(std::move(host)), port_(port) {}
  ~Conn() {
    std::thread t;
    {
      std::lock_guard<std::mutex> g(mu_);
      stop_ = true;
      t = std::move(hs_);
    }
    if (t.joinable()) t.join();
    if (fd_ >= 0) ::close(fd_);
  }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  bool ask(const std::string& line, std::string& reply, int timeout_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    int fd = -1;
    bool reused = false;
    {
      std::unique_lock<std::mutex> g(mu_);
      if (fd_ >= 0 && !in_use_) {
        const bool stale = t0 - idle_since_ >= std::chrono::milliseconds(kIdleMs);
        if (stale || !idle_ok(fd_)) {
          close_rst(fd_);  // idle too long, or the server closed it (EOF, RST, stray bytes)
          fd_ = -1;
        } else {
          fd = fd_;
          reused = used_;
          in_use_ = true;
        }
      }
      if (fd < 0) {
        start_handshake(g);
        g.unlock();
        return ha::ask(host_, port_, line, reply, timeout_ms);
      }
    }
    bool any_byte = false;
    const bool ok = request(fd, line, reply, timeout_ms, any_byte);
    trace_ask(line, port_, "kept", ok);
    {
      std::unique_lock<std::mutex> g(mu_);
      in_use_ = false;
      if (ok) {
        used_ = true;
        idle_since_ = std::chrono::steady_clock::now();
      } else {
        close_rst(fd_);
        fd_ = -1;
        start_handshake(g, /*now_ok=*/true);
      }
    }
    if (ok) return true;
    if (reused && !any_byte && read_only_verb(line)) {
      const int left = timeout_ms - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         std::chrono::steady_clock::now() - t0)
                                                         .count());
      if (left > 0) return ha::ask(host_, port_, line, reply, left);
    }
    return false;
  }

  // Tests: whether a confirmed connection is held now.
  bool kept() {
    std::lock_guard<std::mutex> g(mu_);
    return fd_ >= 0;
  }

 private:
  // An idle connection is fine if nothing is readable and no error.
  static bool idle_ok(int fd) {
    pollfd p{fd, POLLIN, 0};
    int r;
    do r = ::poll(&p, 1, 0);
    while (r < 0 && errno == EINTR);
    return r == 0;
  }

  // Starts the background handshake if none runs, none is held, and the
  // last failure is 60 s old (now_ok: a request failed on a confirmed
  // connection; the server did support keep-alive, so try again at once).
  void start_handshake(std::unique_lock<std::mutex>& g, bool now_ok = false) {
    (void)g;
    if (!keep_enabled() || stop_ || handshaking_ || fd_ >= 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (!now_ok && now < next_try_) return;
    if (hs_.joinable()) hs_.join();  // finished: handshaking_ is false
    try {
      hs_ = std::thread([this] { handshake(); });
      handshaking_ = true;
    } catch (const std::system_error&) {
      next_try_ = now + std::chrono::milliseconds(kRetryMs);  // as a failed handshake; the request goes one-shot
    }
  }

  // One kHandshakeMs budget for the connect, the send and the reply
  // together, so a silent server holds the destructor at most that long.
  void handshake() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeMs);
    const auto left = [&] {
      return std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              end - std::chrono::steady_clock::now())
                                              .count()));
    };
    int fd = connect_to(kHandshakeMs);
    bool ok = false;
    if (fd >= 0) {
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      std::string got;
      bool any = false;
      ok = send_by(fd, "KEEP 1\n", left()) && read_reply(fd, got, left(), any) && got == "OK keep";
    }
    trace_ask("KEEP 1", port_, "handshake", ok);  // A2(a): a KEEP is visible in the trace
    std::lock_guard<std::mutex> g(mu_);
    handshaking_ = false;
    if (ok && !stop_) {
      fd_ = fd;
      used_ = false;
      idle_since_ = std::chrono::steady_clock::now();
      return;
    }
    if (fd >= 0) close_rst(fd);
    next_try_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRetryMs);
  }

  int connect_to(int timeout_ms) const {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, host_.c_str(), &a.sin_addr) != 1) {
      ::close(fd);
      return -1;
    }
    const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    int err = 0;
    socklen_t len = sizeof(err);
    if (r != 0 && (errno != EINPROGRESS || !wait_fd(fd, POLLOUT, timeout_ms) ||
                   ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }

  // All of s by the deadline, without blocking past it (the socket is
  // non-blocking).
  static bool send_by(int fd, const std::string& s, int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t off = 0;
    while (off < s.size()) {
      const ssize_t r = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
        if (left <= 0 || !wait_fd(fd, POLLOUT, left)) return false;
        continue;
      }
      if (r <= 0) return false;
      off += static_cast<std::size_t>(r);
    }
    return true;
  }

  // One reply line within timeout_ms; false if anything follows its
  // newline (the server sends one reply per request). any: a byte arrived.
  static bool read_reply(int fd, std::string& out, int timeout_ms, bool& any) {
    out.clear();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[65536];
    while (out.size() < (std::size_t{1} << 28)) {
      const int left = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
      if (left <= 0 || !wait_fd(fd, POLLIN, left)) return false;
      const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (r <= 0) return false;
      any = true;
      out.append(buf, static_cast<std::size_t>(r));
      const auto nl = out.find('\n');
      if (nl != std::string::npos) {
        if (nl + 1 != out.size()) return false;  // stray bytes after the reply
        out.resize(nl);
        return true;
      }
    }
    return false;
  }

  // One request on the kept connection fd: send bounded by timeout_ms,
  // then the reply within timeout_ms of the send's end (design 4.2).
  bool request(int fd, const std::string& line, std::string& reply, int timeout_ms, bool& any) {
    const std::string seq = std::to_string(++seq_);
    const unsigned uto = static_cast<unsigned>(std::max(1, timeout_ms));
    ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &uto, sizeof(uto));
    if (!send_by(fd, "K " + seq + " " + line + "\n", timeout_ms)) return false;
    std::string got;
    if (!read_reply(fd, got, timeout_ms, any)) return false;
    if (got.size() < seq.size() + 1 || got.compare(0, seq.size(), seq) != 0 || got[seq.size()] != ' ') {
      return false;  // not this request's reply
    }
    reply = got.substr(seq.size() + 1);
    return true;
  }

  const std::string host_;
  const unsigned port_;
  std::mutex mu_;
  int fd_ = -1;  // the confirmed connection, while not in use or in use by one request
  bool in_use_ = false, used_ = false, handshaking_ = false, stop_ = false;
  // Per Conn, not per connection: numbers never repeat across reconnects
  // (a stricter form of design 4.1). Only the request holding in_use_
  // touches it.
  unsigned long long seq_ = 0;
  std::chrono::steady_clock::time_point idle_since_{}, next_try_{};
  std::thread hs_;
};

}  // namespace snc::ha
