// Conventional-server baseline for the step 12 comparison. Same Account
// objects, ops, ID scheme, workload generator, WAL record format and group
// commit settings as snc_bench, but organized the usual way:
//
// - one shared hash map split into 4096 lock stripes (std::mutex each),
//   reachable from every worker thread, plus a striped Account.email index;
// - one shared WAL: appends take a mutex, a dedicated log-writer thread
//   flushes with group commit (pwritev2 with RWF_DSYNC on an O_DIRECT file).
//   `--logs K` splits it into K logs, each with its own writer thread, and
//   worker w appends to log w % K (a partitioned log, as some engines do);
// - pinned worker threads, each running the same closed loop of `--depth`
//   outstanding ops as an SNC shard, completing ops when their LSN is durable.
//
// `--remote PCT` keeps snc_bench's meaning for the key choice: PCT% of a
// worker's ops target keys another worker created. Here that changes only
// cache and lock locality, never the code path, since all state is shared.
//
// Usage: baseline_bench [--threads N] [--objects N] [--seconds S] [--remote PCT]
//                       [--reads PCT] [--depth D] [--no-pin]
//                       [--data-dir DIR] [--logs K] [--group-commit-us N] [--no-sync]
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "baseline_core.h"
#include "bench_common.h"
#include "snc/object_id.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;
using namespace snc::baseline;

namespace {

struct Args {
  u16 threads = 0;
  u64 objects = 1000000;
  double seconds = 5;
  int remote_pct = 0;
  int reads_pct = 50;
  u32 depth = 32;
  bool pin = true;
  std::string data_dir;
  u32 group_commit_us = 50;
  bool sync = true;
  u16 logs = 1;
};

// ---- Workers ---------------------------------------------------------------------

enum Phase : int { kLoad, kRun, kDrain };

struct Shared {
  Args args;
  Map map;
  std::vector<std::unique_ptr<SharedLog>> logs;  // empty: in memory
  std::atomic<int> phase{kLoad};
  std::atomic<u32> loaded{0};
  std::vector<std::vector<ObjectID>> created;  // per worker, for key choice
};

struct Pending {
  u16 log;
  u64 lsn;
  u64 start_ns;
};

struct Worker {
  u16 id = 0;
  std::mt19937_64 rng;
  Histogram hist;
  u64 ops = 0, errors = 0;
};

void pin_to(u16 cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu % std::thread::hardware_concurrency(), &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

ObjectID pick(Shared& sh, Worker& w) {
  u16 n = static_cast<u16>(sh.created.size());
  u16 owner = w.id;
  if (n > 1 && static_cast<int>(w.rng() % 100) < sh.args.remote_pct) {
    owner = static_cast<u16>((w.id + 1 + w.rng() % (n - 1)) % n);
  }
  const auto& v = sh.created[owner];
  return v[w.rng() % v.size()];
}

void worker_main(Shared& sh, Worker& w, u64 load_count) {
  if (sh.args.pin) pin_to(w.id);
  // Load: create this worker's share of accounts.
  auto& mine = sh.created[w.id];
  mine.reserve(load_count);
  u64 last = 0;
  for (u64 i = 0; i < load_count; ++i) {
    auto* o = new Obj;
    o->id = ObjectID::make(1 + w.id, i + 1);
    o->version = 1;
    o->a.balance_cents = 1000;
    o->a.created_ns = i;
    int n = std::snprintf(o->a.email, sizeof(o->a.email), "user%u.%llu@example.com", w.id,
                          (unsigned long long)i);
    o->a.email_len = static_cast<u8>(n);
    o->a.name_len = 8;
    std::memcpy(o->a.name, "Customer", 8);
    Stripe& s = sh.map.stripe(o->id);
    auto g = sh.map.lock(s);
    s.map.emplace(o->id, o);
    if (!sh.logs.empty()) {
      o->last_log = static_cast<u16>(w.id % sh.logs.size());
      last = o->last_lsn = sh.logs[o->last_log]->append(RecType::CREATE, *o);
    }
    g.unlock();
    std::string key(o->a.email, o->a.email_len);
    IndexStripe& is = sh.map.email[std::hash<std::string>{}(key) & (kStripes - 1)];
    {
      std::lock_guard<std::mutex> ig(is.mu);
      is.map.emplace(std::move(key), o->id);
    }
    mine.push_back(o->id);
  }
  if (!sh.logs.empty()) {
    while (sh.logs[w.id % sh.logs.size()]->durable() < last) std::this_thread::yield();
  }
  sh.loaded.fetch_add(1);
  while (sh.phase.load(std::memory_order_acquire) == kLoad) std::this_thread::yield();

  // Run: closed loop with `depth` ops outstanding.
  std::vector<Pending> pending;
  pending.reserve(sh.args.depth);
  auto complete = [&] {
    u64 t = now_ns();
    std::size_t k = 0;
    for (const Pending& p : pending) {
      if (p.lsn <= sh.logs[p.log]->durable()) {
        w.hist.add(t - p.start_ns);
        ++w.ops;
      } else {
        pending[k++] = p;
      }
    }
    bool progressed = k != pending.size();
    pending.resize(k);
    return progressed;
  };
  while (true) {
    bool running = sh.phase.load(std::memory_order_acquire) == kRun;
    if (!running && pending.empty()) break;
    if (running) {
      // At most `depth` ops per pass, then re-check the phase.
      for (u32 batch = 0; batch < sh.args.depth && pending.size() < sh.args.depth; ++batch) {
        u64 t = now_ns();
        ObjectID id = pick(sh, w);
        bool read = static_cast<int>(w.rng() % 100) < sh.args.reads_pct;
        Wait wt = execute_op(sh.map, sh.logs, w.id, id, read, t);
        if (!wt.found) {
          ++w.errors;
          continue;
        }
        if (wt.lsn == 0) {
          w.hist.add(now_ns() - t);
          ++w.ops;
          continue;
        }
        pending.push_back(Pending{wt.log, wt.lsn, t});
      }
    }
    if (!pending.empty() && !complete()) std::this_thread::yield();
  }
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&] {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", k.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--threads") a.threads = static_cast<u16>(std::atoi(val()));
    else if (k == "--objects") a.objects = std::strtoull(val(), nullptr, 10);
    else if (k == "--seconds") a.seconds = std::atof(val());
    else if (k == "--remote") a.remote_pct = std::atoi(val());
    else if (k == "--reads") a.reads_pct = std::atoi(val());
    else if (k == "--depth") a.depth = static_cast<u32>(std::atoi(val()));
    else if (k == "--no-pin") a.pin = false;
    else if (k == "--data-dir") a.data_dir = val();
    else if (k == "--logs") a.logs = static_cast<u16>(std::max(1, std::atoi(val())));
    else if (k == "--group-commit-us") a.group_commit_us = static_cast<u32>(std::atoi(val()));
    else if (k == "--no-sync") a.sync = false;
    else {
      std::fprintf(stderr, "unknown flag %s\n", k.c_str());
      std::exit(2);
    }
  }
  if (a.threads == 0) a.threads = static_cast<u16>(std::max(1u, std::thread::hardware_concurrency()));
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  Shared sh;
  sh.args = parse(argc, argv);
  const Args& args = sh.args;
  if (!args.data_dir.empty()) {
    for (u16 i = 0; i < args.logs; ++i) {
      sh.logs.push_back(std::make_unique<SharedLog>(args.data_dir, i, args.group_commit_us, args.sync));
    }
  }
  const bool durable = !sh.logs.empty();
  auto log_total = [&](u64 (SharedLog::*f)() const) {
    u64 t = 0;
    for (auto& l : sh.logs) t += ((*l).*f)();
    return t;
  };
  u64 rss0 = rss_bytes();  // before the map is sized, like snc_bench before start()
  for (std::size_t i = 0; i < kStripes; ++i) {
    sh.map.stripes[i].map.reserve(args.objects / kStripes * 2);
  }
  sh.created.resize(args.threads);
  std::vector<Worker> workers(args.threads);
  std::vector<std::thread> threads;
  auto t0 = std::chrono::steady_clock::now();
  for (u16 i = 0; i < args.threads; ++i) {
    workers[i].id = i;
    workers[i].rng.seed(1234 + i);
    u64 count = args.objects / args.threads + (i < args.objects % args.threads);
    threads.emplace_back([&sh, &workers, i, count] { worker_main(sh, workers[i], count); });
  }
  while (sh.loaded.load() < args.threads) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  u64 rss1 = rss_bytes();
  u64 contended0 = sh.map.contended.load();
  u64 flushes0 = log_total(&SharedLog::flushes);

  auto t1 = std::chrono::steady_clock::now();
  sh.phase.store(kRun, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(args.seconds));
  sh.phase.store(kDrain, std::memory_order_release);
  for (auto& t : threads) t.join();
  double run_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();

  Histogram all;
  u64 ops = 0, errors = 0;
  for (auto& w : workers) {
    all.merge(w.hist);
    ops += w.ops;
    errors += w.errors;
  }
  u64 contended = sh.map.contended.load() - contended0;
  std::printf("baseline_bench threads=%u pinned=%s durability=%s\n", args.threads,
              args.pin ? "yes" : "no",
              !durable ? "none (in memory)"
                       : (args.sync ? "DURABLE (shared WAL, RWF_DSYNC)" : "WAL without sync (dev only)"));
  std::printf("load: %llu objects in %.2fs (%.0f creates/s), RSS %.1f B/object\n",
              (unsigned long long)args.objects, load_s, args.objects / load_s,
              double(rss1 - rss0) / args.objects);
  std::printf("run: remote_target=%d%% reads=%d%% depth=%u seconds=%.2f\n", args.remote_pct,
              args.reads_pct, args.depth, run_s);
  std::printf("  ops/s %.0f  (%.0f per thread)  errors %llu\n", ops / run_s, ops / run_s / args.threads,
              (unsigned long long)errors);
  std::printf("  latency ns  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f\n", all.percentile(50),
              all.percentile(95), all.percentile(99), all.percentile(99.9));
  std::printf("  contended stripe locks %.3f%% of ops\n", 100.0 * contended / std::max<u64>(1, ops));
  if (durable) {
    u64 flushes = log_total(&SharedLog::flushes) - flushes0;
    std::printf("wal: %u log(s), %llu records, %.1f MiB written, %llu flushes in run (%.1f ops/flush)\n",
                args.logs, (unsigned long long)log_total(&SharedLog::records),
                log_total(&SharedLog::written) / 1048576.0, (unsigned long long)flushes,
                double(ops) / std::max<u64>(1, flushes));
  }
  if (durable) {
    u64 h[24] = {}, total = 0;
    for (auto& l : sh.logs) {
      for (int i = 0; i < 24; ++i) {
        h[i] += l->flush_us_log2[i];
        total += l->flush_us_log2[i];
      }
    }
    auto pct = [&](double p) {
      u64 acc = 0;
      for (int i = 0; i < 24; ++i) {
        acc += h[i];
        if (acc >= p / 100.0 * total) return u64{2} << i;
      }
      return u64{0};
    };
    std::printf("wal flush latency us (log2 bucket upper bound): p50 <%llu p99 <%llu\n",
                (unsigned long long)pct(50), (unsigned long long)pct(99));
  }
  sh.logs.clear();
  return errors ? 1 : 0;
}
