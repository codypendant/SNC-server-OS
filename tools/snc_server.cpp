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
//                   [--checkpoint-s N] [--replica-retain-mib N]
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
#include <signal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "snc/net.h"
#include "snc/snc.h"
#include "snc/types/account.h"

using namespace snc;

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
  double checkpoint_s = 0;  // checkpoint every this many seconds (0 = never)
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
    else if (k == "--host") ncfg.host = val();
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
    else if (k == "--report-ms") report_ms = std::atoi(val());
    else if (k == "--checkpoint-s") checkpoint_s = std::atof(val());
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
  Engine eng(cfg);
  eng.start();
  std::printf("snc_server shards=%u ports %u-%u durability=%s mint_local=%d\n",
              cfg.runtime.n_shards, ncfg.base_port, ncfg.base_port + cfg.runtime.n_shards - 1,
              cfg.data_dir.empty() ? "none (in memory)" : "DURABLE (WAL, RWF_DSYNC)",
              cfg.mint_local);
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
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(wake - now, clock::duration::zero())).count();
      timespec ts{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
      if ((sig = sigtimedwait(&set, nullptr, &ts)) == SIGUSR1) {
        if (cfg.standby) {
          double s = eng.promote();
          cfg.standby = false;
          std::printf("promoted to primary: term %llu, in %.3fs\n",
                      (unsigned long long)eng.term(), s);
        }
        std::fflush(stdout);
        continue;
      }
      if (sig > 0) break;
      now = clock::now();
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
