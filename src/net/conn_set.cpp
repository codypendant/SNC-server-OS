// ConnSet: a listening socket and its connections on one thread.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "snc/net.h"

namespace snc::net {

namespace {
constexpr u32 kReadChunk = 64u << 10;
constexpr std::size_t kMaxQueuedOut = 8u << 20;  // stop reading a client this far behind
constexpr int kMaxEvents = 64;

[[noreturn]] void die(const char* what) {
  std::perror(what);
  std::abort();
}
}  // namespace

struct ConnSet::Conn {
  int fd = -1;
  u32 refs = 1;  // the open connection holds one
  bool closed = false;
  bool dirty = false;
  std::vector<u8> in;
  std::size_t in_len = 0;
  std::vector<u8> out;
  std::size_t out_off = 0;
};

ConnSet::ConnSet(const std::string& host, u16 port, OnRequest on_request, void* user)
    : on_request_(on_request), user_(user) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) die("socket");
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) die("inet_pton");
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) die("bind");
  if (::listen(listen_fd_, 1024) != 0) die("listen");
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) die("epoll_create1");
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.ptr = nullptr;  // the listener
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) != 0) die("epoll_ctl");
}

ConnSet::~ConnSet() {
  for (Conn* c : conns_) {
    ::close(c->fd);
    delete c;
  }
  ::close(epfd_);
  ::close(listen_fd_);
}

void ConnSet::hold(Conn* c) { ++c->refs; }

void ConnSet::release(Conn* c) {
  if (--c->refs == 0) delete c;
}

void ConnSet::accept_all() {
  while (true) {
    int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    auto* c = new Conn;
    c->fd = fd;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = c;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) die("epoll_ctl");
    conns_.push_back(c);
    ++stats_.accepted;
  }
}

void ConnSet::close_conn(Conn* c) {
  if (c->closed) return;
  ::epoll_ctl(epfd_, EPOLL_CTL_DEL, c->fd, nullptr);
  ::close(c->fd);
  c->fd = -1;
  c->closed = true;
  conns_.erase(std::find(conns_.begin(), conns_.end(), c));
  release(c);
}

bool ConnSet::read_conn(Conn* c) {
  if (c->out.size() - c->out_off > kMaxQueuedOut) return false;  // let it drain first
  if (c->in.size() - c->in_len < kReadChunk) c->in.resize(c->in_len + kReadChunk);
  ssize_t n = ::recv(c->fd, c->in.data() + c->in_len, c->in.size() - c->in_len, MSG_DONTWAIT);
  ++stats_.recv_calls;
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
    close_conn(c);
    return true;
  }
  if (n < 0) return false;
  stats_.bytes_in += static_cast<u64>(n);
  c->in_len += static_cast<std::size_t>(n);

  // Dispatch every whole frame. A handler may respond inline, which only
  // appends to c->out, so `c` stays valid; hold it anyway for clarity.
  if (on_batch_) {
    batch_.clear();
    for (std::size_t p = 0; c->in_len - p >= sizeof(wire::ReqHeader);) {
      wire::ReqHeader h;
      std::memcpy(&h, c->in.data() + p, sizeof(h));
      if (h.len < sizeof(h) || c->in_len - p < h.len) break;
      batch_.push_back(h.target);
      p += h.len;
    }
    if (!batch_.empty()) on_batch_(user_, batch_.data(), static_cast<u32>(batch_.size()));
  }
  hold(c);
  std::size_t pos = 0;
  while (!c->closed && c->in_len - pos >= sizeof(u32)) {
    u32 len;
    std::memcpy(&len, c->in.data() + pos, sizeof(len));
    if (len < sizeof(wire::ReqHeader) || len > wire::kMaxFrame) {
      close_conn(c);  // protocol error
      break;
    }
    if (c->in_len - pos < len) break;
    wire::ReqHeader h;
    std::memcpy(&h, c->in.data() + pos, sizeof(h));
    ++stats_.frames_in;
    on_request_(user_, c, h,
                ByteView(c->in.data() + pos + sizeof(h), len - static_cast<u32>(sizeof(h))));
    pos += len;
  }
  if (!c->closed && pos > 0) {
    std::memmove(c->in.data(), c->in.data() + pos, c->in_len - pos);
    c->in_len -= pos;
  }
  release(c);
  return true;
}

bool ConnSet::poll() {
  epoll_event evs[kMaxEvents];
  int n = ::epoll_wait(epfd_, evs, kMaxEvents, 0);
  bool did = false;
  for (int i = 0; i < n; ++i) {
    auto* c = static_cast<Conn*>(evs[i].data.ptr);
    if (c == nullptr) {
      accept_all();
      did = true;
    } else {
      did |= read_conn(c);
    }
  }
  if (!dirty_.empty()) {
    flush();
    did = true;
  }
  return did;
}

void ConnSet::respond(Conn* c, const wire::RespHeader& h, ByteView body) {
  if (c->closed) return;
  const auto* hp = reinterpret_cast<const u8*>(&h);
  c->out.insert(c->out.end(), hp, hp + sizeof(h));
  if (body.len) c->out.insert(c->out.end(), body.data, body.data + body.len);
  ++stats_.frames_out;
  if (!c->dirty) {
    c->dirty = true;
    hold(c);
    dirty_.push_back(c);
  }
}

void ConnSet::flush() {
  std::size_t keep = 0;
  for (std::size_t i = 0; i < dirty_.size(); ++i) {
    Conn* c = dirty_[i];
    bool done = c->closed;
    if (!done) {
      ssize_t n = ::send(c->fd, c->out.data() + c->out_off, c->out.size() - c->out_off,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
      ++stats_.send_calls;
      if (n > 0) {
        stats_.bytes_out += static_cast<u64>(n);
        c->out_off += static_cast<std::size_t>(n);
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_conn(c);
      }
      done = c->closed || c->out_off == c->out.size();
      if (!c->closed && (c->out_off == c->out.size() || c->out_off > (1u << 20))) {
        c->out.erase(c->out.begin(), c->out.begin() + static_cast<std::ptrdiff_t>(c->out_off));
        c->out_off = 0;
      }
    }
    if (done) {
      c->dirty = false;
      release(c);
    } else {
      dirty_[keep++] = c;
    }
  }
  dirty_.resize(keep);
}

}  // namespace snc::net
