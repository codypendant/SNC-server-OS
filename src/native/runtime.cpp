// Native SRI backend: one pinned pthread per shard, a full mesh of SPSC rings
// of 128-byte slots, eventfd wakeup after a short spin (spec sections 5, 7).
#include "snc/native_runtime.h"
#include "snc/trace.h"

#include <emmintrin.h>
#include <liburing.h>
#include <poll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <latch>
#include <mutex>
#include <thread>

#include "ring.h"

namespace snc {

namespace {

inline void cpu_relax() { _mm_pause(); }

u64 now_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

}  // namespace

struct Worker {
  u16 id = 0;
  ShardTopology topo;
  std::unique_ptr<ShardAllocator> alloc;
  int efd = -1;
  alignas(64) std::atomic<bool> sleeping{false};
  // Loop iterations, written only by this worker, read by any thread
  // (NativeRuntime::progress; docs/HEALTH_DECOUPLING.md 3.2). Its own
  // cache line, so readers never share one with the hot fields.
  alignas(64) std::atomic<u64> iterations{0};
  alignas(64) char pad_after_iterations_ = 0;

  std::vector<Ring*> inbound;   // [src], owned; allocated on this worker
  std::vector<Ring*> outbound;  // [dst], = workers[dst].inbound[id]
  std::vector<std::deque<Msg>> overflow;  // [dst], producer-private
  std::vector<u16> touched;     // dsts with unpublished pushes
  std::vector<u8> touched_flag;

  std::mutex inject_mu;
  std::vector<std::function<void()>> injected;
  std::atomic<bool> has_injected{false};

  // File I/O: io_uring when available, else synchronous pwritev2 with the
  // completion deferred to the next loop iteration.
  io_uring ring{};
  bool uring_ok = false;
  u32 io_inflight = 0;
  struct IoReq {
    IoDone done;
    void* ctx;
    int result;
  };
  struct IoOp {
    IoDone done;
    void* ctx;
    iovec iov;
  };
  std::vector<IoReq> io_deferred;

  // Idle wait: the eventfd plus any fds registered with sri_watch_fd.
  std::vector<pollfd> idle_fds;
  u64 last_yield_ns = 0;

  std::thread thread;
};

struct NativeRuntime::Impl {
  RuntimeConfig cfg;
  ShardHooks hooks;
  std::vector<std::unique_ptr<Worker>> workers;
  std::atomic<bool> stopping{false};
  bool started = false;

  void run(Worker& w, std::barrier<>& rings_ready, std::latch& init_done);
  void loop(Worker& w);
  bool drain(Worker& w, u16 src);
  void publish_all(Worker& w);
  void wake(Worker& target);
};

namespace {
thread_local Worker* tl_worker = nullptr;
thread_local NativeRuntime::Impl* tl_rt = nullptr;
}  // namespace

// ---- SRI implementation ----------------------------------------------------

u16 sri_shard_id() { return tl_worker->id; }
u16 sri_shard_count() { return static_cast<u16>(tl_rt->workers.size()); }
const ShardTopology& sri_topology() { return tl_worker->topo; }
ShardAllocator& sri_allocator() { return *tl_worker->alloc; }
u64 sri_now_ns() { return now_ns(); }
const char* sri_backend() { return "native"; }

Msg* sri_claim(u16 dst, bool must_deliver) {
  Worker& w = *tl_worker;
  Ring& r = *w.outbound[dst];
  auto& ov = w.overflow[dst];
  if (ov.empty()) {
    if (r.p_tail - r.p_head_cache == r.mask + 1) r.p_head_cache = r.head.load(std::memory_order_acquire);
    if (r.p_tail - r.p_head_cache != r.mask + 1) return &r.slots[r.p_tail & r.mask];
  }
  ++r.stats.full_events;
  if (!must_deliver && ov.size() >= tl_rt->cfg.overflow_limit) {
    ++r.stats.refused;
    return nullptr;
  }
  return &ov.emplace_back();
}

u64 sri_overflow_queued() {
  u64 n = 0;
  for (const auto& ov : tl_worker->overflow) n += ov.size();
  return n;
}

void sri_publish(u16 dst) {
  Worker& w = *tl_worker;
  Ring& r = *w.outbound[dst];
  if (r.publish()) r.p_unwoken = true;  // the loop's publish_all does the wake check
}

void sri_commit(u16 dst, Msg* msg) {
  Worker& w = *tl_worker;
  Ring& r = *w.outbound[dst];
  if (msg == &r.slots[r.p_tail & r.mask] && w.overflow[dst].empty()) ++r.p_tail;
  ++r.stats.msgs;
  if (msg->flags & kMsgArgsExternal) r.stats.bytes_external += msg->len;
  else r.stats.bytes_inline += msg->len;
  if (!w.touched_flag[dst]) {
    w.touched_flag[dst] = 1;
    w.touched.push_back(dst);
  }
}

bool sri_send(u16 dst, const Msg& msg, bool must_deliver) {
  Worker& w = *tl_worker;
  Ring& r = *w.outbound[dst];
  auto& ov = w.overflow[dst];
  bool queued;
  if (ov.empty() && r.try_push(msg)) {
    queued = true;
  } else {
    ++r.stats.full_events;
    if (!must_deliver && ov.size() >= tl_rt->cfg.overflow_limit) {
      ++r.stats.refused;
      return false;
    }
    ov.push_back(msg);
    queued = true;
  }
  if (queued) {
    ++r.stats.msgs;
    if (msg.flags & kMsgArgsExternal) r.stats.bytes_external += msg.len;
    else r.stats.bytes_inline += msg.len;
    if (!w.touched_flag[dst]) {
      w.touched_flag[dst] = 1;
      w.touched.push_back(dst);
    }
  }
  return true;
}

void sri_write(int fd, u64 off, const void* buf, u32 len, bool dsync, IoDone done, void* ctx) {
  Worker& w = *tl_worker;
  int flags = dsync ? RWF_DSYNC : 0;
  if (w.uring_ok) {
    io_uring_sqe* sqe = io_uring_get_sqe(&w.ring);
    if (sqe == nullptr) {
      io_uring_submit(&w.ring);
      sqe = io_uring_get_sqe(&w.ring);
    }
    if (sqe != nullptr) {
      // The iovec must live until completion, so it travels with the op.
      auto* op = new Worker::IoOp{done, ctx, iovec{const_cast<void*>(buf), len}};
      io_uring_prep_writev2(sqe, fd, &op->iov, 1, off, flags);
      io_uring_sqe_set_data(sqe, op);
      io_uring_submit(&w.ring);
      ++w.io_inflight;
      return;
    }
  }
  iovec iov{const_cast<void*>(buf), len};
  ssize_t n = ::pwritev2(fd, &iov, 1, static_cast<off_t>(off), flags);
  w.io_deferred.push_back(Worker::IoReq{done, ctx, n < 0 ? -errno : static_cast<int>(n)});
}

void sri_watch_fd(int fd) { tl_worker->idle_fds.push_back(pollfd{fd, POLLIN, 0}); }

void sri_fdatasync(int fd, IoDone done, void* ctx) {
  Worker& w = *tl_worker;
  if (w.uring_ok) {
    io_uring_sqe* sqe = io_uring_get_sqe(&w.ring);
    if (sqe == nullptr) {
      io_uring_submit(&w.ring);
      sqe = io_uring_get_sqe(&w.ring);
    }
    if (sqe != nullptr) {
      auto* op = new Worker::IoOp{done, ctx, iovec{}};
      io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
      io_uring_sqe_set_data(sqe, op);
      io_uring_submit(&w.ring);
      ++w.io_inflight;
      return;
    }
  }
  int r = ::fdatasync(fd);
  w.io_deferred.push_back(Worker::IoReq{done, ctx, r < 0 ? -errno : 0});
}

namespace {
// Runs completed file I/O callbacks. Returns true if any ran.
bool reap_io(Worker& w) {
  bool did = false;
  if (!w.io_deferred.empty()) {
    std::vector<Worker::IoReq> ready;
    ready.swap(w.io_deferred);
    for (auto& r : ready) r.done(r.ctx, r.result);
    did = true;
  }
  if (w.uring_ok && w.io_inflight > 0) {
    io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&w.ring, &cqe) == 0) {
      auto* op = static_cast<Worker::IoOp*>(io_uring_cqe_get_data(cqe));
      int res = cqe->res;
      io_uring_cqe_seen(&w.ring, cqe);
      --w.io_inflight;
      op->done(op->ctx, res);
      delete op;
      did = true;
    }
  }
  return did;
}
}  // namespace

// ---- Worker loop ------------------------------------------------------------

void NativeRuntime::Impl::wake(Worker& target) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (target.sleeping.load(std::memory_order_relaxed)) {
    u64 one = 1;
    ssize_t n = ::write(target.efd, &one, sizeof(one));
    (void)n;
  }
}

void NativeRuntime::Impl::publish_all(Worker& w) {
  // Iterate by index: nothing here appends to `touched`. Publish every
  // tail first, then one fence for all of them before checking who sleeps
  // (the Dekker handshake in wake()): a full fence also waits for the
  // store buffer, so one per destination per iteration adds up.
  std::size_t keep = 0;
  u16 published[64];
  u32 n_published = 0;
  for (std::size_t i = 0; i < w.touched.size(); ++i) {
    u16 dst = w.touched[i];
    Ring& r = *w.outbound[dst];
    auto& ov = w.overflow[dst];
    while (!ov.empty() && r.try_push(ov.front())) ov.pop_front();
    bool due = r.publish() || r.p_unwoken;
    r.p_unwoken = false;
    if (due) {
      if (n_published < 64) {
        published[n_published++] = dst;
      } else {
        wake(*workers[dst]);
      }
    }
    if (!ov.empty()) {
      w.touched[keep++] = dst;  // retry next iteration
    } else {
      w.touched_flag[dst] = 0;
    }
  }
  w.touched.resize(keep);
  if (n_published == 0) return;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  for (u32 i = 0; i < n_published; ++i) {
    Worker& t = *workers[published[i]];
    if (t.sleeping.load(std::memory_order_relaxed)) {
      u64 one = 1;
      ssize_t n = ::write(t.efd, &one, sizeof(one));
      (void)n;
    }
  }
}

bool NativeRuntime::Impl::drain(Worker& w, u16 src) {
  Ring& r = *w.inbound[src];
  u32 t = r.tail.load(std::memory_order_acquire);
  if (t == r.c_head) return false;
  u32 n = 0;
  if (hooks.on_batch) {
    const Msg* batch[64];
    u32 m = 0;
    for (u32 h = r.c_head; h != t && m < cfg.batch_in && m < 64; ++h) batch[m++] = &r.slots[h & r.mask];
    hooks.on_batch(hooks.user, batch, m);
  }
  while (r.c_head != t && n < cfg.batch_in) {
    hooks.on_message(hooks.user, r.slots[r.c_head & r.mask]);
    ++r.c_head;
    ++n;
  }
  r.head.store(r.c_head, std::memory_order_release);
  return true;
}

void NativeRuntime::Impl::loop(Worker& w) {
  const u16 n = static_cast<u16>(workers.size());
  u64 idle_since = 0;
  // SNC_TRACE=1: report loop iterations long enough to delay control work
  // (STATUS, admin requests wait on every shard; docs/TRACING.md).
  const bool tr = trace::enabled();
  constexpr u64 kLongIterNs = 20'000'000;
  while (true) {
    // Progress (docs/HEALTH_DECOUPLING.md 3.2): an idle loop still comes
    // here about every 1 ms, so a count that stops means one iteration is
    // still running. Single writer: a plain relaxed load and store.
    w.iterations.store(w.iterations.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    const u64 t_start = tr ? now_ns() : 0;
    u64 t_drain = 0, t_inject = 0, t_io = 0, t_poll = 0;
    std::size_t n_injected = 0;
    // Stop once asked and the shard has nothing in flight (e.g. its WAL is
    // flushed), so a clean stop never drops acknowledged-but-pending work.
    if (stopping.load(std::memory_order_relaxed) &&
        (hooks.quiesced == nullptr || hooks.quiesced(hooks.user)) && w.io_inflight == 0 &&
        w.io_deferred.empty()) {
      break;
    }
    bool did = false;
    for (u16 s = 0; s < n; ++s) {
      if (s != w.id) did |= drain(w, s);
    }
    if (tr) t_drain = now_ns();
    if (w.has_injected.load(std::memory_order_acquire)) {
      std::vector<std::function<void()>> fns;
      {
        std::lock_guard<std::mutex> g(w.inject_mu);
        fns.swap(w.injected);
        w.has_injected.store(false, std::memory_order_relaxed);
      }
      n_injected = fns.size();
      for (auto& fn : fns) fn();
      did = true;
    }
    if (tr) t_inject = now_ns();
    did |= reap_io(w);
    if (tr) t_io = now_ns();
    if (hooks.poll) did |= hooks.poll(hooks.user);
    if (tr) t_poll = now_ns();
    publish_all(w);
    if (tr) {
      const u64 t_end = now_ns();
      if (t_end - t_start >= kLongIterNs) {
        SNC_TRACE("shard_long_iter shard=%u total_ms=%.1f drain_ms=%.1f inject_ms=%.1f injected=%zu io_ms=%.1f "
                  "poll_ms=%.1f publish_ms=%.1f",
                  static_cast<unsigned>(w.id), (t_end - t_start) / 1e6, (t_drain - t_start) / 1e6,
                  (t_inject - t_drain) / 1e6, n_injected, (t_io - t_inject) / 1e6, (t_poll - t_io) / 1e6,
                  (t_end - t_poll) / 1e6);
      }
    }
    if (cfg.io_yield_us && w.io_inflight > 0) {
      u64 now = now_ns();
      if (now - w.last_yield_ns >= u64{cfg.io_yield_us} * 1000) {
        sched_yield();
        w.last_yield_ns = now;
      }
    }
    if (did || !w.touched.empty()) {
      idle_since = 0;
      continue;
    }
    if (cfg.spin_forever) {
      cpu_relax();
      continue;
    }
    u64 now = now_ns();
    if (idle_since == 0) idle_since = now;
    if (now - idle_since < u64{cfg.idle_spin_us} * 1000) {
      cpu_relax();
      continue;
    }
    // Dekker-style handshake with producers (see wake()).
    w.sleeping.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bool pending = w.has_injected.load(std::memory_order_relaxed) ||
                   stopping.load(std::memory_order_relaxed) || !w.io_deferred.empty() ||
                   (w.uring_ok && io_uring_cq_ready(&w.ring) > 0);
    for (u16 s = 0; s < n && !pending; ++s) {
      if (s != w.id) pending = w.inbound[s]->has_pending_for_consumer();
    }
    if (!pending) {
      // Timeout keeps periodic work (group commit, checkpoints) alive later.
      for (pollfd& p : w.idle_fds) p.revents = 0;
      if (::poll(w.idle_fds.data(), w.idle_fds.size(), 1) > 0 && w.idle_fds[0].revents) {
        u64 v;
        ssize_t r = ::read(w.efd, &v, sizeof(v));
        (void)r;
      }
    }
    w.sleeping.store(false, std::memory_order_relaxed);
    idle_since = 0;
  }
}

void NativeRuntime::Impl::run(Worker& w, std::barrier<>& rings_ready, std::latch& init_done) {
  tl_worker = &w;
  tl_rt = this;
  const u16 n = static_cast<u16>(workers.size());

  // Pin before allocating anything, so first-touch lands on the local node.
  w.topo.shard = w.id;
  if (cfg.pin) {
    int cpu = cfg.cpus.empty() ? -1 : cfg.cpus[w.id % cfg.cpus.size()];
    if (cpu < 0) {
      cpu_set_t allowed;
      CPU_ZERO(&allowed);
      sched_getaffinity(0, sizeof(allowed), &allowed);
      int count = CPU_COUNT(&allowed), k = w.id % (count > 0 ? count : 1);
      for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (CPU_ISSET(c, &allowed) && k-- == 0) { cpu = c; break; }
      }
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    w.topo.pinned = sched_setaffinity(0, sizeof(set), &set) == 0;
  }
  unsigned cpu = 0, node = 0;
  if (syscall(SYS_getcpu, &cpu, &node, nullptr) == 0) {
    w.topo.cpu = static_cast<int>(cpu);
    w.topo.numa_node = static_cast<int>(node);
  }

  w.alloc = std::make_unique<ShardAllocator>(cfg.heap_limit_bytes, cfg.pages, cfg.heap_reserve_bytes);
  w.idle_fds.assign(1, pollfd{w.efd, POLLIN, 0});
  // io_uring completions signal the same eventfd that wakes a sleeping loop.
  if (io_uring_queue_init(256, &w.ring, 0) == 0) {
    w.uring_ok = io_uring_register_eventfd(&w.ring, w.efd) == 0;
    if (!w.uring_ok) {
      io_uring_queue_exit(&w.ring);
    } else if (!cfg.io_worker_cpus.empty()) {
      cpu_set_t io_set;
      CPU_ZERO(&io_set);
      for (int c : cfg.io_worker_cpus) CPU_SET(c, &io_set);
      if (io_uring_register_iowq_aff(&w.ring, sizeof(io_set), &io_set) != 0) {
        std::fprintf(stderr, "snc: io_uring_register_iowq_aff failed; using default placement\n");
      }
    }
  }
  w.topo.io_uring = w.uring_ok;
  // Inbound rings are allocated by their consumer (spec: consumer's node).
  w.inbound.assign(n, nullptr);
  for (u16 s = 0; s < n; ++s) {
    if (s != w.id) w.inbound[s] = new Ring(cfg.ring_slots);
  }
  rings_ready.arrive_and_wait();
  w.outbound.assign(n, nullptr);
  for (u16 d = 0; d < n; ++d) {
    if (d != w.id) w.outbound[d] = workers[d]->inbound[w.id];
  }
  w.overflow.resize(n);
  w.touched_flag.assign(n, 0);

  if (hooks.init) hooks.init(hooks.user);
  init_done.count_down();
  loop(w);
  if (hooks.fini) hooks.fini(hooks.user);
  // fini may have waited on I/O; drain whatever is left before tearing down.
  while (w.io_inflight > 0 || !w.io_deferred.empty()) reap_io(w);
  if (w.uring_ok) io_uring_queue_exit(&w.ring);
  // mimalloc heaps belong to their thread: destroy it here, not in ~Worker.
  w.alloc.reset();
  tl_worker = nullptr;
  tl_rt = nullptr;
}

// ---- Public API -------------------------------------------------------------

NativeRuntime::NativeRuntime(const RuntimeConfig& cfg, const ShardHooks& hooks)
    : impl_(std::make_unique<Impl>()) {
  if (cfg.n_shards == 0 || cfg.n_shards > kMaxShards) {
    std::fprintf(stderr, "snc: n_shards must be 1..%u\n", kMaxShards);
    std::abort();
  }
  if (cfg.ring_slots < 2 || (cfg.ring_slots & (cfg.ring_slots - 1)) != 0) {
    std::fprintf(stderr, "snc: ring_slots must be a power of two\n");
    std::abort();
  }
  impl_->cfg = cfg;
  impl_->hooks = hooks;
  for (u16 i = 0; i < cfg.n_shards; ++i) {
    auto w = std::make_unique<Worker>();
    w->id = i;
    w->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w->efd < 0) std::abort();
    impl_->workers.push_back(std::move(w));
  }
}

NativeRuntime::~NativeRuntime() {
  stop();
  for (auto& w : impl_->workers) {
    for (Ring* r : w->inbound) delete r;
    if (w->efd >= 0) ::close(w->efd);
  }
}

void NativeRuntime::start() {
  if (impl_->started) return;
  impl_->started = true;
  const auto n = static_cast<std::ptrdiff_t>(impl_->workers.size());
  std::barrier<> rings_ready(n);
  std::latch init_done(n);
  for (auto& w : impl_->workers) {
    Worker* wp = w.get();
    wp->thread = std::thread([this, wp, &rings_ready, &init_done] {
      impl_->run(*wp, rings_ready, init_done);
    });
  }
  init_done.wait();
}

void NativeRuntime::stop() {
  if (!impl_->started) return;
  impl_->stopping.store(true);
  for (auto& w : impl_->workers) {
    u64 one = 1;
    ssize_t n = ::write(w->efd, &one, sizeof(one));
    (void)n;
  }
  for (auto& w : impl_->workers) {
    if (w->thread.joinable()) w->thread.join();
  }
  impl_->started = false;
}

void NativeRuntime::submit(u16 shard, std::function<void()> fn) {
  Worker& w = *impl_->workers.at(shard);
  {
    std::lock_guard<std::mutex> g(w.inject_mu);
    w.injected.push_back(std::move(fn));
    w.has_injected.store(true, std::memory_order_release);
  }
  impl_->wake(w);
}

void NativeRuntime::run_on_all(const std::function<void(u16)>& fn) {
  std::latch done(static_cast<std::ptrdiff_t>(impl_->workers.size()));
  for (u16 i = 0; i < impl_->workers.size(); ++i) {
    submit(i, [&fn, &done, i] {
      fn(i);
      done.count_down();
    });
  }
  done.wait();
}

u16 NativeRuntime::shard_count() const { return static_cast<u16>(impl_->workers.size()); }

const ShardTopology& NativeRuntime::topology(u16 shard) const {
  return impl_->workers.at(shard)->topo;
}

bool NativeRuntime::progress(u16 shard, u64& iterations) const {
  if (shard >= impl_->workers.size()) return false;
  iterations = impl_->workers[shard]->iterations.load(std::memory_order_relaxed);
  return true;
}

RingStats NativeRuntime::ring_stats(u16 src, u16 dst) const {
  if (src == dst) return {};
  return impl_->workers.at(dst)->inbound.at(src)->stats;
}

}  // namespace snc
