// SNC application API (spec section 6). All calls are asynchronous, are made
// from a shard worker thread, and complete on that same shard. A completion
// may run before the call returns when the target is local (the local fast
// path has no queue), so callers must not issue new work from inside a
// completion recursively without bound.
#pragma once

#include "snc/native_runtime.h"
#include "snc/local_map.h"
#include "snc/registry.h"
#include "snc/slot_map.h"
#include "snc/storage.h"

namespace snc {

enum class Durability : u8 { DURABLE = 0, APPLIED = 1 };

struct ExecOptions {
  u64 expected_version = 0;  // 0 = unconditional
  Durability durability = Durability::DURABLE;
  bool allow_undurable = false;  // reads: do not wait for durability
  // Safe retries (docs/RETRIES.md): a client's mutations and creates on one
  // shard numbered 1, 2, 3...; one already applied is answered from the
  // client's session instead of run again. 0 = unsequenced.
  u64 client_id = 0;
  u64 seq = 0;
};

struct ExecResult {
  Status status = Status::OK;
  u16 app_code = 0;   // set when status == E_APP
  u64 version = 0;    // object version after the op (current version for reads)
  bool durable = false;
  bool replayed = false;  // sequenced: answered from the session, not run again
  ObjectID id;        // target, or the new ID for create
  ByteView result;    // valid until the completion returns
};

using Completion = void (*)(void* ctx, const ExecResult& r);

void execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts, Completion done,
             void* ctx);
void create(u32 type_id, ByteView init_args, const ExecOptions& opts, Completion done, void* ctx);
// Placement (docs/TRANSFERS.md): like create, but the new ID falls in the
// same slot as `near`, so the two objects share an owner under every slot
// map, including after a shard-count change. Objects changed together
// (OpCtx::local) belong together. Mints about 4096 IDs per call to find one.
void create_near(const ObjectID& near, u32 type_id, ByteView init_args, const ExecOptions& opts,
                 Completion done, void* ctx);
void remove(ObjectID target, const ExecOptions& opts, Completion done, void* ctx);

// Secondary index lookup (spec section 11): asks every shard, merges, and
// returns at most `max_results` IDs. Skips objects whose state is not yet
// durable. Not a snapshot across shards.
using IndexCompletion = void (*)(void* ctx, Status s, const ObjectID* ids, u32 n);
void index_lookup(u32 index_id, ByteView key, u32 max_results, IndexCompletion done, void* ctx);

// Hint: `ids` are about to be executed here. Warms the map entries and
// objects of the ones this shard owns, so a batch pays its cache misses in
// parallel rather than one op at a time. Ignores IDs owned elsewhere.
void prefetch(const ObjectID* ids, u32 n);

// Owner of an ID under the current slot map. Valid on a worker thread.
u16 owner_of(const ObjectID& id);
// The slot map this shard routes with. Valid on a worker thread.
const SlotMap& slot_map();

// Handler-facing context (spec section 6, "What apply may do").
class OpCtx {
 public:
  // The object; may move after resize().
  ObjectHeader* obj() { return obj_; }
  const TypeDescriptor& type() const { return *type_; }

  // Payload length apply may resize to (at least the current capacity).
  u32 reserved_len() const { return reserved_len_; }
  // validate only: reserve payload size for apply. Checks max_size and the
  // shard memory limit, so apply's resize cannot fail.
  Status reserve(u32 new_len);
  // apply only: resize to at most the reserved length. Returns the payload.
  u8* resize(u32 new_len);
  // validate: reject with an application code (returns E_APP).
  Status reject(u16 app_code) {
    app_code_ = app_code;
    return Status::E_APP;
  }
  // Durable follow-up (docs/TRANSFERS.md): run `op` with `args` on `target`
  // once this op's WAL record is durable. If the target is missing or not
  // the op's type, run `fail_op` (if nonzero) on this object instead, with
  // the same args and the target as OpCtx::source(). Results are discarded
  // and follow-ups are not logged:
  // anything that must survive a crash belongs in the object, and the
  // type's resume hook re-emits it after recovery. MUTATE ops (apply only)
  // and resume hooks may emit; args are at most kMaxEmitArgs bytes so a
  // follow-up to another shard fits in one ring message.
  static constexpr u32 kMaxEmitArgs = 16;
  void emit(const ObjectID& target, OpID op, ByteView args, OpID fail_op = 0);
  // The object whose emit is running this op; null for a client's op. Ops
  // meant only as follow-ups should reject a null source.
  const ObjectID& source() const { return source_; }

  // Another object this shard owns, for a MUTATE op that changes two
  // objects at once (docs/TRANSFERS.md). Null unless `id` is a live object
  // of type `type_id` owned by this shard, other than obj(). validate may
  // read it. When apply calls it, the engine treats the object as changed:
  // apply may modify its payload in place (not its length or index key),
  // and the engine bumps its version and logs it in one atomic WAL group
  // with this op's record. At most one such object per op.
  ObjectHeader* local(const ObjectID& id, u32 type_id);

  // Write the op result (at most the op's max_result). Inline: it runs on
  // nearly every op and its results are a few bytes.
  void set_result(const void* data, u32 len) {
    if (len > max_result_) {
      result_overflow_ = true;
      return;
    }
    std::memcpy(result_buf_, data, len);
    result_len_ = len;
  }

 private:
  friend class Shard;
  ObjectHeader* obj_ = nullptr;
  const TypeDescriptor* type_ = nullptr;
  const OpDescriptor* op_ = nullptr;
  u32 reserved_len_ = 0;
  u16 app_code_ = 0;
  ObjectID source_;
  ObjectHeader* peer_ = nullptr;  // set when apply calls local()
  bool applying_ = false;
  u8* result_buf_ = nullptr;
  u32 result_len_ = 0;
  u32 max_result_ = 0;
  bool result_overflow_ = false;
};

// Per-shard counters.
struct ShardStats {
  u64 ops_local = 0;        // executed inline on the entry shard
  u64 ops_remote = 0;       // dispatched to another shard
  u64 ops_served = 0;       // executed here on behalf of another shard
  u64 creates = 0;
  u64 deletes = 0;
  u64 overloaded = 0;
  u64 objects = 0;          // live objects owned
  u64 local_map_bytes = 0;
  u64 heap_live_bytes = 0;
  PageMode map_pages = PageMode::SMALL;   // pages the local map got
  PageMode heap_pages = PageMode::SMALL;  // pages the heap arena got (SMALL = no arena)
  u64 heap_arena_bytes = 0;
  u64 heap_arena_misses = 0;              // allocations that spilled to normal pages
  u64 index_lookups_served = 0;
  u64 deferred_completions = 0;  // completions that waited for durability
  u64 emits = 0;                 // follow-ups emitted (OpCtx::emit)
  u64 emits_sent = 0;            // ... sent to another shard
  u64 emits_received = 0;        // ... received from another shard
  u64 emits_failed_over = 0;     // follow-ups that ran their fail_op instead
  u64 emits_in_flight = 0;       // queued here or waiting for durability
  WalStream::Stats wal;
  u64 wal_durable_lsn = 0;
  // Replication (docs/REPLICATION.md).
  u8 repl_role = 0;            // 0 none, 1 primary, 2 standby
  bool repl_live = false;      // link to the other side is up
  bool repl_degraded = false;  // primary, sync: continuing without the standby
  bool repl_blocked = false;   // primary, sync: past the timeout, waiting for leave to degrade
  u64 stale_reads = 0;
  u64 replayed = 0;            // sequenced requests answered from a client session         // standby: reads refused for want of a lease (E_STALE)
  bool repl_lost = false;      // primary: the standby must be reseeded
  u64 repl_acked_lsn = 0;      // primary: the standby is durable up to here
  u64 repl_bytes = 0;          // primary: bytes shipped
  u64 repl_retained_bytes = 0; // primary: unacked records kept in memory
  u64 repl_records = 0;        // standby: records applied
  u64 repl_connects = 0;       // connections made (primary) or accepted (standby)
  u64 repl_disk_bytes = 0;     // primary: shipped from WAL files (catch-up)
  bool wal_direct = false;
};

struct RecoveryReport {
  bool fresh = false;          // initialized a new data directory
  bool remapped = false;       // shard count changed: slot map rebuilt
  u64 checkpoint_seq = 0;      // checkpoint loaded (0 = none)
  u64 images_loaded = 0;
  u64 wal_records_replayed = 0;
  u64 torn_tails = 0;          // WAL segments that ended at a crash tail
  u64 objects = 0;
  double superblock_s = 0, checkpoint_load_s = 0, wal_replay_s = 0, index_rebuild_s = 0,
         final_checkpoint_s = 0, total_s = 0;
};

struct CheckpointReport {
  u64 seq = 0;
  u64 objects = 0;
  u64 bytes = 0;
  u64 segments_deleted = 0;
  double seconds = 0;
};

// Wires the SNC core into a backend. One Engine per process.
class Engine {
 public:
  struct Config {
    RuntimeConfig runtime;
    u64 expected_objects = 0;  // pre-sizes local maps to avoid rehash
    // Optional application work run once per loop iteration on every shard
    // (load drivers, ingress). Returns true if it did work.
    bool (*app_poll)(void* user, u16 shard) = nullptr;
    void* app_user = nullptr;
    // create() mints only IDs that hash to the calling shard, so every create
    // runs on its entry shard (docs/ROUTING.md). Off: IDs land anywhere.
    bool mint_local = false;
    // Prefetch the targets of each batch of requests drained from a ring.
    bool prefetch = true;
    // Requests one shard sends another within a loop iteration travel as
    // one message, answered in the same buffer (docs/CROSSCORE.md).
    bool batch_remote = true;

    // Persistence. Empty data_dir = in-memory only: every state reports durable.
    std::string data_dir;
    bool init = false;            // create the data directory if it has no superblock
    u64 wal_segment_bytes = 64ull << 20;
    u32 group_commit_us = 50;
    bool wal_sync = true;         // false = wal_sync none (development only)
    bool wal_direct = true;
    // With init and no superblock yet: create the space with this UUID
    // instead of a random one (all zero = random). A standby is created
    // with its primary's UUID.
    Uuid init_uuid{};

    // Replication (docs/REPLICATION.md). Needs a data_dir; both sides need
    // the same shard count.
    // Primary: ship the WAL to the standby at replica_host, shard i to port
    // replica_port + i.
    std::string replica_host;
    u16 replica_port = 0;
    bool replica_sync = true;           // durable acks wait for the standby
    u32 replica_timeout_ms = 2000;      // sync: continue without it after this (0 = never)
    u64 replica_retain_bytes = 1ull << 30;  // per shard, unacked records kept for catch-up
    u64 replica_catchup_bytes_per_s = 0;    // per shard, catch-up from disk (0: no limit)
    // Standby: listen on standby_host, shard i on port replica_port + i;
    // install and log what the primary sends; serve no ops (E_STANDBY).
    // Promote by stopping it and starting a primary on the same data_dir.
    bool standby = false;
    std::string standby_host = "0.0.0.0";
    // Standby: listen here instead (0 = replica_port). A standby may then
    // also set replica_host/replica_port: where it ships its WAL once
    // promoted (Engine::promote).
    u16 standby_port = 0;
    // Primary: keep WAL segments a checkpoint covers while the standby may
    // still need them, up to this much per shard beyond the checkpoint.
    u64 replica_retain_disk_bytes = 16ull << 30;
    // Primary: serve seeds on this port (0 = off). A seed is the active
    // checkpoint's files; the standby then catches up from the WAL.
    u16 seed_port = 0;
    // Standby: with no superblock in data_dir, first fetch a seed from this
    // primary ("host:port") instead of starting empty.
    std::string seed_from;
    // Primary, sync: past replica_timeout_ms, continue without the standby
    // only once allow_degrade(true) is called (by whoever holds the
    // failover decision, e.g. a witness). Until then durable writes wait.
    bool replica_degrade_gated = false;
    // Standby: serve READ ops under read leases of this length from a sync
    // primary (0 = serve no ops). Must not exceed the primary's
    // replica_timeout_ms, which a lease can stretch.
    u32 standby_read_lease_ms = 0;
  };

  explicit Engine(const Config& cfg);
  ~Engine();

  // With a data_dir: recovers (superblock, checkpoint, WAL replay, index
  // rebuild, new mint leases) before returning. Aborts on corruption.
  void start();
  // Flushes every WAL stream and waits for pending durability, then stops.
  // Follow-ups still queued are not run; their objects' resume hooks re-emit
  // them at the next start (in memory mode they are lost with the state).
  void stop();
  // Blocks until no shard has a follow-up queued or in flight, e.g. until
  // every transfer has settled. Control path; returns false on timeout.
  bool wait_emits(double timeout_s = 30);

  // Fuzzy checkpoint of every shard (spec section 12). Control path; blocks.
  CheckpointReport checkpoint();

  // A standby becomes the primary in place (docs/REPLICATION.md): it stops
  // taking records, waits until what it has is durable, starts a new term
  // (written to the superblock before anything is served: a primary of an
  // older term is refused by any standby that knows the new one), rebuilds
  // its index, resumes follow-ups, and ships its WAL to replica_host if set.
  // Returns the seconds it took. Control path; blocks.
  double promote();
  u64 term() const { return sb_.term; }
  // With replica_degrade_gated: let sync shards continue without their
  // standby (true), or stop letting them (false; a shard already degraded
  // stays so until its standby catches up).
  void allow_degrade(bool on) { degrade_ok_.store(on); }

  // Replication state summed over shards, cheap enough to poll.
  struct ReplStatus {
    u8 role = 0;           // 0 none, 1 primary, 2 standby
    u16 live = 0;          // shards whose link is up
    u16 degraded = 0;      // primary: shards continuing without the standby
    u16 blocked = 0;       // primary: shards waiting for leave to degrade
    u16 shards = 0;
  };
  ReplStatus replication_status() const;
  const RecoveryReport& recovery() const { return recovery_; }
  // The space's UUID (after start, with a data_dir).
  const Uuid& space_uuid() const { return uuid_; }

  NativeRuntime& runtime() { return *rt_; }
  u16 shard_count() const { return cfg_.runtime.n_shards; }
  // Running: read on the shard's thread (control path). Stopped: final values.
  ShardStats stats(u16 shard) const;

  // Control path: visit every live object owned by `shard` on its thread.
  // Intended for tests and checks, not the hot path.
  void for_each_object(u16 shard, const std::function<void(const ObjectHeader&)>& fn);

 private:
  Config cfg_;
  std::unique_ptr<NativeRuntime> rt_;
  std::vector<class Shard*> shards_;
  bool running_ = false;

  // Persistence state (control thread only).
  void recover();
  // Seeding standbys (src/core/seed.cpp).
  struct Seeding;
  std::unique_ptr<Seeding> seed_;
  std::atomic<bool> degrade_ok_{false};
  void seed_fetch();
  void serve_seed();
  Superblock sb_{};
  Uuid uuid_{};
  SlotMap slots_;
  std::unique_ptr<SegmentPreparer> spares_;
  RecoveryReport recovery_;
};

}  // namespace snc
