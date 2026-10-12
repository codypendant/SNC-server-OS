// Locality micro-benchmark (spec: Benchmark run 4, without durability yet).
// Loads N accounts, then each shard runs a closed loop of deposits/reads
// against targets chosen so that a fixed fraction is owned by another shard.
// Reports throughput, latency percentiles and cross-shard messages per op.
//
// Usage: snc_bench [--shards N] [--objects N] [--seconds S] [--remote PCT]
//                  [--depth D] [--reads PCT] [--no-pin] [--spin]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "snc/snc.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;

namespace {

struct Args {
  u16 shards = 0;
  u64 objects = 1000000;
  double seconds = 5;
  int remote_pct = 0;
  int reads_pct = 50;
  u32 depth = 32;  // outstanding ops per shard
  bool pin = true;
  bool spin = false;
};

// Log-linear latency histogram: 2^-5 relative precision, ns resolution.
class Histogram {
 public:
  Histogram() : counts_(64 * 32, 0) {}
  void add(u64 ns) {
    if (ns < 32) {
      ++counts_[ns];
    } else {
      int msb = 63 - __builtin_clzll(ns);
      u64 sub = (ns >> (msb - 5)) & 31;
      ++counts_[(msb - 4) * 32 + sub];
    }
    ++n_;
  }
  void merge(const Histogram& o) {
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
    n_ += o.n_;
  }
  u64 count() const { return n_; }
  double percentile(double p) const {
    u64 target = static_cast<u64>(std::ceil(p / 100.0 * n_));
    u64 acc = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      acc += counts_[i];
      if (acc >= target && counts_[i]) return bucket_value(i);
    }
    return 0;
  }

 private:
  static double bucket_value(std::size_t i) {
    if (i < 32) return double(i);
    int msb = int(i / 32) + 4;
    u64 sub = i % 32;
    return double((u64{32} + sub) << (msb - 5));
  }
  std::vector<u64> counts_;
  u64 n_ = 0;
};

struct ShardDriver {
  u16 shard = 0;
  Engine* eng = nullptr;
  const std::vector<std::vector<ObjectID>>* by_owner = nullptr;
  int remote_pct = 0, reads_pct = 0;
  std::mt19937_64 rng;
  u32 outstanding = 0;
  u64 ops = 0, errors = 0;
  Histogram hist;
  bool finished_signalled = false;
  struct Slot {
    ShardDriver* d;
    u64 start_ns;
  };
  std::vector<Slot> slots;
  std::vector<u32> free_slots;
};

void on_op_done(void* ctx, const ExecResult& r) {
  auto* slot = static_cast<ShardDriver::Slot*>(ctx);
  ShardDriver& d = *slot->d;
  d.hist.add(sri_now_ns() - slot->start_ns);
  ++d.ops;
  if (r.status != Status::OK) ++d.errors;
  --d.outstanding;
  d.free_slots.push_back(static_cast<u32>(slot - d.slots.data()));
}

ObjectID pick(ShardDriver& d) {
  const auto& by = *d.by_owner;
  u16 n = static_cast<u16>(by.size());
  u16 owner = d.shard;
  if (n > 1 && static_cast<int>(d.rng() % 100) < d.remote_pct) {
    owner = static_cast<u16>((d.shard + 1 + d.rng() % (n - 1)) % n);
  }
  const auto& v = by[owner];
  return v[d.rng() % v.size()];
}

// Refill to full depth. Runs from the shard's poll hook, never from a
// completion, so local completions (which run inline) cannot recurse.
bool issue(ShardDriver& d) {
  bool did = false;
  // Local ops complete inline and return their slot at once, so bound the
  // batch per poll to keep the loop (and the phase check) moving.
  for (std::size_t n = d.slots.size(); n > 0 && !d.free_slots.empty(); --n) {
    u32 i = d.free_slots.back();
    d.free_slots.pop_back();
    ++d.outstanding;
    did = true;
    ShardDriver::Slot& s = d.slots[i];
    s.start_ns = sri_now_ns();
    ObjectID target = pick(d);
    if (static_cast<int>(d.rng() % 100) < d.reads_pct) {
      execute(target, kOpGetBalance, {}, {}, on_op_done, &s);
    } else {
      AmountArgs a{1, s.start_ns};
      execute(target, kOpDeposit, ByteView(&a, sizeof(a)), {}, on_op_done, &s);
    }
  }
  return did;
}

struct Loader {
  u16 shard = 0;
  u64 next = 0, total = 0, done = 0, inflight = 0, errors = 0;
  u32 depth = 0;
  bool signalled = false;
};

void on_created(void* c, const ExecResult& r) {
  auto* l = static_cast<Loader*>(c);
  --l->inflight;
  ++l->done;
  if (r.status != Status::OK) ++l->errors;
}

bool load_step(Loader& l) {
  bool did = false;
  while (l.inflight < l.depth && l.next < l.total) {
    did = true;
    ++l.inflight;
    CreateArgs a{};
    a.balance_cents = 1000;
    a.created_ns = l.next;
    int n = std::snprintf(a.email, sizeof(a.email), "user%u.%llu@example.com", l.shard,
                          (unsigned long long)l.next);
    a.email_len = static_cast<u8>(n);
    a.name_len = 8;
    std::memcpy(a.name, "Customer", 8);
    ++l.next;
    create(kTypeId, ByteView(&a, sizeof(a)), {}, on_created, &l);
  }
  return did;
}

enum Phase : int { kIdle, kLoad, kRun, kDrain };

struct Bench {
  std::atomic<int> phase{kIdle};
  std::vector<Loader> loaders;
  std::vector<ShardDriver> drivers;
  std::latch* loaded = nullptr;
  std::latch* finished = nullptr;
};

bool bench_poll(void* user, u16 shard) {
  auto* b = static_cast<Bench*>(user);
  switch (b->phase.load(std::memory_order_acquire)) {
    case kLoad: {
      Loader& l = b->loaders[shard];
      bool did = load_step(l);
      if (l.done == l.total && !l.signalled) {
        l.signalled = true;
        b->loaded->count_down();
      }
      return did;
    }
    case kRun:
      return issue(b->drivers[shard]);
    case kDrain: {
      ShardDriver& d = b->drivers[shard];
      if (d.outstanding == 0 && !d.finished_signalled) {
        d.finished_signalled = true;
        b->finished->count_down();
      }
      return false;
    }
    default:
      return false;
  }
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&] { return i + 1 < argc ? argv[++i] : (std::fprintf(stderr, "missing value\n"), std::exit(2), ""); };
    if (k == "--shards") a.shards = static_cast<u16>(std::atoi(val()));
    else if (k == "--objects") a.objects = std::strtoull(val(), nullptr, 10);
    else if (k == "--seconds") a.seconds = std::atof(val());
    else if (k == "--remote") a.remote_pct = std::atoi(val());
    else if (k == "--reads") a.reads_pct = std::atoi(val());
    else if (k == "--depth") a.depth = static_cast<u32>(std::atoi(val()));
    else if (k == "--no-pin") a.pin = false;
    else if (k == "--spin") a.spin = true;
    else {
      std::fprintf(stderr, "unknown flag %s\n", k.c_str());
      std::exit(2);
    }
  }
  if (a.shards == 0) a.shards = static_cast<u16>(std::max(1u, std::thread::hardware_concurrency()));
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  Args args = parse(argc, argv);
  register_type();

  Bench bench;
  bench.loaders.resize(args.shards);
  bench.drivers.resize(args.shards);
  std::latch loaded(args.shards);
  std::latch finished(args.shards);
  bench.loaded = &loaded;
  bench.finished = &finished;
  for (u16 s = 0; s < args.shards; ++s) {
    Loader& l = bench.loaders[s];
    l.shard = s;
    l.total = args.objects / args.shards + (s < args.objects % args.shards);
    l.depth = args.depth * 4;
  }

  Engine::Config cfg;
  cfg.runtime.n_shards = args.shards;
  cfg.runtime.pin = args.pin;
  cfg.runtime.spin_forever = args.spin;
  cfg.runtime.ring_slots = 1024;
  cfg.runtime.overflow_limit = 1024;
  cfg.expected_objects = args.objects;
  cfg.app_poll = bench_poll;
  cfg.app_user = &bench;
  Engine eng(cfg);
  eng.start();

  // Load: each shard creates its share of accounts; IDs land on owners by hash.
  auto t0 = std::chrono::steady_clock::now();
  bench.phase.store(kLoad, std::memory_order_release);
  loaded.wait();
  bench.phase.store(kIdle, std::memory_order_release);
  double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  u64 load_errors = 0;
  for (auto& l : bench.loaders) load_errors += l.errors;
  if (load_errors) {
    std::fprintf(stderr, "load: %llu create errors\n", (unsigned long long)load_errors);
    return 1;
  }

  // Collect IDs by owner (control path) so drivers can choose local or remote targets.
  std::vector<std::vector<ObjectID>> by_owner(args.shards);
  for (u16 s = 0; s < args.shards; ++s) {
    eng.for_each_object(s, [&](const ObjectHeader& h) { by_owner[s].push_back(h.id); });
  }

  std::vector<ShardStats> before(args.shards);
  for (u16 s = 0; s < args.shards; ++s) before[s] = eng.stats(s);
  std::vector<RingStats> ring_before;
  for (u16 a = 0; a < args.shards; ++a)
    for (u16 b = 0; b < args.shards; ++b) ring_before.push_back(eng.runtime().ring_stats(a, b));

  for (u16 s = 0; s < args.shards; ++s) {
    ShardDriver& d = bench.drivers[s];
    d.shard = s;
    d.eng = &eng;
    d.by_owner = &by_owner;
    d.remote_pct = args.remote_pct;
    d.reads_pct = args.reads_pct;
    d.rng.seed(1234 + s);
    d.slots.resize(args.depth);
    for (u32 i = 0; i < args.depth; ++i) {
      d.slots[i].d = &d;
      d.free_slots.push_back(i);
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  bench.phase.store(kRun, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(args.seconds));
  bench.phase.store(kDrain, std::memory_order_release);
  finished.wait();
  double run_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();

  Histogram all;
  u64 ops = 0, errors = 0;
  for (auto& d : bench.drivers) {
    all.merge(d.hist);
    ops += d.ops;
    errors += d.errors;
  }
  u64 local = 0, remote = 0, msgs = 0, bytes = 0, full = 0;
  for (u16 s = 0; s < args.shards; ++s) {
    ShardStats st = eng.stats(s);
    local += st.ops_local - before[s].ops_local;
    remote += st.ops_remote - before[s].ops_remote;
  }
  for (u16 a = 0, k = 0; a < args.shards; ++a) {
    for (u16 b = 0; b < args.shards; ++b, ++k) {
      RingStats r = eng.runtime().ring_stats(a, b);
      msgs += r.msgs - ring_before[k].msgs;
      bytes += (r.bytes_inline + r.bytes_external) -
               (ring_before[k].bytes_inline + ring_before[k].bytes_external);
      full += r.full_events - ring_before[k].full_events;
    }
  }
  int pinned = 0;
  for (u16 s = 0; s < args.shards; ++s) pinned += eng.runtime().topology(s).pinned;
  eng.stop();

  u64 heap = 0, objects = 0, map_bytes = 0;
  for (u16 s = 0; s < args.shards; ++s) {
    ShardStats st = eng.stats(s);
    heap += st.heap_live_bytes;
    objects += st.objects;
    map_bytes += st.local_map_bytes;
  }

  std::printf("snc_bench backend=native shards=%u pinned=%d/%u durability=none(no WAL yet)\n",
              args.shards, pinned, args.shards);
  std::printf("load: %llu objects in %.2fs (%.0f creates/s), heap %.1f B/object, local maps %.1f MiB\n",
              (unsigned long long)objects, load_s, objects / load_s, double(heap) / objects,
              map_bytes / 1048576.0);
  std::printf("run: remote_target=%d%% reads=%d%% depth=%u seconds=%.2f\n", args.remote_pct,
              args.reads_pct, args.depth, run_s);
  std::printf("  ops/s %.0f  (%.0f per shard)  errors %llu\n", ops / run_s, ops / run_s / args.shards,
              (unsigned long long)errors);
  std::printf("  latency ns  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f\n", all.percentile(50),
              all.percentile(95), all.percentile(99), all.percentile(99.9));
  std::printf("  local ops %.1f%%  cross-shard msgs/op %.2f  msg payload bytes/op %.1f  ring-full events %llu\n",
              100.0 * local / std::max<u64>(1, local + remote), double(msgs) / std::max<u64>(1, ops),
              double(bytes) / std::max<u64>(1, ops), (unsigned long long)full);
  return errors ? 1 : 0;
}
