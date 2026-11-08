// S0 observability (docs/CHECKPOINT_BACKPRESSURE_DESIGN.md section 11):
// published gauges, include/snc/gauges.h.
//
//   T1 refusal causes: each refusal path counts only its own cause
//   T2 peak and current: current returns to zero after a drain, peak holds
//   T3 WAL mode: the actual O_DIRECT or buffered mode is reported
//   T4 EWMA: exact values for synthetic samples, and reset
//   T5 checkpoint phases: legal order, durations, bytes, results
//   T6 no semantic effect: state after a checkpoint and restart is exact
//      while a reader polls the gauges throughout
//   T7 concurrency: snapshots are never torn (run under TSan too)
//
// T8 (admin compatibility) is tests/test_gauges_admin.py.
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "harness.h"
#include "snc/gauges.h"
#include "snc/slot_map.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr i64 kStart = 1000;

void rm_rf(const std::string& d) { CHECK_EQ(std::system(("rm -rf " + d).c_str()), 0); }
std::string temp_dir(const char* base = "/tmp") {
  std::string t = std::string(base) + "/snc-gauges-XXXXXX";
  std::vector<char> b(t.begin(), t.end());
  b.push_back(0);
  char* d = mkdtemp(b.data());
  return d ? std::string(d) : std::string();
}

Engine::Config config(u16 shards, const std::string& dir, bool init) {
  Engine::Config c;
  c.runtime.n_shards = shards;
  c.expected_objects = 1 << 16;
  c.data_dir = dir;
  c.init = init;
  c.wal_segment_bytes = 4u << 20;
  return c;
}

std::vector<ObjectID> create_accounts(Engine& eng, u64 per_shard) {
  std::vector<ObjectID> ids;
  std::mutex mu;
  run_all(
      eng, per_shard,
      [&](u16, u64, Completion c, void* ctx) {
        CreateArgs a{};
        a.balance_cents = kStart;
        a.created_ns = 1;
        a.email_len = 1;
        a.email[0] = 'e';
        a.name_len = 1;
        a.name[0] = 'n';
        create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [&](u16, u64, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        std::lock_guard<std::mutex> g(mu);
        ids.push_back(r.id);
      });
  return ids;
}

// Waits until shard s has published (published_ns moved past `after`).
GaugeSnapshot fresh(Engine& eng, u16 s) {
  const u64 after = sri_now_ns();
  for (int i = 0; i < 2000; ++i) {
    GaugeSnapshot g = eng.gauges(s);
    if (g.consistent && g[G_PUBLISHED_NS] > after) return g;
    ::usleep(500);
  }
  CHECK(false);
  return {};
}

// ---- T4 -------------------------------------------------------------------------------

void test_ewma() {
  Ewma e;
  CHECK_EQ(e.value(), 0u);
  CHECK_EQ(e.n, 0u);
  e.add(100);  // first sample sets it
  CHECK_EQ(e.x16, 1600u);
  CHECK_EQ(e.value(), 100u);
  e.add(200);  // 1600 + (3200 - 1600) / 8 = 1800
  CHECK_EQ(e.x16, 1800u);
  CHECK_EQ(e.value(), 112u);
  e.add(0);  // 1800 + (0 - 1800) / 8 = 1575 (truncation toward zero)
  CHECK_EQ(e.x16, 1575u);
  CHECK_EQ(e.value(), 98u);
  for (int i = 0; i < 200; ++i) e.add(40);  // converges to the constant exactly
  CHECK_EQ(e.value(), 40u);
  CHECK_EQ(e.n, 203u);
  e.reset();
  CHECK_EQ(e.x16, 0u);
  CHECK_EQ(e.n, 0u);
  e.add(7);
  CHECK_EQ(e.value(), 7u);
  std::printf("T4 ewma: exact\n");
}

// ---- T7 -------------------------------------------------------------------------------

void test_concurrency() {
  ShardGauges g;
  std::atomic<bool> stop{false};
  std::atomic<u64> torn{0}, consistent{0};
  std::thread writer([&] {
    std::array<u64, kGaugeCount> v{};
    for (u64 k = 1; k <= 200000; ++k) {
      v.fill(k);
      v[G_CKPT_PHASE] = k % 6;  // always a legal phase
      g.publish(v);
    }
    stop = true;
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; ++r) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        GaugeSnapshot s = g.read();
        CHECK(s[G_CKPT_PHASE] < 6);
        if (!s.consistent) continue;
        ++consistent;
        const u64 k = s[G_REFUSED_FOLLOWUP];
        for (u32 i = 0; i < kGaugeCount; ++i) {
          const u64 want = i == G_CKPT_PHASE ? k % 6 : k;
          if (s.v[i] != want) ++torn;
        }
      }
    });
  }
  writer.join();
  for (auto& t : readers) t.join();
  CHECK_EQ(torn.load(), 0u);
  CHECK(consistent.load() > 0);
  CHECK_EQ(g.publishes(), 200000u);
  std::printf("T7 concurrency: %llu consistent snapshots, 0 torn\n", (unsigned long long)consistent.load());
}

// ---- T1 -------------------------------------------------------------------------------

// Follow-up backlog: follow-ups held, transfers from shard 0's accounts
// queue one each until the gate refuses.
void test_refused_followup() {
  Engine::Config c = config(2, "", false);
  c.hold_emits = true;
  Engine eng(c);
  eng.start();
  auto ids = create_accounts(eng, 64);
  const SlotMap map = SlotMap::make_default(2);
  std::vector<ObjectID> mine, other;
  for (const ObjectID& id : ids) (map.owner_of(id) == 0 ? mine : other).push_back(id);
  CHECK(!mine.empty() && !other.empty());
  u64 refused = 0, i = 0;
  while (refused < 5) {
    const ObjectID from = mine[i % mine.size()], to = other[i % other.size()];
    ++i;
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
      TransferArgs t{to, 1, 7};
      execute(from, kOpTransfer, ByteView(&t, sizeof(t)), {}, cb, ctx);
    });
    if (r.r.status == Status::E_OVERLOADED) ++refused;
    CHECK(i < 100000);
  }
  ShardStats st = eng.stats(0);
  CHECK_EQ(st.refused_followup, refused);
  CHECK_EQ(st.refused_ring, 0u);
  CHECK_EQ(st.overloaded, st.refused_followup + st.refused_ring);
  GaugeSnapshot g = fresh(eng, 0);
  CHECK_EQ(g[G_REFUSED_FOLLOWUP], refused);
  CHECK_EQ(g[G_REFUSED_RING], 0u);
  CHECK_EQ(g[G_REFUSED_WAITER], 0u);
  CHECK_EQ(g[G_REFUSED_WAL_BUFFER], 0u);
  // T2 (follow-ups): now = in flight, peak >= now, beyond the gate.
  CHECK_EQ(g[G_EMITS_NOW], st.emits_in_flight);
  CHECK(g[G_EMITS_NOW] > 1024);
  CHECK(g[G_EMITS_PEAK] >= g[G_EMITS_NOW]);
  const u64 peak = g[G_EMITS_PEAK];
  eng.release_emits();
  CHECK(eng.wait_emits());
  g = fresh(eng, 0);
  CHECK_EQ(g[G_EMITS_NOW], 0u);
  CHECK_EQ(g[G_EMITS_PEAK], peak);
  CHECK_EQ(eng.gauges(1)[G_REFUSED_FOLLOWUP], 0u);
  eng.stop();
  std::printf("T1 follow-up: %llu refused, all counted as follow-up; T2 emits peak %llu, now 0 after drain\n",
              (unsigned long long)refused, (unsigned long long)peak);
}

// Ring full: shard 1 is held busy while shard 0 forwards more requests to
// it than its ring and overflow can take.
void test_refused_ring() {
  Engine::Config c = config(2, "", false);
  c.runtime.ring_slots = 64;
  c.runtime.overflow_limit = 16;
  c.batch_remote = false;  // one ring slot per request
  Engine eng(c);
  eng.start();
  // One create at a time: with these tiny rings a window of creates could
  // itself be refused under load.
  std::vector<ObjectID> ids;
  while (ids.size() < 64) {
    OneResult r = one(eng, static_cast<u16>(ids.size() % 2), [&](Completion cb, void* ctx) {
      CreateArgs a{};
      a.balance_cents = kStart;
      a.created_ns = 1;
      a.email_len = 1;
      a.email[0] = 'e';
      a.name_len = 1;
      a.name[0] = 'n';
      create(kTypeId, ByteView(&a, sizeof(a)), {}, cb, ctx);
    });
    if (r.r.status == Status::OK) ids.push_back(r.r.id);
    else CHECK_EQ(r.r.status, Status::E_OVERLOADED);
  }
  const u64 pre_ring = eng.stats(0).refused_ring;
  const SlotMap map = SlotMap::make_default(2);
  std::vector<ObjectID> theirs;
  for (const ObjectID& id : ids) {
    if (map.owner_of(id) == 1) theirs.push_back(id);
  }
  CHECK(!theirs.empty());
  std::atomic<bool> release{false}, held{false};
  Engine::DataPlane dp = eng.data_plane();  // until both tasks are done
  CHECK(dp);
  dp.submit(1, [&] {
    held = true;
    while (!release.load()) std::this_thread::yield();
  });
  while (!held.load()) std::this_thread::yield();
  constexpr u32 kN = 400;
  std::atomic<u32> refused{0}, ok{0}, done{0};
  struct Box {
    std::atomic<u32>* refused;
    std::atomic<u32>* ok;
    std::atomic<u32>* done;
  } box{&refused, &ok, &done};
  dp.submit(0, [&] {
    for (u32 i = 0; i < kN; ++i) {
      execute(
          theirs[i % theirs.size()], kOpGetBalance, ByteView{}, {},
          [](void* p, const ExecResult& r) {
            auto* b = static_cast<Box*>(p);
            if (r.status == Status::E_OVERLOADED) ++*b->refused;
            else if (r.status == Status::OK) ++*b->ok;
            ++*b->done;
          },
          &box);
    }
  });
  // The refusals complete on shard 0 at once; the rest wait for shard 1.
  // Shard 1 is held, so nothing completes but the refusals; wait until
  // they stop arriving.
  for (u32 last = ~0u, i = 0; i < 400; ++i) {
    ::usleep(5000);
    if (refused.load() > 0 && refused.load() == last) break;
    last = refused.load();
  }
  release = true;
  for (int i = 0; i < 10000 && done.load() < kN; ++i) ::usleep(500);
  CHECK_EQ(done.load(), kN);
  dp.release();
  CHECK(refused.load() > 0);
  ShardStats st = eng.stats(0);
  CHECK_EQ(st.refused_ring - pre_ring, refused.load());
  CHECK_EQ(st.refused_followup, 0u);
  CHECK_EQ(st.overloaded, st.refused_followup + st.refused_ring);
  GaugeSnapshot g = fresh(eng, 0);
  CHECK_EQ(g[G_REFUSED_RING], st.refused_ring);
  CHECK_EQ(g[G_REFUSED_FOLLOWUP], 0u);
  eng.stop();
  std::printf("T1 ring: %u of %u refused, all counted as ring (%u ok)\n", refused.load(), kN, ok.load());
}

// ---- T2, T3 ---------------------------------------------------------------------------

void test_wal_gauges(const std::string& base, bool direct_cfg, int want_direct, const char* label) {
  std::string dir = temp_dir(base.c_str());
  if (dir.empty()) {
    std::printf("T3 %s: skipped (%s not writable)\n", label, base.c_str());
    return;
  }
  Engine::Config c = config(2, dir, true);
  c.wal_direct = direct_cfg;
  Engine eng(c);
  eng.start();
  auto ids = create_accounts(eng, 2000);
  // Durable deposits: every answer waits for its flush (a waiter each).
  run_all(
      eng, 4000,
      [&](u16 s, u64 i, Completion cb, void* ctx) {
        AmountArgs a{1, 3};
        execute(ids[(s * 4000 + i) % ids.size()], kOpDeposit, ByteView(&a, sizeof(a)), {}, cb, ctx);
      },
      [&](u16, u64, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        CHECK(r.durable);
      });
  for (u16 s = 0; s < 2; ++s) {
    GaugeSnapshot g = fresh(eng, s);
    ShardStats st = eng.stats(s);
    CHECK_EQ(g[G_WAL_PRESENT], 1u);
    CHECK_EQ(g[G_WAL_DIRECT], st.wal_direct ? 1u : 0u);
    if (want_direct >= 0) CHECK_EQ(g[G_WAL_DIRECT], static_cast<u64>(want_direct));
    // Drained: nothing held now; the peaks saw the load.
    CHECK_EQ(g[G_WAITERS_NOW], 0u);
    CHECK_EQ(g[G_WAITER_BYTES_NOW], 0u);
    CHECK(g[G_WAITERS_PEAK] > 0);
    CHECK(g[G_WAITER_BYTES_PEAK] >= g[G_WAITERS_PEAK]);
    CHECK_EQ(g[G_WAL_ACTIVE_BYTES_NOW], 0u);
    CHECK(g[G_WAL_ACTIVE_BYTES_PEAK] > 0);
    CHECK(g[G_WAL_BUFFER_CAP_BYTES] >= g[G_WAL_ACTIVE_BYTES_PEAK]);
    CHECK_EQ(g[G_WAL_UNDURABLE], 0u);
    CHECK_EQ(g[G_WAL_NEXT_LSN], st.wal_next_lsn);
    CHECK_EQ(g[G_WAL_DURABLE_LSN] + 1, g[G_WAL_NEXT_LSN]);
    CHECK_EQ(g[G_WAL_FLUSHES], st.wal.flushes);
    CHECK(g[G_WAL_FLUSHES] > 0);
    CHECK_EQ(g[G_WAL_FLUSH_EWMA_US], st.wal.flush_us_ewma.value());
    CHECK(g[G_WAL_FLUSH_MAX_US] >= g[G_WAL_FLUSH_EWMA_US]);
    CHECK_EQ(g[G_WAL_FLUSH_SINCE_NS], 0u);
    // A fresh space: replay from LSN 1 until the first checkpoint commits.
    CHECK_EQ(g.committed_start_lsn, 1u);
    CHECK_EQ(g.replay_records(), g[G_WAL_NEXT_LSN] - 1);
    if (s == 0) {
      std::printf("T2/T3 %s: wal_direct=%llu waiters_peak=%llu waiter_bytes_peak=%llu wal_active_peak=%llu "
                  "flush_ewma_us=%llu\n",
                  label, (unsigned long long)g[G_WAL_DIRECT], (unsigned long long)g[G_WAITERS_PEAK],
                  (unsigned long long)g[G_WAITER_BYTES_PEAK], (unsigned long long)g[G_WAL_ACTIVE_BYTES_PEAK],
                  (unsigned long long)g[G_WAL_FLUSH_EWMA_US]);
    }
  }
  eng.stop();
  rm_rf(dir);
}

// ---- T5, T6 ---------------------------------------------------------------------------

u64 file_size(const std::string& p) {
  struct stat st{};
  CHECK_EQ(::stat(p.c_str(), &st), 0);
  return static_cast<u64>(st.st_size);
}

void test_checkpoint() {
  std::string dir = temp_dir();
  constexpr u16 kShards = 2;
  std::map<ObjectID, i64> expect;
  u64 seq2 = 0;
  {
    Engine eng(config(kShards, dir, true));
    eng.start();
    auto ids = create_accounts(eng, 30000);
    for (const ObjectID& id : ids) expect[id] = kStart;
    // A fresh directory already took one checkpoint before serving.
    const u64 base_done = fresh(eng, 0)[G_CKPT_COMPLETED];
    const u64 base_node = eng.checkpoint_gauges().completed.load();
    CHECK_EQ(base_done, base_node);
    // A reader polls every gauge for the whole test (T6, T7).
    std::atomic<bool> stop{false};
    std::vector<std::vector<u64>> seen(kShards);
    std::vector<u64> node_seen;
    std::thread reader([&] {
      while (!stop.load()) {
        for (u16 s = 0; s < kShards; ++s) {
          GaugeSnapshot g = eng.gauges(s);
          if (!g.consistent) continue;
          CHECK(g[G_CKPT_PHASE] <= static_cast<u64>(CkptPhase::SYNC));
          if (seen[s].empty() || seen[s].back() != g[G_CKPT_PHASE]) seen[s].push_back(g[G_CKPT_PHASE]);
        }
        const u64 np = eng.checkpoint_gauges().phase.load();
        CHECK(np <= static_cast<u64>(NodeCkptPhase::RECLAIM));
        if (node_seen.empty() || node_seen.back() != np) node_seen.push_back(np);
      }
    });
    CheckpointReport rep = eng.checkpoint();
    CHECK_EQ(rep.objects, ids.size());
    // Writes between the two checkpoints, then the second.
    run_all(
        eng, 1000,
        [&](u16 s, u64 i, Completion cb, void* ctx) {
          AmountArgs a{2, 3};
          execute(ids[(s * 1000 + i) % ids.size()], kOpDeposit, ByteView(&a, sizeof(a)), {}, cb, ctx);
        },
        [&](u16, u64, const ExecResult& r) { CHECK_EQ(r.status, Status::OK); });
    for (u16 s = 0; s < kShards; ++s) {
      for (u64 i = 0; i < 1000; ++i) expect[ids[(s * 1000 + i) % ids.size()]] += 2;
    }
    GaugeSnapshot before = fresh(eng, 0);
    CHECK(before.replay_records() > 0);  // the deposits, since checkpoint 1
    CheckpointReport rep2 = eng.checkpoint();
    seq2 = rep2.seq;
    CHECK_EQ(rep2.seq, rep.seq + 1);
    stop = true;
    reader.join();

    u64 objects = 0;
    for (u16 s = 0; s < kShards; ++s) {
      GaugeSnapshot g = fresh(eng, s);
      CHECK_EQ(g[G_CKPT_PHASE], static_cast<u64>(CkptPhase::IDLE));
      CHECK_EQ(g[G_CKPT_SEQ], rep2.seq);
      CHECK_EQ(g[G_CKPT_COMPLETED], base_done + 2);
      CHECK_EQ(g[G_CKPT_LAST_RESULT], 1u);
      CHECK_EQ(g[G_CKPT_STARTED_NS], 0u);
      CHECK_EQ(g[G_CKPT_PHASE_SINCE_NS], 0u);
      CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT], 0u);
      CHECK(g[G_CKPT_WRITES_INFLIGHT_PEAK] >= 1);
      // Every byte issued was written, and is the part file.
      CHECK_EQ(g[G_CKPT_BYTES_WRITTEN], g[G_CKPT_BYTES_ISSUED]);
      CHECK_EQ(g[G_CKPT_BYTES_ISSUED], file_size(part_path(dir, rep2.seq, s)));
      objects += g[G_CKPT_OBJECTS];
      // Phase durations add up to no more than the whole.
      const u64 parts = g[G_CKPT_LAST_SNAPSHOT_US] + g[G_CKPT_LAST_WALK_US] + g[G_CKPT_LAST_DRAIN_US] +
                        g[G_CKPT_LAST_DURABLE_WAIT_US] + g[G_CKPT_LAST_SYNC_US];
      CHECK(parts <= g[G_CKPT_LAST_TOTAL_US] + 5);
      CHECK(g[G_CKPT_LAST_TOTAL_US] > 0);
      // Committed: nothing to replay yet.
      CHECK(g.committed_start_lsn != 0);
      CHECK_EQ(g.replay_records(), 0u);
      // Phases seen in a legal order: within one checkpoint they only rise,
      // and each checkpoint returns to IDLE.
      for (std::size_t i = 1; i < seen[s].size(); ++i) {
        CHECK(seen[s][i] > seen[s][i - 1] || seen[s][i] == static_cast<u64>(CkptPhase::IDLE));
      }
      CHECK(seen[s].back() == static_cast<u64>(CkptPhase::IDLE));
      // Publishing works at all: some phase other than IDLE was seen (the
      // walk of 30,000 objects spans many 50 us publishes), and the phase
      // durations are real.
      bool busy_seen = false;
      for (u64 p : seen[s]) busy_seen |= p != static_cast<u64>(CkptPhase::IDLE);
      CHECK(busy_seen);
      CHECK(g[G_CKPT_LAST_WALK_US] > 0);
      CHECK(g[G_CKPT_LAST_SYNC_US] > 0);
      std::string path;
      for (u64 p : seen[s]) path += std::string(path.empty() ? "" : ">") + ckpt_phase_name(p);
      std::printf("T5 shard %u: phases seen %s; last snapshot %llu walk %llu drain %llu durable_wait %llu sync %llu "
                  "total %llu us; %llu bytes\n",
                  s, path.c_str(), (unsigned long long)g[G_CKPT_LAST_SNAPSHOT_US],
                  (unsigned long long)g[G_CKPT_LAST_WALK_US], (unsigned long long)g[G_CKPT_LAST_DRAIN_US],
                  (unsigned long long)g[G_CKPT_LAST_DURABLE_WAIT_US], (unsigned long long)g[G_CKPT_LAST_SYNC_US],
                  (unsigned long long)g[G_CKPT_LAST_TOTAL_US], (unsigned long long)g[G_CKPT_BYTES_ISSUED]);
    }
    CHECK_EQ(objects, rep2.objects);
    const NodeCkptGauges& ng = eng.checkpoint_gauges();
    CHECK_EQ(ng.phase.load(), static_cast<u64>(NodeCkptPhase::IDLE));
    CHECK_EQ(ng.seq.load(), rep2.seq);
    CHECK_EQ(ng.completed.load(), base_node + 2);
    CHECK_EQ(ng.started_ns.load(), 0u);
    CHECK(ng.last_total_us.load() >= ng.last_shards_us.load());
    bool node_busy = false;
    for (u64 p : node_seen) node_busy |= p != static_cast<u64>(NodeCkptPhase::IDLE);
    CHECK(node_busy);
    for (std::size_t i = 1; i < node_seen.size(); ++i) {
      CHECK(node_seen[i] > node_seen[i - 1] || node_seen[i] == static_cast<u64>(NodeCkptPhase::IDLE));
    }
    eng.stop();
  }
  // T6: the state recovers exactly, from the checkpoint the gauges named.
  {
    Engine eng(config(kShards, dir, false));
    eng.start();
    CHECK_EQ(eng.recovery().checkpoint_seq, seq2);
    CHECK_EQ(eng.recovery().objects, expect.size());
    for (u16 s = 0; s < kShards; ++s) {
      GaugeSnapshot g = fresh(eng, s);
      CHECK(g.committed_start_lsn != 0);
      CHECK_EQ(g[G_CKPT_COMPLETED], eng.checkpoint_gauges().completed.load());  // a new process
    }
    const SlotMap smap = SlotMap::make_default(kShards);
    u64 checked = 0;
    for (const auto& [id, bal] : expect) {
      if (checked++ % 97) continue;
      OneResult r = one(eng, smap.owner_of(id), [&](Completion cb, void* ctx) {
        execute(id, kOpGetBalance, ByteView{}, {}, cb, ctx);
      });
      CHECK_EQ(r.r.status, Status::OK);
      CHECK_EQ(r.as<BalanceResult>().balance_cents, bal);
    }
    eng.stop();
  }
  rm_rf(dir);
  std::printf("T6 restart: checkpoint %llu recovered, balances exact\n", (unsigned long long)seq2);
}

}  // namespace

int main() {
  register_type();
  test_ewma();
  test_concurrency();
  test_refused_followup();
  test_refused_ring();
  test_wal_gauges("/tmp", true, -1, "default (O_DIRECT where the filesystem allows)");
  test_wal_gauges("/tmp", false, 0, "configured buffered");
  // tmpfs: older kernels refuse O_DIRECT (EINVAL) and the WAL falls back to
  // buffered I/O; since Linux 6.6 tmpfs accepts it. Either way the gauge
  // must equal the mode the WAL actually opened with (ShardStats::wal_direct
  // reads WalStream::direct(), set from the open's result).
  if (::access("/dev/shm", W_OK) == 0) test_wal_gauges("/dev/shm", true, -1, "tmpfs (reported as opened)");
  test_checkpoint();
  std::printf("test_gauges: all passed\n");
  return 0;
}
