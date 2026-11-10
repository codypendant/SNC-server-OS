#include "shard.h"

#include "links_generation.h"

#include "session.h"
#include "slot_state.h"

#include "snc/sri.h"
#include "snc/trace.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace snc {

namespace {
constexpr u8 kMsgRespDurable = 1u << 4;  // RESP flag: resulting version is durable
constexpr u32 kCheckpointSlice = 256;    // objects per loop iteration (spec section 12)

thread_local Shard* tl_shard = nullptr;

// Tests only (Engine::debug_hold_wal): bit s set = shard s starts no WAL
// flush from its loop, so nothing it appends becomes durable. Shards s and
// s+64 share a bit; tests hold shards below 64 only.
std::atomic<u64> g_debug_hold_wal{0};

// Tests only (Engine::debug_ckpt_walk_limit): the walk visits no snapshot
// index at or above this (0 = no limit), so a test can hold a checkpoint
// at a chosen walk position. Never set outside tests.
std::atomic<u64> g_debug_ckpt_walk_limit{0};

// Tests only (S3; Engine::debug_ckpt_hold_writes and friends). Hold: chunk
// write completions are kept by their shard; release adds tokens, each
// lets one held completion be processed, oldest first unless `newest`.
// Fail: chunk index + 1 of the next checkpoint to reach it (0 = none),
// and how it fails: 1 one byte short, 2 -EIO. Consumed once.
std::atomic<bool> g_debug_ckpt_hold{false};
std::atomic<bool> g_debug_ckpt_defer{false};  // submissions queued unissued (same tokens)
std::atomic<u64> g_debug_ckpt_release{0};
std::atomic<bool> g_debug_ckpt_release_newest{false};
std::atomic<u64> g_debug_ckpt_fail_at{0};
std::atomic<int> g_debug_ckpt_fail_mode{0};
std::atomic<bool> g_debug_ckpt_hold_sync{false};  // the final fdatasync's completion held (S3.1)
// Tests only (S4; Engine::debug_ckpt_release_syncs, debug_ckpt_fail_sync).
// Release: while the sync hold is on, each token lets one held final-sync
// completion be processed. Fail: a bit per shard (id & 63); that shard's
// next final fdatasync completes with -EIO. Consumed once.
std::atomic<u64> g_debug_ckpt_sync_release{0};
std::atomic<u64> g_debug_ckpt_fail_sync{0};

[[noreturn]] void contract_violation(const char* what, const OpDescriptor* op) {
  std::fprintf(stderr, "snc: handler contract violated: %s (op %s)\n", what, op ? op->name : "?");
  std::abort();
}

void zero_header(Msg& m) { std::memset(static_cast<void*>(&m), 0, offsetof(Msg, inline_data)); }

}  // namespace

Shard* current_shard() { return tl_shard; }
void debug_hold_wal_flush(u16 shard, bool on) {
  const u64 bit = u64{1} << (shard & 63);
  if (on) g_debug_hold_wal.fetch_or(bit);
  else g_debug_hold_wal.fetch_and(~bit);
}
void debug_ckpt_walk_limit(u64 limit) { g_debug_ckpt_walk_limit.store(limit); }
void debug_ckpt_hold_writes(bool on) {
  if (on) {  // a new hold starts with no tokens left from an earlier one
    g_debug_ckpt_release.store(0);
    g_debug_ckpt_release_newest.store(false);
  }
  g_debug_ckpt_hold.store(on);
}
void debug_ckpt_defer_writes(bool on) {
  if (on) {
    g_debug_ckpt_release.store(0);
    g_debug_ckpt_release_newest.store(false);
  }
  g_debug_ckpt_defer.store(on);
}
void debug_ckpt_release_writes(u32 n, bool newest_first) {
  g_debug_ckpt_release_newest.store(newest_first);
  g_debug_ckpt_release.fetch_add(n);
}
void debug_ckpt_fail_write(u64 index, int mode) {
  g_debug_ckpt_fail_mode.store(mode);
  g_debug_ckpt_fail_at.store(mode == 0 ? 0 : index + 1);
}
void debug_ckpt_hold_sync(bool on) {
  if (on) g_debug_ckpt_sync_release.store(0);  // a new hold starts with no tokens left
  g_debug_ckpt_hold_sync.store(on);
}
void debug_ckpt_release_syncs(u32 n) { g_debug_ckpt_sync_release.fetch_add(n); }
void debug_ckpt_fail_sync(u16 shard) { g_debug_ckpt_fail_sync.fetch_or(u64{1} << (shard & 63)); }
void set_current_shard(Shard* s) { tl_shard = s; }

Shard::Shard(u16 id, const SlotMap& slots, u64 lease, u64 expected_objects, PageMode pages)
    : id_(id),
      slots_(slots),
      map_(expected_objects / (slots.n_shards ? slots.n_shards : 1) + 1024, pages),
      minter_(lease),
      alloc_(sri_allocator()),
      result_scratch_(kMaxResult) {
  // Intake off until set_intake (D.1): a Shard without it behaves as before.
  intake_ = Intake{false, 0, 0, false, 1, false, 0};
  const u16 n = slots_.n_shards ? slots_.n_shards : 1;
  sent_to_.assign(n, 0);
  taken_cache_.assign(n, 0);
  taken_from_.assign(n, 0);
  taken_pub_.assign(n, 0);
  ended_at_.assign(n, 0);
  ended_pub_.assign(n, 0);
  scan_.slots.assign(kSlots, 0);
  scan_.owed_slots.assign(kSlots, 0);
}

Shard::~Shard() {
  if (ckpt_.fd >= 0) ::close(ckpt_.fd);
  if (repl_epfd_ >= 0) ::close(repl_epfd_);
  // Objects live in the shard heap, which the backend destroys with the worker.
}

// ---- Entry-shard API -----------------------------------------------------------

namespace {
// A standby only installs what its primary sends (docs/REPLICATION.md).
void refuse_standby(const ObjectID& id, Completion done, void* ctx) {
  ExecResult r;
  r.status = Status::E_STANDBY;
  r.id = id;
  done(ctx, r);
}
// Another replication group owns it (docs/CONTROL_PLANE.md section 8): say
// which, by this node's map, and under which placement epoch.
void refuse_moved(const ClusterMap& map, const ObjectID& id, Completion done, void* ctx) {
  MovedResult m{map.epoch, map.group_of_id(id), {}};
  ExecResult r;
  r.status = Status::E_MOVED;
  r.id = id;
  r.result = ByteView(&m, sizeof(m));
  done(ctx, r);
}
}  // namespace

// A slot this shard owns but does not serve (docs/MIGRATION.md): another
// group's by this node's map (E_MOVED), or sealed or staged here
// (E_MIGRATING, with the seal's destination when sealed).
Status Shard::refuse_unserved(u16 slot, RunOut& out) {
  const bool moved = foreign_slot(slot);
  u16 group = cluster_->group_of[slot];
  if (!moved && phases_[slot].load(std::memory_order_relaxed) == static_cast<u8>(SlotPhase::SEALED)) {
    if (ObjectHeader* o = slot_state(slot)) {
      SlotStateV1 st;
      std::memcpy(&st, o->payload(), sizeof(st));
      group = st.group;
    }
  }
  moved_buf_ = MovedResult{cluster_->epoch, group, {}};
  out.r.status = moved ? Status::E_MOVED : Status::E_MIGRATING;
  out.r.result = ByteView(&moved_buf_, sizeof(moved_buf_));
  return out.r.status;
}

void Shard::execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts,
                    Completion done, void* ctx) {
  // A standby with reads on serves READ ops (checked where they run).
  if (repl_cfg_.standby && repl_cfg_.receiver.lease_ns == 0) return refuse_standby(target, done, ctx);
  if (withdrawn_) return refuse_standby(target, done, ctx);
  u64 h = snc_hash64(target);  // once: slot (top bits) and map index (low bits)
  // Not ours whatever the load: the client must go elsewhere either way.
  // Except a sequenced request: it may be a resend of one applied here
  // before the slot moved, and must get that answer, not E_MOVED (a client
  // told E_MOVED sends it again to the new owner, as a new request). Its
  // owner shard checks the session first, then refuses (run_exec).
  if (cluster_ && !opts.client_id && foreign_slot(static_cast<u16>(h >> 52))) {
    return refuse_moved(*cluster_, target, done, ctx);
  }
  // Follow-ups first: new client work waits while this shard's backlog of
  // follow-ups is long, so transfers settle instead of piling up.
  // S1 (docs/S1_ADMISSION.md 2): a READ this shard owns consumes nothing
  // the backlog drains (no WAL, no follow-up), so it is not refused for
  // it; one that must wait for durability is bounded by the waiter budget
  // below. A READ forwarded to another shard keeps the gate (unchanged in
  // S1.1, which adds the pending budget and the owner's waiter check).
  const u64 in_flight = emits_in_flight();
  if (in_flight > emits_peak_) emits_peak_ = in_flight;  // observability (S0)
  const OpDescriptor* desc = registry().op(op);
  const bool local_read =
      !adm_.legacy && desc != nullptr && desc->kind == OpKind::READ && slots_.owner[h >> 52] == id_;
  if (in_flight > kEmitBacklog && !local_read) {
    // A resent sequenced mutation already applied here gets its answer,
    // not the follow-up refusal: a client told plain E_OVERLOADED sends it
    // again as a new request, which would apply it twice. Promotion leaves
    // a backlog, and that is exactly when clients resend (found on real
    // hosts, docs/REAL_MACHINES.md). Its wait still takes the waiter budget
    // (S1.2): refused, it is E_OVERLOADED marked replayed, which a client
    // resends with the same sequence (docs/RETRIES.md).
    if (opts.client_id && slots_.owner[h >> 52] == id_) {
      const OpDescriptor* d = desc;
      RunOut out;
      out.r.id = target;
      if (d != nullptr && d->kind == OpKind::MUTATE && replay_if_seen(opts, out)) {
        (void)admit_answer_wait(out);
        return complete_local(out, done, ctx);
      }
    }
    ++stats_.overloaded;
    ++stats_.refused_followup;
    ExecResult r;
    r.status = Status::E_OVERLOADED;
    r.id = target;
    done(ctx, r);
    return;
  }
  u16 owner = slots_.owner[h >> 52];
  if (opts.client_id && owner != id_) {
    // A sequence is per shard and connection: forwarding could reorder it.
    ExecResult r;
    r.status = Status::E_NOT_OWNER;
    r.id = target;
    done(ctx, r);
    return;
  }
  if (owner == id_) {
    ++stats_.ops_local;
    RunOut out;
    out.r.id = target;
    run_exec(target, h, op, args, opts, out);
    // A READ (or any answer R1 did not reserve) that must wait takes a
    // waiter under the budget, or is refused: it changed nothing.
    (void)admit_answer_wait(out);
    complete_local(out, done, ctx);
    return;
  }
  if (pending_refuse(target, done, ctx)) return;
  ++stats_.ops_remote;
  send_request(MsgKind::EXEC_REQ, owner, target, op, args, opts, done, ctx);
}

void Shard::set_admission(const Admission& a) {
  adm_ = a;
  peer_record_max_ = 0;
  for (const TypeDescriptor& t : registry().types()) {
    peer_record_max_ = std::max<u64>(peer_record_max_, record_size(t.max_size));
  }
}

// D.1 (docs/FOLLOWUP_INTAKE_DESIGN.md 3.1): R is one follow-up apply's
// worst record group (its record at the largest type's max_size, plus a
// local() peer record), H defaults to max(2B, B + R), and H >= B + R is
// required, or clients admitted under B could keep the head follow-up
// from ever fitting (B5).
void Shard::set_intake(const Intake& in, FollowupBoard* board) {
  intake_ = in;
  board_ = board;
  u64 rmax = 0;
  for (const TypeDescriptor& t : registry().types()) rmax = std::max<u64>(rmax, record_size(t.max_size));
  followup_need_ = rmax + std::max(peer_record_max_, rmax);
  const u64 b = adm_.wal_buffer_budget_bytes;
  const u64 floor = b + followup_need_;
  wal_cap_ = in.wal_cap != 0 ? in.wal_cap : std::max(2 * b, floor);
  if (in.wal_gate && wal_cap_ < floor) {
    std::fprintf(stderr, "snc: shard %u: followup_wal_cap_bytes %llu is below B + R = %llu\n", id_,
                 (unsigned long long)wal_cap_, (unsigned long long)floor);
    std::abort();
  }
  if (in.exchange_gate && in.exchange_budget == 0) {
    std::fprintf(stderr, "snc: shard %u: exchange_budget must be at least 1\n", id_);
    std::abort();
  }
}

// R1 (docs/S1_ADMISSION.md 3). Changes no state. An empty buffer or an
// empty waiter list always admits, so an op larger than a budget still
// runs (alone) and a small budget cannot block a shard for good.
bool Shard::admit(u64 wal_need, u64 waiter_need, RunOut& out) {
  if (adm_.legacy || !wal_) return true;
  Status refuse = Status::OK;
  if (wal_need != 0 && wal_->active_bytes() != 0 &&
      wal_->active_bytes() + wal_need > adm_.wal_buffer_budget_bytes) {
    ++stats_.refused_wal_buffer;
    refuse = Status::E_OVERLOADED;
  } else if (waiter_need != 0 && !waiters_.empty() &&
             waiter_bytes_ + waiter_need > adm_.waiter_budget_bytes) {
    ++stats_.refused_waiter;
    refuse = Status::E_OVERLOADED;
  }
  if (refuse != Status::OK) {
    ++stats_.overloaded;
    out.r.status = refuse;
    return false;
  }
#ifndef NDEBUG
  adm_active_before_ = wal_->active_bytes();
  adm_wal_need_ = wal_need;
#endif
  return true;
}

// Debug builds: the op appended at most what admit() reserved, and no flush
// ran in between (a flush empties the active buffer: then active < before).
// This is R3's no-yield assumption, checked on every admitted mutation.
void Shard::check_reserved(const OpDescriptor* op) {
#ifndef NDEBUG
  if (adm_.legacy || !wal_) return;
  const u32 now = wal_->active_bytes();
  if (now < adm_active_before_ || now - adm_active_before_ > adm_wal_need_) {
    contract_violation("an admitted op appended more WAL bytes than reserved, or a flush ran inside it", op);
  }
#else
  (void)op;
#endif
}

bool Shard::admit_answer_wait(RunOut& out) {
#if !defined(_GLIBCXX_DEBUG)
  // tests/test_s1_admission.cpp (A2, A13) sizes exact waiter budgets from
  // this; a layout change must update those tests, not silently shift counts.
  static_assert(sizeof(Waiter) == 256, "update the waiter budgets in tests/test_s1_admission.cpp");
#endif
  if (adm_.legacy || out.waiter_reserved || out.wait_lsn == 0 || durable(out.wait_lsn)) return true;
  // REMOTE waiters hold the response (inline up to kMsgInlineBytes) and
  // LOCAL ones a copy of the result: sizeof(Waiter) + the result bounds both.
  if (admit(0, sizeof(Waiter) + out.r.result.len, out)) {
    out.waiter_reserved = true;
    return true;
  }
  // A refused replay keeps replayed = true: the op was applied and must be
  // resent with the same sequence, never as a new request (docs/RETRIES.md).
  if (out.r.replayed) ++stats_.refused_replay;
  out.wait_lsn = 0;
  out.r.result = ByteView{};
  out.r.version = 0;
  out.r.durable = false;
  out.r.app_code = 0;
  return false;
}

bool Shard::pending_refuse(ObjectID target, Completion done, void* ctx) {
  if (adm_.legacy || pending_live() < adm_.pending_budget) return false;
  ++stats_.overloaded;
  ++stats_.refused_pending;
  ExecResult r;
  r.status = Status::E_OVERLOADED;
  r.id = target;
  done(ctx, r);
  return true;
}

void Shard::create(u32 type_id, ByteView args, const ExecOptions& opts, Completion done,
                   void* ctx, const ObjectID* near) {
  if (repl_cfg_.standby || withdrawn_) return refuse_standby(ObjectID{}, done, ctx);
  if (!adm_.legacy && followups_refuse(opts, ObjectID{}, true, done, ctx)) return;
  ObjectID id = minter_.next();
  // Skipping sequence numbers keeps IDs unique and never reused (C12); with
  // N shards this takes N hashes per create on average, and kSlots to land
  // in a given slot.
  if (near != nullptr) {
    const u16 slot = slot_of(*near);
    if (opts.client_id && owner_of(*near) == id_) {
      // A resend of a create applied here before the slot moved gets its
      // answer, not E_MOVED (see execute).
      RunOut out;
      if (replay_if_seen(opts, out)) {
        (void)admit_answer_wait(out);
        return complete_local(out, done, ctx);
      }
    }
    if (foreign_slot(slot)) return refuse_moved(*cluster_, *near, done, ctx);
    while (slot_of(id) != slot) id = minter_.next();
  } else {
    // A sequenced create runs here, where its sequence is kept. Clustered,
    // only IDs in slots this node serves (docs/CLUSTER.md, docs/MIGRATION.md).
    while (((mint_local_ || opts.client_id) && owner_of(id) != id_) || !serving(slot_of(id))) id = minter_.next();
  }
  u16 owner = owner_of(id);
  if (opts.client_id && owner != id_) {
    ExecResult r;
    r.status = Status::E_NOT_OWNER;  // placed near an object another shard owns
    done(ctx, r);
    return;
  }
  if (owner == id_) {
    ++stats_.ops_local;
    RunOut out;
    run(MsgKind::CREATE_REQ, id, type_id, args, opts, out);
    (void)admit_answer_wait(out);
    complete_local(out, done, ctx);
    return;
  }
  if (pending_refuse(id, done, ctx)) return;
  ++stats_.ops_remote;
  send_request(MsgKind::CREATE_REQ, owner, id, type_id, args, opts, done, ctx);
}

void Shard::remove(ObjectID target, const ExecOptions& opts, Completion done, void* ctx) {
  if (repl_cfg_.standby || withdrawn_) return refuse_standby(target, done, ctx);
  // Deletes are never sequenced (no replay), so a refusal is the answer.
  if (!adm_.legacy && followups_refuse(opts, target, false, done, ctx)) return;
  if (foreign(target)) return refuse_moved(*cluster_, target, done, ctx);
  u16 owner = owner_of(target);
  if (owner == id_) {
    ++stats_.ops_local;
    RunOut out;
    run(MsgKind::DELETE_REQ, target, 0, ByteView{}, opts, out);
    (void)admit_answer_wait(out);
    complete_local(out, done, ctx);
    return;
  }
  if (pending_refuse(target, done, ctx)) return;
  ++stats_.ops_remote;
  send_request(MsgKind::DELETE_REQ, owner, target, 0, ByteView{}, opts, done, ctx);
}

// S1: creates and deletes are new mutation work, gated by the follow-up
// backlog as execute's mutations are (before S1 they had no gate). A
// resent sequenced create already applied here is answered, not refused.
bool Shard::followups_refuse(const ExecOptions& opts, ObjectID target, bool replay,
                             Completion done, void* ctx) {
  const u64 in_flight = emits_in_flight();
  if (in_flight > emits_peak_) emits_peak_ = in_flight;
  if (in_flight <= kEmitBacklog) return false;
  if (replay && opts.client_id) {
    RunOut out;
    if (replay_if_seen(opts, out)) {
      (void)admit_answer_wait(out);
      complete_local(out, done, ctx);
      return true;
    }
  }
  ++stats_.overloaded;
  ++stats_.refused_followup;
  ExecResult r;
  r.status = Status::E_OVERLOADED;
  r.id = target;  // null for a create: the follow-up gate runs before minting
  done(ctx, r);
  return true;
}

void Shard::prefetch(const ObjectID* ids, u32 n) {
  constexpr u32 kBatch = 64;
  u64 h[kBatch];
  for (u32 base = 0; base < n; base += kBatch) {
    u32 m = 0;
    for (u32 i = base; i < n && i < base + kBatch; ++i) {
      u64 x = snc_hash64(ids[i]);
      if (slots_.owner[x >> 52] != id_) continue;  // slot_of() uses the top 12 bits
      map_.prefetch_entry(x);
      h[m++] = x;
    }
    for (u32 i = 0; i < m; ++i) map_.prefetch_object(h[i]);
  }
}

void Shard::complete_local(RunOut& out, Completion done, void* ctx) {
  if (out.wait_lsn == 0 || durable(out.wait_lsn)) {
    if (out.wait_lsn != 0) out.r.durable = true;
    done(ctx, out.r);
    return;
  }
  Waiter w;
  w.lsn = out.wait_lsn;
  w.kind = Waiter::Kind::LOCAL;
  w.done = done;
  w.ctx = ctx;
  w.r = out.r;
  w.result.assign(out.r.result.data, out.r.result.data + out.r.result.len);
  defer(std::move(w));
}

// ---- Owner-side execution (spec section 6) -----------------------------------------

void Shard::run(MsgKind kind, ObjectID target, u32 op_or_type, ByteView args,
                const ExecOptions& opts, RunOut& out) {
  out = RunOut{};
  out.r.id = target;
  switch (kind) {
    case MsgKind::EXEC_REQ: run_exec(target, snc_hash64(target), op_or_type, args, opts, out); break;
    case MsgKind::CREATE_REQ: run_create(target, op_or_type, args, opts, out); break;
    case MsgKind::DELETE_REQ: run_delete(target, opts, out); break;
    default: std::abort();
  }
}

void Shard::prepare_ctx(OpCtx& c, ObjectHeader* obj, const TypeDescriptor* t,
                        const OpDescriptor* op) {
  c.obj_ = obj;
  c.type_ = t;
  c.op_ = op;
  // S1.1 (docs/S1_ADMISSION.md 3, R2): an op may grow its object within
  // the allocated capacity without reserve(), but never past max_size.
  // Recovery, standby apply and checkpoint load reject images larger than
  // max_size, so a longer object would make the WAL unrecoverable; and
  // admission reserves record_size(max_size).
  c.reserved_len_ = obj->capacity < t->max_size ? obj->capacity : t->max_size;
  c.app_code_ = 0;
  c.result_buf_ = result_scratch_.data();
  c.result_len_ = 0;
  c.max_result_ = op->max_result;
  c.source_ = cur_source_;
  c.peer_ = nullptr;
  c.applying_ = false;
  c.result_overflow_ = false;
  c.emits_reserved_ = 0;
  c.emits_done_ = 0;
}

Status Shard::validate_checked(OpCtx& c, const OpDescriptor* op, ByteView args) {
#ifndef NDEBUG
  const ObjectHeader* h = c.obj_;
  u64 before = XXH3_64bits(h, sizeof(ObjectHeader) + h->length);
  Status s = op->validate(c, *h, args);
  if (c.obj_ != h || XXH3_64bits(c.obj_, sizeof(ObjectHeader) + c.obj_->length) != before) {
    contract_violation("validate modified the object", op);
  }
  return s;
#else
  return op->validate(c, *c.obj_, args);
#endif
}

void Shard::finish_result(OpCtx& c, const OpDescriptor* op, ExecResult& out) {
  if (c.result_overflow_) contract_violation("result larger than max_result", op);
  out.result = ByteView(c.result_buf_, c.result_len_);
}

u64 Shard::log(RecType type, const ObjectHeader& obj, u16 flags) {
  if (!wal_) return 0;
  u32 len = type == RecType::DELETE ? 0 : obj.length;
  return wal_->append(type, obj.type_id, obj.id, obj.version, obj.schema_ver, obj.payload(), len,
                      flags);
}

void Shard::run_exec(ObjectID target, u64 hash, OpID op_id, ByteView args,
                     const ExecOptions& opts, RunOut& out, bool client) {
  if (client && withdrawn_) {
    out.r.status = Status::E_STANDBY;  // fenced: no longer this group's primary
    return;
  }
  if (client && repl_cfg_.standby) {
    // Reads on the standby (docs/REPLICATION.md): READ ops only, and only
    // under a lease, during which the primary acks no write this standby
    // lacks.
    const OpDescriptor* op = registry().op(op_id);
    if (op == nullptr || op->kind != OpKind::READ || repl_cfg_.receiver.lease_ns == 0) {
      out.r.status = Status::E_STANDBY;
      return;
    }
    if (!receiver_ || !receiver_->lease_valid(sri_now_ns())) {
      ++stats_.stale_reads;
      out.r.status = Status::E_STALE;
      return;
    }
  }
  if (client && opts.client_id) {
    // Only mutations are sequenced: a read runs again on a retry.
    const OpDescriptor* op = registry().op(op_id);
    if (op != nullptr && op->kind == OpKind::MUTATE && replay_if_seen(opts, out)) return;
  }
  // Checked here, on the owner, in the same thread as a seal: an op either
  // runs before the seal's record or is refused (docs/MIGRATION.md).
  if (cluster_ && !serving(static_cast<u16>(hash >> 52))) {
    refuse_unserved(static_cast<u16>(hash >> 52), out);
    return;
  }
  ObjectHeader* obj = map_.find(target, hash);
  if (obj == nullptr || obj->deleted()) {
    out.r.status = Status::E_NOT_FOUND;
    // A just-deleted object reports gone only once the delete is durable.
    if (obj && !opts.allow_undurable) out.wait_lsn = obj->last_lsn;
    return;
  }
  const OpDescriptor* op = registry().op(op_id);
  if (op == nullptr || (op->kind != OpKind::READ && op->kind != OpKind::MUTATE)) {
    out.r.status = Status::E_UNKNOWN_OP;
    return;
  }
  if (op->type_id != obj->type_id) {
    out.r.status = Status::E_TYPE_MISMATCH;
    return;
  }
  // The reply-only contract (docs/FOLLOWUP_INTAKE_DESIGN.md 3.3): follow-up
  // ops run only as follow-ups, and follow-ups run only follow-up ops.
  if (client && op->followup) {
    out.r.status = Status::E_BAD_ARGS;
    return;
  }
  if (!client && !op->followup) {
    // Every emit site checks this (OpCtx::emit), so only a frame from
    // another node (a different op table, or a bad frame) gets here: it
    // is dropped, never run, and the node keeps running.
    if (!bad_followup_logged_) {
      bad_followup_logged_ = true;
      std::fprintf(stderr, "snc: shard %u: dropped a follow-up to op %u, which is not a follow-up op\n", id_, op->op_id);
    }
    out.r.status = Status::E_BAD_ARGS;
    return;
  }
  if (args.len > op->max_args) {
    out.r.status = Status::E_BAD_ARGS;
    return;
  }
  out.r.version = obj->version;
  if (opts.expected_version != 0 && opts.expected_version != obj->version) {
    out.r.status = Status::E_VERSION_CONFLICT;
    return;
  }
  const TypeDescriptor* t = registry().type(obj->type_id);
  OpCtx c;
  prepare_ctx(c, obj, t, op);
  Status s = validate_checked(c, op, args);
  if (s != Status::OK) {
    if (!emit_scratch_.empty()) contract_violation("emit outside apply", op);
    out.r.status = s;
    out.r.app_code = c.app_code_;
    return;
  }
  if (op->kind == OpKind::READ) {
    if (op->apply) op->apply(c, args);
    out.r.durable = durable(c.obj_->last_lsn);
    if (!opts.allow_undurable) out.wait_lsn = c.obj_->last_lsn;
  } else {
    // Ops declared keeps_index_key skip index maintenance; debug builds
    // check the declaration.
#ifdef NDEBUG
    const bool track = t->index_id && !op->keeps_index_key;
#else
    const bool track = t->index_id != 0;
#endif
    // R1 (docs/S1_ADMISSION.md 3): a client's logged mutation reserves its
    // worst case here, after validate and before apply, its first change.
    // Follow-ups (client = false) and unlogged ops are not refused.
    // D.1 (docs/FOLLOWUP_INTAKE_DESIGN.md 3.9.1): an invocation that
    // reserved follow-ups starts that many exchanges; refused, before its
    // first change, while this shard's exchange budget is full. Sequenced
    // replays were answered above, before validate.
    if (client && c.emits_reserved_ != 0 && exchange_full()) {
      if (!emit_scratch_.empty()) contract_violation("emit outside apply", op);
      ++stats_.overloaded;
      ++stats_.refused_exchange;
      out.r.status = Status::E_OVERLOADED;
      return;
    }
    const bool reserve = client && !op->unlogged;
    if (reserve) {
      const u64 wal_need = record_size(t->max_size) + peer_record_max_ +
                           (opts.client_id ? record_size(sizeof(SessionV1)) : 0);
      const u64 waiter_need = opts.durability == Durability::DURABLE ? sizeof(Waiter) + op->max_result : 0;
      if (!admit(wal_need, waiter_need, out)) {
        if (!emit_scratch_.empty()) contract_violation("emit outside apply", op);
        return;
      }
      out.waiter_reserved = true;
    }
    u8 old_key[kMaxIndexKey], new_key[kMaxIndexKey];
    u32 old_len = track ? extract_key(*t, *obj, old_key) : 0;
    c.applying_ = true;
    if (op->apply) op->apply(c, args);
    ObjectHeader* o = c.obj_;
    if (op->unlogged) {
      if (!emit_scratch_.empty() || c.peer_ != nullptr) contract_violation("unlogged op emitted or used local()", op);
      out.r.durable = durable(o->last_lsn);
      out.r.version = o->version;
      finish_result(c, op, out.r);
      return;
    }
    ++o->version;
    ObjectHeader* peer = c.peer_;
#ifndef NDEBUG
    if (peer != nullptr) {
      const TypeDescriptor* pt = registry().type(peer->type_id);
      u8 key[kMaxIndexKey];
      u32 key_len = pt->index_id ? extract_key(*pt, *peer, key) : 0;
      if (peer->length != peer_len_ || key_len != peer_key_len_ ||
          std::memcmp(key, peer_key_, key_len) != 0) {
        contract_violation("op changed the length or index key of its local() object", op);
      }
    }
#endif
    if (peer != nullptr) {
      // Logged first, flagged as continuing into this op's record, so
      // recovery installs both or neither.
      ++peer->version;
      log(RecType::UPDATE, *peer, kRecGroupNext);
    }
    if (track) {
      u32 new_len = extract_key(*t, *o, new_key);
      if (new_len != old_len || std::memcmp(old_key, new_key, new_len) != 0) {
        if (op->keeps_index_key) contract_violation("op declared keeps_index_key changed the key", op);
        if (old_len) index_erase(t->index_id, old_key, old_len, o->id);
        if (new_len) index_insert(t->index_id, new_key, new_len, o->id);
      }
    }
    ObjectHeader* session = client && opts.client_id && op->kind == OpKind::MUTATE
                                ? record_session(opts, o->id, o->version)
                                : nullptr;
    o->last_lsn = log(RecType::UPDATE, *o);
    if (reserve) check_reserved(op);
    if (peer != nullptr) peer->last_lsn = o->last_lsn;  // durable with the group
    if (session != nullptr) session->last_lsn = o->last_lsn;
    if (!emit_scratch_.empty()) queue_emits(o->last_lsn);
    out.r.durable = durable(o->last_lsn);
    if (opts.durability == Durability::DURABLE) out.wait_lsn = o->last_lsn;
  }
  out.r.version = c.obj_->version;
  finish_result(c, op, out.r);
}

void Shard::run_create(ObjectID id, u32 type_id, ByteView args, const ExecOptions& opts,
                       RunOut& out) {
  if (opts.client_id && replay_if_seen(opts, out)) return;
  if (cluster_ && !serving(slot_of(id))) {
    refuse_unserved(slot_of(id), out);
    return;
  }
  const TypeDescriptor* t = registry().type(type_id);
  if (t == nullptr) {
    out.r.status = Status::E_TYPE_MISMATCH;
    return;
  }
  const OpDescriptor* op = registry().op(t->create_op);
  if (args.len > op->max_args) {
    out.r.status = Status::E_BAD_ARGS;
    return;
  }
  if (map_.find(id) != nullptr) {
    std::fprintf(stderr, "snc: ObjectID minted twice: %s\n", id.to_string().c_str());
    std::abort();
  }
  // R1: before the object is allocated and inserted, the create's first
  // change (validate runs on the inserted object; its failure is undone).
  if (!admit(record_size(t->max_size) + (opts.client_id ? record_size(sizeof(SessionV1)) : 0),
             sizeof(Waiter) + op->max_result, out)) {
    return;
  }
  out.waiter_reserved = true;
  std::size_t bytes = sizeof(ObjectHeader) + t->initial_size;
  auto* obj = static_cast<ObjectHeader*>(alloc_.alloc(bytes, kObjectAlign));
  if (obj == nullptr) {
    out.r.status = Status::E_NO_MEMORY;
    return;
  }
  std::memset(static_cast<void*>(obj), 0, bytes);
  obj->id = id;
  obj->type_id = t->type_id;
  obj->schema_ver = t->schema_ver;
  obj->length = t->initial_size;
  obj->capacity = static_cast<u32>(alloc_.usable(obj) - sizeof(ObjectHeader));
  obj->version = 0;  // becomes 1 once created

  // Insert first so a resize inside apply can update the map entry.
  map_.upsert(obj);
  OpCtx c;
  prepare_ctx(c, obj, t, op);
  Status s = validate_checked(c, op, args);
  if (s != Status::OK) {
    map_.erase(id);
    alloc_.free(obj);
    out.r.status = s;
    out.r.app_code = c.app_code_;
    return;
  }
  if (op->apply) op->apply(c, args);
  ObjectHeader* o = c.obj_;
  o->version = 1;
  if (t->index_id) {
    u8 key[kMaxIndexKey];
    u32 len = extract_key(*t, *o, key);
    if (len) index_insert(t->index_id, key, len, o->id);
  }
  ObjectHeader* session = opts.client_id ? record_session(opts, o->id, 1) : nullptr;
  o->last_lsn = log(RecType::CREATE, *o);
  check_reserved(op);
  if (session) session->last_lsn = o->last_lsn;
  ++stats_.creates;
  out.r.version = 1;
  out.r.durable = durable(o->last_lsn);
  out.wait_lsn = o->last_lsn;  // creates are always durable before completing
  finish_result(c, op, out.r);
}

void Shard::run_delete(ObjectID target, const ExecOptions& opts, RunOut& out, bool internal) {
  if (cluster_ && !serving(slot_of(target))) {
    refuse_unserved(slot_of(target), out);
    return;
  }
  ObjectHeader* obj = map_.find(target);
  if (obj == nullptr || obj->deleted()) {
    out.r.status = Status::E_NOT_FOUND;
    if (obj && !opts.allow_undurable) out.wait_lsn = obj->last_lsn;
    return;
  }
  out.r.version = obj->version;
  if (opts.expected_version != 0 && opts.expected_version != obj->version) {
    out.r.status = Status::E_VERSION_CONFLICT;
    return;
  }
  const TypeDescriptor* t = registry().type(obj->type_id);
  const OpDescriptor* dop = t->delete_op != 0 ? registry().op(t->delete_op) : nullptr;
  OpCtx c;
  if (dop != nullptr) {
    prepare_ctx(c, obj, t, dop);
    Status s = validate_checked(c, dop, ByteView{});
    if (s != Status::OK) {
      out.r.status = s;
      out.r.app_code = c.app_code_;
      return;
    }
  }
  // R1: after validate, before the delete op's apply or the index erase,
  // the first change. One DELETE record (no payload); the tombstone's
  // ERASE waiter, and the answer's waiter if it waits. Internal deletes
  // (session expiry) are the shard's own work and are not client admission.
  if (!internal &&
      !admit(record_size(0), sizeof(Waiter) * (opts.durability == Durability::DURABLE ? 2 : 1), out)) {
    return;
  }
  out.waiter_reserved = !internal;
  if (dop != nullptr) {
    if (dop->apply) dop->apply(c, ByteView{});
    obj = c.obj_;
  }
  if (t->index_id) {
    u8 key[kMaxIndexKey];
    u32 len = extract_key(*t, *obj, key);
    if (len) index_erase(t->index_id, key, len, obj->id);
  }
  ++obj->version;
  out.r.version = obj->version;
  ++stats_.deletes;
  if (!wal_) {
    map_.erase(target);
    alloc_.free(obj);
    out.r.durable = true;
    return;
  }
  // Spec section 8: free the payload now, keep a 48-byte tombstone until
  // the DELETE record is durable.
  u64 lsn = log(RecType::DELETE, *obj);
  if (!internal) check_reserved(dop);
  // Copy the header to a fresh block and swap the map entry before freeing
  // the old one: the map compares IDs through the stored pointer, so it must
  // never hold a freed block (a realloc here once left a stale entry behind).
  auto* tomb = static_cast<ObjectHeader*>(alloc_.alloc(sizeof(ObjectHeader), kObjectAlign));
  if (tomb != nullptr) {
    std::memcpy(static_cast<void*>(tomb), obj, sizeof(ObjectHeader));
    tomb->capacity = static_cast<u32>(alloc_.usable(tomb) - sizeof(ObjectHeader));
    map_.upsert(tomb);
    alloc_.free(obj);
  } else {
    tomb = obj;  // over the memory limit: keep the old block as the tombstone
  }
  tomb->length = 0;
  tomb->flags |= kFlagDeleted;
  tomb->last_lsn = lsn;
  Waiter w;
  w.lsn = lsn;
  w.kind = Waiter::Kind::ERASE;
  w.id = target;
  defer(std::move(w));
  out.r.durable = false;
  if (opts.durability == Durability::DURABLE) out.wait_lsn = lsn;
}

// ---- Durability waiters -----------------------------------------------------------

void Shard::defer(Waiter&& w) {
  ++stats_.deferred_completions;
  // Observability (S0): what held answers cost, for the waiter budget (S1).
  waiter_bytes_ += waiter_cost(w);
  if (waiter_bytes_ > waiter_bytes_peak_) waiter_bytes_peak_ = waiter_bytes_;
  if (waiters_.size() + 1 > waiters_peak_) waiters_peak_ = waiters_.size() + 1;
  // Mostly appended in LSN order; reads of older writes insert a little earlier.
  auto it = waiters_.end();
  while (it != waiters_.begin() && std::prev(it)->lsn > w.lsn) --it;
  waiters_.insert(it, std::move(w));
}

void Shard::on_wal_durable(void* ctx) { static_cast<Shard*>(ctx)->release_waiters(); }

void Shard::release_waiters() {
  while (!emit_wait_.empty() && durable(emit_wait_.front().lsn)) {
    route_emit(emit_wait_.front());
    emit_wait_.pop_front();
  }
  while (!waiters_.empty() && durable(waiters_.front().lsn)) {
    Waiter w = std::move(waiters_.front());
    waiters_.pop_front();
    waiter_bytes_ -= waiter_cost(w);
    switch (w.kind) {
      case Waiter::Kind::LOCAL:
        w.r.durable = true;
        w.r.result = ByteView(w.result.data(), static_cast<u32>(w.result.size()));
        w.done(w.ctx, w.r);
        break;
      case Waiter::Kind::REMOTE:
        w.resp.flags |= kMsgRespDurable;
        sri_send(w.dst, w.resp, /*must_deliver=*/true);
        if (w.return_args) return_buffer(w.dst, w.return_args);
        break;
      case Waiter::Kind::ERASE: {
        ObjectHeader* o = map_.find(w.id);
        if (o && o->deleted()) {
          map_.erase(w.id);
          alloc_.free(o);
        }
        break;
      }
    }
  }
}

bool Shard::poll() {
  if (!withdrawn_ && ((sender_ && sender_->fenced()) || (withdraw_flag_ && withdraw_flag_->load(std::memory_order_relaxed)))) {
    withdraw();
  }
  bool busy = run_emits();
  busy |= resume_step();
  if (links_) busy |= poll_links();
  if (wal_ && !((g_debug_hold_wal.load(std::memory_order_relaxed) >> (id_ & 63)) & 1)) {
    busy |= wal_->poll(sri_now_ns(), /*force=*/false);
  }
  if (sender_ || receiver_) busy |= poll_replication();
  if (fence_listener_) busy |= fence_listener_->poll(0, 0, 0, sri_now_ns());
  if (ckpt_.active) {
    checkpoint_step();
    busy = true;
  }
  publish_intake();
  if (gauges_) publish_gauges(sri_now_ns());
  return busy;
}

void Shard::publish_gauges(u64 now, bool force) {
  if (gauges_ == nullptr) return;
  if (!force && !gauges_dirty_ && now - gauges_published_ns_ < ShardGauges::kPublishNs) return;
  gauges_dirty_ = false;
  gauges_published_ns_ = now;
  std::array<u64, kGaugeCount> v{};
  v[G_REFUSED_FOLLOWUP] = stats_.refused_followup;
  v[G_REFUSED_RING] = stats_.refused_ring;
  v[G_REFUSED_WAITER] = stats_.refused_waiter;
  v[G_REFUSED_WAL_BUFFER] = stats_.refused_wal_buffer;
  v[G_REFUSED_PENDING] = stats_.refused_pending;
  v[G_PENDING_NOW] = pending_live();
  v[G_PENDING_PEAK] = stats_.pending_peak;
  v[G_REFUSED_REPLAY] = stats_.refused_replay;
  v[G_GATHER_OVERLOADED] = stats_.gather_overloaded;
  v[G_WAITERS_NOW] = waiters_.size();
  v[G_WAITERS_PEAK] = waiters_peak_;
  v[G_WAITER_BYTES_NOW] = waiter_bytes_;
  v[G_WAITER_BYTES_PEAK] = waiter_bytes_peak_;
  const u64 in_flight = emits_in_flight();
  if (in_flight > emits_peak_) emits_peak_ = in_flight;
  v[G_EMITS_NOW] = in_flight;
  v[G_EMITS_PEAK] = emits_peak_;
  if (wal_) {
    const WalStream::Stats& ws = wal_->stats();
    v[G_WAL_PRESENT] = 1;
    v[G_WAL_DIRECT] = wal_->direct();
    v[G_WAL_ACTIVE_BYTES_NOW] = wal_->active_bytes();
    v[G_WAL_ACTIVE_BYTES_PEAK] = ws.active_bytes_peak;
    v[G_WAL_BUFFER_CAP_BYTES] = wal_->buffer_cap_bytes();
    v[G_WAL_NEXT_LSN] = wal_->next_lsn();
    v[G_WAL_DURABLE_LSN] = wal_->durable_lsn();
    v[G_WAL_UNDURABLE] = wal_->next_lsn() - 1 - wal_->durable_lsn();
    v[G_WAL_FLUSHES] = ws.flushes;
    v[G_WAL_FLUSH_EWMA_US] = ws.flush_us_ewma.value();
    v[G_WAL_FLUSH_MAX_US] = ws.flush_us_max;
    v[G_WAL_FLUSH_SINCE_NS] = wal_->flush_since_ns();
  }
  const CkptObs& o = ckpt_obs_;
  const bool idle = o.phase == CkptPhase::IDLE;
  v[G_CKPT_PHASE] = static_cast<u64>(o.phase);
  v[G_CKPT_SEQ] = o.seq;
  v[G_CKPT_STARTED_NS] = idle ? 0 : o.started_ns;
  v[G_CKPT_PHASE_SINCE_NS] = idle ? 0 : o.phase_since_ns;
  v[G_CKPT_OBJECTS] = ckpt_.active ? ckpt_.objects : o.last_objects;
  v[G_CKPT_BYTES_ISSUED] = ckpt_.active ? ckpt_.file_off : o.last_bytes_issued;
  v[G_CKPT_BYTES_WRITTEN] = o.bytes_written;
  v[G_CKPT_WRITES_INFLIGHT] = ckpt_.active ? ckpt_.writes_inflight : 0;
  v[G_CKPT_WRITES_INFLIGHT_PEAK] = o.writes_peak;
  v[G_CKPT_LAST_SNAPSHOT_US] = o.last_snapshot_us;
  v[G_CKPT_LAST_WALK_US] = o.last_walk_us;
  v[G_CKPT_LAST_DRAIN_US] = o.last_drain_us;
  v[G_CKPT_LAST_DURABLE_WAIT_US] = o.last_durable_wait_us;
  v[G_CKPT_LAST_SYNC_US] = o.last_sync_us;
  v[G_CKPT_LAST_TOTAL_US] = o.last_total_us;
  v[G_CKPT_LAST_RESULT] = o.last_result;
  v[G_CKPT_COMPLETED] = o.completed;
  const bool active = ckpt_.active;
  v[G_CKPT_PACING] = pacing_.on ? 1 : 0;
  // With pacing off the budget and mode are not acting: reported as 0.
  // Pressure, age and target are reports, published either way.
  v[G_CKPT_WALK_BUDGET_NS] = active && pacing_.on ? pacer_.walk_ns : 0;
  v[G_CKPT_MODE] = active && pacing_.on ? static_cast<u64>(pacer_.mode) : 0;
  v[G_CKPT_PRESSURE] = active ? static_cast<u64>(pacer_.pressure) : 0;
  v[G_CKPT_AGE_STATE] = active ? static_cast<u64>(pacer_.age) : 0;
  v[G_CKPT_PAST_TARGET] = active && pacer_.past_target ? 1 : 0;
  v[G_CKPT_PAST_TARGET_COUNT] = o.past_target_count;
  v[G_CKPT_DEGRADED_COUNT] = o.degraded_count;
  v[G_CKPT_TARGET_MS] = pacing_.cfg.target_ns / 1'000'000;
  v[G_CKPT_MAX_AGE_MS] = pacing_.cfg.max_age_ns / 1'000'000;
  v[G_CKPT_WALK_TOTAL] = o.walk_total;
  v[G_CKPT_WALK_DONE] = active && !ckpt_.walk_done ? ckpt_.cursor : o.walk_total;
  v[G_CKPT_WALK_STEPS] = o.walk_steps;
  v[G_CKPT_CATCHUP_STEPS] = o.catchup_steps;
  v[G_CKPT_STEP_MAX_NS] = o.step_max_ns;
  v[G_CKPT_STEP_MAX_OBJECTS] = o.step_max_objects;
  v[G_CKPT_WAL_BASELINE_US] = o.wal_baseline_us;
  v[G_CKPT_IO_DSYNC] = ckpt_io_.dsync ? 1 : 0;
  v[G_CKPT_WINDOW] = ckpt_io_.dsync ? ckpt_io_.window : 0;
  v[G_CKPT_WINDOW_FULL] = active && ckpt_.blocked ? 1 : 0;
  v[G_CKPT_FINAL_PENDING] = active && ckpt_.blocked && ckpt_.visited ? 1 : 0;
  v[G_CKPT_WINDOW_FULL_STEPS] = o.window_full_steps;
  v[G_CKPT_CHUNKS] = o.chunks;
  v[G_CKPT_BUF_BYTES] = active ? ckpt_buf_bytes() : 0;
  v[G_CKPT_BUF_BYTES_PEAK] = o.buf_bytes_peak;
  v[G_CKPT_WRITE_LAT_LAST_US] = o.write_lat_last_us;
  v[G_CKPT_WRITE_LAT_EWMA_US] = o.write_lat_ewma_us;
  v[G_CKPT_WRITE_LAT_MAX_US] = o.write_lat_max_us;
  v[G_CKPT_WRITE_LAT_LT1MS] = o.write_lat_hist[0];
  v[G_CKPT_WRITE_LAT_LT10MS] = o.write_lat_hist[1];
  v[G_CKPT_WRITE_LAT_LT100MS] = o.write_lat_hist[2];
  v[G_CKPT_WRITE_LAT_GE100MS] = o.write_lat_hist[3];
  v[G_CKPT_SYNC_WRITES] = o.sync_writes;
  v[G_CKPT_SYNC_WRITES_TOTAL] = o.sync_writes_total;
  v[G_CKPT_SYNC_TOKENS] = sync_tokens_ != nullptr ? sync_tokens_->limit : 0;
  const bool token_wait = active && ckpt_.ticketed && !ckpt_.token_held;
  v[G_CKPT_SYNC_STATE] = token_wait ? 1 : active && ckpt_.syncing ? 2 : 0;
  v[G_CKPT_TOKEN_WAIT_SINCE_NS] = token_wait ? o.token_request_ns : 0;
  v[G_CKPT_TOKEN_WAIT_LAST_US] = o.token_wait_last_us;
  v[G_CKPT_TOKEN_WAIT_MAX_US] = o.token_wait_max_us;
  v[G_CKPT_TOKEN_WAIT_EWMA_US] = o.token_wait_ewma_us;
  v[G_CKPT_TOKEN_ACQUIRED] = o.token_acquired;
  v[G_CKPT_TOKEN_TICKET] = o.token_ticket;
  // D.1 intake: the settings in force and its state, so no backpressure
  // decision is internal-only.
  v[G_INTAKE_WAL_GATE] = intake_.wal_gate && wal_ ? 1 : 0;
  v[G_INTAKE_WAL_CAP_BYTES] = wal_cap_;
  v[G_INTAKE_NEED_BYTES] = followup_need_;
  v[G_INTAKE_WINDOW] = board_ != nullptr ? intake_.window : 0;
  v[G_INTAKE_EXCHANGE_GATE] = intake_.exchange_gate ? 1 : 0;
  v[G_INTAKE_EXCHANGE_BUDGET] = intake_.exchange_gate ? intake_.exchange_budget : 0;
  v[G_INTAKE_OWNER_GATE] = intake_.owner_gate && !adm_.legacy ? 1 : 0;
  v[G_INTAKE_LINK_HOLD_MAX] = intake_.link_hold_max;
  v[G_INTAKE_GATE_OPEN] = followup_gate_open() ? 1 : 0;
  v[G_INTAKE_GATED] = stats_.followups_gated;
  v[G_INTAKE_REFUSED_EXCHANGE] = stats_.refused_exchange;
  v[G_INTAKE_REFUSED_OWNER] = stats_.refused_owner;
  v[G_INTAKE_EXCHANGES_LIVE_PEAK] = stats_.exchanges_live_peak;
  v[G_INTAKE_EXCHANGES_STARTED] = stats_.exchanges_started;
  v[G_INTAKE_EXCHANGES_ENDED] = stats_.exchanges_ended;
  v[G_INTAKE_STAGED_NOW] = emit_local_.size();
  v[G_INTAKE_STAGED_PEAK] = stats_.followups_staged_peak;
  {
    // Read as stats() does: no cache this shard decides by is touched.
    u64 elsewhere = 0, outstanding = 0;
    if (board_ != nullptr) {
      for (u16 x = 0; x < board_->size(); ++x) {
        if (x == id_) continue;
        elsewhere += board_->ended(x, id_).load(std::memory_order_acquire);
        outstanding += sent_to_[x] - board_->taken(x, id_).load(std::memory_order_acquire);
      }
    }
    v[G_INTAKE_EXCHANGES_LIVE] = started_ - ended_own_ - elsewhere;
    v[G_INTAKE_WINDOW_OUTSTANDING] = outstanding;
  }
  v[G_INTAKE_WINDOW_WAITS] = stats_.window_waits;
  v[G_INTAKE_LINK_HELD_NOW] = link_held_;
  v[G_INTAKE_LINK_HELD_PEAK] = stats_.link_held_peak;
  v[G_INTAKE_LINK_PAUSED] = links_paused_ ? 1 : 0;
  v[G_INTAKE_LINK_PAUSES] = stats_.link_pauses;
  v[G_INTAKE_RESUME_ACTIVE] = scan_.active ? 1 : 0;
  v[G_INTAKE_RESUME_OWED] = scan_.owed_all || scan_.owed_any ? 1 : 0;
  v[G_INTAKE_RESUME_PASSES] = stats_.resume_passes;
  v[G_INTAKE_RESUME_TRIGGERS] = stats_.resume_triggers;
  v[G_INTAKE_RESUME_COALESCED] = stats_.resume_coalesced;
  v[G_INTAKE_RESUME_RESTARTS] = stats_.resume_restarts;
  v[G_INTAKE_RESUME_OBJECTS] = stats_.resume_objects;
  v[G_PUBLISHED_NS] = now;
  gauges_->publish(v);
}

bool Shard::quiesced() const {
  // A primary stops only once a connected standby has everything.
  if (sender_ && sender_->live() && sender_->acked_lsn() + 1 < wal_->next_lsn()) return false;
  return (!wal_ || wal_->idle()) && waiters_.empty() && !ckpt_.active &&
         free_pending_.size() == pending_.size() && free_gathers_.size() == gathers_.size();
}

// ---- Client sessions (docs/RETRIES.md) ---------------------------------------------

ObjectHeader* Shard::find_session(u64 client_id) {
  auto mi = indexes_.find(kSessionIndexId);
  if (mi == indexes_.end()) return nullptr;
  auto it = mi->second.find(std::string(reinterpret_cast<const char*>(&client_id), sizeof(client_id)));
  if (it == mi->second.end() || it->second.empty()) return nullptr;
  ObjectHeader* o = map_.find(it->second.front());
  return o && !o->deleted() ? o : nullptr;
}

// A sequenced op this shard already applied (seq <= the session's last) is
// answered from the session, once that record is durable, not run again.
bool Shard::replay_if_seen(const ExecOptions& opts, RunOut& out) {
  ObjectHeader* s = find_session(opts.client_id);
  if (s == nullptr) return false;
  SessionV1 v;
  std::memcpy(&v, s->payload(), sizeof(v));
  if (opts.seq > v.last_seq) return false;
  out.r.replayed = true;
  if (opts.seq == v.last_seq) {
    out.r.status = static_cast<Status>(v.last_status);
    out.r.app_code = v.last_app_code;
    out.r.id = v.last_id;
    out.r.version = v.last_version;
  } else {
    out.r.status = Status::E_REPLAYED_OLD;
  }
  out.r.durable = durable(s->last_lsn);
  out.wait_lsn = s->last_lsn;
  // S1.2: no waiter is reserved here (waiter_reserved stays false). Every
  // caller passes the answer through admit_answer_wait, which takes the
  // waiter budget if the session record is not yet durable, or refuses
  // with E_OVERLOADED and replayed kept set. Nothing here changes state,
  // so a refused replay leaves object, session and WAL as they were.
  ++stats_.replayed;
  return true;
}

ObjectHeader* Shard::record_session(const ExecOptions& opts, const ObjectID& id, u64 version) {
  ObjectHeader* s = find_session(opts.client_id);
  RecType type = RecType::UPDATE;
  if (s == nullptr) {
    const TypeDescriptor* t = registry().type(kSessionTypeId);
    ObjectID sid = minter_.next();
    while (owner_of(sid) != id_ || foreign(sid)) sid = minter_.next();
    std::size_t bytes = sizeof(ObjectHeader) + sizeof(SessionV1);
    s = static_cast<ObjectHeader*>(alloc_.alloc(bytes, kObjectAlign));
    if (s == nullptr) {
      std::fprintf(stderr, "snc: out of memory for a client session on shard %u\n", id_);
      std::abort();
    }
    std::memset(static_cast<void*>(s), 0, bytes);
    s->id = sid;
    s->type_id = kSessionTypeId;
    s->schema_ver = t->schema_ver;
    s->length = sizeof(SessionV1);
    s->capacity = static_cast<u32>(alloc_.usable(s) - sizeof(ObjectHeader));
    map_.upsert(s);
    index_insert(kSessionIndexId, reinterpret_cast<const u8*>(&opts.client_id),
                 sizeof(opts.client_id), sid);
    type = RecType::CREATE;
  }
  SessionV1 v{opts.client_id, opts.seq, id, version, static_cast<u16>(Status::OK), 0,
              static_cast<u32>(::time(nullptr))};
  std::memcpy(s->payload(), &v, sizeof(v));
  ++s->version;
  log(type, *s, kRecGroupNext);
  return s;
}

u64 Shard::expire_sessions(u32 now_s, u32 ttl_s) {
  std::vector<ObjectID> old;
  map_.for_each([&](const ObjectHeader* o) {
    if (o->type_id != kSessionTypeId || o->deleted()) return;
    SessionV1 v;
    std::memcpy(&v, o->payload(), sizeof(v));
    if (v.touched_s + static_cast<u64>(ttl_s) < now_s) old.push_back(o->id);
  });
  u64 n = 0;
  for (const ObjectID& id : old) {
    RunOut out;
    run_delete(id, ExecOptions{}, out, /*internal=*/true);
    n += out.r.status == Status::OK;
  }
  stats_.sessions_expired += n;
  return n;
}

// ---- Index partition (spec section 11) ----------------------------------------------

u32 Shard::extract_key(const TypeDescriptor& t, const ObjectHeader& obj, u8* key) const {
  const IndexDescriptor* idx = registry().index(t.index_id);
  u32 len = idx->extract(obj.payload(), obj.length, key);
  return std::min(len, kMaxIndexKey);
}

void Shard::index_insert(u32 index_id, const u8* key, u32 len, const ObjectID& id) {
  indexes_[index_id][std::string(reinterpret_cast<const char*>(key), len)].push_back(id);
}

void Shard::index_erase(u32 index_id, const u8* key, u32 len, const ObjectID& id) {
  auto& m = indexes_[index_id];
  auto it = m.find(std::string(reinterpret_cast<const char*>(key), len));
  if (it == m.end()) return;
  auto& v = it->second;
  v.erase(std::remove(v.begin(), v.end(), id), v.end());
  if (v.empty()) m.erase(it);
}

void Shard::index_find(u32 index_id, ByteView key, u32 max_results,
                       std::vector<ObjectID>& out) const {
  auto mi = indexes_.find(index_id);
  if (mi == indexes_.end()) return;
  auto it = mi->second.find(std::string(reinterpret_cast<const char*>(key.data), key.len));
  if (it == mi->second.end()) return;
  for (const ObjectID& id : it->second) {
    if (out.size() >= max_results) break;
    const ObjectHeader* o = map_.find(id);
    // Only durable state is visible (D7); a racing update may hide an entry.
    // A slot sealed or staged here is not this group's to show.
    if (o && !o->deleted() && durable(o->last_lsn) && serving(slot_of(id))) out.push_back(id);
  }
}

void Shard::rebuild_index() {
  indexes_.clear();
  u8 key[kMaxIndexKey];
  map_.for_each([&](const ObjectHeader* o) {
    if (o->deleted()) return;
    const TypeDescriptor* t = registry().type(o->type_id);
    if (!t->index_id) return;
    u32 len = extract_key(*t, *o, key);
    if (len) index_insert(t->index_id, key, len, o->id);
  });
}

void Shard::index_lookup(u32 index_id, ByteView key, u32 max_results, IndexCompletion done,
                         void* ctx) {
  if (repl_cfg_.standby || withdrawn_) {
    done(ctx, Status::E_STANDBY, nullptr, 0);
    return;
  }
  if (registry().index(index_id) == nullptr || key.len > kMaxIndexKey) {
    done(ctx, Status::E_BAD_ARGS, nullptr, 0);
    return;
  }
  u32 slot;
  if (free_gathers_.empty()) {
    slot = static_cast<u32>(gathers_.size());
    gathers_.emplace_back();
  } else {
    slot = free_gathers_.back();
    free_gathers_.pop_back();
  }
  Gather& g = gathers_[slot];
  g.done = done;
  g.ctx = ctx;
  ++g.gen;
  g.remaining = slots_.n_shards;
  g.max_results = max_results;
  g.status = Status::OK;
  g.ids.clear();

  for (u16 s = 0; s < slots_.n_shards; ++s) {
    if (s == id_) continue;
    Msg m;
    zero_header(m);
    m.kind = MsgKind::INDEX_QUERY;
    m.src_shard = id_;
    m.op_id = index_id;
    m.req_id = (u64{g.gen} << 32) | slot;
    m.expected_version = max_results;
    m.len = key.len;
    if (key.len <= kMsgInlineBytes) {
      m.flags = kMsgArgsInline;
      std::memcpy(m.inline_data, key.data, key.len);
    } else {
      m.flags = kMsgArgsExternal;
      m.ext.ptr = static_cast<u8*>(alloc_.alloc(key.len, 16));
      if (m.ext.ptr == nullptr) {
        gathers_[slot].status = Status::E_NO_MEMORY;
        --gathers_[slot].remaining;
        continue;
      }
      std::memcpy(m.ext.ptr, key.data, key.len);
    }
    if (!sri_send(s, m)) {
      if (m.flags & kMsgArgsExternal) alloc_.free(m.ext.ptr);
      gathers_[slot].status = Status::E_OVERLOADED;
      ++stats_.gather_overloaded;
      --gathers_[slot].remaining;
    }
  }
  // Own partition last, inline.
  index_find(index_id, key, max_results, gathers_[slot].ids);
  --gathers_[slot].remaining;
  if (gathers_[slot].remaining == 0) finish_gather(slot);
}

void Shard::finish_gather(u32 slot) {
  Gather& g = gathers_[slot];
  IndexCompletion done = g.done;
  void* ctx = g.ctx;
  Status st = g.status;
  std::vector<ObjectID> ids;
  ids.swap(g.ids);
  if (ids.size() > g.max_results) ids.resize(g.max_results);
  g.done = nullptr;
  free_gathers_.push_back(slot);
  done(ctx, st, ids.data(), static_cast<u32>(ids.size()));
}

void Shard::serve_index(const Msg& m) {
  ++stats_.index_lookups_served;
  std::vector<ObjectID> ids;
  index_find(m.op_id, ByteView(m.data(), m.len), static_cast<u32>(m.expected_version), ids);
  Msg resp;
  zero_header(resp);
  resp.kind = MsgKind::INDEX_RESP;
  resp.src_shard = id_;
  resp.req_id = m.req_id;
  u32 bytes = static_cast<u32>(ids.size() * sizeof(ObjectID));
  resp.len = bytes;
  if (bytes <= kMsgInlineBytes) {
    resp.flags = kMsgArgsInline;
    if (bytes) std::memcpy(resp.inline_data, ids.data(), bytes);
  } else {
    resp.flags = kMsgArgsExternal;
    resp.ext.ptr = static_cast<u8*>(alloc_.alloc(bytes, 16));
    if (resp.ext.ptr == nullptr) {
      resp.flags = kMsgArgsInline;
      resp.len = 0;
      resp.status = static_cast<u16>(Status::E_NO_MEMORY);
    } else {
      std::memcpy(resp.ext.ptr, ids.data(), bytes);
    }
  }
  sri_send(m.src_shard, resp, /*must_deliver=*/true);
  if (m.flags & kMsgArgsExternal) return_buffer(m.src_shard, m.ext.ptr);
}

void Shard::complete_index(const Msg& m) {
  u32 slot = static_cast<u32>(m.req_id);
  Gather& g = gathers_.at(slot);
  if (g.gen != static_cast<u32>(m.req_id >> 32) || g.done == nullptr) {
    std::fprintf(stderr, "snc: index response for unknown request on shard %u\n", id_);
    std::abort();
  }
  if (m.status != 0) g.status = static_cast<Status>(m.status);
  u32 n = m.len / sizeof(ObjectID);
  const u8* p = m.data();
  for (u32 i = 0; i < n; ++i) {
    ObjectID id;
    std::memcpy(&id, p + i * sizeof(ObjectID), sizeof(ObjectID));
    g.ids.push_back(id);
  }
  if (m.flags & kMsgArgsExternal) return_buffer(m.src_shard, m.ext.ptr);
  if (--g.remaining == 0) finish_gather(slot);
}

// ---- Cross-shard dispatch (spec section 7) ----------------------------------------

void Shard::on_message(const Msg& m) {
  switch (m.kind) {
    case MsgKind::EXEC_REQ:
    case MsgKind::CREATE_REQ:
    case MsgKind::DELETE_REQ: serve(m); break;
    case MsgKind::RESP: complete(m); break;
    case MsgKind::INDEX_QUERY: serve_index(m); break;
    case MsgKind::INDEX_RESP: complete_index(m); break;
    case MsgKind::BUF_RETURN: alloc_.free(m.ext.ptr); break;
    case MsgKind::EMIT: serve_emit(m); break;
    case MsgKind::MULTI_REQ: serve_multi(m); break;
    case MsgKind::MULTI_RESP: complete_multi(m); break;
    default:
      std::fprintf(stderr, "snc: shard %u got unexpected message kind %u\n", id_,
                   static_cast<unsigned>(m.kind));
      std::abort();
  }
}

void Shard::send_request(MsgKind kind, u16 dst, ObjectID target, u32 op_or_type, ByteView args,
                         const ExecOptions& opts, Completion done, void* ctx) {
  u32 slot;
  if (free_pending_.empty()) {
    slot = static_cast<u32>(pending_.size());
    pending_.emplace_back();
  } else {
    slot = free_pending_.back();
    free_pending_.pop_back();
  }
  Pending& p = pending_[slot];
  p.done = done;
  p.ctx = ctx;
  p.target = target;
  ++p.gen;
  if (pending_live() > stats_.pending_peak) stats_.pending_peak = pending_live();

  if (batch_remote_ && kind == MsgKind::EXEC_REQ && args.len <= kMsgInlineBytes &&
      opts.expected_version == 0) {
    if (out_batches_.size() <= dst) out_batches_.resize(slots_.n_shards);
    OutBatch& b = out_batches_[dst];
    if (b.recs == nullptr) {
      b.recs = static_cast<BatchReq*>(alloc_.alloc(sizeof(BatchReq) * kBatchMax, 64));
    }
    if (b.recs != nullptr) {
      if (b.n == 0) batch_dsts_.push_back(dst);
      BatchReq& q = b.recs[b.n++];
      q.target = target;
      q.op = op_or_type;
      q.slot = slot;
      q.gen = p.gen;
      q.flags = static_cast<u8>((opts.allow_undurable ? kMsgAllowUndurable : 0) |
                                (opts.durability == Durability::APPLIED ? kMsgDurApplied : 0));
      q.len = static_cast<u8>(args.len);
      q.pad = 0;
      if (args.len) std::memcpy(q.args, args.data, args.len);
      if (b.n == kBatchMax) send_batch(dst);
      return;
    }
  }

  u8* ext = nullptr;
  if (args.len > kMsgInlineBytes) {
    ext = static_cast<u8*>(alloc_.alloc(args.len, 16));
    if (ext == nullptr) {
      fail_pending(slot, target, Status::E_NO_MEMORY);
      return;
    }
    std::memcpy(ext, args.data, args.len);
  }
  Msg* m = sri_claim(dst);
  if (m == nullptr) {
    if (ext) alloc_.free(ext);
    ++stats_.overloaded;
    ++stats_.refused_ring;
    fail_pending(slot, target, Status::E_OVERLOADED);
    return;
  }
  zero_header(*m);
  m->kind = kind;
  m->src_shard = id_;
  m->op_id = op_or_type;
  m->req_id = (u64{p.gen} << 32) | slot;
  m->target = target;
  m->expected_version = opts.expected_version;
  m->len = args.len;
  u8 flags = ext ? kMsgArgsExternal : kMsgArgsInline;
  if (opts.allow_undurable) flags |= kMsgAllowUndurable;
  if (opts.durability == Durability::APPLIED) flags |= kMsgDurApplied;
  m->flags = flags;
  if (ext) {
    m->ext.ptr = ext;
  } else if (args.len) {
    std::memcpy(m->inline_data, args.data, args.len);
  }
  sri_commit(dst, m);
}

// ---- Batched requests (docs/CROSSCORE.md) --------------------------------------------

void Shard::flush_batches() {
  for (u16 dst : batch_dsts_) send_batch(dst);
  batch_dsts_.clear();
}

void Shard::send_batch(u16 dst) {
  OutBatch& b = out_batches_[dst];
  if (b.n == 0) return;
  const u32 n = b.n;
  b.n = 0;
  Msg* m = sri_claim(dst);
  if (m == nullptr) {
    for (u32 i = 0; i < n; ++i) {
      ++stats_.overloaded;
      ++stats_.refused_ring;
      fail_pending(b.recs[i].slot, b.recs[i].target, Status::E_OVERLOADED);
    }
    return;  // the buffer stays for the next batch
  }
  zero_header(*m);
  m->src_shard = id_;
  if (n == 1) {
    // Alone: an ordinary request, and the buffer stays here for reuse.
    const BatchReq& q = b.recs[0];
    m->kind = MsgKind::EXEC_REQ;
    m->op_id = q.op;
    m->req_id = (u64{q.gen} << 32) | q.slot;
    m->target = q.target;
    m->len = q.len;
    m->flags = static_cast<u8>(q.flags | kMsgArgsInline);
    if (q.len) std::memcpy(m->inline_data, q.args, q.len);
  } else {
    m->kind = MsgKind::MULTI_REQ;
    m->flags = kMsgArgsExternal;
    m->len = n;
    m->ext.ptr = reinterpret_cast<u8*>(b.recs);
    b.recs = nullptr;  // comes back with MULTI_RESP
  }
  sri_commit(dst, m);
}

void Shard::serve_multi(const Msg& m) {
  auto* recs = reinterpret_cast<BatchReq*>(m.ext.ptr);
  const u32 n = m.len;
  ObjectID ids[kBatchMax];
  for (u32 i = 0; i < n; ++i) ids[i] = recs[i].target;
  prefetch(ids, n);
  for (u32 i = 0; i < n; ++i) {
    const BatchReq q = recs[i];  // the response is written over it
    ++stats_.ops_served;
    ExecOptions opts;
    opts.allow_undurable = q.flags & kMsgAllowUndurable;
    opts.durability = (q.flags & kMsgDurApplied) ? Durability::APPLIED : Durability::DURABLE;
    RunOut out;
    if (owner_refuses(MsgKind::EXEC_REQ, q.op)) {
      out.r.id = q.target;
      out.r.status = Status::E_OVERLOADED;
    } else {
      run(MsgKind::EXEC_REQ, q.target, q.op, ByteView(q.args, q.len), opts, out);
      (void)admit_answer_wait(out);  // as serve
    }
    auto& r = *reinterpret_cast<BatchResp*>(&recs[i]);
    r.slot = q.slot;
    r.gen = q.gen;
    const bool wait = out.wait_lsn != 0 && !durable(out.wait_lsn);
    if (wait || out.r.result.len > kMsgInlineBytes) {
      // Answered apart: when durable, or with a result too big for the record.
      Msg req;
      zero_header(req);
      req.req_id = (u64{q.gen} << 32) | q.slot;
      req.src_shard = m.src_shard;
      if (wait) {
        Waiter w;
        w.lsn = out.wait_lsn;
        w.kind = Waiter::Kind::REMOTE;
        w.resp = make_resp(req, out.r);
        w.dst = m.src_shard;
        defer(std::move(w));
      } else {
        if (out.wait_lsn != 0) out.r.durable = true;
        Msg* resp = sri_claim(m.src_shard, /*must_deliver=*/true);
        fill_resp(*resp, req, out.r);
        sri_commit(m.src_shard, resp);
      }
      r.status = kBatchAnsweredApart;
      continue;
    }
    r.status = static_cast<u16>(out.r.status);
    r.app_code = out.r.app_code;
    r.durable = out.r.durable || out.wait_lsn != 0;
    r.len = static_cast<u8>(out.r.result.len);
    r.pad = 0;
    r.version = out.r.version;
    if (r.len) std::memcpy(r.result, out.r.result.data, r.len);
  }
  Msg* resp = sri_claim(m.src_shard, /*must_deliver=*/true);
  zero_header(*resp);
  resp->kind = MsgKind::MULTI_RESP;
  resp->src_shard = id_;
  resp->flags = kMsgArgsExternal;
  resp->len = n;
  resp->ext.ptr = m.ext.ptr;
  sri_commit(m.src_shard, resp);
  sri_publish(m.src_shard);  // the sender is waiting on these; don't hold them for the rest of the loop
}

void Shard::complete_multi(const Msg& m) {
  const auto* recs = reinterpret_cast<const BatchResp*>(m.ext.ptr);
  for (u32 i = 0; i < m.len; ++i) {
    const BatchResp& r = *reinterpret_cast<const BatchResp*>(
        reinterpret_cast<const u8*>(recs) + i * sizeof(BatchReq));
    if (r.status == kBatchAnsweredApart) continue;
    Pending p = pending_.at(r.slot);
    if (p.gen != r.gen || p.done == nullptr) {
      std::fprintf(stderr, "snc: batched response for unknown request on shard %u\n", id_);
      std::abort();
    }
    pending_[r.slot].done = nullptr;
    free_pending_.push_back(r.slot);
    ExecResult x;
    x.status = static_cast<Status>(r.status);
    x.app_code = r.app_code;
    x.version = r.version;
    x.durable = r.durable;
    x.id = p.target;
    x.result = ByteView(r.result, r.len);
    p.done(p.ctx, x);
  }
  alloc_.free(m.ext.ptr);
}

void Shard::fail_pending(u32 slot, ObjectID target, Status s) {
  Pending p = pending_[slot];
  pending_[slot].done = nullptr;
  free_pending_.push_back(slot);
  ExecResult r;
  r.status = s;
  r.id = target;
  p.done(p.ctx, r);
}

Msg Shard::make_resp(const Msg& req, const ExecResult& r) {
  Msg resp;
  fill_resp(resp, req, r);
  return resp;
}

void Shard::fill_resp(Msg& resp, const Msg& req, const ExecResult& r) {
  zero_header(resp);
  resp.kind = MsgKind::RESP;
  resp.src_shard = id_;
  resp.req_id = req.req_id;
  resp.target = r.id;
  resp.expected_version = r.version;
  resp.status = static_cast<u16>(r.status);
  resp.aux = r.app_code;
  resp.len = r.result.len;
  if (r.durable) resp.flags |= kMsgRespDurable;
  if (r.result.len <= kMsgInlineBytes) {
    resp.flags |= kMsgArgsInline;
    if (r.result.len) std::memcpy(resp.inline_data, r.result.data, r.result.len);
  } else {
    resp.flags |= kMsgArgsExternal;
    resp.ext.ptr = static_cast<u8*>(alloc_.alloc(r.result.len, 16));
    if (resp.ext.ptr == nullptr) {
      // The op already ran; report it without the result bytes.
      resp.flags = static_cast<u8>((resp.flags & ~kMsgArgsExternal) | kMsgArgsInline);
      resp.len = 0;
      resp.status = static_cast<u16>(Status::E_NO_MEMORY);
    } else {
      std::memcpy(resp.ext.ptr, r.result.data, r.result.len);
    }
  }
}

void Shard::serve(const Msg& m) {
  ++stats_.ops_served;
  ExecOptions opts;
  opts.expected_version = m.expected_version;
  opts.allow_undurable = m.flags & kMsgAllowUndurable;
  opts.durability = (m.flags & kMsgDurApplied) ? Durability::APPLIED : Durability::DURABLE;
  RunOut out;
  if (owner_refuses(m.kind, m.op_id)) {
    out.r.id = m.target;
    out.r.status = Status::E_OVERLOADED;
  } else {
    run(m.kind, m.target, m.op_id, ByteView(m.data(), m.len), opts, out);
    // S1.1: a forwarded READ (or unreserved answer) that must wait takes a
    // REMOTE waiter under the budget, or is answered E_OVERLOADED now. The
    // answer always goes back, so the origin's pending slot is freed.
    (void)admit_answer_wait(out);
  }
  u8* args_buf = (m.flags & kMsgArgsExternal) ? m.ext.ptr : nullptr;

  if (out.wait_lsn != 0 && !durable(out.wait_lsn)) {
    Waiter w;
    w.lsn = out.wait_lsn;
    w.kind = Waiter::Kind::REMOTE;
    w.resp = make_resp(m, out.r);
    w.dst = m.src_shard;
    w.return_args = args_buf;
    defer(std::move(w));
    return;
  }
  if (out.wait_lsn != 0) out.r.durable = true;
  Msg* resp = sri_claim(m.src_shard, /*must_deliver=*/true);
  fill_resp(*resp, m, out.r);
  sri_commit(m.src_shard, resp);
  if (args_buf) return_buffer(m.src_shard, args_buf);
}

void Shard::complete(const Msg& m) {
  u32 slot = static_cast<u32>(m.req_id);
  Pending p = pending_.at(slot);
  if (p.gen != static_cast<u32>(m.req_id >> 32) || p.done == nullptr) {
    std::fprintf(stderr, "snc: response for unknown request on shard %u\n", id_);
    std::abort();
  }
  pending_[slot].done = nullptr;
  free_pending_.push_back(slot);

  ExecResult r;
  r.status = static_cast<Status>(m.status);
  r.app_code = m.aux;
  r.version = m.expected_version;
  r.durable = m.flags & kMsgRespDurable;
  r.id = m.target;
  r.result = ByteView(m.data(), m.len);
  p.done(p.ctx, r);

  if (m.flags & kMsgArgsExternal) return_buffer(m.src_shard, m.ext.ptr);
}

void Shard::return_buffer(u16 owner, u8* ptr) {
  Msg b;
  zero_header(b);
  b.kind = MsgKind::BUF_RETURN;
  b.src_shard = id_;
  b.flags = kMsgArgsExternal;
  b.ext.ptr = ptr;
  sri_send(owner, b, /*must_deliver=*/true);
}

// ---- Recovery (spec section 12) -------------------------------------------------------

void Shard::install(const WalRecord& r, const u8* payload) {
  max_lease_seen_ = std::max(max_lease_seen_, r.id.lease());
  ObjectHeader* o = map_.find(r.id);
  auto type = static_cast<RecType>(r.rec_type);
  if (type == RecType::DELETE) {
    if (o && o->version < r.version) {
      map_.erase(r.id);
      alloc_.free(o);
    }
    return;
  }
  if (o && o->version >= r.version) return;  // version rule: redo is idempotent
  const TypeDescriptor* t = registry().type(r.type_id);
  if (t == nullptr || r.payload_len > t->max_size || (t->check && !t->check(payload, r.payload_len))) {
    std::fprintf(stderr, "snc recovery: invalid object image for %s (type %u)\n",
                 r.id.to_string().c_str(), r.type_id);
    std::abort();
  }
  std::size_t bytes = sizeof(ObjectHeader) + r.payload_len;
  if (o == nullptr || o->capacity < r.payload_len) {
    ObjectHeader** slot = o ? map_.slot(o->id) : nullptr;
    void* p = o ? alloc_.resize(o, bytes, kObjectAlign) : alloc_.alloc(bytes, kObjectAlign);
    if (p == nullptr) {
      std::fprintf(stderr, "snc recovery: out of memory on shard %u\n", id_);
      std::abort();
    }
    o = static_cast<ObjectHeader*>(p);
    if (slot) *slot = o;
  }
  o->id = r.id;
  o->version = r.version;
  o->last_lsn = 0;  // everything recovered is durable
  o->type_id = r.type_id;
  o->length = r.payload_len;
  o->capacity = static_cast<u32>(alloc_.usable(o) - sizeof(ObjectHeader));
  o->flags = 0;
  o->schema_ver = r.schema_ver;
  std::memcpy(o->payload(), payload, r.payload_len);
  map_.upsert(o);
}

void Shard::finish_recovery(const WalConfig& wal_cfg, u64 next_lsn) {
  if (repl_cfg_.standby) {
    // A standby runs no follow-ups and serves no lookups: it installs,
    // logs and keeps its index. Promotion makes it a primary, which runs
    // follow-ups and serves.
    rebuild_index();
    refresh_slots();
    if (!wal_cfg.data_dir.empty()) {
      wal_ = std::make_unique<WalStream>(wal_cfg, id_, next_lsn);
      wal_->on_durable = &Shard::on_wal_durable;
      wal_->on_durable_ctx = this;
    }
    repl_epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    sri_watch_fd(repl_epfd_);
    repl_cfg_.receiver.epoll_fd = repl_epfd_;
    receiver_ = std::make_unique<repl::Receiver>(repl_cfg_.receiver, &Shard::apply_replicated, this);
    return;
  }
  rebuild_index();
  refresh_slots();  // before the resume pass: it skips slots not served
  // The log first: the start barrier's re-seal is an ordinary record, and
  // it goes before anything is served (docs/MIGRATION_FAIL_CLOSED.md 5a).
  if (!wal_cfg.data_dir.empty()) {
    wal_ = std::make_unique<WalStream>(wal_cfg, id_, next_lsn);
    wal_->on_durable = &Shard::on_wal_durable;
    wal_->on_durable_ctx = this;
    if (repl_cfg_.primary) start_sender(wal_cfg.data_dir, next_lsn);
  }
  apply_reseal();
  start_serving();
  activate_staged();  // a move committed while this node was down
  start_links();
}

// Follow-ups the objects' resume hooks re-emit. The state must be durable:
// what they emit is ready at once.
void Shard::start_serving() { resume_all(0); }

// D.1 (docs/FOLLOWUP_INTAKE_DESIGN.md 3.9.2): a request for a resume
// pass, run by resume_step in pages under the exchange budget.
void Shard::resume_all(u64 lsn) { owe_resume(lsn, -1); }

// ---- Node links (docs/CLUSTER.md) ------------------------------------------------------

void Shard::start_links() {
  if (cluster_ == nullptr || links_) return;
  // A new links instance: a new incarnation, durable before any connection
  // can carry it (2.1). None to be had: no links.
  u64 inc = 0;
  if (xnode_.gen == nullptr || !*xnode_.gen || !(*xnode_.gen)->next(inc)) {
    if (!links_no_gen_said_) {
      std::fprintf(stderr, "snc: shard %u: no links incarnation generation: node links stay closed\n", id_);
      links_no_gen_said_ = true;
    }
    return;
  }
  NodeLinks::Options o;
  o.member = member_;
  o.incarnation = inc;
  o.accounting = xnode_.accounting;
  o.hb_ns = xnode_.hb_ns;
  o.dead_ns = xnode_.dead_ns;
  o.highest = &peer_highest_;
  links_ = std::make_unique<NodeLinks>(*cluster_, group_, id_, o);
  emit_node_.resize(links_->size());
}

// A follow-up from another group's node, stamped with the placement epoch
// its source's owner emitted it at (docs/MIGRATION.md, follow-ups). If that
// is newer than our map, we are behind: hold it, and refresh. Otherwise, if
// the target is not ours, the sender is behind: drop it; the sender
// refreshes too (it polls), and its resume pass after the refresh sends it
// to the owner. route_emit drops it if it is stale (Shard::stale).
void Shard::on_link_frame(void* ctx, const NodeLinks::From& from, const NodeLinks::Frame& f) {
  auto* s = static_cast<Shard*>(ctx);
  (void)from;
  if (NodeLinks::kind(f) != NodeLinks::DATA) return;  // control frames: D.2 accounting
  ++s->stats_.node_emits_received;
  const u32 mine = static_cast<u32>(s->cluster_->epoch);
  const bool newer = f.epoch > mine;
  if (newer && s->epoch_hint_) {
    u64 cur = s->epoch_hint_->load();
    while (f.epoch > cur && !s->epoch_hint_->compare_exchange_weak(cur, f.epoch)) {}
  }
  if (!newer && s->foreign(f.target)) {
    ++s->stats_.node_emits_misrouted;
    return;
  }
  Emit e;
  e.source = f.source;
  e.target = f.target;
  e.op = f.op;
  e.fail_op = f.fail_op;
  e.len = f.len <= sizeof(e.args) ? f.len : sizeof(e.args);
  std::memcpy(e.args, f.args, e.len);
  e.lsn = 0;
  e.remote = true;
  e.epoch = f.epoch;
  // Counted against L until it runs, leaves this shard or is dropped
  // (3.6); its exchange belongs to no shard of this node (D.2).
  e.origin = kNoShard;
  e.link_held = true;
  if (++s->link_held_ > s->stats_.link_held_peak) s->stats_.link_held_peak = s->link_held_;
  if (newer) {
    // Whatever we still run from older maps must run first: a follow-up
    // its source's new owner sent cannot overtake one its old owner did.
    s->hold_emit(e);
    return;
  }
  s->route_emit(e);
}

bool Shard::poll_links() {
  // D.1 (3.6): reads pause while L follow-ups from links are held here,
  // until half of them drain; at most L - held frames are taken per poll.
  const u64 lmax = intake_.link_hold_max;
  if (lmax != 0) {
    if (!links_paused_ && link_held_ >= lmax) {
      links_paused_ = true;
      links_->pause_reads(true);
      ++stats_.link_pauses;
    } else if (links_paused_ && link_held_ <= lmax / 2) {
      links_paused_ = false;
      links_->pause_reads(false);
    }
  }
  const u32 budget = lmax == 0 ? ~u32{0} : link_held_ >= lmax ? 0 : static_cast<u32>(lmax - link_held_);
  bool busy = links_->poll(sri_now_ns(), &Shard::on_link_frame, this, budget);
  if (links_->take_connected()) {
    // Whatever was sent on a link before it dropped, or dropped while it
    // was down, may be lost: re-emit everything unsettled. What the hooks
    // emit to other nodes waits until every record so far is durable, as
    // the state it reflects may not be yet.
    ++stats_.node_resumes;
    resume_all(wal_ ? wal_->next_lsn() - 1 : 0);
    busy = true;
  }
  return busy;
}

void Shard::start_sender(const std::string& data_dir, u64 next_lsn) {
  if (repl_epfd_ < 0) {
    repl_epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    sri_watch_fd(repl_epfd_);
  }
  repl_cfg_.sender.epoll_fd = repl_epfd_;
  repl_cfg_.sender.data_dir = data_dir;
  repl_cfg_.sender.start_lsn = next_lsn;
  sender_ = std::make_unique<repl::Sender>(repl_cfg_.sender);
  if (replicating_flag_) replicating_flag_->store(true);
  wal_->on_flush = &Shard::on_wal_flush;
  wal_->on_flush_ctx = this;
  repl_hold_ = repl_cfg_.sync;
}

// ---- Promotion (docs/REPLICATION.md) ------------------------------------------------

void Shard::stop_standby(u64 new_term) {
  // Nothing more is installed. The listener stays, refusing the old term:
  // an old primary still running learns there that it was replaced.
  if (!receiver_) return;
  receiver_->fence(new_term);
  fence_listener_ = std::move(receiver_);
  if (wal_) wal_->poll(sri_now_ns(), /*force=*/true);
}

bool Shard::standby_settled() const {
  return !wal_ || (wal_->idle() && wal_->durable_lsn() + 1 >= wal_->next_lsn());
}

void Shard::promote(const ReplicaConfig& rc, const std::string& data_dir) {
  repl_cfg_ = rc;
  refresh_slots();
  if (wal_ && rc.primary) start_sender(data_dir, wal_->next_lsn());
  // The start barrier's re-seal (docs/MIGRATION_FAIL_CLOSED.md 5a): this
  // prefix is final (stop_standby, settled) and nothing has been served.
  apply_reseal();
  start_serving();
  activate_staged();  // the old primary may have died between a commit and its activation
  // Clustered: other groups' links find this member once it listens, and
  // every link coming up re-runs the resume pass, which re-sends what the
  // old primary's links lost.
  start_links();
}

// A primary that learns it was replaced leaves the data plane for good
// (docs/CLUSTER.md, "A fenced primary withdraws"): it can never again make
// a write durable, so it must not hold other groups' links, run follow-ups
// or answer clients as if it could. Its admin side is the application's.
void Shard::withdraw() {
  if (withdrawn_) return;
  withdrawn_ = true;
  if (withdraw_flag_) withdraw_flag_->store(true);
  std::fprintf(stderr, "snc: shard %u: fenced (a newer primary exists), leaving the data plane\n", id_);
  links_.reset();         // the listener and every link, in and out
  emits_paused_ = true;   // queued follow-ups stay queued; the new primary resumes them
  // What waits for the standby's ack will never get it here. Whether it
  // committed is up to what the new primary received: E_FENCED, in doubt.
  while (!waiters_.empty()) {
    Waiter w = std::move(waiters_.front());
    waiters_.pop_front();
    waiter_bytes_ -= waiter_cost(w);
    switch (w.kind) {
      case Waiter::Kind::LOCAL:
        w.r.status = Status::E_FENCED;
        w.r.result = ByteView();
        w.done(w.ctx, w.r);
        break;
      case Waiter::Kind::REMOTE:
        w.resp.status = static_cast<u16>(Status::E_FENCED);
        sri_send(w.dst, w.resp, /*must_deliver=*/true);
        if (w.return_args) return_buffer(w.dst, w.return_args);
        break;
      case Waiter::Kind::ERASE:
        break;  // the tombstone stays in memory
    }
  }
}

// ---- Replication (docs/REPLICATION.md) -------------------------------------------------

void Shard::on_wal_flush(void* ctx, const u8* recs, u32 len, u64 first_lsn, u64 last_lsn) {
  static_cast<Shard*>(ctx)->sender_->on_flush(recs, len, first_lsn, last_lsn);
}

bool Shard::apply_replicated(void* ctx, const u8* rec) {
  auto* s = static_cast<Shard*>(ctx);
  WalRecord r;
  std::memcpy(&r, rec, sizeof(r));
  if (!s->wal_->append_raw(rec)) return false;  // not the next LSN
  s->tail_lsn_ = r.lsn;
  s->tail_crc_ = r.crc32c;
  s->install_indexed(r, rec + sizeof(r));
  return true;
}

// A standby keeps its index live as it installs, so promotion need not
// rebuild it.
void Shard::install_indexed(const WalRecord& r, const u8* payload) {
  if (r.type_id == kSlotStateTypeId && static_cast<RecType>(r.rec_type) != RecType::DELETE &&
      r.payload_len == sizeof(SlotStateV1) && phases_ != nullptr) {
    // A seal or activation from the primary: a standby serving reads must
    // stop (or start) serving the slot with it.
    SlotStateV1 st;
    std::memcpy(&st, payload, sizeof(st));
    const ObjectHeader* cur = map_.find(r.id);
    if (cur == nullptr || cur->version < r.version) set_cache(st.slot, &st);
  }
  const ObjectHeader* o = map_.find(r.id);
  const TypeDescriptor* t = registry().type(r.type_id);
  if (t == nullptr || !t->index_id) return install(r, payload);
  u8 old_key[kMaxIndexKey], new_key[kMaxIndexKey];
  u32 old_len = o && !o->deleted() ? extract_key(*t, *o, old_key) : 0;
  const u64 old_version = o ? o->version : 0;
  install(r, payload);
  o = map_.find(r.id);
  if (o == nullptr) {
    if (old_len) index_erase(t->index_id, old_key, old_len, r.id);
    return;
  }
  if (o->version == old_version) return;  // not applied (version rule)
  u32 new_len = o->deleted() ? 0 : extract_key(*t, *o, new_key);
  if (new_len == old_len && std::memcmp(old_key, new_key, new_len) == 0) return;
  if (old_len) index_erase(t->index_id, old_key, old_len, r.id);
  if (new_len) index_insert(t->index_id, new_key, new_len, r.id);
}

bool Shard::poll_replication() {
  const u64 now = sri_now_ns();
  if (receiver_) {
    // Idle sleeps wake on the sockets (repl_epfd_). Stay awake while records
    // wait to become durable here: the primary is waiting for their ack,
    // and an idle sleep would add up to a millisecond to it.
    const u64 next = wal_->next_lsn();
    bool did = receiver_->poll(next, wal_->durable_lsn(), tail_lsn_ + 1 == next ? tail_crc_ : 0, now);
    return did || wal_->durable_lsn() + 1 < wal_->next_lsn();
  }
  if (!sender_) return false;
  bool busy = false;
  sender_->set_in_sync(repl_cfg_.sync && repl_hold_ && !repl_degraded_);
  if (sender_->poll(now)) {
    busy = true;
    if (repl_hold_) release_waiters();
  }
  // A regression (the standby lost acknowledged records) stays latched
  // until this link has proven the standby holds everything durable here
  // again (docs/SYNC_READINESS.md 4).
  if (sender_->regressed() && sender_->live() && !sender_->catching_up() && wal_ != nullptr &&
      sender_->link_acked_lsn() >= wal_->durable_lsn()) {
    sender_->clear_regressed();
  }
  if (repl_cfg_.sync) {
    const bool overdue = repl_hold_ && !sender_->live() && repl_cfg_.timeout_ns != 0 &&
                         sender_->down_since_ns() != 0 &&
                         now - sender_->down_since_ns() >= repl_cfg_.timeout_ns;
    // Not before a read lease granted to the standby runs out, nor without
    // a witness's leave when one decides.
    repl_blocked_ = overdue && ((repl_cfg_.degrade_gate && !repl_cfg_.degrade_gate->load()) ||
                                now < sender_->lease_until_ns());
    if (overdue && !repl_blocked_) {
      // The standby has been gone too long: keep serving on local
      // durability alone and say so (docs/REPLICATION.md).
      std::fprintf(stderr, "snc: shard %u: standby unreachable, continuing without it\n", id_);
      repl_hold_ = false;
      repl_degraded_ = true;
      release_waiters();
    } else if (repl_degraded_ && sender_->live() && !sender_->catching_up() &&
               sender_->link_acked_lsn() >= wal_->durable_lsn()) {
      std::fprintf(stderr, "snc: shard %u: standby caught up, replication is synchronous again\n",
                   id_);
      repl_hold_ = true;
      repl_degraded_ = false;
    }
  }
  // Acks wake an idle shard through repl_epfd_; reconnects and the timeout
  // run on the idle loop's 1 ms tick.
  return busy;
}

// ---- Checkpoint (spec section 12) ------------------------------------------------------

const char* ckpt_phase_name(u64 p) {
  static const char* const names[] = {"idle", "snapshot", "walk", "drain", "durable_wait", "sync"};
  return p < 6 ? names[p] : "?";
}

const char* node_ckpt_phase_name(u64 p) {
  static const char* const names[] = {"idle", "shards", "commit", "reclaim"};
  return p < 4 ? names[p] : "?";
}

void Shard::begin_checkpoint(const std::string& data_dir, const Uuid& uuid, u64 seq,
                             CheckpointDone done) {
  Checkpoint& c = ckpt_;
  c = Checkpoint{};
  c.active = true;
  c.done = std::move(done);
  // Observability (S0): phase times; no effect on the checkpoint.
  const u64 t0 = sri_now_ns();
  ckpt_obs_.seq = seq;
  ckpt_obs_.started_ns = t0;
  ckpt_obs_.bytes_written = 0;
  ckpt_obs_.writes_peak = 0;
  ckpt_obs_.snapshot_us = ckpt_obs_.walk_us = ckpt_obs_.drain_us = ckpt_obs_.durable_wait_us = 0;
  // S2 (docs/S2_PACING.md): the controller starts at its floor. The WAL
  // baseline is the flush-latency EWMA as it stands now, learned while no
  // checkpoint ran (checkpoints on a shard never overlap).
  ckpt_obs_.walk_steps = ckpt_obs_.catchup_steps = 0;
  ckpt_obs_.step_max_ns = ckpt_obs_.step_max_objects = 0;
  ckpt_obs_.past_target_counted = ckpt_obs_.degraded_counted = false;
  ckpt_obs_.wal_baseline_us = wal_ ? wal_->stats().flush_us_ewma.value() : 0;
  // S3 (docs/S3_WRITEBACK.md): per-checkpoint writeback counters.
  ckpt_obs_.window_full_steps = ckpt_obs_.chunks = ckpt_obs_.buf_bytes_peak = 0;
  ckpt_obs_.write_lat_last_us = ckpt_obs_.write_lat_ewma_us = ckpt_obs_.write_lat_max_us = 0;
  for (u64& b : ckpt_obs_.write_lat_hist) b = 0;
  ckpt_obs_.sync_writes = 0;
  ckpt_obs_.token_request_ns = 0;  // S4
  pacer_ = ckpt::reset(pacing_.cfg, t0);
  ckpt_enter(CkptPhase::SNAPSHOT, t0);
  c.start_lsn = wal_ ? wal_->next_lsn() : 0;
  c.ids.reserve(map_.size());
  map_.for_each([&](ObjectHeader* o) {
    if (!o->deleted()) c.ids.push_back(o->id);
  });
  ckpt_obs_.walk_total = c.ids.size();
  c.path = part_path(data_dir, seq, id_);
  c.fd = ::open(c.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (c.fd < 0) storage_fatal("create checkpoint part failed: " + c.path);
  FileHeader h{};
  std::memcpy(h.magic, "SNCCKP01", 8);
  h.format = kFormatVersion;
  h.stream = id_;
  h.seq = seq;
  h.first_lsn = c.start_lsn;
  std::memcpy(h.space_uuid, uuid.data(), 16);
  h.crc32c = crc32c(&h, sizeof(h));
  c.chunk.reserve(kCheckpointChunkReserve);
  c.chunk.assign(kBlock, 0);
  std::memcpy(c.chunk.data(), &h, sizeof(h));
  ckpt_note_buf();
  ckpt_enter(CkptPhase::WALK, sri_now_ns());
}

// Observability (S0): records how long the phase just left took. Legal
// order: IDLE, SNAPSHOT, WALK, DRAIN, DURABLE_WAIT, SYNC, IDLE.
void Shard::ckpt_enter(CkptPhase p, u64 now) {
  CkptObs& o = ckpt_obs_;
  const u64 us = (now - o.phase_since_ns) / 1000;
  switch (o.phase) {
    case CkptPhase::SNAPSHOT: o.snapshot_us = us; break;
    case CkptPhase::WALK: o.walk_us = us; break;
    case CkptPhase::DRAIN: o.drain_us = us; break;
    case CkptPhase::DURABLE_WAIT:
      // S4: the durable wait ends when the part asks for its sync token;
      // the token wait is reported on its own (token_wait_*), so this
      // time keeps its S0 meaning whatever K is.
      o.durable_wait_us = o.token_request_ns != 0 ? (o.token_request_ns - o.phase_since_ns) / 1000 : us;
      break;
    case CkptPhase::SYNC:
      o.last_snapshot_us = o.snapshot_us;
      o.last_walk_us = o.walk_us;
      o.last_drain_us = o.drain_us;
      o.last_durable_wait_us = o.durable_wait_us;
      o.last_sync_us = us;
      o.last_total_us = (now - o.started_ns) / 1000;
      o.last_result = 1;
      ++o.completed;
      break;
    case CkptPhase::IDLE: break;
  }
  o.phase = p;
  o.phase_since_ns = now;
  gauges_dirty_ = true;
}

u64 Shard::ckpt_buf_bytes() const {
  u64 b = ckpt_.chunk.capacity();
  for (const auto& w : ckpt_.slots) b += w->buf.capacity();
  return b;
}

void Shard::ckpt_note_buf() {
  const u64 b = ckpt_buf_bytes();
  if (b > ckpt_obs_.buf_bytes_peak) ckpt_obs_.buf_bytes_peak = b;
}

// S3 (docs/S3_WRITEBACK.md section 3). Hands the chunk being built to a
// write slot and submits it; the slot owns the buffer until that write's
// own completion. With dsync, at most `window` writes are in flight: a
// full window refuses, the chunk stays where it is, and the caller stops
// the walk. Legacy (the path before S3) has no window and no dsync.
bool Shard::checkpoint_submit_chunk() {
  Checkpoint& c = ckpt_;
  if (c.chunk.empty()) return true;
  if (ckpt_io_.dsync && c.writes_inflight >= ckpt_io_.window) return false;
  CkptWrite* w = nullptr;
  if (!c.free_slots.empty()) {
    w = c.free_slots.back();
    c.free_slots.pop_back();
  } else {
    c.slots.push_back(std::make_unique<CkptWrite>());
    w = c.slots.back().get();
    w->shard = this;
  }
  w->buf.swap(c.chunk);  // the chunk being built is now empty, capacity 0
  w->len = static_cast<u32>(w->buf.size());
  w->index = c.chunks++;
  w->submit_ns = sri_now_ns();
  w->busy = true;
  ++c.writes_inflight;
  if (c.writes_inflight > ckpt_obs_.writes_peak) ckpt_obs_.writes_peak = c.writes_inflight;
  ckpt_obs_.chunks = c.chunks;
  const u64 off = c.file_off;
  c.file_off += w->len;
  ckpt_note_buf();
  gauges_dirty_ = true;
  if (g_debug_ckpt_defer.load(std::memory_order_relaxed)) {
    ckpt_deferred_.push_back({w, off});  // tests: issued later by checkpoint_process_held
    return true;
  }
  checkpoint_issue(w, off);
  return true;
}

void Shard::checkpoint_issue(CkptWrite* w, u64 off) {
  Checkpoint& c = ckpt_;
  if (!sri_write(c.fd, off, w->buf.data(), w->len, ckpt_io_.dsync, &Shard::checkpoint_write_done, w)) {
    // The synchronous fallback: a durable write blocked this loop for its
    // whole duration. Counted, never hidden (review PR76 section 8).
    ++ckpt_obs_.sync_writes;
    ++ckpt_obs_.sync_writes_total;
  }
}

void Shard::checkpoint_write_done(void* ctx, int result) {
  auto* w = static_cast<CkptWrite*>(ctx);
  Shard* s = w->shard;
  if (g_debug_ckpt_hold.load(std::memory_order_relaxed)) {
    s->ckpt_held_.push_back({w, result});  // tests: processed by checkpoint_process_held
    return;
  }
  s->checkpoint_write_complete(w, result);
}

// Exactly one completion per submitted write, in any order. A chunk write
// must complete in full: an error or a short write is fatal before the
// commit (spec C7), so the previous checkpoint and its WAL stay
// authoritative (CC-5).
void Shard::checkpoint_write_complete(CkptWrite* w, int result) {
  Checkpoint& c = ckpt_;
  if (!w->busy) {
    std::fprintf(stderr, "snc: checkpoint write completed twice (chunk %llu)\n", (unsigned long long)w->index);
    std::abort();
  }
  u64 fail_at = g_debug_ckpt_fail_at.load(std::memory_order_relaxed);
  if (fail_at == w->index + 1 && g_debug_ckpt_fail_at.compare_exchange_strong(fail_at, 0)) {
    result = g_debug_ckpt_fail_mode.load() == 1 && result > 0 ? result - 1 : -EIO;
  }
  if (result < 0) {
    errno = -result;
    storage_fatal("checkpoint write failed: " + c.path);
  }
  if (static_cast<u32>(result) != w->len) {
    errno = EIO;
    storage_fatal("checkpoint short write: " + std::to_string(result) + " of " + std::to_string(w->len) +
                  " bytes, chunk " + std::to_string(w->index) + ": " + c.path);
  }
  const u64 us = (sri_now_ns() - w->submit_ns) / 1000;
  CkptObs& o = ckpt_obs_;
  o.write_lat_last_us = us;
  o.write_lat_ewma_us = o.write_lat_ewma_us == 0 ? us : (o.write_lat_ewma_us * 7 + us) / 8;
  if (us > o.write_lat_max_us) o.write_lat_max_us = us;
  ++o.write_lat_hist[us < 1000 ? 0 : us < 10'000 ? 1 : us < 100'000 ? 2 : 3];
  o.bytes_written += w->len;
  // The write is complete and no kernel or runtime path references its
  // buffer any more: free it now (review PR76 section 3).
  std::vector<u8>().swap(w->buf);
  w->busy = false;
  c.free_slots.push_back(w);
  --c.writes_inflight;
  gauges_dirty_ = true;
}

// Tests only: held completions, processed as release tokens allow, or all
// once the hold is off.
void Shard::checkpoint_process_held() {
  // Deferred submissions: a released token issues one, the newest first
  // when asked, so writes reach the kernel out of submission order while
  // later slots still own unissued buffers (a wrong free corrupts the part).
  while (!ckpt_deferred_.empty()) {
    if (g_debug_ckpt_defer.load(std::memory_order_relaxed)) {
      u64 t = g_debug_ckpt_release.load(std::memory_order_relaxed);
      if (t == 0 || !g_debug_ckpt_release.compare_exchange_strong(t, t - 1)) break;
    }
    DeferredWrite d{};
    if (g_debug_ckpt_release_newest.load(std::memory_order_relaxed)) {
      d = ckpt_deferred_.back();
      ckpt_deferred_.pop_back();
    } else {
      d = ckpt_deferred_.front();
      ckpt_deferred_.erase(ckpt_deferred_.begin());
    }
    checkpoint_issue(d.w, d.off);
  }
  while (!ckpt_held_.empty()) {
    if (g_debug_ckpt_hold.load(std::memory_order_relaxed)) {
      u64 t = g_debug_ckpt_release.load(std::memory_order_relaxed);
      if (t == 0 || !g_debug_ckpt_release.compare_exchange_strong(t, t - 1)) return;
    }
    HeldWrite h{};
    if (g_debug_ckpt_release_newest.load(std::memory_order_relaxed)) {
      h = ckpt_held_.back();
      ckpt_held_.pop_back();
    } else {
      h = ckpt_held_.front();
      ckpt_held_.erase(ckpt_held_.begin());
    }
    checkpoint_write_complete(h.w, h.result);
  }
  if (ckpt_sync_held_) {
    if (g_debug_ckpt_hold_sync.load(std::memory_order_relaxed)) {
      u64 t = g_debug_ckpt_sync_release.load(std::memory_order_relaxed);
      if (t == 0 || !g_debug_ckpt_sync_release.compare_exchange_strong(t, t - 1)) return;
    }
    ckpt_sync_held_ = false;
    checkpoint_sync_complete(ckpt_sync_result_);
  }
}

void Shard::checkpoint_sync_done(void* ctx, int result) {
  auto* s = static_cast<Shard*>(ctx);
  const u64 bit = u64{1} << (s->id_ & 63);
  if ((g_debug_ckpt_fail_sync.load(std::memory_order_relaxed) & bit) != 0 &&
      (g_debug_ckpt_fail_sync.fetch_and(~bit) & bit) != 0) {
    result = -EIO;  // tests: the injected final-sync failure (S4-T6)
  }
  if (g_debug_ckpt_hold_sync.load(std::memory_order_relaxed)) {
    s->ckpt_sync_held_ = true;  // tests: processed by checkpoint_process_held
    s->ckpt_sync_result_ = result;
    return;
  }
  s->checkpoint_sync_complete(result);
}

void Shard::checkpoint_sync_complete(int result) {
  Shard* s = this;
  Checkpoint& c = s->ckpt_;
  // S4-I2: the token is released on every outcome, before anything else,
  // so the failure path below releases it too before the process ends.
  sync_tokens_->sync_finished();
  if (c.token_held) {
    c.token_held = false;
    sync_tokens_->release();
  }
  if (result < 0) {
    errno = -result;
    storage_fatal("checkpoint fdatasync failed: " + c.path);
  }
  ::close(c.fd);
  c.fd = -1;
  CheckpointResult res{c.start_lsn, c.objects, c.bytes};
  s->ckpt_obs_.last_objects = c.objects;
  s->ckpt_obs_.last_bytes_issued = c.file_off;
  CheckpointDone done = std::move(c.done);
  c = Checkpoint{};
  s->ckpt_enter(CkptPhase::IDLE, sri_now_ns());
  done(res);
}

// S2: one controller step (docs/S2_PACING.md section 3). Runs on every
// checkpoint_step of the checkpoint in progress, in every phase, so the
// target and age reports stay current while the part drains, waits for
// WAL durability or syncs. Counts each crossing once per checkpoint.
void Shard::pace(u64 now) {
  ckpt::Signals g;
  g.now_ns = now;
  if (wal_) g.wal_ewma_us = wal_->stats().flush_us_ewma.value();
  g.wal_baseline_us = ckpt_obs_.wal_baseline_us;
  g.backlog = emits_in_flight();
  g.backlog_cap = kEmitBacklog;
  g.done = ckpt_.walk_done ? ckpt_obs_.walk_total : ckpt_.cursor;
  g.total = ckpt_obs_.walk_total;
  const ckpt::PacerState before = pacer_;
  pacer_ = ckpt::step(pacing_.cfg, pacer_, g);
  if (pacer_.past_target && !ckpt_obs_.past_target_counted) {
    ckpt_obs_.past_target_counted = true;
    ++ckpt_obs_.past_target_count;
  }
  if (pacer_.age == ckpt::AgeState::DEGRADED && !ckpt_obs_.degraded_counted) {
    ckpt_obs_.degraded_counted = true;
    ++ckpt_obs_.degraded_count;
  }
  // Publish at once on a budget, age or target change. A mode change
  // alone waits for the regular publish: near the schedule line the mode
  // can flip every iteration. With pacing off the budget is not applied
  // and is not published.
  if ((pacing_.on && pacer_.walk_ns != before.walk_ns) || pacer_.age != before.age ||
      pacer_.past_target != before.past_target) {
    gauges_dirty_ = true;
  }
}

void Shard::checkpoint_step() {
  Checkpoint& c = ckpt_;
  if (!ckpt_held_.empty() || !ckpt_deferred_.empty() || ckpt_sync_held_) {
    checkpoint_process_held();
    if (!ckpt_.active) return;  // a held final sync completed: the checkpoint is over
  }
  const u64 t0 = sri_now_ns();
  pace(t0);
  if (c.syncing) return;
  if (!c.walk_done) {
    // S3: a chunk that found the window full is submitted before anything
    // else is encoded. Until it is, the walk stays stopped: the bound is
    // structural (review PR76 section 4), not a deferred submit.
    if (c.blocked) {
      if (!checkpoint_submit_chunk()) {
        ++ckpt_obs_.window_full_steps;
        return;
      }
      c.blocked = false;
      gauges_dirty_ = true;
    }
    if (!c.visited) {
      const u64 limit = g_debug_ckpt_walk_limit.load(std::memory_order_relaxed);
      const std::size_t end = limit != 0 && limit < c.ids.size() ? static_cast<std::size_t>(limit) : c.ids.size();
      const std::size_t from = c.cursor;
      // Visits snapshot IDs [cursor, cursor + n). Pacing changes when each
      // ID is visited, never which IDs are (CC-1). A full window stops the
      // visit after the record that filled the chunk; the cursor is already
      // past that ID, so nothing is visited twice or skipped.
      auto visit = [&](std::size_t n) {
        for (; n > 0 && c.cursor < end && !c.blocked; --n) {
          // Objects deleted since begin are absent; their DELETE is at or after
          // start_lsn and replay handles it. Objects created since begin are
          // covered by CREATE records at or after start_lsn.
          const ObjectHeader* o = map_.find(c.ids[c.cursor++]);
          if (o == nullptr || o->deleted()) continue;
          u32 size = record_size(o->length);
          std::size_t at = c.chunk.size();
          // Reserved at kCheckpointChunkReserve; a record that does not fit
          // grows it to the exact size, never geometrically, so a retained
          // buffer stays within kCheckpointChunkMax.
          if (c.chunk.capacity() < kCheckpointChunkReserve) c.chunk.reserve(kCheckpointChunkReserve);
          if (at + size > c.chunk.capacity()) {
            // The growth copies: old and new buffers are both held for the
            // moment, and the peak counts both (docs/S3_WRITEBACK.md 3).
            const u64 both = ckpt_buf_bytes() + at + size;
            if (both > ckpt_obs_.buf_bytes_peak) ckpt_obs_.buf_bytes_peak = both;
            c.chunk.reserve(at + size);
          }
          c.chunk.resize(at + size);
          encode_record(c.chunk.data() + at, RecType::IMAGE, o->last_lsn, o->type_id, o->id,
                        o->version, o->schema_ver, o->payload(), o->length);
          c.max_lsn = std::max(c.max_lsn, o->last_lsn);
          ++c.objects;
          c.bytes += size;
          if (c.chunk.size() >= kCheckpointChunk && !checkpoint_submit_chunk()) c.blocked = true;
        }
      };
      u64 t1 = t0;
      if (!pacing_.on) {
        visit(kCheckpointSlice);  // before S2: a fixed count per iteration
        t1 = sri_now_ns();         // observed only
      } else {
        // A time budget, read every `batch` IDs. At least one batch per
        // iteration (the floor that keeps the walk moving), and at most one
        // batch past the budget (the clock is read after each batch).
        const u64 deadline = t0 + pacer_.walk_ns;
        do {
          visit(pacing_.batch);
          t1 = sri_now_ns();
        } while (c.cursor < end && !c.blocked && t1 < deadline);
      }
      ckpt_note_buf();
      if (c.cursor > from) {
        CkptObs& o = ckpt_obs_;
        ++o.walk_steps;
        if (pacing_.on && pacer_.mode == ckpt::Mode::CATCH_UP) ++o.catchup_steps;
        if (t1 - t0 > o.step_max_ns) o.step_max_ns = t1 - t0;
        if (c.cursor - from > o.step_max_objects) o.step_max_objects = c.cursor - from;
      }
      // Visited once the cursor is at the end, even if the record that
      // ended the walk also filled a chunk the window refused: that chunk
      // is then the last one, and is reported as the final one pending.
      if (c.cursor == c.ids.size()) {
        c.visited = true;
        gauges_dirty_ = true;
      }
      if (c.blocked) {
        ++ckpt_obs_.window_full_steps;
        gauges_dirty_ = true;
        return;
      }
    }
    if (c.visited) {
      // Every ID is visited; the last chunk must be submitted before the
      // walk is done (review PR76 section 5). If the window is full it
      // waits, with the IDs kept and the phase still WALK.
      if (!checkpoint_submit_chunk()) {
        c.blocked = true;
        ++ckpt_obs_.window_full_steps;
        gauges_dirty_ = true;
        return;
      }
      c.walk_done = true;
      std::vector<ObjectID>().swap(c.ids);
      ckpt_enter(CkptPhase::DRAIN, sri_now_ns());
    }
    return;
  }
  // Spec C5: the part may only hold state whose WAL record is durable
  // here. Local durability, not the standby's acknowledgement: the part is
  // this node's recovery image and holds nothing its own WAL replay would
  // not restore. Waiting for the standby wedged the shard's thread when the
  // standby died mid-checkpoint, so it could never ask for DEGRADE
  // (docs/BASELINE_READINESS.md B1). The standby's acknowledgement stays
  // part of a sync client OK (durable()).
  if (c.writes_inflight == 0 && ckpt_obs_.phase == CkptPhase::DRAIN) {
    ckpt_enter(CkptPhase::DURABLE_WAIT, sri_now_ns());  // observability (S0)
  }
  // S4 (docs/S4_SYNC_TOKENS.md): the final sync waits for one of the
  // node's K tokens. The ticket is taken only once this part could sync
  // (every write done, the WAL durable here to max_lsn), and the same
  // condition is evaluated again at every step up to the grant, so CC-3's
  // check still precedes the sync. Waiting is this check failing at a
  // step: the shard returns to its loop and keeps serving (S4-I4).
  if (c.writes_inflight == 0 && lsn_local_durable(c.max_lsn)) {
    const u64 now = sri_now_ns();
    CkptObs& o = ckpt_obs_;
    if (!c.ticketed) {
      c.ticket = sync_tokens_->take_ticket();
      c.ticketed = true;
      o.token_request_ns = now;
      o.token_ticket = c.ticket + 1;
      gauges_dirty_ = true;
    }
    if (!sync_tokens_->try_acquire(c.ticket)) return;
    c.token_held = true;
    const u64 waited = (now - o.token_request_ns) / 1000;
    o.token_wait_last_us = waited;
    if (waited > o.token_wait_max_us) o.token_wait_max_us = waited;
    o.token_wait_ewma_us = o.token_wait_ewma_us == 0 ? waited : (o.token_wait_ewma_us * 7 + waited) / 8;
    ++o.token_acquired;
    sync_tokens_->note_wait(waited);
    sync_tokens_->sync_started();
    c.syncing = true;
    ckpt_enter(CkptPhase::SYNC, now);  // observability (S0)
    sri_fdatasync(c.fd, &Shard::checkpoint_sync_done, this);
  }
}

// ---- Stats ---------------------------------------------------------------------------

Shard::ReplSummary Shard::repl_summary() const {
  if (sender_) {
    // Ready (docs/SYNC_READINESS.md 3): a sync primary, not degraded or
    // blocked, whose current link is live, not catching up from disk, and
    // has itself proven the standby holds every durable record here.
    // An async primary acknowledges before its standby has anything, so
    // no link can prove that; its readiness stays what it was (the link is
    // up), and async promotion policy (R08) is unchanged here.
    const bool ready =
        repl_cfg_.sync ? repl_hold_ && !repl_degraded_ && !repl_blocked_ && sender_->live() && !sender_->catching_up() &&
                             wal_ != nullptr && sender_->link_acked_lsn() >= wal_->durable_lsn() && !sender_->regressed()
                       : sender_->live() && !repl_degraded_ && !repl_blocked_;
    // A regression only matters to sync readiness; an async primary's is
    // not reported (R08 unchanged).
    return {1, sender_->live(), repl_degraded_, repl_blocked_, ready, repl_cfg_.sync && sender_->regressed()};
  }
  if (receiver_) return {2, receiver_->connected(), false, false};
  return {0, false, false, false};
}

ShardStats Shard::stats() const {
  ShardStats s = stats_;
  s.objects = 0;
  map_.for_each([&](const ObjectHeader* o) { s.objects += !o->deleted(); });
  s.emits_in_flight = emits_in_flight();
  s.emits_held_now = emit_held_.size();
  {
    u64 elsewhere = 0, outstanding = 0;
    if (board_ != nullptr) {
      for (u16 x = 0; x < board_->size(); ++x) {
        if (x == id_) continue;
        elsewhere += board_->ended(x, id_).load(std::memory_order_acquire);
        outstanding += sent_to_[x] - board_->taken(x, id_).load(std::memory_order_acquire);
      }
    }
    s.exchanges_live = started_ - ended_own_ - elsewhere;
    s.window_outstanding = outstanding;
  }
  s.followups_staged_now = emit_local_.size();
  s.link_held_now = link_held_;
  s.resume_active = scan_.active;
  s.resume_owed = scan_.owed_all || scan_.owed_any;
  s.slots_unproven = unproven_count();
  if (links_) s.node_connects = links_->connects();
  s.local_map_bytes = map_.memory_bytes();
  s.heap_live_bytes = alloc_.stats().live_bytes;
  s.waiters_now = waiters_.size();
  s.pending_now = pending_live();
  s.overflow_now = sri_overflow_queued();
  s.wal_next_lsn = wal_ ? wal_->next_lsn() : 0;
  if (sender_) {
    s.repl_role = 1;
    s.repl_live = sender_->live();
    s.repl_degraded = repl_degraded_;
    s.repl_blocked = repl_blocked_;
    s.repl_lost = sender_->lost();
    s.repl_acked_lsn = sender_->acked_lsn();
    s.repl_bytes = sender_->stats().bytes_sent;
    s.repl_retained_bytes = sender_->stats().retained_bytes;
    s.repl_connects = sender_->stats().connects;
    s.repl_disk_bytes = sender_->stats().disk_bytes;
  } else if (receiver_) {
    s.repl_role = 2;
    s.repl_live = receiver_->connected();
    s.repl_records = receiver_->stats().records;
    s.repl_connects = receiver_->stats().connects;
  }
  s.map_pages = map_.page_mode();
  s.heap_pages = alloc_.stats().arena_pages;
  s.heap_arena_bytes = alloc_.stats().arena_bytes;
  s.heap_arena_misses = alloc_.stats().arena_misses;
  if (wal_) {
    s.wal = wal_->stats();
    s.wal_durable_lsn = wal_->durable_lsn();
    s.wal_direct = wal_->direct();
  }
  return s;
}

// ---- OpCtx ---------------------------------------------------------------------------

Status OpCtx::reserve(u32 new_len) {
  if (new_len > type_->max_size) return Status::E_TOO_LARGE;
  if (new_len > obj_->capacity) {
    std::size_t extra = new_len - obj_->capacity;
    if (!tl_shard->alloc().can_reserve(extra)) return Status::E_NO_MEMORY;
  }
  if (new_len > reserved_len_) reserved_len_ = new_len;
  return Status::OK;
}

u8* OpCtx::resize(u32 new_len) {
  if (new_len > reserved_len_) contract_violation("resize beyond reserved length", op_);
  u32 old_len = obj_->length;
  if (new_len > obj_->capacity) {
    ObjectHeader** slot = tl_shard->map().slot(obj_->id);
    auto* moved = static_cast<ObjectHeader*>(
        tl_shard->alloc().resize(obj_, sizeof(ObjectHeader) + new_len, kObjectAlign));
    if (moved == nullptr) contract_violation("reserved memory unavailable at apply", op_);
    moved->capacity = static_cast<u32>(tl_shard->alloc().usable(moved) - sizeof(ObjectHeader));
    *slot = moved;
    obj_ = moved;
  }
  if (new_len > old_len) std::memset(obj_->payload() + old_len, 0, new_len - old_len);
  obj_->length = new_len;
  return obj_->payload();
}

// ---- Two objects in one op (docs/TRANSFERS.md) --------------------------------------

ObjectHeader* OpCtx::local(const ObjectID& id, u32 type_id) {
  Shard* s = tl_shard;
  if (op_ == nullptr || op_->kind != OpKind::MUTATE) contract_violation("local() from a non-MUTATE op", op_);
  if (id == obj_->id) return nullptr;
  if (peer_ != nullptr) {
    if (peer_->id == id) return peer_;
    contract_violation("local() on a second object", op_);
  }
  u64 h = snc_hash64(id);
  if (s->slots_.owner[h >> 52] != s->id_) return nullptr;  // another shard's: no map probe
  if (!s->serving(static_cast<u16>(h >> 52))) return nullptr;  // sealed or staged here: not this group's
  ObjectHeader* o = s->map_.find(id, h);
  if (o == nullptr || o->deleted() || o->type_id != type_id) return nullptr;
  if (applying_) {
    peer_ = o;
#ifndef NDEBUG
    const TypeDescriptor* t = registry().type(type_id);
    s->peer_len_ = o->length;
    s->peer_key_len_ = t->index_id ? s->extract_key(*t, *o, s->peer_key_) : 0;
#endif
  }
  return o;
}

// ---- Follow-ups (docs/TRANSFERS.md) ------------------------------------------------

void OpCtx::reserve_emits(u16 n) {
  if (op_ == nullptr || op_->kind != OpKind::MUTATE || op_->unlogged || applying_) {
    contract_violation("reserve_emits outside the validate of a logged MUTATE op", op_);
  }
  if (n > op_->max_emits) contract_violation("reserve_emits above the op's max_emits", op_);
  emits_reserved_ = n;
}

// The follow-up contract (docs/FOLLOWUP_INTAKE_DESIGN.md 3.3, 3.9.1): a
// resume hook emits at most its type's max_resume_emits per object; an op
// emits only from apply, at most its max_emits; a client-run apply at
// most what validate reserved; a follow-up apply once at most, and only
// back to its source (reply-only). Anything else stops the process.
void OpCtx::emit(const ObjectID& target, OpID op, ByteView args, OpID fail_op) {
  Shard* s = tl_shard;
  ++emits_done_;
  if (s->resuming_) {
    if (emits_done_ > type_->max_resume_emits) contract_violation("a resume hook emitted more than max_resume_emits", op_);
  } else {
    if (op_ == nullptr || op_->kind != OpKind::MUTATE) contract_violation("emit from a non-MUTATE op", op_);
    if (!applying_) contract_violation("emit outside apply", op_);
    if (emits_done_ > op_->max_emits) contract_violation("an op emitted more than its max_emits", op_);
    if (!source_.is_null()) {
      if (!(target == source_)) contract_violation("a follow-up emitted to a target other than its source", op_);
      if (emits_done_ > 1) contract_violation("a follow-up emitted more than once", op_);
    } else if (emits_done_ > emits_reserved_) {
      contract_violation("an op emitted more than it reserved (reserve_emits)", op_);
    }
  }
  if (args.len > kMaxEmitArgs) contract_violation("emit args too large", op_);
  // Reply-only (3.3): a follow-up runs only an op declared followup (its
  // fail op too). Checked here, at the emitting op, so that the receiver
  // can treat a frame that breaks it as foreign input, not as our bug.
  for (OpID x : {op, fail_op}) {
    const OpDescriptor* d = x != 0 ? registry().op(x) : nullptr;
    if (d != nullptr && !d->followup) contract_violation("a follow-up was emitted to an op not declared followup", op_);
  }
  Shard::Emit& e = s->emit_scratch_.emplace_back();
  e.source = obj_->id;
  e.target = target;
  e.op = op;
  e.fail_op = fail_op;
  e.len = static_cast<u8>(args.len);
  e.epoch = s->cluster_ ? static_cast<u32>(s->cluster_->epoch) : 0;
  if (args.len) std::memcpy(e.args, args.data, args.len);
  if (source_.is_null()) {
    // A root: a client-run op's or a resume hook's. It starts an exchange
    // this shard owns until the exchange ends, wherever that is (3.4).
    e.origin = s->id_;
    ++s->started_;
    ++s->stats_.exchanges_started;
  } else {
    e.origin = s->cur_origin_;  // the reply continues its exchange
  }
}

void Shard::queue_emits(u64 lsn) {
  stats_.emits += emit_scratch_.size();
  for (Emit& e : emit_scratch_) {
    e.lsn = lsn;
    // A local follow-up's record lands later in this shard's WAL stream, and
    // recovery replays a prefix of the stream, so it cannot outlive the
    // record that emitted it: no need to wait.
    // Never so for another node's object: the follow-up waits.
    if (durable(lsn) || (owner_of(e.target) == id_ && !foreign(e.target))) {
      route_emit(e);
    } else {
      emit_wait_.push_back(e);  // this shard's LSNs only grow, so stays sorted
    }
  }
  emit_scratch_.clear();
}

// ---- Follow-up intake (docs/FOLLOWUP_INTAKE_DESIGN.md, D.1) --------------------------

// 3.1: per item, immediately before it runs (no yield before its append).
// The empty-buffer clause is S1's progress rule: one item always fits.
bool Shard::followup_gate_open() const {
  if (!intake_.wal_gate || !wal_) return true;
  const u64 a = wal_->active_bytes();
  return a == 0 || a + followup_need_ <= wal_cap_;
}

// 3.2: at most W follow-ups to `dst` that `dst` has not taken out of its
// staging. Full: flag that we wait (Dekker, against publish_intake on
// `dst`: we store the flag then read `taken`, it stores `taken` then reads
// the flag), so `dst` wakes us when it takes one.
bool Shard::window_open(u16 dst) {
  if (intake_.window == 0 || board_ == nullptr) return true;
  const u64 w = intake_.window;
  if (sent_to_[dst] - taken_cache_[dst] < w) return true;
  taken_cache_[dst] = board_->taken(dst, id_).load(std::memory_order_acquire);
  if (sent_to_[dst] - taken_cache_[dst] < w) return true;
  board_->wait(id_, dst).store(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  taken_cache_[dst] = board_->taken(dst, id_).load(std::memory_order_acquire);
  if (sent_to_[dst] - taken_cache_[dst] < w) return true;
  ++stats_.window_waits;
  return false;
}

// 3.4: the exchange of a follow-up that ran and emitted nothing, or was
// dropped, or left this node, is over; its origin shard counts it.
void Shard::end_exchange(u16 origin) {
  if (origin == kNoShard) return;  // another node's (D.2)
  ++stats_.exchanges_ended;
  if (origin == id_) ++ended_own_;
  else ++ended_at_[origin];
}

void Shard::release_link(Emit& e) {
  if (!e.link_held) return;
  e.link_held = false;
  --link_held_;
}

u64 Shard::exchanges_live() {
  if (board_ != nullptr) {
    u64 sum = 0;
    for (u16 x = 0; x < board_->size(); ++x) {
      if (x != id_) sum += board_->ended(x, id_).load(std::memory_order_acquire);
    }
    ended_seen_ = sum;
  }
  const u64 ended = ended_own_ + ended_seen_;
  if (ended > started_) {
    std::fprintf(stderr, "snc: shard %u: exchanges ended (%llu) exceed started (%llu)\n", id_,
                 (unsigned long long)ended, (unsigned long long)started_);
    std::abort();
  }
  const u64 live = started_ - ended;
  if (live > stats_.exchanges_live_peak) stats_.exchanges_live_peak = live;
  return live;
}

// The cached count only overestimates (ends elsewhere only grow): reread
// them only when it says full.
bool Shard::exchange_full() {
  if (!intake_.exchange_gate) return false;
  if (started_ - ended_own_ - ended_seen_ < intake_.exchange_budget) return false;
  return exchanges_live() >= intake_.exchange_budget;
}

// Once per loop iteration: what this shard took and ended, for the shards
// that count on it; then wake a sender whose window to us was full.
void Shard::publish_intake() {
  if (board_ == nullptr) return;
  bool took = false;
  for (u16 o = 0; o < ended_at_.size(); ++o) {
    if (ended_at_[o] != ended_pub_[o]) {
      board_->ended(id_, o).store(ended_at_[o], std::memory_order_release);
      ended_pub_[o] = ended_at_[o];
    }
  }
  for (u16 src = 0; src < taken_from_.size(); ++src) {
    if (taken_from_[src] != taken_pub_[src]) {
      board_->taken(id_, src).store(taken_from_[src], std::memory_order_release);
      took = true;
    }
  }
  if (!took) return;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  for (u16 src = 0; src < taken_from_.size(); ++src) {
    if (taken_from_[src] == taken_pub_[src]) continue;
    taken_pub_[src] = taken_from_[src];
    std::atomic<u64>& w = board_->wait(src, id_);
    if (w.load(std::memory_order_relaxed) != 0) {
      w.store(0, std::memory_order_relaxed);
      sri_wake(src);
    }
  }
}

// 3.5: forwarded new mutation work is refused by the owner while its own
// follow-up backlog is past the gate its clients see.
bool Shard::owner_refuses(MsgKind kind, u32 op_or_type) const {
  if (!intake_.owner_gate || adm_.legacy || emits_in_flight() <= kEmitBacklog) return false;
  if (kind == MsgKind::EXEC_REQ) {
    const OpDescriptor* d = registry().op(op_or_type);
    if (d == nullptr || d->kind == OpKind::READ) return false;
  }
  const_cast<Shard*>(this)->stats_.overloaded++;
  const_cast<Shard*>(this)->stats_.refused_owner++;
  return true;
}

// 3.9.2: what a trigger asks for joins the pass owed after the active one,
// so triggers during a pass cost at most one more pass.
void Shard::owe_resume(u64 lsn, int slot) {
  ++stats_.resume_triggers;
  if (scan_.active || scan_.owed_all || scan_.owed_any) ++stats_.resume_coalesced;
  if (slot < 0) {
    scan_.owed_all = true;
  } else {
    scan_.owed_slots[static_cast<u16>(slot)] = 1;
    scan_.owed_any = true;
  }
  if (lsn != 0) scan_.owed_lsn = true;
}

bool Shard::resume_step() {
  ResumeScan& sc = scan_;
  if (repl_cfg_.standby || withdrawn_) {
    // Nothing resumes here any more (a standby never did).
    sc.active = sc.owed_all = sc.owed_any = sc.owed_lsn = false;
    sc.page.clear();
    return false;
  }
  if (!sc.active) {
    if (!sc.owed_all && !sc.owed_any) return false;
    sc.active = true;
    sc.all = sc.owed_all;
    sc.slots.swap(sc.owed_slots);
    std::fill(sc.owed_slots.begin(), sc.owed_slots.end(), u8{0});
    // A trigger that asked to wait for the WAL so far (link up, map change,
    // a slot): the whole log up to this pass's start, which follows it.
    const u64 tail = wal_ ? wal_->next_lsn() - 1 : 0;
    sc.base_lsn = sc.owed_lsn ? tail : 0;
    sc.owed_all = sc.owed_any = sc.owed_lsn = false;
    map_.walk_begin(sc.walk);
    sc.page.clear();
    sc.page_at = 0;
    sc.walk_done = false;
    ++stats_.resume_passes;
  }
  constexpr u32 kObjectsPerStep = 256;
  constexpr std::size_t kPage = 64;
  bool did = false;
  u32 budget = kObjectsPerStep;
  while (budget != 0) {
    if (sc.page_at == sc.page.size()) {
      sc.page.clear();
      sc.page_at = 0;
      if (sc.walk_done) {
        sc.active = false;  // an owed pass starts at the next poll
        return true;
      }
      // A rehash moved everything: start the walk again (objects seen
      // twice re-emit what they did; their exchanges are idempotent).
      if (map_.generation() != sc.walk.gen) {
        map_.walk_begin(sc.walk);
        ++stats_.resume_restarts;
      }
      sc.walk_done = map_.walk_step(
          sc.walk,
          [&](const ObjectHeader* o) {
            if (!o->deleted() && (sc.all || sc.slots[slot_of(o->id)])) sc.page.push_back(o->id);
          },
          [&] { return sc.page.size() >= kPage; });
      did = true;
      continue;
    }
    // Paged under X: roots are added only while the budget has room, so a
    // pass adds at most one object's max_resume_emits past it.
    if (exchange_full()) break;
    const ObjectID id = sc.page[sc.page_at++];
    --budget;
    did = true;
    ObjectHeader* o = map_.find(id);
    if (o == nullptr || o->deleted() || !serving(slot_of(id))) continue;
    const TypeDescriptor* t = registry().type(o->type_id);
    if (t == nullptr || t->resume == nullptr) continue;
    OpCtx c;
    c.obj_ = o;
    c.type_ = t;
    resuming_ = true;
    t->resume(c);
    resuming_ = false;
    ++stats_.resume_objects;
    // What it emits reflects its state now (3.9.2): it waits for the WAL
    // position current when the hook ran. (Not the pass's start: client
    // ops run between start_serving and the pass, and between pages.)
    const u64 lsn = std::max(sc.base_lsn, wal_ ? wal_->next_lsn() - 1 : 0);
    if (!emit_scratch_.empty()) queue_emits(lsn);
  }
  return did;
}

// Ready follow-ups wait per destination: local ones run at the next poll,
// remote ones go out in order until that destination's ring is full.
void Shard::route_emit(Emit e) {
  ++emit_ready_count_;
  if (cluster_) {
    if (e.epoch > static_cast<u32>(cluster_->epoch)) {
      // Emitted under a newer map than this shard's (another shard of this
      // node adopted it first): never routed by an older one. Held until
      // this shard adopts it too.
      --emit_ready_count_;
      hold_emit(e);
      return;
    }
    const u16 slot = slot_of(e.target);
    const u16 group = cluster_->group_of[slot];
    if (stale(e)) {
      --emit_ready_count_;
      ++stats_.emits_stale;
      release_link(e);
      end_exchange(e.origin);
      return;
    }
    if (group != group_ && e.remote) {
      // Not ours by our map, and its source is another node's: only the
      // node holding the source sends it (that node re-sends it after its
      // own map is refreshed). Nothing is forwarded twice.
      --emit_ready_count_;
      ++stats_.node_emits_misrouted;
      release_link(e);
      end_exchange(e.origin);
      return;
    }
    if (group != group_) {
      if (!links_) {
        // Recovery's resume pass, before the links open: re-emitted once
        // they come up.
        --emit_ready_count_;
        ++stats_.node_emits_dropped;
        release_link(e);
        end_exchange(e.origin);
        return;
      }
      emit_node_[links_->link_of(group, cluster_->shard_of(slot))].push_back(e);
      return;
    }
  }
  u16 owner = owner_of(e.target);
  if (owner == id_) {
    emit_local_.push_back(e);
    if (emit_local_.size() > stats_.followups_staged_peak) stats_.followups_staged_peak = emit_local_.size();
  } else {
    if (emit_remote_.size() <= owner) emit_remote_.resize(slots_.n_shards);
    emit_remote_[owner].push_back(e);
  }
}

bool Shard::run_emits() {
  if (emits_paused_ || emit_ready_count_ == 0) return false;
  bool did = false;
  if (!emit_local_.empty()) {
    emit_running_.swap(emit_local_);  // runs may emit more into emit_local_
    std::size_t i = 0;
    for (; i < emit_running_.size(); ++i) {
      // 3.1: each follow-up applies only while the WAL has room for it.
      if (!followup_gate_open()) {
        ++stats_.followups_gated;
        break;
      }
      --emit_ready_count_;
      run_emit(emit_running_[i]);
      did = true;
    }
    if (i < emit_running_.size()) {
      // The rest wait, still ahead of whatever the runs queued.
      emit_running_.erase(emit_running_.begin(), emit_running_.begin() + static_cast<std::ptrdiff_t>(i));
      emit_running_.insert(emit_running_.end(), emit_local_.begin(), emit_local_.end());
      emit_local_.swap(emit_running_);
    }
    emit_running_.clear();
  }
  for (u16 dst = 0; dst < emit_remote_.size(); ++dst) {
    Fifo<Emit>& q = emit_remote_[dst];
    while (!q.empty()) {
      if (!window_open(dst)) break;      // 3.2: dst has W of ours in staging
      if (!send_emit(dst, q.front())) break;  // ring full: keep the rest in order
      ++sent_to_[dst];
      release_link(q.front());
      q.pop_front();
      --emit_ready_count_;
      did = true;
    }
  }
  bool node_left = false;
  for (u32 i = 0; i < emit_node_.size(); ++i) {
    Fifo<Emit>& q = emit_node_[i];
    if (q.empty()) continue;
    if (!links_->up(i)) {
      // Lost with the link: re-emitted when it comes back up.
      stats_.node_emits_dropped += q.size();
      emit_ready_count_ -= q.size();
      while (!q.empty()) {
        release_link(q.front());
        end_exchange(q.front().origin);
        q.pop_front();
      }
      continue;
    }
    while (!q.empty() && links_->room(i)) {
      Emit& e = q.front();
      NodeLinks::Frame f{};
      f.source = e.source;
      f.target = e.target;
      f.op = e.op;
      f.fail_op = e.fail_op;
      f.len = e.len;
      f.epoch = e.epoch;
      std::memcpy(f.args, e.args, e.len);
      links_->push(i, f);
      // Leaving the node ends its exchange here (D.1, 3.9.3).
      release_link(e);
      end_exchange(e.origin);
      q.pop_front();
      --emit_ready_count_;
      ++stats_.node_emits_sent;
      did = true;
    }
    node_left |= !q.empty();
  }
  // A link without room keeps the loop awake, as before D.1.
  return did || node_left;
}

void Shard::run_emit(const Emit& in) {
  Emit e = in;
  // It leaves staging, whatever happens to it next: its sender's intake
  // window gets the room back (3.2, 3.7). Copies (held, re-routed) owe none.
  if (e.from != kNoShard) {
    ++taken_from_[e.from];
    e.from = kNoShard;
  }
  // Fenced: nothing runs here any more. Follow-ups are recoverable: their
  // sources re-send them to the new primary (docs/TRANSFERS.md).
  if (withdrawn_) {
    release_link(e);
    end_exchange(e.origin);
    return;
  }
  if (cluster_ && e.epoch > static_cast<u32>(cluster_->epoch)) {
    hold_emit(e);  // from a shard of this node that adopted a newer map first (route_emit)
    return;
  }
  if (stale(e)) {
    ++stats_.emits_stale;
    release_link(e);
    end_exchange(e.origin);
    return;
  }
  if (cluster_ && !serving(slot_of(e.target))) {
    // Ours by the map but sealed or staged here: held until the slot is
    // activated, unsealed, or the map moves it (docs/MIGRATION.md). Not
    // ours any more (the map changed since it was routed): routed again.
    if (foreign(e.target)) route_emit(e);
    else hold_emit(e);
    return;
  }
  release_link(e);
  ExecOptions opts;
  opts.durability = Durability::APPLIED;  // the follow-up's own emits wait for its record
  opts.allow_undurable = true;
  RunOut out;
  out.r.id = e.target;
  cur_source_ = e.source;
  cur_origin_ = e.origin;
  const u64 emitted = stats_.emits;
  run_exec(e.target, snc_hash64(e.target), e.op, ByteView(e.args, e.len), opts, out,
           /*client=*/false);
  cur_source_ = ObjectID{};
  cur_origin_ = kNoShard;
  const bool replied = stats_.emits != emitted;
  // No reply and no fail op: the exchange is over (3.4, 3.7).
  if (!emit_result(e, out.r.status) && !replied) end_exchange(e.origin);
}

bool Shard::send_emit(u16 dst, const Emit& e) {
  // The exchange is between this shard and dst (owners within a node are
  // fixed, so a reply never reaches a third shard): one bit names its origin.
  if (e.origin != kNoShard && e.origin != id_ && e.origin != dst) {
    std::fprintf(stderr, "snc: shard %u: follow-up exchange of shard %u sent to a third shard %u\n", id_,
                 e.origin, dst);
    std::abort();
  }
  Msg* m = sri_claim(dst);
  if (m == nullptr) return false;
  zero_header(*m);
  m->kind = MsgKind::EMIT;
  m->flags = kMsgArgsInline | (e.remote ? kMsgEmitRemote : 0) |
             (e.origin == kNoShard ? kMsgEmitOriginNone : e.origin == id_ ? kMsgEmitOriginSender : 0);
  m->src_shard = id_;
  m->op_id = e.op;
  m->req_id = e.source.hi;
  m->expected_version = e.source.lo;
  m->status = static_cast<u16>(e.fail_op);
  m->aux = static_cast<u16>(e.fail_op >> 16);
  m->target = e.target;
  // EMIT: args length (at most 16) in the low byte, the emission epoch's
  // low 24 bits above it (rebuilt against the receiver's, never far off).
  m->len = e.len | (e.epoch << 8);
  if (e.len) std::memcpy(m->inline_data, e.args, e.len);
  sri_commit(dst, m);
  ++stats_.emits_sent;
  return true;
}

void Shard::serve_emit(const Msg& m) {
  ++stats_.emits_received;
  Emit e;
  e.source = ObjectID{m.req_id, m.expected_version};
  e.target = m.target;
  e.op = m.op_id;
  e.fail_op = OpID{m.status} | (OpID{m.aux} << 16);
  e.len = static_cast<u8>(m.len & 0xff);
  e.remote = (m.flags & kMsgEmitRemote) != 0;
  e.origin = (m.flags & kMsgEmitOriginNone) ? kNoShard : (m.flags & kMsgEmitOriginSender) ? m.src_shard : id_;
  e.from = m.src_shard;  // owed a credit once it leaves staging (3.2)
  if (cluster_) {
    // Both shards' maps are this node's: the sender's epoch is within a
    // few of ours, so the nearest value with these low 24 bits is it.
    const u32 cur = static_cast<u32>(cluster_->epoch), low = m.len >> 8;
    u32 e32 = (cur & ~0xffffffu) | low;
    if (e32 > cur + 0x800000u) e32 -= 0x1000000u;
    else if (e32 + 0x800000u < cur) e32 += 0x1000000u;
    e.epoch = e32;
  }
  if (e.len) std::memcpy(e.args, m.inline_data, e.len);
  // Rings always drain (3.1): a follow-up behind others, or one the WAL has
  // no room for, is staged in order; never ahead of what is queued.
  const bool gated = emit_local_.empty() && !followup_gate_open();
  if (!emit_local_.empty() || gated) {
    if (gated) ++stats_.followups_gated;
    ++emit_ready_count_;
    emit_local_.push_back(e);
    if (emit_local_.size() > stats_.followups_staged_peak) stats_.followups_staged_peak = emit_local_.size();
    return;
  }
  run_emit(e);
}

// Emitted under a map older than a move of its source's slot or its
// target's: dropped (docs/MIGRATION.md, follow-ups). A copy from before a
// move can be overtaken by one sent after it (the source's new owner, or
// a target that became local and so no longer waits for durability), and
// if it then runs after its exchange has finished and been forgotten, it
// runs twice. Nothing is lost: every node re-sends all it has unsettled,
// stamped with the new epoch, once it adopts a map (map_changed).
bool Shard::stale(const Emit& e) const {
  if (cluster_ == nullptr) return false;
  if (e.epoch < moved_at_[slot_of(e.target)]) return true;
  return !e.source.is_null() && e.epoch < moved_at_[slot_of(e.source)];
}

void Shard::hold_emit(const Emit& e) {
  emit_held_.push_back(e);
  ++stats_.emits_held;
}

// The slot's state or the map changed: everything held goes round again.
void Shard::release_held() {
  if (emit_held_.empty()) return;
  std::vector<Emit> held;
  held.swap(emit_held_);
  for (const Emit& e : held) route_emit(e);
}

// True when it routed the fail op, which continues the exchange.
bool Shard::emit_result(const Emit& e, Status s) {
  switch (s) {
    case Status::E_NOT_FOUND:
    case Status::E_TYPE_MISMATCH:
    case Status::E_UNKNOWN_OP:
      if (e.fail_op != 0) {
        ++stats_.emits_failed_over;
        Emit f = e;  // runs on the source, with the missing target as its source
        f.source = e.target;
        f.target = e.source;
        f.op = e.fail_op;
        f.fail_op = 0;
        f.lsn = 0;
        f.remote = false;  // from here, on behalf of the missing target (a slot of ours)
        f.epoch = cluster_ ? static_cast<u32>(cluster_->epoch) : 0;
        route_emit(f);
        return true;
      }
      break;
    default:
      break;
  }
  return false;
}


// ---- Slot moves (docs/MIGRATION.md) ------------------------------------------------

const char* slot_proof_name(SlotProof p) {
  switch (p) {
    case SlotProof::OK: return "ok";
    case SlotProof::FOREIGN: return "foreign";
    case SlotProof::SEALED: return "sealed";
    case SlotProof::STAGED: return "staged";
    case SlotProof::MISSING: return "missing";
    case SlotProof::STALE_MAP: return "stale-map";
  }
  return "?";
}

u64 slot_object_digest(const ObjectID& id, u64 version, u32 crc) {
  u64 x = snc_hash64(id) ^ (version * 0x9E3779B97F4A7C15ull) ^ (u64{crc} << 17);
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

namespace {
// Application objects move with their slot; the runtime's own (sessions,
// slot states) are per shard and stay.
bool moves_with_slot(u32 type_id) { return type_id >= 256; }
u32 image_crc(u32 type_id, u16 schema_ver, const u8* payload, u32 len) {
  u8 head[6];
  std::memcpy(head, &type_id, 4);
  std::memcpy(head + 4, &schema_ver, 2);
  return crc32c(payload, len, crc32c(head, sizeof(head)));
}
u32 image_crc(const ObjectHeader& o) { return image_crc(o.type_id, o.schema_ver, o.payload(), o.length); }
}  // namespace

ObjectHeader* Shard::slot_state(u16 slot) {
  auto mi = indexes_.find(kSlotStateIndexId);
  if (mi == indexes_.end()) return nullptr;
  auto it = mi->second.find(std::string(reinterpret_cast<const char*>(&slot), sizeof(slot)));
  if (it == mi->second.end()) return nullptr;
  for (const ObjectID& id : it->second) {
    ObjectHeader* o = map_.find(id);
    if (o && !o->deleted()) return o;
  }
  return nullptr;
}

// One state object per slot, created the first time; logged like any write,
// so it is durable (and on the standby) exactly when an ack would be.
u64 Shard::write_slot_state(u16 slot, const SlotStateV1& st) {
  ObjectHeader* o = slot_state(slot);
  RecType type = RecType::UPDATE;
  if (o == nullptr) {
    const TypeDescriptor* t = registry().type(kSlotStateTypeId);
    ObjectID sid = minter_.next();
    while (owner_of(sid) != id_ || foreign(sid)) sid = minter_.next();
    const std::size_t bytes = sizeof(ObjectHeader) + sizeof(SlotStateV1);
    o = static_cast<ObjectHeader*>(alloc_.alloc(bytes, kObjectAlign));
    if (o == nullptr) {
      std::fprintf(stderr, "snc: out of memory for a slot state on shard %u\n", id_);
      std::abort();
    }
    std::memset(static_cast<void*>(o), 0, bytes);
    o->id = sid;
    o->type_id = kSlotStateTypeId;
    o->schema_ver = t->schema_ver;
    o->length = sizeof(SlotStateV1);
    o->capacity = static_cast<u32>(alloc_.usable(o) - sizeof(ObjectHeader));
    map_.upsert(o);
    index_insert(kSlotStateIndexId, reinterpret_cast<const u8*>(&slot), sizeof(slot), sid);
    type = RecType::CREATE;
  }
  std::memcpy(o->payload(), &st, sizeof(st));
  ++o->version;
  o->last_lsn = log(type, *o);
  set_cache(slot, &st);
  return o->last_lsn;
}

// Every slot this shard owns, with or without a state object: a slot with
// none but an arrival on record is exactly the lost evidence the proof
// must catch (docs/MIGRATION_FAIL_CLOSED.md 4, "never-moved NONE vs
// evidence-lost NONE").
void Shard::refresh_slots() {
  if (phases_ == nullptr) return;
  for (u32 s = 0; s < kSlots; ++s) {
    if (slots_.owner[s] == id_) recompute_slot(static_cast<u16>(s));
  }
}

// PROVEN (docs/MIGRATION_FAIL_CLOSED.md 4): local evidence that the image
// here belongs to the ownership era the map's arrival `a` began.
//   - no arrival on record (never moved, or moved before placement v2):
//     today's rule, the slot is proven;
//   - ACTIVE by exactly that move;
//   - or a later move's seal here, undone because that move aborted. A seal
//     needs a proven, served slot, and move IDs grow, so such a record was
//     written in this era, and a replica holding it holds everything before
//     it. An undone seal of an EARLIER move is an older era's (D1d).
// Nothing else proves it: not phase NONE without a record, not an older
// ACTIVE (D1c), not a STAGED (activation turns the right one into ACTIVE).
static bool proven(const SlotArrival& a, const SlotStateV1* st) {
  if (a.move == 0) return true;
  if (st == nullptr) return false;
  const bool unsealed = (st->flags & kSlotUnsealed) != 0;
  const auto ph = static_cast<SlotPhase>(st->phase);
  if (ph == SlotPhase::ACTIVE && !unsealed && st->move_id == a.move) return true;
  return unsealed && st->move_id > a.move && (ph == SlotPhase::NONE || ph == SlotPhase::ACTIVE);
}

u8 Shard::cache_value(u16 slot, const SlotStateV1* st) const {
  const u8 ph = st ? st->phase : 0;
  if (cluster_ == nullptr || ph > static_cast<u8>(SlotPhase::ACTIVE)) return ph;  // sealed, staged: never served
  if (cluster_->group_of[slot] != group_) return ph;  // another group's: never served
  return proven(cluster_->arrived[slot], st) ? ph : kCacheUnproven;
}

void Shard::set_cache(u16 slot, const SlotStateV1* st) {
  if (phases_ == nullptr) return;
  const u8 v = cache_value(slot, st);
  const u8 was = phases_[slot].exchange(v, std::memory_order_relaxed);
  // A STAGED record older than the arrival (D1b) keeps its phase in the
  // cache (never served either way) but is as unproven as the rest: said
  // once, when the record or the arrival changes.
  const SlotArrival arr = cluster_ ? cluster_->arrived[slot] : SlotArrival{};
  const bool stale_staged = v == static_cast<u8>(SlotPhase::STAGED) && st && arr.move != 0 &&
                            st->move_id < arr.move && cluster_->group_of[slot] == group_;
  const std::pair<u64, u64> key{arr.move, stale_staged ? st->move_id : 0};
  auto said = stale_staged_said_.find(slot);
  if (stale_staged && (said == stale_staged_said_.end() || said->second != key)) {
    stale_staged_said_[slot] = key;
    std::fprintf(stderr,
                 "snc: shard %u: slot %u MIGRATION_STATE_INCOMPLETE: placement says arrived by move %llx at epoch "
                 "%llu; local state staged move %llx (older) - not serving\n",
                 id_, slot, (unsigned long long)arr.move, (unsigned long long)arr.epoch,
                 (unsigned long long)st->move_id);
    SNC_TRACE("slot_unproven slot=%u move=%llx local_phase=3 local_move=%llx", slot, (unsigned long long)arr.move,
              (unsigned long long)st->move_id);
  }
  if (v == kCacheUnproven && was != kCacheUnproven) {
    const SlotArrival& a = cluster_->arrived[slot];
    std::fprintf(stderr,
                 "snc: shard %u: slot %u MIGRATION_STATE_INCOMPLETE: placement says arrived by move %llx at epoch "
                 "%llu; local state %s move %llx%s - not serving\n",
                 id_, slot, (unsigned long long)a.move, (unsigned long long)a.epoch,
                 st == nullptr ? "none" : st->phase == 1 ? "active" : "none",
                 (unsigned long long)(st ? st->move_id : 0),
                 st && (st->flags & kSlotUnsealed) ? " unsealed" : "");
    SNC_TRACE("slot_unproven slot=%u move=%llx local_phase=%u local_move=%llx", slot, (unsigned long long)a.move,
              st ? st->phase : 0u, (unsigned long long)(st ? st->move_id : 0));
  }
}

void Shard::recompute_slot(u16 slot) {
  if (const ObjectHeader* o = slot_state(slot)) {
    SlotStateV1 st;
    std::memcpy(&st, o->payload(), sizeof(st));
    set_cache(slot, &st);
  } else {
    set_cache(slot, nullptr);
  }
}

SlotProof Shard::proof_of(u16 slot, const SlotStateV1* st) const {
  if (cluster_ == nullptr) return SlotProof::OK;
  if (cluster_->group_of[slot] != group_) return SlotProof::FOREIGN;
  const u8 v = cache_value(slot, st);
  if (v == static_cast<u8>(SlotPhase::SEALED)) return SlotProof::SEALED;
  if (v == static_cast<u8>(SlotPhase::STAGED)) {
    // Staged by a move older than the arrival: it can never activate (exact
    // activation), so nothing here will prove this slot.
    const u64 a = cluster_->arrived[slot].move;
    return st && a != 0 && st->move_id < a ? SlotProof::MISSING : SlotProof::STAGED;
  }
  if (v != kCacheUnproven) return SlotProof::OK;
  // Evidence of a later move than the map's arrival, not undone by an
  // abort: this node's map is older than its state.
  if (st && !(st->flags & kSlotUnsealed) && st->move_id > cluster_->arrived[slot].move) return SlotProof::STALE_MAP;
  return SlotProof::MISSING;
}

u64 Shard::unproven_count() const {
  u64 n = 0;
  if (phases_ == nullptr) return 0;
  for (u32 s = 0; s < kSlots; ++s) {
    if (slots_.owner[s] != id_) continue;
    const u8 v = phases_[s].load(std::memory_order_relaxed);
    if (v == kCacheUnproven) {
      ++n;
    } else if (v == static_cast<u8>(SlotPhase::STAGED) && cluster_ && cluster_->group_of[s] == group_) {
      // A stale STAGED (older than the arrival) proves nothing either.
      SlotStateV1 st;
      const ObjectHeader* o = const_cast<Shard*>(this)->slot_state(static_cast<u16>(s));  // a lookup; changes nothing
      if (o) std::memcpy(&st, o->payload(), sizeof(st));
      n += proof_of(static_cast<u16>(s), o ? &st : nullptr) == SlotProof::MISSING;
    }
  }
  return n;
}

void Shard::set_map(const ClusterMap* map) {
  for (u32 s = 0; s < kSlots; ++s) {
    if (cluster_->group_of[s] != map->group_of[s]) moved_at_[s] = static_cast<u32>(map->epoch);
  }
  cluster_ = map;
  if (links_) links_->set_epoch(map->epoch);
  // The proof first: nothing below may run on a slot the new map's arrival
  // leaves unproven (invariant 17, pass 1).
  refresh_slots();
  // At once, before anything else runs under the new map: what was held
  // for it goes ahead of whatever arrives from now on.
  activate_staged();
  release_held();
}

void Shard::map_changed() {
  // What this node sent by its old map may have gone to a node that no
  // longer owns the target, and been dropped there: send it all again,
  // stamped with the new epoch. Only now: every shard of this node routes
  // by the new map, so nothing re-sent here is routed by an older one.
  if (!repl_cfg_.standby && !withdrawn_) resume_all(wal_ ? wal_->next_lsn() - 1 : 0);
}

// A staged slot the map now gives this group (at or past the epoch its move
// commits at) becomes this group's: only a committed move can make the map
// say so while the slot is staged here for it (docs/MIGRATION.md, proof).
void Shard::activate_staged() {
  if (cluster_ == nullptr || phases_ == nullptr || repl_cfg_.standby || withdrawn_) return;
  std::vector<SlotStateV1> ready;
  map_.for_each([&](const ObjectHeader* o) {
    if (o->deleted() || o->type_id != kSlotStateTypeId) return;
    SlotStateV1 st;
    std::memcpy(&st, o->payload(), sizeof(st));
    // Exact (docs/MIGRATION_FAIL_CLOSED.md 4): the move the map's arrival
    // names, not any staged move whose epoch it passes (an aborted move's
    // STAGED can share the epoch of the move that committed: D1b).
    const SlotArrival& a = cluster_->arrived[st.slot];
    if (st.phase == static_cast<u8>(SlotPhase::STAGED) && cluster_->group_of[st.slot] == group_ &&
        cluster_->epoch >= st.epoch && (a.move == 0 || a.move == st.move_id)) {
      ready.push_back(st);
    }
  });
  for (SlotStateV1 st : ready) {
    st.phase = static_cast<u8>(SlotPhase::ACTIVE);
    write_slot_state(st.slot, st);
    SNC_TRACE("slot_active slot=%u move=%llx epoch=%llu", st.slot, (unsigned long long)st.move_id,
              (unsigned long long)st.epoch);
    std::fprintf(stderr, "snc: shard %u: slot %u active here (move %llx, epoch %llu)\n", id_, st.slot,
                 (unsigned long long)st.move_id, (unsigned long long)st.epoch);
    resume_slot(st.slot);
  }
  if (!ready.empty()) release_held();
}

void Shard::resume_slot(u16 slot) {
  // Only a slot this node serves: an unseal that leaves the slot without
  // proof (D1d: an older seal reopened) must not re-emit its stale
  // objects' obligations (docs/MIGRATION_FAIL_CLOSED.md 12).
  if (cluster_ && !serving(slot)) return;
  owe_resume(wal_ ? wal_->next_lsn() - 1 : 0, slot);
}

SlotReport Shard::slot_report_head(u16 slot) {
  SlotReport r;
  r.slot = slot;
  SlotStateV1 st{};
  const ObjectHeader* so = slot_state(slot);
  if (so) std::memcpy(&st, so->payload(), sizeof(st));
  r.proof = proof_of(slot, so ? &st : nullptr);
  if (cluster_) r.arrived = cluster_->arrived[slot];
  if (ObjectHeader* o = slot_state(slot)) {
    r.flags = st.flags;
    r.phase = st.phase;
    r.group = st.group;
    r.move_id = st.move_id;
    r.epoch = st.epoch;
    r.count = st.count;
    r.digest = st.digest;
    r.durable = durable(o->last_lsn);
  }
  r.serving = serving(slot) && !repl_cfg_.standby && !withdrawn_;
  if (cluster_) {
    r.map_group = cluster_->group_of[slot];
    r.map_epoch = cluster_->epoch;
  }
  return r;
}

SlotReport Shard::slot_report(u16 slot) {
  SlotReport r = slot_report_head(slot);
  map_.for_each([&](const ObjectHeader* o) {
    if (o->deleted() || !moves_with_slot(o->type_id) || slot_of(o->id) != slot) return;
    ++r.live_count;
    r.live_digest += slot_object_digest(o->id, o->version, image_crc(*o));
  });
  return r;
}

bool Shard::slot_report_unit(ReportWalk& w, SlotReport& out, u64 unit_ns) {
  const u64 deadline = trace::now_ns() + unit_ns;
  // Restart if the table grew, staging changed the slot, or the slot
  // turned served (activated: client ops may then change it too).
  if (!w.started || map_.generation() != w.walk.gen || slot_mut_[w.slot] != w.mut ||
      foreign_slot(w.slot) != w.foreign) {
    if (w.started) ++w.restarts;
    w.started = true;
    map_.walk_begin(w.walk);
    w.mut = slot_mut_[w.slot];
    w.foreign = foreign_slot(w.slot);
    w.n = w.d = 0;
  }
  const bool complete = map_.walk_step(
      w.walk,
      [&](const ObjectHeader* o) {
        if (o->deleted() || !moves_with_slot(o->type_id) || slot_of(o->id) != w.slot) return;
        ++w.n;
        w.d += slot_object_digest(o->id, o->version, image_crc(*o));
      },
      [&] { return trace::now_ns() >= deadline; });
  if (!complete) return false;
  out = slot_report_head(w.slot);
  out.live_count = w.n;
  out.live_digest = w.d;
  return true;
}

void Shard::slot_states(std::vector<SlotReport>& out) {
  map_.for_each([&](const ObjectHeader* o) {
    if (o->deleted() || o->type_id != kSlotStateTypeId) return;
    SlotStateV1 st;
    std::memcpy(&st, o->payload(), sizeof(st));
    SlotReport r;
    r.slot = st.slot;
    r.phase = st.phase;
    r.flags = st.flags;
    r.group = st.group;
    r.move_id = st.move_id;
    r.epoch = st.epoch;
    r.durable = durable(o->last_lsn);
    out.push_back(r);
  });
}

void Shard::slot_objects(u16 slot, bool payloads, std::vector<SlotObject>& out) {
  map_.for_each([&](const ObjectHeader* o) {
    if (o->deleted() || !moves_with_slot(o->type_id) || slot_of(o->id) != slot) return;
    SlotObject so;
    so.id = o->id;
    so.version = o->version;
    so.type_id = o->type_id;
    so.schema_ver = o->schema_ver;
    so.crc = image_crc(*o);
    if (payloads) {
      so.has_payload = true;
      so.payload.assign(o->payload(), o->payload() + o->length);
    }
    out.push_back(std::move(so));
  });
}

Status Shard::seal_slot(u16 slot, u64 move, u16 to, u64 epoch, u64& lsn) {
  lsn = 0;
  if (cluster_ == nullptr || repl_cfg_.standby || withdrawn_) return Status::E_STANDBY;
  ObjectHeader* o = slot_state(slot);
  SlotStateV1 cur{};
  if (o) std::memcpy(&cur, o->payload(), sizeof(cur));
  const auto ph = static_cast<SlotPhase>(o ? cur.phase : 0);
  // Move IDs grow with every move: a request for an older one is late, as
  // is one for a move whose seal was undone (aborted: it never returns).
  if (o && (cur.move_id > move || (cur.move_id == move && (cur.flags & kSlotUnsealed)))) {
    return Status::E_MIGRATING;
  }
  if (ph == SlotPhase::SEALED) {
    if (cur.move_id != move) return Status::E_MIGRATING;  // sealed for another move
    lsn = o->last_lsn;  // again: the same seal
    return Status::OK;
  }
  if (ph == SlotPhase::STAGED) return Status::E_MIGRATING;
  if (foreign_slot(slot)) return Status::E_MOVED;
  // An unproven slot is never sealed: an aborted move's undone seal is
  // proof of the era it was written in (docs/MIGRATION_FAIL_CLOSED.md 9).
  if (phases_[slot].load(std::memory_order_relaxed) == kCacheUnproven) return Status::E_MIGRATING;
  // Every op on the slot runs on this thread: those before this point are
  // in the log before the seal, and none runs after it.
  SlotStateV1 st{};
  st.slot = slot;
  st.phase = static_cast<u8>(SlotPhase::SEALED);
  st.prior = static_cast<u8>(ph);
  st.group = to;
  st.move_id = move;
  st.epoch = epoch;
  map_.for_each([&](const ObjectHeader* x) {
    if (x->deleted() || !moves_with_slot(x->type_id) || slot_of(x->id) != slot) return;
    ++st.count;
    st.digest += slot_object_digest(x->id, x->version, image_crc(*x));
  });
  lsn = write_slot_state(slot, st);
  std::fprintf(stderr, "snc: shard %u: slot %u sealed for move %llx (%llu objects)\n", id_, slot,
               (unsigned long long)move, (unsigned long long)st.count);
  return Status::OK;
}

Status Shard::reseal_slot(u16 slot, u64 move, u16 to, u64 epoch, u64& lsn) {
  lsn = 0;
  if (cluster_ == nullptr || repl_cfg_.standby || withdrawn_) return Status::E_STANDBY;
  if (const ObjectHeader* o = slot_state(slot)) {
    SlotStateV1 cur;
    std::memcpy(&cur, o->payload(), sizeof(cur));
    if (cur.phase == static_cast<u8>(SlotPhase::SEALED) && cur.move_id == move) {
      lsn = o->last_lsn;  // the old primary's seal reached this prefix
      return Status::OK;
    }
  }
  if (phases_[slot].load(std::memory_order_relaxed) == kCacheUnproven) {
    std::fprintf(stderr, "snc: shard %u: slot %u not re-sealed for move %llx: unproven here, so not served\n", id_,
                 slot, (unsigned long long)move);
    return Status::OK;
  }
  const Status st = seal_slot(slot, move, to, epoch, lsn);
  std::fprintf(stderr, "snc: shard %u: slot %u re-sealed for move %llx at the start barrier: %s\n", id_, slot,
               (unsigned long long)move, status_name(st));
  SNC_TRACE("slot_resealed slot=%u move=%llx status=%s", slot, (unsigned long long)move, status_name(st));
  return st;
}

void Shard::apply_reseal() {
  reseal_lsn_ = 0;
  if (!reseal_.on) return;
  reseal_.on = false;
  if (slots_.owner[reseal_.slot] != id_) return;
  u64 lsn = 0;
  if (reseal_slot(reseal_.slot, reseal_.move, reseal_.to, reseal_.epoch, lsn) == Status::OK) reseal_lsn_ = lsn;
}

Status Shard::unseal_slot(u16 slot, u64 move, u64& lsn) {
  lsn = 0;
  if (cluster_ == nullptr || repl_cfg_.standby || withdrawn_) return Status::E_STANDBY;
  ObjectHeader* o = slot_state(slot);
  SlotStateV1 st{};
  if (o) std::memcpy(&st, o->payload(), sizeof(st));
  const auto ph = static_cast<SlotPhase>(o ? st.phase : 0);
  if (ph == SlotPhase::NONE || ph == SlotPhase::ACTIVE) {
    lsn = o ? o->last_lsn : 0;  // not sealed (or unsealed already)
    return Status::OK;
  }
  if (ph != SlotPhase::SEALED || st.move_id != move) return Status::E_MIGRATING;
  st.phase = st.prior;
  st.flags |= kSlotUnsealed;
  lsn = write_slot_state(slot, st);
  std::fprintf(stderr, "snc: shard %u: slot %u unsealed (move %llx aborted)\n", id_, slot, (unsigned long long)move);
  release_held();
  resume_slot(slot);  // its objects were skipped by resume passes while sealed
  return Status::OK;
}

Status Shard::export_slot(u16 slot, u64 move, bool final, const std::vector<SlotObject>& have,
                          std::vector<SlotObject>& out, u64& count, u64& digest) {
  count = digest = 0;
  if (cluster_ == nullptr || repl_cfg_.standby || withdrawn_) return Status::E_STANDBY;
  ObjectHeader* o = slot_state(slot);
  SlotStateV1 st{};
  if (o) std::memcpy(&st, o->payload(), sizeof(st));
  const auto ph = static_cast<SlotPhase>(o ? st.phase : 0);
  const bool sealed_here = ph == SlotPhase::SEALED && st.move_id == move;
  if (final) {
    // Only the sealed state, once durable: nothing on the slot changes after it.
    if (!sealed_here || !durable(o->last_lsn)) return Status::E_MIGRATING;
  } else if (!sealed_here) {
    if (ph == SlotPhase::SEALED || ph == SlotPhase::STAGED) return Status::E_MIGRATING;
    if (foreign_slot(slot)) return Status::E_MOVED;
    if (phases_[slot].load(std::memory_order_relaxed) == kCacheUnproven) return Status::E_MIGRATING;
  }
  std::unordered_map<ObjectID, std::pair<u64, u32>, ObjectIdHash> known;
  for (const SlotObject& h : have) known[h.id] = {h.version, h.crc};
  map_.for_each([&](const ObjectHeader* x) {
    if (x->deleted() || !moves_with_slot(x->type_id) || slot_of(x->id) != slot) return;
    // A copy ahead of the seal takes durable state only: a version lost in
    // a crash before it was durable must never be staged anywhere.
    if (!final && !durable(x->last_lsn)) return;
    SlotObject so;
    so.id = x->id;
    so.version = x->version;
    so.type_id = x->type_id;
    so.schema_ver = x->schema_ver;
    so.crc = image_crc(*x);
    auto k = known.find(x->id);
    so.has_payload = k == known.end() || k->second.first != so.version || k->second.second != so.crc;
    if (so.has_payload) so.payload.assign(x->payload(), x->payload() + x->length);
    ++count;
    digest += slot_object_digest(so.id, so.version, so.crc);
    out.push_back(std::move(so));
  });
  return Status::OK;
}

ObjectHeader* Shard::put_image(const SlotObject& so, u64& lsn) {
  const TypeDescriptor* t = registry().type(so.type_id);
  ObjectHeader* o = map_.find(so.id);
  u8 old_key[kMaxIndexKey];
  const u32 old_len = o && !o->deleted() && t->index_id ? extract_key(*t, *o, old_key) : 0;
  const u32 len = static_cast<u32>(so.payload.size());
  const RecType type = o == nullptr ? RecType::CREATE : RecType::UPDATE;
  if (o == nullptr || o->capacity < len) {
    ObjectHeader** at = o ? map_.slot(o->id) : nullptr;
    void* p = o ? alloc_.resize(o, sizeof(ObjectHeader) + len, kObjectAlign)
                : alloc_.alloc(sizeof(ObjectHeader) + len, kObjectAlign);
    if (p == nullptr) return nullptr;
    o = static_cast<ObjectHeader*>(p);
    if (at) *at = o;
  }
  o->id = so.id;
  o->version = so.version;
  o->type_id = so.type_id;
  o->length = len;
  o->capacity = static_cast<u32>(alloc_.usable(o) - sizeof(ObjectHeader));
  o->flags = 0;
  o->schema_ver = so.schema_ver;
  std::memcpy(o->payload(), so.payload.data(), len);
  map_.upsert(o);
  if (t->index_id) {
    const u64 t0 = trace::enabled() ? trace::now_ns() : 0;
    u8 key[kMaxIndexKey];
    const u32 new_len = extract_key(*t, *o, key);
    if (old_len) index_erase(t->index_id, old_key, old_len, o->id);
    if (new_len) index_insert(t->index_id, key, new_len, o->id);
    if (trace::enabled()) trace_index_ns_ += trace::now_ns() - t0;
  }
  const u64 t_log = trace::enabled() ? trace::now_ns() : 0;
  o->last_lsn = lsn = log(type, *o);
  if (trace::enabled()) trace_log_ns_ += trace::now_ns() - t_log;
  ++slot_mut_[slot_of(o->id)];
  return o;
}

void Shard::drop_object(ObjectHeader* o, u64& lsn) {
  const TypeDescriptor* t = registry().type(o->type_id);
  if (t->index_id) {
    u8 key[kMaxIndexKey];
    const u32 len = extract_key(*t, *o, key);
    if (len) index_erase(t->index_id, key, len, o->id);
  }
  ++o->version;
  lsn = log(RecType::DELETE, *o);
  ++slot_mut_[slot_of(o->id)];
  map_.erase(o->id);
  alloc_.free(o);
}

const char* StageChain::phase_name(u8 p) {
  static const char* const names[] = {"admit", "check", "put", "scan", "drop", "stage", "done"};
  return p <= DONE ? names[p] : "?";
}

Status Shard::stage_admit(StageChain& c) {
  c.lsn = 0;
  const u16 slot = c.slot;
  if (cluster_ == nullptr || repl_cfg_.standby || withdrawn_) return Status::E_STANDBY;
  ObjectHeader* so = slot_state(slot);
  SlotStateV1 cur{};
  if (so) std::memcpy(&cur, so->payload(), sizeof(cur));
  const auto ph = static_cast<SlotPhase>(so ? cur.phase : 0);
  // Move IDs grow with every move: a request for an older one is late (a
  // driver that stalled across an abort and a newer move), and must not
  // replace what a newer move staged here.
  if (so && cur.move_id > c.move) return Status::E_MIGRATING;
  if (ph == SlotPhase::STAGED && cur.move_id == c.move) {
    // Final already: the staged copy is the sealed state and stays so. A
    // late copy from before the seal must not change it.
    if (!c.final) return Status::E_MIGRATING;
    if (cur.count != c.count || cur.digest != c.digest) return Status::E_VERSION_CONFLICT;
    c.lsn = so->last_lsn;
    c.phase = StageChain::DONE;
    return Status::OK;
  }
  // A slot this node's map gives this group is never staged over: here it
  // is the source (serving or sealed), never the destination. A
  // destination's map names the source until the move commits.
  if (!foreign_slot(slot)) return Status::E_MOVED;
  // One chain per slot: a second request for the same or an older move
  // waits for this one (the driver resends it later, and is then answered
  // from what this one did); a newer move's supersedes it (it stops at its
  // next unit).
  auto it = stage_chain_.find(slot);
  if (it != stage_chain_.end() && it->second.second >= c.move) return Status::E_OVERLOADED;
  c.token = ++stage_token_;
  stage_chain_[slot] = {c.token, c.move};
  c.mut = slot_mut_[slot];
  c.phase = StageChain::CHECK;
  c.i = 0;
  return Status::OK;
}

void Shard::stage_release(StageChain& c) {
  auto it = stage_chain_.find(c.slot);
  if (c.token != 0 && it != stage_chain_.end() && it->second.first == c.token) stage_chain_.erase(it);
  c.token = 0;
}

Status Shard::stage_unit(StageChain& c) {
  const u64 t0 = trace::now_ns();
  const u16 slot = c.slot;
  // Guards: local state only, no round trip (docs/MIGRATION_STAGING.md 3.2).
  if (cluster_ == nullptr || repl_cfg_.standby) return Status::E_STANDBY;
  // A fence is seen here before poll() applies it.
  if (withdrawn_ || (sender_ && sender_->fenced()) ||
      (withdraw_flag_ && withdraw_flag_->load(std::memory_order_relaxed))) {
    return Status::E_FENCED;
  }
  auto it = stage_chain_.find(slot);
  if (it == stage_chain_.end() || it->second.first != c.token) return Status::E_MIGRATING;  // superseded
  if (!foreign_slot(slot)) return Status::E_MOVED;
  // Only this chain writes the slot's objects while it runs: another write
  // voids what it checked or counted.
  if (slot_mut_[slot] != c.mut) return Status::E_VERSION_CONFLICT;
  // What became durable since the last unit no longer counts against the window.
  while (!c.pending.empty() && durable(c.pending.front().first)) {
    c.pending_bytes -= c.pending.front().second;
    c.pending.pop_front();
  }
  const std::vector<SlotObject>& objs = *c.objs;
  const u64 deadline = c.unit_ns == ~u64{0} ? ~u64{0} : t0 + c.unit_ns;
  u64 unit_bytes = 0;
  bool did = false;  // at least one object (bucket) per unit
  auto over = [&] { return did && (trace::now_ns() >= deadline || unit_bytes >= c.unit_bytes); };
  auto over_window = [&] { return did && c.pending_bytes > c.window_bytes; };
  auto appended = [&](u64 lsn, u64 bytes) {
    c.lsn = std::max(c.lsn, lsn);
    if (c.window_bytes != ~u64{0}) {  // unbounded (one task): no bookkeeping, nothing to wait for
      c.pending.emplace_back(lsn, bytes);
      c.pending_bytes += bytes;
      c.pending_hw = std::max(c.pending_hw, c.pending_bytes);
    }
    unit_bytes += bytes;
    c.bytes += bytes;
  };
  trace_index_ns_ = trace_log_ns_ = 0;
  const u8 phase_at_start = c.phase;
  Status st = Status::OK;
  while (st == Status::OK && c.phase != StageChain::DONE) {
    if (c.phase == StageChain::CHECK) {
      // Every entry checked before anything is written (a bad entry
      // anywhere refuses the request with nothing put).
      for (; c.i < objs.size(); ++c.i) {
        if (over()) break;
        const SlotObject& x = objs[c.i];
        did = true;
        if (slot_of(x.id) != slot || !moves_with_slot(x.type_id)) return Status::E_BAD_ARGS;
        ObjectHeader* o = map_.find(x.id);
        if (!x.has_payload) {
          // Already here at this state (the destination said so).
          if (o == nullptr || o->deleted() || o->version != x.version || image_crc(*o) != x.crc) {
            return Status::E_VERSION_CONFLICT;
          }
        } else {
          const TypeDescriptor* t = registry().type(x.type_id);
          if (t == nullptr || t->schema_ver != x.schema_ver || x.payload.size() > t->max_size ||
              image_crc(x.type_id, x.schema_ver, x.payload.data(), static_cast<u32>(x.payload.size())) != x.crc ||
              (t->check && !t->check(x.payload.data(), static_cast<u32>(x.payload.size())))) {
            return Status::E_BAD_ARGS;
          }
          // A source's state only moves forward (staged copies are durable states of it).
          if (o != nullptr && !o->deleted() && o->version > x.version) return Status::E_VERSION_CONFLICT;
        }
        if (c.final) c.listed[x.id] = &x;
      }
      if (c.i < objs.size()) break;
      c.phase = StageChain::PUT;
      c.i = 0;
    } else if (c.phase == StageChain::PUT) {
      for (; c.i < objs.size(); ++c.i) {
        if (over() || over_window()) break;
        const SlotObject& x = objs[c.i];
        did = true;  // a skipped entry costs a lookup and a crc: it counts
        if (!x.has_payload) continue;
        ObjectHeader* o = map_.find(x.id);
        if (o && !o->deleted() && o->version == x.version && image_crc(*o) == x.crc) continue;
        u64 at = 0;
        if (put_image(x, at) == nullptr) return Status::E_NO_MEMORY;
        c.mut = slot_mut_[slot];
        ++c.put;
        appended(at, sizeof(ObjectHeader) + x.payload.size());
      }
      if (c.i < objs.size()) break;
      if (!c.final) {
        c.phase = StageChain::DONE;
        break;
      }
      // Final: exactly the source's objects. Anything else staged here for
      // the slot is gone at the source (IDs are never reused).
      c.phase = StageChain::SCAN;
      map_.walk_begin(c.walk);
      c.n = c.d = 0;
      c.gone.clear();
    } else if (c.phase == StageChain::SCAN) {
      if (map_.generation() != c.walk.gen) {  // the table grew: start again
        map_.walk_begin(c.walk);
        c.n = c.d = 0;
        c.gone.clear();
        ++c.scan_restarts;
      }
      const bool complete = map_.walk_step(
          c.walk,
          [&](const ObjectHeader* o) {
            did = true;
            if (o->deleted() || !moves_with_slot(o->type_id) || slot_of(o->id) != slot) return;
            if (!c.listed.count(o->id)) {
              c.gone.push_back(o->id);
              return;
            }
            ++c.n;
            c.d += slot_object_digest(o->id, o->version, image_crc(*o));
          },
          [&] { return trace::now_ns() >= deadline; });
      if (!complete) break;
      if (c.n != c.count || c.d != c.digest) return Status::E_VERSION_CONFLICT;  // nothing recorded as staged
      c.phase = StageChain::DROP;
      c.i = 0;
    } else if (c.phase == StageChain::DROP) {
      for (; c.i < c.gone.size(); ++c.i) {
        if (over() || over_window()) break;
        ObjectHeader* o = map_.find(c.gone[c.i]);
        did = true;
        if (o == nullptr || o->deleted()) continue;
        const u64 bytes = sizeof(ObjectHeader) + o->length;
        u64 at = 0;
        drop_object(o, at);
        c.mut = slot_mut_[slot];
        ++c.dropped;
        appended(at, bytes);
      }
      if (c.i < c.gone.size()) break;
      c.phase = StageChain::STAGE;
      // STAGED is written in a unit of its own, after every unit that wrote
      // objects (a stop or crash point between them; one task when unbounded).
      if (c.unit_ns != ~u64{0}) break;
    } else if (c.phase == StageChain::STAGE) {
      SlotStateV1 st_rec{};
      st_rec.slot = slot;
      st_rec.phase = static_cast<u8>(SlotPhase::STAGED);
      st_rec.group = c.from;
      st_rec.move_id = c.move;
      st_rec.epoch = c.epoch;
      st_rec.count = c.count;
      st_rec.digest = c.digest;
      c.lsn = write_slot_state(slot, st_rec);  // after every staged record in this stream
      c.phase = StageChain::DONE;
      std::fprintf(stderr, "snc: shard %u: slot %u staged for move %llx (%llu objects)\n", id_, slot,
                   (unsigned long long)c.move, (unsigned long long)c.count);
    }
  }
  const u64 took = trace::now_ns() - t0;
  ++c.units;
  ++c.phase_units[phase_at_start < StageChain::DONE ? phase_at_start : 0];
  if (took > c.max_unit_ns) {
    c.max_unit_ns = took;
    c.max_unit_phase = phase_at_start;
  }
  c.shard_ns += took;
  c.unit_us.push_back(static_cast<u32>(std::min<u64>(took / 1000, 0xffffffffu)));
  c.index_ns += trace_index_ns_;
  c.log_ns += trace_log_ns_;
  return st;
}

// ---- Public API -------------------------------------------------------------------------

void execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts, Completion done,
             void* ctx) {
  tl_shard->execute(target, op, args, opts, done, ctx);
}

void create(u32 type_id, ByteView init_args, const ExecOptions& opts, Completion done, void* ctx) {
  tl_shard->create(type_id, init_args, opts, done, ctx);
}

void create_near(const ObjectID& near, u32 type_id, ByteView init_args, const ExecOptions& opts,
                 Completion done, void* ctx) {
  tl_shard->create(type_id, init_args, opts, done, ctx, &near);
}

void remove(ObjectID target, const ExecOptions& opts, Completion done, void* ctx) {
  tl_shard->remove(target, opts, done, ctx);
}

void index_lookup(u32 index_id, ByteView key, u32 max_results, IndexCompletion done, void* ctx) {
  tl_shard->index_lookup(index_id, key, max_results, done, ctx);
}

u16 owner_of(const ObjectID& id) { return tl_shard->owner_of(id); }
const SlotMap& slot_map() { return tl_shard->slots(); }
const ClusterMap* cluster_map() { return tl_shard->cluster(); }
u16 group_id() { return tl_shard->group(); }
void prefetch(const ObjectID* ids, u32 n) { tl_shard->prefetch(ids, n); }

}  // namespace snc
