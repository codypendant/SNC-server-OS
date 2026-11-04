// SNC server: the engine with the socket front end (docs/ROUTING.md). Shard i
// listens on port + i. Runs until SIGINT or SIGTERM, then prints per-shard
// request and forwarding counts.
//
// Usage: snc_server [--shards N] [--cpus 0,1,...] [--host H] [--port P]
//                   [--objects N] [--data-dir DIR] [--group-commit-us N]
//                   [--no-mint-local] [--spin] [--pages 4k|thp|hugetlb]
//                   [--replica-of HOST:PORT [--async-replica] [--replica-timeout-ms N]]
//                   [--standby PORT [--standby-host H] [--space-uuid HEX]
//                    [--seed-from HOST:PORT]] [--seed-port P] [--report-ms N]
//                   [--checkpoint-s N] [--replica-retain-mib N] [--catchup-mib-s N]
//                   [--admin-port P [--debug-admin]] [--witness HOST:PORT] [--standby-reads LEASE_MS]
//                   [--adopt-role primary|standby]
//                   [--rejoin STANDBY_PORT,PEER_HOST:ADMIN,PEER_HOST:SEED]
//                   [--cluster GROUP,GROUP,... --group G [--member M]]
//                   [--control HOST:PORT,... --node-id N]
//
// --control (docs/CLUSTER.md, "Starting from the control plane"): instead
// of --cluster/--group/--member, fetch the placement map from the control
// members (the witnesses; written once by snc_ctl init), find this node's
// group by --node-id, and keep the map in --data-dir; if no control member
// answers within 10 s, start from the stored map (provisional).
//
// Replication (docs/REPLICATION.md): a primary started with --replica-of
// ships shard i's WAL to HOST:PORT+i. A standby started with --standby PORT
// listens on PORT+i, keeps a copy and serves no ops. A new standby is
// created with its primary's space UUID (printed at start). Promote a
// standby with SIGUSR1 (in place; it then ships to --replica-of if given,
// and --standby takes the port to listen on), or by stopping it and starting
// it without --standby on the same --data-dir. Either way the start barrier
// runs first (docs/MIGRATION_FAIL_CLOSED.md 5a, 8): the placement read
// linearizably and adopted, a slot this group is moving away re-sealed,
// and only then role "primary" in DIR/role; a directory last a standby
// does not start while the control plane cannot be read (ROLE m on the
// admin port while an in-place promotion runs it). Promotion starts a new term:
// standbys refuse a primary of an older term, so an old primary that comes
// back with --replica-timeout-ms 0 can acknowledge no durable write.
// A primary started with --seed-port serves its latest checkpoint; a
// standby started with --seed-from on an empty --data-dir fetches it first,
// then catches up from the primary's WAL. --report-ms prints a line of
// replication state every N ms (lag = durable records not yet acked).
// --adopt-role (docs/LEGACY_ROLE.md): a data directory written by a binary
// from before role files (no DIR/role, no role tracking in its superblock)
// may be a standby's, so it does not start as a primary: start it once
// with --standby, or with --adopt-role primary if it is its group's
// primary's own disk. That is refused if the control plane did not answer
// or any other member of the group is silent, serving or promoting, or at
// a newer term; then the start barrier runs, and only its end records the
// directory as a primary's. --adopt-role standby (with --standby) is the
// same as --standby. On a directory with a role on record the flag must
// match it (then it does nothing).
// Slot moves (docs/MIGRATION.md): with --control, --admin-port also answers
// the MOVE requests of snc_ctl move (tools/migrate.h). --admin-port also
// answers AUDIT [PATH] (the accounts served here; docs/REAL_MACHINES.md).
// With --control, clients are served on every interface unless --host is set.
// Automatic failover (snc_witness): --admin-port answers HEALTH, STATUS and
// PROMOTE lines (role p primary with a replication sender, n primary
// without one, s standby, f fenced: a newer primary exists, and this node
// has left the data plane). HEALTH (docs/HEALTH_DECOUPLING.md) answers at
// once from published state, with shard and main-loop progress as
// diagnostics; it is the witness's liveness signal and proves nothing
// about replication or promotion eligibility. STATUS asks every shard for
// its replication state and may be slow. --debug-admin also accepts DEBUG
// STALL <shard> <ms> and DEBUG REPL-PAUSE 1|0, and --debug-barrier hold:MS|crash
// (the end of the start barrier, docs/MIGRATION_FAIL_CLOSED.md; tests only). With --witness, a sync primary past --replica-timeout-ms continues
// without its standby only once the witness grants it, and tells the
// witness when its standby is back in sync. --witness takes a comma-separated
// list of replicated witnesses; each request goes to the first that
// answers. --standby-reads lets a standby
// serve reads under leases from its sync primary (E_STALE without one).
// --rejoin (with --space-uuid): a node started as primary first asks the
// other node of its pair; if that one is now the primary, the node moves
// its data directory aside (DIR.stale-TIME) and starts as that primary's
// standby, listening on STANDBY_PORT and seeded from PEER_HOST:SEED. So a
// supervisor can restart a failed primary with its usual command line.
// Cluster (docs/CLUSTER.md): --cluster lists every replication group, the
// same list on each node; a GROUP is its members separated by '|', each
// HOST:LINK_PORT:SHARDS[:CLIENT_PORT]. --group and --member say which this
// node is (member defaults to 0). The group owns a fixed share of the
// slots, refuses ops on the others' objects (E_MOVED), and sends
// follow-ups for them over node links (shard i of its primary listens on
// LINK_PORT + i). Members of a group replicate as any primary and standby
// (--replica-of, --standby, --witness ...); a promoted standby opens the
// group's links, and the other groups find it there. A CLUSTER request
// returns the map with each member's CLIENT_PORT, so that
// net::ClusterClient sends each request to its owner's primary.
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "admin_loop.h"
#include "control.h"
#include "ha.h"
#include "migrate.h"
#include "snc/net.h"
#include "snc/snc.h"
#include "snc/trace.h"
#include "snc/types/account.h"

using namespace snc;

namespace {

// AUDIT [PATH] (docs/REAL_MACHINES.md): the accounts this node serves now.
// "AUDIT accounts total pending epoch held misrouted", pending being
// unsettled transfer entries (outbox plus inbox). With PATH, also writes
// "ID BALANCE" for each of those accounts to PATH on this host.
std::string audit(Engine& eng, const std::string& path) {
  u64 accounts = 0, pending = 0, held = 0, misrouted = 0;
  i64 total = 0;
  std::string bal;
  for (u16 sh = 0; sh < eng.shard_count(); ++sh) {
    eng.for_each_served_object(sh, [&](const ObjectHeader& h) {
      if (h.type_id != account::kTypeId) return;
      const auto* a = reinterpret_cast<const account::AccountV1*>(h.payload());
      ++accounts;
      total += a->balance_cents;
      pending += a->n_out + a->n_in;
      if (!path.empty()) bal += h.id.to_string() + " " + std::to_string(a->balance_cents) + "\n";
    });
    const ShardStats st = eng.stats(sh);
    held += st.emits_held;
    misrouted += st.node_emits_misrouted;
  }
  if (!path.empty()) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr || std::fwrite(bal.data(), 1, bal.size(), f) != bal.size() || std::fclose(f) != 0) {
      return "NO cannot write " + path;
    }
  }
  return "AUDIT " + std::to_string(accounts) + " " + std::to_string(total) + " " + std::to_string(pending) + " " +
         std::to_string(eng.cluster().epoch) + " " + std::to_string(held) + " " + std::to_string(misrouted);
}

}  // namespace

namespace {

std::vector<int> parse_cpus(const char* s) {
  std::vector<int> v;
  for (const char* p = s; *p;) {
    v.push_back(std::atoi(p));
    while (*p && *p != ',') ++p;
    if (*p == ',') ++p;
  }
  return v;
}

bool parse_uuid(const char* s, Uuid& u) {
  std::string h;
  for (const char* p = s; *p; ++p)
    if (*p != '-') h += *p;
  if (h.size() != 32) return false;
  for (int i = 0; i < 16; ++i) u[i] = static_cast<u8>(std::stoul(h.substr(2 * i, 2), nullptr, 16));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Engine::Config cfg;
  cfg.runtime.n_shards = 0;
  cfg.runtime.ring_slots = 1024;
  cfg.runtime.overflow_limit = 1024;
  cfg.expected_objects = 1000000;
  cfg.mint_local = true;
  cfg.init = true;
  net::NetServer::Config ncfg;
  int report_ms = 0;
  unsigned admin_port = 0;
  bool host_given = false;
  std::vector<std::pair<std::string, unsigned>> witnesses;
  double checkpoint_s = 0;  // checkpoint every this many seconds (0 = never)
  // Rejoin (see the header): the other node's admin and seed ports, and the
  // port to listen on as its standby.
  std::string peer_admin_host, peer_seed;
  unsigned peer_admin_port = 0;
  u16 rejoin_port = 0;
  control::Members control_members;
  long long node_id = -1;
  bool debug_admin = false;  // DEBUG STALL on the admin port (tests only)
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&] {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", k.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--shards") cfg.runtime.n_shards = static_cast<u16>(std::atoi(val()));
    else if (k == "--cpus") cfg.runtime.cpus = parse_cpus(val());
    else if (k == "--host") {
      ncfg.host = val();
      host_given = true;
    }
    else if (k == "--port") ncfg.base_port = static_cast<u16>(std::atoi(val()));
    else if (k == "--objects") cfg.expected_objects = std::strtoull(val(), nullptr, 10);
    else if (k == "--data-dir") cfg.data_dir = val();
    else if (k == "--group-commit-us") cfg.group_commit_us = static_cast<u32>(std::atoi(val()));
    else if (k == "--no-mint-local") cfg.mint_local = false;
    else if (k == "--spin") cfg.runtime.spin_forever = true;
    else if (k == "--replica-of") {
      std::string v = val();
      auto colon = v.rfind(':');
      if (colon == std::string::npos) {
        std::fprintf(stderr, "--replica-of takes HOST:PORT\n");
        return 2;
      }
      cfg.replica_host = v.substr(0, colon);
      cfg.replica_port = static_cast<u16>(std::atoi(v.c_str() + colon + 1));
    }
    else if (k == "--async-replica") cfg.replica_sync = false;
    else if (k == "--replica-timeout-ms") cfg.replica_timeout_ms = static_cast<u32>(std::atoi(val()));
    else if (k == "--standby") {
      cfg.standby = true;
      cfg.standby_port = static_cast<u16>(std::atoi(val()));
    }
    else if (k == "--standby-host") cfg.standby_host = val();
    else if (k == "--seed-port") cfg.seed_port = static_cast<u16>(std::atoi(val()));
    else if (k == "--seed-from") cfg.seed_from = val();
    else if (k == "--cluster") cfg.cluster = parse_cluster(val());
    else if (k == "--group") cfg.group_id = static_cast<u16>(std::atoi(val()));
    else if (k == "--member") cfg.member = static_cast<u16>(std::atoi(val()));
    else if (k == "--control") {
      if (!control::parse_members(val(), control_members)) {
        std::fprintf(stderr, "--control takes HOST:PORT[,HOST:PORT...]\n");
        return 2;
      }
    }
    else if (k == "--node-id") node_id = std::atoll(val());
    else if (k == "--report-ms") report_ms = std::atoi(val());
    else if (k == "--standby-reads") cfg.standby_read_lease_ms = static_cast<u32>(std::atoi(val()));
    else if (k == "--admin-port") admin_port = static_cast<unsigned>(std::atoi(val()));
    else if (k == "--debug-admin") debug_admin = true;
    else if (k == "--adopt-role") {
      // docs/LEGACY_ROLE.md: once, for a data directory from before role files.
      const std::string v = val();
      if (v == "primary") cfg.adopt_role = Engine::Config::AdoptRole::PRIMARY;
      else if (v == "standby") cfg.adopt_role = Engine::Config::AdoptRole::STANDBY;
      else {
        std::fprintf(stderr, "--adopt-role takes primary or standby\n");
        return 2;
      }
    }
    else if (k == "--debug-barrier") {
      // Tests only (needs --debug-admin): hold:MS or crash, at the end of
      // the start barrier before role "primary" (Engine::Config).
      const std::string v = val();
      if (v == "crash") cfg.debug_barrier_crash = true;
      else if (v.rfind("hold:", 0) == 0) cfg.debug_barrier_hold_ms = static_cast<u32>(std::atoi(v.c_str() + 5));
      else {
        std::fprintf(stderr, "--debug-barrier takes hold:MS or crash\n");
        return 2;
      }
    }
    else if (k == "--witness") {
      std::istringstream in(val());
      std::string hp, h;
      unsigned p = 0;
      while (std::getline(in, hp, ',')) {
        if (!ha::parse_hostport(hp, h, p)) {
          std::fprintf(stderr, "--witness takes HOST:PORT[,HOST:PORT...]\n");
          return 2;
        }
        witnesses.emplace_back(h, p);
      }
      cfg.replica_degrade_gated = true;
    }
    else if (k == "--rejoin") {
      // --rejoin STANDBY_PORT,PEER_HOST:ADMIN,PEER_HOST:SEED
      std::istringstream in(val());
      std::string port, admin;
      if (!std::getline(in, port, ',') || !std::getline(in, admin, ',') ||
          !std::getline(in, peer_seed) ||
          !ha::parse_hostport(admin, peer_admin_host, peer_admin_port) || peer_seed.empty()) {
        std::fprintf(stderr, "--rejoin takes STANDBY_PORT,PEER_HOST:ADMIN,PEER_HOST:SEED\n");
        return 2;
      }
      rejoin_port = static_cast<u16>(std::atoi(port.c_str()));
    }
    else if (k == "--checkpoint-s") checkpoint_s = std::atof(val());
    else if (k == "--catchup-mib-s") cfg.replica_catchup_bytes_per_s = static_cast<u64>(std::atof(val()) * (1 << 20));
    else if (k == "--replica-retain-mib") cfg.replica_retain_bytes = std::strtoull(val(), nullptr, 10) << 20;
    else if (k == "--space-uuid") {
      if (!parse_uuid(val(), cfg.init_uuid)) {
        std::fprintf(stderr, "--space-uuid takes 32 hex digits\n");
        return 2;
      }
    }
    else if (k == "--pages") {
      std::string v = val();
      if (v == "4k") cfg.runtime.pages = PageMode::SMALL;
      else if (v == "thp") cfg.runtime.pages = PageMode::THP;
      else if (v == "hugetlb") cfg.runtime.pages = PageMode::HUGETLB;
      else {
        std::fprintf(stderr, "--pages takes 4k, thp or hugetlb\n");
        return 2;
      }
    }
    else {
      std::fprintf(stderr, "unknown flag %s\n", k.c_str());
      return 2;
    }
  }
  if ((cfg.debug_barrier_crash || cfg.debug_barrier_hold_ms) && !debug_admin) {
    std::fprintf(stderr, "--debug-barrier is for tests: it needs --debug-admin\n");
    return 2;
  }
  if (cfg.runtime.n_shards == 0) {
    cfg.runtime.n_shards = cfg.runtime.cpus.empty()
                               ? static_cast<u16>(std::max(1u, std::thread::hardware_concurrency()))
                               : static_cast<u16>(cfg.runtime.cpus.size());
  }
  account::register_type();

  // Control plane (docs/CONTROL_PLANE.md): the placement map, this node's
  // group and member, and the group's space, from the control members (or
  // the map stored in --data-dir if they do not answer).
  u16 my_group = 0;
  // --adopt-role primary: the group's other members' admin ports, each
  // asked before a legacy directory is adopted as a primary's.
  std::vector<std::pair<std::string, unsigned>> adopt_peers;
  bool adopt_unknown_peer = false;  // a member with no admin port in the map
  bool adopt_stored_map = false;
  if (!control_members.empty() || node_id >= 0) {
    if (control_members.empty() || node_id < 0 || node_id > 0xffffffffLL || cfg.data_dir.empty() ||
        !cfg.cluster.empty()) {
      std::fprintf(stderr, "--control needs --node-id and --data-dir, and replaces --cluster\n");
      return 2;
    }
    control::NodeSetup setup;
    std::string why;
    if (!control::resolve(control_members, cfg.data_dir, static_cast<u32>(node_id), 10000, setup, why)) {
      std::fprintf(stderr, "control: %s\n", why.c_str());
      return 1;
    }
    control::apply(setup, control_members, cfg);
    my_group = setup.group;
    adopt_stored_map = setup.stored;
    for (std::size_t i = 0; i < setup.placement.groups[setup.group].members.size(); ++i) {
      if (i == setup.member) continue;
      const u32 id = setup.placement.groups[setup.group].members[i].node;
      const PlacementNode* nd = nullptr;
      for (const PlacementNode& x : setup.placement.nodes) {
        if (x.id == id) nd = &x;
      }
      if (nd == nullptr || nd->admin_port == 0) adopt_unknown_peer = true;
      else adopt_peers.emplace_back(nd->host, nd->admin_port);
    }
    // Clients reach it where the map says (if it says).
    const PlacementMember& me = setup.placement.groups[setup.group].members[setup.member];
    if (me.client_port != 0) ncfg.base_port = me.client_port;
    // The map names this node's host for other hosts' clients: listen on
    // every interface, not only loopback, unless --host says otherwise.
    if (!host_given) ncfg.host = "0.0.0.0";
    std::printf("control: node %lld, group %u member %u, placement epoch %llu%s\n", node_id, setup.group,
                setup.member, (unsigned long long)setup.placement.epoch,
                setup.stored ? " (stored map: the control plane did not answer; provisional)" : "");
    std::fflush(stdout);
  }

  // Rejoin: a node starting as primary first asks the other node. If that
  // one serves as primary of the same space, the witnesses promoted it
  // while this node was away: this node's data has fallen behind or
  // diverged, so it is moved aside and the node starts as the other's
  // standby, seeded from it.
  // (Not with --adopt-role primary: the adoption check below asks the same
  // peer and refuses if it is primary, before the directory is touched.)
  if (rejoin_port != 0 && !cfg.standby && cfg.adopt_role != Engine::Config::AdoptRole::PRIMARY) {
    std::string reply;
    bool asked = false;
    // HEALTH (docs/HEALTH_DECOUPLING.md 3.4): answered without waiting on
    // the peer's shards, so a long shard task there does not read as
    // "unreachable". A peer that does not know the verb is asked STATUS.
    for (int i = 0; i < 5 && !asked; ++i) {
      asked = ha::ask(peer_admin_host, peer_admin_port, "HEALTH", reply, 500);
      if (asked && reply.rfind("role=", 0) != 0) {
        asked = ha::ask(peer_admin_host, peer_admin_port, "STATUS", reply, 500);
      }
      if (!asked) ::usleep(100000);
    }
    std::string role, uuid;
    std::istringstream in(reply);
    for (std::string kv; in >> kv;) {
      if (kv.rfind("role=", 0) == 0) role = kv.substr(5);
      else if (kv.rfind("uuid=", 0) == 0) uuid = kv.substr(5);
    }
    std::string my_uuid;
    for (u8 b : cfg.init_uuid) {
      char h[3];
      std::snprintf(h, sizeof(h), "%02x", b);
      my_uuid += h;
    }
    if (asked && (role == "p" || role == "n") && uuid == my_uuid) {
      if (!cfg.data_dir.empty() && ::access(cfg.data_dir.c_str(), F_OK) == 0) {
        std::string aside = cfg.data_dir + ".stale-" + std::to_string(::time(nullptr));
        if (::rename(cfg.data_dir.c_str(), aside.c_str()) != 0) {
          std::perror("rejoin: cannot move the data directory aside");
          return 1;
        }
        std::printf("rejoin: the other node is primary; old data moved to %s\n", aside.c_str());
      }
      cfg.standby = true;
      cfg.standby_port = rejoin_port;
      cfg.seed_from = peer_seed;
      std::printf("rejoin: starting as its standby, seeded from %s\n", peer_seed.c_str());
    } else {
      std::printf("rejoin: the other node is %s; starting as primary\n",
                  asked ? (role.empty() ? "unknown" : role == "s" ? "a standby" : "not primary")
                        : "unreachable");
    }
    std::fflush(stdout);
  }

  // Legacy directory adopted as a primary's (docs/LEGACY_ROLE.md): refused
  // unless every other member of its group that the map or --rejoin names
  // answers, and none of them is a primary, promoting, or at a newer term
  // of this space. A member that does not answer could be the primary this
  // directory's owner was a standby of; nothing here can tell, so the
  // adoption waits for it (or for the map to drop it). The start barrier
  // runs after this check either way.
  if (cfg.adopt_role == Engine::Config::AdoptRole::PRIMARY) {
    if (peer_admin_port != 0) adopt_peers.emplace_back(peer_admin_host, peer_admin_port);
    const bool by_control = !control_members.empty();
    const Uuid group_space = cfg.init_uuid;
    cfg.adopt_primary_check = [adopt_peers, adopt_unknown_peer, adopt_stored_map, by_control, group_space](
                                  const Uuid& space, u64 my_term) {
      if (by_control && space != group_space) {
        throw std::runtime_error("--adopt-role primary: this data directory holds another space than its group's");
      }
      if (adopt_peers.empty()) {
        std::fprintf(stderr, "adopt: WARNING: no other group member is known to ask (no --control placement "
                             "naming one, no --rejoin): adopting as a primary's on the operator's word alone\n");
      }
      if (adopt_stored_map) {
        throw std::runtime_error("--adopt-role primary: the control plane did not answer (stored map only)");
      }
      if (adopt_unknown_peer) {
        throw std::runtime_error("--adopt-role primary: a member of this group has no admin port in the map, so "
                                 "its role cannot be asked");
      }
      std::string my_uuid;
      for (u8 b : space) {
        char h[3];
        std::snprintf(h, sizeof(h), "%02x", b);
        my_uuid += h;
      }
      for (const auto& [host, port] : adopt_peers) {
        std::string reply;
        bool asked = false;
        for (int i = 0; i < 5 && !asked; ++i) {
          asked = ha::ask(host, port, "HEALTH", reply, 500);
          if (asked && reply.rfind("role=", 0) != 0) asked = ha::ask(host, port, "STATUS", reply, 500);
          if (asked && reply.rfind("role=", 0) != 0) asked = false;
          if (!asked) ::usleep(100000);
        }
        const std::string who = host + ":" + std::to_string(port);
        if (!asked) {
          throw std::runtime_error("--adopt-role primary: group member " + who +
                                   " did not answer; it may be the primary (refused until it answers)");
        }
        std::string role, uuid;
        unsigned long long term = 0;
        std::istringstream in(reply);
        for (std::string kv; in >> kv;) {
          if (kv.rfind("role=", 0) == 0) role = kv.substr(5);
          else if (kv.rfind("uuid=", 0) == 0) uuid = kv.substr(5);
          else if (kv.rfind("term=", 0) == 0) term = std::strtoull(kv.c_str() + 5, nullptr, 10);
        }
        if (uuid.size() != 32 || role.empty()) {
          throw std::runtime_error("--adopt-role primary: group member " + who + " gave an unreadable answer: " +
                                   reply.substr(0, 120));
        }
        if (uuid != my_uuid) {
          std::fprintf(stderr, "adopt: group member %s holds another space (%s); not evidence either way\n",
                       who.c_str(), uuid.c_str());
          continue;
        }
        // HEALTH says s while a standby is inside its promotion; MOVE ROLE
        // says m then (answered with --control; an older binary may not).
        std::string mr;
        if (ha::ask(host, port, "MOVE ROLE", mr, 500) && mr.rfind("ROLE ", 0) == 0 && mr.size() > 5 &&
            (mr[5] == 'm' || mr[5] == 'p')) {
          role = mr[5] == 'm' ? "m" : role == "s" ? "p" : role;
        }
        if (role == "p" || role == "n" || role == "m") {
          throw std::runtime_error("--adopt-role primary: group member " + who + " is serving as primary or " +
                                   "promoting (role " + role + ") of this space");
        }
        // Fenced: it was this space's primary at its term. At this
        // directory's term or a later one, this directory was not.
        if (role == "f" && term >= my_term) {
          throw std::runtime_error("--adopt-role primary: group member " + who + " was this space's primary at term " +
                                   std::to_string(term) + " (fenced), not older than this directory's " +
                                   std::to_string(my_term));
        }
        if (term > my_term) {
          throw std::runtime_error("--adopt-role primary: group member " + who + " is at term " +
                                   std::to_string(term) + ", newer than this directory's " +
                                   std::to_string(my_term) + " (this directory is stale)");
        }
        std::printf("adopt: group member %s is role %s at term %llu (not newer)\n", who.c_str(), role.c_str(), term);
      }
      std::fflush(stdout);
    };
  }

  // Workers inherit this mask, so only the main thread takes the signal.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGUSR1);  // a standby: promote to primary
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  net::NetServer server(ncfg, cfg.runtime.n_shards);
  cfg.app_poll = net::NetServer::poll_hook;
  cfg.app_user = &server;
  // Its own client port is --port unless the list says otherwise.
  if (!cfg.cluster.empty() && cfg.group_id < cfg.cluster.size() &&
      cfg.member < cfg.cluster[cfg.group_id].members.size() &&
      cfg.cluster[cfg.group_id].members[cfg.member].client_port == 0) {
    cfg.cluster[cfg.group_id].members[cfg.member].client_port = ncfg.base_port;
  }
  Engine eng(cfg);
  try {
    eng.start();
  } catch (const std::exception& e) {
    // A refused start (legacy directory, start barrier, ...): exit cleanly.
    std::fflush(stdout);
    std::fprintf(stderr, "snc_server: not started: %s\n", e.what());
    return 1;
  }
  if (!control_members.empty() && eng.space_uuid() != cfg.init_uuid) {
    std::fprintf(stderr, "control: this data directory holds another space than its group's\n");
    eng.stop();
    return 1;
  }
  std::printf("snc_server shards=%u ports %u-%u durability=%s mint_local=%d\n",
              cfg.runtime.n_shards, ncfg.base_port, ncfg.base_port + cfg.runtime.n_shards - 1,
              cfg.data_dir.empty() ? "none (in memory)" : "DURABLE (WAL, RWF_DSYNC)",
              cfg.mint_local);
  if (!cfg.cluster.empty()) {
    const ClusterMap& m = eng.cluster();
    u32 lo = kSlots, hi = 0;
    for (u32 s = 0; s < kSlots; ++s) {
      if (m.group_of[s] != cfg.group_id) continue;
      lo = std::min(lo, s);
      hi = s;
    }
    const u16 link = m.groups[cfg.group_id].members[cfg.member].link_port;
    std::printf("cluster: group %u of %zu (member %u), slots %u-%u, node links on ports %u-%u while primary "
                "(map crc %08x, placement epoch %llu)\n",
                cfg.group_id, m.groups.size(), cfg.member, lo, hi, link, link + cfg.runtime.n_shards - 1, m.crc(),
                (unsigned long long)m.epoch);
  }
  if (!cfg.data_dir.empty()) {
    std::printf("space uuid ");
    for (u8 b : eng.space_uuid()) std::printf("%02x", b);
    if (cfg.standby) std::printf("  standby, listening on %u-%u", cfg.standby_port,
                                 cfg.standby_port + cfg.runtime.n_shards - 1);
    else if (!cfg.replica_host.empty())
      std::printf("  replicating to %s:%u (%s)", cfg.replica_host.c_str(), cfg.replica_port,
                  cfg.replica_sync ? "sync" : "async");
    std::printf("\n");
  }
  std::fflush(stdout);

  // Admin port (docs/HEALTH_DECOUPLING.md): HEALTH, STATUS, PROMOTE, AUDIT,
  // MOVE. The main thread promotes (on SIGUSR1); the admin side only reads.
  std::string uuid_hex;
  for (u8 b : eng.space_uuid()) {
    char h[3];
    std::snprintf(h, sizeof(h), "%02x", b);
    uuid_hex += h;
  }
  std::atomic<bool> is_standby{cfg.standby};
  // A promotion in progress (from before its start barrier reads the
  // placement until it serves): MOVE ROLE answers m, so a move's commit
  // gate waits (docs/MIGRATION_FAIL_CLOSED.md 5b).
  std::atomic<bool> promoting{false};
  std::atomic<u64> term{eng.term()};
  // The mint lease block held (0-0 without blocks), for STATUS.
  std::atomic<u64> lease_lo{eng.lease_block().first}, lease_hi{eng.lease_block().second};
  // The main loop's last iteration (admin::mono_ns), for HEALTH's main_ms.
  std::atomic<u64> main_tick{admin::mono_ns()};
  // Role: s standby, f fenced, else p (a replication sender exists) or n.
  // A role fact only: p does not say the standby is connected or in sync.
  auto role_now = [&](bool p_from_status, int status_role) {
    if (is_standby.load()) return 's';
    if (eng.fenced()) return 'f';
    if (p_from_status) return status_role == 1 ? 'p' : 'n';
    return eng.replicating() ? 'p' : 'n';
  };

  // Shard progress (3.2): each shard's loop counter, sampled every 10 ms.
  // HEALTH compares the counter read at answer time with the last sample:
  // changed means progress (stall 0); unchanged means stalled since the
  // sample that last saw it change. A late sampler can only lengthen the
  // stall it reports, never shorten it.
  const u16 n_shards = cfg.runtime.n_shards;
  std::mutex prog_mu;
  std::vector<u64> prog_count(n_shards, 0), prog_changed(n_shards, admin::mono_ns());
  bool prog_ok = true;
  for (u16 s = 0; s < n_shards; ++s) prog_ok &= eng.runtime().progress(s, prog_count[s]);
  std::atomic<bool> sampler_stop{false};
  std::thread sampler;
  if (prog_ok) {
    sampler = std::thread([&] {
      while (!sampler_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const u64 now = admin::mono_ns();
        std::lock_guard<std::mutex> g(prog_mu);
        for (u16 s = 0; s < n_shards; ++s) {
          u64 v = 0;
          eng.runtime().progress(s, v);
          if (v != prog_count[s]) {
            prog_count[s] = v;
            prog_changed[s] = now;
          }
        }
      }
    });
  }

  // HEALTH (3.1): published values only; no shard task, no lock a shard
  // thread holds. Liveness of this process and its admin path, plus
  // diagnostics. It does NOT say that replication is current, that the
  // node is eligible for promotion, or that every shard makes progress:
  // stall_ms and main_ms are diagnostic and drive no decision (review of
  // PR #36, option B).
  std::atomic<admin::Loop*> admin_loop{nullptr};
  auto health = [&] {
    char b[512];
    int n = std::snprintf(b, sizeof(b), "role=%c term=%llu uuid=%s epoch=%llu leases=%llu-%llu shards=%u",
                          role_now(false, 0), (unsigned long long)term.load(), uuid_hex.c_str(),
                          (unsigned long long)eng.cluster().epoch, (unsigned long long)lease_lo.load(),
                          (unsigned long long)lease_hi.load(), n_shards);
    auto clamp = [&] { n = std::min(std::max(n, 0), static_cast<int>(sizeof(b)) - 1); };
    clamp();
    const u64 now = admin::mono_ns();
    if (prog_ok) {
      u64 worst = 0;
      unsigned worst_shard = 0;
      std::lock_guard<std::mutex> g(prog_mu);
      for (u16 s = 0; s < n_shards; ++s) {
        u64 v = 0;
        eng.runtime().progress(s, v);
        const u64 stall = v != prog_count[s] || now < prog_changed[s] ? 0 : now - prog_changed[s];
        if (stall > worst) {
          worst = stall;
          worst_shard = s;
        }
      }
      n += std::snprintf(b + n, sizeof(b) - n, " stall_ms=%llu stall_shard=%u", (unsigned long long)(worst / 1000000),
                         worst_shard);
      clamp();
    }
    const u64 tick = main_tick.load();
    n += std::snprintf(b + n, sizeof(b) - n, " main_ms=%llu", (unsigned long long)(now > tick ? (now - tick) / 1000000 : 0));
    clamp();
    if (const admin::Loop* al = admin_loop.load()) {
      const admin::Counters& c = al->counters();
      std::snprintf(b + n, sizeof(b) - n, " adm_q=%llu adm_q_hw=%llu adm_busy=%llu adm_half=%llu adm_half_hw=%llu "
                    "adm_evicted=%llu adm_timeouts=%llu",
                    c.queue_depth.load(), c.queue_hw.load(), c.busy.load(), c.half.load(), c.half_hw.load(),
                    c.evicted.load(), c.timeouts.load());
    }
    return std::string(b);
  };

  // Slot moves (docs/MIGRATION.md): MOVE requests, each on its own thread
  // (a seal or a stage waits for durability; nothing may wait behind it).
  std::unique_ptr<migrate::NodeHandler> mover;
  if (!control_members.empty()) {
    mover = std::make_unique<migrate::NodeHandler>(eng, control_members, my_group, [&] {
      return promoting.load() ? 'm' : is_standby.load() ? 's' : eng.fenced() ? 'f' : 'p';
    });
  }
  std::mutex move_mu;
  std::condition_variable move_cv;
  int moving = 0;
  std::unique_ptr<admin::Loop> admin_owner;
  if (admin_port != 0) {
    int lfd = ha::listen_on(admin_port, 256);
    if (lfd < 0) {
      std::fprintf(stderr, "cannot listen on admin port %u\n", admin_port);
      sampler_stop.store(true);
      if (sampler.joinable()) sampler.join();
      eng.stop();
      return 1;
    }
    admin::Loop::Handlers h;
    h.inline_verb = [&](const std::string& line, std::string& reply) {
      if (line == "HEALTH") {
        reply = health();
        return true;
      }
      // DEBUG STALL <shard> <ms> (tests; docs/HEALTH_DECOUPLING.md 6): one
      // task that spins on that shard's thread. Unknown, as any verb,
      // unless the server was started with --debug-admin.
      if (debug_admin && line.rfind("DEBUG STALL ", 0) == 0) {
        unsigned sh = 0, ms = 0;
        if (std::sscanf(line.c_str() + 12, "%u %u", &sh, &ms) != 2 || sh >= n_shards || ms > 600000) {
          reply = "NO DEBUG STALL <shard> <ms>";
          return true;
        }
        eng.runtime().submit(static_cast<u16>(sh), [ms] {
          const u64 end = admin::mono_ns() + u64{ms} * 1000000;
          while (admin::mono_ns() < end) {
          }
        });
        std::printf("debug: stalling shard %u for %u ms\n", sh, ms);
        std::fflush(stdout);
        reply = "OK";
        return true;
      }
      // DEBUG REPL-PAUSE 1|0 (tests; docs/MIGRATION_FAIL_CLOSED.md 14): this
      // node's replication senders ship nothing more until resumed, so a
      // test chooses exactly which records the standby lacks.
      if (debug_admin && (line == "DEBUG REPL-PAUSE 1" || line == "DEBUG REPL-PAUSE 0")) {
        Engine::debug_pause_replication(line.back() == '1');
        std::printf("debug: replication %s\n", line.back() == '1' ? "paused" : "resumed");
        std::fflush(stdout);
        reply = "OK";
        return true;
      }
      return false;
    };
    h.heavy = [&](const std::string& line) -> std::string {
      if (line == "STATUS") {
        const u64 t_st = trace::enabled() ? trace::now_ns() : 0;
        Engine::ReplStatus st = eng.replication_status();
        if (trace::enabled()) {
          const u64 ms = (trace::now_ns() - t_st) / 1000000;
          if (ms >= 20) SNC_TRACE("admin_status_slow repl_status_ms=%llu", (unsigned long long)ms);
        }
        // Owned slots the map gives this group that fail the serving proof
        // (docs/MIGRATION_FAIL_CLOSED.md 13).
        u64 unproven = 0;
        for (u16 s = 0; s < n_shards; ++s) unproven += eng.stats(s).slots_unproven;
        char b[320];
        std::snprintf(b, sizeof(b), "role=%c term=%llu uuid=%s shards=%u live=%u degraded=%u blocked=%u epoch=%llu leases=%llu-%llu unproven=%llu",
                      role_now(true, st.role), (unsigned long long)term.load(), uuid_hex.c_str(), st.shards, st.live,
                      st.degraded, st.blocked, (unsigned long long)eng.cluster().epoch,
                      (unsigned long long)lease_lo.load(), (unsigned long long)lease_hi.load(),
                      (unsigned long long)unproven);
        return b;
      }
      if (line == "AUDIT" || line.rfind("AUDIT ", 0) == 0) return audit(eng, line.size() > 6 ? line.substr(6) : "");
      if (line == "PROMOTE") {
        if (!is_standby.load()) return "NO not a standby";
        ::kill(::getpid(), SIGUSR1);
        return "OK";
      }
      return "NO unknown request";
    };
    if (mover) {
      h.move = [&](int fd, std::string got, u64 deadline) {
        {
          std::lock_guard<std::mutex> g(move_mu);
          ++moving;
        }
        std::thread([&, fd, got = std::move(got), deadline]() mutable {
          const u64 t0 = trace::enabled() ? trace::now_ns() : 0;
          if (admin::finish_line(fd, got, deadline)) {
            const u64 t1 = trace::enabled() ? trace::now_ns() : 0;
            SNC_TRACE("admin_move_in verb=%.24s bytes=%zu read_ms=%.1f", got.c_str() + 5, got.size(), (t1 - t0) / 1e6);
            const std::string out = mover->handle(got);
            const u64 t2 = trace::enabled() ? trace::now_ns() : 0;
            ha::write_all(fd, out + "\n");
            SNC_TRACE("admin_move_out verb=%.24s handle_ms=%.1f write_ms=%.1f reply=%.40s", got.c_str() + 5,
                      (t2 - t1) / 1e6, trace::enabled() ? (trace::now_ns() - t2) / 1e6 : 0.0, out.c_str());
          } else {
            SNC_TRACE("admin_read_fail move bytes=%zu", got.size());
          }
          ::close(fd);
          std::lock_guard<std::mutex> g(move_mu);
          --moving;
          move_cv.notify_all();
        }).detach();
      };
    }
    // Opened only now, after Engine::start: no admin verb, HEALTH
    // included, is answered during recovery (CORE_CONTRACTS E6, E8).
    admin_owner = std::make_unique<admin::Loop>(lfd, std::move(h));
    admin_loop.store(admin_owner.get());  // before any HEALTH: its counters
    admin_owner->start();
  }
  // The witness's grant to continue without the standby (see the header).
  // After running alone, the primary owes the witnesses a SYNCED: until one
  // is confirmed they keep the grant on record and will not promote.
  bool granted = false, refused = false, owe_synced = false;
  // Asks the witnesses in turn; the first reply decides. A witness runs
  // Paxos among its peers before answering, so allow it time.
  auto ask_witness = [&](const std::string& req, std::string& reply) {
    for (const auto& [h, p] : witnesses) {
      const u64 t0 = trace::enabled() ? trace::now_ns() : 0;
      const bool ok = ha::ask(h, p, req, reply, 1500);
      SNC_TRACE("ask_witness port=%u req=%.24s ok=%d ms=%.1f reply=%.40s", p, req.c_str(), ok ? 1 : 0,
                trace::enabled() ? (trace::now_ns() - t0) / 1e6 : 0.0, reply.c_str());
      if (ok) return true;
    }
    reply.clear();
    return false;
  };

  int sig = 0;
  // Wait for a signal, reporting and checkpointing on the way.
  {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto next_report = t0 + std::chrono::milliseconds(report_ms);
    auto next_ckpt = t0 + std::chrono::duration_cast<clock::duration>(
                              std::chrono::duration<double>(checkpoint_s));
    while (true) {
      main_tick.store(admin::mono_ns());
      auto now = clock::now();
      // At least every 100 ms, so HEALTH's main_ms means something.
      auto wake = now + std::chrono::milliseconds(100);
      if (report_ms > 0) wake = std::min(wake, next_report);
      if (checkpoint_s > 0) wake = std::min(wake, next_ckpt);
      if (!witnesses.empty()) wake = std::min(wake, now + std::chrono::milliseconds(50));
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(wake - now, clock::duration::zero())).count();
      timespec ts{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
      if ((sig = sigtimedwait(&set, nullptr, &ts)) == SIGUSR1) {
        if (cfg.standby) {
          double s = 0;
          promoting.store(true);
          try {
            s = eng.promote();
          } catch (const std::exception& e) {
            // Nothing changed (no placement read or no lease block): still
            // a standby; the witnesses ask again.
            promoting.store(false);
            std::printf("promote refused: %s\n", e.what());
            std::fflush(stdout);
            continue;
          }
          cfg.standby = false;
          term.store(eng.term());
          lease_lo.store(eng.lease_block().first);
          lease_hi.store(eng.lease_block().second);
          is_standby.store(false);
          promoting.store(false);
          std::printf("promoted to primary: term %llu, in %.3fs\n",
                      (unsigned long long)eng.term(), s);
        }
        std::fflush(stdout);
        continue;
      }
      if (sig > 0) break;
      now = clock::now();
      if (!witnesses.empty() && !cfg.standby) {
        Engine::ReplStatus st = eng.replication_status();
        // Clustered, the witnesses keep one HA machine per group: say which.
        const std::string tail = " " + uuid_hex + " " + std::to_string(eng.term()) +
                                 (cfg.cluster.empty() ? "" : " " + std::to_string(cfg.group_id));
        const bool healthy = st.degraded == 0 && st.blocked == 0 && st.live == st.shards;
        std::string reply;
        if (!granted && st.blocked > 0) {
          if (ask_witness("DEGRADE" + tail, reply) && reply == "OK") {
            granted = true;
            refused = false;
            eng.allow_degrade(true);
            std::printf("witness: granted, continuing without the standby\n");
          } else if (reply.rfind("NO", 0) == 0 && !refused) {
            refused = true;
            std::printf("witness: refused (%s); durable writes wait\n", reply.c_str());
          }
          if (reply.rfind("NO term", 0) == 0 && !eng.fenced()) {
            eng.withdraw();  // replaced: leave the data plane (docs/CLUSTER.md)
            std::printf("witness: this primary's term was replaced; withdrawn\n");
          }
        } else if (granted && healthy) {
          // Close the gate first: a shard that drops again waits for a new
          // grant. The gate stays closed even if the witnesses do not hear
          // the SYNCED: a lost reply may hide that they recorded it.
          eng.allow_degrade(false);
          granted = false;
          owe_synced = true;
        }
        if (owe_synced && !granted && healthy) {
          if (ask_witness("SYNCED" + tail, reply) && reply == "OK") {
            owe_synced = false;
            std::printf("witness: standby back in sync\n");
          } else if (reply.rfind("NO term", 0) == 0) {
            owe_synced = false;  // replaced: this node is no longer the primary
            eng.withdraw();
          }
        }
      }
      if (checkpoint_s > 0 && now >= next_ckpt && !cfg.data_dir.empty()) {
        CheckpointReport r = eng.checkpoint();
        std::printf("checkpoint: %llu objects in %.2fs, %llu WAL segments deleted\n",
                    (unsigned long long)r.objects, r.seconds,
                    (unsigned long long)r.segments_deleted);
        next_ckpt = clock::now() + std::chrono::duration_cast<clock::duration>(
                                       std::chrono::duration<double>(checkpoint_s));
      }
      if (report_ms > 0 && now >= next_report) {
        next_report += std::chrono::milliseconds(report_ms);
        u64 lag = 0, disk = 0, records = 0, bytes = 0;
        bool live = true, lost = false;
        for (u16 s = 0; s < cfg.runtime.n_shards; ++s) {
          ShardStats ss = eng.stats(s);
          if (ss.wal_durable_lsn > ss.repl_acked_lsn) lag += ss.wal_durable_lsn - ss.repl_acked_lsn;
          disk += ss.repl_disk_bytes;
          bytes += ss.repl_bytes;
          records += ss.repl_records;
          live &= ss.repl_live;
          lost |= ss.repl_lost;
        }
        double t = std::chrono::duration<double>(now - t0).count();
        if (cfg.standby) {
          std::printf("report t=%.2f standby live=%d records=%llu\n", t, live,
                      (unsigned long long)records);
        } else {
          std::printf("report t=%.2f live=%d lost=%d lag=%llu shipped_mib=%.1f from_disk_mib=%.1f\n",
                      t, live, lost, (unsigned long long)lag, bytes / 1048576.0, disk / 1048576.0);
        }
      }
      std::fflush(stdout);
    }
  }
  // Stop the admin side before the engine: no shard task is submitted
  // once eng.stop() begins.
  if (admin_owner) admin_owner->stop();
  sampler_stop.store(true);
  if (sampler.joinable()) sampler.join();
  {
    std::unique_lock<std::mutex> g(move_mu);
    move_cv.wait(g, [&] { return moving == 0; });
  }
  eng.stop();

  u64 req = 0, fwd = 0, recvs = 0, sends = 0, frames = 0, ring = 0;
  for (u16 s = 0; s < cfg.runtime.n_shards; ++s) {
    net::NetServer::Stats st = server.stats(s);
    ShardStats ss = eng.stats(s);
    std::printf("shard %u: requests %llu forwarded %.1f%% ops_local %llu ops_remote %llu served %llu\n",
                s, (unsigned long long)st.requests,
                100.0 * st.forwarded / std::max<u64>(1, st.requests),
                (unsigned long long)ss.ops_local, (unsigned long long)ss.ops_remote,
                (unsigned long long)ss.ops_served);
    req += st.requests;
    fwd += st.forwarded;
    recvs += st.conn.recv_calls;
    sends += st.conn.send_calls;
    frames += st.conn.frames_in;
    for (u16 d = 0; d < cfg.runtime.n_shards; ++d) ring += eng.runtime().ring_stats(s, d).msgs;
  }
  for (u16 s = 0; s < cfg.runtime.n_shards; ++s) {
    ShardStats ss = eng.stats(s);
    if (ss.repl_role == 1) {
      std::printf("shard %u: replication primary, %s%s%s, acked LSN %llu, %.1f MiB shipped, "
                  "%llu connects\n", s, ss.repl_live ? "live" : "down",
                  ss.repl_degraded ? ", degraded" : "", ss.repl_lost ? ", LOST" : "",
                  (unsigned long long)ss.repl_acked_lsn, ss.repl_bytes / 1048576.0,
                  (unsigned long long)ss.repl_connects);
    } else if (ss.repl_role == 2) {
      std::printf("shard %u: standby, %llu records applied, %llu connects\n", s,
                  (unsigned long long)ss.repl_records, (unsigned long long)ss.repl_connects);
    }
  }
  std::printf("total: requests %llu forwarded %.1f%% frames/recv %.1f frames/send %.1f "
              "ring msgs/request %.2f\n",
              (unsigned long long)req, 100.0 * fwd / std::max<u64>(1, req),
              double(frames) / std::max<u64>(1, recvs), double(frames) / std::max<u64>(1, sends),
              double(ring) / std::max<u64>(1, req));
  return 0;
}
