#include "snc/cluster.h"

#include <xxhash.h>

#include <stdexcept>

namespace snc {

ClusterMap ClusterMap::make(const std::vector<ClusterGroup>& groups) {
  if (groups.empty() || groups.size() > kMaxGroups) throw std::invalid_argument("cluster: 1 to 255 groups");
  ClusterMap m;
  m.groups = groups;
  u64 total = 0;
  for (const ClusterGroup& g : groups) {
    if (g.n_shards == 0) throw std::invalid_argument("cluster: a group with no shards");
    if (g.members.empty()) throw std::invalid_argument("cluster: a group with no members");
    total += g.n_shards;
  }
  u64 before = 0;
  for (u32 i = 0; i < groups.size(); ++i) {
    const u64 lo = kSlots * before / total;
    before += groups[i].n_shards;
    const u64 hi = kSlots * before / total;
    for (u64 s = lo; s < hi; ++s) m.group_of[s] = static_cast<u8>(i);
  }
  return m;
}

ClusterMap ClusterMap::assign(const std::vector<ClusterGroup>& groups, const std::array<u8, kSlots>& group_of,
                              u64 epoch) {
  ClusterMap m = make(groups);  // checks the groups
  for (u8 g : group_of) {
    if (g >= groups.size()) throw std::invalid_argument("cluster: a slot assigned to no group");
  }
  m.group_of = group_of;
  m.epoch = epoch;
  return m;
}

u32 ClusterMap::crc() const {
  XXH64_state_t* st = XXH64_createState();
  XXH64_reset(st, 0);
  XXH64_update(st, group_of.data(), group_of.size());
  for (const ClusterGroup& g : groups) XXH64_update(st, &g.n_shards, sizeof(g.n_shards));
  const u64 h = XXH64_digest(st);
  XXH64_freeState(st);
  return static_cast<u32>(h ^ (h >> 32));
}

u32 ClusterMap::shape_crc() const {
  XXH64_state_t* st = XXH64_createState();
  XXH64_reset(st, 0x5e1f);
  const u32 n = static_cast<u32>(groups.size());
  XXH64_update(st, &n, sizeof(n));
  for (const ClusterGroup& g : groups) XXH64_update(st, &g.n_shards, sizeof(g.n_shards));
  const u64 h = XXH64_digest(st);
  XXH64_freeState(st);
  return static_cast<u32>(h ^ (h >> 32));
}

namespace {
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  for (std::size_t p = 0;;) {
    const std::size_t q = s.find(sep, p);
    out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) return out;
    p = q + 1;
  }
}
}  // namespace

std::vector<ClusterGroup> parse_cluster(const std::string& text) {
  std::vector<ClusterGroup> out;
  for (const std::string& g : split(text, ',')) {
    ClusterGroup group;
    for (const std::string& e : split(g, '|')) {
      // host:link_port:shards[:client_port]; the host may not contain ':'.
      const std::vector<std::string> f = split(e, ':');
      if (f.size() != 3 && f.size() != 4) {
        throw std::invalid_argument("cluster: expected host:link_port:shards[:client_port], got '" + e + "'");
      }
      unsigned long port = 0, shards = 0, client = 0;
      try {
        port = std::stoul(f[1]);
        shards = std::stoul(f[2]);
        if (f.size() == 4) client = std::stoul(f[3]);
      } catch (const std::exception&) {
        throw std::invalid_argument("cluster: bad number in '" + e + "'");
      }
      if (f[0].empty() || port == 0 || port > 65535 || shards == 0 || shards > kMaxShards || client > 65535) {
        throw std::invalid_argument("cluster: bad entry '" + e + "'");
      }
      if (group.n_shards != 0 && group.n_shards != shards) {
        throw std::invalid_argument("cluster: members of one group with different shard counts in '" + g + "'");
      }
      group.n_shards = static_cast<u16>(shards);
      group.members.push_back({f[0], static_cast<u16>(port), static_cast<u16>(client)});
    }
    out.push_back(std::move(group));
  }
  return out;
}

}  // namespace snc
