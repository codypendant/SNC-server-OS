// Shard (internal): owns a local object map, a heap, a mint lease, its index
// partition and its WAL stream, and executes ops against its objects in place
// (spec sections 3 to 12). Only this shard's worker thread touches a Shard.
#pragma once

#include <atomic>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ckpt_pacer.h"
#include "node_link.h"
#include "replication.h"
#include "snc/cluster.h"
#include "snc/snc.h"

namespace snc {

struct ObjectIdHash {
  std::size_t operator()(const ObjectID& id) const { return snc_hash64(id); }
};

// A stage request run as a chain of bounded units on the slot's shard
// (S1, docs/MIGRATION_STAGING.md 3.2). Volatile, on the requesting
// (control) thread's stack: the shard reads and writes it only inside
// stage_admit / stage_unit / stage_release, each one shard task. Durable
// progress is only the records the units write (no manifest): a lost
// chain leaves inert records in a foreign slot, which a retry finds.
struct StageChain {
  enum Phase : u8 { ADMIT, CHECK, PUT, SCAN, DROP, STAGE, DONE };
  static const char* phase_name(u8 p);
  // The request.
  u16 slot = 0, from = 0;
  u64 move = 0, epoch = 0, count = 0, digest = 0;
  bool final = false;
  const std::vector<SlotObject>* objs = nullptr;
  // Bounds of one unit (~0: none, the whole request in one unit). A unit
  // stops at the first object boundary (a scan: the first empty bucket)
  // past unit_ns of shard-thread time or unit_bytes of payload put or
  // dropped, or (puts, drops) while more than window_bytes it appended
  // are not yet durable; it always does at least one object.
  u64 unit_ns = ~u64{0}, unit_bytes = ~u64{0}, window_bytes = ~u64{0};
  // State.
  u8 phase = ADMIT;
  u64 token = 0;       // this chain's registration (0: none)
  u32 mut = 0;         // the slot's mutation count after this chain's own writes
  std::size_t i = 0;   // next entry (check, put) or gone ID (drop)
  std::unordered_map<ObjectID, const SlotObject*, ObjectIdHash> listed;
  LocalMap::Walk walk;
  std::vector<ObjectID> gone;
  u64 n = 0, d = 0;
  u64 lsn = 0;         // the record that must be durable before OK (0: none)
  std::deque<std::pair<u64, u64>> pending;  // (lsn, bytes) appended, maybe not yet durable
  u64 pending_bytes = 0;
  // Measurements (reported at the end).
  u64 runs = 0;  // returns to the requesting thread (window waits, debug stops)
  u64 units = 0, put = 0, dropped = 0, bytes = 0, scan_restarts = 0, max_unit_ns = 0, pending_hw = 0;
  u64 index_ns = 0, log_ns = 0, shard_ns = 0;
  std::vector<u32> unit_us;
  u8 max_unit_phase = ADMIT;
  u64 phase_units[DONE] = {};
};

class Shard {
 public:
  Shard(u16 id, const SlotMap& slots, u64 lease, u64 expected_objects,
        PageMode pages = PageMode::SMALL);
  ~Shard();

  u16 id() const { return id_; }
  u16 owner_of(const ObjectID& id) const { return slots_.owner_of(id); }
  // Cluster (docs/CLUSTER.md): slots another replication group owns are
  // foreign here. `member` is which of its group's members this node is.
  void set_cluster(const ClusterMap* map, u16 group, u16 member) {
    cluster_ = map;
    group_ = group;
    member_ = member;
    // Without a record of earlier maps, every slot may have moved up to now.
    moved_at_.assign(kSlots, map ? static_cast<u32>(map->epoch) : 0);
  }
  const ClusterMap* cluster() const { return cluster_; }
  u16 group() const { return group_; }
  bool foreign_slot(u16 slot) const { return cluster_ != nullptr && cluster_->group_of[slot] != group_; }
  bool foreign(const ObjectID& id) const { return cluster_ != nullptr && foreign_slot(slot_of(id)); }

  // ---- Slot moves (docs/MIGRATION.md) ----
  // This group's map gives it the slot, the slot is neither sealed nor
  // staged here, and local evidence proves the arrival the map records
  // (docs/MIGRATION_FAIL_CLOSED.md 4; the cache holds kCacheUnproven
  // otherwise): ops on its objects may run (on the shard that owns it).
  bool serving(u16 slot) const {
    if (cluster_ == nullptr) return true;
    const u8 p = phases_ ? phases_[slot].load(std::memory_order_relaxed) : 0;
    return cluster_->group_of[slot] == group_ && p <= 1;  // NONE or ACTIVE
  }
  // Every shard's slot phases (SlotPhase), shared: each slot's is written
  // only by the shard that owns it; others read it to mint.
  void set_phases(std::atomic<u8>* phases) { phases_ = phases; }
  // Placement changed (Engine::update_placement), in two passes over every
  // shard. set_map: route by `map` from now on, recompute the serving
  // proof of every slot this shard owns, activate slots staged here for the
  // arrival it records, and re-route the follow-ups held.
  // map_changed, once every shard has it: re-send everything unsettled.
  void set_map(const ClusterMap* map);
  void map_changed();
  // After recovery, promotion or a replicated state change: phases from the
  // state objects, then serving, then (a primary) activation.
  void refresh_slots();
  SlotReport slot_report(u16 slot);
  // slot_report in bounded steps (S1: the final stage's reply), the walk
  // restarting if the table grows or the slot's objects change between
  // steps. True once `out` is filled.
  struct ReportWalk {
    u16 slot = 0;
    bool started = false, foreign = false;
    LocalMap::Walk walk;
    u32 mut = 0;
    u64 n = 0, d = 0, restarts = 0;
  };
  bool slot_report_unit(ReportWalk& w, SlotReport& out, u64 unit_ns);
  // Every slot state object here, as reports without the live scan.
  void slot_states(std::vector<SlotReport>& out);
  // The slot's application objects, with payloads if asked.
  void slot_objects(u16 slot, bool payloads, std::vector<SlotObject>& out);
  // Control-path steps of a move, on the shard owning the slot. Each sets
  // `lsn` to the record that must be durable before it counts (0: none).
  Status seal_slot(u16 slot, u64 move, u16 to, u64 epoch, u64& lsn);
  Status unseal_slot(u16 slot, u64 move, u64& lsn);
  Status export_slot(u16 slot, u64 move, bool final, const std::vector<SlotObject>& have,
                     std::vector<SlotObject>& out, u64& count, u64& digest);
  // Destination staging as a chain (StageChain). admit: the refusals
  // that need no work (a finished STAGED for the move answers at once:
  // phase DONE), then registers the chain for its slot (one at a time; a
  // newer move's chain supersedes an older one's). unit: the guards (fence,
  // superseded, map, the slot's mutations), then work up to the bounds;
  // DONE when finished. release: drops the registration (every exit).
  Status stage_admit(StageChain& c);
  Status stage_unit(StageChain& c);
  void stage_release(StageChain& c);
  // Activates every slot staged here that the map now gives this group by
  // exactly that staged move (its arrival on record), or, with no arrival
  // on record (placement v1), at or past the staged epoch.
  void activate_staged();
  // The start barrier (docs/MIGRATION_FAIL_CLOSED.md 5a): the placement says
  // this group is moving `slot` away (move `move`, SEALED or STAGED there).
  // Seals it here as an ordinary seal if it is not sealed for that move and
  // its arrival is proven; writes nothing if it is unproven (it does not
  // serve anyway). OK with `lsn` the seal's record (0: nothing written).
  Status reseal_slot(u16 slot, u64 move, u16 to, u64 epoch, u64& lsn);
  // Asks finish_recovery or promote to re-seal before anything serves;
  // reseal_lsn() then has the record to wait for (0: none).
  void set_reseal(u16 slot, u64 move, u16 to, u64 epoch) { reseal_ = Reseal{true, slot, to, move, epoch}; }
  u64 reseal_lsn() const { return reseal_lsn_; }
  // Durable in this node's own WAL (not waiting for a standby's ack).
  bool lsn_local_durable(u64 lsn) const { return wal_ == nullptr || lsn <= wal_->durable_lsn(); }
  // Slots this shard owns that the map gives this group but that are unproven.
  u64 unproven_count() const;
  bool lsn_durable(u64 lsn) const { return durable(lsn); }
  bool is_standby() const { return repl_cfg_.standby; }
  // A placement epoch newer than this shard's was seen (a node link frame).
  void set_epoch_hint(std::atomic<u64>* hint) { epoch_hint_ = hint; }
  // Published gauges (include/snc/gauges.h, S0). Observability only.
  void set_gauges(ShardGauges* g) { gauges_ = g; }
  // Admission budgets (S1, docs/S1_ADMISSION.md). Set before the runtime
  // runs ops; legacy restores the gate as it was before S1.
  struct Admission {
    bool legacy = false;
    u64 waiter_budget_bytes = 0;
    u64 wal_buffer_budget_bytes = 0;
    u64 pending_budget = 0;  // S1.1: forwarded client requests awaiting an answer
  };
  void set_admission(const Admission& a);
  // Checkpoint walk pacing (S2, docs/S2_PACING.md). Set before the first
  // checkpoint; off restores the walk before S2 (kCheckpointSlice objects
  // per loop iteration). The controller's reports run either way.
  struct Pacing {
    bool on = false;
    u32 batch = 16;  // objects visited between clock reads
    ckpt::PacerConfig cfg;
  };
  void set_pacing(const Pacing& p) { pacing_ = p; }
  // S3 (docs/S3_WRITEBACK.md): dsync = RWF_DSYNC chunks with at most
  // `window` in flight; off = the write path before S3 (no window).
  struct CkptIo {
    bool dsync = true;
    u32 window = 4;
  };
  void set_ckpt_io(const CkptIo& io) { ckpt_io_ = io; }
  // Copies this shard's counters into its gauge block: at most every
  // ShardGauges::kPublishNs, or at once when `force` or after a checkpoint
  // phase change. Shard thread only.
  void publish_gauges(u64 now_ns, bool force = false);
  // Opens this shard's node links, once its state is recovered (or it is
  // promoted: a standby has none).
  void start_links();
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
  void install_indexed(const WalRecord& r, const u8* payload);
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
    // If set, the timeout lets a shard continue without its standby only
    // while this is true (a witness granted it; docs/REPLICATION.md).
    const std::atomic<bool>* degrade_gate = nullptr;
    // Standby: receive, install and log the primary's records.
    bool standby = false;
    repl::Receiver::Config receiver;
  };
  void set_replication(const ReplicaConfig& c) { repl_cfg_ = c; }
  struct ReplSummary {
    u8 role;
    bool live, degraded, blocked;
    bool ready = false, regressed = false;  // docs/SYNC_READINESS.md 3
  };
  ReplSummary repl_summary() const;
  // Promotion, on the shard's thread: stop_standby, then once every shard
  // is standby_settled (all it installed is durable), promote.
  // `new_term` is the term the promotion starts; the replication listener
  // stays up, refusing primaries of older terms.
  void stop_standby(u64 new_term);
  bool standby_settled() const;
  void promote(const ReplicaConfig& rc, const std::string& data_dir);
  // Fenced: close the node links, run no follow-ups, refuse clients with
  // E_STANDBY. Happens by itself when a member of the group refuses this
  // primary's term; `flag` (engine-wide) spreads it to every shard, and
  // setting it from outside (a witness said so) does the same.
  void set_withdraw_flag(std::atomic<bool>* flag) { withdraw_flag_ = flag; }
  // Set once a sender exists (Engine::replicating).
  void set_replicating_flag(std::atomic<bool>* flag) { replicating_flag_ = flag; }
  void withdraw();
  bool withdrawn() const { return withdrawn_; }
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
    // True only when a successful admit() already counted this answer's
    // waiter bytes (R1 in run_exec, run_create, a client run_delete, or
    // admit_answer_wait itself). Never an exemption: a replay leaves it
    // false (S1.2, docs/S1_2_REPLAY.md).
    bool waiter_reserved = false;
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
    bool remote = false;  // came over a node link: its source is another group's
    u32 epoch = 0;        // the placement epoch (low 32 bits) its source's owner emitted it at
  };
  static constexpr u64 kEmitBacklog = 1024;
  void route_emit(const Emit& e);
  bool send_emit(u16 dst, const Emit& e);
  void run_emit(const Emit& e);
  void serve_emit(const Msg& m);
  void emit_result(const Emit& e, Status s);
  bool run_emits();
  void queue_emits(u64 lsn);
  // Its target's slot is this group's by the map but not served (sealed or
  // staged): kept here until the slot or the map changes.
  bool stale(const Emit& e) const;
  void hold_emit(const Emit& e);
  void release_held();
  // Slot states.
  ObjectHeader* slot_state(u16 slot);
  u64 write_slot_state(u16 slot, const struct SlotStateV1& st);
  // The serving cache's value for `slot` with state `st` (null: none) under
  // this shard's map: the phase, or kCacheUnproven. Stored by set_cache,
  // which reports a slot that becomes unproven.
  u8 cache_value(u16 slot, const struct SlotStateV1* st) const;
  void set_cache(u16 slot, const struct SlotStateV1* st);
  void recompute_slot(u16 slot);  // from its state object
  SlotProof proof_of(u16 slot, const struct SlotStateV1* st) const;
  struct Reseal {
    bool on = false;
    u16 slot = 0, to = 0;
    u64 move = 0, epoch = 0;
  };
  Reseal reseal_;
  u64 reseal_lsn_ = 0;
  void apply_reseal();
  // SNC_TRACE=1 only: time inside put_image's index updates and WAL
  // appends during one stage_slot (docs/TRACING.md).
  u64 trace_index_ns_ = 0, trace_log_ns_ = 0;
  void resume_slot(u16 slot);
  // Puts an object image (a staged copy) in the map, indexed and logged.
  ObjectHeader* put_image(const SlotObject& so, u64& lsn);
  void drop_object(ObjectHeader* o, u64& lsn);
  Status refuse_unserved(u16 slot, RunOut& out);

  // One part chunk write (S3, docs/S3_WRITEBACK.md section 3). The write's
  // completion context: it owns the chunk's buffer from submit until that
  // write's own completion is processed, then frees it, whatever order
  // completions arrive in. A busy slot is never reused or freed before
  // then; the shard owns the slots so none leaks.
  struct CkptWrite {
    Shard* shard = nullptr;
    std::vector<u8> buf;
    u32 len = 0;  // bytes submitted; the completion must report exactly this
    u64 index = 0;      // chunk number within the checkpoint
    u64 submit_ns = 0;
    bool busy = false;
  };
  struct Checkpoint {
    bool active = false;
    std::vector<ObjectID> ids;  // snapshot at begin; objects found later are skipped
    std::size_t cursor = 0;
    u64 start_lsn = 0, max_lsn = 0, objects = 0, bytes = 0;
    int fd = -1;
    u64 file_off = 0;
    std::vector<u8> chunk;  // being built: not yet submitted
    u32 writes_inflight = 0;
    bool syncing = false, walk_done = false;
    // S3 walk states: `visited` once the cursor reached the snapshot's
    // end; `blocked` while a chunk (full, or the last one once visited)
    // waits for a window slot. walk_done only once visited and nothing is
    // left to submit: then the IDs are freed and DRAIN begins.
    bool visited = false, blocked = false;
    std::vector<std::unique_ptr<CkptWrite>> slots;  // never more than the window (dsync)
    std::vector<CkptWrite*> free_slots;
    u64 chunks = 0;  // submitted
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
  // `client`: a client's op, which a standby refuses or serves as a read;
  // follow-ups run regardless (they arrive only once shards are promoted).
  void run_exec(ObjectID target, u64 hash, OpID op_id, ByteView args, const ExecOptions& opts,
                RunOut& out, bool client = true);
  void run_create(ObjectID id, u32 type_id, ByteView args, const ExecOptions& opts, RunOut& out);
  // Client sessions (docs/RETRIES.md).
  ObjectHeader* find_session(u64 client_id);
 public:
  // Deletes sessions untouched for ttl_s (primary, before a checkpoint).
  u64 expire_sessions(u32 now_s, u32 ttl_s);
 private:
  bool replay_if_seen(const ExecOptions& opts, RunOut& out);
  // Logs the op's session record, first in its atomic group; returns the
  // session so the caller can set its last_lsn with the group's.
  ObjectHeader* record_session(const ExecOptions& opts, const ObjectID& id, u64 version);
  void run_delete(ObjectID target, const ExecOptions& opts, RunOut& out, bool internal = false);

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
  // Runs every object's resume hook; what they emit waits for `lsn`.
  void resume_all(u64 lsn);
  bool poll_links();
  static void on_link_frame(void* ctx, const NodeLinks::Frame& f);
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
  std::unique_ptr<repl::Receiver> fence_listener_;  // promoted: refuses the old term
  std::atomic<bool>* withdraw_flag_ = nullptr;
  std::atomic<bool>* replicating_flag_ = nullptr;
  bool withdrawn_ = false;
  bool repl_hold_ = false;      // sync replication in force: durable() waits for acks
  bool repl_degraded_ = false;  // sync, but continuing without the standby
  bool repl_blocked_ = false;  // overdue, but the degrade gate is closed
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
  std::vector<Fifo<Emit>> emit_node_;    // per node link (another group's shard)
  u64 emit_ready_count_ = 0;
  ObjectID cur_source_;  // source of the follow-up now running (OpCtx::source)
  Fifo<Emit> emit_wait_;  // sorted by lsn
  bool emits_paused_ = false;
  bool resuming_ = false;
  Checkpoint ckpt_;
  // Observability (S0): survives `ckpt_ = Checkpoint{}`.
  struct CkptObs {
    CkptPhase phase = CkptPhase::IDLE;
    u64 seq = 0, started_ns = 0, phase_since_ns = 0;
    u64 bytes_written = 0, writes_peak = 0;
    u64 snapshot_us = 0, walk_us = 0, drain_us = 0, durable_wait_us = 0;  // of the one in progress
    u64 last_snapshot_us = 0, last_walk_us = 0, last_drain_us = 0, last_durable_wait_us = 0,
        last_sync_us = 0, last_total_us = 0, last_result = 0, completed = 0;
    u64 last_objects = 0, last_bytes_issued = 0;
    // S2 (docs/S2_PACING.md): of the checkpoint in progress or the last one.
    u64 walk_total = 0;        // snapshot IDs to visit
    u64 walk_steps = 0;        // loop iterations that walked
    u64 catchup_steps = 0;     // of those, in catch-up mode
    u64 step_max_ns = 0;       // longest walk step
    u64 step_max_objects = 0;  // most snapshot IDs visited in one step
    u64 wal_baseline_us = 0;   // learned before this checkpoint began
    // Per shard, since start: checkpoints that passed their target or
    // reached the hard maximum age (each counted once).
    u64 past_target_count = 0, degraded_count = 0;
    bool past_target_counted = false, degraded_counted = false;
    // S3 (docs/S3_WRITEBACK.md section 6): of the checkpoint in progress or
    // the last one, except sync_writes_total (since start).
    u64 window_full_steps = 0, chunks = 0, buf_bytes_peak = 0;
    u64 write_lat_last_us = 0, write_lat_ewma_us = 0, write_lat_max_us = 0;
    u64 write_lat_hist[4] = {0, 0, 0, 0};  // < 1 ms, < 10 ms, < 100 ms, more
    u64 sync_writes = 0, sync_writes_total = 0;
  } ckpt_obs_;
  CkptIo ckpt_io_;
  // Tests only (Engine::debug_ckpt_hold_writes): completions kept unprocessed.
  struct HeldWrite {
    CkptWrite* w;
    int result;
  };
  std::vector<HeldWrite> ckpt_held_;
  // Tests only (Engine::debug_ckpt_defer_writes): submitted, not yet issued.
  struct DeferredWrite {
    CkptWrite* w;
    u64 off;
  };
  std::vector<DeferredWrite> ckpt_deferred_;
  void checkpoint_issue(CkptWrite* w, u64 off);  // sri_write, counting the synchronous fallback
  bool checkpoint_submit_chunk();  // false: the window is full and the chunk stays
  void checkpoint_write_complete(CkptWrite* w, int result);
  void checkpoint_process_held();
  // Chunk buffer bytes retained now: the chunk being built plus every
  // slot's buffer, summed from the buffers themselves (at most window + 1
  // with dsync), so the gauge cannot drift from what is really held.
  u64 ckpt_buf_bytes() const;
  void ckpt_note_buf();  // updates the retained-buffer peak
  Pacing pacing_;
  ckpt::PacerState pacer_;
  void pace(u64 now_ns);  // one controller step for the checkpoint in progress
  void ckpt_enter(CkptPhase p, u64 now_ns);  // records the phase just left
  // Observability (S0): memory a held answer pins, excluding std::deque
  // block overhead: the Waiter, a LOCAL answer's copied result, a REMOTE
  // answer's external result buffer.
  static u64 waiter_cost(const Waiter& w) {
    u64 b = sizeof(Waiter) + w.result.size();
    if (w.kind == Waiter::Kind::REMOTE && w.resp.len > kMsgInlineBytes) b += w.resp.len;
    return b;
  }
  // S1 admission (docs/S1_ADMISSION.md). admit() is the R1 check: called
  // after an op's validation and before its first mutation, with the
  // op's worst-case WAL bytes and waiter bytes. It changes no state; a
  // refusal sets E_OVERLOADED and counts its cause.
  bool admit(u64 wal_need, u64 waiter_need, RunOut& out);
  // S1.1: an answer that must wait for durability and was not reserved
  // takes a waiter under the waiter budget, or is refused (its op changed
  // nothing). True when it may wait or need not.
  bool admit_answer_wait(RunOut& out);
  // S1.1: a client request forwarded to another shard takes a pending
  // slot under the pending budget, or is refused (refused_pending).
  bool pending_refuse(ObjectID target, Completion done, void* ctx);
  u64 pending_live() const { return pending_.size() - free_pending_.size(); }
  bool followups_refuse(const ExecOptions& opts, ObjectID target, bool replay, Completion done,
                        void* ctx);
  // Worst-case bytes of a mutation's local() peer record: the largest
  // registered type's record (the peer's type is chosen inside apply).
  u64 peer_record_max_ = 0;
  Admission adm_{true, 0, 0};  // legacy until set_admission
  // Debug builds: WAL active bytes when admit() last passed, to check
  // that the op appended no more than it reserved (no flush runs between).
  u32 adm_active_before_ = 0;
  u64 adm_wal_need_ = 0;
  void check_reserved(const OpDescriptor* op);
  ShardGauges* gauges_ = nullptr;
  u64 gauges_published_ns_ = 0;
  bool gauges_dirty_ = false;
  u64 waiters_peak_ = 0, waiter_bytes_ = 0, waiter_bytes_peak_ = 0, emits_peak_ = 0;

  const ClusterMap* cluster_ = nullptr;
  // Per slot: the placement epoch at which it last changed group (low 32
  // bits), as far as this shard knows (docs/MIGRATION.md, follow-ups).
  std::vector<u32> moved_at_;
  u16 group_ = 0, member_ = 0;
  std::unique_ptr<NodeLinks> links_;
  // Slot moves (docs/MIGRATION.md).
  std::atomic<u8>* phases_ = nullptr;
  // Stage chains (S1): per slot, the running chain's (token, move); and
  // per slot, a count of the writes staging made to its objects (put_image,
  // drop_object), which a chain compares between units.
  std::unordered_map<u16, std::pair<u64, u64>> stage_chain_;
  u64 stage_token_ = 0;
  SlotReport slot_report_head(u16 slot);
  std::vector<u32> slot_mut_ = std::vector<u32>(kSlots, 0);
  // Slots whose stale STAGED (older than the arrival) was logged, with the
  // (arrival, staged move) logged: said once per change.
  std::unordered_map<u16, std::pair<u64, u64>> stale_staged_said_;
  std::vector<Emit> emit_held_;
  std::atomic<u64>* epoch_hint_ = nullptr;
  MovedResult moved_buf_{};
};

// The Shard running on the current worker thread.
Shard* current_shard();
void debug_hold_wal_flush(u16 shard, bool on);  // tests only (Engine::debug_hold_wal)
void debug_ckpt_walk_limit(u64 limit);           // tests only (Engine::debug_ckpt_walk_limit)
// Checkpoint part chunks (S3, docs/S3_WRITEBACK.md section 3): a chunk is
// submitted once it holds kCheckpointChunk bytes, so a chunk never exceeds
// kCheckpointChunk - 1 + one record. Its buffer is reserved at
// kCheckpointChunkReserve, so it grows past that only for a record that
// does not fit, and then only to the exact size. Each retained buffer is
// thus at most kCheckpointChunkMax bytes (wal_format.h record_size of
// kMaxPayload is kMaxPayload plus a header of under 4 KiB).
inline constexpr u32 kCheckpointChunk = 1u << 20;
inline constexpr u32 kCheckpointChunkReserve = kCheckpointChunk + (64u << 10);
inline constexpr u64 kCheckpointChunkMax = kCheckpointChunk + kMaxPayload + 4096;
// Tests only (S3; the Engine::debug_ckpt_* functions of the same names).
void debug_ckpt_hold_writes(bool on);
void debug_ckpt_defer_writes(bool on);
void debug_ckpt_release_writes(u32 n, bool newest_first);
void debug_ckpt_fail_write(u64 index, int mode);
void set_current_shard(Shard* s);

}  // namespace snc
