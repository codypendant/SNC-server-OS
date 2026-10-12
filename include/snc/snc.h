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
};

struct ExecResult {
  Status status = Status::OK;
  u16 app_code = 0;   // set when status == E_APP
  u64 version = 0;    // object version after the op (current version for reads)
  bool durable = false;
  ObjectID id;        // target, or the new ID for create
  ByteView result;    // valid until the completion returns
};

using Completion = void (*)(void* ctx, const ExecResult& r);

void execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts, Completion done,
             void* ctx);
void create(u32 type_id, ByteView init_args, const ExecOptions& opts, Completion done, void* ctx);
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
  u64 index_lookups_served = 0;
  u64 deferred_completions = 0;  // completions that waited for durability
  u64 emits = 0;                 // follow-ups emitted (OpCtx::emit)
  u64 emits_sent = 0;            // ... sent to another shard
  u64 emits_received = 0;        // ... received from another shard
  u64 emits_failed_over = 0;     // follow-ups that ran their fail_op instead
  u64 emits_in_flight = 0;       // queued here or waiting for durability
  WalStream::Stats wal;
  u64 wal_durable_lsn = 0;
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

    // Persistence. Empty data_dir = in-memory only: every state reports durable.
    std::string data_dir;
    bool init = false;            // create the data directory if it has no superblock
    u64 wal_segment_bytes = 64ull << 20;
    u32 group_commit_us = 50;
    bool wal_sync = true;         // false = wal_sync none (development only)
    bool wal_direct = true;
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
  const RecoveryReport& recovery() const { return recovery_; }

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
  Superblock sb_{};
  Uuid uuid_{};
  SlotMap slots_;
  std::unique_ptr<SegmentPreparer> spares_;
  RecoveryReport recovery_;
};

}  // namespace snc
