// F1 regression (docs/F1_Q10_LIVENESS_ROOT_CAUSE.md): while file I/O is in
// flight and none of it has completed for io_block_after_us, a shard loop
// that always has work must give its core up by blocking, so the kernel
// threads that complete that I/O on the core can run. sched_yield does not
// do that: the thread stays runnable, and on the hosted 7.0 kernels the
// core's ksoftirqd and dio kworker were starved for minutes.
//
// The check does not depend on the scheduler's choices. A blocking pause is
// a voluntary context switch (voluntary_ctxt_switches in /proc); sched_yield
// never is, whatever else is runnable. So with a write held in flight and the
// loop kept busy, the shard thread's voluntary switches must keep rising.
//
// A write to a full pipe stays in flight until the pipe is drained, so the
// in-flight state is held for as long as the test needs, with no disk.
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
std::atomic<int> g_done{0};      // 1 = write completed
std::atomic<int> g_result{0};
std::atomic<int> g_async{-1};    // 1 = io_uring took it, 0 = synchronous fallback
char g_buf[4096];

bool poll(void*) { return g_busy.load(std::memory_order_relaxed); }  // a shard that always has work

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

bool wait_for(const std::atomic<int>& v, int want, int ms) {
  for (int i = 0; i < ms && v.load() != want; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return v.load() == want;
}
}  // namespace

int main() {
  int p[2];
  CHECK(::pipe2(p, O_CLOEXEC) == 0);
  // Fill the pipe, so the next write cannot complete until it is drained.
  int fl = ::fcntl(p[1], F_GETFL);
  CHECK(::fcntl(p[1], F_SETFL, fl | O_NONBLOCK) == 0);
  std::memset(g_buf, 'x', sizeof(g_buf));
  long filled = 0;
  for (;;) {
    ssize_t n = ::write(p[1], g_buf, sizeof(g_buf));
    if (n <= 0) break;
    filled += n;
  }
  CHECK(::fcntl(p[1], F_SETFL, fl) == 0);  // blocking again: the write below must wait, not fail

  RuntimeConfig cfg;
  cfg.n_shards = 1;
  cfg.io_yield_us = 50;          // the defaults; a regression must not
  cfg.io_block_after_us = 1000;  // depend on a tuned value
  ShardHooks hooks;
  hooks.poll = poll;
  NativeRuntime rt(cfg, hooks);
  rt.start();

  rt.submit(0, [&] {
    g_tid.store(static_cast<int>(::syscall(SYS_gettid)));
    g_async.store(sri_write(p[1], 0, g_buf, sizeof(g_buf), false, &write_done, nullptr) ? 1 : 0);
  });
  CHECK(wait_for(g_async, 1, 5000) || g_async.load() == 0);
  if (g_async.load() != 1) {
    std::printf("test_io_yield: SKIP (io_uring unavailable; the write ran synchronously)\n");
    g_busy = false;
    rt.stop();
    return 0;
  }

  // Normal condition first: the write is in flight and has not completed.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(g_done.load() == 0);

  // The defect: a busy loop with I/O in flight that never blocks. Over 300 ms
  // a 50 us period gives thousands of pauses; sched_yield gives none.
  const int tid = g_tid.load();
  const long v0 = voluntary_switches(tid);
  CHECK(v0 >= 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const long v1 = voluntary_switches(tid);
  std::printf("test_io_yield: %ld voluntary switches in 300 ms with a write in flight\n", v1 - v0);
  CHECK(v1 - v0 >= 100);
  CHECK(g_done.load() == 0);

  // Failure condition cleared: drain the pipe; the held write completes and
  // its completion is delivered (nothing lost by pausing).
  char sink[4096];
  long drained = 0;
  while (drained < filled + static_cast<long>(sizeof(g_buf))) {
    ssize_t n = ::read(p[0], sink, sizeof(sink));
    CHECK(n > 0);
    drained += n;
    if (g_done.load() == 1 && drained >= filled) break;
  }
  CHECK(wait_for(g_done, 1, 5000));
  CHECK(g_result.load() == static_cast<int>(sizeof(g_buf)));

  // With nothing in flight, the pause stops: the busy loop no longer blocks.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const long v2 = voluntary_switches(tid);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const long v3 = voluntary_switches(tid);
  std::printf("test_io_yield: %ld voluntary switches in 300 ms with nothing in flight\n", v3 - v2);
  CHECK(v3 - v2 < 100);

  g_busy = false;
  rt.stop();
  ::close(p[0]);
  ::close(p[1]);
  std::printf("test_io_yield: OK\n");
  return 0;
}
