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

namespace snc::ha {

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

// Sends `line` to host:port and reads the reply, all within timeout_ms.
inline bool ask(const std::string& host, unsigned port, const std::string& line,
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
