// snc_ctl: the cluster's placement map (docs/CONTROL_PLANE.md).
//
//   snc_ctl --control HOST:PORT,... get
//       prints the map the control plane holds (one line), or "none".
//   snc_ctl --control HOST:PORT,... init --nodes ID@HOST:ADMIN,...
//                                         --groups GROUP,GROUP,...
//       writes the first map (placement epoch 1): a random cluster ID, the
//       nodes, and one replication group per GROUP, each
//       SHARDS:NODE/LINK/CLIENT+NODE/LINK/CLIENT... (members in order; which
//       is primary is HA state, not placement), with a random space UUID.
//       Slots are split in contiguous ranges by shard count. Refused once a
//       map exists (the same map again is accepted).
//
// --control lists the control members (the witnesses). Nodes are then
// started with snc_server --control ... --node-id ID.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "control.h"

using namespace snc;

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string x; std::getline(in, x, sep);) out.push_back(x);
  return out;
}

int usage() {
  std::fprintf(stderr,
               "usage: snc_ctl --control HOST:PORT,... get\n"
               "       snc_ctl --control HOST:PORT,... init --nodes ID@HOST:ADMIN,... "
               "--groups SHARDS:NODE/LINK/CLIENT+...,...\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  control::Members ms;
  std::string cmd, nodes_arg, groups_arg;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "get" || k == "init") {
      cmd = k;
      continue;
    }
    if (i + 1 >= argc) return usage();
    const std::string v = argv[++i];
    if (k == "--control") {
      if (!control::parse_members(v, ms)) return usage();
    } else if (k == "--nodes") {
      nodes_arg = v;
    } else if (k == "--groups") {
      groups_arg = v;
    } else {
      return usage();
    }
  }
  if (ms.empty() || cmd.empty()) return usage();

  if (cmd == "get") {
    Placement p;
    std::string why;
    switch (control::get(ms, p, why)) {
      case control::Got::MAP: std::printf("%s\n", p.encode().c_str()); return 0;
      case control::Got::NONE: std::printf("none\n"); return 0;
      case control::Got::UNREACHABLE: std::fprintf(stderr, "snc_ctl: %s\n", why.c_str()); return 1;
    }
  }

  // init
  std::vector<PlacementNode> nodes;
  for (const std::string& e : split(nodes_arg, ',')) {
    const auto at = e.find('@');
    PlacementNode n;
    unsigned port = 0;
    if (at == std::string::npos || !ha::parse_hostport(e.substr(at + 1), n.host, port) || port > 65535) {
      std::fprintf(stderr, "snc_ctl: bad node '%s' (ID@HOST:ADMIN)\n", e.c_str());
      return 2;
    }
    n.id = static_cast<u32>(std::stoul(e.substr(0, at)));
    n.admin_port = static_cast<u16>(port);
    nodes.push_back(n);
  }
  std::vector<PlacementGroup> groups;
  for (const std::string& e : split(groups_arg, ',')) {
    const auto colon = e.find(':');
    PlacementGroup g;
    if (colon == std::string::npos) {
      std::fprintf(stderr, "snc_ctl: bad group '%s' (SHARDS:NODE/LINK/CLIENT+...)\n", e.c_str());
      return 2;
    }
    g.n_shards = static_cast<u16>(std::stoul(e.substr(0, colon)));
    g.space = random_uuid();
    for (const std::string& m : split(e.substr(colon + 1), '+')) {
      const auto f = split(m, '/');
      if (f.size() != 3) {
        std::fprintf(stderr, "snc_ctl: bad member '%s' (NODE/LINK/CLIENT)\n", m.c_str());
        return 2;
      }
      g.members.push_back({static_cast<u32>(std::stoul(f[0])), static_cast<u16>(std::stoul(f[1])),
                           static_cast<u16>(std::stoul(f[2]))});
    }
    groups.push_back(std::move(g));
  }
  Placement p;
  try {
    p = Placement::initial(random_uuid(), nodes, groups);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "snc_ctl: %s\n", e.what());
    return 2;
  }
  const std::string r = control::init(ms, p);
  std::printf("%s\n", r == "OK" ? p.encode().c_str() : r.c_str());
  return r == "OK" ? 0 : 1;
}
