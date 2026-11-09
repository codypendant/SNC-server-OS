// S4 staggered final checkpoint sync (docs/S4_SYNC_TOKENS.md; work order
// docs/handoff/PR80_S33_Review_S4_Authorization.txt sections 9-12).
//
// Eight shards, so up to eight part syncs compete for K node tokens. The
// final fdatasync's completion is held (Engine::debug_ckpt_hold_sync) and
// released one at a time (debug_ckpt_release_syncs), so a test decides
// when each token comes back. Engine::sync_tokens() gives the allocator's
// counts and, independently, the fdatasyncs really in flight.
//
//   T1  hard K bound, K = 1, 2, 4, 8 and unlimited (0): with every part
//       ready and completions held, exactly min(K, 8) syncs are in flight
//       (counted around the fdatasync itself, not by the tokens), the rest
//       wait, and nothing more starts while held
//   T2  exact release, K = 1 and 2: each released completion lets exactly
//       one waiting shard into its sync, until all eight are done
//   T3  fairness: parts become ready in a chosen order (WAL flush held per
//       shard); tickets follow that order, and after each release exactly
//       the parts inside the eligibility frontier (ticket < releases + K)
//       have acquired, never by shard id; every waiter gets one. Ordered
//       eligibility and eventual acquisition, not strict FIFO grants
//       (docs/S4_SYNC_TOKENS.md section 2). A part
//       waiting for its WAL reads sync_state 0, one waiting for a token 1,
//       one syncing 2 (the HEALTH distinction)
//   T4  foreground progress: with seven shards waiting for the one token,
//       durable deposits and reads complete on every shard, and every
//       waiting shard's WAL keeps flushing
//   T5  crash (K = 1, 2, 8) with zero parts synced, some synced, all synced before the
//       manifest, the manifest written before the superblock, and just
//       after the superblock: each recovers exactly, from the previous
//       checkpoint before the superblock and from the new one after it
//   T6  final-sync failure with the token held: the process aborts with
//       the token already released (checked from the abort itself), the
//       checkpoint does not commit, and the previous one stays
//       authoritative
//   T7  unlimited rollback: K = 0 grants at the first poll (zero token
//       wait), so all eight parts sync at once, as before S4
//   T8  accounting: requests, grants, releases, active, waiting, syncs in
//       flight and the per-shard gauges reconcile exactly
//   T9  repeated checkpoints under load: 30 each at K = 1 and 2, every
//       shard one token per checkpoint, nothing drifts
//   TC  config: K above 256 is refused
// (T10, negative mutations, is bench/results/s4-validation-*/negative.)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "harness.h"
#include "snc/gauges.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr u16 kShards = 8;
constexpr u64 kPerShard = 1500;  // accounts created from each shard

void rm_rf(const std::string& d) { CHECK_EQ(std::system(("rm -rf " + d).c_str()), 0); }
std::string temp_dir() {
  std::string t = "/tmp/snc-s4-XXXXXX";
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

Engine::Config config(const std::string& dir, u32 k) {
  Engine::Config c;
  c.runtime.n_shards = kShards;
  c.runtime.pin = false;
  c.expected_objects = 1 << 17;
  c.data_dir = dir;
  c.init = true;
  c.wal_segment_bytes = 4u << 20;
  c.ckpt_sync_tokens = k;
  return c;
}

Engine::Config reopen(Engine::Config c) {
  c.init = false;
  return c;
}

// Syncs granted at once when every part is ready.
u64 granted_at_once(u32 k) { return k == 0 ? kShards : std::min<u64>(k, kShards); }

const char* k_name(u32 k) {
  static char b[8][16];
  static int i = 0;
  char* out = b[i++ & 7];
  if (k == 0) std::snprintf(out, 16, "unlimited");
  else std::snprintf(out, 16, "%u", k);
  return out;
}

// Polls `pred` until true; fails with `what` after 60 s.
void wait_until(const std::function<bool()>& pred, const char* what) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) {
      std::fprintf(stderr, "timed out waiting for: %s\n", what);
      CHECK(false);
    }
    ::usleep(200);
  }
}

// Every shard's block, each published after this call began.
std::vector<GaugeSnapshot> fresh_all(Engine& eng) {
  const u64 after = sri_now_ns();
  std::vector<GaugeSnapshot> out(kShards);
  for (u16 s = 0; s < kShards; ++s) {
    bool ok = false;
    for (int i = 0; i < 100000 && !ok; ++i) {
      out[s] = eng.gauges(s);
      ok = out[s].consistent && out[s][G_PUBLISHED_NS] > after;
      if (!ok) ::usleep(200);
    }
    CHECK(ok);
  }
  return out;
}

// Token counts relative to a baseline (the counters never reset).
struct Delta {
  u64 requests, grants, releases;
};
Delta delta(const Engine::SyncTokenStats& now, const Engine::SyncTokenStats& base) {
  return {now.requests - base.requests, now.grants - base.grants, now.releases - base.releases};
}

std::vector<ObjectID> make_accounts(Engine& eng, u64 per_shard, i64 balance) {
  std::vector<ObjectID> out;
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
        out.push_back(r.id);
      });
  return out;
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

i64 balance_of(Engine& eng, const ObjectID& id) {
  OneResult r = one(eng, 0, [&](Completion cb, void* ctx) { execute(id, kOpGetBalance, ByteView{}, {}, cb, ctx); });
  if (r.r.status == Status::E_NOT_FOUND) return -1;
  CHECK_EQ(r.r.status, Status::OK);
  return r.as<BalanceResult>().balance_cents;
}

struct Entry {
  ObjectID id;
  i64 balance;
};
using Model = std::vector<Entry>;

Model model_of(const std::vector<ObjectID>& ids, i64 b) {
  Model m;
  for (const ObjectID& id : ids) m.push_back({id, b});
  return m;
}

i64* find(Model& m, const ObjectID& id) {
  for (Entry& e : m) {
    if (e.id == id) return &e.balance;
  }
  CHECK(false);
  return nullptr;
}

void verify(Engine& eng, const Model& m) {
  for (const Entry& e : m) CHECK_EQ(balance_of(eng, e.id), e.balance);
}

// One object each shard owns.
std::vector<ObjectID> one_per_shard(Engine& eng) {
  std::vector<ObjectID> out(kShards);
  for (u16 s = 0; s < kShards; ++s) {
    bool got = false;
    eng.for_each_object(s, [&](const ObjectHeader& o) {
      if (!got && !o.deleted() && o.type_id == kTypeId) {
        out[s] = o.id;
        got = true;
      }
    });
    CHECK(got);
  }
  return out;
}

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

constexpr u64 kIdle = static_cast<u64>(CkptPhase::IDLE);
constexpr u64 kDurableWait = static_cast<u64>(CkptPhase::DURABLE_WAIT);
constexpr u64 kSync = static_cast<u64>(CkptPhase::SYNC);

void recover_exact(const Engine::Config& c, u64 seq, const Model& m) {
  Engine eng(reopen(c));
  eng.start();
  CHECK_EQ(eng.recovery().checkpoint_seq, seq);
  verify(eng, m);
  eng.stop();
}

// T8: the node counts and the per-shard gauges agree, with nothing held
// in between (`g` read after `t` and again before `t2`, which must match).
void check_reconciled(Engine& eng, u32 k) {
  const Engine::SyncTokenStats t = eng.sync_tokens();
  const std::vector<GaugeSnapshot> g = fresh_all(eng);
  const Engine::SyncTokenStats t2 = eng.sync_tokens();
  CHECK_EQ(t.requests, t2.requests);
  CHECK_EQ(t.grants, t2.grants);
  CHECK_EQ(t.releases, t2.releases);
  CHECK_EQ(t.limit, k);
  CHECK_EQ(t.active, t.grants - t.releases);
  CHECK_EQ(t.waiting, t.requests - t.grants);
  CHECK_EQ(t.syncs_inflight, t.active);
  if (k != 0) CHECK(t.active <= k);
  u64 waiting = 0, syncing = 0, acquired = 0;
  for (u16 s = 0; s < kShards; ++s) {
    CHECK_EQ(g[s][G_CKPT_SYNC_TOKENS], k);
    waiting += g[s][G_CKPT_SYNC_STATE] == 1;
    syncing += g[s][G_CKPT_SYNC_STATE] == 2;
    acquired += g[s][G_CKPT_TOKEN_ACQUIRED];
    if (g[s][G_CKPT_SYNC_STATE] == 1) {
      CHECK_EQ(g[s][G_CKPT_PHASE], kDurableWait);
      CHECK(g[s][G_CKPT_TOKEN_WAIT_SINCE_NS] != 0);
    } else {
      CHECK_EQ(g[s][G_CKPT_TOKEN_WAIT_SINCE_NS], 0u);
    }
    if (g[s][G_CKPT_SYNC_STATE] == 2) CHECK_EQ(g[s][G_CKPT_PHASE], kSync);
  }
  CHECK_EQ(waiting, t.waiting);
  CHECK_EQ(syncing, t.active);
  CHECK_EQ(acquired, t.grants);  // every grant of this engine, on some shard
}

// Starts a checkpoint with final syncs held and waits until every part
// has asked for its token and the first min(K, 8) hold one.
void start_all_ready(Engine& eng, Background& bg, const Engine::SyncTokenStats& base, u32 k) {
  Engine::debug_ckpt_hold_sync(true);
  bg.start(eng);
  const u64 e = granted_at_once(k);
  wait_until(
      [&] {
        const Delta d = delta(eng.sync_tokens(), base);
        return d.requests == kShards && d.grants == e;
      },
      "every part ready and min(K, 8) granted");
  // A grant is counted a few instructions before its sync is: wait for
  // the syncs themselves too (the shard can be preempted in between).
  wait_until([&] { return eng.sync_tokens().syncs_inflight == e; }, "min(K, 8) syncs in flight");
}

// ---- T1, T7, T8: hard bound, unlimited rollback ----------------------------------------

void test_bound() {
  std::string report;
  for (u32 k : {1u, 2u, 4u, 8u, 0u}) {
    const std::string dir = temp_dir();
    const Engine::Config c = config(dir, k);
    Model m;
    u64 seq = 0;
    {
      Engine eng(c);
      eng.start();
      m = model_of(make_accounts(eng, kPerShard, 100), 100);
      const Engine::SyncTokenStats base = eng.sync_tokens();
      Background bg;
      start_all_ready(eng, bg, base, k);
      const u64 e = granted_at_once(k);
      // Held: every observation, over a while, shows exactly e in flight.
      u64 max_seen = 0;
      for (int i = 0; i < 400; ++i) {
        const Engine::SyncTokenStats t = eng.sync_tokens();
        const Delta d = delta(t, base);
        max_seen = std::max(max_seen, t.syncs_inflight);
        CHECK_EQ(t.syncs_inflight, e);
        CHECK_EQ(d.grants, e);
        CHECK_EQ(d.requests, static_cast<u64>(kShards));
        CHECK_EQ(d.releases, 0u);
        CHECK_EQ(t.active, e);
        CHECK_EQ(t.waiting, kShards - e);
        if (k != 0) CHECK(t.syncs_inflight <= k);
        ::usleep(250);
      }
      check_reconciled(eng, k);
      CHECK(!bg.done.load());
      Engine::debug_ckpt_hold_sync(false);
      bg.join();
      seq = bg.rep.seq;
      const Engine::SyncTokenStats t = eng.sync_tokens();
      const Delta d = delta(t, base);
      CHECK_EQ(d.requests, static_cast<u64>(kShards));
      CHECK_EQ(d.grants, static_cast<u64>(kShards));
      CHECK_EQ(d.releases, static_cast<u64>(kShards));
      CHECK_EQ(t.active, 0u);
      CHECK_EQ(t.waiting, 0u);
      CHECK_EQ(t.syncs_inflight, 0u);
      CHECK_EQ(t.syncs_inflight_peak, e);
      if (k != 0) CHECK(t.active_peak <= k);
      check_reconciled(eng, k);
      if (k == 0) {
        // T7: unlimited grants at the first poll: no part waited at all.
        for (const GaugeSnapshot& g : fresh_all(eng)) CHECK_EQ(g[G_CKPT_TOKEN_WAIT_MAX_US], 0u);
      }
      char b[96];
      std::snprintf(b, sizeof(b), " K=%s:%llu", k_name(k), (unsigned long long)max_seen);
      report += b;
      eng.stop();
    }
    recover_exact(c, seq, m);
    rm_rf(dir);
  }
  std::printf("T1 hard bound, syncs in flight with every part ready and completions held (K:observed max):%s; "
              "each committed and recovered exactly\n",
              report.c_str());
  std::printf("T7 unlimited: all 8 parts synced at once with zero token wait (the pre-S4 behaviour)\n");
}

// ---- T2: exact one-at-a-time release --------------------------------------------------

void test_exact_release() {
  for (u32 k : {1u, 2u}) {
    const std::string dir = temp_dir();
    const Engine::Config c = config(dir, k);
    Model m;
    u64 seq = 0;
    {
      Engine eng(c);
      eng.start();
      m = model_of(make_accounts(eng, kPerShard, 100), 100);
      const Engine::SyncTokenStats base = eng.sync_tokens();
      Background bg;
      start_all_ready(eng, bg, base, k);
      for (u64 r = 1; r <= kShards; ++r) {
        Engine::debug_ckpt_release_syncs(1);
        const u64 want = std::min<u64>(kShards, k + r);
        wait_until([&] { return delta(eng.sync_tokens(), base).releases == r; }, "one more release");
        wait_until([&] { return delta(eng.sync_tokens(), base).grants == want; }, "the next grant");
        wait_until([&] { return eng.sync_tokens().syncs_inflight == want - r; }, "its sync in flight");
        // Exactly one advanced: nothing more is granted while held.
        for (int i = 0; i < 80; ++i) {
          const Engine::SyncTokenStats t = eng.sync_tokens();
          const Delta d = delta(t, base);
          CHECK_EQ(d.releases, r);
          CHECK_EQ(d.grants, want);
          CHECK_EQ(t.syncs_inflight, want - r);
          ::usleep(250);
        }
        if (r < kShards) check_reconciled(eng, k);
      }
      bg.join();
      seq = bg.rep.seq;
      Engine::debug_ckpt_hold_sync(false);
      check_reconciled(eng, k);
      eng.stop();
    }
    recover_exact(c, seq, m);
    rm_rf(dir);
  }
  std::printf("T2 exact release, K=1 and K=2: each released completion let exactly one waiting part into its "
              "sync, eight times; committed and recovered exactly\n");
}

// ---- T3: fairness in a chosen order ----------------------------------------------------

void test_fairness() {
  const u16 order[kShards] = {3, 7, 0, 5, 1, 6, 2, 4};
  for (u32 k : {1u, 2u}) {
    const std::string dir = temp_dir();
    const Engine::Config c = config(dir, k);
    Model m;
    u64 seq = 0;
    {
      Engine eng(c);
      eng.start();
      m = model_of(make_accounts(eng, kPerShard, 100), 100);
      const std::vector<ObjectID> own = one_per_shard(eng);
      // Every part waits for its WAL: a deposit applied, its flush held.
      for (u16 s = 0; s < kShards; ++s) Engine::debug_hold_wal(s, true);
      for (u16 s = 0; s < kShards; ++s) {
        deposit(eng, own[s], 7, Durability::APPLIED);
        *find(m, own[s]) += 7;
      }
      const Engine::SyncTokenStats base = eng.sync_tokens();
      Engine::debug_ckpt_hold_sync(true);
      Background bg;
      bg.start(eng);
      // Waiting for the WAL, not a token: sync_state 0 in DURABLE_WAIT.
      std::vector<GaugeSnapshot> g;
      wait_until(
          [&] {
            g = fresh_all(eng);
            for (const GaugeSnapshot& x : g) {
              if (x[G_CKPT_PHASE] != kDurableWait) return false;
            }
            return true;
          },
          "every part in DURABLE_WAIT on its WAL");
      for (const GaugeSnapshot& x : g) CHECK_EQ(x[G_CKPT_SYNC_STATE], 0u);
      CHECK_EQ(delta(eng.sync_tokens(), base).requests, 0u);
      // Ready in the chosen order: tickets follow it.
      for (u16 i = 0; i < kShards; ++i) {
        Engine::debug_hold_wal(order[i], false);
        wait_until([&] { return delta(eng.sync_tokens(), base).requests == i + 1u; }, "the next ticket");
        const GaugeSnapshot x = fresh_all(eng)[order[i]];
        CHECK_EQ(x[G_CKPT_TOKEN_TICKET] - 1 - base.requests, static_cast<u64>(i));
      }
      wait_until([&] { return delta(eng.sync_tokens(), base).grants == granted_at_once(k); }, "the first grants");
      // The eligibility frontier: after r releases, exactly the parts with
      // tickets below r + K have had a token (every eligible one acquired,
      // none outside).
      for (u64 r = 0; r <= kShards; ++r) {
        if (r > 0) {
          Engine::debug_ckpt_release_syncs(1);
          wait_until([&] { return delta(eng.sync_tokens(), base).releases == r; }, "one more release");
          wait_until([&] { return delta(eng.sync_tokens(), base).grants == std::min<u64>(kShards, k + r); },
                     "the next grant");
        }
        if (r == kShards) break;
        g = fresh_all(eng);
        for (u16 i = 0; i < kShards; ++i) {
          const GaugeSnapshot& x = g[order[i]];
          const bool had_token = x[G_CKPT_SYNC_STATE] == 2 || x[G_CKPT_PHASE] == kIdle;
          CHECK_EQ(had_token, i < r + k);
          if (i >= r + k) {
            CHECK_EQ(x[G_CKPT_SYNC_STATE], 1u);  // waiting for a token, not for its WAL
            CHECK_EQ(x[G_CKPT_PHASE], kDurableWait);
          }
        }
      }
      bg.join();
      seq = bg.rep.seq;
      Engine::debug_ckpt_hold_sync(false);
      // Every waiter got a token (the order is checked above).
      g = fresh_all(eng);
      for (u16 s = 0; s < kShards; ++s) CHECK(g[s][G_CKPT_TOKEN_ACQUIRED] >= 1);
      check_reconciled(eng, k);
      eng.stop();
    }
    recover_exact(c, seq, m);
    rm_rf(dir);
  }
  std::printf("T3 fairness, K=1 and K=2: parts ready in the order 3 7 0 5 1 6 2 4 took tickets in that order and "
              "after each release exactly the parts inside the eligibility frontier had acquired; every waiter acquired; WAL wait (sync_state 0), token wait (1) and sync (2) "
              "told apart\n");
}

// ---- T4: foreground progress while waiting ---------------------------------------------

void test_foreground() {
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, 1);
  Model m;
  u64 seq = 0;
  {
    Engine eng(c);
    eng.start();
    const std::vector<ObjectID> ids = make_accounts(eng, kPerShard, 100);
    m = model_of(ids, 100);
    const Engine::SyncTokenStats base = eng.sync_tokens();
    Background bg;
    start_all_ready(eng, bg, base, 1);
    const std::vector<GaugeSnapshot> before = fresh_all(eng);
    u32 waiting = 0;
    for (const GaugeSnapshot& g : before) waiting += g[G_CKPT_SYNC_STATE] == 1;
    CHECK_EQ(waiting, kShards - 1u);
    // Durable deposits spread over every shard, then reads.
    for (std::size_t i = 0; i < 800; ++i) {
      const std::size_t j = (i * 37) % ids.size();
      deposit(eng, ids[j], 3);
      m[j].balance += 3;
    }
    for (std::size_t i = 0; i < 200; ++i) {
      const std::size_t j = (i * 53) % ids.size();
      CHECK_EQ(balance_of(eng, ids[j]), m[j].balance);
    }
    const std::vector<GaugeSnapshot> after = fresh_all(eng);
    for (u16 s = 0; s < kShards; ++s) {
      CHECK(after[s][G_WAL_DURABLE_LSN] > before[s][G_WAL_DURABLE_LSN]);  // this shard's WAL kept flushing
      CHECK(after[s][G_WAL_FLUSHES] > before[s][G_WAL_FLUSHES]);
      CHECK_EQ(after[s][G_CKPT_SYNC_STATE], before[s][G_CKPT_SYNC_STATE]);  // still waiting / still syncing
    }
    const Delta d = delta(eng.sync_tokens(), base);
    CHECK_EQ(d.grants, 1u);
    CHECK_EQ(d.releases, 0u);
    CHECK(!bg.done.load());
    Engine::debug_ckpt_hold_sync(false);
    bg.join();
    seq = bg.rep.seq;
    verify(eng, m);
    eng.stop();
  }
  recover_exact(c, seq, m);
  rm_rf(dir);
  std::printf("T4 foreground progress, K=1: with 7 parts waiting for the token, 800 durable deposits and 200 "
              "reads completed and every shard's WAL kept flushing; the checkpoint then committed\n");
}

// ---- T5, T6: crash and failure ---------------------------------------------------------

struct CrashFile {
  u64 seq = 0;
  u64 n = 0;
};

void write_model(const std::string& dir, u64 seq, const Model& m) {
  const int fd = ::open((dir + "/model").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) std::_Exit(3);
  CrashFile f{seq, m.size()};
  if (::write(fd, &f, sizeof(f)) != static_cast<ssize_t>(sizeof(f))) std::_Exit(4);
  const ssize_t bytes = static_cast<ssize_t>(m.size() * sizeof(Entry));
  if (::write(fd, m.data(), static_cast<std::size_t>(bytes)) != bytes) std::_Exit(5);
  ::fsync(fd);
  ::close(fd);
}

Model read_model(const std::string& dir, u64& seq) {
  CrashFile f{};
  Model m;
  FILE* in = std::fopen((dir + "/model").c_str(), "rb");
  CHECK(in != nullptr);
  CHECK_EQ(std::fread(&f, sizeof(f), 1, in), 1u);
  m.resize(f.n);
  CHECK_EQ(std::fread(m.data(), sizeof(Entry), f.n, in), f.n);
  std::fclose(in);
  seq = f.seq;
  return m;
}

pid_t fork_child(const std::function<void()>& body) {
  CHECK(thread_count() <= 1u + kRuntimeThreads);  // no engine thread alive
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    body();
    std::_Exit(0);
  }
  return pid;
}

// A committed checkpoint, then deposits after it (recovery replays them).
struct ChildState {
  std::vector<ObjectID> ids;
  Model m;
  u64 seq = 0;
};

ChildState child_setup(Engine& eng) {
  ChildState s;
  s.ids = make_accounts(eng, kPerShard, 100);
  s.m = model_of(s.ids, 100);
  s.seq = eng.checkpoint().seq;
  for (std::size_t i = 0; i < 300; ++i) {
    const std::size_t j = (i * 7) % s.ids.size();
    deposit(eng, s.ids[j], 5);
    s.m[j].balance += 5;
  }
  return s;
}

void more_deposits(Engine& eng, ChildState& s) {
  for (std::size_t i = 0; i < 100; ++i) {
    const std::size_t j = (i * 11) % s.ids.size();
    deposit(eng, s.ids[j], 2);
    s.m[j].balance += 2;
  }
}

// After a crash child: the checkpoint the model names is used and the state
// is exact; a new checkpoint then commits and recovers exactly.
void recover_after_crash(const std::string& dir, u32 k) {
  u64 seq = 0;
  const Model m = read_model(dir, seq);
  const Engine::Config c = reopen(config(dir, k));
  u64 next = 0;
  {
    Engine eng(c);
    eng.start();
    CHECK_EQ(eng.recovery().checkpoint_seq, seq);
    verify(eng, m);
    next = eng.checkpoint().seq;
    CHECK(next > seq);
    eng.stop();
  }
  recover_exact(c, next, m);
}

enum class Pos { NONE_SYNCED, SOME_SYNCED, ALL_SYNCED, MANIFEST, COMMITTED };
const char* pos_name(Pos p) {
  switch (p) {
    case Pos::NONE_SYNCED: return "none-synced";
    case Pos::SOME_SYNCED: return "some-synced";
    case Pos::ALL_SYNCED: return "all-synced-before-manifest";
    case Pos::MANIFEST: return "manifest-before-superblock";
    case Pos::COMMITTED: return "after-superblock";
  }
  return "?";
}

void test_crash() {
  std::string report;
  for (u32 k : {1u, 2u, 8u}) {
    report += std::string(" K=") + k_name(k) + ":";
    for (Pos p : {Pos::NONE_SYNCED, Pos::SOME_SYNCED, Pos::ALL_SYNCED, Pos::MANIFEST, Pos::COMMITTED}) {
      const std::string dir = temp_dir();
      const pid_t pid = fork_child([&] {
        Engine eng(config(dir, k));
        eng.start();
        ChildState s = child_setup(eng);
        const Engine::SyncTokenStats base = eng.sync_tokens();
        Background bg;
        u64 seq = s.seq;
        if (p == Pos::NONE_SYNCED || p == Pos::SOME_SYNCED) {
          start_all_ready(eng, bg, base, k);
          if (p == Pos::SOME_SYNCED) {
            Engine::debug_ckpt_release_syncs(3);
            wait_until(
                [&] {
                  const Delta d = delta(eng.sync_tokens(), base);
                  return d.releases == 3 && d.grants == std::min<u64>(kShards, k + 3);
                },
                "three parts synced, the next K syncing");
          }
          if (delta(eng.sync_tokens(), base).releases != (p == Pos::SOME_SYNCED ? 3u : 0u)) std::_Exit(8);
        } else if (p == Pos::ALL_SYNCED) {
          Engine::debug_ckpt_hold_commit(true);
          bg.start(eng);
          while (!Engine::debug_ckpt_commit_reached()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
          if (delta(eng.sync_tokens(), base).releases != kShards) std::_Exit(9);
        } else if (p == Pos::MANIFEST) {
          Engine::debug_ckpt_hold_superblock(true);
          bg.start(eng);
          while (!Engine::debug_ckpt_superblock_reached()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
          seq = eng.checkpoint().seq;  // committed: the new one is authoritative
          if (seq <= s.seq) std::_Exit(10);
        }
        more_deposits(eng, s);  // the WAL carries them
        write_model(dir, seq, s.m);
        std::fflush(nullptr);
        std::_Exit(0);  // the crash: nothing stopped
      });
      int st = 0;
      CHECK_EQ(::waitpid(pid, &st, 0), pid);
      CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
      recover_after_crash(dir, k);
      rm_rf(dir);
      report += std::string(" ") + pos_name(p);
    }
  }
  std::printf("T5 crash, K=1, 2 and 8:%s; each recovered exactly (previous checkpoint before the superblock, the new one "
              "after), then a new checkpoint committed and recovered\n",
              report.c_str());
}

// T6: read from the abort itself. The token must already be back.
// Atomic: written by the test thread, read in the handler on a shard
// thread (a plain pointer is a data race TSan reports).
std::atomic<Engine*> g_abort_engine{nullptr};
void put_u64(char*& p, u64 v) {
  char t[24];
  int n = 0;
  do {
    t[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (n > 0) *p++ = t[--n];
}
void on_abort(int) {
  if (Engine* e = g_abort_engine.load(); e != nullptr) {
    const Engine::SyncTokenStats t = e->sync_tokens();
    char b[160];
    char* p = b;
    const char* a = "T6 abort: grants=";
    while (*a) *p++ = *a++;
    put_u64(p, t.grants);
    a = " releases=";
    while (*a) *p++ = *a++;
    put_u64(p, t.releases);
    a = " inflight=";
    while (*a) *p++ = *a++;
    put_u64(p, t.syncs_inflight);
    *p++ = '\n';
    if (::write(2, b, static_cast<std::size_t>(p - b)) < 0) {
    }
  }
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

void test_sync_failure() {
  const u32 k = 1;
  const u16 bad = 3;
  const std::string dir = temp_dir();
  const pid_t pid = fork_child([&] {
    const int fd = ::open((dir + "/stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ::dup2(fd, 2) < 0) std::_Exit(6);
    Engine eng(config(dir, k));
    eng.start();
    ChildState s = child_setup(eng);
    const std::vector<ObjectID> own = one_per_shard(eng);
    // The failing shard asks for its token last, so nothing else holds or
    // wants one when it fails: the abort must see every token back.
    Engine::debug_hold_wal(bad, true);
    deposit(eng, own[bad], 9, Durability::APPLIED);
    *find(s.m, own[bad]) += 9;  // durable before the part may sync
    write_model(dir, s.seq, s.m);
    const Engine::SyncTokenStats base = eng.sync_tokens();
    Engine::debug_ckpt_fail_sync(bad);
    Background bg;
    bg.start(eng);
    wait_until(
        [&] {
          const Delta d = delta(eng.sync_tokens(), base);
          return d.requests == kShards - 1u && d.releases == kShards - 1u;
        },
        "every other part synced");
    const Engine::SyncTokenStats t = eng.sync_tokens();
    std::fprintf(stderr, "T6 child: before the failure grants=%llu releases=%llu\n", (unsigned long long)t.grants,
                 (unsigned long long)t.releases);
    std::fflush(stderr);
    g_abort_engine.store(&eng);
    std::signal(SIGABRT, on_abort);
    Engine::debug_hold_wal(bad, false);  // the last part syncs, holding the token, and fails
    bg.join();                           // must not return
    std::_Exit(7);
  });
  int st = 0;
  CHECK_EQ(::waitpid(pid, &st, 0), pid);
  std::ifstream in(dir + "/stderr");
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  std::printf("%s", text.c_str());
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
  CHECK(text.find("checkpoint fdatasync failed") != std::string::npos);
  // The child's counts: before, every token of the other seven back; at the
  // abort, one more grant and its release (S4-I2 on the fatal path).
  unsigned long long g0 = 0, r0 = 0, g1 = 0, r1 = 0, inflight = 99;
  const std::size_t a = text.find("T6 child: before the failure");
  const std::size_t b = text.find("T6 abort:");
  CHECK(a != std::string::npos && b != std::string::npos);
  CHECK_EQ(std::sscanf(text.c_str() + a, "T6 child: before the failure grants=%llu releases=%llu",
                       &g0, &r0),
           2);
  CHECK_EQ(std::sscanf(text.c_str() + b, "T6 abort: grants=%llu releases=%llu inflight=%llu",
                       &g1, &r1, &inflight),
           3);
  CHECK_EQ(g0, r0);
  CHECK_EQ(g1, g0 + 1);
  CHECK_EQ(r1, g1);
  CHECK_EQ(inflight, 0ull);
  CHECK(text.find("Sanitizer") == std::string::npos);
  CHECK(text.find("runtime error") == std::string::npos);
  recover_after_crash(dir, k);
  rm_rf(dir);
  std::printf("T6 final-sync failure, K=1: -EIO on the last part with its token held aborted with the token already "
              "released (grants = releases at the abort, none in flight); nothing committed, the previous "
              "checkpoint recovered exactly\n");
}

// ---- T9: repeated checkpoints under load ----------------------------------------------

void test_repeated() {
  std::string report;
  for (u32 k : {1u, 2u}) {
    const std::string dir = temp_dir();
    const Engine::Config c = config(dir, k);
    Model m;
    u64 seq = 0;
    constexpr int kRounds = 30;
    {
      Engine eng(c);
      eng.start();
      const std::vector<ObjectID> ids = make_accounts(eng, 500, 100);
      m = model_of(ids, 100);
      const Engine::SyncTokenStats base = eng.sync_tokens();
      std::vector<u64> acquired0(kShards);
      {
        const std::vector<GaugeSnapshot> g = fresh_all(eng);
        for (u16 s = 0; s < kShards; ++s) acquired0[s] = g[s][G_CKPT_TOKEN_ACQUIRED];
      }
      // Load: deposits on another thread for the whole run.
      std::atomic<bool> stop{false};
      std::atomic<u64> ops{0};
      std::mutex mu;
      std::thread load([&] {
        for (std::size_t i = 0; !stop.load(); ++i) {
          const std::size_t j = (i * 31) % ids.size();
          deposit(eng, ids[j], 1);
          std::lock_guard<std::mutex> g(mu);
          m[j].balance += 1;
          ++ops;
        }
      });
      for (int r = 1; r <= kRounds; ++r) {
        const CheckpointReport rep = eng.checkpoint();
        CHECK(rep.seq > seq);
        seq = rep.seq;
        const Engine::SyncTokenStats t = eng.sync_tokens();
        const Delta d = delta(t, base);
        CHECK_EQ(d.requests, static_cast<u64>(r) * kShards);
        CHECK_EQ(d.grants, d.requests);
        CHECK_EQ(d.releases, d.requests);
        CHECK_EQ(t.active, 0u);
        CHECK_EQ(t.waiting, 0u);
        CHECK_EQ(t.syncs_inflight, 0u);
        CHECK(t.syncs_inflight_peak <= k);
        const std::vector<GaugeSnapshot> g = fresh_all(eng);
        for (u16 s = 0; s < kShards; ++s) CHECK_EQ(g[s][G_CKPT_TOKEN_ACQUIRED] - acquired0[s], static_cast<u64>(r));
      }
      stop = true;
      load.join();
      const Engine::SyncTokenStats t = eng.sync_tokens();
      char b[160];
      std::snprintf(b, sizeof(b), " K=%u: %d checkpoints, %llu deposits, peak in flight %llu, peak waiting %llu,",
                    k, kRounds, (unsigned long long)ops.load(), (unsigned long long)t.syncs_inflight_peak,
                    (unsigned long long)t.waiting_peak);
      report += b;
      check_reconciled(eng, k);
      verify(eng, m);
      eng.stop();
    }
    recover_exact(c, seq, m);
    rm_rf(dir);
  }
  std::printf("T9 repeated checkpoints under load:%s every shard one token per checkpoint, all counts reconciled, "
              "recovered exactly\n",
              report.c_str());
}

// ---- TC: config -----------------------------------------------------------------------

void test_config() {
  bool threw = false;
  try {
    Engine eng(config("/tmp/snc-s4-unused", 257));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  {
    Engine eng(config("/tmp/snc-s4-unused", 256));  // the maximum is accepted
    CHECK_EQ(eng.sync_tokens().limit, 256u);
  }
  std::printf("TC config: K=257 refused, 256 accepted\n");
}

}  // namespace

int main(int argc, char** argv) {
  register_type();
  // Each test sets its own K; the suite-wide override would replace it.
  if (std::getenv("SNC_CKPT_SYNC_TOKENS") != nullptr) {
    std::printf("test_s4_sync_tokens: SNC_CKPT_SYNC_TOKENS ignored (each test sets its own K)\n");
    ::unsetenv("SNC_CKPT_SYNC_TOKENS");
  }
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"TC", test_config},
      {"T6", test_sync_failure},  // forks: first, with no engine thread alive
      {"T5", test_crash},         // forks
      {"T1", test_bound},
      {"T2", test_exact_release},
      {"T3", test_fairness},
      {"T4", test_foreground},
      {"T9", test_repeated},
  };
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  std::printf("test_s4_sync_tokens: all passed\n");
  return 0;
}
