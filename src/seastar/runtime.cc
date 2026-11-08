// Seastar backend for the SNC Runtime Interface (handoff §24 step 2, spec D2;
// docs/SEASTAR.md). Implements the same NativeRuntime class as
// src/native/runtime.cpp, so a binary picks its backend at link time and SNC
// core is unchanged.
//
// Shards are Seastar reactor threads (--smp, --cpuset). Each shard's loop
// body (drain, poll hook, publish) runs as a Seastar poller. Cross-shard
// messages travel on Seastar's own inter-core queues: messages for one
// destination collected during an iteration go as one smp::submit_to. With
// SNC_SEASTAR_MSG=rings they use SNC's own SPSC rings (src/native/ring.h)
// instead, which separates the cost of Seastar's messaging from the cost of
// its reactor. File
// writes use a per-shard io_uring polled by the same poller, as in the
// native backend, so durability is comparable.
//
// Limits of this evaluation backend: one start per process (a Seastar
// reactor cannot be restarted), shards never sleep (Seastar polls), and
// sends are never refused (Seastar's queues are unbounded).
#include <liburing.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

#include <seastar/core/app-template.hh>
#include <seastar/core/internal/poll.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>

#include "snc/native_runtime.h"

#include "../native/ring.h"

namespace snc {

namespace {
// Tests only (sri_debug_force_sync_io): file I/O takes the synchronous path.
std::atomic<bool> g_force_sync_io{false};
}  // namespace

namespace {

u64 now_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

struct Shard;

struct Global {
  RuntimeConfig cfg;
  ShardHooks hooks;
  std::vector<Shard*> shards;  // indexed by shard id; set during start
  std::atomic<bool> stopping{false};
  std::atomic<u32> done{0};    // shards that finished their loop
  std::thread app;
  bool started = false;
  bool ran = false;
  bool rings = false;  // SNC_SEASTAR_MSG=rings
};
Global* g = nullptr;

struct Shard {
  u16 id = 0;
  ShardTopology topo;
  std::unique_ptr<ShardAllocator> alloc;
  std::vector<std::vector<Msg>> out;  // [dst], sent this iteration
  std::vector<u16> touched;
  std::vector<RingStats> stats;       // [dst]
  // Rings mode.
  std::vector<Ring*> inbound;   // [src], owned
  std::vector<Ring*> outbound;  // [dst]
  std::vector<std::deque<Msg>> overflow;  // [dst]
  std::vector<u8> touched_flag;
  std::unique_ptr<seastar::internal::poller> poller;
  bool finished = false;

  // Control-path injection (NativeRuntime::submit).
  std::mutex inject_mu;
  std::vector<std::function<void()>> injected;
  std::atomic<bool> has_injected{false};

  io_uring ring{};
  bool uring_ok = false;
  u32 io_inflight = 0;
  struct IoOp {
    IoDone done;
    void* ctx;
    iovec iov;
  };
  struct IoReq {
    IoDone done;
    void* ctx;
    int result;
  };
  std::vector<IoReq> io_deferred;
};
thread_local Shard* tl = nullptr;

void deliver(std::vector<Msg>& v) {
  if (tl->alloc == nullptr) return;  // arrived after this shard's fini: the shard is gone
  if (g->hooks.on_batch) {
    const Msg* ptrs[256];
    for (std::size_t base = 0; base < v.size(); base += 256) {
      u32 m = 0;
      for (std::size_t i = base; i < v.size() && m < 256; ++i) ptrs[m++] = &v[i];
      g->hooks.on_batch(g->hooks.user, ptrs, m);
      for (u32 i = 0; i < m; ++i) g->hooks.on_message(g->hooks.user, *ptrs[i]);
    }
    return;
  }
  for (const Msg& m : v) g->hooks.on_message(g->hooks.user, m);
}

void flush_to(Shard& s, u16 dst) {
  std::vector<Msg>& v = s.out[dst];
  if (v.empty()) return;
  std::vector<Msg> batch;
  batch.reserve(64);
  batch.swap(v);
  (void)seastar::smp::submit_to(dst, [b = std::move(batch)]() mutable { deliver(b); });
}

void flush_all(Shard& s) {
  for (u16 dst : s.touched) flush_to(s, dst);
  s.touched.clear();
}

// Rings mode: the same drain and publish as the native loop, minus the
// wake-ups (Seastar shards never sleep).
bool drain_rings(Shard& s) {
  const u16 n = static_cast<u16>(g->shards.size());
  bool did = false;
  for (u16 src = 0; src < n; ++src) {
    if (src == s.id) continue;
    Ring& r = *s.inbound[src];
    u32 t = r.tail.load(std::memory_order_acquire);
    if (t == r.c_head) continue;
    did = true;
    if (t - r.c_head > g->cfg.batch_in) t = r.c_head + g->cfg.batch_in;
    if (g->hooks.on_batch) {
      const Msg* batch[64];
      u32 m = 0;
      for (u32 h = r.c_head; h != t && m < 64; ++h) batch[m++] = &r.slots[h & r.mask];
      g->hooks.on_batch(g->hooks.user, batch, m);
    }
    while (r.c_head != t) {
      g->hooks.on_message(g->hooks.user, r.slots[r.c_head & r.mask]);
      ++r.c_head;
    }
    r.head.store(r.c_head, std::memory_order_release);
  }
  return did;
}

void publish_rings(Shard& s) {
  std::size_t keep = 0;
  for (std::size_t i = 0; i < s.touched.size(); ++i) {
    u16 dst = s.touched[i];
    Ring& r = *s.outbound[dst];
    auto& ov = s.overflow[dst];
    while (!ov.empty() && r.try_push(ov.front())) ov.pop_front();
    r.publish();
    if (!ov.empty()) s.touched[keep++] = dst;
    else s.touched_flag[dst] = 0;
  }
  s.touched.resize(keep);
}

bool reap_io(Shard& s) {
  bool did = false;
  if (!s.io_deferred.empty()) {
    std::vector<Shard::IoReq> ready;
    ready.swap(s.io_deferred);
    for (auto& r : ready) r.done(r.ctx, r.result);
    did = true;
  }
  if (s.uring_ok && s.io_inflight > 0) {
    io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&s.ring, &cqe) == 0) {
      auto* op = static_cast<Shard::IoOp*>(io_uring_cqe_get_data(cqe));
      int res = cqe->res;
      io_uring_cqe_seen(&s.ring, cqe);
      --s.io_inflight;
      op->done(op->ctx, res);
      delete op;
      did = true;
    }
  }
  return did;
}

// One loop iteration, run by Seastar's reactor as a poller. Inbound
// messages arrive as tasks between polls, so there is no drain step here.
bool shard_poll() {
  Shard& s = *tl;
  if (s.finished) return false;
  bool did = false;
  if (s.has_injected.load(std::memory_order_acquire)) {
    std::vector<std::function<void()>> fns;
    {
      std::lock_guard<std::mutex> lk(s.inject_mu);
      fns.swap(s.injected);
      s.has_injected.store(false, std::memory_order_relaxed);
    }
    for (auto& fn : fns) fn();
    did = true;
  }
  if (g->rings) did |= drain_rings(s);
  did |= reap_io(s);
  if (g->hooks.poll) did |= g->hooks.poll(g->hooks.user);
  if (g->rings) publish_rings(s);
  else flush_all(s);
  if (g->stopping.load(std::memory_order_relaxed) &&
      (g->hooks.quiesced == nullptr || g->hooks.quiesced(g->hooks.user)) && s.io_inflight == 0 &&
      s.io_deferred.empty()) {
    s.finished = true;
    g->done.fetch_add(1);
  }
  return did;
}

void shard_init(u16 id) {
  auto* s = g->shards[id];
  tl = s;
  s->id = id;
  s->topo.shard = id;
  s->topo.pinned = !g->cfg.cpus.empty() || g->cfg.pin;
  unsigned cpu = 0, node = 0;
  if (syscall(SYS_getcpu, &cpu, &node, nullptr) == 0) {
    s->topo.cpu = static_cast<int>(cpu);
    s->topo.numa_node = static_cast<int>(node);
  }
  const u16 n = static_cast<u16>(g->shards.size());
  s->out.resize(n);
  s->stats.resize(n);
  s->alloc = std::make_unique<ShardAllocator>(g->cfg.heap_limit_bytes, g->cfg.pages,
                                              g->cfg.heap_reserve_bytes);
  if (g->rings) {
    s->inbound.assign(n, nullptr);
    for (u16 src = 0; src < n; ++src) {
      if (src != id) s->inbound[src] = new Ring(g->cfg.ring_slots);
    }
    s->overflow.resize(n);
    s->touched_flag.assign(n, 0);
  }
  s->uring_ok = io_uring_queue_init(256, &s->ring, 0) == 0;
  s->topo.io_uring = s->uring_ok;
}

// After every shard ran shard_init, so every inbound ring exists.
void shard_start(u16 id) {
  Shard* s = g->shards[id];
  if (g->rings) {
    const u16 n = static_cast<u16>(g->shards.size());
    s->outbound.assign(n, nullptr);
    for (u16 d = 0; d < n; ++d) {
      if (d != id) s->outbound[d] = g->shards[d]->inbound[id];
    }
  }
  if (g->hooks.init) g->hooks.init(g->hooks.user);
  s->poller = std::make_unique<seastar::internal::poller>(
      seastar::internal::poller::simple([] { return shard_poll(); }));
}

void shard_fini() {
  Shard& s = *tl;
  s.poller.reset();
  if (g->hooks.fini) g->hooks.fini(g->hooks.user);
  while (s.io_inflight > 0 || !s.io_deferred.empty()) reap_io(s);
  if (s.uring_ok) io_uring_queue_exit(&s.ring);
  s.alloc.reset();
}

io_uring_sqe* get_sqe(Shard& s) {
  io_uring_sqe* sqe = io_uring_get_sqe(&s.ring);
  if (sqe == nullptr) {
    io_uring_submit(&s.ring);
    sqe = io_uring_get_sqe(&s.ring);
  }
  return sqe;
}

}  // namespace

// ---- SRI implementation ----------------------------------------------------

u16 sri_shard_id() { return tl->id; }
u16 sri_shard_count() { return static_cast<u16>(g->shards.size()); }
const ShardTopology& sri_topology() { return tl->topo; }
ShardAllocator& sri_allocator() { return *tl->alloc; }
u64 sri_now_ns() { return now_ns(); }
const char* sri_backend() { return "seastar"; }

Msg* sri_claim(u16 dst, bool must_deliver) {
  Shard& s = *tl;
  if (g->rings) {
    Ring& r = *s.outbound[dst];
    auto& ov = s.overflow[dst];
    if (ov.empty()) {
      if (r.p_tail - r.p_head_cache == r.mask + 1) r.p_head_cache = r.head.load(std::memory_order_acquire);
      if (r.p_tail - r.p_head_cache != r.mask + 1) return &r.slots[r.p_tail & r.mask];
    }
    ++s.stats[dst].full_events;
    if (!must_deliver && ov.size() >= g->cfg.overflow_limit) {
      ++s.stats[dst].refused;
      return nullptr;
    }
    return &ov.emplace_back();
  }
  std::vector<Msg>& v = s.out[dst];
  if (v.empty()) s.touched.push_back(dst);
  return &v.emplace_back();
}

void sri_commit(u16 dst, Msg* msg) {
  Shard& s = *tl;
  if (g->rings) {
    Ring& r = *s.outbound[dst];
    if (msg == &r.slots[r.p_tail & r.mask] && s.overflow[dst].empty()) ++r.p_tail;
    if (!s.touched_flag[dst]) {
      s.touched_flag[dst] = 1;
      s.touched.push_back(dst);
    }
  }
  RingStats& st = s.stats[dst];
  ++st.msgs;
  if (msg->flags & kMsgArgsExternal) st.bytes_external += msg->len;
  else st.bytes_inline += msg->len;
}

bool sri_send(u16 dst, const Msg& msg, bool must_deliver) {
  Msg* m = sri_claim(dst, must_deliver);
  if (m == nullptr) return false;
  *m = msg;
  sri_commit(dst, m);
  return true;
}

u64 sri_overflow_queued() {
  u64 n = 0;
  for (const auto& ov : tl->overflow) n += ov.size();
  return n;
}

void sri_publish(u16 dst) {
  Shard& s = *tl;
  if (g->rings) {
    s.outbound[dst]->publish();
    return;
  }
  flush_to(s, dst);  // `touched` may still name dst; flushing an empty vector is a no-op
}

bool sri_write(int fd, u64 off, const void* buf, u32 len, bool dsync, IoDone done, void* ctx) {
  Shard& s = *tl;
  int flags = dsync ? RWF_DSYNC : 0;
  if (s.uring_ok && !g_force_sync_io.load(std::memory_order_relaxed)) {
    if (io_uring_sqe* sqe = get_sqe(s)) {
      auto* op = new Shard::IoOp{done, ctx, iovec{const_cast<void*>(buf), len}};
      io_uring_prep_writev2(sqe, fd, &op->iov, 1, off, flags);
      io_uring_sqe_set_data(sqe, op);
      io_uring_submit(&s.ring);
      ++s.io_inflight;
      return true;
    }
  }
  iovec iov{const_cast<void*>(buf), len};
  ssize_t n = ::pwritev2(fd, &iov, 1, static_cast<off_t>(off), flags);
  s.io_deferred.push_back(Shard::IoReq{done, ctx, n < 0 ? -errno : static_cast<int>(n)});
  return false;
}

void sri_fdatasync(int fd, IoDone done, void* ctx) {
  Shard& s = *tl;
  if (s.uring_ok && !g_force_sync_io.load(std::memory_order_relaxed)) {
    if (io_uring_sqe* sqe = get_sqe(s)) {
      auto* op = new Shard::IoOp{done, ctx, iovec{}};
      io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
      io_uring_sqe_set_data(sqe, op);
      io_uring_submit(&s.ring);
      ++s.io_inflight;
      return;
    }
  }
  int r = ::fdatasync(fd);
  s.io_deferred.push_back(Shard::IoReq{done, ctx, r < 0 ? -errno : 0});
}

void sri_debug_force_sync_io(bool on) { g_force_sync_io.store(on); }

// Shards never sleep, so there is nothing to wake.
void sri_watch_fd(int) {}

// ---- NativeRuntime, Seastar edition -----------------------------------------

struct NativeRuntime::Impl {};

NativeRuntime::NativeRuntime(const RuntimeConfig& cfg, const ShardHooks& hooks)
    : impl_(std::make_unique<Impl>()) {
  if (g != nullptr) {
    std::fprintf(stderr, "snc: the Seastar backend supports one runtime per process\n");
    std::abort();
  }
  g = new Global;
  g->cfg = cfg;
  const char* mode = std::getenv("SNC_SEASTAR_MSG");
  g->rings = mode != nullptr && std::string(mode) == "rings";
  g->hooks = hooks;
  for (u16 i = 0; i < cfg.n_shards; ++i) g->shards.push_back(new Shard);
}

NativeRuntime::~NativeRuntime() { stop(); }

void NativeRuntime::start() {
  if (g->started) return;
  if (g->ran) {
    std::fprintf(stderr, "snc: the Seastar backend cannot restart its reactor\n");
    std::abort();
  }
  g->started = g->ran = true;
  std::latch init_done(1);
  std::string cpus;
  const u16 n = static_cast<u16>(g->shards.size());
  for (u16 i = 0; i < n; ++i) {
    int c = g->cfg.cpus.empty() ? i : g->cfg.cpus[i % g->cfg.cpus.size()];
    cpus += (i ? "," : "") + std::to_string(c);
  }
  g->app = std::thread([n, cpus, &init_done] {
    std::vector<std::string> args = {"snc", "--smp", std::to_string(n), "--memory", "2G",
                                     "--reactor-backend", "epoll", "--default-log-level",
                                     "warn", "--blocked-reactor-notify-ms", "100000"};
    if (g->cfg.pin) {
      args.push_back("--cpuset");
      args.push_back(cpus);
    } else {
      args.push_back("--thread-affinity");
      args.push_back("0");
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    seastar::app_template::config acfg;
    acfg.auto_handle_sigint_sigterm = false;
    seastar::app_template app(std::move(acfg));
    app.run(static_cast<int>(argv.size()), argv.data(), [&init_done] {
      return seastar::smp::invoke_on_all([] { shard_init(seastar::this_shard_id()); })
          .then([] { return seastar::smp::invoke_on_all([] { shard_start(seastar::this_shard_id()); }); })
          .then([&init_done] {
            init_done.count_down();
            return seastar::do_until(
                [] { return g->done.load() == g->shards.size(); },
                [] { return seastar::sleep(std::chrono::milliseconds(1)); });
          })
          .then([] { return seastar::smp::invoke_on_all([] { shard_fini(); }); })
          .then([] { return 0; });
    });
  });
  init_done.wait();
}

void NativeRuntime::stop() {
  if (!g || !g->started) return;
  g->stopping.store(true);
  if (g->app.joinable()) g->app.join();
  g->started = false;
}

void NativeRuntime::submit(u16 shard, std::function<void()> fn) {
  Shard& s = *g->shards.at(shard);
  std::lock_guard<std::mutex> lk(s.inject_mu);
  s.injected.push_back(std::move(fn));
  s.has_injected.store(true, std::memory_order_release);
}

void NativeRuntime::run_on_all(const std::function<void(u16)>& fn) {
  std::latch done(static_cast<std::ptrdiff_t>(g->shards.size()));
  for (u16 i = 0; i < g->shards.size(); ++i) {
    submit(i, [&fn, &done, i] {
      fn(i);
      done.count_down();
    });
  }
  done.wait();
}

u16 NativeRuntime::shard_count() const { return static_cast<u16>(g->shards.size()); }

const ShardTopology& NativeRuntime::topology(u16 shard) const { return g->shards.at(shard)->topo; }

// Not counted on this backend (docs/HEALTH_DECOUPLING.md 3.2): HEALTH then
// reports no stall_ms, and only silence is visible.
bool NativeRuntime::progress(u16, u64&) const { return false; }

RingStats NativeRuntime::ring_stats(u16 src, u16 dst) const {
  if (src == dst) return {};
  return g->shards.at(src)->stats.at(dst);
}

}  // namespace snc
