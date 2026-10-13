// Engine: wires the core into the runtime and owns the control-thread side
// of persistence: superblock, recovery and checkpoint orchestration (spec
// section 12, recovery steps 1 to 9).
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <latch>
#include <mutex>
#include <stdexcept>

#include "seed.h"
#include "shard.h"

namespace snc {

namespace {

struct EngineHooks {
  Engine::Config* cfg;
  std::vector<Shard*>* shards;
  std::vector<ShardStats>* final_stats;
  const SlotMap* slots;
  u64 lease_base;
};

void hook_init(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  u16 id = sri_shard_id();
  auto* s = new Shard(id, *h->slots, h->lease_base + id, h->cfg->expected_objects,
                      h->cfg->runtime.pages);
  s->set_mint_local(h->cfg->mint_local);
  s->set_batch_remote(h->cfg->batch_remote);
  set_current_shard(s);
  (*h->shards)[id] = s;
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
  if (h->cfg->app_poll) busy |= h->cfg->app_poll(h->cfg->app_user, sri_shard_id());
  current_shard()->flush_batches();  // after the app issued this iteration's requests
  return busy;
}

bool hook_quiesced(void*) { return current_shard()->quiesced(); }

void hook_fini(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  u16 id = sri_shard_id();
  (*h->final_stats)[id] = current_shard()->stats();
  (*h->shards)[id] = nullptr;
  delete current_shard();
  set_current_shard(nullptr);
}

double since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
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
};

static std::unique_ptr<EngineState> g_state;

Engine::Engine(const Config& cfg) : cfg_(cfg) {
  RuntimeConfig& rc = cfg_.runtime;
  if (rc.pages != PageMode::SMALL && rc.heap_reserve_bytes == 0) {
    // Room for the expected objects at 512 B each (docs/HUGEPAGES.md);
    // anything beyond it spills to normal pages.
    const u64 per_shard = cfg.expected_objects / (rc.n_shards ? rc.n_shards : 1);
    rc.heap_reserve_bytes = std::max<u64>(per_shard * 512, 256ull << 20);
  }
  shards_.assign(cfg.runtime.n_shards, nullptr);
  slots_ = SlotMap::make_default(cfg.runtime.n_shards);
  g_state = std::make_unique<EngineState>();
  g_state->final_stats.assign(cfg.runtime.n_shards, ShardStats{});
  g_state->hooks = EngineHooks{&cfg_, &shards_, &g_state->final_stats, &slots_, 1};
  ShardHooks h;
  h.init = hook_init;
  h.on_message = hook_message;
  if (cfg_.prefetch) h.on_batch = hook_batch;
  h.poll = hook_poll;
  h.quiesced = hook_quiesced;
  h.fini = hook_fini;
  h.user = &g_state->hooks;
  rt_ = std::make_unique<NativeRuntime>(cfg_.runtime, h);
}

Engine::~Engine() {
  stop();
  rt_.reset();
  g_state.reset();
}

void Engine::start() {
  if (running_) return;
  if (cfg_.data_dir.empty()) {
    // In-memory mode: no WAL, every state is durable at once. Leases repeat
    // across processes, which is harmless because nothing persists.
    rt_->start();
    running_ = true;
    return;
  }
  recover();
}

// Follow-ups queued on any shard or still in a ring, and emitted so far.
static void emit_totals(NativeRuntime& rt, u64& in_flight, u64& emitted) {
  std::atomic<u64> queued{0}, sent{0}, received{0}, e{0};
  rt.run_on_all([&](u16) {
    Shard* s = current_shard();
    queued += s->emits_in_flight();
    sent += s->emits_sent();
    received += s->emits_received();
    e += s->stats().emits;
  });
  in_flight = queued + (sent - received);
  emitted = e;
}

bool Engine::wait_emits(double timeout_s) {
  const auto t0 = std::chrono::steady_clock::now();
  u64 last = ~u64{0};
  while (since(t0) < timeout_s) {
    // The shards are sampled one at a time, so a follow-up can move between
    // two of them unseen; two idle rounds with nothing emitted between them
    // cannot both miss it.
    u64 in_flight, emitted;
    emit_totals(*rt_, in_flight, emitted);
    if (in_flight == 0 && emitted == last) return true;
    last = in_flight == 0 ? emitted : ~u64{0};
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

void Engine::stop() {
  if (!running_) return;
  if (seed_ && seed_->thread.joinable()) {
    seed_->stop = true;
    seed_->thread.join();
    ::close(seed_->listen_fd);
  }
  // Stop dispatching follow-ups and let the ones already sent to another
  // shard come back, so no shard waits on a response from a stopped one.
  // Paused shards still run follow-ups that arrive; wait until every one
  // sent has arrived, so none is left in a ring.
  rt_->run_on_all([](u16) { current_shard()->pause_emits(true); });
  while (true) {
    std::atomic<u64> sent{0}, received{0};
    rt_->run_on_all([&](u16) {
      sent += current_shard()->emits_sent();
      received += current_shard()->emits_received();
    });
    if (sent == received) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  rt_->stop();  // shards quiesce first: WAL flushed, waiters released
  running_ = false;
  spares_.reset();  // after the shards: their WAL streams hold a pointer to it
}

void Engine::recover() {
  const auto t0 = std::chrono::steady_clock::now();
  const std::string& dir = cfg_.data_dir;
  const u16 n = cfg_.runtime.n_shards;
  recovery_ = RecoveryReport{};

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
  // shard can mint.
  g_state->hooks.lease_base = sb_.next_lease;
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
      rt_->run_on_all([&](u16 s) {
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
    rt_->run_on_all([&](u16 s) { scans[s] = scan_stream(dir, s, from_lsn(s), uuid_, own(s)); });
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
      rt_->submit(owner, [data] {
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
      rt_->run_on_all([](u16) {});  // submit queues are FIFO: everything before is installed
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
    rt_->run_on_all([&](u16) {
      u64 m = current_shard()->max_lease_seen();
      u64 cur = max_seen.load();
      while (m > cur && !max_seen.compare_exchange_weak(cur, m)) {}
    });
    if (max_seen.load() >= g_state->hooks.lease_base) {
      u64 base = max_seen.load() + 1;
      sb_.next_lease = base + n;
      ++sb_.generation;
      write_superblock(dir, sb_);
      rt_->run_on_all([&](u16 s) { current_shard()->set_lease(base + s); });
    }
  }

  if (!cfg_.replica_host.empty() || cfg_.standby) {
    const u32 crc = repl::slot_map_crc(slots_);
    rt_->run_on_all([&](u16 s) {
      Shard::ReplicaConfig rc;
      if (cfg_.standby) {
        rc.standby = true;
        rc.receiver.host = cfg_.standby_host;
        rc.receiver.port = static_cast<u16>(cfg_.replica_port + s);
        rc.receiver.uuid = uuid_;
        rc.receiver.shard = s;
        rc.receiver.n_shards = n;
        rc.receiver.slot_crc = crc;
      } else {
        rc.primary = true;
        rc.sender.host = cfg_.replica_host;
        rc.sender.port = static_cast<u16>(cfg_.replica_port + s);
        rc.sender.uuid = uuid_;
        rc.sender.shard = s;
        rc.sender.n_shards = n;
        rc.sender.slot_crc = crc;
        rc.sender.retain_bytes = cfg_.replica_retain_bytes;
        rc.sync = cfg_.replica_sync;
        rc.timeout_ns = u64{cfg_.replica_timeout_ms} * 1'000'000;
      }
      current_shard()->set_replication(rc);
    });
  }
  rt_->run_on_all([&](u16 s) {
    if (tail_crc[s] != 0) current_shard()->set_wal_tail(next_lsn[s] - 1, tail_crc[s]);
    current_shard()->finish_recovery(wc, next_lsn[s]);
  });
  recovery_.index_rebuild_s = since(t3);

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

CheckpointReport Engine::checkpoint() {
  CheckpointReport rep;
  if (cfg_.data_dir.empty() || !running_) return rep;
  const auto t0 = std::chrono::steady_clock::now();
  const std::string& dir = cfg_.data_dir;
  const u16 n = cfg_.runtime.n_shards;
  u64 seq = sb_.active_ckpt + 1;
  rep.seq = seq;
  make_dirs(ckpt_dir(dir, seq));

  // Steps 1 to 3: every shard writes and syncs its part.
  std::vector<Shard::CheckpointResult> results(n);
  std::latch done(n);
  for (u16 s = 0; s < n; ++s) {
    rt_->submit(s, [&, s] {
      current_shard()->begin_checkpoint(dir, uuid_, seq, [&, s](const Shard::CheckpointResult& r) {
        results[s] = r;
        done.count_down();
      });
    });
  }
  done.wait();
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
  rt_->run_on_all([&](u16 s) { keep_from[s] = current_shard()->repl_retain_from(); });
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
    rt_->run_on_all([&](u16) {
      u64 m = current_shard()->max_lease_seen();
      u64 cur = max_seen.load();
      while (m > cur && !max_seen.compare_exchange_weak(cur, m)) {}
    });
    sb_.next_lease = std::max(sb_.next_lease, max_seen.load() + 1);
  }
  ++sb_.generation;
  write_superblock(dir, sb_);

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
  return rep;
}

ShardStats Engine::stats(u16 shard) const {
  if (!running_) return g_state->final_stats.at(shard);
  // Read on the owning thread: shard state is single-writer.
  ShardStats out;
  std::latch done(1);
  rt_->submit(shard, [&] {
    out = current_shard()->stats();
    done.count_down();
  });
  done.wait();
  return out;
}

void Engine::for_each_object(u16 shard, const std::function<void(const ObjectHeader&)>& fn) {
  std::latch done(1);
  rt_->submit(shard, [&] {
    current_shard()->for_each_object(fn);
    done.count_down();
  });
  done.wait();
}

}  // namespace snc
