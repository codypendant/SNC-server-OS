// Small line protocol shared by snc_server's admin port and snc_witness
// (docs/REPLICATION.md, "Automatic failover"). One request line, one reply
// line, then the connection closes.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>

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
struct Caller {
  std::size_t prev;
  explicit Caller(const char* name) : prev(caller_path().size()) {
    if (prev != 0) caller_path() += '/';
    caller_path() += name;
  }
  // Another thread's path, carried into a helper thread (Paxos broadcast).
  struct Adopt {
    std::string prev;
    explicit Adopt(const std::string& path) : prev(caller_path()) { caller_path() = path; }
    ~Adopt() { caller_path() = prev; }
  };
  ~Caller() { caller_path().resize(prev); }
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

// One connection: connect, send `line`, read one reply line (ask's body).
inline bool ask_once(const std::string& host, unsigned port, const std::string& line,
                     std::string& reply, int timeout_ms);

// Sends `line` to host:port and reads the reply, all within timeout_ms. One
// connection per request (docs/CONTROL_CONNECTIONS_DESIGN.md). With
// SNC_TRACE=1, one "ha_ask" line per call: caller path, verb, port, result.
inline bool ask(const std::string& host, unsigned port, const std::string& line,
                std::string& reply, int timeout_ms) {
  const bool ok = ask_once(host, port, line, reply, timeout_ms);
  if (::snc::trace::enabled()) {
    std::string c = caller_path().empty() ? "-" : caller_path();
    for (char& ch : c) if (ch == ' ') ch = '_';  // one token: "serve PLACEMENT SYNC" -> serve_PLACEMENT_SYNC
    ::snc::trace::emit("ha_ask caller=%s verb=%s port=%u conn=oneshot ok=%d", c.c_str(),
                       verb_of(line).c_str(), port, ok ? 1 : 0);
  }
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

}  // namespace snc::ha
