// ClusterClient: a Client per node, routed by the cluster map.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "snc/net.h"

namespace snc::net {

namespace {
bool io_all(int fd, void* p, std::size_t n, bool write) {
  u8* b = static_cast<u8*>(p);
  while (n > 0) {
    const ssize_t r = write ? ::send(fd, b, n, MSG_NOSIGNAL) : ::recv(fd, b, n, 0);
    if (r <= 0) return false;
    b += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}
}  // namespace

ClusterClient::~ClusterClient() {
  if (epfd_ >= 0) ::close(epfd_);
}

bool ClusterClient::connect(const std::string& host, u16 base_port) {
  auto fail = [&](const std::string& what) {
    error_ = what;
    return false;
  };
  // The map, from shard 0 of the node given.
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(base_port);
  if (fd < 0 || ::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    if (fd >= 0) ::close(fd);
    return fail("connect: " + std::string(std::strerror(errno)));
  }
  wire::ReqHeader q{};
  q.len = sizeof(q);
  q.kind = wire::Kind::CLUSTER;
  wire::RespHeader h{};
  std::vector<u8> body;
  bool ok = io_all(fd, &q, sizeof(q), true) && io_all(fd, &h, sizeof(h), false) &&
            h.kind == wire::Kind::RESP && h.len >= sizeof(h) + sizeof(wire::ClusterBody);
  if (ok) {
    body.resize(h.len - sizeof(h));
    ok = io_all(fd, body.data(), body.size(), false);
  }
  ::close(fd);
  if (!ok) return fail("cluster map: no valid answer");
  wire::ClusterBody b;
  std::memcpy(&b, body.data(), sizeof(b));
  if (body.size() != sizeof(b) + b.n_nodes * sizeof(wire::ClusterEntry)) return fail("cluster map: bad length");

  nodes_.clear();
  if (b.n_nodes == 0) {
    // Not clustered: one node owns every slot.
    node_of_.fill(0);
    crc_ = 0;
    nodes_.push_back(std::make_unique<Client>());
    if (!nodes_[0]->connect(host, base_port)) return fail("node 0: " + nodes_[0]->error());
  } else {
    std::memcpy(node_of_.data(), b.node_of, kSlots);
    crc_ = b.crc;
    for (u16 n = 0; n < b.n_nodes; ++n) {
      wire::ClusterEntry e;
      std::memcpy(&e, body.data() + sizeof(b) + n * sizeof(e), sizeof(e));
      e.host[sizeof(e.host) - 1] = 0;
      if (e.client_port == 0) return fail("node " + std::to_string(n) + ": no client port in the cluster map");
      nodes_.push_back(std::make_unique<Client>());
      if (!nodes_[n]->connect(e.host, e.client_port)) {
        return fail("node " + std::to_string(n) + ": " + nodes_[n]->error());
      }
    }
  }
  if (epfd_ >= 0) ::close(epfd_);
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  for (u16 n = 0; n < nodes_.size(); ++n) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = n;
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, nodes_[n]->wait_fd(), &ev);
  }
  return true;
}

void ClusterClient::send(const wire::ReqHeader& h, ByteView args, u16 create_node) {
  const bool anywhere = h.kind == wire::Kind::CREATE && h.target.is_null();
  Client& c = *nodes_[anywhere ? create_node : node_of(h.target)];
  // The node's own slot map names the shard (any shard for a create).
  c.send(anywhere ? 0 : c.owner_of(h.target), h, args);
}

int ClusterClient::poll(Client::OnResponse on, void* ctx, int timeout_ms) {
  int got = 0;
  for (auto& c : nodes_) {
    const int r = c->poll(on, ctx, 0);  // also sends what is queued
    if (r < 0) {
      error_ = c->error();
      return -1;
    }
    got += r;
  }
  if (got > 0 || timeout_ms == 0) return got;
  epoll_event evs[16];
  const int n = ::epoll_wait(epfd_, evs, 16, timeout_ms);
  for (int i = 0; i < n; ++i) {
    const int r = nodes_[evs[i].data.u32]->poll(on, ctx, 0);
    if (r < 0) {
      error_ = nodes_[evs[i].data.u32]->error();
      return -1;
    }
    got += r;
  }
  return got;
}

}  // namespace snc::net
