// F1 regression (docs/F1_Q10_LIVENESS_ROOT_CAUSE.md): once an in-flight file
// I/O op has been in flight for io_block_after_us, a shard loop that always
// has work must give its core up by blocking, so the kernel threads that
// complete that I/O on the core can run. sched_yield does not do that: the
// thread stays runnable, and on the hosted 7.0 kernels the core's ksoftirqd
// and dio kworker were starved for minutes.
//
// The check does not depend on the scheduler's choices. A blocking pause is
// a voluntary context switch (voluntary_ctxt_switches in /proc); sched_yield
// never is, whatever else is runnable. So with a write held in flight and the
// loop kept busy, the shard thread's voluntary switches must keep rising.
//
// A write to a full pipe stays in flight until the pipe is drained, so the
// in-flight state is held for as long as the test needs, with no disk.
//
// Phases:
//  1. grace: an op younger than io_block_after_us still gets sched_yield
//     (run with a 60 s threshold, so the held write never reaches it);
//  2. stuck op: the pause blocks, and the loop still runs between pauses;
//  3. stuck op while other I/O on the shard completes at once (writes to
//     /dev/null): the pause keys on the oldest op, so it goes on;
//  4. drained: the held write completes with its full length;
//  5. nothing in flight: no pauses.
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "check.h"
#include "snc/native_runtime.h"

using namespace snc;

namespace {
std::atomic<bool> g_busy{true};
std::atomic<int> g_tid{0};
std::atomic<int> g_done{0};      // 1 = held write completed
std::atomic<int> g_result{0};
std::atomic<int> g_async{-1};    // 1 = io_uring took it, 0 = synchronous fallback
std::atomic<long> g_polls{0};    // loop passes
char g_buf[4096];

std::atomic<bool> g_side_io{false};  // keep quick writes completing on the shard
std::atomic<long> g_side_done{0};
bool g_side_pending = false;  // shard thread only
int g_null_fd = -1;
char g_side_buf[512];

void side_done(void*, int) {
  g_side_pending = false;
  g_side_done.fetch_add(1);
}

// A shard that always has work; optionally also other I/O that completes at once.
bool poll(void*) {
  g_polls.fetch_add(1, std::memory_order_relaxed);
  if (g_side_io.load(std::memory_order_relaxed) && !g_side_pending) {
    g_side_pending = true;
    // Returns false on the synchronous fallback; side_done still runs.
    (void)sri_write(g_null_fd, 0, g_side_buf, sizeof(g_side_buf), false, &side_done, nullptr);
  }
  return g_busy.load(std::memory_order_relaxed);
}

void write_done(void*, int result) {
  g_result.store(result);
  g_done.store(1);
}

long voluntary_switches(int tid) {
  std::ifstream f("/proc/self/task/" + std::to_string(tid) + "/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("voluntary_ctxt_switches:", 0) == 0) return std::stol(line.substr(24));
  }
  return -1;
}

// The side writes stop once the flag is off and the last one has completed.
bool wait_for_side_idle() {
  long last = -1;
  for (int i = 0; i < 1000; ++i) {
    long now = g_side_done.load();
    if (now == last) return true;
    last = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool wait_for(const std::atomic<int>& v, int want, int ms) {
  for (int i = 0; i < ms && v.load() != want; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return v.load() == want;
}

struct Window {
  long switches;
  long polls;
  long side;
};

// Long enough that a loaded host still shows hundreds of pauses.
Window measure(int tid) {
  const long v0 = voluntary_switches(tid), p0 = g_polls.load(), s0 = g_side_done.load();
  CHECK(v0 >= 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  return Window{voluntary_switches(tid) - v0, g_polls.load() - p0, g_side_done.load() - s0};
}

struct Pipe {
  int r = -1, w = -1;
  long filled = 0;
};

// A pipe filled to capacity, so the next write cannot complete until it is drained.
Pipe full_pipe() {
  int p[2];
  CHECK(::pipe2(p, O_CLOEXEC) == 0);
  int fl = ::fcntl(p[1], F_GETFL);
  CHECK(::fcntl(p[1], F_SETFL, fl | O_NONBLOCK) == 0);
  Pipe out{p[0], p[1], 0};
  for (;;) {
    ssize_t n = ::write(p[1], g_buf, sizeof(g_buf));
    if (n <= 0) break;
    out.filled += n;
  }
  CHECK(::fcntl(p[1], F_SETFL, fl) == 0);  // blocking again: the held write must wait, not fail
  return out;
}

// Drains the pipe; the held write then completes and its completion is
// delivered with its full length (nothing lost by pausing).
void drain(Pipe& p) {
  char sink[4096];
  long drained = 0;
  while (drained < p.filled + static_cast<long>(sizeof(g_buf))) {
    ssize_t n = ::read(p.r, sink, sizeof(sink));
    CHECK(n > 0);
    drained += n;
    if (g_done.load() == 1 && drained >= p.filled) break;
  }
  CHECK(wait_for(g_done, 1, 5000));
  CHECK(g_result.load() == static_cast<int>(sizeof(g_buf)));
  ::close(p.r);
  ::close(p.w);
}

// Starts a one-shard runtime and holds a write in flight on it. False if
// io_uring is unavailable (the write ran synchronously).
bool hold_write(NativeRuntime& rt, Pipe& p) {
  g_done = 0;
  g_async = -1;
  rt.start();
  rt.submit(0, [&] {
    g_tid.store(static_cast<int>(::syscall(SYS_gettid)));
    g_async.store(sri_write(p.w, 0, g_buf, sizeof(g_buf), false, &write_done, nullptr) ? 1 : 0);
  });
  CHECK(wait_for(g_async, 1, 5000) || g_async.load() == 0);
  if (g_async.load() != 1) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(g_done.load() == 0);  // in flight, not completed
  return true;
}

RuntimeConfig config(u32 block_after_us) {
  RuntimeConfig cfg;
  cfg.n_shards = 1;
  cfg.io_yield_us = 50;  // the default; a regression must not depend on a tuned value
  cfg.io_block_after_us = block_after_us;
  return cfg;
}
}  // namespace

int main() {
  std::memset(g_buf, 'x', sizeof(g_buf));
  g_null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  CHECK(g_null_fd >= 0);
  ShardHooks hooks;
  hooks.poll = poll;

  // 1. Grace: the held write never gets old enough, so the pause stays
  //    sched_yield (which is never a voluntary switch).
  {
    Pipe p = full_pipe();
    NativeRuntime rt(config(60'000'000), hooks);
    if (!hold_write(rt, p)) {
      std::printf("test_io_yield: SKIP (io_uring unavailable; the write ran synchronously)\n");
      g_busy = false;
      rt.stop();
      return 0;
    }
    Window w = measure(g_tid.load());
    std::printf("test_io_yield: grace: %ld voluntary switches in 1 s with a young write in flight\n", w.switches);
    CHECK(w.switches < 100);
    CHECK(g_done.load() == 0);
    drain(p);
    g_busy = false;
    rt.stop();
    g_busy = true;
  }

  // 2-5 with the default threshold.
  Pipe p = full_pipe();
  NativeRuntime rt(config(RuntimeConfig{}.io_block_after_us), hooks);
  CHECK(hold_write(rt, p));
  const int tid = g_tid.load();

  // 2. The defect: a busy loop with an op stuck in flight that never blocks.
  //    A 50 us period gives thousands of pauses a second; sched_yield gives
  //    none. Between pauses the loop must still run (many passes per pause),
  //    not sleep on every pass.
  Window w = measure(tid);
  std::printf("test_io_yield: stuck: %ld voluntary switches, %ld loop passes in 1 s\n", w.switches, w.polls);
  CHECK(w.switches >= 100);
  CHECK(w.polls >= 10 * w.switches);
  CHECK(g_done.load() == 0);

  // 3. Same, while other I/O on the shard keeps completing: the held write is
  //    still the oldest in flight, so the pause must go on.
  g_side_io = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  w = measure(tid);
  g_side_io = false;
  std::printf("test_io_yield: stuck + side I/O: %ld voluntary switches, %ld other writes completed in 1 s\n",
              w.switches, w.side);
  CHECK(w.side >= 100);
  CHECK(w.switches >= 100);
  CHECK(g_done.load() == 0);
  CHECK(wait_for_side_idle());

  // 4. Failure condition cleared.
  drain(p);

  // 5. Nothing in flight: the pause stops.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  w = measure(tid);
  std::printf("test_io_yield: idle: %ld voluntary switches in 1 s with nothing in flight\n", w.switches);
  CHECK(w.switches < 100);

  g_busy = false;
  rt.stop();
  ::close(g_null_fd);
  std::printf("test_io_yield: OK\n");
  return 0;
}
