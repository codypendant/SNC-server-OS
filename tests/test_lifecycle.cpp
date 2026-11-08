// S3.2 Engine lifecycle gate (docs/ENGINE_LIFECYCLE.md; work order
// docs/handoff/PR78_S31_Review.txt sections 10-14).
//
// An operation that has entered (holds its permit) is parked with
// Engine::debug_park_ops before its shard work, so stop() is called while
// the operation is known to be inside. The contract: stop() moves to
// STOPPING, admits nothing new, waits for the operation, which completes
// on live shards, and only then stops the runtime.
//
//   L1  stats (one shard task + latch) vs stop
//   L2  slot_report (on_shard + latch) vs stop
//   L3  a final stage chain (on_shard_steps, held between units) vs stop
//   L4  replication_status (a task on every shard) vs stop
//   L5  update_placement (run_on_all, twice) vs stop
//   L6  new operations once STOPPING has begun, and once STOPPED: each is
//       refused at once with its defined result and submits nothing
//   L7  start vs stop: stop waits for a start in progress; restart after
//       stop is refused; stop on a never-started engine is a no-op
//   L8  many threads issuing operations while another stops (TSan): every
//       thread returns, nothing hangs
//   L9  a failed start (before and after the runtime came up): FAILED,
//       every operation refused, restart refused, stop tears down
//   L10 nesting across engines (A -> B -> A on one thread while A is
//       STOPPING): the inner A call is nested, not counted or refused
//   L11 stop() from inside an operation, and from a shard thread: aborts
//       with a message instead of waiting for itself
//
// S3.3 (docs/handoff/PR79_S32_Review.txt sections 11-13):
//   R1  a data-plane lease vs stop: stop waits for the lease; work submitted
//       through it while STOPPING still runs on a live worker
//   R2  a new lease during STOPPING: refused at once
//   R3  after STOPPED: refused; using a refused lease aborts
//   C1  promote vs checkpoint overlap (run under TSan in the serial suite)
//   C2  the two linearize either way round, and the superblock after both
//       holds the promoted term, the checkpoint, the lease block and a
//       generation of exactly +2; recovery succeeds
//   C3  the checkpoint's role view is the role before or after the
//       promotion, matching the order
//   C4  a crash with one of the two held and the other waiting recovers to
//       one serial prefix
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "harness.h"
#include "snc/cluster.h"
#include "snc/slot_map.h"
#include "snc/snc.h"
#include "snc/types/account.h"
#include "snc/storage.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

std::vector<std::string> made_dirs;

std::string fresh_dir() {
  char tmpl[] = "/tmp/snc-life-XXXXXX";
  CHECK(::mkdtemp(tmpl) != nullptr);
  made_dirs.push_back(tmpl);
  return tmpl;
}

// Two groups, this node group 0, so slot moves and placement updates run.
// `port` offsets the node-link ports (a second engine in this process).
Engine::Config config(const std::string& dir, u16 port = 1) {
  Engine::Config c;
  c.runtime.n_shards = 2;
  c.runtime.pin = false;
  c.expected_objects = 4096;
  c.data_dir = dir;
  c.init = true;
  c.move_units = true;
  c.move_unit_us = 1;
  c.move_unit_bytes = 1;
  c.move_window_bytes = 8u << 20;
  for (u16 g = 0; g < 2; ++g) {
    ClusterGroup grp;
    grp.n_shards = 2;
    grp.members.push_back({"127.0.0.1", static_cast<u16>(port + g), static_cast<u16>(port + 2 + g)});
    c.cluster.push_back(grp);
  }
  c.group_id = 0;
  return c;
}

bool becomes(const std::function<bool()>& p, int ms) {
  for (int i = 0; i < ms; ++i) {
    if (p()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return p();
}

u16 slot_of_group(Engine& eng, u8 group) {
  for (u16 s = 0; s < kSlots; ++s) {
    if (eng.cluster().group_of[s] == group) return s;
  }
  CHECK(false);
  return 0;
}

std::vector<ObjectID> ids_in(u16 slot, std::size_t n) {
  std::vector<ObjectID> v;
  for (u64 seq = 1; v.size() < n; ++seq) {
    const ObjectID id = ObjectID::make(0x7171, seq);
    if (slot_of(id) == slot) v.push_back(id);
  }
  return v;
}

SlotObject account_obj(const ObjectID& id) {
  AccountV1 a{};
  a.balance_cents = 1000;
  a.created_ns = 1;
  a.email_len = 1;
  a.email[0] = 'e';
  a.name_len = 1;
  a.name[0] = 'n';
  SlotObject o;
  o.id = id;
  o.version = 1;
  o.type_id = kTypeId;
  o.schema_ver = registry().type(kTypeId)->schema_ver;
  o.has_payload = true;
  o.payload.resize(sizeof(a));
  std::memcpy(o.payload.data(), &a, sizeof(a));
  u8 head[6];
  std::memcpy(head, &o.type_id, 4);
  std::memcpy(head + 4, &o.schema_ver, 2);
  o.crc = crc32c(o.payload.data(), o.payload.size(), crc32c(head, sizeof(head)));
  return o;
}

Status stage(Engine& eng, u16 slot, const std::vector<SlotObject>& objs) {
  u64 count = 0, digest = 0;
  for (const SlotObject& o : objs) {
    ++count;
    digest += slot_object_digest(o.id, o.version, o.crc);
  }
  SlotReport r;
  return eng.stage_slot(slot, 0x51, 1, eng.cluster().epoch + 1, true, objs, count, digest, r, 30);
}

// Every refusal a stopping or stopped engine gives: at once, with its
// defined "not running" result, and without touching a shard.
void check_refused(Engine& eng, u16 home, u16 foreign) {
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_EQ(eng.checkpoint().seq, 0u);
  CHECK(!eng.slot_report(home).serving);
  CHECK(eng.slot_states().empty());
  CHECK(!eng.wait_emits(5));
  CHECK_EQ(eng.replication_status().live, 0u);
  std::array<u8, kSlots> g = eng.cluster().group_of;
  CHECK(!eng.update_placement(g, eng.cluster().epoch + 7, nullptr));
  std::vector<SlotObject> none;
  CHECK_EQ(eng.slot_objects(home, none), Status::E_SHUTTING_DOWN);
  CHECK_EQ(stage(eng, foreign, none), Status::E_SHUTTING_DOWN);
  SlotReport r;
  CHECK_EQ(eng.seal_slot(home, 1, 1, 1, r, 5), Status::E_SHUTTING_DOWN);
  CHECK_EQ(eng.unseal_slot(home, 1, r, 5), Status::E_SHUTTING_DOWN);
  u64 c = 0, d = 0;
  CHECK_EQ(eng.export_slot(home, 1, false, none, none, c, d), Status::E_SHUTTING_DOWN);
  bool visited = false;
  eng.for_each_object(0, [&](const ObjectHeader&) { visited = true; });
  CHECK(!visited);
  eng.release_emits();  // a no-op
  (void)eng.stats(0);   // final values, never blocks
  bool threw = false;
  try {
    eng.promote();
  } catch (const std::logic_error&) {
    threw = true;
  }
  CHECK(threw);
  // All of it without waiting (a wait_emits that entered would poll for 5 s).
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

// The common shape of L1, L2, L4, L5: `op` enters and parks; stop() is
// called on another thread; while stop waits, new operations are refused
// (L6); released, the operation completes on live shards (`ok` checks its
// result), then stop returns.
void op_vs_stop(const char* name, const std::function<bool(Engine&)>& op) {
  const std::string dir = fresh_dir();
  Engine eng(config(dir));
  eng.start();
  const u16 home = slot_of_group(eng, 0), foreign = slot_of_group(eng, 1);
  Engine::debug_park_ops(true);
  std::atomic<bool> op_done{false}, op_ok{false}, stopped{false};
  std::thread a([&] {
    op_ok = op(eng);
    op_done = true;
  });
  CHECK(becomes([] { return Engine::debug_ops_parked() == 1; }, 10000));  // inside, permit held
  std::thread b([&] {
    eng.stop();
    stopped = true;
  });
  CHECK(becomes([&] { return eng.lifecycle() == Lifecycle::STOPPING; }, 10000));
  CHECK(!becomes([&] { return stopped.load(); }, 300));  // stop waits for the operation
  CHECK(!op_done.load());
  check_refused(eng, home, foreign);  // L6: STOPPING admits nothing new
  Engine::debug_park_ops(false);
  CHECK(becomes([&] { return op_done.load(); }, 60000));  // not abandoned
  CHECK(op_ok.load());                                     // it really ran on the shards
  CHECK(becomes([&] { return stopped.load(); }, 60000));
  a.join();
  b.join();
  CHECK(eng.lifecycle() == Lifecycle::STOPPED);
  check_refused(eng, home, foreign);  // L6: STOPPED admits nothing either
  std::printf("%s vs stop: the operation entered, stop waited in STOPPING (new operations refused), the "
              "operation completed, then stop returned\n",
              name);
}

void l1_stats() {
  // Entered while RUNNING: it reads the live shard (a task and a latch),
  // which only a running worker can answer.
  op_vs_stop("L1 stats", [](Engine& eng) {
    (void)eng.stats(0);
    return true;
  });
}

void l2_slot_report() {
  op_vs_stop("L2 slot_report", [](Engine& eng) { return eng.slot_report(slot_of_group(eng, 0)).serving; });
}

void l4_replication() {
  op_vs_stop("L4 replication_status", [](Engine& eng) { return eng.replication_status().shards == 2; });
}

void l5_placement() {
  op_vs_stop("L5 update_placement", [](Engine& eng) {
    std::array<u8, kSlots> g = eng.cluster().group_of;
    const u64 epoch = eng.cluster().epoch + 1;
    return eng.update_placement(g, epoch, nullptr) && eng.cluster().epoch == epoch;
  });
}

// L3: a final stage chain held between its units (debug_stage_stop: the
// chain sleeps on its control thread between two shard tasks); stop()
// waits for the whole chain, which completes and answers OK.
void l3_stage_chain() {
  const std::string dir = fresh_dir();
  Engine eng(config(dir));
  eng.start();
  const u16 foreign = slot_of_group(eng, 1);
  std::vector<SlotObject> v;
  for (const ObjectID& id : ids_in(foreign, 300)) v.push_back(account_obj(id));
  eng.debug_stage_stop(2, 2, false, 3000);  // before put unit 2
  std::atomic<int> st{-1};
  std::atomic<bool> stopped{false};
  std::thread a([&] { st = static_cast<int>(stage(eng, foreign, v)); });
  // The hold has begun: the chain is between units (it sleeps 3 s there).
  CHECK(becomes([&] { return !eng.debug_stage_pending(); }, 60000));
  std::thread b([&] {
    eng.stop();
    stopped = true;
  });
  CHECK(becomes([&] { return eng.lifecycle() == Lifecycle::STOPPING; }, 10000));
  CHECK(!becomes([&] { return stopped.load(); }, 300));
  CHECK_EQ(st.load(), -1);  // the chain is still between units
  CHECK(becomes([&] { return st.load() != -1; }, 60000));
  CHECK_EQ(static_cast<Status>(st.load()), Status::OK);  // the chain completed, not stranded
  CHECK(becomes([&] { return stopped.load(); }, 60000));
  a.join();
  b.join();
  // Reopened, the staged objects are there: the chain finished its writes
  // before the shards stopped.
  Engine::Config c = config(dir);
  c.init = false;
  Engine again(c);
  again.start();
  std::vector<SlotObject> held;
  CHECK_EQ(again.slot_objects(foreign, held), Status::OK);
  CHECK_EQ(held.size(), v.size());
  again.stop();
  std::printf("L3 stage chain vs stop: stop waited while the chain was held between units; the chain "
              "completed (OK, %zu objects durable), then stop returned\n",
              held.size());
}

// L7: start vs stop, and the one-way lifecycle.
void l7_start_stop() {
  // A second start() while one is in progress returns once RUNNING.
  {
    const std::string dir = fresh_dir();
    Engine eng(config(dir));
    eng.stop();  // never started: a no-op
    CHECK(eng.lifecycle() == Lifecycle::CONSTRUCTED);
    Engine::debug_park_start(true);
    std::atomic<bool> started{false}, again_done{false};
    std::thread a([&] {
      eng.start();
      started = true;
    });
    CHECK(becomes([] { return Engine::debug_start_parked(); }, 10000));
    CHECK(eng.lifecycle() == Lifecycle::STARTING);
    std::thread c([&] {
      eng.start();
      again_done = true;
    });
    CHECK(!becomes([&] { return again_done.load(); }, 300));  // waits for the start in progress
    CHECK_EQ(eng.checkpoint().seq, 0u);  // STARTING admits no operation from another thread
    Engine::debug_park_start(false);
    CHECK(becomes([&] { return started.load() && again_done.load(); }, 60000));
    a.join();
    c.join();
    CHECK(eng.lifecycle() == Lifecycle::RUNNING);
    eng.stop();
  }
  // stop() during a start waits for it, then stops; restart is refused.
  {
    const std::string dir = fresh_dir();
    Engine eng(config(dir));
    Engine::debug_park_start(true);
    std::atomic<bool> started{false}, stopped{false};
    std::thread a([&] {
      eng.start();
      started = true;
    });
    CHECK(becomes([] { return Engine::debug_start_parked(); }, 10000));
    std::thread b([&] {
      eng.stop();
      stopped = true;
    });
    CHECK(!becomes([&] { return stopped.load(); }, 300));  // stop waits for the start
    CHECK(eng.lifecycle() == Lifecycle::STARTING);
    Engine::debug_park_start(false);
    CHECK(becomes([&] { return started.load() && stopped.load(); }, 60000));
    a.join();
    b.join();
    CHECK(eng.lifecycle() == Lifecycle::STOPPED);
    bool threw = false;
    try {
      eng.start();
    } catch (const std::logic_error&) {
      threw = true;
    }
    CHECK(threw);  // restart is refused
    eng.stop();    // a second stop: returns
    CHECK(eng.lifecycle() == Lifecycle::STOPPED);
  }
  std::printf("L7 start vs stop: a second start waited for the one in progress; stop waited for a start, "
              "then stopped; restart after stop refused; stop before start a no-op\n");
}

// L8: many threads issuing operations while another stops. Every thread
// returns; under TSan, no race.
void l8_many() {
  const std::string dir = fresh_dir();
  Engine eng(config(dir));
  eng.start();
  const u16 home = slot_of_group(eng, 0);
  std::atomic<bool> go{true};
  std::atomic<u64> calls{0}, late{0};
  std::atomic<int> finished{0};
  std::vector<std::thread> ts;
  for (int k = 0; k < 6; ++k) {
    ts.emplace_back([&, k] {
      while (go.load()) {
        bool ok = true;
        switch (k) {
          case 0: (void)eng.stats(0); ok = eng.lifecycle() == Lifecycle::RUNNING; break;
          case 1: ok = eng.slot_report(home).serving; break;
          case 2: ok = eng.replication_status().shards == 2 && eng.lifecycle() == Lifecycle::RUNNING; break;
          case 3: ok = !eng.slot_states().empty(); break;
          case 4: ok = eng.wait_emits(0.01); break;
          case 5: ok = eng.checkpoint().seq != 0; break;
        }
        (void)ok;  // results vary (wait_emits can time out); what matters is that each call returns
        ++calls;
        const Lifecycle l = eng.lifecycle();
        if (l != Lifecycle::RUNNING) ++late;  // ran into STOPPING or STOPPED
        if (l == Lifecycle::STOPPED) break;
      }
      ++finished;
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::atomic<bool> stopped{false};
  std::thread s([&] {
    eng.stop();
    stopped = true;
  });
  CHECK(becomes([&] { return stopped.load(); }, 120000));
  s.join();
  go = false;
  CHECK(becomes([&] { return finished.load() == 6; }, 60000));  // every thread returned: no abandoned waiter
  for (auto& t : ts) t.join();
  CHECK(calls.load() > 6);
  CHECK(late.load() >= 6);  // every thread made a call once stop had begun
  std::printf("L8 %llu operations on 6 threads across a stop: every thread returned (%llu calls after stop began)\n",
              (unsigned long long)calls.load(), (unsigned long long)late.load());
}

// L9: a failed start. Before the runtime came up (an empty directory
// without init) and after it (the seed port taken: recovery has run on
// live shards when the listen fails). Either way FAILED: no operation
// enters, start() is refused, stop() tears down and ends STOPPED.
void l9_failed_start() {
  // The seed port, held by a listening socket of this process.
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = 0;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
  CHECK_EQ(::listen(fd, 1), 0);
  socklen_t len = sizeof(a);
  CHECK_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len), 0);
  const u16 port = ntohs(a.sin_port);
  for (int k = 0; k < 2; ++k) {
    const bool after_runtime = k == 1;
    const std::string dir = fresh_dir();
    Engine::Config c = config(dir);
    if (after_runtime) {
      c.seed_port = port;
    } else {
      c.init = false;  // an empty directory without init: refused before the runtime starts
    }
    Engine eng(c);
    bool threw = false;
    try {
      eng.start();
    } catch (const std::logic_error&) {
      CHECK(false);  // a start failure, not a lifecycle refusal
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
    CHECK(eng.lifecycle() == Lifecycle::FAILED);
    const u16 home = slot_of_group(eng, 0), foreign = slot_of_group(eng, 1);
    check_refused(eng, home, foreign);  // a half-started engine admits nothing
    CHECK_EQ(eng.stats(0).objects, 0u);
    bool refused = false;
    try {
      eng.start();
    } catch (const std::logic_error&) {
      refused = true;
    }
    CHECK(refused);  // no second start on state the first one changed
    eng.stop();
    CHECK(eng.lifecycle() == Lifecycle::STOPPED);
    check_refused(eng, home, foreign);
  }
  ::close(fd);
  std::printf("L9 failed start (before and after the runtime came up): FAILED, every operation refused, "
              "restart refused, stop tore down to STOPPED\n");
}

// L10: an operation of engine A calls into engine B, which calls back into
// A, on one thread, while A is STOPPING. The inner A call is nested in the
// outer A permit (granted, not counted, not refused); stop waits for the
// outer one only.
Engine* g_a = nullptr;
Engine* g_b = nullptr;
std::atomic<int> g_l10_step{0};
std::atomic<int> g_l10_inner{-1};
void l10_hook(const Engine& e) {
  if (&e == g_a && g_l10_step.load() == 0) {  // the outer A operation has entered
    g_l10_step = 1;
    while (g_a->lifecycle() != Lifecycle::STOPPING) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    (void)g_b->slot_report(slot_of_group(*g_b, 0));  // into B (a counted B permit: the hook runs again)
  } else if (&e == g_b && g_l10_step.load() == 1) {  // inside B, inside A
    g_l10_step = 2;
    g_l10_inner = g_a->slot_report(slot_of_group(*g_a, 0)).serving ? 1 : 0;  // back into A
  }
}
void l10_cross_engine() {
  const std::string da = fresh_dir(), db = fresh_dir();
  Engine a(config(da)), b(config(db, 11));
  a.start();
  b.start();
  g_a = &a;
  g_b = &b;
  Engine::debug_op_hook(l10_hook);
  std::atomic<bool> outer_done{false}, stopped{false};
  std::atomic<bool> outer_ok{false};
  std::thread t([&] {
    outer_ok = a.slot_report(slot_of_group(a, 0)).serving;
    outer_done = true;
  });
  CHECK(becomes([] { return g_l10_step.load() >= 1; }, 10000));
  std::thread s([&] {
    a.stop();
    stopped = true;
  });
  CHECK(becomes([&] { return outer_done.load(); }, 60000));
  CHECK(becomes([&] { return stopped.load(); }, 60000));
  t.join();
  s.join();
  Engine::debug_op_hook(nullptr);
  CHECK_EQ(g_l10_step.load(), 2);
  CHECK_EQ(g_l10_inner.load(), 1);  // nested: granted, ran on A's live shard
  CHECK(outer_ok.load());
  CHECK(a.lifecycle() == Lifecycle::STOPPED);
  CHECK(b.lifecycle() == Lifecycle::RUNNING);
  b.stop();
  std::printf("L10 A -> B -> A on one thread while A was STOPPING: the inner A call was nested (granted, "
              "live result); stop waited for the outer operation\n");
}

// L11: stop() where it would wait for itself aborts with a message. Each
// case runs in a child process (its directory made by the parent, which
// removes it); the parent checks for SIGABRT.
std::string g_l11_dir;
void l11_child_stop_in_op() {
  Engine eng(config(g_l11_dir));
  eng.start();
  g_a = &eng;
  Engine::debug_op_hook([](const Engine&) { g_a->stop(); });
  (void)eng.stats(0);
}
void l11_child_stop_on_shard() {
  Engine eng(config(g_l11_dir));
  eng.start();
  g_a = &eng;
  // One object, so the for_each callback (an application hook on a shard
  // thread) runs on its shard.
  const u16 foreign = slot_of_group(eng, 1);
  std::vector<SlotObject> v;
  for (const ObjectID& id : ids_in(foreign, 1)) v.push_back(account_obj(id));
  CHECK_EQ(stage(eng, foreign, v), Status::OK);
  for (u16 s = 0; s < 2; ++s) eng.for_each_object(s, [](const ObjectHeader&) { g_a->stop(); });
}
void l11_aborts() {
  struct Case {
    const char* what;
    void (*child)();
    const char* message;  // stop()'s own abort, not a failed CHECK in the child
  };
  const Case cases[] = {
      {"inside an operation", l11_child_stop_in_op, "Engine::stop() called from inside an Engine operation"},
      {"on a shard thread", l11_child_stop_on_shard, "Engine::stop() called from a shard thread"}};
  for (const auto& [what, child, message] : cases) {
    g_l11_dir = fresh_dir();
    std::fflush(stdout);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    const std::string err = g_l11_dir + "/stderr";
    if (pid == 0) {
      const int efd = ::open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (efd < 0 || ::dup2(efd, 2) < 0) std::_Exit(3);
      ::alarm(120);  // a stop() that waits for itself instead: SIGALRM, not SIGABRT
      child();
      std::_Exit(0);  // not reached: stop() aborted
    }
    int ws = 0;
    CHECK_EQ(::waitpid(pid, &ws, 0), pid);
    CHECK(WIFSIGNALED(ws));
    CHECK_EQ(WTERMSIG(ws), SIGABRT);
    std::string text;
    if (FILE* f = std::fopen(err.c_str(), "r")) {
      char buf[4096];
      std::size_t k;
      while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, k);
      std::fclose(f);
    }
    CHECK(text.find(message) != std::string::npos);
    std::printf("L11 stop() %s: aborted with a message (SIGABRT)\n", what);
  }
}

// ---- S3.3: the data-plane lease -------------------------------------------------------

void r_lease() {
  const std::string dir = fresh_dir();
  Engine eng(config(dir));
  eng.start();
  Engine::DataPlane dp = eng.data_plane();
  CHECK(dp);
  std::atomic<int> ran{0};
  dp.submit(0, [&] { ++ran; });
  CHECK(becomes([&] { return ran.load() == 1; }, 10000));
  std::atomic<bool> stopped{false};
  std::thread b([&] {
    eng.stop();
    stopped = true;
  });
  // R1: stop waits for the lease.
  CHECK(becomes([&] { return eng.lifecycle() == Lifecycle::STOPPING; }, 10000));
  CHECK(!becomes([&] { return stopped.load(); }, 300));
  // R2: no new lease once STOPPING.
  CHECK(!eng.data_plane());
  // Work submitted through the live lease still reaches a live worker,
  // and a wait on it finishes (no abandoned waiter).
  std::latch l(1);
  dp.submit(1, [&] {
    ++ran;
    l.count_down();
  });
  l.wait();
  std::atomic<int> all{0};
  dp.run_on_all([&](u16) { ++all; });
  CHECK_EQ(all.load(), 2);
  CHECK(!stopped.load());
  dp.release();
  CHECK(!dp);
  CHECK(becomes([&] { return stopped.load(); }, 60000));
  b.join();
  // R3: after STOPPED, no lease.
  CHECK(eng.lifecycle() == Lifecycle::STOPPED);
  CHECK(!eng.data_plane());
  CHECK_EQ(ran.load(), 2);
  // A moved lease is counted once (moved-from is empty).
  {
    const std::string d2 = fresh_dir();
    Engine e2(config(d2, 11));
    e2.start();
    Engine::DataPlane x = e2.data_plane();
    Engine::DataPlane y = std::move(x);
    CHECK(!x);
    CHECK(y);
    Engine::DataPlane z = e2.data_plane();
    z = std::move(y);  // releases z's own lease, takes y's
    CHECK(z && !y);
    z.release();
    e2.stop();  // returns: no lease left (a miscount would hang here)
    CHECK(e2.lifecycle() == Lifecycle::STOPPED);
  }
  // R3: a refused lease aborts if used (in a child process).
  {
    std::fflush(stdout);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      ::alarm(60);
      const int nul = ::open("/dev/null", O_WRONLY);
      if (nul >= 0) ::dup2(nul, 2);
      eng.data_plane().submit(0, [] {});
      std::_Exit(0);
    }
    int ws = 0;
    CHECK_EQ(::waitpid(pid, &ws, 0), pid);
    CHECK(WIFSIGNALED(ws));
    CHECK_EQ(WTERMSIG(ws), SIGABRT);
  }
  std::printf("R1-R3 data-plane lease: stop waited in STOPPING for the lease (its work still ran, its waits "
              "finished); new leases refused in STOPPING and STOPPED; a refused lease aborts when used\n");
}

// ---- S3.3: promote vs checkpoint ------------------------------------------------------

u16 standby_port() { return static_cast<u16>(26000 + (::getpid() % 500) * 8); }

// A lone standby (no primary connects: promotion settles at once), with a
// lease block from a fake control plane at promotion.
Engine::Config standby_config(const std::string& dir, bool standby) {
  Engine::Config c;
  c.runtime.n_shards = 2;
  c.runtime.pin = false;
  c.expected_objects = 4096;
  c.data_dir = dir;
  c.init = true;
  c.standby = standby;
  c.standby_host = "127.0.0.1";
  c.replica_port = standby_port();
  c.lease_blocks = [](u64, u64& lo, u64& hi) {
    lo = 5000;
    hi = 9000;
    return true;
  };
  return c;
}

Superblock disk_sb(const std::string& dir) {
  std::optional<Superblock> sb = read_superblock(dir);
  CHECK(sb.has_value());
  return *sb;
}

// One overlap of checkpoint() and promote(), with the first of them held
// at its durable point while the second is called. Order A: the
// checkpoint is held after its parts are written (before manifest and
// superblock); order B: the promotion is held after its superblock write.
// The second must not run until the first is released.
void c_overlap(bool checkpoint_first) {
  const std::string dir = fresh_dir();
  Superblock before{};
  {
    Engine eng(standby_config(dir, true));
    eng.start();
    before = disk_sb(dir);
    CHECK_EQ(before.term, 0u);
    CHECK_EQ(eng.term(), 0u);
    std::atomic<bool> ck_done{false}, pr_done{false};
    CheckpointReport rep;
    std::thread ck, pr;
    auto run_ck = [&] {
      rep = eng.checkpoint();
      ck_done = true;
    };
    auto run_pr = [&] {
      (void)eng.promote();
      pr_done = true;
    };
    if (checkpoint_first) {
      Engine::debug_ckpt_hold_commit(true);
      ck = std::thread(run_ck);
      CHECK(becomes([] { return Engine::debug_ckpt_commit_reached(); }, 30000));
      pr = std::thread(run_pr);
      CHECK(!becomes([&] { return pr_done.load(); }, 300));  // waits for the whole checkpoint
      CHECK_EQ(eng.term(), 0u);                               // nothing of the promotion yet
      CHECK_EQ(disk_sb(dir).term, 0u);
      CHECK_EQ(disk_sb(dir).generation, before.generation);
      Engine::debug_ckpt_hold_commit(false);
    } else {
      Engine::debug_park_promote(true);
      pr = std::thread(run_pr);
      CHECK(becomes([] { return Engine::debug_promote_parked(); }, 30000));
      ck = std::thread(run_ck);
      CHECK(!becomes([&] { return ck_done.load(); }, 300));  // waits for the whole promotion
      CHECK_EQ(disk_sb(dir).term, 1u);                        // the promotion's superblock is written
      CHECK_EQ(disk_sb(dir).active_ckpt, before.active_ckpt);  // the checkpoint has not committed
      Engine::debug_park_promote(false);
    }
    CHECK(becomes([&] { return ck_done.load() && pr_done.load(); }, 60000));
    ck.join();
    pr.join();
    // C3: the role view matches the order.
    CHECK(rep.seq != 0);
    CHECK_EQ(rep.standby, checkpoint_first);
    // C2: both updates in the superblock, generation exactly +2.
    const Superblock after = disk_sb(dir);
    CHECK_EQ(after.term, 1u);
    CHECK_EQ(after.active_ckpt, rep.seq);
    CHECK_EQ(after.generation, before.generation + 2);
    CHECK_EQ(after.lease_lo, 5000u);
    CHECK_EQ(after.lease_hi, 9000u);
    CHECK(after.next_lease >= 5000 && after.next_lease < 9000);
    CHECK_EQ(eng.term(), 1u);
    CHECK(eng.lease_block() == std::make_pair(u64{5000}, u64{9000}));
    eng.stop();
  }
  // Recovery succeeds on the result, as the primary it now is.
  Engine::Config c = standby_config(dir, false);
  c.init = false;
  Engine again(c);
  again.start();
  CHECK_EQ(again.term(), 1u);
  CHECK(again.checkpoint().seq != 0);
  again.stop();
  std::printf("C1-C3 %s: the second waited for the whole first; superblock after both: term 1, checkpoint %s, "
              "lease block kept, generation +2; checkpoint role %s; recovery succeeded\n",
              checkpoint_first ? "checkpoint then promote" : "promote then checkpoint", "committed",
              checkpoint_first ? "standby" : "primary");
}

void c1_c3() {
  c_overlap(true);
  c_overlap(false);
}

// C4: a crash (the child exits) with the first held and the second
// waiting. The superblock is one serial prefix: nothing of the second.
void c4_crash() {
  for (int k = 0; k < 2; ++k) {
    const bool checkpoint_first = k == 0;
    const std::string dir = fresh_dir();
    std::fflush(stdout);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      ::alarm(120);
      Engine eng(standby_config(dir, true));
      eng.start();
      std::thread ck, pr;
      if (checkpoint_first) {
        Engine::debug_ckpt_hold_commit(true);
        ck = std::thread([&] { (void)eng.checkpoint(); });
        while (!Engine::debug_ckpt_commit_reached()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pr = std::thread([&] { (void)eng.promote(); });
      } else {
        Engine::debug_park_promote(true);
        pr = std::thread([&] { (void)eng.promote(); });
        while (!Engine::debug_promote_parked()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ck = std::thread([&] { (void)eng.checkpoint(); });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the second is waiting
      std::_Exit(0);                                                // the crash
    }
    int ws = 0;
    CHECK_EQ(::waitpid(pid, &ws, 0), pid);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0);
    const Superblock sb = disk_sb(dir);
    // The fresh space's recovery checkpoint is seq 1 (generation 2 after
    // init and the lease write); the held checkpoint would be seq 2.
    if (checkpoint_first) {
      CHECK_EQ(sb.term, 0u);  // neither the held checkpoint nor the promotion
      CHECK_EQ(sb.active_ckpt, 1u);
    } else {
      CHECK_EQ(sb.term, 1u);  // the promotion only
      CHECK_EQ(sb.active_ckpt, 1u);
      CHECK_EQ(sb.lease_lo, 5000u);
    }
    // Recovery succeeds (as the standby the directory still is: the role
    // file is written once the promotion serves).
    Engine::Config c = standby_config(dir, true);
    c.init = false;
    Engine again(c);
    again.start();
    CHECK_EQ(again.term(), sb.term);
    again.stop();
    std::printf("C4 crash with %s held and the other waiting: superblock term %llu, checkpoint %llu (one "
                "serial prefix); recovery succeeded\n",
                checkpoint_first ? "the checkpoint" : "the promotion", (unsigned long long)sb.term,
                (unsigned long long)sb.active_ckpt);
  }
}

}  // namespace

int main(int argc, char** argv) {
  register_type();
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"L1", l1_stats}, {"L2", l2_slot_report}, {"L3", l3_stage_chain}, {"L4", l4_replication},
      {"L5", l5_placement}, {"L7", l7_start_stop}, {"L8", l8_many}, {"L9", l9_failed_start},
      {"L10", l10_cross_engine}, {"L11", l11_aborts}, {"R", r_lease}, {"C1", c1_c3}, {"C4", c4_crash},
  };
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  for (const std::string& d : made_dirs) CHECK_EQ(std::system(("rm -rf " + d).c_str()), 0);
  std::printf("test_lifecycle: all passed\n");
  return 0;
}
