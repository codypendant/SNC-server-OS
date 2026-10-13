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

#include "replication.h"
#include "snc/snc.h"

namespace snc {

class Shard {
 public:
  Shard(u16 id, const SlotMap& slots, u64 lease, u64 expected_objects,
        PageMode pages = PageMode::SMALL);
  ~Shard();

  u16 id() const { return id_; }
  u16 owner_of(const ObjectID& id) const { return slots_.owner_of(id); }
  const SlotMap& slots() const { return slots_; }
  // Creates mint IDs that this shard owns (docs/ROUTING.md).
  void set_mint_local(bool on) { mint_local_ = on; }
  void set_batch_remote(bool on) { batch_remote_ = on; }
  // Sends the requests batched for each destination this iteration.
  void flush_batches();

  // ---- Application API (entry shard) ----
  void execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts, Completion done,
               void* ctx);
  void create(u32 type_id, ByteView args, const ExecOptions& opts, Completion done, void* ctx,
              const ObjectID* near = nullptr);
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
  // Highest mint lease among the IDs installed so far (0 = none).
  u64 max_lease_seen() const { return max_lease_seen_; }
  // Mint new IDs from this lease from now on.
  void set_lease(u64 lease) { minter_ = Minter(lease); }

  // ---- Replication (docs/REPLICATION.md); call before finish_recovery ----
  struct ReplicaConfig {
    // Primary: ship the WAL to a standby.
    bool primary = false;
    repl::Sender::Config sender;
    bool sync = true;           // durable completions wait for the standby's ack
    u64 timeout_ns = 0;         // sync: continue without the standby after this long (0 = never)
    // Standby: receive, install and log the primary's records.
    bool standby = false;
    repl::Receiver::Config receiver;
  };
  void set_replication(const ReplicaConfig& c) { repl_cfg_ = c; }
  // Promotion, on the shard's thread: stop_standby, then once every shard
  // is standby_settled (all it installed is durable), promote.
  void stop_standby();
  bool standby_settled() const;
  void promote(const ReplicaConfig& rc, const std::string& data_dir);
  // The WAL's last record after replay, so a standby can prove its log
  // matches its primary's when it reconnects.
  void set_wal_tail(u64 lsn, u32 crc) {
    tail_lsn_ = lsn;
    tail_crc_ = crc;
  }
  // Primary: first LSN its standby may still need from WAL files (0: none).
  u64 repl_retain_from() const { return sender_ ? sender_->retain_from() : 0; }

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

  // Follow-ups (OpCtx::emit). Paused shards queue but do not dispatch.
  void pause_emits(bool on) { emits_paused_ = on; }
  // Queued here; follow-ups in rings are sent minus received, summed over
  // all shards (Engine::wait_emits).
  u64 emits_in_flight() const { return emit_ready_count_ + emit_wait_.size(); }
  u64 emits_sent() const { return stats_.emits_sent; }
  u64 emits_received() const { return stats_.emits_received; }

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
    ObjectID target;  // batched requests: their responses do not carry it
  };

  // MULTI_REQ records, overwritten in place by MULTI_RESP records.
  struct BatchReq {
    ObjectID target;
    u32 op;
    u32 slot;
    u32 gen;
    u8 flags;  // kMsgAllowUndurable, kMsgDurApplied
    u8 len;
    u16 pad;
    u8 args[kMsgInlineBytes];
  };
  struct BatchResp {
    u32 slot;
    u32 gen;
    u16 status;  // kBatchAnsweredApart: answered by its own RESP
    u16 app_code;
    u8 durable;
    u8 len;
    u16 pad;
    u64 version;
    u8 result[kMsgInlineBytes];
  };
  static_assert(sizeof(BatchReq) == 48 && sizeof(BatchResp) <= sizeof(BatchReq));
  static constexpr u16 kBatchAnsweredApart = 0xffff;
  static constexpr u32 kBatchMax = 32;
  struct OutBatch {
    BatchReq* recs = nullptr;
    u32 n = 0;
  };
  void send_batch(u16 dst);
  void serve_multi(const Msg& m);
  void complete_multi(const Msg& m);

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

  struct Emit {
    ObjectID source, target;
    OpID op = 0, fail_op = 0;
    alignas(8) u8 args[OpCtx::kMaxEmitArgs];
    u64 lsn = 0;  // dispatch once durable
    u8 len = 0;
  };
  static constexpr u64 kEmitBacklog = 1024;
  void route_emit(const Emit& e);
  bool send_emit(u16 dst, const Emit& e);
  void run_emit(const Emit& e);
  void serve_emit(const Msg& m);
  void emit_result(const Emit& e, Status s);
  bool run_emits();
  void queue_emits(u64 lsn);

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

  bool durable(u64 lsn) const {
    if (wal_ == nullptr) return true;
    if (lsn > wal_->durable_lsn()) return false;
    return !repl_hold_ || lsn <= sender_->acked_lsn();
  }

  void run(MsgKind kind, ObjectID target, u32 op_or_type, ByteView args, const ExecOptions& opts,
           RunOut& out);
  void run_exec(ObjectID target, u64 hash, OpID op_id, ByteView args, const ExecOptions& opts,
                RunOut& out);
  void run_create(ObjectID id, u32 type_id, ByteView args, RunOut& out);
  void run_delete(ObjectID target, const ExecOptions& opts, RunOut& out);

  void prepare_ctx(OpCtx& c, ObjectHeader* obj, const TypeDescriptor* t, const OpDescriptor* op);
  Status validate_checked(OpCtx& c, const OpDescriptor* op, ByteView args);
  void finish_result(OpCtx& c, const OpDescriptor* op, ExecResult& out);
  u64 log(RecType type, const ObjectHeader& obj, u16 flags = 0);

  // Index partition.
  u32 extract_key(const TypeDescriptor& t, const ObjectHeader& obj, u8* key) const;
#ifndef NDEBUG
  // OpCtx::local's object before apply, to check apply kept its shape.
  u32 peer_len_ = 0, peer_key_len_ = 0;
  u8 peer_key_[kMaxIndexKey];
#endif
  void index_insert(u32 index_id, const u8* key, u32 len, const ObjectID& id);
  void index_erase(u32 index_id, const u8* key, u32 len, const ObjectID& id);
  void index_find(u32 index_id, ByteView key, u32 max_results, std::vector<ObjectID>& out) const;
  void rebuild_index();

  // Completion paths.
  void complete_local(RunOut& out, Completion done, void* ctx);
  void defer(Waiter&& w);
  void release_waiters();
  static void on_wal_durable(void* ctx);

  // Replication.
  static void on_wal_flush(void* ctx, const u8* recs, u32 len, u64 first_lsn, u64 last_lsn);
  static bool apply_replicated(void* ctx, const u8* rec);
  bool poll_replication();
  void start_serving();
  void start_sender(const std::string& data_dir, u64 next_lsn);

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
  void fill_resp(Msg& resp, const Msg& req, const ExecResult& r);
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
  bool batch_remote_ = true;
  std::vector<OutBatch> out_batches_;  // per destination
  std::vector<u16> batch_dsts_;        // destinations with a batch open
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
  u64 max_lease_seen_ = 0;
  ReplicaConfig repl_cfg_;
  std::unique_ptr<repl::Sender> sender_;
  std::unique_ptr<repl::Receiver> receiver_;
  bool repl_hold_ = false;      // sync replication in force: durable() waits for acks
  bool repl_degraded_ = false;  // sync, but continuing without the standby
  int repl_epfd_ = -1;          // the replication sockets, for idle wake-ups
  u64 tail_lsn_ = 0;            // standby: last record in its WAL, and its crc
  u32 tail_crc_ = 0;
  std::deque<Waiter> waiters_;  // sorted by lsn

  // FIFO over a vector that keeps its memory: std::deque allocates and
  // frees a node every few entries, and follow-ups pass through at
  // millions per second.
  template <typename T>
  class Fifo {
   public:
    bool empty() const { return head_ == buf_.size(); }
    std::size_t size() const { return buf_.size() - head_; }
    T& front() { return buf_[head_]; }
    void push_back(const T& v) { buf_.push_back(v); }
    void pop_front() {
      if (++head_ == buf_.size()) {
        buf_.clear();
        head_ = 0;
      } else if (head_ >= 4096 && head_ * 2 >= buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
      }
    }

   private:
    std::vector<T> buf_;
    std::size_t head_ = 0;
  };

  std::vector<Emit> emit_scratch_;  // emitted by the op now running
  // Durable source state: dispatch at the next poll.
  std::vector<Emit> emit_local_;
  std::vector<Emit> emit_running_;  // emit_local_ being run (keeps its memory)
  std::vector<Fifo<Emit>> emit_remote_;  // per destination shard
  u64 emit_ready_count_ = 0;
  ObjectID cur_source_;  // source of the follow-up now running (OpCtx::source)
  Fifo<Emit> emit_wait_;  // sorted by lsn
  bool emits_paused_ = false;
  bool resuming_ = false;
  Checkpoint ckpt_;
};

// The Shard running on the current worker thread.
Shard* current_shard();
void set_current_shard(Shard* s);

}  // namespace snc
