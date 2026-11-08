// S3 bounded checkpoint writeback (docs/S3_WRITEBACK.md; work order
// docs/handoff/PR76_S3D_Review.txt sections 3-9 and 16).
//
// Checkpoint chunk write completions are held (Engine::debug_ckpt_hold_writes)
// so the tests decide when, and in what order, each write completes.
//
//   T1  window hard bound, W = 1, 2, 4: with completions held, exactly W
//       chunk writes are submitted, the walk stops (its cursor does not
//       move), and no further chunk is built or submitted
//   T3  resume: releasing one completion lets exactly one more chunk be
//       submitted, then the walk stops again; released, the checkpoint
//       completes and recovers exactly (T1 and T3 run together)
//   T2  memory hard bound: a part of many chunks, written while
//       completions trickle out one at a time; retained chunk-buffer bytes
//       never exceed (W + 1) chunk reservations, against a part many times
//       larger
//   T2b the same with maximum-size (1 MiB) objects ending chunks: the
//       peak stays within (W + 1) x kCheckpointChunkMax + one reservation
//   T4  out-of-order completion: submissions deferred and issued newest
//       first, so each completion arrives while older slots still own
//       unissued buffers; a wrong or FIFO free would corrupt the part.
//       Accounting stays exact and the checkpoint recovers exactly
//   T5  final partial chunk under a full window: every ID visited, the
//       last chunk waits for a slot; the phase stays WALK until it is
//       submitted, then DRAIN follows
//   T6  exact completion: a positive short write and an -EIO each kill the
//       process before the commit; recovery uses the previous checkpoint
//       and the WAL to the exact acknowledged state
//   T7  crash positions: chunks in flight, some complete and some not,
//       all complete with the part waiting on WAL durability, and every
//       part synced before the manifest; each recovers from the previous
//       checkpoint exactly, and a new checkpoint then commits. A process
//       crash, not a power loss: bytes the kernel accepted survive it, so
//       "some durable, some not" is the shard's view of completions, not
//       the device's (docs/S3_WRITEBACK.md section 9)
//   T8  final sync: a dsync checkpoint commits; objects, bytes and the
//       part's size on disk agree exactly
//   T9  legacy: --ckpt-io legacy submits every chunk with no window and
//       commits and recovers exactly
//   T10 synchronous fallback: forced, every chunk write is counted as
//       synchronous and the checkpoint is still exact; with io_uring and
//       no force, none is
//   T11 foreground progress: with the window full and the walk stopped,
//       deposits and reads complete
//   T13 config: a window of 0 or above 64 is refused
//   T14-T18 lifecycle (S3.1): stop, final sync, destruction, process exit
//       and a storage failure with checkpoint I/O in flight (see each test)
// (T12, the existing suites under dsync, is the rest of ctest: dsync is
// the default.)
#include <dirent.h>
#include <fcntl.h>
#include <liburing.h>
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

#include "../src/core/shard.h"
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
  std::string t = "/tmp/snc-s3-XXXXXX";
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

Engine::Config config(const std::string& dir, u32 window, bool dsync = true) {
  Engine::Config c;
  c.runtime.n_shards = 1;
  c.runtime.pin = false;
  c.expected_objects = 1 << 18;
  c.data_dir = dir;
  c.init = true;
  c.wal_segment_bytes = 4u << 20;
  c.ckpt_io_dsync = dsync;
  c.ckpt_write_window = window;
  return c;
}

Engine::Config reopen(Engine::Config c) {
  c.init = false;
  return c;
}

GaugeSnapshot fresh(Engine& eng) {
  const u64 after = sri_now_ns();
  for (int i = 0; i < 20000; ++i) {
    GaugeSnapshot g = eng.gauges(0);
    if (g.consistent && g[G_PUBLISHED_NS] > after) return g;
    ::usleep(200);
  }
  CHECK(false);
  return {};
}

template <typename P>
GaugeSnapshot wait_gauges(Engine& eng, P pred) {
  for (int i = 0; i < 300000; ++i) {
    GaugeSnapshot g = fresh(eng);
    if (pred(g)) return g;
  }
  CHECK(false);
  return {};
}

std::vector<ObjectID> make_accounts(Engine& eng, u64 n, i64 balance) {
  std::vector<ObjectID> out;
  std::mutex mu;
  run_all(
      eng, n,
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

void add(Model& m, std::size_t i, i64 delta) { m[i].balance += delta; }

void verify(Engine& eng, const Model& m) {
  for (const Entry& e : m) CHECK_EQ(balance_of(eng, e.id), e.balance);
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

constexpr u64 kWalk = static_cast<u64>(CkptPhase::WALK);
constexpr u64 kDurableWait = static_cast<u64>(CkptPhase::DURABLE_WAIT);
constexpr u64 kIdle = static_cast<u64>(CkptPhase::IDLE);
constexpr u64 kDrain = static_cast<u64>(CkptPhase::DRAIN);

// The retained-buffer envelope for small records (docs/S3_WRITEBACK.md
// section 3): the chunk being built plus W in flight, each one reservation.
u64 envelope(u32 w) { return (static_cast<u64>(w) + 1) * kCheckpointChunkReserve; }

u64 file_size(const std::string& p) {
  struct stat st{};
  CHECK_EQ(::stat(p.c_str(), &st), 0);
  return static_cast<u64>(st.st_size);
}

std::string part_file(const std::string& dir, u64 seq) {
  char b[64];
  std::snprintf(b, sizeof(b), "/ckpt/%016llx/part-000.ckpt", (unsigned long long)seq);
  return dir + b;
}

// Recovers `dir` with `c` and checks the active checkpoint and the model.
void recover_exact(const Engine::Config& c, u64 seq, const Model& m) {
  Engine eng(reopen(c));
  eng.start();
  CHECK_EQ(eng.recovery().checkpoint_seq, seq);
  verify(eng, m);
  eng.stop();
}

// The common end of a held checkpoint: hold off, the checkpoint commits,
// the counters agree exactly, recovery gives the model.
void finish_and_check(Engine& eng, Background& bg, const Engine::Config& c, const Model& m) {
  Engine::debug_ckpt_hold_writes(false);
  bg.join();
  const GaugeSnapshot g = fresh(eng);
  CHECK_EQ(g[G_CKPT_PHASE], kIdle);
  CHECK_EQ(g[G_CKPT_BYTES_WRITTEN], g[G_CKPT_BYTES_ISSUED]);
  CHECK_EQ(g[G_CKPT_BUF_BYTES], 0u);
  CHECK_EQ(file_size(part_file(c.data_dir, bg.rep.seq)), g[G_CKPT_BYTES_ISSUED]);
  eng.stop();
  recover_exact(c, bg.rep.seq, m);
}

// ---- T1 + T3: window bound and resume ------------------------------------------------

void test_window_bound() {
  std::string report;
  for (u32 w : {1u, 2u, 4u}) {
    const std::string dir = temp_dir();
    const Engine::Config c = config(dir, w);
    Engine eng(c);
    eng.start();
    const std::vector<ObjectID> ids = make_accounts(eng, 100000, 100);
    const Model m = model_of(ids, 100);
    Engine::debug_ckpt_hold_writes(true);
    Background bg;
    bg.start(eng);
    GaugeSnapshot g = wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_WINDOW_FULL] == 1; });
    CHECK_EQ(g[G_CKPT_WINDOW], w);
    CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT], w);
    CHECK_EQ(g[G_CKPT_CHUNKS], w);
    CHECK_EQ(g[G_CKPT_PHASE], kWalk);
    CHECK(g[G_CKPT_BUF_BYTES] <= envelope(w));
    const u64 cursor = g[G_CKPT_WALK_DONE];
    CHECK(cursor < g[G_CKPT_WALK_TOTAL]);
    // Stays stopped: no chunk built or submitted, the cursor still, the
    // stopped iterations counted.
    const u64 steps = g[G_CKPT_WINDOW_FULL_STEPS];
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    g = fresh(eng);
    CHECK_EQ(g[G_CKPT_CHUNKS], w);
    CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT], w);
    CHECK_EQ(g[G_CKPT_WALK_DONE], cursor);
    CHECK_EQ(g[G_CKPT_WINDOW_FULL], 1u);
    CHECK(g[G_CKPT_WINDOW_FULL_STEPS] > steps);
    CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT_PEAK], w);
    // T3: one completion frees exactly one slot.
    u64 prev = cursor;
    for (u32 k = 1; k <= 3; ++k) {
      Engine::debug_ckpt_release_writes(1, false);
      g = wait_gauges(eng, [&](const GaugeSnapshot& x) { return x[G_CKPT_CHUNKS] == w + k && x[G_CKPT_WINDOW_FULL] == 1; });
      CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT], w);
      CHECK(g[G_CKPT_WALK_DONE] > prev);  // the walk resumed, then stopped again
      prev = g[G_CKPT_WALK_DONE];
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      g = fresh(eng);
      CHECK_EQ(g[G_CKPT_CHUNKS], w + k);
      CHECK_EQ(g[G_CKPT_WALK_DONE], prev);
    }
    CHECK_EQ(fresh(eng)[G_CKPT_WRITES_INFLIGHT_PEAK], w);
    finish_and_check(eng, bg, c, m);
    rm_rf(dir);
    char line[96];
    std::snprintf(line, sizeof(line), " W=%u stopped at %llu/%llu", w, (unsigned long long)cursor,
                  (unsigned long long)g[G_CKPT_WALK_TOTAL]);
    report += line;
  }
  std::printf("T1+T3 window bound:%s; exactly W submitted while held, one more per completion, then exact\n",
              report.c_str());
}

// ---- T2: memory bound ----------------------------------------------------------------

// Slow storage: a completion is let through only once the window is full
// (or the walk has ended), at most one every 10 ms, so the window is full
// whenever the walk could produce. Gated on the gauge, not on a fixed
// interval: under a sanitizer the walk can take longer than any fixed
// interval to fill a chunk, and the peak checks must not depend on timing.
void slow_storage(Engine& eng, Background& bg) {
  while (!bg.done.load()) {
    wait_gauges(eng, [&](const GaugeSnapshot& x) {
      return bg.done.load() || x[G_CKPT_WINDOW_FULL] == 1 || x[G_CKPT_PHASE] != kWalk;
    });
    Engine::debug_ckpt_release_writes(1, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void test_memory_bound() {
  const u32 w = 2;
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, w);
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 200000, 100);
  const Model m = model_of(ids, 100);
  Engine::debug_ckpt_hold_writes(true);
  Background bg;
  bg.start(eng);
  slow_storage(eng, bg);
  Engine::debug_ckpt_hold_writes(false);
  bg.join();
  const GaugeSnapshot g = fresh(eng);
  const u64 bytes = g[G_CKPT_BYTES_ISSUED];
  CHECK(g[G_CKPT_CHUNKS] >= 10);
  CHECK(g[G_CKPT_BUF_BYTES_PEAK] <= envelope(w));
  CHECK(bytes > 4 * envelope(w));
  CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT_PEAK], w);
  CHECK(g[G_CKPT_WINDOW_FULL_STEPS] > 0);  // the bound held under a full window
  CHECK_EQ(g[G_CKPT_BUF_BYTES], 0u);
  eng.stop();
  recover_exact(c, bg.rep.seq, m);
  rm_rf(dir);
  std::printf("T2 memory bound: %llu chunks, part %llu bytes, retained peak %llu <= %llu (W=%u)\n",
              (unsigned long long)g[G_CKPT_CHUNKS], (unsigned long long)bytes,
              (unsigned long long)g[G_CKPT_BUF_BYTES_PEAK], (unsigned long long)envelope(w), w);
}

// ---- T2b: memory bound with maximum-size objects ---------------------------------------

// Big: an object of `len` payload bytes, up to kMaxPayload (1 MiB), so a
// chunk can end with a maximum-size record.
namespace big {
constexpr u32 kType = 910;
constexpr OpID kCreate = 9101;
struct LenArgs {
  u32 len;
};
Status create_v(OpCtx& c, const ObjectHeader&, ByteView a) {
  LenArgs l{};
  std::memcpy(&l, a.data, std::min<std::size_t>(a.len, sizeof(l)));
  return c.reserve(l.len);
}
void create_a(OpCtx& c, ByteView a) {
  LenArgs l{};
  std::memcpy(&l, a.data, std::min<std::size_t>(a.len, sizeof(l)));
  c.resize(l.len);
  if (l.len) c.obj()->payload()[0] = 0x5a;
}
void register_big() {
  Registry& r = registry();
  if (r.type(kType)) return;
  TypeDescriptor t;
  t.type_id = kType;
  t.name = "Big";
  t.initial_size = 16;
  t.max_size = kMaxPayload;
  t.create_op = kCreate;
  r.add_type(t);
  OpDescriptor d;
  d.op_id = kCreate;
  d.type_id = kType;
  d.name = "Big.create";
  d.kind = OpKind::CREATE;
  d.max_args = sizeof(LenArgs);
  d.validate = create_v;
  d.apply = create_a;
  r.add_op(d);
}
}  // namespace big

void test_memory_bound_max_objects() {
  big::register_big();
  const u32 w = 2;
  const std::string dir = temp_dir();
  Engine::Config c = config(dir, w);
  c.wal_segment_bytes = 64u << 20;
  Engine eng(c);
  eng.start();
  // Small accounts with a maximum-size object after every ~1 MiB of them,
  // so chunks end on 1 MiB records: the worst case for one buffer.
  std::vector<ObjectID> ids;
  for (int k = 0; k < 12; ++k) {
    const std::vector<ObjectID> part = make_accounts(eng, 3500, 100);
    ids.insert(ids.end(), part.begin(), part.end());
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
      big::LenArgs a{kMaxPayload};
      create(big::kType, ByteView(&a, sizeof(a)), {}, cb, ctx);
    });
    CHECK_EQ(r.r.status, Status::OK);
  }
  const Model m = model_of(ids, 100);
  Engine::debug_ckpt_hold_writes(true);
  Background bg;
  bg.start(eng);
  slow_storage(eng, bg);
  Engine::debug_ckpt_hold_writes(false);
  bg.join();
  const GaugeSnapshot g = fresh(eng);
  // W in flight and the chunk being built, each at most kCheckpointChunkMax,
  // plus, while that chunk grows for a record that does not fit, its old
  // reservation (docs/S3_WRITEBACK.md section 3).
  const u64 bound = (static_cast<u64>(w) + 1) * kCheckpointChunkMax + kCheckpointChunkReserve;
  CHECK(g[G_CKPT_BUF_BYTES_PEAK] <= bound);
  CHECK(g[G_CKPT_BUF_BYTES_PEAK] > envelope(w));  // a buffer really grew for a maximum-size record
  CHECK(g[G_CKPT_WINDOW_FULL_STEPS] > 0);
  CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT_PEAK], w);
  CHECK(g[G_CKPT_BYTES_ISSUED] > 3 * bound);
  eng.stop();
  recover_exact(c, bg.rep.seq, m);
  rm_rf(dir);
  std::printf("T2b memory bound, 1 MiB objects: part %llu bytes, retained peak %llu <= %llu (W=%u)\n",
              (unsigned long long)g[G_CKPT_BYTES_ISSUED], (unsigned long long)g[G_CKPT_BUF_BYTES_PEAK],
              (unsigned long long)bound, w);
}

// ---- T4: out-of-order completion -----------------------------------------------------

void test_out_of_order() {
  const u32 w = 4;
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, w);
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 60000, 100);
  const Model m = model_of(ids, 100);
  // Submissions deferred: each submitted chunk's buffer is owned by its
  // slot but not yet handed to the kernel. Issuing newest first completes
  // the newest write while older slots still hold unissued buffers, so an
  // implementation that freed by FIFO position, or freed any buffer but
  // the completed write's own, would issue a freed buffer and corrupt the
  // part (or fail the write), and recovery below would not be exact.
  Engine::debug_ckpt_defer_writes(true);
  Background bg;
  bg.start(eng);
  GaugeSnapshot g = wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_WINDOW_FULL] == 1; });
  CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT], w);
  CHECK_EQ(g[G_CKPT_BYTES_WRITTEN], 0u);  // none issued yet
  u64 rounds = 0;
  while (!bg.done.load()) {
    const u64 before = fresh(eng)[G_CKPT_BYTES_WRITTEN];
    Engine::debug_ckpt_release_writes(1, true);
    g = wait_gauges(eng, [&](const GaugeSnapshot& x) {
      return x[G_CKPT_BYTES_WRITTEN] > before || x[G_CKPT_PHASE] == kIdle;
    });
    CHECK(g[G_CKPT_BYTES_WRITTEN] <= g[G_CKPT_BYTES_ISSUED]);
    CHECK(g[G_CKPT_WRITES_INFLIGHT] <= w);
    CHECK(g[G_CKPT_BUF_BYTES] <= envelope(w));
    ++rounds;
  }
  Engine::debug_ckpt_defer_writes(false);
  CHECK(rounds >= 4);
  finish_and_check(eng, bg, c, m);
  rm_rf(dir);
  std::printf("T4 out-of-order: %llu writes issued newest first while older slots held unissued buffers; bytes "
              "written == issued, part size exact, recovered exactly\n",
              (unsigned long long)rounds);
}

// ---- T5: final partial chunk ---------------------------------------------------------

void test_final_chunk() {
  const u32 w = 1;
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, w);
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 30000, 100);
  const Model m = model_of(ids, 100);
  Engine::debug_ckpt_hold_writes(true);
  Background bg;
  bg.start(eng);
  GaugeSnapshot g;
  for (;;) {
    g = wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_WINDOW_FULL] == 1; });
    if (g[G_CKPT_FINAL_PENDING] == 1) break;
    CHECK_EQ(g[G_CKPT_PHASE], kWalk);
    const u64 chunks = g[G_CKPT_CHUNKS];
    Engine::debug_ckpt_release_writes(1, false);
    wait_gauges(eng, [&](const GaugeSnapshot& x) { return x[G_CKPT_CHUNKS] > chunks; });
  }
  // Every ID visited, the last chunk unsubmitted: still WALK, IDs kept.
  CHECK_EQ(g[G_CKPT_PHASE], kWalk);
  CHECK_EQ(g[G_CKPT_WALK_DONE], g[G_CKPT_WALK_TOTAL]);
  const u64 chunks = g[G_CKPT_CHUNKS];
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  g = fresh(eng);
  CHECK_EQ(g[G_CKPT_PHASE], kWalk);
  CHECK_EQ(g[G_CKPT_FINAL_PENDING], 1u);
  CHECK_EQ(g[G_CKPT_CHUNKS], chunks);
  Engine::debug_ckpt_release_writes(1, false);
  g = wait_gauges(eng, [&](const GaugeSnapshot& x) { return x[G_CKPT_PHASE] != kWalk; });
  CHECK_EQ(g[G_CKPT_CHUNKS], chunks + 1);
  CHECK_EQ(g[G_CKPT_FINAL_PENDING], 0u);
  finish_and_check(eng, bg, c, m);
  rm_rf(dir);
  std::printf("T5 final chunk: WALK held with all %llu IDs visited until chunk %llu was submitted, then DRAIN\n",
              (unsigned long long)g[G_CKPT_WALK_TOTAL], (unsigned long long)(chunks + 1));
}

// ---- Forked crash children (T6, T7) ---------------------------------------------------

struct CrashFile {
  u64 seq_before = 0;
  u64 n = 0;
};

void write_model(const std::string& dir, u64 seq, const Model& m) {
  const std::string path = dir + "/model";
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
  seq = f.seq_before;
  return m;
}

// Common child prefix: 40,000 accounts (about 10 chunks), a committed checkpoint, then 300
// deposits after it (recovery replays them from the WAL).
struct ChildState {
  std::vector<ObjectID> ids;
  Model m;
  u64 seq = 0;
};

ChildState child_setup(Engine& eng) {
  ChildState s;
  s.ids = make_accounts(eng, 40000, 100);
  s.m = model_of(s.ids, 100);
  s.seq = eng.checkpoint().seq;
  for (std::size_t i = 0; i < 300; ++i) {
    deposit(eng, s.ids[(i * 7) % s.ids.size()], 5);
    add(s.m, (i * 7) % s.ids.size(), 5);
  }
  return s;
}

// After a crash child: the previous checkpoint is used and the state is
// exact; a new checkpoint then commits and recovers exactly.
void recover_after_crash(const std::string& dir, u32 w) {
  u64 seq = 0;
  const Model m = read_model(dir, seq);
  const Engine::Config c = reopen(config(dir, w));
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

void test_completion_errors() {
  for (int mode : {1, 2}) {
    const std::string dir = temp_dir();
    const pid_t pid = fork_child([&] {
      const std::string err = dir + "/stderr";
      const int fd = ::open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0 || ::dup2(fd, 2) < 0) std::_Exit(6);
      Engine eng(config(dir, 4));
      eng.start();
      ChildState s = child_setup(eng);
      write_model(dir, s.seq, s.m);
      Engine::debug_ckpt_fail_write(1, mode);  // the second chunk of the next checkpoint
      eng.checkpoint();                          // must not return
      std::_Exit(7);
    });
    int st = 0;
    CHECK_EQ(::waitpid(pid, &st, 0), pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
    std::ifstream in(dir + "/stderr");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find(mode == 1 ? "checkpoint short write" : "checkpoint write failed") != std::string::npos);
    recover_after_crash(dir, 4);
    rm_rf(dir);
  }
  std::printf("T6 exact completion: a one-byte-short write and an -EIO each aborted before the commit; "
              "recovered from the previous checkpoint exactly, then a new checkpoint committed\n");
}

enum class Pos { INFLIGHT, PARTIAL, ALL_COMPLETE, BEFORE_COMMIT };
const char* pos_name(Pos p) {
  switch (p) {
    case Pos::INFLIGHT: return "in-flight";
    case Pos::PARTIAL: return "partly-complete";
    case Pos::ALL_COMPLETE: return "all-complete";
    case Pos::BEFORE_COMMIT: return "synced-before-commit";
  }
  return "?";
}

void test_crash_positions() {
  std::string report;
  for (Pos p : {Pos::INFLIGHT, Pos::PARTIAL, Pos::ALL_COMPLETE, Pos::BEFORE_COMMIT}) {
    const std::string dir = temp_dir();
    const pid_t pid = fork_child([&] {
      Engine eng(config(dir, 4));
      eng.start();
      ChildState s = child_setup(eng);
      Background bg;
      if (p == Pos::INFLIGHT || p == Pos::PARTIAL) {
        Engine::debug_ckpt_hold_writes(true);
        bg.start(eng);
        wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_WINDOW_FULL] == 1; });
        if (p == Pos::PARTIAL) {
          Engine::debug_ckpt_release_writes(2, false);
          GaugeSnapshot g = wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_CHUNKS] == 6; });
          if (g[G_CKPT_BYTES_WRITTEN] == 0) std::_Exit(8);
        }
      } else if (p == Pos::ALL_COMPLETE) {
        Engine::debug_hold_wal(0, true);
        deposit(eng, s.ids[1], 1000, Durability::APPLIED);  // never durable: not in the model
        bg.start(eng);
        GaugeSnapshot g = wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_PHASE] == kDurableWait; });
        if (g[G_CKPT_WRITES_INFLIGHT] != 0 || g[G_CKPT_BYTES_WRITTEN] != g[G_CKPT_BYTES_ISSUED]) std::_Exit(9);
      } else {
        Engine::debug_ckpt_hold_commit(true);
        bg.start(eng);
        while (!Engine::debug_ckpt_commit_reached()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // Durable deposits while the checkpoint is held (the WAL carries them).
      if (p != Pos::ALL_COMPLETE) {
        for (std::size_t i = 0; i < 100; ++i) {
          deposit(eng, s.ids[(i * 11) % s.ids.size()], 2);
          add(s.m, (i * 11) % s.ids.size(), 2);
        }
      }
      write_model(dir, s.seq, s.m);
      std::fflush(nullptr);
      std::_Exit(0);  // the crash: nothing stopped, the checkpoint uncommitted
    });
    int st = 0;
    CHECK_EQ(::waitpid(pid, &st, 0), pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    recover_after_crash(dir, 4);
    rm_rf(dir);
    report += std::string(" ") + pos_name(p);
  }
  std::printf("T7 crash positions:%s; each recovered from the previous checkpoint exactly, then a new "
              "checkpoint committed and recovered\n",
              report.c_str());
}

// ---- T8, T9, T10: final sync, legacy, synchronous fallback ----------------------------

struct Run {
  GaugeSnapshot g;
  u64 seq = 0;
  u64 objects = 0;
};

// Creates n accounts, checkpoints, checks counts and the part's size, and
// recovers exactly.
Run checkpoint_run(const Engine::Config& c, u64 n, const std::function<void(Engine&)>& before = {}) {
  Run r;
  Model m;
  {
    Engine eng(c);
    eng.start();
    const std::vector<ObjectID> ids = make_accounts(eng, n, 100);
    m = model_of(ids, 100);
    if (before) before(eng);
    const CheckpointReport rep = eng.checkpoint();
    r.seq = rep.seq;
    r.objects = rep.objects;
    r.g = fresh(eng);
    CHECK_EQ(rep.objects, n);
    CHECK_EQ(r.g[G_CKPT_OBJECTS], n);
    CHECK_EQ(r.g[G_CKPT_BYTES_WRITTEN], r.g[G_CKPT_BYTES_ISSUED]);
    CHECK_EQ(file_size(part_file(c.data_dir, rep.seq)), r.g[G_CKPT_BYTES_ISSUED]);
    CHECK_EQ(r.g[G_CKPT_LAST_RESULT], 1u);
    eng.stop();
  }
  recover_exact(c, r.seq, m);
  return r;
}

bool io_uring_available() {
  io_uring ring{};
  if (io_uring_queue_init(8, &ring, 0) != 0) return false;
  io_uring_queue_exit(&ring);
  return true;
}

void test_final_sync() {
  const std::string dir = temp_dir();
  const Run r = checkpoint_run(config(dir, 4), 50000);
  CHECK_EQ(r.g[G_CKPT_IO_DSYNC], 1u);
  CHECK(r.g[G_CKPT_CHUNKS] >= 2);
  CHECK(r.g[G_CKPT_WRITES_INFLIGHT_PEAK] <= 4);
  CHECK_EQ(r.g[G_CKPT_WRITE_LAT_LT1MS] + r.g[G_CKPT_WRITE_LAT_LT10MS] + r.g[G_CKPT_WRITE_LAT_LT100MS] +
               r.g[G_CKPT_WRITE_LAT_GE100MS],
           r.g[G_CKPT_CHUNKS]);
  CHECK(r.g[G_CKPT_WRITE_LAT_MAX_US] >= r.g[G_CKPT_WRITE_LAT_LAST_US]);
  rm_rf(dir);
  std::printf("T8 final sync: dsync part of %llu chunks, %llu bytes = size on disk, final sync %llu us, chunk "
              "write max %llu us\n",
              (unsigned long long)r.g[G_CKPT_CHUNKS], (unsigned long long)r.g[G_CKPT_BYTES_ISSUED],
              (unsigned long long)r.g[G_CKPT_LAST_SYNC_US], (unsigned long long)r.g[G_CKPT_WRITE_LAT_MAX_US]);
}

void test_legacy() {
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, 4, /*dsync=*/false);
  Model m;
  u64 chunks = 0;
  {
    Engine eng(c);
    eng.start();
    const std::vector<ObjectID> ids = make_accounts(eng, 60000, 100);
    m = model_of(ids, 100);
    // No window: with every completion held, the walk still finishes and
    // every chunk is in flight at once (the write path before S3).
    Engine::debug_ckpt_hold_writes(true);
    Background bg;
    bg.start(eng);
    GaugeSnapshot g = wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_PHASE] == kDrain; });
    CHECK_EQ(g[G_CKPT_IO_DSYNC], 0u);
    CHECK_EQ(g[G_CKPT_WINDOW], 0u);
    CHECK_EQ(g[G_CKPT_WINDOW_FULL_STEPS], 0u);
    CHECK(g[G_CKPT_CHUNKS] > 4);
    CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT], g[G_CKPT_CHUNKS]);
    chunks = g[G_CKPT_CHUNKS];
    finish_and_check(eng, bg, c, m);
  }
  rm_rf(dir);
  std::printf("T9 legacy: no window, all %llu chunks in flight at once, then exact\n", (unsigned long long)chunks);
}

void test_sync_fallback() {
  const std::string dir = temp_dir();
  Engine::debug_force_sync_io(true);
  const Run forced = checkpoint_run(config(dir, 4), 30000);
  Engine::debug_force_sync_io(false);
  CHECK(forced.g[G_CKPT_CHUNKS] >= 2);
  CHECK_EQ(forced.g[G_CKPT_SYNC_WRITES], forced.g[G_CKPT_CHUNKS]);
  CHECK(forced.g[G_CKPT_SYNC_WRITES_TOTAL] >= forced.g[G_CKPT_CHUNKS]);
  rm_rf(dir);
  const bool uring = io_uring_available();
  u64 normal_sync = 0;
  if (uring) {
    const std::string d2 = temp_dir();
    const Run normal = checkpoint_run(config(d2, 4), 30000);
    normal_sync = normal.g[G_CKPT_SYNC_WRITES];
    CHECK_EQ(normal_sync, 0u);
    rm_rf(d2);
  }
  std::printf("T10 sync fallback: forced, %llu of %llu chunk writes counted synchronous and exact; %s\n",
              (unsigned long long)forced.g[G_CKPT_SYNC_WRITES], (unsigned long long)forced.g[G_CKPT_CHUNKS],
              uring ? "with io_uring, none" : "io_uring unavailable here: unforced check skipped");
}

// ---- T11: foreground progress ---------------------------------------------------------

void test_foreground() {
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, 2);
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 60000, 100);
  Model m = model_of(ids, 100);
  Engine::debug_ckpt_hold_writes(true);
  Background bg;
  bg.start(eng);
  const GaugeSnapshot g0 = wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_WINDOW_FULL] == 1; });
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < 500; ++i) {
    const std::size_t k = (i * 13) % ids.size();
    deposit(eng, ids[k], 1);
    add(m, k, 1);
    CHECK_EQ(balance_of(eng, ids[k]), m[k].balance);
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const GaugeSnapshot g1 = fresh(eng);
  CHECK_EQ(g1[G_CKPT_WINDOW_FULL], 1u);
  CHECK_EQ(g1[G_CKPT_WALK_DONE], g0[G_CKPT_WALK_DONE]);  // the walk stayed stopped
  CHECK_EQ(g1[G_CKPT_CHUNKS], g0[G_CKPT_CHUNKS]);
  finish_and_check(eng, bg, c, m);
  rm_rf(dir);
  std::printf("T11 foreground: 500 durable deposits and 500 reads in %.2f s with the window full and the walk "
              "stopped\n",
              s);
}

void test_config() {
  auto refused = [](u32 w) {
    const std::string dir = temp_dir();
    bool threw = false;
    try {
      Engine eng(config(dir, w));
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    rm_rf(dir);
    return threw;
  };
  CHECK(refused(0));
  CHECK(refused(65));
  CHECK(!refused(1));
  CHECK(!refused(64));
  std::printf("T13 config: window 0 and 65 refused, 1 and 64 accepted\n");
}

}  // namespace


// ---- T14-T18: lifecycle (S3.1; review PR #77 sections 9-13) ---------------------------

// Starts a checkpoint in `bg` with completions held, and waits until W
// chunk writes are in flight and the walk is stopped.
void start_held_checkpoint(Engine& eng, Background& bg) {
  Engine::debug_ckpt_hold_writes(true);
  bg.start(eng);
  wait_gauges(eng, [](const GaugeSnapshot& x) { return x[G_CKPT_WINDOW_FULL] == 1; });
}

// Polls `flag` for up to `ms`: true once set.
bool becomes(const std::atomic<bool>& flag, int ms) {
  for (int i = 0; i < ms && !flag.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return flag.load();
}

// While a stop (or destruction) waits on a checkpoint in progress, the
// shard is alive: its gauges keep being published, and nothing moved.
void check_still_held(Engine& eng, const std::atomic<bool>& ended, Background& bg, u64 phase, u32 inflight) {
  CHECK(!becomes(ended, 300));  // stop has not returned
  CHECK(!bg.done.load());       // nor has the checkpoint
  const GaugeSnapshot g = fresh(eng);  // the shard still publishes (not destroyed)
  CHECK_EQ(g[G_CKPT_PHASE], phase);
  CHECK_EQ(g[G_CKPT_WRITES_INFLIGHT], inflight);
}

// T14 + T17: stop() from another thread while chunk writes are in flight
// and a thread waits in Engine::checkpoint(). The contract is "the
// checkpoint finishes, then stop proceeds": stop waits, the shard stays
// alive, the waiter is not abandoned, and the checkpoint commits. A
// checkpoint asked for after stop starts nothing (seq 0).
void test_stop_with_writes() {
  const u32 w = 2;
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, w);
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 60000, 100);
  const Model m = model_of(ids, 100);
  Background bg;
  start_held_checkpoint(eng, bg);
  std::atomic<bool> stopped{false};
  std::thread st([&] {
    eng.stop();
    stopped = true;
  });
  check_still_held(eng, stopped, bg, kWalk, w);
  Engine::debug_ckpt_hold_writes(false);
  CHECK(becomes(bg.done, 60000));  // the waiter returns: not abandoned
  CHECK(becomes(stopped, 60000));
  bg.join();
  st.join();
  CHECK(bg.rep.seq > 0);
  CHECK_EQ(eng.checkpoint().seq, 0u);  // stopped: nothing started
  recover_exact(c, bg.rep.seq, m);     // it committed: recovery uses it
  rm_rf(dir);
  std::printf("T14+T17 stop with %u chunk writes in flight: stop waited, the shard stayed live, checkpoint %llu "
              "committed, then stop returned; checkpoint after stop started nothing\n",
              w, (unsigned long long)bg.rep.seq);
}

// T15: stop() while the final fdatasync is in flight (its completion held).
void test_stop_with_final_sync() {
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, 2);
  Engine eng(c);
  eng.start();
  const std::vector<ObjectID> ids = make_accounts(eng, 30000, 100);
  const Model m = model_of(ids, 100);
  Engine::debug_ckpt_hold_sync(true);
  Background bg;
  bg.start(eng);
  const u64 sync = static_cast<u64>(CkptPhase::SYNC);
  wait_gauges(eng, [&](const GaugeSnapshot& x) { return x[G_CKPT_PHASE] == sync; });
  std::atomic<bool> stopped{false};
  std::thread st([&] {
    eng.stop();
    stopped = true;
  });
  check_still_held(eng, stopped, bg, sync, 0);
  Engine::debug_ckpt_hold_sync(false);
  CHECK(becomes(bg.done, 60000));
  CHECK(becomes(stopped, 60000));
  bg.join();
  st.join();
  CHECK(bg.rep.seq > 0);
  recover_exact(c, bg.rep.seq, m);
  rm_rf(dir);
  std::printf("T15 stop with the final sync in flight: stop waited in SYNC, checkpoint %llu committed, clean stop\n",
              (unsigned long long)bg.rep.seq);
}

// T16: destruction through the supported lifecycle while a checkpoint is
// in progress on another thread: the owning thread calls stop() and then
// deletes the Engine. stop() waits for the checkpoint (whose last access
// to the Engine, the lifecycle unlock, happens before stop returns), so
// the destructor frees nothing the checkpoint or its in-flight writes
// use. Calling into an Engine whose destructor has begun is not supported
// (as for any C++ object), so ~Engine is reached only after stop().
void test_destroy_with_writes() {
  const u32 w = 2;
  const std::string dir = temp_dir();
  const Engine::Config c = config(dir, w);
  auto* eng = new Engine(c);
  eng->start();
  const std::vector<ObjectID> ids = make_accounts(*eng, 60000, 100);
  const Model m = model_of(ids, 100);
  Background bg;
  start_held_checkpoint(*eng, bg);
  std::atomic<bool> destroyed{false};
  std::thread d([&] {
    eng->stop();
    delete eng;
    destroyed = true;
  });
  // Not probed through the gauges, as T14 does: the owning thread may be
  // inside delete by the time a probe runs.
  CHECK(!becomes(destroyed, 300));
  CHECK(!bg.done.load());
  Engine::debug_ckpt_hold_writes(false);
  CHECK(becomes(bg.done, 60000));
  CHECK(becomes(destroyed, 60000));
  bg.join();
  d.join();
  CHECK(bg.rep.seq > 0);
  recover_exact(c, bg.rep.seq, m);
  rm_rf(dir);
  std::printf("T16 stop + delete with %u chunk writes in flight: both waited, checkpoint %llu committed, the Engine "
              "was destroyed after it\n",
              w, (unsigned long long)bg.rep.seq);
}

// T16b: a process that exits (std::exit, static destructors run) with an
// Engine still running and checkpoint writes in flight. Before S3.1 the
// engine's state was a static and was freed under the shard threads (the
// heap-use-after-free of PR #77's validation). Now the Engine owns it:
// the child exits with its own status and no sanitizer report. This
// proves only that the engine frees nothing of its own at exit; other
// static destructors still run under live threads, so exit is not a
// supported shutdown: stop() first (docs/S3_WRITEBACK.md section 10).
void test_exit_with_writes() {
  const std::string dir = temp_dir();
  const pid_t pid = fork_child([&] {
    const int fd = ::open((dir + "/stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ::dup2(fd, 2) < 0) std::_Exit(6);
    auto* eng = new Engine(config(dir, 2));  // never destroyed: still running at exit
    eng->start();
    make_accounts(*eng, 60000, 100);
    auto* bg = new Background;
    start_held_checkpoint(*eng, *bg);
    std::exit(7);
  });
  int st = 0;
  CHECK_EQ(::waitpid(pid, &st, 0), pid);
  std::ifstream in(dir + "/stderr");
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  if (!(WIFEXITED(st) && WEXITSTATUS(st) == 7)) std::fprintf(stderr, "T16b child stderr:\n%s\n", text.c_str());
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
  CHECK(text.find("Sanitizer") == std::string::npos);
  rm_rf(dir);
  std::printf("T16b exit with a running engine and writes in flight: exit status 7, no sanitizer report\n");
}

// T18: a storage failure while other chunk writes are outstanding. The
// failure is fatal (abort, no destructors), and nothing secondary (a
// use-after-free in teardown) hides it: the report names the failure and
// no sanitizer report follows; recovery uses the previous checkpoint.
void test_failure_with_writes() {
  const u32 w = 2;
  const std::string dir = temp_dir();
  const pid_t pid = fork_child([&] {
    const int fd = ::open((dir + "/stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ::dup2(fd, 2) < 0) std::_Exit(6);
    Engine eng(config(dir, w));
    eng.start();
    ChildState s = child_setup(eng);
    write_model(dir, s.seq, s.m);
    Engine::debug_ckpt_fail_write(0, 2);  // the first chunk: -EIO
    Background bg;
    start_held_checkpoint(eng, bg);  // W in flight, a third chunk blocked
    std::fprintf(stderr, "T18 child: %llu chunk writes in flight\n",
                 (unsigned long long)fresh(eng)[G_CKPT_WRITES_INFLIGHT]);
    Engine::debug_ckpt_hold_writes(false);  // chunk 0 fails with chunk 1 outstanding
    bg.join();                              // must not return
    std::_Exit(7);
  });
  int st = 0;
  CHECK_EQ(::waitpid(pid, &st, 0), pid);
  std::ifstream in(dir + "/stderr");
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
  CHECK(text.find("T18 child: 2 chunk writes in flight") != std::string::npos);
  CHECK(text.find("checkpoint write failed") != std::string::npos);
  CHECK(text.find("Sanitizer") == std::string::npos);
  CHECK(text.find("runtime error") == std::string::npos);
  recover_after_crash(dir, w);
  rm_rf(dir);
  std::printf("T18 failure with writes outstanding: -EIO on chunk 0 with chunk 1 in flight aborted with only that "
              "report; recovered from the previous checkpoint exactly\n");
}

int main(int argc, char** argv) {
  register_type();
  // Each test sets the write path it tests; the suite-wide override
  // (SNC_CKPT_IO) would replace it.
  if (std::getenv("SNC_CKPT_IO") != nullptr) {
    std::printf("test_s3_writeback: SNC_CKPT_IO ignored (each test sets its own write path)\n");
    ::unsetenv("SNC_CKPT_IO");
  }
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"T13", test_config},
      {"T6", test_completion_errors},  // forks: first, with no engine thread alive
      {"T7", test_crash_positions},
      {"T16b", test_exit_with_writes},  // forks
      {"T18", test_failure_with_writes},  // forks
      {"T1", test_window_bound},
      {"T2", test_memory_bound},
      {"T2b", test_memory_bound_max_objects},
      {"T4", test_out_of_order},
      {"T5", test_final_chunk},
      {"T8", test_final_sync},
      {"T9", test_legacy},
      {"T10", test_sync_fallback},
      {"T11", test_foreground},
      {"T14", test_stop_with_writes},
      {"T15", test_stop_with_final_sync},
      {"T16", test_destroy_with_writes},
  };
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  std::printf("test_s3_writeback: all passed\n");
  return 0;
}
