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
//   snc_ctl --control HOST:PORT,... move SLOT GROUP
//       moves SLOT to replication group GROUP (docs/MIGRATION.md), or
//       carries on the move of SLOT in progress; prints DONE, ABORTED,
//       BUSY ..., or STUCK <why> (then run it again: every step resumes).
//   snc_ctl --control HOST:PORT,... [--placement-v2] placement backfill
//       sets the arrival (placement v2) of slots that moved before v2, from
//       unambiguous evidence only (docs/MIGRATION_FAIL_CLOSED.md 15); prints
//       BACKFILLED N at epoch E, or NOTHING, and the slots it could not
//       settle (UNRESOLVED). Needs --placement-v2: it writes v2.
//   --placement-v2 (with move or init): write placement v2, recording each
//       commit's arrival. Only once every node, driver and snc_ctl reads v2;
//       a placement already v2 stays v2 whatever the flag. There is no
//       command back to v1 (docs/MIGRATION_FAIL_CLOSED.md 15, "Rollback").
//   snc_ctl --control HOST:PORT,... move --abort SLOT
//       aborts the move of SLOT if it has not committed (ABORTED, NONE,
//       or "NO committed": then only finishing it with move is possible).
//
// --control lists the control members (the witnesses). Nodes are then
// started with snc_server --control ... --node-id ID.
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "control.h"
#include "migrate.h"

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
               "--groups SHARDS:NODE/LINK/CLIENT+...,...\n"
               "       snc_ctl --control HOST:PORT,... [--placement-v2] move [--stop-after STEP] [--chunk N] SLOT GROUP | move --abort SLOT\n"
               "       snc_ctl --control HOST:PORT,... --placement-v2 placement backfill\n"
               "STEP (for crash tests): prepared copying copied sealed staged committed activated\n"
               "N: objects per request when copying ahead of the seal (default 8)\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  control::Members ms;
  std::string cmd, nodes_arg, groups_arg;
  std::vector<std::string> move_args;
  bool v2 = false;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--placement-v2") {
      v2 = true;
      continue;
    }
    if (k == "placement" && i + 1 < argc && std::string(argv[i + 1]) == "backfill") {
      cmd = "backfill";
      ++i;
      continue;
    }
    if (k == "get" || k == "init") {
      cmd = k;
      continue;
    }
    if (k == "move") {
      cmd = k;
      for (++i; i < argc; ++i) move_args.push_back(argv[i]);
      break;
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

  if (cmd == "backfill") {
    if (!v2) {
      std::fprintf(stderr, "snc_ctl: backfill writes placement v2: pass --placement-v2 once every node reads v2\n");
      return 2;
    }
    migrate::Driver d(ms);
    d.deadline_ms = 60000;
    const std::string r = d.backfill();
    std::fputs(d.log.c_str(), stderr);
    std::printf("%s\n", r.c_str());
    return r.rfind("BACKFILLED", 0) == 0 || r.rfind("NOTHING", 0) == 0 ? 0 : 1;
  }

  if (cmd == "move") {
    migrate::Driver d(ms);
    d.deadline_ms = 60000;
    d.placement_v2 = v2;
    while (move_args.size() >= 4 && (move_args[0] == "--stop-after" || move_args[0] == "--chunk")) {
      const std::string v = move_args[1];
      if (move_args[0] == "--chunk") d.chunk = std::max(1, std::atoi(v.c_str()));
      else d.stop_at = [v](const std::string& step) { return step == v; };
      move_args.erase(move_args.begin(), move_args.begin() + 2);
    }
    std::string r;
    if (move_args.size() == 2 && move_args[0] == "--abort") {
      r = d.abort(static_cast<u16>(std::stoul(move_args[1])));
    } else if (move_args.size() == 2) {
      const unsigned long slot = std::stoul(move_args[0]);
      if (slot >= kSlots) return usage();
      r = d.move(static_cast<u16>(slot), static_cast<u16>(std::stoul(move_args[1])));
    } else {
      return usage();
    }
    std::fputs(d.log.c_str(), stderr);
    std::printf("%s\n", r.c_str());
    return r == "DONE" || r.rfind("ABORTED", 0) == 0 || r == "NONE" || r.rfind("STOPPED", 0) == 0 ? 0 : 1;
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
    if (v2) p.version = 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "snc_ctl: %s\n", e.what());
    return 2;
  }
  const std::string r = control::init(ms, p);
  std::printf("%s\n", r == "OK" ? p.encode().c_str() : r.c_str());
  return r == "OK" ? 0 : 1;
}
