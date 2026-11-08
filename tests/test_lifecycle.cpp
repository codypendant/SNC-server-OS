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

}  // namespace

int main(int argc, char** argv) {
  register_type();
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"L1", l1_stats}, {"L2", l2_slot_report}, {"L3", l3_stage_chain}, {"L4", l4_replication},
      {"L5", l5_placement}, {"L7", l7_start_stop}, {"L8", l8_many}, {"L9", l9_failed_start},
      {"L10", l10_cross_engine}, {"L11", l11_aborts},
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
