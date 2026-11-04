// The control plane's client side (docs/CONTROL_PLANE.md sections 7 and 9):
// fetch the placement map from the control members (the witnesses), write
// the first one (snc_ctl init), and set a node up from it at start. The
// node keeps its last map and its NodeID in its data directory.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ha.h"
#include "snc/placement.h"
#include "snc/snc.h"

namespace snc::control {

using Members = std::vector<std::pair<std::string, unsigned>>;

// "HOST:PORT,HOST:PORT,..."
inline bool parse_members(const std::string& text, Members& out) {
  std::istringstream in(text);
  std::string hp, h;
  unsigned p = 0;
  out.clear();
  while (std::getline(in, hp, ',')) {
    if (!ha::parse_hostport(hp, h, p)) return false;
    out.emplace_back(h, p);
  }
  return !out.empty();
}

enum class Got { MAP, NONE, UNREACHABLE };

// Asks the members in turn; the first that answers for a majority decides
// (each reads the latest placement a majority holds).
inline Got get(const Members& ms, Placement& out, std::string& why) {
  why = "no control member answered";
  for (const auto& [h, p] : ms) {
    std::string reply;
    if (!ha::ask(h, p, "PLACEMENT GET", reply, 2000)) continue;
    if (reply == "NONE") return Got::NONE;
    if (reply.rfind("MAP ", 0) == 0) {
      if (auto pl = Placement::decode(reply.substr(4))) {
        out = *pl;
        return Got::MAP;
      }
      why = "a malformed map from " + h + ":" + std::to_string(p);
      continue;
    }
    why = h + ":" + std::to_string(p) + ": " + reply;
  }
  return Got::UNREACHABLE;
}

// The placement, linearizably (PLACEMENT SYNC): the control plane's latest,
// with the placement machine's version. Decisions that must not rest on a
// stale view use it: unsealing, the driver's next step, and the start
// barrier (docs/MIGRATION_FAIL_CLOSED.md 5a).
inline bool sync(const Members& ms, u64& version, Placement& out, std::string& why) {
  why = "no control member answered";
  for (const auto& [h, port] : ms) {
    std::string reply;
    if (!ha::ask(h, port, "PLACEMENT SYNC", reply, 4000)) continue;
    std::istringstream in(reply);
    std::string tag;
    if (!(in >> tag) || tag != "STATE" || !(in >> version)) {
      why = h + ":" + std::to_string(port) + ": " + reply;
      continue;
    }
    std::string body;
    std::getline(in, body);
    const auto b = body.find_first_not_of(' ');
    body = b == std::string::npos ? "" : body.substr(b);
    auto p = Placement::decode(body);
    if (!p) {
      why = "no placement (or a malformed one) in the control plane";
      return false;
    }
    out = std::move(*p);
    return true;
  }
  return false;
}

// "OK" once the placement is chosen; "NO ..." otherwise (already
// initialised with another map, no quorum, nobody answered).
inline std::string init(const Members& ms, const Placement& p) {
  std::string last = "NO no control member answered";
  for (const auto& [h, port] : ms) {
    std::string reply;
    if (!ha::ask(h, port, "PLACEMENT INIT " + p.encode(), reply, 5000)) continue;
    if (reply == "OK" || reply.rfind("NO already", 0) == 0) return reply;
    last = reply;
  }
  return last;
}

// A block of mint leases from the control plane (the `leases` machine):
// [lo, hi), at least `want` and normally kLeaseBlock, given to no one else.
// Tries the members in turn for up to wait_ms.
inline constexpr u64 kLeaseBlock = u64{1} << 16;

inline bool take_leases(const Members& ms, u64 want, u64& lo, u64& hi, int wait_ms = 5000) {
  const u64 count = std::max(want, kLeaseBlock);
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  do {
    for (const auto& [h, p] : ms) {
      std::string reply;
      if (!ha::ask(h, p, "LEASES TAKE " + std::to_string(count), reply, 4000)) continue;
      unsigned long long a = 0, b = 0;
      if (std::sscanf(reply.c_str(), "BLOCK %llu %llu", &a, &b) == 2 && b >= a + want) {
        lo = a;
        hi = b;
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (std::chrono::steady_clock::now() < end);
  return false;
}

// ---- A node's start ---------------------------------------------------------------------

inline std::string read_small(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

// Written whole and synced, then renamed into place.
inline bool write_synced(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const std::string s = text + "\n";
  const bool ok = ::write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) return false;
  const std::string dir = path.substr(0, path.rfind('/'));
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

struct NodeSetup {
  Placement placement;
  u16 group = 0, member = 0;
  bool stored = false;  // the control plane did not answer: the map kept on disk
};

// Node `node_id` with data in `data_dir`: fetches the placement (trying for
// up to wait_ms), checks it names this node, and keeps it and the NodeID in
// the data directory. Unreachable control plane: the stored map, if any.
// Safe with slot moves: seals and arrival proofs, not the map alone, decide
// what a node serves (docs/MIGRATION.md, docs/MIGRATION_FAIL_CLOSED.md); a
// directory that was last a standby does not start on the stored map at
// all (the start barrier, in Engine::start). False, with `why`, if the node may not start.
inline bool resolve(const Members& ms, const std::string& data_dir, u32 node_id, int wait_ms, NodeSetup& out,
                    std::string& why) {
  ::mkdir(data_dir.c_str(), 0755);
  const std::string node_path = data_dir + "/node", map_path = data_dir + "/placement";
  const std::string had = read_small(node_path);
  if (!had.empty() && had != std::to_string(node_id)) {
    why = "this data directory belongs to node " + had;
    return false;
  }
  std::optional<Placement> stored;
  if (const std::string s = read_small(map_path); !s.empty()) {
    stored = Placement::decode(s);
    if (!stored) {
      why = "the stored placement map is malformed: " + map_path;
      return false;
    }
  }
  Placement p;
  Got got = Got::UNREACHABLE;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  while ((got = get(ms, p, why)) == Got::UNREACHABLE && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (got == Got::NONE) {
    why = "the cluster has no placement yet (snc_ctl init)";
    return false;
  }
  if (got == Got::UNREACHABLE) {
    if (!stored) {
      why += ", and no map is stored";
      return false;
    }
    p = *stored;
    out.stored = true;
  } else if (stored) {
    if (stored->cluster_id != p.cluster_id) {
      why = "the control plane serves another cluster than the stored map's";
      return false;
    }
    if (stored->epoch > p.epoch) {
      why = "the stored map is newer than the control plane's";
      return false;
    }
  }
  if (!p.find(node_id, out.group, out.member)) {
    why = "node " + std::to_string(node_id) + " is in no group of the placement map";
    return false;
  }
  if (!out.stored && !write_synced(map_path, p.encode())) {
    why = "cannot write " + map_path;
    return false;
  }
  if (had.empty() && !write_synced(node_path, std::to_string(node_id))) {
    why = "cannot write " + node_path;
    return false;
  }
  out.placement = std::move(p);
  return true;
}

// The engine's cluster settings from a resolved placement. A new data
// directory is initialised with the group's space UUID; mint leases come in
// cluster-wide blocks from the control members; newer placements (a slot
// moved, docs/MIGRATION.md) are fetched from them and kept in the data
// directory, newest only.
inline void apply(const NodeSetup& s, const Members& ms, Engine::Config& c) {
  c.cluster = s.placement.cluster_groups();
  c.cluster_slots.assign(s.placement.slots.begin(), s.placement.slots.end());
  c.cluster_epoch = s.placement.epoch;
  c.cluster_arrived.assign(s.placement.arrived.begin(), s.placement.arrived.end());
  c.group_id = s.group;
  c.member = s.member;
  c.init_uuid = s.placement.groups[s.group].space;
  c.lease_blocks = [ms](u64 want, u64& lo, u64& hi) { return take_leases(ms, want, lo, hi); };
  auto stored = std::make_shared<std::atomic<u64>>(s.placement.epoch);
  const std::string path = c.data_dir + "/placement";
  const Uuid cluster = s.placement.cluster_id;
  c.placement_fetch = [ms, stored, path, cluster](Placement& out) {
    Placement p;
    std::string why;
    if (get(ms, p, why) != Got::MAP || p.cluster_id != cluster) return false;
    // A majority read may be behind one this node had: keep only newer.
    // Written as read: a v2 placement stays v2 with its arrivals.
    u64 cur = stored->load();
    if (p.epoch > cur && write_synced(path, p.encode())) stored->compare_exchange_strong(cur, p.epoch);
    out = std::move(p);
    return true;
  };
  c.placement_sync = [ms, stored, path, cluster](Placement& out) {
    u64 version = 0;
    Placement p;
    std::string why;
    if (!sync(ms, version, p, why)) {
      std::fprintf(stderr, "control: no linearizable placement read: %s\n", why.c_str());
      return false;
    }
    if (p.cluster_id != cluster) {
      std::fprintf(stderr, "control: the control plane serves another cluster\n");
      return false;
    }
    u64 cur = stored->load();
    if (p.epoch > cur && write_synced(path, p.encode())) stored->compare_exchange_strong(cur, p.epoch);
    out = std::move(p);
    return true;
  };
}

}  // namespace snc::control
