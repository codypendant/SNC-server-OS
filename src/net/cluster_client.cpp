// ClusterClient: a Client per replication group, routed by the cluster map.
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
  if (body.size() != sizeof(b) + b.n_members * sizeof(wire::ClusterEntry)) return fail("cluster map: bad length");

  groups_.clear();
  auto make_client = [&] {
    auto c = std::make_unique<Client>();
    if (client_id_ != 0) c->enable_session(client_id_, failover_ms_, request_timeout_ms_);
    return c;
  };
  if (b.n_groups == 0) {
    // Not clustered: one space owns every slot.
    group_of_.fill(0);
    crc_ = 0;
    epoch_ = 0;
    groups_.push_back(make_client());
    if (!groups_[0]->connect(host, base_port)) return fail("group 0: " + groups_[0]->error());
  } else {
    std::memcpy(group_of_.data(), b.group_of, kSlots);
    crc_ = b.crc;
    epoch_ = b.epoch;
    std::vector<std::vector<std::pair<std::string, u16>>> members(b.n_groups);
    for (u16 i = 0; i < b.n_members; ++i) {
      wire::ClusterEntry e;
      std::memcpy(&e, body.data() + sizeof(b) + i * sizeof(e), sizeof(e));
      e.host[sizeof(e.host) - 1] = 0;
      if (e.group >= b.n_groups) return fail("cluster map: a member of no group");
      if (e.client_port == 0) {
        return fail("group " + std::to_string(e.group) + ": a member with no client port in the cluster map");
      }
      members[e.group].emplace_back(e.host, e.client_port);
    }
    for (u16 g = 0; g < b.n_groups; ++g) {
      // Its primary is whichever member answers as one (with a session; a
      // client without one takes the first member that accepts). Each is
      // tried first in turn, the others kept as failover endpoints.
      const auto& m = members[g];
      if (m.empty()) return fail("group " + std::to_string(g) + ": no members");
      std::string why;
      for (std::size_t first = 0; first < m.size() && groups_.size() == g; ++first) {
        auto c = make_client();
        for (std::size_t j = 1; j < m.size(); ++j) {
          const auto& [mh, mp] = m[(first + j) % m.size()];
          c->add_endpoint(mh, mp);
        }
        if (c->connect(m[first].first, m[first].second)) groups_.push_back(std::move(c));
        else why = c->error();
      }
      if (groups_.size() == g) return fail("group " + std::to_string(g) + ": " + why);
    }
  }
  if (epfd_ >= 0) ::close(epfd_);
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  for (u16 g = 0; g < groups_.size(); ++g) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = g;
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, groups_[g]->wait_fd(), &ev);
  }
  return true;
}

void ClusterClient::enable_sessions(u64 client_id, int failover_ms, int request_timeout_ms) {
  client_id_ = client_id;
  failover_ms_ = failover_ms;
  request_timeout_ms_ = request_timeout_ms;
}

void ClusterClient::send(const wire::ReqHeader& h, ByteView args, u16 create_group) {
  const bool anywhere = h.kind == wire::Kind::CREATE && h.target.is_null();
  Client& c = *groups_[anywhere ? create_group : group_of(h.target)];
  // The group's own slot map names the shard (any shard for a create).
  c.send(anywhere ? 0 : c.owner_of(h.target), h, args);
}

int ClusterClient::poll(Client::OnResponse on, void* ctx, int timeout_ms) {
  int got = 0;
  for (auto& c : groups_) {
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
    const int r = groups_[evs[i].data.u32]->poll(on, ctx, 0);
    if (r < 0) {
      error_ = groups_[evs[i].data.u32]->error();
      return -1;
    }
    got += r;
  }
  return got;
}

}  // namespace snc::net
