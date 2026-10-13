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
//                   [--admin-port P] [--witness HOST:PORT] [--standby-reads LEASE_MS]
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
// it without --standby on the same --data-dir. Promotion starts a new term:
// standbys refuse a primary of an older term, so an old primary that comes
// back with --replica-timeout-ms 0 can acknowledge no durable write.
// A primary started with --seed-port serves its latest checkpoint; a
// standby started with --seed-from on an empty --data-dir fetches it first,
// then catches up from the primary's WAL. --report-ms prints a line of
// replication state every N ms (lag = durable records not yet acked).
// Slot moves (docs/MIGRATION.md): with --control, --admin-port also answers
// the MOVE requests of snc_ctl move (tools/migrate.h). --admin-port also
// answers AUDIT [PATH] (the accounts served here; docs/REAL_MACHINES.md).
// With --control, clients are served on every interface unless --host is set.
// Automatic failover (snc_witness): --admin-port answers STATUS and PROMOTE
// lines (STATUS role p primary, n no replication, s standby, f fenced: a
// newer primary exists, and this node has left the data plane); with --witness, a sync primary past --replica-timeout-ms continues
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

#include "control.h"
#include "ha.h"
#include "migrate.h"
#include "snc/net.h"
#include "snc/snc.h"
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
  if (rejoin_port != 0 && !cfg.standby) {
    std::string reply;
    bool asked = false;
    for (int i = 0; i < 5 && !asked; ++i) {
      asked = ha::ask(peer_admin_host, peer_admin_port, "STATUS", reply, 500);
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
  eng.start();
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

  // Admin port: STATUS and PROMOTE, for snc_witness. The main thread
  // promotes (on SIGUSR1); this thread only reads.
  std::string uuid_hex;
  for (u8 b : eng.space_uuid()) {
    char h[3];
    std::snprintf(h, sizeof(h), "%02x", b);
    uuid_hex += h;
  }
  std::atomic<bool> is_standby{cfg.standby}, admin_stop{false};
  std::atomic<u64> term{eng.term()};
  // The mint lease block held (0-0 without blocks), for STATUS.
  std::atomic<u64> lease_lo{eng.lease_block().first}, lease_hi{eng.lease_block().second};
  // Slot moves (docs/MIGRATION.md): MOVE requests, each on its own thread
  // (a seal or a stage waits for durability; STATUS must not wait behind it).
  std::unique_ptr<migrate::NodeHandler> mover;
  if (!control_members.empty()) {
    mover = std::make_unique<migrate::NodeHandler>(eng, control_members, my_group, [&] {
      return is_standby.load() ? 's' : eng.fenced() ? 'f' : 'p';
    });
  }
  std::mutex move_mu;
  std::condition_variable move_cv;
  int moving = 0;
  std::thread admin;
  if (admin_port != 0) {
    int lfd = ha::listen_on(admin_port);
    if (lfd < 0) {
      std::fprintf(stderr, "cannot listen on admin port %u\n", admin_port);
      return 1;
    }
    admin = std::thread([&, lfd] {
      while (!admin_stop.load()) {
        if (!ha::wait_fd(lfd, POLLIN, 100)) continue;
        int fd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        std::string line, reply;
        if (ha::read_line(fd, line, 500)) {
          if (line.rfind("MOVE ", 0) == 0 && mover) {
            {
              std::lock_guard<std::mutex> g(move_mu);
              ++moving;
            }
            std::thread([&, fd, line] {
              ha::write_all(fd, mover->handle(line) + "\n");
              ::close(fd);
              std::lock_guard<std::mutex> g(move_mu);
              --moving;
              move_cv.notify_all();
            }).detach();
            continue;
          }
          if (line == "STATUS") {
            Engine::ReplStatus st = eng.replication_status();
            char b[256];
            std::snprintf(b, sizeof(b), "role=%c term=%llu uuid=%s shards=%u live=%u degraded=%u blocked=%u epoch=%llu leases=%llu-%llu",
                          is_standby.load() ? 's' : eng.fenced() ? 'f' : (st.role == 1 ? 'p' : 'n'),
                          (unsigned long long)term.load(), uuid_hex.c_str(), st.shards, st.live,
                          st.degraded, st.blocked, (unsigned long long)eng.cluster().epoch,
                          (unsigned long long)lease_lo.load(), (unsigned long long)lease_hi.load());
            reply = b;
          } else if (line == "AUDIT" || line.rfind("AUDIT ", 0) == 0) {
            reply = audit(eng, line.size() > 6 ? line.substr(6) : "");
          } else if (line == "PROMOTE") {
            reply = is_standby.load() ? "OK" : "NO not a standby";
            if (is_standby.load()) ::kill(::getpid(), SIGUSR1);
          } else {
            reply = "NO unknown request";
          }
          ha::write_all(fd, reply + "\n");
        }
        ::close(fd);
      }
      ::close(lfd);
    });
  }
  // The witness's grant to continue without the standby (see the header).
  // After running alone, the primary owes the witnesses a SYNCED: until one
  // is confirmed they keep the grant on record and will not promote.
  bool granted = false, refused = false, owe_synced = false;
  // Asks the witnesses in turn; the first reply decides. A witness runs
  // Paxos among its peers before answering, so allow it time.
  auto ask_witness = [&](const std::string& req, std::string& reply) {
    for (const auto& [h, p] : witnesses) {
      if (ha::ask(h, p, req, reply, 1500)) return true;
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
      auto now = clock::now();
      auto wake = now + std::chrono::hours(24);
      if (report_ms > 0) wake = std::min(wake, next_report);
      if (checkpoint_s > 0) wake = std::min(wake, next_ckpt);
      if (!witnesses.empty()) wake = std::min(wake, now + std::chrono::milliseconds(50));
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(wake - now, clock::duration::zero())).count();
      timespec ts{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
      if ((sig = sigtimedwait(&set, nullptr, &ts)) == SIGUSR1) {
        if (cfg.standby) {
          double s = 0;
          try {
            s = eng.promote();
          } catch (const std::exception& e) {
            // Nothing changed (no lease block): still a standby; the
            // witnesses ask again.
            std::printf("promote refused: %s\n", e.what());
            std::fflush(stdout);
            continue;
          }
          cfg.standby = false;
          term.store(eng.term());
          lease_lo.store(eng.lease_block().first);
          lease_hi.store(eng.lease_block().second);
          is_standby.store(false);
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
  admin_stop.store(true);
  if (admin.joinable()) admin.join();
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
