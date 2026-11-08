// S2 checkpoint walk pacing (docs/S2_PACING.md; work order
// docs/handoff/PR74_S12_Review.txt sections 12-15; DESIGN-R tests T1, T7,
// T15, T16).
//
//   P1 controller traces (T15): the pure controller, step by step: floor at
//      reset, additive increase to the max under low pressure, halving to
//      the floor under high pressure and staying there, the hold band
//      (hysteresis), an unknown WAL baseline, one change per update
//      interval, catch-up up to the ceiling and never above it, back within
//      the max after catch-up, target and age states, config validation,
//      and a seeded fuzz of the bounds
//   P2 bounded walk step (T7): a walk step visits at most one batch past
//      its budget: with the budget pinned at 1 ns every step visits exactly
//      one batch; pacing off visits 256 (the walk before S2); a huge budget
//      walks in one step. The visit set is the snapshot either way (CC-1)
//   P3 foreground progress: reads and mutations are answered while the walk
//      is in progress and advancing (review 15.C)
//   P4 concurrent mutation consistency (T1): deposits, creates and deletes
//      on two shards during a paced walk; the checkpoint commits and the
//      space recovers from it to the exact model (CC-1..CC-4, review 15.D)
//   P5 crash at walk positions (review 15.E): a child process crashes with
//      its checkpoint held at the first ID, the middle, the last ID, and in
//      DURABLE_WAIT; recovery uses the previous checkpoint plus the WAL and
//      gives the exact acknowledged state; nothing undurable leaks (CC-3,
//      CC-5); a paced checkpoint then commits and recovers exactly
//   P6 target and hard maximum (T16): a held walk passes its target
//      (catch-up raises the budget to the ceiling, not above), then warn,
//      then degraded, each no earlier than its threshold; the committed
//      checkpoint does not move (CC-2, CC-5); released, it completes and
//      the reports clear; counts are per checkpoint (review 15.F)
//   P7 Engine refuses an inconsistent pacing config
//   P8 backlog pressure, end to end: a shard whose follow-up backlog is
//      above the high mark keeps its walk budget at the floor; a shard
//      without one rises to the max
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/ckpt_pacer.h"
#include "harness.h"
#include "snc/gauges.h"
#include "snc/slot_map.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

void rm_rf(const std::string& d) { CHECK_EQ(std::system(("rm -rf " + d).c_str()), 0); }
std::string temp_dir() {
  std::string t = "/tmp/snc-s2-XXXXXX";
  std::vector<char> b(t.begin(), t.end());
  b.push_back(0);
  char* d = mkdtemp(b.data());
  CHECK(d != nullptr);
  return std::string(d);
}

#if defined(__SANITIZE_THREAD__)
constexpr u32 kRuntimeThreads = 1;  // TSan's background thread
#else
constexpr u32 kRuntimeThreads = 0;
#endif

u32 thread_count() {
  u32 n = 0;
  if (DIR* d = ::opendir("/proc/self/task")) {
    while (dirent* e = ::readdir(d)) n += e->d_name[0] != '.';
    ::closedir(d);
  }
  return n;
}

// The walk budget pinned at `ns` (min = max = ceiling), or the defaults.
Engine::Config config(u16 shards, const std::string& dir, u64 ns = 0) {
  Engine::Config c;
  c.runtime.n_shards = shards;
  c.runtime.pin = false;
  c.expected_objects = 1 << 18;
  c.data_dir = dir;
  c.init = true;
  c.wal_segment_bytes = 4u << 20;
  if (ns != 0) c.ckpt_walk_min_ns = c.ckpt_walk_max_ns = c.ckpt_walk_ceiling_ns = ns;
  return c;
}

Engine::Config reopen(Engine::Config c) {
  c.init = false;
  return c;
}

GaugeSnapshot fresh(Engine& eng, u16 s) {
  const u64 after = sri_now_ns();
  for (int i = 0; i < 20000; ++i) {
    GaugeSnapshot g = eng.gauges(s);
    if (g.consistent && g[G_PUBLISHED_NS] > after) return g;
    ::usleep(200);
  }
  CHECK(false);
  return {};
}

// Waits (up to about 60 s) for pred on shard s's fresh gauges.
template <typename P>
GaugeSnapshot wait_gauges(Engine& eng, u16 s, P pred) {
  for (int i = 0; i < 300000; ++i) {
    GaugeSnapshot g = fresh(eng, s);
    if (pred(g)) return g;
  }
  CHECK(false);
  return {};
}

// Creates n accounts with `balance` on each shard (windowed), returning
// them per owning shard.
std::vector<std::vector<ObjectID>> make_accounts(Engine& eng, u64 per_shard, i64 balance) {
  const u16 n = eng.shard_count();
  const SlotMap map = SlotMap::make_default(n);
  std::vector<std::vector<ObjectID>> out(n);
  std::mutex mu;
  run_all(
      eng, per_shard,
      [&](u16, u64, Completion cb, void* ctx) {
        CreateArgs a{};
        a.balance_cents = balance;
        a.created_ns = 1;
        a.email_len = 1;
        a.email[0] = 'e';
        create(kTypeId, ByteView(&a, sizeof(a)), {}, cb, ctx);
      },
      [&](u16, u64, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        std::lock_guard<std::mutex> g(mu);
        out[map.owner_of(r.id)].push_back(r.id);
      });
  return out;
}

ObjectID create_one(Engine& eng, u16 s, i64 balance) {
  OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
    CreateArgs a{};
    a.balance_cents = balance;
    a.created_ns = 1;
    a.email_len = 1;
    a.email[0] = 'e';
    create(kTypeId, ByteView(&a, sizeof(a)), {}, cb, ctx);
  });
  CHECK_EQ(r.r.status, Status::OK);
  CHECK(r.r.durable);
  return r.r.id;
}

void deposit(Engine& eng, const ObjectID& id, i64 amount, Durability d = Durability::DURABLE) {
  OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
    AmountArgs a{amount, 1};
    ExecOptions o;
    o.durability = d;
    execute(id, kOpDeposit, ByteView(&a, sizeof(a)), o, cb, ctx);
  });
  CHECK_EQ(r.r.status, Status::OK);
  if (d == Durability::DURABLE) CHECK(r.r.durable);
}

void remove_one(Engine& eng, const ObjectID& id) {
  OneResult r = one(eng, 0, [&](Completion cb, void* ctx) { remove(id, {}, cb, ctx); });
  CHECK_EQ(r.r.status, Status::OK);
  CHECK(r.r.durable);
}

// The balance, or -1 when the object does not exist.
i64 balance_of(Engine& eng, const ObjectID& id) {
  OneResult r = one(eng, 0, [&](Completion cb, void* ctx) { execute(id, kOpGetBalance, ByteView{}, {}, cb, ctx); });
  if (r.r.status == Status::E_NOT_FOUND) return -1;
  CHECK_EQ(r.r.status, Status::OK);
  return r.as<BalanceResult>().balance_cents;
}

// The acknowledged state: every account the test touched, with its
// balance, or -1 once deleted.
struct Entry {
  ObjectID id;
  i64 balance;
};
using Model = std::vector<Entry>;

void set(Model& m, const ObjectID& id, i64 b) {
  for (Entry& e : m) {
    if (e.id == id) {
      e.balance = b;
      return;
    }
  }
  m.push_back({id, b});
}

void verify(Engine& eng, const Model& m) {
  for (const Entry& e : m) CHECK_EQ(balance_of(eng, e.id), e.balance);
}

// A checkpoint on its own thread (Engine::checkpoint blocks).
struct Background {
  std::thread t;
  std::atomic<bool> done{false};
  CheckpointReport rep;
  void start(Engine& eng) {
    t = std::thread([this, &eng] {
      rep = eng.checkpoint();
      done = true;
    });
  }
  void join() { t.join(); }
};

constexpr u64 kWalk = static_cast<u64>(CkptPhase::WALK);
constexpr u64 kDurableWait = static_cast<u64>(CkptPhase::DURABLE_WAIT);

// ---- P1 -------------------------------------------------------------------------------

ckpt::PacerConfig trace_config() {
  ckpt::PacerConfig c;
  c.walk_min_ns = 10;
  c.walk_max_ns = 100;
  c.walk_ceiling_ns = 200;
  c.walk_step_ns = 10;
  c.update_ns = 5;
  c.wal_high_pct = 200;
  c.wal_low_pct = 125;
  c.backlog_high_pct = 50;
  c.backlog_low_pct = 25;
  c.target_ns = 1000;
  c.max_age_ns = 4000;
  c.warn_pct = 50;
  return c;
}

// On schedule (done = total), WAL `wal_pct` of a 100 us baseline, backlog of 1024.
ckpt::Signals sig(u64 now, u64 wal_pct, u64 backlog, u64 done = 1, u64 total = 1, u64 base = 100) {
  ckpt::Signals g;
  g.now_ns = now;
  g.wal_baseline_us = base;
  g.wal_ewma_us = base * wal_pct / 100;
  g.backlog = backlog;
  g.backlog_cap = 1024;
  g.done = done;
  g.total = total;
  return g;
}

void test_controller() {
  using namespace ckpt;
  const PacerConfig c = trace_config();
  CHECK(valid(c));
  // Reset: the floor, normal, ok.
  PacerState s = reset(c, 0);
  CHECK_EQ(s.walk_ns, 10u);
  CHECK(s.mode == Mode::NORMAL && s.age == AgeState::OK && !s.past_target);
  // Low pressure: +10 per update interval, never above max; nothing
  // changes between updates.
  for (u64 t = 1; t <= 70; ++t) {
    s = step(c, s, sig(t, 100, 0));
    CHECK(s.pressure == Pressure::LOW);
    CHECK_EQ(s.walk_ns, std::min<u64>(10 + 10 * (t / 5), 100));
  }
  CHECK_EQ(s.walk_ns, 100u);
  // Constant high pressure (WAL at 300%): halves to the floor and stays.
  const u64 high[] = {50, 25, 12, 10, 10, 10, 10, 10, 10, 10, 10, 10};
  u64 t = 70;
  for (u64 want : high) {
    for (int k = 0; k < 5; ++k) s = step(c, s, sig(++t, 300, 0));
    CHECK(s.pressure == Pressure::HIGH);
    CHECK_EQ(s.walk_ns, want);
  }
  // Backlog alone above its high mark is high pressure too.
  CHECK(pressure(c, sig(0, 100, 600)) == Pressure::HIGH);
  CHECK(pressure(c, sig(0, 100, 200)) == Pressure::LOW);
  // The hold band: WAL between its marks, or backlog between its marks,
  // changes nothing, whatever the order.
  s.walk_ns = 40;
  const u64 band[][2] = {{150, 0}, {190, 0}, {130, 0}, {100, 300}, {199, 260}, {126, 0}, {100, 500}};
  for (int r = 0; r < 4; ++r) {
    for (const auto& b : band) {
      for (int k = 0; k < 5; ++k) s = step(c, s, sig(++t, b[0], b[1]));
      CHECK(s.pressure == Pressure::HOLD);
      CHECK_EQ(s.walk_ns, 40u);
    }
  }
  CHECK(pressure(c, sig(0, 201, 0)) == Pressure::HIGH);
  CHECK(pressure(c, sig(0, 124, 255)) == Pressure::LOW);
  CHECK(pressure(c, sig(0, 124, 256)) == Pressure::HOLD);  // 25% is not below 25%
  // An unknown baseline: the WAL signal is left out.
  CHECK(pressure(c, sig(0, 100000, 0, 1, 1, 0)) == Pressure::LOW);
  CHECK(pressure(c, sig(0, 100000, 600, 1, 1, 0)) == Pressure::HIGH);
  // One change per update interval at most.
  s = reset(c, 1000);
  for (u64 k = 1001; k <= 1004; ++k) {
    s = step(c, s, sig(k, 100, 0, 1, 1));
    CHECK_EQ(s.walk_ns, 10u);
  }
  s = step(c, s, sig(1005, 100, 0));
  CHECK_EQ(s.walk_ns, 20u);
  for (u64 k = 1006; k <= 1009; ++k) {
    s = step(c, s, sig(k, 300, 0));
    CHECK_EQ(s.walk_ns, 20u);
  }
  // Catch-up: behind the linear schedule, the budget rises one step per
  // update even under high pressure, to the ceiling and never above it.
  s = reset(c, 0);
  for (u64 k = 1; k <= 60; ++k) {
    s = step(c, s, sig(5 * k, 300, 900, 0, 1000));
    CHECK(s.mode == Mode::CATCH_UP);
    CHECK(s.pressure == Pressure::HIGH);
    CHECK_EQ(s.walk_ns, std::min<u64>(10 + 10 * k, 200));
  }
  // Back on schedule (done/total >= elapsed/target): back within the max,
  // then pressure applies.
  s = step(c, s, sig(305, 100, 0, 400, 1000));
  CHECK(s.mode == Mode::NORMAL);
  CHECK_EQ(s.walk_ns, 100u);
  s.walk_ns = 200;
  s = step(c, s, sig(310, 300, 0, 400, 1000));
  CHECK_EQ(s.walk_ns, 50u);
  CHECK(!behind(c, 0, sig(500, 100, 0, 500, 1000)));  // exactly on schedule
  CHECK(behind(c, 0, sig(500, 100, 0, 499, 1000)));
  CHECK(behind(c, 0, sig(1000, 100, 0, 999, 1000)));  // at the target, unfinished
  CHECK(!behind(c, 0, sig(5000, 100, 0, 1000, 1000)));  // finished
  // Target and age: past target after 1000; warn from 2000 (50% of 4000);
  // degraded from 4000. Past target stays set.
  s = reset(c, 0);
  struct AgeCase {
    u64 now;
    bool past;
    AgeState age;
  };
  const AgeCase ages[] = {{999, false, AgeState::OK},   {1000, false, AgeState::OK},
                          {1001, true, AgeState::OK},   {1999, true, AgeState::OK},
                          {2000, true, AgeState::WARN}, {3999, true, AgeState::WARN},
                          {4000, true, AgeState::DEGRADED}, {90000, true, AgeState::DEGRADED}};
  for (const AgeCase& a : ages) {
    s = step(c, s, sig(a.now, 100, 0, 1000, 1000));
    CHECK_EQ(s.past_target, a.past);
    CHECK(s.age == a.age);
  }
  // No target and no maximum: never behind, never past, always ok.
  PacerConfig none = c;
  none.target_ns = 0;
  none.max_age_ns = 0;
  s = reset(none, 0);
  s = step(none, s, sig(1u << 30, 100, 0, 0, 1000));
  CHECK(s.mode == Mode::NORMAL && !s.past_target && s.age == AgeState::OK);
  // Validation.
  auto bad = [&](auto mutate) {
    PacerConfig b = c;
    mutate(b);
    return !valid(b);
  };
  CHECK(bad([](PacerConfig& b) { b.walk_min_ns = 0; }));
  CHECK(bad([](PacerConfig& b) { b.walk_min_ns = 101; }));
  CHECK(bad([](PacerConfig& b) { b.walk_max_ns = 201; }));
  CHECK(bad([](PacerConfig& b) { b.walk_step_ns = 0; }));
  CHECK(bad([](PacerConfig& b) { b.wal_low_pct = 200; }));
  CHECK(bad([](PacerConfig& b) { b.backlog_low_pct = 60; }));
  CHECK(bad([](PacerConfig& b) { b.warn_pct = 101; }));
  CHECK(bad([](PacerConfig& b) { b.target_ns = 4001; }));
  // Seeded fuzz: the budget never leaves [min, ceiling], and in normal mode
  // an update never leaves it above max.
  u64 x = 0x9e3779b97f4a7c15ull;
  auto rnd = [&] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  };
  s = reset(c, 0);
  u64 now = 0, changes = 0;
  for (int i = 0; i < 200000; ++i) {
    now += rnd() % 4;
    const u64 total = 1000;
    const u64 prev_update = s.last_update_ns;
    s = step(c, s, sig(now, rnd() % 400, rnd() % 1100, rnd() % (total + 1), total, rnd() % 4 == 0 ? 0 : 100));
    CHECK(s.walk_ns >= c.walk_min_ns && s.walk_ns <= c.walk_ceiling_ns);
    const bool updated = s.last_update_ns != prev_update;
    if (s.mode == Mode::NORMAL && updated) CHECK(s.walk_ns <= c.walk_max_ns);
    changes = s.changes;
    if (now > 3000) {
      s = reset(c, now);
      now = 0;
      s.started_ns = s.last_update_ns = 0;
    }
  }
  std::printf("P1 controller: floor, +step to max, halve to floor and hold, hysteresis band, one change per "
              "update, catch-up to the ceiling and back, target/warn/degraded, validation; fuzz %llu changes in "
              "bounds\n",
              (unsigned long long)changes);
}

// ---- P2 -------------------------------------------------------------------------------

struct WalkFigures {
  u64 total, done, steps, step_max_objects, step_max_ns, objects, pacing;
};

WalkFigures walk_once(const std::string& dir, bool init, const std::function<void(Engine::Config&)>& tune) {
  Engine::Config c = config(1, dir);
  c.init = init;
  tune(c);
  Engine eng(c);
  eng.start();
  if (init) make_accounts(eng, 20000, 100);
  eng.checkpoint();
  const GaugeSnapshot g = fresh(eng, 0);
  eng.stop();
  return {g[G_CKPT_WALK_TOTAL], g[G_CKPT_WALK_DONE], g[G_CKPT_WALK_STEPS], g[G_CKPT_STEP_MAX_OBJECTS],
          g[G_CKPT_STEP_MAX_NS], g[G_CKPT_OBJECTS], g[G_CKPT_PACING]};
}

u64 ceil_div(u64 a, u64 b) { return (a + b - 1) / b; }

void test_bounded_step() {
  const std::string dir = temp_dir();
  // Budget pinned at 1 ns: every step stops after exactly one batch (the
  // clock is read after each batch, and a step always walks one).
  for (u32 batch : {16u, 64u}) {
    const WalkFigures f = walk_once(dir, batch == 16, [&](Engine::Config& c) {
      c.ckpt_walk_min_ns = c.ckpt_walk_max_ns = c.ckpt_walk_ceiling_ns = 1;
      c.ckpt_walk_batch = batch;
    });
    CHECK(f.total >= 20000);
    CHECK_EQ(f.done, f.total);
    CHECK_EQ(f.objects, f.total);  // nothing deleted: every snapshot ID is in the part
    CHECK_EQ(f.pacing, 1u);
    CHECK_EQ(f.step_max_objects, u64{batch});
    CHECK_EQ(f.steps, ceil_div(f.total, batch));
  }
  // Pacing off: the walk before S2, 256 IDs per iteration.
  const WalkFigures off = walk_once(dir, false, [](Engine::Config& c) { c.ckpt_pacing = false; });
  CHECK_EQ(off.pacing, 0u);
  CHECK_EQ(off.step_max_objects, 256u);
  CHECK_EQ(off.steps, ceil_div(off.total, 256));
  CHECK_EQ(off.objects, off.total);
  // A budget far above the walk's cost: one step.
  const WalkFigures big = walk_once(dir, false, [](Engine::Config& c) {
    c.ckpt_walk_min_ns = c.ckpt_walk_max_ns = c.ckpt_walk_ceiling_ns = 60'000'000'000ull;
  });
  CHECK_EQ(big.steps, 1u);
  CHECK_EQ(big.step_max_objects, big.total);
  // The defaults, for the record (time depends on the machine).
  const WalkFigures def = walk_once(dir, false, [](Engine::Config&) {});
  CHECK_EQ(def.done, def.total);
  CHECK(def.step_max_objects <= def.total);
  rm_rf(dir);
  std::printf("P2 bounded step: %llu IDs; 1 ns budget -> one batch per step (16 and 64); off -> 256 per step "
              "in %llu steps; 60 s budget -> 1 step; defaults -> %llu steps, longest %llu ns / %llu IDs\n",
              (unsigned long long)off.total, (unsigned long long)off.steps, (unsigned long long)def.steps,
              (unsigned long long)def.step_max_ns, (unsigned long long)def.step_max_objects);
}

// ---- P3 -------------------------------------------------------------------------------

void test_foreground_progress() {
  const std::string dir = temp_dir();
  Engine::Config c = config(1, dir, 1);  // one batch (16 IDs) per iteration
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 100000, 100)[0];
  Background bg;
  bg.start(eng);
  wait_gauges(eng, 0, [](const GaugeSnapshot& g) { return g[G_CKPT_PHASE] == kWalk; });
  u64 reads = 0, writes = 0, first_done = ~0ull, last_done = 0, max_delta = 0;
  i64 deposited = 0;
  for (u64 i = 0; !bg.done; ++i) {
    const GaugeSnapshot before = eng.gauges(0);
    const ObjectID& id = ids[(i * 7919) % ids.size()];
    if (i % 2 == 0) {
      (void)balance_of(eng, id);
    } else {
      deposit(eng, id, 1, Durability::APPLIED);
      ++deposited;
    }
    const GaugeSnapshot after = eng.gauges(0);
    if (before.consistent && after.consistent && before[G_CKPT_PHASE] == kWalk && after[G_CKPT_PHASE] == kWalk &&
        before[G_CKPT_SEQ] == after[G_CKPT_SEQ]) {
      (i % 2 == 0 ? reads : writes) += 1;
      first_done = std::min(first_done, before[G_CKPT_WALK_DONE]);
      last_done = std::max(last_done, after[G_CKPT_WALK_DONE]);
      if (after[G_CKPT_WALK_DONE] >= before[G_CKPT_WALK_DONE]) {
        max_delta = std::max(max_delta, after[G_CKPT_WALK_DONE] - before[G_CKPT_WALK_DONE]);
      }
    }
  }
  bg.join();
  const GaugeSnapshot g = fresh(eng, 0);
  CHECK(reads >= 20);
  CHECK(writes >= 20);
  CHECK(last_done > first_done);  // the walk advanced while they ran
  CHECK_EQ(g[G_CKPT_STEP_MAX_OBJECTS], 16u);
  CHECK_EQ(g[G_CKPT_WALK_DONE], g[G_CKPT_WALK_TOTAL]);
  eng.stop();
  rm_rf(dir);
  std::printf("P3 foreground: %llu reads and %llu applied deposits answered inside the walk while it advanced "
              "from ID %llu to %llu of %llu (at most %llu IDs walked around one op, gauges as published)\n",
              (unsigned long long)reads, (unsigned long long)writes, (unsigned long long)first_done,
              (unsigned long long)last_done, (unsigned long long)g[G_CKPT_WALK_TOTAL],
              (unsigned long long)max_delta);
}

// ---- P4 -------------------------------------------------------------------------------

void test_concurrent_consistency() {
  const std::string dir = temp_dir();
  const Engine::Config c = config(2, dir, 1);
  Model m;
  u64 seq = 0, during = 0;
  {
    Engine eng(c);
    eng.start();
    const auto own = make_accounts(eng, 4000, 100);
    std::vector<ObjectID> all, zero;
    for (const auto& v : own) all.insert(all.end(), v.begin(), v.end());
    for (const ObjectID& id : all) m.push_back({id, 100});
    for (u16 s = 0; s < 2; ++s) {
      for (int k = 0; k < 200; ++k) {
        const ObjectID z = create_one(eng, s, 0);
        zero.push_back(z);
        m.push_back({z, 0});
      }
    }
    // The walk is paced (one batch per iteration) and also held at a
    // series of positions, so the history lands at known points of the
    // walk on both shards: ahead of the cursor and behind it.
    Engine::debug_ckpt_walk_limit(1);
    Background bg;
    bg.start(eng);
    u64 totals[2];
    for (u16 s = 0; s < 2; ++s) {
      totals[s] = wait_gauges(eng, s, [](const GaugeSnapshot& g) {
                    return g[G_CKPT_PHASE] == kWalk && g[G_CKPT_WALK_DONE] == 1;
                  })[G_CKPT_WALK_TOTAL];
    }
    u64 i = 0;
    std::size_t next_zero = 0;
    const u64 top = std::max(totals[0], totals[1]);
    for (u64 pos = 1; pos <= top + top / 20; pos += top / 20) {
      Engine::debug_ckpt_walk_limit(pos);
      for (u16 s = 0; s < 2; ++s) {
        const u64 want = std::min(pos, totals[s]);
        wait_gauges(eng, s, [&](const GaugeSnapshot& g) {
          return g[G_CKPT_PHASE] != kWalk || g[G_CKPT_WALK_DONE] == want;
        });
      }
      for (int k = 0; k < 15; ++k, ++i) {
        switch (i % 3) {
          case 0: {
            const ObjectID& id = all[(i * 7919) % all.size()];
            const i64 amount = static_cast<i64>(i % 7 + 1);
            deposit(eng, id, amount);
            for (Entry& e : m) {
              if (e.id == id) e.balance += amount;
            }
            break;
          }
          case 1: {
            const ObjectID id = create_one(eng, static_cast<u16>(i % 2), 50);
            m.push_back({id, 50});
            all.push_back(id);
            break;
          }
          case 2:
            if (next_zero < zero.size()) {
              remove_one(eng, zero[next_zero]);
              set(m, zero[next_zero], -1);
              ++next_zero;
            }
            break;
        }
        during += !bg.done;
      }
    }
    Engine::debug_ckpt_walk_limit(0);
    bg.join();
    seq = bg.rep.seq;
    CHECK(during >= 300);
    verify(eng, m);
    eng.stop();
  }
  // Recover from the paced checkpoint plus the WAL after its start.
  Engine again(reopen(c));
  again.start();
  CHECK_EQ(again.recovery().checkpoint_seq, seq);
  verify(again, m);
  again.stop();
  rm_rf(dir);
  std::printf("P4 concurrent: %llu deposits, creates and deletes ran at 21 held positions of a 1 ns-budget walk on 2 shards; "
              "checkpoint %llu recovers to the exact model (%zu accounts)\n",
              (unsigned long long)during, (unsigned long long)seq, m.size());
}

// ---- P5 -------------------------------------------------------------------------------

enum class Crash { FIRST, MIDDLE, LAST, DURABLE_WAIT };
const char* crash_name(Crash k) {
  switch (k) {
    case Crash::FIRST: return "first ID";
    case Crash::MIDDLE: return "middle";
    case Crash::LAST: return "last ID";
    case Crash::DURABLE_WAIT: return "durable_wait";
  }
  return "?";
}

struct CrashFile {
  u64 seq_before;  // the committed checkpoint at the crash
  u64 walk_total, held_at;
  u64 n;
};

// Child: an acknowledged history, a committed checkpoint, more history, a
// checkpoint held at the crash point, more acknowledged history; then
// dies without stopping anything. The model goes to dir/model.
[[noreturn]] void crash_child(const std::string& dir, Crash k) {
  Engine::Config c = config(1, dir, 2000);
  Engine eng(c);
  eng.start();
  Model m;
  const std::vector<ObjectID> ids = make_accounts(eng, 3000, 100)[0];
  for (const ObjectID& id : ids) m.push_back({id, 100});
  std::vector<ObjectID> zero;
  for (int i = 0; i < 100; ++i) {
    zero.push_back(create_one(eng, 0, 0));
    m.push_back({zero.back(), 0});
  }
  const CheckpointReport first = eng.checkpoint();
  // 500 distinct accounts (13 is coprime with 3000), +3 each, after the
  // committed checkpoint: recovery replays them from the WAL.
  for (int i = 0; i < 500; ++i) {
    deposit(eng, ids[(i * 13) % ids.size()], 3);
    set(m, ids[(i * 13) % ids.size()], 103);
  }
  CrashFile f{first.seq, 0, 0, 0};
  ObjectID undurable{};
  Background bg;
  if (k == Crash::DURABLE_WAIT) {
    // An applied deposit the WAL never makes durable: the walk finishes,
    // and the part waits for that LSN (CC-3).
    Engine::debug_hold_wal(0, true);
    undurable = ids[1];
    deposit(eng, undurable, 1000, Durability::APPLIED);
    bg.start(eng);
    const GaugeSnapshot g = wait_gauges(eng, 0, [](const GaugeSnapshot& x) { return x[G_CKPT_PHASE] == kDurableWait; });
    f.walk_total = g[G_CKPT_WALK_TOTAL];
    f.held_at = g[G_CKPT_WALK_DONE];
  } else {
    u64 at = 1;
    if (k == Crash::MIDDLE) at = 1550;
    if (k == Crash::LAST) at = ~0ull;  // set below, once the snapshot size is known
    Engine::debug_ckpt_walk_limit(k == Crash::LAST ? 1 : at);
    bg.start(eng);
    GaugeSnapshot g = wait_gauges(eng, 0, [](const GaugeSnapshot& x) { return x[G_CKPT_PHASE] == kWalk && x[G_CKPT_WALK_DONE] >= 1; });
    if (k == Crash::LAST) {
      at = g[G_CKPT_WALK_TOTAL] - 1;
      Engine::debug_ckpt_walk_limit(at);
    }
    g = wait_gauges(eng, 0, [&](const GaugeSnapshot& x) { return x[G_CKPT_WALK_DONE] == at; });
    f.walk_total = g[G_CKPT_WALK_TOTAL];
    f.held_at = at;
    CHECK_EQ(g[G_CKPT_PHASE], kWalk);
    // History after begin: deposits on accounts both sides of the cursor,
    // creates, deletes (all after start_lsn: the WAL carries them, CC-2).
    for (int i = 0; i < 300; ++i) {
      const ObjectID& id = ids[(i * 7) % ids.size()];
      deposit(eng, id, 2);
      for (Entry& e : m) {
        if (e.id == id) e.balance += 2;
      }
    }
    for (int i = 0; i < 50; ++i) m.push_back({create_one(eng, 0, 77), 77});
    for (int i = 0; i < 50; ++i) {
      remove_one(eng, zero[static_cast<std::size_t>(i)]);
      set(m, zero[static_cast<std::size_t>(i)], -1);
    }
    CHECK_EQ(fresh(eng, 0)[G_CKPT_PHASE], kWalk);  // still held
  }
  f.n = m.size();
  const std::string path = dir + "/model";
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) std::_Exit(3);
  if (::write(fd, &f, sizeof(f)) != static_cast<ssize_t>(sizeof(f))) std::_Exit(4);
  const ssize_t bytes = static_cast<ssize_t>(m.size() * sizeof(Entry));
  if (::write(fd, m.data(), static_cast<std::size_t>(bytes)) != bytes) std::_Exit(5);
  ::fsync(fd);
  ::close(fd);
  std::fflush(nullptr);
  std::_Exit(0);  // the crash: no stop, no flush, the checkpoint uncommitted
}

void test_crash_positions() {
  std::string report;
  for (Crash k : {Crash::FIRST, Crash::MIDDLE, Crash::LAST, Crash::DURABLE_WAIT}) {
    const std::string dir = temp_dir();
    CHECK(thread_count() <= 1u + kRuntimeThreads);  // fork with no engine thread alive (TSan adds one with the first)
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) crash_child(dir, k);
    int st = 0;
    CHECK_EQ(::waitpid(pid, &st, 0), pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CrashFile f{};
    Model m;
    {
      FILE* in = std::fopen((dir + "/model").c_str(), "rb");
      CHECK(in != nullptr);
      CHECK_EQ(std::fread(&f, sizeof(f), 1, in), 1u);
      m.resize(f.n);
      CHECK_EQ(std::fread(m.data(), sizeof(Entry), f.n, in), f.n);
      std::fclose(in);
    }
    const Engine::Config c = reopen(config(1, dir, 2000));
    u64 seq = 0;
    {
      Engine eng(c);
      eng.start();
      // The held checkpoint never committed: the previous one is used,
      // with the WAL after its start (CC-5, CC-2), and nothing that was
      // only applied leaks in (CC-3).
      CHECK_EQ(eng.recovery().checkpoint_seq, f.seq_before);
      verify(eng, m);
      // A paced checkpoint now commits and recovers to the same state.
      seq = eng.checkpoint().seq;
      CHECK(seq > f.seq_before);
      eng.stop();
    }
    {
      Engine eng(c);
      eng.start();
      CHECK_EQ(eng.recovery().checkpoint_seq, seq);
      verify(eng, m);
      eng.stop();
    }
    rm_rf(dir);
    char line[160];
    std::snprintf(line, sizeof(line), " %s (%llu/%llu)", crash_name(k), (unsigned long long)f.held_at,
                  (unsigned long long)f.walk_total);
    report += line;
  }
  std::printf("P5 crash positions:%s; each recovered from the previous checkpoint to the exact model, then a "
              "paced checkpoint committed and recovered exactly\n",
              report.c_str());
}

// ---- P6 -------------------------------------------------------------------------------

void test_target_and_max_age() {
  const std::string dir = temp_dir();
  Engine::Config c = config(1, dir);
  c.ckpt_walk_min_ns = 1000;
  c.ckpt_walk_max_ns = 2000;
  c.ckpt_walk_ceiling_ns = 8000;
  c.ckpt_walk_step_ns = 1000;
  c.ckpt_pace_update_ns = 1'000'000;
  c.ckpt_target_ms = 40;
  c.ckpt_max_age_ms = 160;
  c.ckpt_warn_pct = 50;
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 3000, 100)[0];
  eng.checkpoint();
  const GaugeSnapshot base = fresh(eng, 0);
  CHECK_EQ(base[G_CKPT_TARGET_MS], 40u);
  CHECK_EQ(base[G_CKPT_MAX_AGE_MS], 160u);
  const u64 committed = base.committed_start_lsn;
  CHECK(committed != 0);
  const u64 past0 = base[G_CKPT_PAST_TARGET_COUNT], degraded0 = base[G_CKPT_DEGRADED_COUNT];
  deposit(eng, ids[0], 1);  // so this checkpoint's start is past the committed one
  Engine::debug_ckpt_walk_limit(1500);
  Background bg;
  bg.start(eng);
  // Each state is reported no earlier than its threshold: the shard
  // decided it at a time no later than the snapshot's publish time.
  auto elapsed_ms = [](const GaugeSnapshot& g) { return (g[G_PUBLISHED_NS] - g[G_CKPT_STARTED_NS]) / 1'000'000; };
  bool saw_past = false, saw_warn = false;
  GaugeSnapshot g;
  for (;;) {
    g = fresh(eng, 0);
    if (g[G_CKPT_STARTED_NS] == 0) continue;
    if (g[G_CKPT_PAST_TARGET]) {
      CHECK(elapsed_ms(g) >= 40);
      saw_past = true;
    }
    if (g[G_CKPT_AGE_STATE] >= 1) CHECK(elapsed_ms(g) >= 80);
    saw_warn |= g[G_CKPT_AGE_STATE] == 1;
    if (g[G_CKPT_AGE_STATE] == 2) {
      CHECK(elapsed_ms(g) >= 160);
      break;
    }
    CHECK(g[G_CKPT_WALK_BUDGET_NS] <= 8000u);
    // Not committed: recovery would still use the previous checkpoint and
    // the WAL after it (CC-2, CC-5).
    CHECK_EQ(g.committed_start_lsn, committed);
    // Foreground work is still served, and still durable.
    deposit(eng, ids[static_cast<std::size_t>(g[G_PUBLISHED_NS] % ids.size())], 1);
  }
  CHECK(saw_past);
  CHECK_EQ(g[G_CKPT_PHASE], kWalk);
  CHECK_EQ(g[G_CKPT_WALK_DONE], 1500u);
  CHECK_EQ(g[G_CKPT_MODE], 1u);                // behind: catch-up
  CHECK_EQ(g[G_CKPT_WALK_BUDGET_NS], 8000u);   // raised to the ceiling, not above
  CHECK(g[G_CKPT_CATCHUP_STEPS] >= 1);
  CHECK_EQ(g[G_CKPT_PAST_TARGET_COUNT], past0 + 1);
  CHECK_EQ(g[G_CKPT_DEGRADED_COUNT], degraded0 + 1);
  CHECK_EQ(g.committed_start_lsn, committed);
  // Degraded never abandons the checkpoint: released, it completes.
  Engine::debug_ckpt_walk_limit(0);
  bg.join();
  const GaugeSnapshot done = fresh(eng, 0);
  CHECK_EQ(done[G_CKPT_PHASE], 0u);
  CHECK_EQ(done[G_CKPT_AGE_STATE], 0u);
  CHECK_EQ(done[G_CKPT_PAST_TARGET], 0u);
  CHECK_EQ(done[G_CKPT_MODE], 0u);
  CHECK_EQ(done[G_CKPT_WALK_BUDGET_NS], 0u);
  CHECK_EQ(done[G_CKPT_PAST_TARGET_COUNT], past0 + 1);
  CHECK_EQ(done[G_CKPT_DEGRADED_COUNT], degraded0 + 1);
  CHECK_EQ(done[G_CKPT_COMPLETED], base[G_CKPT_COMPLETED] + 1);
  CHECK(done.committed_start_lsn > committed);
  CHECK_EQ(bg.rep.seq, base[G_CKPT_SEQ] + 1);
  eng.stop();
  rm_rf(dir);
  std::printf("P6 target and maximum: held walk past target (>= 40 ms), warn%s (>= 80 ms), degraded (>= 160 ms); "
              "catch-up raised the budget to the 8000 ns ceiling; the committed checkpoint did not move; released, "
              "it committed and the reports cleared (counts +1 each)\n",
              saw_warn ? "" : " (not sampled)");
}

// ---- P8 -------------------------------------------------------------------------------

// The controller wired to a real signal: shard 0's follow-up backlog above
// its high mark holds its walk budget at the floor, while shard 1, with no
// backlog, rises to the max. The WAL signal is set out of reach here (P1
// covers it), so device noise cannot move either shard.
void test_backlog_pressure() {
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  c.hold_emits = true;
  c.ckpt_walk_min_ns = 1000;
  c.ckpt_walk_max_ns = 4000;
  c.ckpt_walk_ceiling_ns = 8000;
  c.ckpt_walk_step_ns = 1000;
  c.ckpt_target_ms = 0;  // no catch-up
  c.ckpt_wal_high_pct = 1'000'000;
  c.ckpt_wal_low_pct = 999'999;
  Engine eng(c);
  eng.start();
  const auto own = make_accounts(eng, 400, 1000);
  CHECK(own[0].size() >= 64 && !own[1].empty());
  const ObjectID dst = own[1][0];
  u64 issued = 0;
  while (eng.stats(0).emits_in_flight <= 600) {  // 58% of the 1024 gate: above the 50% mark
    CHECK(issued < 100000);
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
      ExecOptions o;
      o.durability = Durability::APPLIED;
      TransferArgs t{dst, 1, issued + 1};
      execute(own[0][issued % own[0].size()], kOpTransfer, ByteView(&t, sizeof(t)), o, cb, ctx);
    });
    CHECK_EQ(r.r.status, Status::OK);
    ++issued;
  }
  Engine::debug_ckpt_walk_limit(1);
  Background bg;
  bg.start(eng);
  const GaugeSnapshot g0 = wait_gauges(eng, 0, [](const GaugeSnapshot& g) {
    return g[G_CKPT_PHASE] == kWalk && g[G_CKPT_PRESSURE] == 2 &&
           g[G_PUBLISHED_NS] - g[G_CKPT_STARTED_NS] > 20'000'000;  // many update intervals
  });
  const GaugeSnapshot g1 = wait_gauges(eng, 1, [](const GaugeSnapshot& g) {
    return g[G_CKPT_PHASE] == kWalk && g[G_CKPT_WALK_BUDGET_NS] == 4000;
  });
  CHECK_EQ(g0[G_CKPT_WALK_BUDGET_NS], 1000u);  // held at the floor
  CHECK_EQ(g0[G_CKPT_MODE], 0u);
  CHECK_EQ(g1[G_CKPT_PRESSURE], 0u);
  CHECK_EQ(g1[G_CKPT_MODE], 0u);
  Engine::debug_ckpt_walk_limit(0);
  eng.release_emits();
  bg.join();
  eng.stop();
  rm_rf(dir);
  std::printf("P8 backlog pressure: shard 0 with %llu follow-ups in flight held its walk budget at the 1000 ns "
              "floor (high); shard 1 with none rose to the 4000 ns max (low)\n",
              (unsigned long long)g0[G_EMITS_NOW]);
}

// ---- P7 -------------------------------------------------------------------------------

void test_config_refused() {
  auto refused = [](const std::function<void(Engine::Config&)>& f) {
    Engine::Config c = config(1, "");
    c.init = false;
    f(c);
    try {
      Engine eng(c);
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  CHECK(refused([](Engine::Config& c) { c.ckpt_walk_min_ns = 0; }));
  CHECK(refused([](Engine::Config& c) { c.ckpt_walk_min_ns = c.ckpt_walk_max_ns + 1; }));
  CHECK(refused([](Engine::Config& c) { c.ckpt_walk_ceiling_ns = c.ckpt_walk_max_ns - 1; }));
  CHECK(refused([](Engine::Config& c) { c.ckpt_walk_batch = 0; }));
  CHECK(refused([](Engine::Config& c) { c.ckpt_wal_low_pct = c.ckpt_wal_high_pct; }));
  CHECK(refused([](Engine::Config& c) { c.ckpt_target_ms = c.ckpt_max_age_ms + 1; }));
  CHECK(refused([](Engine::Config& c) { c.ckpt_warn_pct = 101; }));
  CHECK(!refused([](Engine::Config& c) { c.ckpt_max_age_ms = 0; }));  // no maximum is allowed
  std::printf("P7 config: inconsistent pacing values refused at construction\n");
}

}  // namespace

int main(int argc, char** argv) {
  register_type();
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"P1", test_controller},
      {"P7", test_config_refused},
      {"P5", test_crash_positions},  // forks: first, with no engine thread alive
      {"P2", test_bounded_step},
      {"P3", test_foreground_progress},
      {"P4", test_concurrent_consistency},
      {"P6", test_target_and_max_age},
      {"P8", test_backlog_pressure},
  };
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  std::printf("test_s2_pacing: all passed\n");
  return 0;
}
