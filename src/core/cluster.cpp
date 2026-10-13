#include "snc/cluster.h"

#include <xxhash.h>

#include <stdexcept>

namespace snc {

ClusterMap ClusterMap::make(const std::vector<ClusterNode>& nodes) {
  if (nodes.empty() || nodes.size() > kMaxNodes) throw std::invalid_argument("cluster: 1 to 255 nodes");
  ClusterMap m;
  m.nodes = nodes;
  u64 total = 0;
  for (const ClusterNode& n : nodes) {
    if (n.n_shards == 0) throw std::invalid_argument("cluster: a node with no shards");
    total += n.n_shards;
  }
  u64 before = 0;
  for (u32 i = 0; i < nodes.size(); ++i) {
    const u64 lo = kSlots * before / total;
    before += nodes[i].n_shards;
    const u64 hi = kSlots * before / total;
    for (u64 s = lo; s < hi; ++s) m.node_of[s] = static_cast<u8>(i);
  }
  return m;
}

u32 ClusterMap::crc() const {
  XXH64_state_t* st = XXH64_createState();
  XXH64_reset(st, 0);
  XXH64_update(st, node_of.data(), node_of.size());
  for (const ClusterNode& n : nodes) XXH64_update(st, &n.n_shards, sizeof(n.n_shards));
  const u64 h = XXH64_digest(st);
  XXH64_freeState(st);
  return static_cast<u32>(h ^ (h >> 32));
}

std::vector<ClusterNode> parse_cluster(const std::string& text) {
  std::vector<ClusterNode> out;
  std::size_t at = 0;
  while (at <= text.size()) {
    std::size_t end = text.find(',', at);
    if (end == std::string::npos) end = text.size();
    const std::string e = text.substr(at, end - at);
    // host:link_port:shards[:client_port]; the host may not contain ':'.
    std::vector<std::string> f;
    for (std::size_t p = 0;;) {
      const std::size_t q = e.find(':', p);
      f.push_back(e.substr(p, q == std::string::npos ? std::string::npos : q - p));
      if (q == std::string::npos) break;
      p = q + 1;
    }
    if (f.size() != 3 && f.size() != 4) {
      throw std::invalid_argument("cluster: expected host:link_port:shards[:client_port], got '" + e + "'");
    }
    ClusterNode n;
    n.host = f[0];
    unsigned long port = 0, shards = 0, client = 0;
    try {
      port = std::stoul(f[1]);
      shards = std::stoul(f[2]);
      if (f.size() == 4) client = std::stoul(f[3]);
    } catch (const std::exception&) {
      throw std::invalid_argument("cluster: bad number in '" + e + "'");
    }
    if (n.host.empty() || port == 0 || port > 65535 || shards == 0 || shards > kMaxShards ||
        client > 65535) {
      throw std::invalid_argument("cluster: bad entry '" + e + "'");
    }
    n.link_port = static_cast<u16>(port);
    n.n_shards = static_cast<u16>(shards);
    n.client_port = static_cast<u16>(client);
    out.push_back(n);
    at = end + 1;
  }
  return out;
}

}  // namespace snc
