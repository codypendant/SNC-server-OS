// Shard (internal): owns a local object map, a heap, a mint lease, its index
// partition and its WAL stream, and executes ops against its objects in place
// (spec sections 3 to 12). Only this shard's worker thread touches a Shard.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "snc/snc.h"

namespace snc {

class Shard {
 public:
  Shard(u16 id, const SlotMap& slots, u64 lease, u64 expected_objects);
  ~Shard();

  u16 id() const { return id_; }
  u16 owner_of(const ObjectID& id) const { return slots_.owner_of(id); }
  const SlotMap& slots() const { return slots_; }
  // Creates mint IDs that this shard owns (docs/ROUTING.md).
  void set_mint_local(bool on) { mint_local_ = on; }

  // ---- Application API (entry shard) ----
  void execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts, Completion done,
               void* ctx);
  void create(u32 type_id, ByteView args, const ExecOptions& opts, Completion done, void* ctx);
  void remove(ObjectID target, const ExecOptions& opts, Completion done, void* ctx);
  void index_lookup(u32 index_id, ByteView key, u32 max_results, IndexCompletion done, void* ctx);
  void prefetch(const ObjectID* ids, u32 n);

  // ---- Backend hooks ----
  void on_message(const Msg& m);
  bool poll();          // WAL group commit, checkpoint slices; true if busy
  bool quiesced() const;

  // ---- Persistence (control path, on this shard's thread) ----
  // Recovery: install one record under the version rule (spec section 12).
  void install(const WalRecord& r, const u8* payload);
  // After replay: rebuild the index and open this shard's WAL stream.
  void finish_recovery(const WalConfig& wal_cfg, u64 next_lsn);

  struct CheckpointResult {
    u64 start_lsn = 0, objects = 0, bytes = 0;
  };
  using CheckpointDone = std::function<void(const CheckpointResult&)>;
  void begin_checkpoint(const std::string& data_dir, const Uuid& uuid, u64 seq,
                        CheckpointDone done);

  ShardStats stats() const;
  template <typename F>
  void for_each_object(F&& fn) const {
    map_.for_each([&](const ObjectHeader* h) {
      if (!h->deleted()) fn(*h);
    });
  }

  LocalMap& map() { return map_; }
  ShardAllocator& alloc() { return alloc_; }

 private:
  friend class OpCtx;

  // Result of running an op on the owner.
  struct RunOut {
    ExecResult r;
    u64 wait_lsn = 0;  // completion must wait until this LSN is durable (0 = no wait)
  };

  struct Pending {
    Completion done = nullptr;
    void* ctx = nullptr;
    u32 gen = 0;
  };

  struct Gather {
    IndexCompletion done = nullptr;
    void* ctx = nullptr;
    u32 gen = 0;
    u32 remaining = 0;
    u32 max_results = 0;
    Status status = Status::OK;
    std::vector<ObjectID> ids;
  };

  // A completion held until its LSN is durable (spec C1, D7).
  struct Waiter {
    u64 lsn = 0;
    enum class Kind : u8 { LOCAL, REMOTE, ERASE } kind = Kind::LOCAL;
    // LOCAL
    Completion done = nullptr;
    void* ctx = nullptr;
    ExecResult r;
    std::vector<u8> result;
    // REMOTE
    Msg resp{};
    u16 dst = 0;
    u8* return_args = nullptr;  // request's external args buffer to hand back
    // ERASE
    ObjectID id;
  };

  struct Checkpoint {
    bool active = false;
    std::vector<ObjectID> ids;  // snapshot at begin; objects found later are skipped
    std::size_t cursor = 0;
    u64 start_lsn = 0, max_lsn = 0, objects = 0, bytes = 0;
    int fd = -1;
    u64 file_off = 0;
    std::vector<u8> chunk;
    u32 writes_inflight = 0;
    bool syncing = false, walk_done = false;
    std::vector<std::unique_ptr<std::vector<u8>>> inflight_bufs;
    CheckpointDone done;
    std::string path;
  };

  bool durable(u64 lsn) const { return wal_ == nullptr || lsn <= wal_->durable_lsn(); }

  void run(MsgKind kind, ObjectID target, u32 op_or_type, ByteView args, const ExecOptions& opts,
           RunOut& out);
  void run_exec(ObjectID target, OpID op_id, ByteView args, const ExecOptions& opts, RunOut& out);
  void run_create(ObjectID id, u32 type_id, ByteView args, RunOut& out);
  void run_delete(ObjectID target, const ExecOptions& opts, RunOut& out);

  void prepare_ctx(OpCtx& c, ObjectHeader* obj, const TypeDescriptor* t, const OpDescriptor* op);
  Status validate_checked(OpCtx& c, const OpDescriptor* op, ByteView args);
  void finish_result(OpCtx& c, const OpDescriptor* op, ExecResult& out);
  u64 log(RecType type, const ObjectHeader& obj);

  // Index partition.
  u32 extract_key(const TypeDescriptor& t, const ObjectHeader& obj, u8* key) const;
  void index_insert(u32 index_id, const u8* key, u32 len, const ObjectID& id);
  void index_erase(u32 index_id, const u8* key, u32 len, const ObjectID& id);
  void index_find(u32 index_id, ByteView key, u32 max_results, std::vector<ObjectID>& out) const;
  void rebuild_index();

  // Completion paths.
  void complete_local(RunOut& out, Completion done, void* ctx);
  void defer(Waiter&& w);
  void release_waiters();
  static void on_wal_durable(void* ctx);

  // Dispatch.
  void send_request(MsgKind kind, u16 dst, ObjectID target, u32 op_or_type, ByteView args,
                    const ExecOptions& opts, Completion done, void* ctx);
  void fail_pending(u32 slot, ObjectID target, Status s);
  void serve(const Msg& m);
  void serve_index(const Msg& m);
  void complete(const Msg& m);
  void complete_index(const Msg& m);
  void finish_gather(u32 slot);
  void return_buffer(u16 owner, u8* ptr);
  Msg make_resp(const Msg& req, const ExecResult& r);

  // Checkpoint.
  void checkpoint_step();
  void checkpoint_write_chunk();
  static void checkpoint_write_done(void* ctx, int result);
  static void checkpoint_sync_done(void* ctx, int result);

  u16 id_;
  SlotMap slots_;
  LocalMap map_;
  Minter minter_;
  bool mint_local_ = false;
  ShardAllocator& alloc_;
  ShardStats stats_;
  std::vector<Pending> pending_;
  std::vector<u32> free_pending_;
  std::vector<Gather> gathers_;
  std::vector<u32> free_gathers_;
  std::vector<u8> result_scratch_;

  using IndexMap = std::unordered_map<std::string, std::vector<ObjectID>>;
  std::unordered_map<u32, IndexMap> indexes_;

  std::unique_ptr<WalStream> wal_;
  std::deque<Waiter> waiters_;  // sorted by lsn
  Checkpoint ckpt_;
};

// The Shard running on the current worker thread.
Shard* current_shard();
void set_current_shard(Shard* s);

}  // namespace snc
