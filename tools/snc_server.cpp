// SNC server: the engine with the socket front end (docs/ROUTING.md). Shard i
// listens on port + i. Runs until SIGINT or SIGTERM, then prints per-shard
// request and forwarding counts.
//
// Usage: snc_server [--shards N] [--cpus 0,1,...] [--host H] [--port P]
//                   [--objects N] [--data-dir DIR] [--group-commit-us N]
//                   [--no-mint-local] [--spin] [--pages 4k|thp|hugetlb]
#include <signal.h>

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
  std::fflush(stdout);

  int sig = 0;
  sigwait(&set, &sig);
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
  std::printf("total: requests %llu forwarded %.1f%% frames/recv %.1f frames/send %.1f "
              "ring msgs/request %.2f\n",
              (unsigned long long)req, 100.0 * fwd / std::max<u64>(1, req),
              double(frames) / std::max<u64>(1, recvs), double(frames) / std::max<u64>(1, sends),
              double(ring) / std::max<u64>(1, req));
  return 0;
}
