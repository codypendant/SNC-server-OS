// The control plane's client side (docs/CONTROL_PLANE.md sections 7 and 9):
// fetch the placement map from the control members (the witnesses), write
// the first one (snc_ctl init), and set a node up from it at start. The
// node keeps its last map and its NodeID in its data directory.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
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
// That is provisional (docs/CONTROL_PLANE.md section 9) and safe only while
// slots never move. False, with `why`, if the node may not start.
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
// directory is initialised with the group's space UUID.
inline void apply(const NodeSetup& s, Engine::Config& c) {
  c.cluster = s.placement.cluster_groups();
  c.cluster_slots.assign(s.placement.slots.begin(), s.placement.slots.end());
  c.cluster_epoch = s.placement.epoch;
  c.group_id = s.group;
  c.member = s.member;
  c.init_uuid = s.placement.groups[s.group].space;
}

}  // namespace snc::control
