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
    const std::size_t c2 = e.rfind(':');
    const std::size_t c1 = c2 == std::string::npos || c2 == 0 ? std::string::npos : e.rfind(':', c2 - 1);
    if (c1 == std::string::npos) throw std::invalid_argument("cluster: expected host:port:shards, got '" + e + "'");
    ClusterNode n;
    n.host = e.substr(0, c1);
    const unsigned long port = std::stoul(e.substr(c1 + 1, c2 - c1 - 1));
    const unsigned long shards = std::stoul(e.substr(c2 + 1));
    if (n.host.empty() || port == 0 || port > 65535 || shards == 0 || shards > kMaxShards) {
      throw std::invalid_argument("cluster: bad entry '" + e + "'");
    }
    n.link_port = static_cast<u16>(port);
    n.n_shards = static_cast<u16>(shards);
    out.push_back(n);
    at = end + 1;
  }
  return out;
}

}  // namespace snc
