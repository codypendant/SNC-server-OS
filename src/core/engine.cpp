// Engine: wires the core into the runtime and owns the control-thread side
// of persistence: superblock, recovery and checkpoint orchestration (spec
// section 12, recovery steps 1 to 9).
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <latch>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "seed.h"
#include "session.h"
#include "shard.h"
#include "slot_state.h"
#include "snc/trace.h"

namespace snc {

namespace {

// S3 (docs/S3_WRITEBACK.md): the window's upper limit, well inside the
// runtime's 256-entry io_uring queue, which the WAL shares.
constexpr u32 kMaxCkptWriteWindow = 64;

// Tests only (Engine::debug_ckpt_hold_commit).
std::atomic<bool> g_debug_ckpt_hold_commit{false};
std::atomic<bool> g_debug_ckpt_commit_reached{false};

struct EngineHooks {
  Engine::Config* cfg;
  std::vector<Shard*>* shards;
  std::vector<ShardStats>* final_stats;
  const SlotMap* slots;
  u64 lease_base;
  const ClusterMap* cluster;
  const std::atomic<bool>* serving;
  std::atomic<bool>* fenced;
  std::atomic<bool>* replicating;
  std::atomic<u8>* phases;
  std::atomic<u64>* epoch_hint;
  ShardGauges* gauges;  // one per shard (S0, observability)
};

void hook_init(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  u16 id = sri_shard_id();
  auto* s = new Shard(id, *h->slots, h->lease_base + id, h->cfg->expected_objects,
                      h->cfg->runtime.pages);
  s->set_mint_local(h->cfg->mint_local);
  s->set_batch_remote(h->cfg->batch_remote);
  // Recovering: no follow-ups run until every shard has finished (see
  // Engine::recover, step 7).
  s->pause_emits(h->cfg->hold_emits || !h->cfg->data_dir.empty());
  s->set_withdraw_flag(h->fenced);
  s->set_replicating_flag(h->replicating);
  s->set_phases(h->phases);
  s->set_epoch_hint(h->epoch_hint);
  s->set_gauges(&h->gauges[id]);
  s->set_admission(Shard::Admission{h->cfg->admission_legacy, h->cfg->waiter_budget_bytes,
                                    h->cfg->wal_buffer_budget_bytes, h->cfg->pending_budget});
  {
    const Engine::Config& c = *h->cfg;
    Shard::Pacing p;
    p.on = c.ckpt_pacing;
    p.batch = c.ckpt_walk_batch;
    p.cfg.walk_min_ns = c.ckpt_walk_min_ns;
    p.cfg.walk_max_ns = c.ckpt_walk_max_ns;
    p.cfg.walk_ceiling_ns = c.ckpt_walk_ceiling_ns;
    p.cfg.walk_step_ns = c.ckpt_walk_step_ns;
    p.cfg.update_ns = c.ckpt_pace_update_ns;
    p.cfg.wal_high_pct = c.ckpt_wal_high_pct;
    p.cfg.wal_low_pct = c.ckpt_wal_low_pct;
    p.cfg.backlog_high_pct = c.ckpt_backlog_high_pct;
    p.cfg.backlog_low_pct = c.ckpt_backlog_low_pct;
    p.cfg.target_ns = c.ckpt_target_ms * 1'000'000;
    p.cfg.max_age_ns = c.ckpt_max_age_ms * 1'000'000;
    p.cfg.warn_pct = c.ckpt_warn_pct;
    s->set_pacing(p);
    s->set_ckpt_io(Shard::CkptIo{c.ckpt_io_dsync, c.ckpt_write_window});
  }
  if (!h->cluster->groups.empty()) s->set_cluster(h->cluster, h->cfg->group_id, h->cfg->member);
  set_current_shard(s);
  (*h->shards)[id] = s;
  // In memory there is nothing to recover: links open at once.
  if (h->cfg->data_dir.empty()) s->start_links();
}

void hook_message(void*, const Msg& m) { current_shard()->on_message(m); }

void hook_batch(void*, const Msg* const* msgs, u32 n) {
  ObjectID ids[64];
  u32 k = 0;
  for (u32 i = 0; i < n && k < 64; ++i) {
    MsgKind kind = msgs[i]->kind;
    if (kind == MsgKind::EXEC_REQ || kind == MsgKind::DELETE_REQ || kind == MsgKind::EMIT) {
      ids[k++] = msgs[i]->target;
    }
  }
  if (k) current_shard()->prefetch(ids, k);
}

bool hook_poll(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  bool busy = current_shard()->poll();
  if (h->cfg->app_poll && h->serving->load(std::memory_order_acquire)) {
    busy |= h->cfg->app_poll(h->cfg->app_user, sri_shard_id());
  }
  current_shard()->flush_batches();  // after the app issued this iteration's requests
  return busy;
}

bool hook_quiesced(void*) { return current_shard()->quiesced(); }

void hook_fini(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  u16 id = sri_shard_id();
  current_shard()->publish_gauges(sri_now_ns(), /*force=*/true);  // stopped: the final values
  (*h->final_stats)[id] = current_shard()->stats();
  (*h->shards)[id] = nullptr;
  delete current_shard();
  set_current_shard(nullptr);
}

double since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// DIR/role (docs/MIGRATION_FAIL_CLOSED.md 4.3, review of PR #39 item 4):
// whether this data directory holds a primary's own disk or a standby's
// replica prefix, which may be lossy. Correctness metadata: written whole,
// synced, renamed into place, the directory synced. Anything but exactly
// "primary" or "standby" reads as MALFORMED. A directory from before role
// files existed (superblock without kSbRoleTracked) lacks it, and its role
// is unknown: it starts as a standby, or as a primary only when adopted
// as one (docs/LEGACY_ROLE.md).
enum class DirRole { MISSING, PRIMARY, STANDBY, MALFORMED };

const char* role_name(DirRole r) {
  switch (r) {
    case DirRole::MISSING: return "missing";
    case DirRole::PRIMARY: return "primary";
    case DirRole::STANDBY: return "standby";
    case DirRole::MALFORMED: return "malformed";
  }
  return "?";
}

DirRole read_role(const std::string& dir) {
  const std::string path = dir + "/role";
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? DirRole::MISSING : DirRole::MALFORMED;
  char b[32];
  const ssize_t n = ::read(fd, b, sizeof(b));
  ::close(fd);
  const std::string s(b, n > 0 ? static_cast<std::size_t>(n) : 0);
  if (s == "primary\n") return DirRole::PRIMARY;
  if (s == "standby\n") return DirRole::STANDBY;
  return DirRole::MALFORMED;
}

void write_role(const std::string& dir, DirRole role) {
  const std::string path = dir + "/role", tmp = path + ".tmp";
  const std::string text = std::string(role_name(role)) + "\n";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0 || ::write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size()) || ::fsync(fd) != 0) {
    if (fd >= 0) ::close(fd);
    storage_fatal("cannot write " + tmp);
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) storage_fatal("cannot rename " + tmp);
  fsync_dir(dir);
}

void remove_tree(const std::string& path) {
  // Only used on flat directories this engine created (checkpoints).
  if (DIR* d = ::opendir(path.c_str())) {
    while (dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") ::unlink((path + "/" + n).c_str());
    }
    ::closedir(d);
  }
  ::rmdir(path.c_str());
}

}  // namespace

struct EngineState {
  EngineHooks hooks;
  std::vector<ShardStats> final_stats;
  // The application's poll hook (a network front end) runs only once every
  // shard has recovered.
  std::atomic<bool> serving{false};
  // A newer primary of this group exists: every shard withdraws from the
  // data plane (Shard::withdraw).
  std::atomic<bool> fenced{false};
  // Some shard has a replication sender (Engine::replicating).
  std::atomic<bool> replicating{false};
  // Slot moves (docs/MIGRATION.md): each slot's phase here, and the newest
  // placement epoch seen on a node link.
  std::array<std::atomic<u8>, kSlots> phases{};
  std::atomic<u64> epoch_hint{0};
  // Published gauges (include/snc/gauges.h, S0): one block per shard, and
  // the node's checkpoint.
  std::unique_ptr<ShardGauges[]> gauges;
  NodeCkptGauges node_ckpt;
};

// An Engine's state is owned by the Engine (Engine::state_), not by a file
// static: a process that exits with an Engine still running must not free
// state its shard threads still read (review PR #77 section 9;
// docs/S3_WRITEBACK.md section 10). Shard threads reach it only through
// the hooks set up in the constructor.

// ---- Lifecycle (S3.2, docs/ENGINE_LIFECYCLE.md) ---------------------------------------

namespace {
// The engine whose permit (or lifecycle step) this thread holds, if any.
// A permit nested inside another for the same engine is granted without
// counting again: the outer one already keeps stop() waiting, so nested
// Engine calls (seal_slot -> wait_lsn -> slot_report, promote ->
// update_placement, recovery -> checkpoint) can neither self-deadlock nor
// be refused half-way.
thread_local const Engine* tl_op_engine = nullptr;
std::atomic<bool> g_debug_park_ops{false};
std::atomic<u32> g_debug_ops_parked{0};
std::atomic<bool> g_debug_park_start{false};
std::atomic<bool> g_debug_start_parked{false};
}  // namespace

const char* lifecycle_name(Lifecycle l) {
  switch (l) {
    case Lifecycle::CONSTRUCTED: return "constructed";
    case Lifecycle::STARTING: return "starting";
    case Lifecycle::RUNNING: return "running";
    case Lifecycle::STOPPING: return "stopping";
    case Lifecycle::STOPPED: return "stopped";
  }
  return "?";
}

// An operation permit: granted only while RUNNING (or nested in one this
// thread holds, or inside start()/stop() on their own thread). While any
// is held, stop() waits before stopping the runtime. Refused: the
// operation returns its "not running" result at once and touches no shard.
class Engine::Op {
 public:
  explicit Op(const Engine& e) : e_(e), prev_(tl_op_engine) {
    if (prev_ == &e) {
      ok_ = true;  // nested (or the lifecycle owner's own call)
      return;
    }
    {
      std::lock_guard<std::mutex> g(e.life_mu_);
      if (e.life_ != Lifecycle::RUNNING) return;
      ++e.active_ops_;
    }
    ok_ = counted_ = true;
    tl_op_engine = &e;
    if (g_debug_park_ops.load(std::memory_order_relaxed)) {  // tests only
      g_debug_ops_parked.fetch_add(1);
      while (g_debug_park_ops.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      g_debug_ops_parked.fetch_sub(1);
    }
  }
  ~Op() {
    if (!counted_) return;
    tl_op_engine = prev_;
    std::lock_guard<std::mutex> g(e_.life_mu_);
    if (--e_.active_ops_ == 0) e_.life_cv_.notify_all();
  }
  Op(const Op&) = delete;
  Op& operator=(const Op&) = delete;
  explicit operator bool() const { return ok_; }

 private:
  const Engine& e_;
  const Engine* prev_;
  bool ok_ = false, counted_ = false;
};

// start() and stop() on their own thread: the internal calls they make
// (recovery, release of follow-ups, the final pause) pass the gate, since
// no other operation can be inside (STARTING admits none; STOPPING waited
// for all to leave).
class Engine::OwnerScope {
 public:
  explicit OwnerScope(const Engine& e) : prev_(tl_op_engine) { tl_op_engine = &e; }
  ~OwnerScope() { tl_op_engine = prev_; }
  OwnerScope(const OwnerScope&) = delete;
  OwnerScope& operator=(const OwnerScope&) = delete;

 private:
  const Engine* prev_;
};

void Engine::require_op() const {
  if (tl_op_engine != this) {
    std::fprintf(stderr, "snc: shard work submitted outside an Engine operation permit (lifecycle %s)\n",
                 lifecycle_name(lifecycle()));
    std::abort();
  }
}

void Engine::run_on_all_(const std::function<void(u16)>& fn) const {
  require_op();
  rt_->run_on_all(fn);
}

void Engine::submit_(u16 shard, std::function<void()> fn) const {
  require_op();
  rt_->submit(shard, std::move(fn));
}

Lifecycle Engine::lifecycle() const {
  std::lock_guard<std::mutex> g(life_mu_);
  return life_;
}

void Engine::debug_park_ops(bool on) { g_debug_park_ops.store(on); }
u32 Engine::debug_ops_parked() { return g_debug_ops_parked.load(); }
void Engine::debug_park_start(bool on) { g_debug_park_start.store(on); }
bool Engine::debug_start_parked() { return g_debug_start_parked.load(); }

Engine::Engine(const Config& cfg) : cfg_(cfg) {
  // Validation only (docs/S3_WRITEBACK.md section 8): SNC_CKPT_IO=legacy or
  // dsync overrides Config::ckpt_io_dsync, so a whole test suite can run on
  // either write path without editing every test's config. Announced on
  // stderr whenever it applies; anything else is refused.
  if (const char* e = std::getenv("SNC_CKPT_IO"); e != nullptr && e[0] != '\0') {
    const std::string v = e;
    if (v != "legacy" && v != "dsync") throw std::invalid_argument("SNC_CKPT_IO must be legacy or dsync");
    cfg_.ckpt_io_dsync = v == "dsync";
    std::fprintf(stderr, "snc: SNC_CKPT_IO=%s overrides ckpt_io\n", v.c_str());
  }
  register_session_type();
  register_slot_state_type();
  RuntimeConfig& rc = cfg_.runtime;
  if (rc.pages != PageMode::SMALL && rc.heap_reserve_bytes == 0) {
    // Room for the expected objects at 512 B each (docs/HUGEPAGES.md);
    // anything beyond it spills to normal pages.
    const u64 per_shard = cfg.expected_objects / (rc.n_shards ? rc.n_shards : 1);
    rc.heap_reserve_bytes = std::max<u64>(per_shard * 512, 256ull << 20);
  }
  {
    ckpt::PacerConfig p;
    p.walk_min_ns = cfg.ckpt_walk_min_ns;
    p.walk_max_ns = cfg.ckpt_walk_max_ns;
    p.walk_ceiling_ns = cfg.ckpt_walk_ceiling_ns;
    p.walk_step_ns = cfg.ckpt_walk_step_ns;
    p.update_ns = cfg.ckpt_pace_update_ns;
    p.wal_high_pct = cfg.ckpt_wal_high_pct;
    p.wal_low_pct = cfg.ckpt_wal_low_pct;
    p.backlog_high_pct = cfg.ckpt_backlog_high_pct;
    p.backlog_low_pct = cfg.ckpt_backlog_low_pct;
    p.target_ns = cfg.ckpt_target_ms * 1'000'000;
    p.max_age_ns = cfg.ckpt_max_age_ms * 1'000'000;
    p.warn_pct = cfg.ckpt_warn_pct;
    if (!ckpt::valid(p) || cfg.ckpt_walk_batch == 0 || cfg.ckpt_target_ms > (1ull << 40) ||
        cfg.ckpt_max_age_ms > (1ull << 40)) {
      throw std::invalid_argument(
          "checkpoint pacing: need 1 <= walk_min <= walk_max <= walk_ceiling <= 60 s, 1 <= step <= 60 s, "
          "update <= 60 s, batch >= 1, low < high for each pressure signal, 1 <= warn_pct <= 100, "
          "target <= max age");
    }
    if (cfg_.ckpt_write_window < 1 || cfg_.ckpt_write_window > kMaxCkptWriteWindow) {
      throw std::invalid_argument("checkpoint writeback: need 1 <= ckpt_write_window <= 64");
    }
  }
  shards_.assign(cfg.runtime.n_shards, nullptr);
  slots_ = SlotMap::make_default(cfg.runtime.n_shards);
  if (!cfg.cluster.empty()) {
    if (cfg.group_id >= cfg.cluster.size() || cfg.member >= cfg.cluster[cfg.group_id].members.size() ||
        cfg.cluster[cfg.group_id].n_shards != cfg.runtime.n_shards) {
      throw std::invalid_argument(
          "cluster: group_id or member out of range, or the group's shard count is not runtime.n_shards");
    }
    if (cfg.cluster_slots.empty()) {
      cluster_ = ClusterMap::make(cfg.cluster);
      cluster_.epoch = cfg.cluster_epoch;
    } else {
      if (cfg.cluster_slots.size() != kSlots) throw std::invalid_argument("cluster: cluster_slots needs every slot");
      std::array<u8, kSlots> a;
      std::copy(cfg.cluster_slots.begin(), cfg.cluster_slots.end(), a.begin());
      cluster_ = ClusterMap::assign(cfg.cluster, a, cfg.cluster_epoch);
    }
    if (!cfg.cluster_arrived.empty()) {
      if (cfg.cluster_arrived.size() != kSlots) throw std::invalid_argument("cluster: cluster_arrived needs every slot");
      std::copy(cfg.cluster_arrived.begin(), cfg.cluster_arrived.end(), cluster_.arrived.begin());
    }
  }
  state_ = std::make_unique<EngineState>();
  state_->final_stats.assign(cfg.runtime.n_shards, ShardStats{});
  state_->gauges = std::make_unique<ShardGauges[]>(cfg.runtime.n_shards);
  state_->hooks = EngineHooks{&cfg_, &shards_, &state_->final_stats, &slots_, 1, &cluster_, &state_->serving, &state_->fenced,
                               &state_->replicating, state_->phases.data(), &state_->epoch_hint,
                               state_->gauges.get()};
  ShardHooks h;
  h.init = hook_init;
  h.on_message = hook_message;
  if (cfg_.prefetch) h.on_batch = hook_batch;
  h.poll = hook_poll;
  h.quiesced = hook_quiesced;
  h.fini = hook_fini;
  h.user = &state_->hooks;
  rt_ = std::make_unique<NativeRuntime>(cfg_.runtime, h);
}

Engine::~Engine() {
  stop();
  rt_.reset();
  state_.reset();
}

void Engine::start() {
  {
    std::unique_lock<std::mutex> lk(life_mu_);
    if (life_ == Lifecycle::STARTING) {  // another thread is starting it
      life_cv_.wait(lk, [&] { return life_ != Lifecycle::STARTING; });
    }
    if (life_ == Lifecycle::RUNNING) return;
    if (life_ != Lifecycle::CONSTRUCTED) {
      throw std::logic_error(std::string("Engine::start: restart is not supported (state ") +
                             lifecycle_name(life_) + "); construct a new Engine");
    }
    life_ = Lifecycle::STARTING;
  }
  if (g_debug_park_start.load()) {  // tests only (L7)
    g_debug_start_parked.store(true);
    while (g_debug_park_start.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g_debug_start_parked.store(false);
  }
  // The starting thread owns the engine until RUNNING: its internal calls
  // (recovery's checkpoint, stats, release_emits) pass as nested.
  OwnerScope own(*this);
  try {
    start_();
  } catch (...) {
    // A failed start: if the runtime came up, the engine can still be
    // stopped (and must be, by stop() or the destructor); else it is as
    // constructed.
    std::lock_guard<std::mutex> g(life_mu_);
    life_ = running_ ? Lifecycle::RUNNING : Lifecycle::CONSTRUCTED;
    life_cv_.notify_all();
    throw;
  }
  std::lock_guard<std::mutex> g(life_mu_);
  life_ = Lifecycle::RUNNING;
  life_cv_.notify_all();
}

void Engine::start_() {
  if (cfg_.data_dir.empty()) {
    // In-memory mode: no WAL, every state is durable at once. Leases repeat
    // across processes, which is harmless because nothing persists.
    rt_->start();
    running_ = true;
    state_->serving = true;
    return;
  }
  recover();
  // Follow-ups were held while shards recovered: one shard's would otherwise
  // reach another still recovering, run there before its WAL is open, and
  // be acted on as durable though never logged (a lost credit).
  if (!cfg_.hold_emits) release_emits_();
  state_->serving = true;
  if (cfg_.placement_fetch && !cfg_.cluster.empty()) {
    watcher_stop_ = false;
    watcher_ = std::thread([this] { watch_placement(); });
  }
}

// Follow-ups queued on any shard or still in a ring, and emitted so far.
// `run` is Engine::run_on_all_ (the caller holds a permit).
template <class Run>
static void emit_totals(const Run& run, u64& in_flight, u64& emitted) {
  std::atomic<u64> queued{0}, sent{0}, received{0}, e{0};
  run([&](u16) {
    Shard* s = current_shard();
    queued += s->emits_in_flight();
    sent += s->emits_sent();
    received += s->emits_received();
    e += s->stats().emits;
  });
  in_flight = queued + (sent - received);
  emitted = e;
}

void Engine::release_emits() {
  Op op(*this);
  if (!op) return;  // not running: nothing to release
  release_emits_();
}

void Engine::release_emits_() {
  run_on_all_([](u16) { current_shard()->pause_emits(false); });
}

bool Engine::wait_emits(double timeout_s) {
  Op op(*this);
  if (!op) return false;
  const auto t0 = std::chrono::steady_clock::now();
  u64 last = ~u64{0};
  while (since(t0) < timeout_s) {
    // The shards are sampled one at a time, so a follow-up can move between
    // two of them unseen; two idle rounds with nothing emitted between them
    // cannot both miss it.
    u64 in_flight, emitted;
    emit_totals([this](const std::function<void(u16)>& f) { run_on_all_(f); }, in_flight, emitted);
    if (in_flight == 0 && emitted == last) return true;
    last = in_flight == 0 ? emitted : ~u64{0};
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

void Engine::stop() {
  // docs/ENGINE_LIFECYCLE.md. Operations already inside (a checkpoint, a
  // slot move, stats, ...) finish first: no waiter is abandoned, no shard
  // stops under one, a checkpoint commits fully (review PR #77 section 11,
  // PR #78 section 11). Shard-side I/O is then drained by the runtime's
  // quiesce: a shard stops only with no checkpoint active and no I/O in
  // flight.
  if (tl_op_engine == this) {
    std::fprintf(stderr, "snc: Engine::stop() called from inside an Engine operation (would wait for itself)\n");
    std::abort();
  }
  {
    std::unique_lock<std::mutex> lk(life_mu_);
    life_cv_.wait(lk, [&] { return life_ != Lifecycle::STARTING && life_ != Lifecycle::STOPPING; });
    if (life_ != Lifecycle::RUNNING) return;  // never started, or already stopped
    life_ = Lifecycle::STOPPING;  // from here no operation enters
  }
  // Threads of the engine's own that enter operations are told to finish
  // (the placement watcher's next refresh is refused anyway).
  watcher_stop_ = true;
  if (seed_) seed_->stop = true;
  {
    std::unique_lock<std::mutex> lk(life_mu_);
    life_cv_.wait(lk, [&] { return active_ops_ == 0; });
  }
  OwnerScope own(*this);  // this thread's internal shard calls below
  if (watcher_.joinable()) watcher_.join();
  if (seed_ && seed_->thread.joinable()) {
    seed_->thread.join();
    ::close(seed_->listen_fd);
  }
  // Stop dispatching follow-ups and let the ones already sent to another
  // shard come back, so no shard waits on a response from a stopped one.
  // Paused shards still run follow-ups that arrive; wait until every one
  // sent has arrived, so none is left in a ring.
  run_on_all_([](u16) { current_shard()->pause_emits(true); });
  while (true) {
    std::atomic<u64> sent{0}, received{0};
    run_on_all_([&](u16) {
      sent += current_shard()->emits_sent();
      received += current_shard()->emits_received();
    });
    if (sent == received) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  rt_->stop();  // shards quiesce first: WAL flushed, waiters released
  running_ = false;
  spares_.reset();  // after the shards: their WAL streams hold a pointer to it
  std::lock_guard<std::mutex> g(life_mu_);
  life_ = Lifecycle::STOPPED;
  life_cv_.notify_all();
}

// Shard s's replication settings, as standby or as primary.
static Shard::ReplicaConfig replica_config(const Engine::Config& cfg_, const SlotMap& slots,
                                           const Uuid& uuid_, u64 term, u16 s, bool standby,
                                           const std::atomic<bool>* gate) {
  const u16 n = cfg_.runtime.n_shards;
  const u32 crc = repl::slot_map_crc(slots);
  Shard::ReplicaConfig rc;
  if (standby) {
    rc.standby = true;
    rc.receiver.host = cfg_.standby_host;
    rc.receiver.port = static_cast<u16>((cfg_.standby_port ? cfg_.standby_port : cfg_.replica_port) + s);
    rc.receiver.uuid = uuid_;
    rc.receiver.shard = s;
    rc.receiver.n_shards = n;
    rc.receiver.slot_crc = crc;
    rc.receiver.term = term;
    rc.receiver.lease_ns = u64{cfg_.standby_read_lease_ms} * 1'000'000;
  } else if (!cfg_.replica_host.empty() && (!cfg_.standby || cfg_.standby_port != 0)) {
    rc.primary = true;
    rc.sender.host = cfg_.replica_host;
    rc.sender.port = static_cast<u16>(cfg_.replica_port + s);
    rc.sender.uuid = uuid_;
    rc.sender.shard = s;
    rc.sender.n_shards = n;
    rc.sender.slot_crc = crc;
    rc.sender.retain_bytes = cfg_.replica_retain_bytes;
    rc.sender.catchup_bytes_per_s = cfg_.replica_catchup_bytes_per_s;
    rc.sender.term = term;
    rc.sync = cfg_.replica_sync;
    rc.timeout_ns = u64{cfg_.replica_timeout_ms} * 1'000'000;
    rc.sender.ack_timeout_ns = rc.timeout_ns;
    if (cfg_.replica_degrade_gated) rc.degrade_gate = gate;
  }
  return rc;
}

void Engine::recover() {
  const auto t0 = std::chrono::steady_clock::now();
  const std::string& dir = cfg_.data_dir;
  const u16 n = cfg_.runtime.n_shards;
  recovery_ = RecoveryReport{};

  // 0. The role (docs/MIGRATION_FAIL_CLOSED.md 4.3). A standby says so on
  // disk before any replica record or seed can land here: from then on
  // this directory may hold a lossy prefix, until a promotion's start
  // barrier completes.
  make_dirs(dir);
  DirRole role = read_role(dir);
  using Adopt = Config::AdoptRole;
  // --adopt-role (docs/LEGACY_ROLE.md) is checked before anything is
  // written: it names the role of an untracked (legacy) directory, never
  // overrides a role on record, and a standby is adopted only to run as one.
  if (cfg_.adopt_role != Adopt::NONE) {
    const DirRole want = cfg_.adopt_role == Adopt::PRIMARY ? DirRole::PRIMARY : DirRole::STANDBY;
    if (want == DirRole::PRIMARY && cfg_.standby) {
      throw std::runtime_error("--adopt-role primary contradicts --standby");
    }
    if (want == DirRole::STANDBY && !cfg_.standby) {
      throw std::runtime_error("--adopt-role standby needs --standby (it adopts the directory to run as a standby)");
    }
    const std::optional<Superblock> pre = read_superblock(dir);
    if (!pre) throw std::runtime_error("--adopt-role: " + dir + " has no superblock (nothing to adopt)");
    const bool untracked = role == DirRole::MISSING && !(pre->flags & kSbRoleTracked);
    if (!untracked && role != want) {
      throw std::runtime_error(std::string("--adopt-role ") + role_name(want) + ": data directory " + dir +
                               " already has role " + role_name(role) + " (a recorded role is never overridden)");
    }
  }
  if (cfg_.standby) write_role(dir, DirRole::STANDBY);

  // 1. Superblock. A new standby may first fetch a seed from its primary.
  std::optional<Superblock> sb = read_superblock(dir);
  if (!sb && !cfg_.seed_from.empty()) {
    bool has_data = !list_streams(dir).empty() || ::access((dir + "/ckpt").c_str(), F_OK) == 0;
    if (has_data) {
      throw std::runtime_error("seed_from: " + dir + " has data but no superblock; clear it first");
    }
    make_dirs(dir);
    seed_fetch();
    sb = read_superblock(dir);
    if (!sb) storage_fatal("seed: superblock not readable after seeding");
  }
  if (!sb) {
    bool has_data = !list_streams(dir).empty() || ::access((dir + "/ckpt").c_str(), F_OK) == 0;
    if (!cfg_.init || has_data) {
      throw std::runtime_error("no valid SNC superblock in " + dir +
                               (has_data ? " (data present: refusing to initialize, spec C13)"
                                         : " (start with init to create it)"));
    }
    make_dirs(dir);
    const bool given = cfg_.init_uuid != Uuid{};
    sb = new_superblock(given ? cfg_.init_uuid : random_uuid());
    recovery_.fresh = true;
  }
  sb_ = *sb;
  std::memcpy(uuid_.data(), sb_.space_uuid, 16);

  // A start that may hold a replica prefix (the directory was last a
  // standby, or its role cannot be read and it has had one) runs the start
  // barrier: the placement read linearizably and adopted before any shard
  // starts (the stored map is not accepted), a re-seal before anything
  // serves, and only then role "primary". Without a control plane to ask
  // (no placement_sync) there are no moves, and nothing to re-seal.
  //
  // Legacy directories (docs/LEGACY_ROLE.md): a superblock from before
  // role files (no kSbRoleTracked) and no role file. Nothing on disk says
  // whether it was a primary's or a standby's (an older standby's
  // directory looks exactly like its primary's), so it never starts as a
  // primary on its own: the operator says which, once (--adopt-role), and
  // even then it starts only through the start barrier. A legacy
  // directory started with --standby has said so already (its role file
  // was written above).
  const bool legacy = !recovery_.fresh && role == DirRole::MISSING && !(sb_.flags & kSbRoleTracked) &&
                      !cfg_.standby;
  if (legacy) {
    if (cfg_.adopt_role != Adopt::PRIMARY) {
      throw std::runtime_error(
          "legacy data directory " + dir +
          ": no role file and no role tracking (written by a binary from before role files), so it may hold a "
          "standby's prefix and does not start as a primary. Start it once with --standby [--adopt-role standby] "
          "if it is a standby's, or with --adopt-role primary if it is its group's primary's own disk (checked "
          "against the group's other members, then started through the start barrier)");
    }
    // Refused if what can be seen says otherwise: another member of the
    // group serving or promoting, or one at a newer term of this space.
    if (cfg_.adopt_primary_check) cfg_.adopt_primary_check(uuid_, sb_.term);
    std::fprintf(stderr, "snc: legacy data directory adopted as a primary's; start barrier before serving\n");
  }
  std::optional<Placement> barrier;
  bool after_barrier = false;
  if (!cfg_.standby) {
    after_barrier = role == DirRole::STANDBY || role == DirRole::MALFORMED ||
                    (role == DirRole::MISSING && (sb_.flags & kSbRoleTracked)) || legacy;
    if (after_barrier) {
      std::fprintf(stderr, "snc: data directory role %s: start barrier before serving\n", role_name(role));
      if (cfg_.placement_sync && !cfg_.cluster.empty()) {
        Placement p;
        if (!cfg_.placement_sync(p)) {
          throw std::runtime_error(
              std::string("start barrier: this data directory ") +
              (legacy ? "has no recorded role (a legacy directory may hold a standby's lossy prefix)"
                      : "was last a standby (its records may be a lossy prefix)") +
              ", so the placement must be read linearizably before it serves, and the control plane did not answer");
        }
        if (p.epoch >= cluster_.epoch) {
          ClusterMap m = ClusterMap::assign(cluster_.groups, p.slots, p.epoch);
          m.arrived = p.arrived;
          cluster_ = std::move(m);
        }
        barrier = std::move(p);
      }
    } else if (role == DirRole::MISSING) {
      write_role(dir, DirRole::PRIMARY);  // a new primary's own disk
    }
  }
  // In the superblock written below. Not yet for a legacy directory being
  // adopted as a primary's: until its barrier has written role "primary",
  // a crash must leave it exactly as ambiguous as before (refused again
  // without --adopt-role), not "tracked, role missing".
  if (!legacy) sb_.flags |= kSbRoleTracked;

  // 2. Manifest and type catalog.
  std::optional<Manifest> m;
  if (sb_.active_ckpt != 0) {
    m = read_manifest(dir, sb_.active_ckpt);
    if (!m || std::memcmp(m->space_uuid.data(), uuid_.data(), 16) != 0) {
      storage_fatal("superblock names a checkpoint with no valid manifest");
    }
    for (const auto& t : m->types) {
      const TypeDescriptor* reg = registry().type(t.type_id);
      if (reg == nullptr || reg->schema_ver != t.schema_ver || t.name != reg->name) {
        throw std::runtime_error("type catalog mismatch for type " + std::to_string(t.type_id) +
                                 " (" + t.name + "): refusing to start");
      }
    }
    recovery_.checkpoint_seq = m->seq;
  }

  // 3. Slot map.
  if (m && m->n_shards == n) {
    slots_ = m->slots;
  } else {
    slots_ = SlotMap::make_default(n, m ? m->slots.epoch + 1 : 1);
    recovery_.remapped = m.has_value();
  }

  // 7 (done early, spec C12): reserve one mint lease per shard before any
  // shard can mint. Clustered with lease blocks, from the block this node
  // holds, or a new one (a standby mints nothing until it is promoted,
  // which takes a fresh block).
  if (cfg_.lease_blocks && !cfg_.standby) {
    if (sb_.lease_hi == 0 || sb_.next_lease < sb_.lease_lo || sb_.next_lease + n > sb_.lease_hi) {
      take_lease_block(n);
    }
  }
  state_->hooks.lease_base = sb_.next_lease;
  sb_.next_lease += n;
  ++sb_.generation;
  write_superblock(dir, sb_);
  recovery_.superblock_s = since(t0);

  // 4. Start workers; they hold no WAL until replay finishes.
  rt_->start();
  running_ = true;

  std::vector<u16> streams = list_streams(dir);
  for (u16 s : streams) adopt_spare(dir, s, uuid_);
  std::vector<u64> next_lsn(n, 1);
  std::vector<u32> tail_crc(n, 0);  // crc of each stream's last record, for replication
  bool orphans = false;
  auto from_lsn = [&](u16 s) -> u64 { return (m && s < m->start_lsn.size()) ? m->start_lsn[s] : 1; };
  // Observability (S0): where a recovery replays each stream from, until
  // the next checkpoint commits (a remap commits one at step 8).
  if (!recovery_.remapped) {
    for (u16 s = 0; s < n; ++s) state_->gauges[s].set_committed_start_lsn(from_lsn(s));
  }

  if (m && !recovery_.remapped) {
    // Same slot map as when the files were written: part s and stream s hold
    // only objects shard s owns, so every shard reads its own files on its
    // own thread and installs directly, all shards in parallel.
    auto own = [&](u16 s) {
      return [this, s](const WalRecord& r, const u8* payload) {
        if (slots_.owner_of(r.id) != s) storage_fatal("record in the wrong shard's stream");
        current_shard()->install(r, payload);
      };
    };
    std::vector<u64> images(n, 0);
    auto t1 = std::chrono::steady_clock::now();
    {
      run_on_all_([&](u16 s) {
        for (const auto& p : m->parts) {
          if (p.stream != s) continue;
          images[s] = scan_part(dir, m->seq, s, uuid_, own(s));
          if (images[s] != p.objects) storage_fatal("checkpoint part object count mismatch");
        }
      });
    }
    for (u64 i : images) recovery_.images_loaded += i;
    recovery_.checkpoint_load_s = since(t1);

    auto t2 = std::chrono::steady_clock::now();
    std::vector<StreamScan> scans(n);
    run_on_all_([&](u16 s) { scans[s] = scan_stream(dir, s, from_lsn(s), uuid_, own(s)); });
    for (u16 s = 0; s < n; ++s) {
      recovery_.wal_records_replayed += scans[s].records;
      recovery_.torn_tails += scans[s].torn_tails;
      next_lsn[s] = std::max(scans[s].last_lsn + 1, from_lsn(s));
      if (scans[s].last_lsn + 1 == next_lsn[s]) tail_crc[s] = scans[s].last_crc;
    }
    for (u16 s : streams) {
      if (s >= n) storage_fatal("WAL stream without a shard under an unchanged slot map");
    }
    recovery_.wal_replay_s = since(t2);
  } else {
    // Shard count changed, or no checkpoint yet (so the slot map the files
    // were written under is unknown): records are read on the control thread
    // and routed to their owners in batches. A remap is followed by a
    // checkpoint before READY (step 8), so this path is rare.
    struct Batch {
      std::vector<u8> bytes;
      u32 count = 0;
    };
    std::vector<Batch> batches(n);
    auto flush_batch = [&](u16 owner) {
      if (batches[owner].count == 0) return;
      auto data = std::make_shared<std::vector<u8>>(std::move(batches[owner].bytes));
      batches[owner] = Batch{};
      submit_(owner, [data] {
        Shard* s = current_shard();
        std::size_t off = 0;
        while (off < data->size()) {
          WalRecord r;
          std::memcpy(&r, data->data() + off, sizeof(r));
          s->install(r, data->data() + off + sizeof(r));
          off += r.total_len;
        }
      });
    };
    auto route = [&](const WalRecord& r, const u8* payload) {
      u16 owner = slots_.owner_of(r.id);
      Batch& b = batches[owner];
      std::size_t at = b.bytes.size();
      b.bytes.resize(at + r.total_len);
      std::memcpy(b.bytes.data() + at, &r, sizeof(r));
      std::memcpy(b.bytes.data() + at + sizeof(r), payload, r.total_len - sizeof(r));
      if (++b.count >= 4096) flush_batch(owner);
    };
    auto barrier = [&] {
      for (u16 s = 0; s < n; ++s) flush_batch(s);
      run_on_all_([](u16) {});  // submit queues are FIFO: everything before is installed
    };

    auto t1 = std::chrono::steady_clock::now();
    if (m) {
      for (const auto& p : m->parts) {
        u64 got = scan_part(dir, m->seq, p.stream, uuid_, route);
        if (got != p.objects) storage_fatal("checkpoint part object count mismatch");
        recovery_.images_loaded += got;
      }
    }
    barrier();
    recovery_.checkpoint_load_s = since(t1);

    auto t2 = std::chrono::steady_clock::now();
    for (u16 s : streams) {
      StreamScan sc = scan_stream(dir, s, from_lsn(s), uuid_, route);
      recovery_.wal_records_replayed += sc.records;
      recovery_.torn_tails += sc.torn_tails;
      if (s < n) {
        next_lsn[s] = std::max(sc.last_lsn + 1, from_lsn(s));
      } else if (sc.last_lsn > 0) {
        orphans = true;  // shard count shrank: this stream has no writer any more
      }
    }
    barrier();
    recovery_.wal_replay_s = since(t2);
  }

  // 6. Index rebuild and new WAL segments (never append to a possibly torn one).
  auto t3 = std::chrono::steady_clock::now();
  WalConfig wc;
  wc.data_dir = dir;
  wc.uuid = uuid_;
  wc.segment_bytes = cfg_.wal_segment_bytes;
  wc.group_commit_us = cfg_.group_commit_us;
  wc.sync = cfg_.wal_sync;
  wc.direct = cfg_.wal_direct;
  spares_ = std::make_unique<SegmentPreparer>(dir, n, cfg_.wal_segment_bytes, cfg_.wal_direct);
  wc.spares = spares_.get();

  // IDs from leases this superblock never handed out can arrive by
  // replication (a standby being promoted): mint above all of them.
  {
    std::atomic<u64> max_seen{0};
    run_on_all_([&](u16) {
      u64 m = current_shard()->max_lease_seen();
      u64 cur = max_seen.load();
      while (m > cur && !max_seen.compare_exchange_weak(cur, m)) {}
    });
    // A block is disjoint from every lease anyone else was given: nothing
    // minted elsewhere can be in it.
    if (max_seen.load() >= state_->hooks.lease_base && sb_.lease_hi == 0) {
      u64 base = max_seen.load() + 1;
      sb_.next_lease = base + n;
      ++sb_.generation;
      write_superblock(dir, sb_);
      run_on_all_([&](u16 s) { current_shard()->set_lease(base + s); });
    }
  }

  if (!cfg_.replica_host.empty() || cfg_.standby) {
    run_on_all_([&](u16 s) { current_shard()->set_replication(replica_config(cfg_, slots_, uuid_, sb_.term, s, cfg_.standby, &degrade_ok_)); });
  }
  run_on_all_([&](u16 s) {
    if (tail_crc[s] != 0) current_shard()->set_wal_tail(next_lsn[s] - 1, tail_crc[s]);
    if (barrier) reseal_from(*barrier);
    current_shard()->finish_recovery(wc, next_lsn[s]);
  });
  recovery_.index_rebuild_s = since(t3);
  if (after_barrier) {
    finish_barrier(barrier.has_value());
    if (!(sb_.flags & kSbRoleTracked)) {  // an adopted legacy primary: role "primary" is on disk now
      sb_.flags |= kSbRoleTracked;
      ++sb_.generation;
      write_superblock(dir, sb_);
    }
  }

  // 8. A changed slot map or orphaned streams need a checkpoint before READY
  // (spec C9) so no object's history spans two streams.
  if (recovery_.remapped || orphans || recovery_.fresh) {
    auto t4 = std::chrono::steady_clock::now();
    checkpoint();
    recovery_.final_checkpoint_s = since(t4);
  }
  for (u16 s = 0; s < n; ++s) recovery_.objects += stats(s).objects;
  recovery_.total_s = since(t0);

  seed_ = std::make_unique<Seeding>();
  seed_->wal_pin.assign(n, 0);
  if (cfg_.seed_port != 0) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg_.seed_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, 4) != 0) {
      throw std::runtime_error("cannot listen for seeds on port " + std::to_string(cfg_.seed_port));
    }
    seed_->listen_fd = fd;
    seed_->thread = std::thread([this] { serve_seed(); });
  }
}

// The move states in which the start barrier seals the slot at its source:
// SEALED and STAGED (the seal must still hold), and PREPARED: the driver
// seals next (the same record; SEAL is idempotent for a move), and a
// starting node that the commit gate cannot see yet (an offline start has
// no admin port, so no ROLE m) must not serve a slot a move may be about
// to take (review of the slice, finding 3). An abort unseals it as usual.
static bool barrier_seals(PlacementMove::State s) {
  return s == PlacementMove::State::PREPARED || s == PlacementMove::State::SEALED ||
         s == PlacementMove::State::STAGED;
}

// On a shard's thread: the start barrier's re-seal, if `p` says this group
// is moving a slot away (PREPARED, SEALED or STAGED) and this shard owns it.
void Engine::reseal_from(const Placement& p) {
  const PlacementMove& m = p.move;
  if (barrier_seals(m.state) &&
      m.from == cfg_.group_id) {
    current_shard()->set_reseal(m.slot, m.id, m.to, m.epoch);
  }
}

// The end of the start barrier: every re-seal durable here, then (with a
// control plane) the placement read once more, in case the move went on
// while this node recovered, and only then role "primary".
void Engine::finish_barrier(bool synced) {
  const u16 n = cfg_.runtime.n_shards;
  auto wait_local = [&](u16 s, u64 lsn) {
    while (!on_shard(s, [&] { return current_shard()->lsn_local_durable(lsn); })) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  };
  for (u16 s = 0; s < n; ++s) wait_local(s, on_shard(s, [] { return current_shard()->reseal_lsn(); }));
  if (synced) {
    Placement p;
    if (!cfg_.placement_sync(p)) {
      throw std::runtime_error("start barrier: the control plane stopped answering before this node could serve");
    }
    update_placement(p.slots, p.epoch, &p.arrived);
    const PlacementMove& m = p.move;
    if (barrier_seals(m.state) &&
        m.from == cfg_.group_id) {
      const u16 s = slots_.owner[m.slot];
      const u64 lsn = on_shard(s, [&] {
        u64 l = 0;
        current_shard()->reseal_slot(m.slot, m.id, m.to, m.epoch, l);
        return l;
      });
      wait_local(s, lsn);
    }
  }
  if (cfg_.debug_barrier_hold_ms) {
    std::fprintf(stderr, "debug: start barrier held %u ms before role primary\n", cfg_.debug_barrier_hold_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.debug_barrier_hold_ms));
  }
  if (cfg_.debug_barrier_crash) {
    std::fprintf(stderr, "debug: crash before role primary\n");
    std::fflush(stderr);
    ::_exit(9);
  }
  write_role(cfg_.data_dir, DirRole::PRIMARY);
  std::fprintf(stderr, "snc: start barrier done: serving as a primary\n");
}

double Engine::promote() {
  Op op(*this);
  if (!op || !running_ || !cfg_.standby || cfg_.data_dir.empty()) {
    throw std::logic_error("promote: not a running standby");
  }
  const auto t0 = std::chrono::steady_clock::now();
  const u16 n = cfg_.runtime.n_shards;
  // The start barrier (docs/MIGRATION_FAIL_CLOSED.md 5a), first: this
  // standby's prefix may be lossy, so it serves nothing before it has read
  // the placement linearizably. If it cannot, nothing has changed.
  std::optional<Placement> barrier;
  if (cfg_.placement_sync && !cfg_.cluster.empty()) {
    Placement p;
    if (!cfg_.placement_sync(p)) {
      throw std::runtime_error("promote: no linearizable placement read (start barrier)");
    }
    barrier = std::move(p);
  }
  // 0. With lease blocks, a fresh one first: if the control plane does not
  // give one, nothing has changed yet.
  u64 block_lo = 0, block_hi = 0;
  if (cfg_.lease_blocks && (!cfg_.lease_blocks(n, block_lo, block_hi) || block_hi < block_lo + n)) {
    throw std::runtime_error("promote: no mint lease block from the control plane");
  }
  // The synced map, adopted (a commit that already happened makes the
  // slot foreign here; its arrival decides the proof).
  if (barrier) update_placement(barrier->slots, barrier->epoch, &barrier->arrived);
  // 1. Take no more records, and wait until all installed are durable: the
  // follow-ups resumed below act on them.
  // The listeners stay, refusing the old primary's term (sb_.term + 1 is
  // the term this promotion starts, below).
  const u64 new_term = sb_.term + 1;
  run_on_all_([&](u16) { current_shard()->stop_standby(new_term); });
  while (true) {
    std::atomic<bool> settled{true};
    run_on_all_([&](u16) {
      if (!current_shard()->standby_settled()) settled = false;
    });
    if (settled) break;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  // 2. The new term, and leases above every ID the old primary minted,
  // durable before anything is served.
  std::atomic<u64> max_seen{0};
  run_on_all_([&](u16) {
    u64 m = current_shard()->max_lease_seen();
    u64 cur = max_seen.load();
    while (m > cur && !max_seen.compare_exchange_weak(cur, m)) {}
  });
  const u64 base = block_hi != 0 ? block_lo : std::max(sb_.next_lease, max_seen.load() + 1);
  {
    std::unique_lock<std::mutex> g;
    if (seed_) g = std::unique_lock<std::mutex>(seed_->mu);
    if (block_hi != 0) {
      sb_.lease_lo = block_lo;
      sb_.lease_hi = block_hi;
    }
    sb_.next_lease = base + n;
    ++sb_.term;
    ++sb_.generation;
    write_superblock(cfg_.data_dir, sb_);
  }
  // 3. Serve. Follow-ups wait until every shard is a primary (as after
  // recovery): one sent to a shard still a standby would run outside its log.
  cfg_.standby = false;
  run_on_all_([&](u16 s) {
    Shard* sh = current_shard();
    sh->pause_emits(true);
    sh->set_lease(base + s);
    if (barrier) reseal_from(*barrier);
    sh->promote(replica_config(cfg_, slots_, uuid_, sb_.term, s, false, &degrade_ok_), cfg_.data_dir);
  });
  release_emits();
  finish_barrier(false);  // the re-seal durable here, then role "primary"
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

CheckpointReport Engine::checkpoint() {
  CheckpointReport rep;
  // The permit is held to the end: stop() waits for it. Not running
  // (constructed, stopping, stopped): nothing is started, seq 0.
  Op op(*this);
  if (!op) return rep;
  std::lock_guard<std::mutex> one_at_a_time(checkpoint_mu_);
  if (cfg_.data_dir.empty() || !running_) return rep;
  const auto t0 = std::chrono::steady_clock::now();
  const std::string& dir = cfg_.data_dir;
  const u16 n = cfg_.runtime.n_shards;
  u64 seq = sb_.active_ckpt + 1;
  rep.seq = seq;
  // Observability (S0): the node's phase, read by HEALTH without waiting.
  NodeCkptGauges& ng = state_->node_ckpt;
  const u64 g0 = sri_now_ns();
  ng.seq.store(seq, std::memory_order_relaxed);
  ng.started_ns.store(g0, std::memory_order_relaxed);
  ng.phase.store(static_cast<u64>(NodeCkptPhase::SHARDS), std::memory_order_relaxed);
  make_dirs(ckpt_dir(dir, seq));

  // Expired client sessions go first (a standby gets the deletes from its
  // primary).
  if (cfg_.session_ttl_s != 0 && !cfg_.standby) {
    const u32 now_s = static_cast<u32>(::time(nullptr));
    run_on_all_([&](u16) { current_shard()->expire_sessions(now_s, cfg_.session_ttl_s); });
  }

  // Steps 1 to 3: every shard writes and syncs its part.
  std::vector<Shard::CheckpointResult> results(n);
  std::latch done(n);
  for (u16 s = 0; s < n; ++s) {
    submit_(s, [&, s] {
      current_shard()->begin_checkpoint(dir, uuid_, seq, [&, s](const Shard::CheckpointResult& r) {
        results[s] = r;
        done.count_down();
      });
    });
  }
  done.wait();
  // Tests only (S3 T7): every part is written and synced; nothing names
  // this checkpoint yet. A crash here must recover from the previous one.
  if (g_debug_ckpt_hold_commit.load()) {
    g_debug_ckpt_commit_reached.store(true);
    while (g_debug_ckpt_hold_commit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g_debug_ckpt_commit_reached.store(false);
  }
  const u64 g1 = sri_now_ns();
  ng.phase.store(static_cast<u64>(NodeCkptPhase::COMMIT), std::memory_order_relaxed);
  fsync_dir(ckpt_dir(dir, seq));

  // Step 4: manifest, then the superblock switch (spec C6).
  Manifest m;
  m.seq = seq;
  m.space_uuid = uuid_;
  m.n_shards = n;
  m.slots = slots_;
  m.start_lsn.resize(n);
  for (u16 s = 0; s < n; ++s) {
    m.start_lsn[s] = results[s].start_lsn;
    m.parts.push_back(Manifest::Part{s, results[s].objects, results[s].bytes});
    rep.objects += results[s].objects;
    rep.bytes += results[s].bytes;
  }
  for (const auto& t : registry().types()) m.types.push_back({t.type_id, t.schema_ver, t.name});
  write_manifest(dir, m);
  // Replication: the first LSN each shard's standby may still need, and
  // seeds in progress (docs/REPLICATION.md). WAL from there is kept.
  std::vector<u64> keep_from(n, 0);
  run_on_all_([&](u16 s) { keep_from[s] = current_shard()->repl_retain_from(); });
  std::unique_lock<std::mutex> seed_lock;
  if (seed_) {
    seed_lock = std::unique_lock<std::mutex>(seed_->mu);
    const bool expired = std::chrono::steady_clock::now() > seed_->wal_pin_until;
    for (u16 s = 0; s < n; ++s) {
      u64& pin = seed_->wal_pin[s];
      // Released once the standby is past it.
      if (pin != 0 && (expired || keep_from[s] > pin)) pin = 0;
      if (pin != 0 && (keep_from[s] == 0 || pin < keep_from[s])) keep_from[s] = pin;
    }
  }
  u64 old = sb_.active_ckpt;
  sb_.active_ckpt = seq;
  // A standby has installed IDs minted under its primary's leases; once the
  // checkpoint drops their records, only the superblock remembers them.
  {
    std::atomic<u64> max_seen{0};
    run_on_all_([&](u16) {
      u64 m = current_shard()->max_lease_seen();
      u64 cur = max_seen.load();
      while (m > cur && !max_seen.compare_exchange_weak(cur, m)) {}
    });
    if (sb_.lease_hi == 0) sb_.next_lease = std::max(sb_.next_lease, max_seen.load() + 1);
  }
  ++sb_.generation;
  write_superblock(dir, sb_);
  // Committed: a recovery now replays each stream from its start LSN.
  for (u16 s = 0; s < n; ++s) state_->gauges[s].set_committed_start_lsn(m.start_lsn[s]);
  const u64 g2 = sri_now_ns();
  ng.phase.store(static_cast<u64>(NodeCkptPhase::RECLAIM), std::memory_order_relaxed);

  // Step 5: only now delete what the new checkpoint covers (spec C10),
  // except a checkpoint a seed is still sending.
  if (old != 0) {
    if (seed_ && seed_->ckpt_pins.count(old)) seed_->deferred.push_back(old);
    else remove_tree(ckpt_dir(dir, old));
  }
  if (seed_lock.owns_lock()) seed_lock.unlock();
  for (u16 s : list_streams(dir)) {
    if (s >= n) {
      for (u64 first : list_segments(dir, s)) ::unlink(segment_path(dir, s, first).c_str());
      ::unlink(spare_path(dir, s).c_str());
      ::rmdir(stream_dir(dir, s).c_str());
      ++rep.segments_deleted;
      continue;
    }
    std::vector<u64> segs = list_segments(dir, s);
    // Segments the checkpoint covers but a standby may still need, kept up
    // to a budget; beyond it the oldest go and that standby needs a seed.
    u64 keep = m.start_lsn[s];
    if (keep_from[s] != 0) keep = std::min(keep, keep_from[s]);
    std::size_t held = 0;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
      if (segs[i + 1] <= m.start_lsn[s] && segs[i + 1] > keep) ++held;
    }
    const std::size_t budget =
        static_cast<std::size_t>(cfg_.replica_retain_disk_bytes / cfg_.wal_segment_bytes);
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
      // Segment i holds LSNs below segs[i+1]; drop it if all are below start_lsn.
      if (segs[i + 1] > m.start_lsn[s]) continue;
      bool needed = segs[i + 1] > keep;
      if (needed && held > budget) {
        needed = false;
        --held;
      }
      if (!needed) {
        ::unlink(segment_path(dir, s, segs[i]).c_str());
        ++rep.segments_deleted;
      }
    }
    fsync_dir(stream_dir(dir, s));
  }
  fsync_dir(dir + "/ckpt");
  fsync_dir(dir + "/wal");
  rep.seconds = since(t0);
  const u64 g3 = sri_now_ns();
  ng.last_shards_us.store((g1 - g0) / 1000, std::memory_order_relaxed);
  ng.last_commit_us.store((g2 - g1) / 1000, std::memory_order_relaxed);
  ng.last_reclaim_us.store((g3 - g2) / 1000, std::memory_order_relaxed);
  ng.last_total_us.store((g3 - g0) / 1000, std::memory_order_relaxed);
  ng.completed.fetch_add(1, std::memory_order_relaxed);
  ng.started_ns.store(0, std::memory_order_relaxed);
  ng.phase.store(static_cast<u64>(NodeCkptPhase::IDLE), std::memory_order_relaxed);
  return rep;
}

// ---- Slot moves (docs/MIGRATION.md) ------------------------------------------------

template <class F>
auto Engine::on_shard(u16 shard, F fn) -> decltype(fn()) {
  using R = decltype(fn());
  require_op();
  std::optional<R> out;
  std::latch done(1);
  submit_(shard, [&] {
    out.emplace(fn());
    done.count_down();
  });
  done.wait();
  return std::move(*out);
}

template <typename F>
void Engine::on_shard_steps(u16 shard, F step) {
  std::latch back(1);
  require_op();  // the caller's permit covers the whole chain
  std::function<void()> task = [&] {
    if (step()) {
      back.count_down();  // the last touch of this frame
      return;
    }
    rt_->submit(shard, task);  // on the shard thread: the caller's permit still holds
  };
  rt_->submit(shard, task);
  back.wait();
}

bool Engine::update_placement(const std::array<u8, kSlots>& slots, u64 epoch,
                              const std::array<SlotArrival, kSlots>* arrived) {
  Op op(*this);
  if (!op || !running_ || cfg_.cluster.empty()) return false;
  std::lock_guard<std::mutex> g(map_mu_);
  const ClusterMap* cur = map_.load();
  // Epoch only: arrivals change with an epoch (a commit, or the backfill,
  // which raises it so nodes adopt it).
  if (epoch <= cur->epoch) return false;
  auto next = std::make_unique<ClusterMap>(ClusterMap::assign(cur->groups, slots, epoch));
  if (arrived) next->arrived = *arrived;
  const ClusterMap* p = next.get();
  maps_.push_back(std::move(next));
  map_.store(p);
  std::fprintf(stderr, "snc: placement epoch %llu adopted (was %llu)\n", (unsigned long long)epoch,
               (unsigned long long)cur->epoch);
  run_on_all_([&](u16) { current_shard()->set_map(p); });
  run_on_all_([&](u16) { current_shard()->map_changed(); });
  return true;
}

u64 Engine::epoch_seen() const {
  const u64 h = state_ ? state_->epoch_hint.load() : 0;
  return h > map_.load()->epoch ? h : 0;
}

// Asks the control plane every placement_poll_ms, and at once when a node
// link shows a newer epoch: invariant 14, the hot path only reads the map
// it caches; this thread is what refreshes it.
void Engine::watch_placement() {
  using clock = std::chrono::steady_clock;
  auto last = clock::now() - std::chrono::hours(1);
  while (!watcher_stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto since_last = clock::now() - last;
    const bool due = since_last >= std::chrono::milliseconds(cfg_.placement_poll_ms);
    const bool hinted = epoch_seen() != 0 && since_last >= std::chrono::milliseconds(50);
    if (!due && !hinted) continue;
    last = clock::now();
    refresh_placement();
  }
}

bool Engine::refresh_placement() {
  if (!cfg_.placement_fetch) return false;
  Placement p;
  return cfg_.placement_fetch(p) && update_placement(p.slots, p.epoch, &p.arrived);
}

Status Engine::wait_lsn(u16 shard, u64 lsn, double timeout_s) {
  const auto t0 = std::chrono::steady_clock::now();
  while (true) {
    const int r = on_shard(shard, [&] {
      Shard* s = current_shard();
      if (s->withdrawn()) return 2;
      return s->lsn_durable(lsn) ? 1 : 0;
    });
    if (r == 1) return Status::OK;
    if (r == 2) return Status::E_FENCED;  // will never be acknowledged here
    if (since(t0) > timeout_s) return Status::E_OVERLOADED;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

SlotReport Engine::slot_report(u16 slot) {
  Op op(*this);
  if (!op || !running_) return SlotReport{};
  return on_shard(slots_.owner[slot], [&] { return current_shard()->slot_report(slot); });
}

std::vector<SlotReport> Engine::slot_states() {
  std::vector<SlotReport> out;
  Op op(*this);
  if (!op || !running_) return out;
  std::mutex mu;
  run_on_all_([&](u16) {
    std::vector<SlotReport> v;
    current_shard()->slot_states(v);
    std::lock_guard<std::mutex> g(mu);
    out.insert(out.end(), v.begin(), v.end());
  });
  std::sort(out.begin(), out.end(), [](const SlotReport& a, const SlotReport& b) { return a.slot < b.slot; });
  return out;
}

Status Engine::seal_slot(u16 slot, u64 move, u16 to, u64 epoch, SlotReport& out, double timeout_s) {
  Op op(*this);
  if (!op) return Status::E_SHUTTING_DOWN;
  const u16 sh = slots_.owner[slot];
  u64 lsn = 0;
  const u64 t0 = trace::enabled() ? trace::now_ns() : 0;
  Status st = on_shard(sh, [&] { return current_shard()->seal_slot(slot, move, to, epoch, lsn); });
  const u64 t1 = trace::enabled() ? trace::now_ns() : 0;
  if (st == Status::OK) st = wait_lsn(sh, lsn, timeout_s);
  SNC_TRACE("seal slot=%u move=%llx on_shard_ms=%.1f wait_lsn_ms=%.1f status=%s", slot, (unsigned long long)move,
            (t1 - t0) / 1e6, trace::enabled() ? (trace::now_ns() - t1) / 1e6 : 0.0, status_name(st));
  out = slot_report(slot);
  return st;
}

Status Engine::unseal_slot(u16 slot, u64 move, SlotReport& out, double timeout_s) {
  Op op(*this);
  if (!op) return Status::E_SHUTTING_DOWN;
  const u16 sh = slots_.owner[slot];
  u64 lsn = 0;
  Status st = on_shard(sh, [&] { return current_shard()->unseal_slot(slot, move, lsn); });
  if (st == Status::OK) st = wait_lsn(sh, lsn, timeout_s);
  out = slot_report(slot);
  return st;
}

Status Engine::export_slot(u16 slot, u64 move, bool final, const std::vector<SlotObject>& have,
                           std::vector<SlotObject>& out, u64& count, u64& digest) {
  Op op(*this);
  if (!op) return Status::E_SHUTTING_DOWN;
  const u16 sh = slots_.owner[slot];
  if (final) {
    // The seal is durable before its state is shown to anyone.
    const SlotReport r = slot_report(slot);
    if (r.phase == static_cast<u8>(SlotPhase::SEALED) && r.move_id == move && !r.durable) {
      u64 lsn = 0;
      on_shard(sh, [&] { return current_shard()->seal_slot(slot, move, r.group, r.epoch, lsn); });
      if (const Status w = wait_lsn(sh, lsn, 10); w != Status::OK) return w;
    }
  }
  const u64 t0 = trace::enabled() ? trace::now_ns() : 0;
  const Status st =
      on_shard(sh, [&] { return current_shard()->export_slot(slot, move, final, have, out, count, digest); });
  SNC_TRACE("export slot=%u move=%llx final=%d have=%zu out=%zu on_shard_ms=%.1f status=%s", slot,
            (unsigned long long)move, final ? 1 : 0, have.size(), out.size(),
            trace::enabled() ? (trace::now_ns() - t0) / 1e6 : 0.0, status_name(st));
  return st;
}

Status Engine::slot_objects(u16 slot, std::vector<SlotObject>& out) {
  Op op(*this);
  if (!op) return Status::E_SHUTTING_DOWN;
  on_shard(slots_.owner[slot], [&] {
    current_shard()->slot_objects(slot, false, out);
    return 0;
  });
  return Status::OK;
}

// S1 (docs/MIGRATION_STAGING.md 3.2): the request runs as a chain of
// shard tasks driven from this (control) thread; between two units the
// shard runs whatever else is queued. Every exit releases the chain's
// registration. The chain is volatile: what it wrote is ordinary staged
// records, inert in a foreign slot, found again by a retry's HAVE.
Status Engine::stage_slot(u16 slot, u64 move, u16 from, u64 epoch, bool final, const std::vector<SlotObject>& objs,
                          u64 count, u64 digest, SlotReport& out, double timeout_s) {
  Op op(*this);
  if (!op) return Status::E_SHUTTING_DOWN;
  const u16 sh = slots_.owner[slot];
  StageChain c;
  c.slot = slot;
  c.move = move;
  c.from = from;
  c.epoch = epoch;
  c.final = final;
  c.objs = &objs;
  c.count = count;
  c.digest = digest;
  // Sized here, off the shard: growing it entry by entry would rehash
  // inside a unit.
  if (final) c.listed.reserve(objs.size());
  if (cfg_.move_units) {
    c.unit_ns = u64{cfg_.move_unit_us} * 1000;
    c.unit_bytes = cfg_.move_unit_bytes;
    c.window_bytes = cfg_.move_window_bytes;
  }
  const u64 t0 = trace::now_ns();
  SNC_TRACE("stage_begin slot=%u move=%llx final=%d objs=%zu shard=%u", slot, (unsigned long long)move, final ? 1 : 0,
            objs.size(), sh);
  Status st;
  u64 window_waits = 0;
  if (!cfg_.move_units) {
    st = on_shard(sh, [&] {
      Shard* s = current_shard();
      Status r = s->stage_admit(c);
      if (r == Status::OK && c.phase != StageChain::DONE) r = s->stage_unit(c);
      s->stage_release(c);
      return r;
    });
  } else {
    if (final) debug_stage_point(StageChain::ADMIT, 0);
    st = on_shard(sh, [&] { return current_shard()->stage_admit(c); });
    // A run of units: each unit, when the chain goes on, queues the next
    // behind whatever the shard has waiting (its rings, I/O and injected
    // tasks run in between), with no round trip to this thread. The run
    // comes back here when the chain is done or refused, when the window
    // is full, or after every unit while a debug stop is armed.
    auto run_units = [&]() -> Status {
      Status out = Status::OK;
      on_shard_steps(sh, [&] {
        const u8 ph = c.phase;
        const Status r = current_shard()->stage_unit(c);
        SNC_TRACE("stage_unit slot=%u move=%llx final=%d phase=%s unit_us=%u put=%llu dropped=%llu pending=%llu "
                  "status=%s",
                  slot, (unsigned long long)move, final ? 1 : 0, StageChain::phase_name(ph),
                  c.unit_us.empty() ? 0u : c.unit_us.back(), (unsigned long long)c.put,
                  (unsigned long long)c.dropped, (unsigned long long)c.pending_bytes, status_name(r));
        out = r;
        return r != Status::OK || c.phase == StageChain::DONE || c.pending_bytes > c.window_bytes ||
               (final && debug_stage_armed_.load(std::memory_order_relaxed));
      });
      return out;
    };
    try {
      while (st == Status::OK && c.phase != StageChain::DONE) {
        if (final) debug_stage_point(c.phase, static_cast<u32>(c.phase_units[c.phase]));
        ++c.runs;
        st = run_units();
        // The window: wait (off the shard) for the oldest appended record
        // while too much of what the chain wrote is not yet durable.
        if (st == Status::OK && c.pending_bytes > c.window_bytes && !c.pending.empty()) {
          // Until enough of it is durable to bring it back under the window
          // (the oldest records first: durability is in LSN order).
          u64 left = c.pending_bytes;
          u64 lsn = 0;
          for (const auto& [l, b] : c.pending) {
            lsn = std::max(lsn, l);
            left -= b;
            if (left <= c.window_bytes) break;
          }
          ++window_waits;
          st = wait_lsn(sh, lsn, timeout_s);
        }
      }
    } catch (...) {
      // Every exit clears the registration (docs/MIGRATION_STAGING.md 3.2).
      on_shard(sh, [&] {
        current_shard()->stage_release(c);
        return 0;
      });
      throw;
    }
    on_shard(sh, [&] {
      current_shard()->stage_release(c);
      return 0;
    });
  }
  const u64 t1 = trace::now_ns();
  if (st == Status::OK) st = wait_lsn(sh, c.lsn, timeout_s);
  if (final && st == Status::OK) debug_stage_point(StageChain::DONE, 0);  // "reply"
  if (final) {
    // A stop point this chain never reached (its phase took fewer units)
    // must not fire in a later, unrelated move.
    std::lock_guard<std::mutex> g(stage_mu_);
    debug_stage_ = DebugStageStop{};
    debug_stage_armed_ = false;
  }
  // The reply's report walks the whole map: in bounded steps too (an
  // unchanged slot is the common case; a slot that keeps changing under
  // the walk falls back to one task).
  u64 report_units = 0, report_restarts = 0;
  if (cfg_.move_units) {
    Shard::ReportWalk w;
    w.slot = slot;
    const u64 unit_ns = c.unit_ns;
    bool walked = false;
    on_shard_steps(sh, [&] {
      ++report_units;
      walked = current_shard()->slot_report_unit(w, out, unit_ns);
      return walked || w.restarts > 8;
    });
    if (!walked) out = slot_report(slot);  // the slot kept changing under the walk
    report_restarts = w.restarts;
  } else {
    out = slot_report(slot);
  }
  const u64 t2 = trace::now_ns();
  if (final && c.units != 0) {
    std::vector<u32> u = c.unit_us;
    std::sort(u.begin(), u.end());
    auto pct = [&](double p) { return u.empty() ? 0.0 : u[std::min(u.size() - 1, static_cast<std::size_t>(p * u.size()))] / 1e3; };
    char line[640];
    std::snprintf(line, sizeof(line),
                  "stage_chain slot=%u move=%llx objs=%zu status=%s units=%llu (check %llu put %llu scan %llu drop %llu "
                  "stage %llu) unit_ms p50=%.3f p95=%.3f p99=%.3f max=%.3f (%s) put=%llu dropped=%llu bytes=%llu "
                  "window_hw=%llu window_waits=%llu scan_restarts=%llu shard_ms=%.1f chain_ms=%.1f wait_ms=%.1f "
                  "listed_entries=%zu gone=%zu report_units=%llu report_restarts=%llu runs=%llu",
                  slot, (unsigned long long)move, objs.size(), status_name(st), (unsigned long long)c.units,
                  (unsigned long long)c.phase_units[StageChain::CHECK], (unsigned long long)c.phase_units[StageChain::PUT],
                  (unsigned long long)c.phase_units[StageChain::SCAN], (unsigned long long)c.phase_units[StageChain::DROP],
                  (unsigned long long)c.phase_units[StageChain::STAGE], pct(0.5), pct(0.95), pct(0.99),
                  c.max_unit_ns / 1e6, StageChain::phase_name(c.max_unit_phase), (unsigned long long)c.put, (unsigned long long)c.dropped,
                  (unsigned long long)c.bytes, (unsigned long long)c.pending_hw, (unsigned long long)window_waits,
                  (unsigned long long)c.scan_restarts, c.shard_ns / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6,
                  c.listed.size(), c.gone.size(), (unsigned long long)report_units,
                  (unsigned long long)report_restarts, (unsigned long long)c.runs);
    std::fprintf(stderr, "snc: %s\n", line);
    std::lock_guard<std::mutex> g(stage_mu_);
    last_stage_ = line;
  }
  SNC_TRACE("stage_end slot=%u move=%llx final=%d objs=%zu units=%llu max_unit_ms=%.3f chain_ms=%.1f wait_lsn_ms=%.1f "
            "index_ms=%.1f wal_ms=%.1f status=%s",
            slot, (unsigned long long)move, final ? 1 : 0, objs.size(), (unsigned long long)c.units, c.max_unit_ns / 1e6,
            (t1 - t0) / 1e6, (t2 - t1) / 1e6, c.index_ns / 1e6, c.log_ns / 1e6, status_name(st));
  return st;
}

void Engine::debug_stage_stop(int phase, u32 unit, bool crash, u32 hold_ms) {
  std::lock_guard<std::mutex> g(stage_mu_);
  debug_stage_ = DebugStageStop{phase, unit, hold_ms, crash};
  debug_stage_armed_ = true;
}

std::string Engine::last_stage_report() const {
  std::lock_guard<std::mutex> g(stage_mu_);
  return last_stage_;
}

void Engine::debug_stage_point(int phase, u32 unit) {
  DebugStageStop d;
  {
    std::lock_guard<std::mutex> g(stage_mu_);
    if (debug_stage_.phase != phase || debug_stage_.unit != unit) return;
    d = debug_stage_;
    debug_stage_ = DebugStageStop{};  // once
    debug_stage_armed_ = false;
  }
  const char* name = phase == StageChain::DONE ? "reply" : StageChain::phase_name(static_cast<u8>(phase));
  if (d.crash) {
    std::fprintf(stderr, "debug: stage chain crash before %s unit %u\n", name, unit);
    std::fflush(stderr);
    _exit(9);
  }
  std::fprintf(stderr, "debug: stage chain held %u ms before %s unit %u\n", d.hold_ms, name, unit);
  std::this_thread::sleep_for(std::chrono::milliseconds(d.hold_ms));
}

void Engine::take_lease_block(u16 n) {
  u64 lo = 0, hi = 0;
  if (!cfg_.lease_blocks(n, lo, hi) || hi < lo + n) {
    throw std::runtime_error("no mint lease block from the control plane, and the one held is used up");
  }
  sb_.lease_lo = lo;
  sb_.lease_hi = hi;
  sb_.next_lease = lo;
}

void Engine::withdraw() {
  if (state_ && running_) state_->fenced.store(true);
}

bool Engine::fenced() const { return state_ && state_->fenced.load(); }

void Engine::debug_pause_replication(bool on) { repl::set_debug_pause(on); }
void Engine::debug_hold_wal(u16 shard, bool on) { debug_hold_wal_flush(shard, on); }
void Engine::debug_ckpt_walk_limit(u64 limit) { snc::debug_ckpt_walk_limit(limit); }
void Engine::debug_ckpt_hold_writes(bool on) { snc::debug_ckpt_hold_writes(on); }
void Engine::debug_ckpt_defer_writes(bool on) { snc::debug_ckpt_defer_writes(on); }
void Engine::debug_ckpt_release_writes(u32 n, bool newest_first) { snc::debug_ckpt_release_writes(n, newest_first); }
void Engine::debug_ckpt_fail_write(u64 index, int mode) { snc::debug_ckpt_fail_write(index, mode); }
void Engine::debug_ckpt_hold_sync(bool on) { snc::debug_ckpt_hold_sync(on); }
void Engine::debug_ckpt_hold_commit(bool on) { g_debug_ckpt_hold_commit.store(on); }
bool Engine::debug_ckpt_commit_reached() { return g_debug_ckpt_commit_reached.load(); }
void Engine::debug_force_sync_io(bool on) { sri_debug_force_sync_io(on); }

bool Engine::replicating() const { return state_ && state_->replicating.load(); }

Engine::ReplStatus Engine::replication_status() const {
  ReplStatus st;
  st.shards = cfg_.runtime.n_shards;
  Op op(*this);
  if (!op || !running_) return st;
  std::mutex mu;
  std::latch done(st.shards);
  for (u16 s = 0; s < st.shards; ++s) {
    submit_(s, [&] {
      Shard::ReplSummary r = current_shard()->repl_summary();
      {
        std::lock_guard<std::mutex> g(mu);
        st.role = std::max(st.role, r.role);
        st.live += r.live;
        st.degraded += r.degraded;
        st.blocked += r.blocked;
        st.ready += r.ready;
        st.regressed += r.regressed;
      }
      done.count_down();
    });
  }
  done.wait();
  return st;
}

GaugeSnapshot Engine::gauges(u16 shard) const {
  if (!state_ || !state_->gauges || shard >= cfg_.runtime.n_shards) return GaugeSnapshot{};
  return state_->gauges[shard].read();
}

const NodeCkptGauges& Engine::checkpoint_gauges() const {
  static const NodeCkptGauges none;
  return state_ ? state_->node_ckpt : none;
}

ShardStats Engine::stats(u16 shard) const {
  // Not running: once STOPPED, the values each shard left at stop (its
  // worker wrote them before the runtime joined it, and STOPPED is set
  // under life_mu_ after that, which this read takes); while STARTING or
  // STOPPING, zeros (the workers may be writing them). Never blocks.
  Op op(*this);
  if (!op || !running_) {
    if (lifecycle() == Lifecycle::STOPPED) return state_->final_stats.at(shard);
    return ShardStats{};
  }
  // Read on the owning thread: shard state is single-writer.
  ShardStats out;
  std::latch done(1);
  submit_(shard, [&] {
    out = current_shard()->stats();
    done.count_down();
  });
  done.wait();
  return out;
}

void Engine::for_each_object(u16 shard, const std::function<void(const ObjectHeader&)>& fn) {
  Op op(*this);
  if (!op) return;  // not running: nothing visited
  std::latch done(1);
  submit_(shard, [&] {
    current_shard()->for_each_object(fn);
    done.count_down();
  });
  done.wait();
}

void Engine::for_each_served_object(u16 shard, const std::function<void(const ObjectHeader&)>& fn) {
  Op op(*this);
  if (!op) return;  // not running: nothing visited
  std::latch done(1);
  submit_(shard, [&] {
    Shard* sh = current_shard();
    if (!sh->is_standby() && !sh->withdrawn()) {
      sh->for_each_object([&](const ObjectHeader& h) {
        if (sh->serving(slot_of(h.id))) fn(h);
      });
    }
    done.count_down();
  });
  done.wait();
}

}  // namespace snc
