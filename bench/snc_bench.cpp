// Locality and durability micro-benchmark (spec: Benchmark runs 1, 4 and 5).
// Loads N accounts, then each shard runs a closed loop of deposits/reads
// against targets chosen so that a fixed fraction is owned by another shard.
// Reports throughput, latency percentiles and cross-shard messages per op.
// With --data-dir every op is DURABLE (writes and reads wait for the WAL),
// and after the run it times a checkpoint and a full restart (recovery).
//
// Usage: snc_bench [--shards N] [--objects N] [--seconds S] [--remote PCT]
//                  [--depth D] [--reads PCT] [--no-pin] [--spin]
//                  [--data-dir DIR] [--group-commit-us N] [--no-direct] [--no-sync]
//                  [--no-checkpoint] [--io-yield-us N] [--io-cpus 3] [--no-prefetch]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "zipf.h"
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
  std::string data_dir;  // empty = in memory, no WAL
  u32 group_commit_us = 50;
  bool direct = true;
  bool sync = true;
  bool checkpoint = true;  // false: restart replays the whole WAL
  u32 io_yield_us = 50;  // RuntimeConfig default
  bool prefetch = true;
  std::vector<int> io_cpus;
  double zipf = 0;  // > 0: Zipf key choice over all objects; --remote is ignored
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
  bool prefetch = true;
  std::vector<ObjectID> next;  // targets chosen ahead, for prefetch
  const bench::Zipf* zipf = nullptr;
  const std::vector<ObjectID>* ranked = nullptr;
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
  if (d.zipf) return (*d.ranked)[d.zipf->next(d.rng)];
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
  // Choose the batch's targets first and prefetch them, as an ingress path
  // that reads many requests at once can.
  d.next.clear();
  for (std::size_t i = 0; i < d.free_slots.size(); ++i) d.next.push_back(pick(d));
  if (d.prefetch && !d.next.empty()) prefetch(d.next.data(), static_cast<u32>(d.next.size()));
  std::size_t k = 0;
  // Local ops complete inline and return their slot at once, so bound the
  // batch per poll to keep the loop (and the phase check) moving.
  for (std::size_t n = d.next.size(); n > 0 && !d.free_slots.empty(); --n) {
    u32 i = d.free_slots.back();
    d.free_slots.pop_back();
    ++d.outstanding;
    did = true;
    ShardDriver::Slot& s = d.slots[i];
    s.start_ns = sri_now_ns();
    ObjectID target = d.next[k++];
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
  u64 next = 0, total = 0, done = 0, inflight = 0, errors = 0, retries = 0;
  u32 depth = 0;
  bool signalled = false;
};

void on_created(void* c, const ExecResult& r) {
  auto* l = static_cast<Loader*>(c);
  --l->inflight;
  if (r.status == Status::E_OVERLOADED) {
    ++l->total;  // backpressure from a full owner (spec: bounded overflow): issue one more
    ++l->retries;
    return;
  }
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
      if (l.done + l.retries == l.total && !l.signalled) {
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
    else if (k == "--zipf") a.zipf = std::atof(val());
    else if (k == "--depth") a.depth = static_cast<u32>(std::atoi(val()));
    else if (k == "--no-pin") a.pin = false;
    else if (k == "--spin") a.spin = true;
    else if (k == "--data-dir") a.data_dir = val();
    else if (k == "--group-commit-us") a.group_commit_us = static_cast<u32>(std::atoi(val()));
    else if (k == "--no-direct") a.direct = false;
    else if (k == "--no-sync") a.sync = false;
    else if (k == "--no-checkpoint") a.checkpoint = false;
    else if (k == "--no-prefetch") a.prefetch = false;
    else if (k == "--io-yield-us") a.io_yield_us = static_cast<u32>(std::atoi(val()));
    else if (k == "--io-cpus") {
      for (const char* p = val(); *p;) {
        a.io_cpus.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
      }
    }
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
  cfg.runtime.io_yield_us = args.io_yield_us;
  cfg.runtime.io_worker_cpus = args.io_cpus;
  cfg.prefetch = args.prefetch;
  cfg.expected_objects = args.objects;
  cfg.app_poll = bench_poll;
  cfg.app_user = &bench;
  cfg.data_dir = args.data_dir;
  cfg.init = true;  // the directory must be new or empty
  cfg.group_commit_us = args.group_commit_us;
  cfg.wal_direct = args.direct;
  cfg.wal_sync = args.sync;
  const bool durable = !args.data_dir.empty();
  u64 rss0 = rss_bytes();
  auto eng_ptr = std::make_unique<Engine>(cfg);
  Engine& eng = *eng_ptr;
  eng.start();

  // Load: each shard creates its share of accounts; IDs land on owners by hash.
  auto t0 = std::chrono::steady_clock::now();
  bench.phase.store(kLoad, std::memory_order_release);
  loaded.wait();
  bench.phase.store(kIdle, std::memory_order_release);
  double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  u64 rss1 = rss_bytes();
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
  std::vector<ObjectID> ranked;
  double zipf_top = 0;
  bench::Zipf zipf;
  if (args.zipf > 0) {
    ranked = bench::zipf_order(by_owner);
    zipf = bench::Zipf(ranked.size(), args.zipf);
    zipf_top = zipf.top_share();
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
    d.prefetch = args.prefetch;
    if (args.zipf > 0) {
      d.zipf = &zipf;
      d.ranked = &ranked;
    }
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
  CheckpointReport cp;
  if (durable && args.checkpoint) cp = eng.checkpoint();
  eng.stop();

  u64 heap = 0, objects = 0, map_bytes = 0;
  WalStream::Stats wal;
  int direct = 0;
  for (u16 s = 0; s < args.shards; ++s) {
    ShardStats st = eng.stats(s);
    heap += st.heap_live_bytes;
    objects += st.objects;
    map_bytes += st.local_map_bytes;
    wal.records += st.wal.records;
    wal.record_bytes += st.wal.record_bytes;
    wal.written_bytes += st.wal.written_bytes;
    wal.flushes += st.wal.flushes;
    wal.segments += st.wal.segments;
    wal.segment_opens += st.wal.segment_opens;
    for (int i = 0; i < 24; ++i) wal.flush_us_log2[i] += st.wal.flush_us_log2[i];
    wal.flush_us_max = std::max(wal.flush_us_max, st.wal.flush_us_max);
    wal.rollover_us_max = std::max(wal.rollover_us_max, st.wal.rollover_us_max);
    direct += st.wal_direct;
  }

  std::printf("snc_bench backend=native shards=%u pinned=%d/%u durability=%s\n", args.shards,
              pinned, args.shards,
              !durable ? "none (in memory)"
                       : (args.sync ? "DURABLE (WAL, RWF_DSYNC)" : "WAL without sync (dev only)"));
  std::printf("load: %llu objects in %.2fs (%.0f creates/s), RSS %.1f B/object, heap %.1f B/object, "
              "local maps %.1f MiB\n",
              (unsigned long long)objects, load_s, objects / load_s,
              double(rss1 - rss0) / objects, double(heap) / objects, map_bytes / 1048576.0);
  if (args.zipf > 0) std::printf("keys: zipf theta=%.2f (hottest key %.2f%% of ops)\n", args.zipf, 100 * zipf_top);
  std::printf("run: remote_target=%d%% reads=%d%% depth=%u seconds=%.2f\n", args.remote_pct,
              args.reads_pct, args.depth, run_s);
  std::printf("  ops/s %.0f  (%.0f per shard)  errors %llu\n", ops / run_s, ops / run_s / args.shards,
              (unsigned long long)errors);
  std::printf("  latency ns  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f\n", all.percentile(50),
              all.percentile(95), all.percentile(99), all.percentile(99.9));
  std::printf("  local ops %.1f%%  cross-shard msgs/op %.2f  msg payload bytes/op %.1f  ring-full events %llu\n",
              100.0 * local / std::max<u64>(1, local + remote), double(msgs) / std::max<u64>(1, ops),
              double(bytes) / std::max<u64>(1, ops), (unsigned long long)full);
  if (durable) {
    std::printf("wal: %llu records, %.1f MiB written (%.0f%% padding), %llu flushes "
                "(%.1f records/flush), %llu segments, O_DIRECT %d/%u, group commit %u us\n",
                (unsigned long long)wal.records, wal.written_bytes / 1048576.0,
                100.0 * (wal.written_bytes - wal.record_bytes) / std::max<u64>(1, wal.written_bytes),
                (unsigned long long)wal.flushes, double(wal.records) / std::max<u64>(1, wal.flushes),
                (unsigned long long)wal.segments, direct, args.shards, args.group_commit_us);
    // Flush latency percentiles, as the upper bound of the log2 bucket.
    auto flush_pct = [&](double p) {
      u64 total = 0, acc = 0;
      for (u64 c : wal.flush_us_log2) total += c;
      for (int i = 0; i < 24; ++i) {
        acc += wal.flush_us_log2[i];
        if (acc >= p / 100.0 * total) return u64{2} << i;
      }
      return u64{0};
    };
    std::printf("wal flush latency us (log2 bucket upper bound): p50 <%llu p99 <%llu max %llu; "
                "segment opens on shard %llu, longest rollover %llu us\n",
                (unsigned long long)flush_pct(50), (unsigned long long)flush_pct(99),
                (unsigned long long)wal.flush_us_max, (unsigned long long)wal.segment_opens,
                (unsigned long long)wal.rollover_us_max);
    if (args.checkpoint) std::printf("checkpoint: %llu objects, %.1f MiB in %.2fs, %llu WAL segments deleted\n",
                (unsigned long long)cp.objects, cp.bytes / 1048576.0, cp.seconds,
                (unsigned long long)cp.segments_deleted);
    // Restart: recover the space from the checkpoint plus the WAL written since.
    eng_ptr.reset();
    Engine::Config rc;
    rc.runtime.n_shards = args.shards;
    rc.runtime.pin = args.pin;
    rc.expected_objects = args.objects;
    rc.data_dir = args.data_dir;
    rc.wal_direct = args.direct;
    Engine again(rc);
    again.start();
    const RecoveryReport& rr = again.recovery();
    std::printf("recovery: %llu objects (%llu images + %llu WAL records) in %.2fs: "
                "checkpoint load %.2fs, WAL replay %.2fs, index rebuild %.2fs\n",
                (unsigned long long)rr.objects, (unsigned long long)rr.images_loaded,
                (unsigned long long)rr.wal_records_replayed, rr.total_s, rr.checkpoint_load_s,
                rr.wal_replay_s, rr.index_rebuild_s);
    again.stop();
  }
  return errors ? 1 : 0;
}
