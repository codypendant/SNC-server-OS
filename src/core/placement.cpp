#include "snc/placement.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <stdexcept>

namespace snc {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  for (std::size_t p = 0;;) {
    const std::size_t q = s.find(sep, p);
    out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) return out;
    p = q + 1;
  }
}

std::string hex(const Uuid& u) {
  std::string s;
  char b[3];
  for (u8 x : u) {
    std::snprintf(b, sizeof(b), "%02x", x);
    s += b;
  }
  return s;
}

bool unhex(const std::string& s, Uuid& u) {
  if (s.size() != 32) return false;
  for (std::size_t i = 0; i < 16; ++i) {
    unsigned v = 0;
    if (std::sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
    u[i] = static_cast<u8>(v);
  }
  return true;
}

// A decimal number no larger than `max`.
bool num(const std::string& s, u64 max, u64& out) {
  if (s.empty() || s.size() > 20 || s.find_first_not_of("0123456789") != std::string::npos) return false;
  try {
    out = std::stoull(s);
  } catch (const std::exception&) {
    return false;
  }
  return out <= max;
}

// "A:B:C" into three numbers no larger than 65535 (the first up to u32).
bool member_of(const std::string& s, PlacementMember& m) {
  const auto f = split(s, ':');
  u64 node = 0, link = 0, client = 0;
  if (f.size() != 3 || !num(f[0], 0xffffffffu, node) || !num(f[1], 65535, link) || !num(f[2], 65535, client) ||
      link == 0) {
    return false;
  }
  m = {static_cast<u32>(node), static_cast<u16>(link), static_cast<u16>(client)};
  return true;
}

}  // namespace

Placement Placement::initial(const Uuid& cluster_id, std::vector<PlacementNode> nodes,
                             std::vector<PlacementGroup> groups) {
  Placement p;
  p.cluster_id = cluster_id;
  p.epoch = 1;
  p.nodes = std::move(nodes);
  p.groups = std::move(groups);
  p.slots = ClusterMap::make(p.cluster_groups()).group_of;  // also checks the groups
  if (!decode(p.encode())) throw std::invalid_argument("placement: unknown or repeated node");
  return p;
}

std::string Placement::encode() const {
  std::ostringstream o;
  bool any_arrived = false;
  for (const SlotArrival& a : arrived) any_arrived |= a.move != 0;
  if (version < 2 && any_arrived) throw std::logic_error("placement: v1 cannot carry arrivals");
  o << (version >= 2 ? "v2" : "v1") << " cluster=" << hex(cluster_id) << " epoch=" << epoch << " nodes=";
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    o << (i ? "," : "") << nodes[i].id << '@' << nodes[i].host << ':' << nodes[i].admin_port;
  }
  o << " groups=";
  for (std::size_t g = 0; g < groups.size(); ++g) {
    o << (g ? "," : "") << groups[g].n_shards << '/' << hex(groups[g].space) << '/';
    for (std::size_t m = 0; m < groups[g].members.size(); ++m) {
      const PlacementMember& x = groups[g].members[m];
      o << (m ? "+" : "") << x.node << ':' << x.link_port << ':' << x.client_port;
    }
  }
  o << " slots=";
  for (u32 s = 0; s < kSlots;) {
    u32 e = s;
    while (e < kSlots && slots[e] == slots[s]) ++e;
    o << (s ? "," : "") << unsigned(slots[s]) << '*' << (e - s);
    s = e;
  }
  if (move.state != PlacementMove::State::NONE) {
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", (unsigned long long)move.id);
    o << " move=" << id << ':' << move.slot << ':' << move.from << ':' << move.to << ':' << move.epoch << ':'
      << PlacementMove::name(move.state);
  }
  if (version >= 2 && any_arrived) {
    o << " arrived=";
    bool first = true;
    for (u32 s = 0; s < kSlots; ++s) {
      if (arrived[s].move == 0) continue;
      o << (first ? "" : ",") << s << ':' << std::hex << arrived[s].move << std::dec << ':' << arrived[s].epoch;
      first = false;
    }
  }
  return o.str();
}

const char* PlacementMove::name(State s) {
  switch (s) {
    case State::NONE: return "none";
    case State::PREPARED: return "prepared";
    case State::SEALED: return "sealed";
    case State::STAGED: return "staged";
    case State::COMMITTED: return "committed";
  }
  return "?";
}

std::optional<Placement> Placement::decode(const std::string& text) {
  std::istringstream in(text);
  std::string tok;
  if (!(in >> tok) || (tok != "v1" && tok != "v2")) return std::nullopt;
  Placement p;
  p.version = tok == "v2" ? 2 : 1;
  bool have[5] = {};
  bool seen_arrived = false;
  while (in >> tok) {
    const auto eq = tok.find('=');
    if (eq == std::string::npos) return std::nullopt;
    const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
    u64 n = 0;
    if (k == "cluster") {
      if (!unhex(v, p.cluster_id)) return std::nullopt;
      have[0] = true;
    } else if (k == "epoch") {
      if (!num(v, ~u64{0}, p.epoch) || p.epoch == 0) return std::nullopt;
      have[1] = true;
    } else if (k == "nodes") {
      for (const std::string& e : split(v, ',')) {
        const auto at = e.find('@'), colon = e.rfind(':');
        PlacementNode nd;
        if (at == std::string::npos || colon == std::string::npos || colon < at || !num(e.substr(0, at), 0xffffffffu, n)) {
          return std::nullopt;
        }
        nd.id = static_cast<u32>(n);
        nd.host = e.substr(at + 1, colon - at - 1);
        if (nd.host.empty() || !num(e.substr(colon + 1), 65535, n)) return std::nullopt;
        nd.admin_port = static_cast<u16>(n);
        p.nodes.push_back(nd);
      }
      have[2] = true;
    } else if (k == "groups") {
      for (const std::string& e : split(v, ',')) {
        const auto f = split(e, '/');
        PlacementGroup g;
        if (f.size() != 3 || !num(f[0], kMaxShards, n) || n == 0 || !unhex(f[1], g.space)) return std::nullopt;
        g.n_shards = static_cast<u16>(n);
        for (const std::string& m : split(f[2], '+')) {
          PlacementMember x;
          if (!member_of(m, x)) return std::nullopt;
          g.members.push_back(x);
        }
        if (g.members.empty()) return std::nullopt;
        p.groups.push_back(std::move(g));
      }
      have[3] = true;
    } else if (k == "move") {
      const auto f = split(v, ':');
      u64 slot = 0, from = 0, to = 0, ep = 0;
      if (f.size() != 6 || f[0].size() != 16 || !num(f[1], kSlots - 1, slot) || !num(f[2], kMaxGroups - 1, from) ||
          !num(f[3], kMaxGroups - 1, to) || !num(f[4], ~u64{0}, ep) || from == to) {
        return std::nullopt;
      }
      p.move.id = std::strtoull(f[0].c_str(), nullptr, 16);
      p.move.slot = static_cast<u16>(slot);
      p.move.from = static_cast<u16>(from);
      p.move.to = static_cast<u16>(to);
      p.move.epoch = ep;
      p.move.state = PlacementMove::State::NONE;
      for (auto st : {PlacementMove::State::PREPARED, PlacementMove::State::SEALED, PlacementMove::State::STAGED,
                      PlacementMove::State::COMMITTED}) {
        if (f[5] == PlacementMove::name(st)) p.move.state = st;
      }
      if (p.move.state == PlacementMove::State::NONE || p.move.id == 0) return std::nullopt;
    } else if (k == "arrived") {
      if (p.version < 2 || seen_arrived) return std::nullopt;
      seen_arrived = true;
      long prev = -1;
      for (const std::string& e : split(v, ',')) {
        const auto f = split(e, ':');
        u64 slot = 0, ep = 0;
        // Canonical only (as encode writes them): no leading zeros.
        auto canonical = [](const std::string& x) { return x.size() <= 1 || x[0] != '0'; };
        if (f.size() != 3 || !canonical(f[0]) || !canonical(f[1]) || !canonical(f[2])) return std::nullopt;
        if (!num(f[0], kSlots - 1, slot) || static_cast<long>(slot) <= prev || f[1].empty() ||
            f[1].size() > 16 || f[1].find_first_not_of("0123456789abcdef") != std::string::npos ||
            !num(f[2], ~u64{0}, ep) || ep == 0) {
          return std::nullopt;
        }
        const u64 mv = std::strtoull(f[1].c_str(), nullptr, 16);
        if (mv == 0) return std::nullopt;
        p.arrived[slot] = SlotArrival{mv, ep};
        prev = static_cast<long>(slot);
      }
    } else if (k == "slots") {
      u32 at = 0;
      for (const std::string& e : split(v, ',')) {
        const auto star = e.find('*');
        u64 g = 0, cnt = 0;
        if (star == std::string::npos || !num(e.substr(0, star), kMaxGroups - 1, g) ||
            !num(e.substr(star + 1), kSlots, cnt) || cnt == 0 || at + cnt > kSlots) {
          return std::nullopt;
        }
        for (u64 i = 0; i < cnt; ++i) p.slots[at++] = static_cast<u8>(g);
      }
      if (at != kSlots) return std::nullopt;
      have[4] = true;
    } else {
      return std::nullopt;
    }
  }
  for (bool h : have) {
    if (!h) return std::nullopt;
  }
  if (p.groups.empty() || p.groups.size() > kMaxGroups) return std::nullopt;
  std::set<u32> ids, used;
  for (const PlacementNode& nd : p.nodes) {
    if (!ids.insert(nd.id).second) return std::nullopt;
  }
  for (const PlacementGroup& g : p.groups) {
    for (const PlacementMember& m : g.members) {
      // Milestone 2 runs one membership per node (an operational rule, not
      // the schema's): a node in two groups is refused for now.
      if (!ids.count(m.node) || !used.insert(m.node).second) return std::nullopt;
    }
  }
  for (u8 g : p.slots) {
    if (g >= p.groups.size()) return std::nullopt;
  }
  for (const SlotArrival& a : p.arrived) {
    if (a.epoch > p.epoch) return std::nullopt;  // a commit is never ahead of the placement
  }
  if (p.move.state != PlacementMove::State::NONE &&
      (p.move.from >= p.groups.size() || p.move.to >= p.groups.size())) {
    return std::nullopt;
  }
  return p;
}

const PlacementNode* Placement::node(u32 id) const {
  for (const PlacementNode& n : nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

bool Placement::find(u32 id, u16& group, u16& member) const {
  for (std::size_t g = 0; g < groups.size(); ++g) {
    for (std::size_t m = 0; m < groups[g].members.size(); ++m) {
      if (groups[g].members[m].node == id) {
        group = static_cast<u16>(g);
        member = static_cast<u16>(m);
        return true;
      }
    }
  }
  return false;
}

std::vector<ClusterGroup> Placement::cluster_groups() const {
  std::vector<ClusterGroup> out;
  for (const PlacementGroup& g : groups) {
    ClusterGroup c;
    c.n_shards = g.n_shards;
    for (const PlacementMember& m : g.members) {
      const PlacementNode* nd = node(m.node);
      c.members.push_back({nd ? nd->host : std::string(), m.link_port, m.client_port});
    }
    out.push_back(std::move(c));
  }
  return out;
}

ClusterMap Placement::map() const {
  ClusterMap m = ClusterMap::assign(cluster_groups(), slots, epoch);
  m.arrived = arrived;
  return m;
}

}  // namespace snc
