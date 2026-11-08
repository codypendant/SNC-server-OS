// SNC application API (spec section 6). All calls are asynchronous, are made
// from a shard worker thread, and complete on that same shard. A completion
// may run before the call returns when the target is local (the local fast
// path has no queue), so callers must not issue new work from inside a
// completion recursively without bound.
#pragma once

#include "snc/native_runtime.h"
#include "snc/cluster.h"
#include "snc/placement.h"
#include "snc/local_map.h"
#include "snc/registry.h"
#include "snc/slot_map.h"
#include "snc/storage.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

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
// Clustered (docs/CLUSTER.md): the cluster map and this node's replication
// group; null when the engine is not clustered.
const ClusterMap* cluster_map();
u16 group_id();

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
  u64 overloaded = 0;            // the five refused_* below, summed
  u64 refused_followup = 0;      // follow-up backlog above kEmitBacklog
  u64 refused_ring = 0;          // forward or batch found the ring full
  u64 refused_waiter = 0;        // S1: waiter budget (Config::waiter_budget_bytes)
  u64 refused_wal_buffer = 0;    // S1: WAL buffer budget (Config::wal_buffer_budget_bytes)
  u64 refused_pending = 0;       // S1.1: pending budget (Config::pending_budget)
  u64 refused_replay = 0;        // S1.2: duplicates refused for waiter room (a subset of refused_waiter)
  u64 pending_now = 0;           // forwarded client requests awaiting an answer
  u64 pending_peak = 0;
  u64 gather_overloaded = 0;     // index gather legs dropped: ring full
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
  // Cluster (docs/CLUSTER.md).
  u64 node_emits_sent = 0;       // follow-ups sent to another group's node
  u64 node_emits_received = 0;   // ... received from another group's node
  u64 node_emits_dropped = 0;    // ... dropped, their link down (resume re-emits them)
  u64 node_emits_misrouted = 0;  // ... received for a slot not ours, from a node behind ours
  // Slot moves (docs/MIGRATION.md).
  u64 emits_held = 0;            // follow-ups held: their slot sealed or staged here
  u64 emits_held_now = 0;        // ... held at this moment
  u64 emits_stale = 0;           // dropped: sent by their source's owner before the source's slot moved
  // Fail-closed serving (docs/MIGRATION_FAIL_CLOSED.md): slots this shard
  // owns that the map gives this group but local evidence does not prove.
  u64 slots_unproven = 0;
  u64 node_connects = 0;         // outbound node links that came up
  u64 node_resumes = 0;          // resume passes run because a link came up
  WalStream::Stats wal;
  u64 wal_durable_lsn = 0;
  // Replication (docs/REPLICATION.md).
  u8 repl_role = 0;            // 0 none, 1 primary, 2 standby
  bool repl_live = false;      // link to the other side is up
  bool repl_degraded = false;  // primary, sync: continuing without the standby
  bool repl_blocked = false;   // primary, sync: past the timeout, waiting for leave to degrade
  u64 stale_reads = 0;
  u64 replayed = 0;            // sequenced requests answered from a client session
  u64 sessions_expired = 0;         // standby: reads refused for want of a lease (E_STALE)
  bool repl_lost = false;      // primary: the standby must be reseeded
  u64 repl_acked_lsn = 0;      // primary: the standby is durable up to here
  u64 repl_bytes = 0;          // primary: bytes shipped
  u64 repl_retained_bytes = 0; // primary: unacked records kept in memory
  u64 repl_records = 0;        // standby: records applied
  u64 repl_connects = 0;       // connections made (primary) or accepted (standby)
  u64 repl_disk_bytes = 0;     // primary: shipped from WAL files (catch-up)
  bool wal_direct = false;
  // Resource gauges at this moment (docs/EXPERIMENTAL_BASELINE.md T-Q4).
  u64 waiters_now = 0;    // completions waiting for durability (client answers held)
  u64 overflow_now = 0;   // messages queued behind full rings
  u64 wal_next_lsn = 0;   // next LSN this shard's WAL will assign
};

// Replication catch-up from WAL files, process-wide: reader threads alive,
// frames read ahead and not yet sent (bytes, now and high-water), readers
// started, and readers that stopped because their link went
// (docs/BASELINE_READINESS.md B3). Readable from any thread.
struct ReplDiskGauges {
  u64 readers_live = 0;
  u64 queued_bytes = 0;
  u64 queued_hwm = 0;
  u64 started = 0;
  u64 stopped_on_cancel = 0;
};
ReplDiskGauges repl_disk_gauges();

// Slot moves (docs/MIGRATION.md): one object of a slot, as the source
// exports it and the destination stages it. `crc` covers the type, schema
// and payload, so (version, crc) names the object's state exactly.
struct SlotObject {
  ObjectID id;
  u64 version = 0;
  u32 type_id = 0;
  u16 schema_ver = 0;
  u32 crc = 0;
  bool has_payload = false;  // false: the receiver holds this state already
  std::vector<u8> payload;
};
// Order-independent digest of a slot's objects: the sum of this over them.
u64 slot_object_digest(const ObjectID& id, u64 version, u32 crc);

// A slot's state in this group (docs/MIGRATION.md).
// Why a slot is or is not served here (docs/MIGRATION_FAIL_CLOSED.md 13).
enum class SlotProof : u8 {
  OK = 0,     // served (or would be, on a primary)
  FOREIGN,    // another group's by this node's map
  SEALED,     // sealed here (moving away)
  STAGED,     // staged here, not active
  MISSING,    // the map says it arrived by a move this node holds no evidence of
  STALE_MAP,  // this node's evidence is of a later move than the map's arrival: refresh the map
};
const char* slot_proof_name(SlotProof p);

struct SlotReport {
  u16 slot = 0;
  u8 phase = 0;  // SlotPhase: 0 none, 1 active, 2 sealed, 3 staged
  u16 flags = 0;  // the state's flags (kSlotUnsealed)
  SlotProof proof = SlotProof::OK;
  SlotArrival arrived;  // the arrival this node's map has on record
  u16 group = 0;
  u64 move_id = 0, epoch = 0, count = 0, digest = 0;  // as recorded
  bool durable = true;   // the state record is durable
  bool serving = false;  // ops on the slot run here
  u16 map_group = 0;     // owner by this node's map
  u64 map_epoch = 0;
  u64 live_count = 0, live_digest = 0;  // the slot's objects now
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

struct EngineState;  // src/core/engine.cpp

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
    // Primary: when set, a seed is served only while it returns true;
    // otherwise the standby is told to retry (docs/SYNC_READINESS.md 4).
    // Called on the seed thread.
    std::function<bool()> seed_gate;
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
    // Client sessions (docs/RETRIES.md) untouched this long are deleted at
    // the next checkpoint (0 = keep forever). A client returning later must
    // use a new client_id.
    u32 session_ttl_s = 3600;

    // Cluster (docs/CLUSTER.md): every replication group of a cluster, in
    // group-id order, the same list on each node; empty = one space owning
    // every slot. This node is member `member` of cluster[group_id], whose
    // shard count must be runtime.n_shards. Its replication (replica_*,
    // standby) is configured as for any space, toward the other members.
    // Ops on another group's objects are refused (E_MOVED); follow-ups
    // to them travel over node links, which a standby opens on promotion.
    std::vector<ClusterGroup> cluster;
    u16 group_id = 0;
    u16 member = 0;
    // From the control plane (docs/CONTROL_PLANE.md, Placement::map): the
    // slot -> group assignment (kSlots entries) and its placement epoch.
    // Empty: ClusterMap::make's ranges at epoch 1.
    std::vector<u8> cluster_slots;
    u64 cluster_epoch = 1;
    // Cluster-wide mint leases (docs/CONTROL_PLANE.md section 11): returns
    // a block [lo, hi) of at least `want` leases that no other node holds
    // or will be given (from the control plane's `leases` machine). Set, a
    // primary mints only from blocks: it takes one when its superblock's
    // is used up, and a promotion always takes a fresh one, recorded in
    // the superblock before anything is minted. A block it still holds
    // needs no control plane. Unset: leases from the space's own counter,
    // unique only within the space.
    std::function<bool(u64 want, u64& lo, u64& hi)> lease_blocks;

    // Per slot, the arrival on record at start (placement v2; kSlots
    // entries, or empty: none).
    std::vector<SlotArrival> cluster_arrived;

    // Legacy directories (docs/LEGACY_ROLE.md): a data directory from
    // before role files (no DIR/role, superblock without kSbRoleTracked)
    // never starts as a primary unless this says, once, that it is one.
    // PRIMARY: its group's primary's own disk (adopt_primary_check may
    // refuse; the start barrier still runs; tracked only once role
    // "primary" is durable). STANDBY: needs `standby` (the same as starting
    // it with `standby` alone; for scripts that say it explicitly).
    // Refused on a new directory, or one whose recorded role differs.
    enum class AdoptRole : u8 { NONE, PRIMARY, STANDBY };
    AdoptRole adopt_role = AdoptRole::NONE;
    // Called (space UUID, superblock term) before a legacy directory is
    // adopted as a primary's; throws std::runtime_error to refuse.
    std::function<void(const Uuid& space, u64 term)> adopt_primary_check;
    // Placement refresh (docs/MIGRATION.md): returns the control plane's
    // latest placement (and keeps it where the node starts from next
    // time). Asked every placement_poll_ms, and at once when a newer epoch
    // is seen on a node link. Unset: the map never changes.
    std::function<bool(Placement& out)> placement_fetch;
    u32 placement_poll_ms = 1000;
    // The placement read linearizably (the control plane's latest). Set,
    // it is the start barrier (docs/MIGRATION_FAIL_CLOSED.md 5a): a
    // promotion, and any start of a data directory that was last a standby,
    // read it, adopt it and re-seal a slot this group is moving away
    // before serving; if it fails they do not happen.
    std::function<bool(Placement& out)> placement_sync;
    // Tests only (snc_server --debug-barrier, with --debug-admin): at the
    // end of the start barrier, after the re-seal is durable and before
    // role "primary" is written, sleep this long (ROLE m stays visible),
    // or exit at once as a crash would (no destructors, nothing flushed).
    u32 debug_barrier_hold_ms = 0;
    bool debug_barrier_crash = false;
    // Destination staging (S1, docs/MIGRATION_STAGING.md 3.2): a stage
    // request runs as a chain of shard tasks ("units"), each stopping past
    // move_unit_us of shard time or move_unit_bytes of payload written, and
    // no put or drop unit starts while more than move_window_bytes the
    // chain appended are not yet durable. Prototype defaults (a sweep, not
    // a contract). move_units = false: the whole request in one task
    // (the behavior before S1; same records, same order).
    bool move_units = true;
    u32 move_unit_us = 1000;
    u64 move_unit_bytes = 1u << 20;
    u64 move_window_bytes = 8u << 20;

    // Start with follow-ups held (queued, not run) until release_emits(),
    // so a test can inspect exactly the recovered state.
    bool hold_emits = false;

    // Admission (docs/CHECKPOINT_BACKPRESSURE_DESIGN.md 6, slice S1;
    // docs/S1_ADMISSION.md). Per shard, checked before an op's first
    // mutation; a refusal is E_OVERLOADED. The defaults are provisional,
    // derived in docs/S1_ADMISSION.md section 4; revisit before S6.
    // admission_legacy: the behaviour before S1 (one follow-up gate on
    // every execute, none on create or delete, no budgets).
    bool admission_legacy = false;
    u64 waiter_budget_bytes = 32ull << 20;      // durability waiters' memory
    u64 wal_buffer_budget_bytes = 30ull << 20;  // unflushed WAL record bytes
    u64 pending_budget = 131072;                // S1.1: forwarded requests in flight, per shard (0 refuses every forwarded request)

    // Checkpoint walk pacing (slice S2, docs/S2_PACING.md). ckpt_pacing
    // false restores the walk before S2 (256 objects per loop iteration);
    // the target, age and their gauges are reported either way. Every
    // value is a provisional hypothesis (docs/S2_PACING.md section 5); the
    // constructor refuses an inconsistent set (std::invalid_argument).
    bool ckpt_pacing = true;
    u32 ckpt_walk_batch = 16;                 // objects visited between clock reads
    u64 ckpt_walk_min_ns = 20'000;            // walk budget per loop iteration: floor
    u64 ckpt_walk_max_ns = 100'000;           //   top of the pressure-driven range
    u64 ckpt_walk_ceiling_ns = 250'000;       //   catch-up never above this
    u64 ckpt_walk_step_ns = 10'000;           //   additive increase
    u64 ckpt_pace_update_ns = 1'000'000;      // at most one budget change per this
    u32 ckpt_wal_high_pct = 200, ckpt_wal_low_pct = 125;         // flush EWMA vs baseline
    u32 ckpt_backlog_high_pct = 50, ckpt_backlog_low_pct = 25;   // follow-up backlog vs its gate
    u64 ckpt_target_ms = 30'000;              // target completion time (0 = none)
    u64 ckpt_max_age_ms = 300'000;            // hard maximum age (0 = none)
    u32 ckpt_warn_pct = 80;                   // warn at this percent of the hard maximum

    // Bounded checkpoint writeback (slice S3, docs/S3_WRITEBACK.md;
    // mechanism chosen in docs/S3D_WRITEBACK.md). ckpt_io_dsync: each part
    // chunk is written with RWF_DSYNC, at most ckpt_write_window chunks in
    // flight per shard, and the walk stops producing chunks while the
    // window is full. false (`--ckpt-io legacy`) restores the write path
    // before S3: buffered chunks, no window. The final fdatasync is kept in
    // both. The window is a provisional hypothesis; 1..64.
    bool ckpt_io_dsync = true;
    u32 ckpt_write_window = 4;
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
  // With Config::hold_emits: start running follow-ups.
  void release_emits();

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
  // The mint lease block this node holds, [lo, hi); 0, 0 without blocks.
  std::pair<u64, u64> lease_block() const { return {sb_.lease_lo, sb_.lease_hi}; }
  // With replica_degrade_gated: let sync shards continue without their
  // standby (true), or stop letting them (false; a shard already degraded
  // stays so until its standby catches up).
  void allow_degrade(bool on) { degrade_ok_.store(on); }
  // A primary that learns a newer primary of its group exists withdraws
  // from the data plane for good (docs/CLUSTER.md): node links closed,
  // follow-ups stopped, clients answered E_STANDBY. It happens by itself
  // when the promoted member refuses this primary's term on the replication
  // port; withdraw() does the same when the news comes another way (a
  // witness says the term was replaced). Only a restart (e.g. --rejoin)
  // brings the node back. Control path.
  void withdraw();
  bool fenced() const;
  // Tests only: every replication sender in this process stops shipping
  // (on) or resumes (off). See repl::set_debug_pause.
  static void debug_pause_replication(bool on);
  // Tests only: shard `shard` starts no WAL flush while on, so its appends
  // stay in the active buffer and its waiters wait (docs/S1_ADMISSION.md 6).
  static void debug_hold_wal(u16 shard, bool on);
  // Tests only: every shard's checkpoint walk visits no snapshot index at
  // or above `limit` (0 = no limit), holding the checkpoint in WALK at that
  // position (docs/S2_PACING.md section 6).
  static void debug_ckpt_walk_limit(u64 limit);
  // Tests only (S3, docs/S3_WRITEBACK.md section 8). Hold: checkpoint chunk
  // write completions are kept by their shard, unprocessed (the write
  // stays in flight and its buffer retained), until released: `n` more,
  // oldest or newest first; hold off processes the rest. Fail: the
  // completion of chunk `index` (0-based, of the next checkpoint to reach
  // it) reports one byte short (mode 1) or -EIO (mode 2); both are fatal.
  // Commit hold: Engine::checkpoint stops after every part is synced and
  // before the manifest, until released; reached() says it is there.
  static void debug_ckpt_hold_writes(bool on);
  // Defer: chunk writes are queued unissued (in flight, buffer owned) and
  // each release token issues one, oldest or newest first.
  static void debug_ckpt_defer_writes(bool on);
  static void debug_ckpt_release_writes(u32 n, bool newest_first);
  static void debug_ckpt_fail_write(u64 index, int mode);
  static void debug_ckpt_hold_commit(bool on);
  // Sync hold (S3.1): the final fdatasync's completion is kept by its
  // shard (the phase stays SYNC) until the hold is turned off.
  static void debug_ckpt_hold_sync(bool on);
  static bool debug_ckpt_commit_reached();
  static void debug_force_sync_io(bool on);
  // A role fact, not replication state (docs/HEALTH_DECOUPLING.md 3.1):
  // true once any shard has started a replication sender (at recovery as a
  // primary with a replica, or at promotion). Senders are never torn down
  // before stop, so it stays true through link loss, degraded running,
  // fencing and withdrawal. The same answer as replication_status().role
  // == 1, without a shard task. Safe from any thread.
  bool replicating() const;

  // ---- Slot moves (docs/MIGRATION.md). Control path; each blocks. ----
  // A newer placement: adopt it (false if `epoch` is not newer than the
  // current map's). Slots staged here that it gives this group activate.
  // `arrived`: the placement's arrivals (null: none on record).
  bool update_placement(const std::array<u8, kSlots>& slots, u64 epoch,
                        const std::array<SlotArrival, kSlots>* arrived = nullptr);
  // Asks Config::placement_fetch now and adopts what it returns if newer.
  bool refresh_placement();
  // The newest placement epoch seen from another node (0: none newer).
  u64 epoch_seen() const;
  SlotReport slot_report(u16 slot);
  // The shard that owns `slot` on this node.
  u16 slot_owner(u16 slot) const { return slots_.owner[slot]; }
  // Every slot state object this node holds (slot, phase, flags, move_id,
  // epoch, durable), for snc_ctl placement backfill.
  std::vector<SlotReport> slot_states();
  // Source: seal `slot` for move `move` (to `to`, committing at `epoch`).
  // OK once the seal is durable; from then on no op on the slot runs here.
  // Idempotent for the same move.
  Status seal_slot(u16 slot, u64 move, u16 to, u64 epoch, SlotReport& out, double timeout_s = 10);
  // Source: undo a seal whose move was aborted (the caller has checked
  // with the control plane). OK once durable.
  Status unseal_slot(u16 slot, u64 move, SlotReport& out, double timeout_s = 10);
  // Source: the slot's objects; those `have` lists at the same version and
  // crc go without payload. Not final: durable state only, sealed or not.
  // Final: only once `move`'s seal is durable, then every object.
  Status export_slot(u16 slot, u64 move, bool final, const std::vector<SlotObject>& have,
                     std::vector<SlotObject>& out, u64& count, u64& digest);
  // Destination: the objects it holds for a slot it does not serve.
  Status slot_objects(u16 slot, std::vector<SlotObject>& out);
  // Destination: stage objects of a slot this group does not serve. Not
  // final: add or replace them. Final: `objs` lists every object of the
  // sealed source; the staged copy is made equal to it (others deleted),
  // checked against count and digest, and recorded STAGED, durably.
  Status stage_slot(u16 slot, u64 move, u16 from, u64 epoch, bool final, const std::vector<SlotObject>& objs,
                    u64 count, u64 digest, SlotReport& out, double timeout_s = 10);
  // Tests only (snc_server DEBUG MOVE-STOP): the next final stage chain
  // stops before its `unit`-th unit (from 0) that starts in `phase`
  // (admit, check, put, scan, drop, stage; or reply: STAGED durable, not
  // yet answered), and holds there `hold_ms` (the shard runs other work
  // meanwhile), or exits the process as a crash would. Once.
  void debug_stage_stop(int phase, u32 unit, bool crash, u32 hold_ms);
  // The last final stage chain's measurements, one line (empty: none).
  std::string last_stage_report() const;

  // Replication state summed over shards, cheap enough to poll.
  struct ReplStatus {
    u8 role = 0;           // 0 none, 1 primary, 2 standby
    u16 live = 0;          // shards whose link is up
    u16 degraded = 0;      // primary: shards continuing without the standby
    u16 blocked = 0;       // primary: shards waiting for leave to degrade
    u16 shards = 0;
    // Primary (docs/SYNC_READINESS.md 3): shards whose current link has
    // proven the standby holds every durable record (a SYNCED may be sent
    // when all are), and shards whose standby came back without records it
    // had acknowledged (readiness lost: NOT READY must be recorded).
    u16 ready = 0;
    u16 regressed = 0;
  };
  ReplStatus replication_status() const;
  const RecoveryReport& recovery() const { return recovery_; }
  // Empty unless clustered. The current placement map.
  const ClusterMap& cluster() const { return *map_.load(); }
  // The space's UUID (after start, with a data_dir).
  const Uuid& space_uuid() const { return uuid_; }

  NativeRuntime& runtime() { return *rt_; }
  u16 shard_count() const { return cfg_.runtime.n_shards; }
  // Running: read on the shard's thread (control path). Stopped: final values.
  ShardStats stats(u16 shard) const;
  // Published gauges (include/snc/gauges.h): read from any thread with no
  // shard task and no lock a shard holds. Stopped: the last published
  // values. Observability only.
  GaugeSnapshot gauges(u16 shard) const;
  const NodeCkptGauges& checkpoint_gauges() const;

  // Control path: visit every live object owned by `shard` on its thread.
  // Intended for tests and checks, not the hot path.
  void for_each_object(u16 shard, const std::function<void(const ObjectHeader&)>& fn);
  // The same, for the objects whose slot this node serves at that moment
  // (its group's by the map, not sealed or staged; none on a standby or a
  // withdrawn primary). For audits (snc_server's AUDIT).
  void for_each_served_object(u16 shard, const std::function<void(const ObjectHeader&)>& fn);

 private:
  Config cfg_;
  std::unique_ptr<NativeRuntime> rt_;
  std::vector<class Shard*> shards_;
  std::atomic<bool> running_{false};
  // Lifecycle (docs/S3_WRITEBACK.md section 10): checkpoint() holds it for
  // its whole run and stop() takes it first, so a stop waits for a
  // checkpoint in progress to finish and a checkpoint never starts on, or
  // continues into, a stopping engine.
  std::mutex lifecycle_mu_;
  std::unique_ptr<EngineState> state_;

  // Persistence state (control thread only).
  void recover();
  // The start barrier (docs/MIGRATION_FAIL_CLOSED.md 5a).
  void reseal_from(const Placement& p);
  void finish_barrier(bool synced);
  // Seeding standbys (src/core/seed.cpp).
  struct Seeding;
  std::unique_ptr<Seeding> seed_;
  std::atomic<bool> degrade_ok_{false};
  void seed_fetch();
  void serve_seed();
  Superblock sb_{};
  Uuid uuid_{};
  SlotMap slots_;
  void take_lease_block(u16 n);
  ClusterMap cluster_;
  // Every map adopted so far (shards may still hold an older one's
  // address while they switch); the current one.
  std::vector<std::unique_ptr<ClusterMap>> maps_;
  std::atomic<const ClusterMap*> map_{&cluster_};
  std::mutex map_mu_;
  std::thread watcher_;
  std::atomic<bool> watcher_stop_{false};
  void watch_placement();
  Status wait_lsn(u16 shard, u64 lsn, double timeout_s);
  template <class F>
  auto on_shard(u16 shard, F fn) -> decltype(fn());
  // Runs step() on `shard` until it returns true, each call a task of its
  // own queued behind whatever the shard has waiting (no round trip to the
  // caller between calls); blocks until then.
  template <typename F>
  void on_shard_steps(u16 shard, F step);
  std::unique_ptr<SegmentPreparer> spares_;
  RecoveryReport recovery_;
  struct DebugStageStop {
    int phase = -1;
    u32 unit = 0, hold_ms = 0;
    bool crash = false;
  };
  mutable std::mutex stage_mu_;
  DebugStageStop debug_stage_;
  std::atomic<bool> debug_stage_armed_{false};
  std::string last_stage_;
  void debug_stage_point(int phase, u32 unit);
};

}  // namespace snc
