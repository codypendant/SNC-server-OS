#include "shard.h"

#include <fcntl.h>
#include <unistd.h>
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace snc {

namespace {
constexpr u8 kMsgRespDurable = 1u << 4;  // RESP flag: resulting version is durable
constexpr u32 kCheckpointSlice = 256;    // objects per loop iteration (spec section 12)
constexpr u32 kCheckpointChunk = 1u << 20;

thread_local Shard* tl_shard = nullptr;

[[noreturn]] void contract_violation(const char* what, const OpDescriptor* op) {
  std::fprintf(stderr, "snc: handler contract violated: %s (op %s)\n", what, op ? op->name : "?");
  std::abort();
}

void zero_header(Msg& m) { std::memset(static_cast<void*>(&m), 0, offsetof(Msg, inline_data)); }
}  // namespace

Shard* current_shard() { return tl_shard; }
void set_current_shard(Shard* s) { tl_shard = s; }

Shard::Shard(u16 id, const SlotMap& slots, u64 lease, u64 expected_objects)
    : id_(id),
      slots_(slots),
      map_(expected_objects / (slots.n_shards ? slots.n_shards : 1) + 1024),
      minter_(lease),
      alloc_(sri_allocator()),
      result_scratch_(kMaxResult) {}

Shard::~Shard() {
  if (ckpt_.fd >= 0) ::close(ckpt_.fd);
  // Objects live in the shard heap, which the backend destroys with the worker.
}

// ---- Entry-shard API -----------------------------------------------------------

void Shard::execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts,
                    Completion done, void* ctx) {
  u16 owner = owner_of(target);
  if (owner == id_) {
    ++stats_.ops_local;
    RunOut out;
    run(MsgKind::EXEC_REQ, target, op, args, opts, out);
    complete_local(out, done, ctx);
    return;
  }
  ++stats_.ops_remote;
  send_request(MsgKind::EXEC_REQ, owner, target, op, args, opts, done, ctx);
}

void Shard::create(u32 type_id, ByteView args, const ExecOptions& opts, Completion done,
                   void* ctx) {
  ObjectID id = minter_.next();
  u16 owner = owner_of(id);
  if (owner == id_) {
    ++stats_.ops_local;
    RunOut out;
    run(MsgKind::CREATE_REQ, id, type_id, args, opts, out);
    complete_local(out, done, ctx);
    return;
  }
  ++stats_.ops_remote;
  send_request(MsgKind::CREATE_REQ, owner, id, type_id, args, opts, done, ctx);
}

void Shard::remove(ObjectID target, const ExecOptions& opts, Completion done, void* ctx) {
  u16 owner = owner_of(target);
  if (owner == id_) {
    ++stats_.ops_local;
    RunOut out;
    run(MsgKind::DELETE_REQ, target, 0, ByteView{}, opts, out);
    complete_local(out, done, ctx);
    return;
  }
  ++stats_.ops_remote;
  send_request(MsgKind::DELETE_REQ, owner, target, 0, ByteView{}, opts, done, ctx);
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
    case MsgKind::EXEC_REQ: run_exec(target, op_or_type, args, opts, out); break;
    case MsgKind::CREATE_REQ: run_create(target, op_or_type, args, out); break;
    case MsgKind::DELETE_REQ: run_delete(target, opts, out); break;
    default: std::abort();
  }
}

void Shard::prepare_ctx(OpCtx& c, ObjectHeader* obj, const TypeDescriptor* t,
                        const OpDescriptor* op) {
  c.obj_ = obj;
  c.type_ = t;
  c.op_ = op;
  c.reserved_len_ = obj->capacity;
  c.app_code_ = 0;
  c.result_buf_ = result_scratch_.data();
  c.result_len_ = 0;
  c.result_overflow_ = false;
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

u64 Shard::log(RecType type, const ObjectHeader& obj) {
  if (!wal_) return 0;
  u32 len = type == RecType::DELETE ? 0 : obj.length;
  return wal_->append(type, obj.type_id, obj.id, obj.version, obj.schema_ver, obj.payload(), len);
}

void Shard::run_exec(ObjectID target, OpID op_id, ByteView args, const ExecOptions& opts,
                     RunOut& out) {
  ObjectHeader* obj = map_.find(target);
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
    out.r.status = s;
    out.r.app_code = c.app_code_;
    return;
  }
  if (op->kind == OpKind::READ) {
    if (op->apply) op->apply(c, args);
    out.r.durable = durable(c.obj_->last_lsn);
    if (!opts.allow_undurable) out.wait_lsn = c.obj_->last_lsn;
  } else {
    u8 old_key[kMaxIndexKey], new_key[kMaxIndexKey];
    u32 old_len = t->index_id ? extract_key(*t, *obj, old_key) : 0;
    if (op->apply) op->apply(c, args);
    ObjectHeader* o = c.obj_;
    ++o->version;
    if (t->index_id) {
      u32 new_len = extract_key(*t, *o, new_key);
      if (new_len != old_len || std::memcmp(old_key, new_key, new_len) != 0) {
        if (old_len) index_erase(t->index_id, old_key, old_len, o->id);
        if (new_len) index_insert(t->index_id, new_key, new_len, o->id);
      }
    }
    o->last_lsn = log(RecType::UPDATE, *o);
    out.r.durable = durable(o->last_lsn);
    if (opts.durability == Durability::DURABLE) out.wait_lsn = o->last_lsn;
  }
  out.r.version = c.obj_->version;
  finish_result(c, op, out.r);
}

void Shard::run_create(ObjectID id, u32 type_id, ByteView args, RunOut& out) {
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
  o->last_lsn = log(RecType::CREATE, *o);
  ++stats_.creates;
  out.r.version = 1;
  out.r.durable = durable(o->last_lsn);
  out.wait_lsn = o->last_lsn;  // creates are always durable before completing
  finish_result(c, op, out.r);
}

void Shard::run_delete(ObjectID target, const ExecOptions& opts, RunOut& out) {
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
  if (t->delete_op != 0) {
    const OpDescriptor* op = registry().op(t->delete_op);
    OpCtx c;
    prepare_ctx(c, obj, t, op);
    Status s = validate_checked(c, op, ByteView{});
    if (s != Status::OK) {
      out.r.status = s;
      out.r.app_code = c.app_code_;
      return;
    }
    if (op->apply) op->apply(c, ByteView{});
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
  // Mostly appended in LSN order; reads of older writes insert a little earlier.
  auto it = waiters_.end();
  while (it != waiters_.begin() && std::prev(it)->lsn > w.lsn) --it;
  waiters_.insert(it, std::move(w));
}

void Shard::on_wal_durable(void* ctx) { static_cast<Shard*>(ctx)->release_waiters(); }

void Shard::release_waiters() {
  while (!waiters_.empty() && durable(waiters_.front().lsn)) {
    Waiter w = std::move(waiters_.front());
    waiters_.pop_front();
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
  bool busy = false;
  if (wal_) busy |= wal_->poll(sri_now_ns(), /*force=*/false);
  if (ckpt_.active) {
    checkpoint_step();
    busy = true;
  }
  return busy;
}

bool Shard::quiesced() const {
  return (!wal_ || wal_->idle()) && waiters_.empty() && !ckpt_.active &&
         free_pending_.size() == pending_.size() && free_gathers_.size() == gathers_.size();
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
    if (o && !o->deleted() && durable(o->last_lsn)) out.push_back(id);
  }
}

void Shard::rebuild_index() {
  indexes_.clear();
  u8 key[kMaxIndexKey];
  map_.for_each([&](ObjectHeader* o) {
    if (o->deleted()) return;
    const TypeDescriptor* t = registry().type(o->type_id);
    if (!t->index_id) return;
    u32 len = extract_key(*t, *o, key);
    if (len) index_insert(t->index_id, key, len, o->id);
  });
}

void Shard::index_lookup(u32 index_id, ByteView key, u32 max_results, IndexCompletion done,
                         void* ctx) {
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
  ++p.gen;

  Msg m;
  zero_header(m);
  m.kind = kind;
  m.src_shard = id_;
  m.op_id = op_or_type;
  m.req_id = (u64{p.gen} << 32) | slot;
  m.target = target;
  m.expected_version = opts.expected_version;
  m.len = args.len;
  if (opts.allow_undurable) m.flags |= kMsgAllowUndurable;
  if (opts.durability == Durability::APPLIED) m.flags |= kMsgDurApplied;
  if (args.len <= kMsgInlineBytes) {
    m.flags |= kMsgArgsInline;
    if (args.len) std::memcpy(m.inline_data, args.data, args.len);
  } else {
    m.flags |= kMsgArgsExternal;
    m.ext.ptr = static_cast<u8*>(alloc_.alloc(args.len, 16));
    if (m.ext.ptr == nullptr) {
      fail_pending(slot, target, Status::E_NO_MEMORY);
      return;
    }
    std::memcpy(m.ext.ptr, args.data, args.len);
  }
  if (!sri_send(dst, m)) {
    if (m.flags & kMsgArgsExternal) alloc_.free(m.ext.ptr);
    ++stats_.overloaded;
    fail_pending(slot, target, Status::E_OVERLOADED);
  }
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
  return resp;
}

void Shard::serve(const Msg& m) {
  ++stats_.ops_served;
  ExecOptions opts;
  opts.expected_version = m.expected_version;
  opts.allow_undurable = m.flags & kMsgAllowUndurable;
  opts.durability = (m.flags & kMsgDurApplied) ? Durability::APPLIED : Durability::DURABLE;
  RunOut out;
  run(m.kind, m.target, m.op_id, ByteView(m.data(), m.len), opts, out);
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
  sri_send(m.src_shard, make_resp(m, out.r), /*must_deliver=*/true);
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
  rebuild_index();
  if (!wal_cfg.data_dir.empty()) {
    wal_ = std::make_unique<WalStream>(wal_cfg, id_, next_lsn);
    wal_->on_durable = &Shard::on_wal_durable;
    wal_->on_durable_ctx = this;
  }
}

// ---- Checkpoint (spec section 12) ------------------------------------------------------

void Shard::begin_checkpoint(const std::string& data_dir, const Uuid& uuid, u64 seq,
                             CheckpointDone done) {
  Checkpoint& c = ckpt_;
  c = Checkpoint{};
  c.active = true;
  c.done = std::move(done);
  c.start_lsn = wal_ ? wal_->next_lsn() : 0;
  c.ids.reserve(map_.size());
  map_.for_each([&](ObjectHeader* o) {
    if (!o->deleted()) c.ids.push_back(o->id);
  });
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
  c.chunk.assign(kBlock, 0);
  std::memcpy(c.chunk.data(), &h, sizeof(h));
}

void Shard::checkpoint_write_chunk() {
  Checkpoint& c = ckpt_;
  if (c.chunk.empty()) return;
  auto buf = std::make_unique<std::vector<u8>>();
  buf->swap(c.chunk);
  const u8* data = buf->data();
  u32 len = static_cast<u32>(buf->size());
  c.inflight_bufs.push_back(std::move(buf));
  ++c.writes_inflight;
  u64 off = c.file_off;
  c.file_off += len;
  sri_write(c.fd, off, data, len, /*dsync=*/false, &Shard::checkpoint_write_done, this);
}

void Shard::checkpoint_write_done(void* ctx, int result) {
  auto* s = static_cast<Shard*>(ctx);
  if (result < 0) {
    errno = -result;
    storage_fatal("checkpoint write failed: " + s->ckpt_.path);
  }
  --s->ckpt_.writes_inflight;
}

void Shard::checkpoint_sync_done(void* ctx, int result) {
  auto* s = static_cast<Shard*>(ctx);
  Checkpoint& c = s->ckpt_;
  if (result < 0) {
    errno = -result;
    storage_fatal("checkpoint fdatasync failed: " + c.path);
  }
  ::close(c.fd);
  c.fd = -1;
  CheckpointResult res{c.start_lsn, c.objects, c.bytes};
  CheckpointDone done = std::move(c.done);
  c = Checkpoint{};
  done(res);
}

void Shard::checkpoint_step() {
  Checkpoint& c = ckpt_;
  if (c.syncing) return;
  if (!c.walk_done) {
    for (u32 n = 0; n < kCheckpointSlice && c.cursor < c.ids.size(); ++n, ++c.cursor) {
      // Objects deleted since begin are absent; their DELETE is at or after
      // start_lsn and replay handles it. Objects created since begin are
      // covered by CREATE records at or after start_lsn.
      const ObjectHeader* o = map_.find(c.ids[c.cursor]);
      if (o == nullptr || o->deleted()) continue;
      u32 size = record_size(o->length);
      std::size_t at = c.chunk.size();
      c.chunk.resize(at + size);
      encode_record(c.chunk.data() + at, RecType::IMAGE, o->last_lsn, o->type_id, o->id,
                    o->version, o->schema_ver, o->payload(), o->length);
      c.max_lsn = std::max(c.max_lsn, o->last_lsn);
      ++c.objects;
      c.bytes += size;
      if (c.chunk.size() >= kCheckpointChunk) checkpoint_write_chunk();
    }
    if (c.cursor == c.ids.size()) {
      checkpoint_write_chunk();
      c.walk_done = true;
      std::vector<ObjectID>().swap(c.ids);
    }
    return;
  }
  // Spec C5: the part may only hold state whose WAL record is durable.
  if (c.writes_inflight == 0 && durable(c.max_lsn)) {
    c.inflight_bufs.clear();
    c.syncing = true;
    sri_fdatasync(c.fd, &Shard::checkpoint_sync_done, this);
  }
}

// ---- Stats ---------------------------------------------------------------------------

ShardStats Shard::stats() const {
  ShardStats s = stats_;
  s.objects = 0;
  map_.for_each([&](const ObjectHeader* o) { s.objects += !o->deleted(); });
  s.local_map_bytes = map_.memory_bytes();
  s.heap_live_bytes = alloc_.stats().live_bytes;
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

void OpCtx::set_result(const void* data, u32 len) {
  if (len > op_->max_result) {
    result_overflow_ = true;
    return;
  }
  std::memcpy(result_buf_, data, len);
  result_len_ = len;
}

// ---- Public API -------------------------------------------------------------------------

void execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts, Completion done,
             void* ctx) {
  tl_shard->execute(target, op, args, opts, done, ctx);
}

void create(u32 type_id, ByteView init_args, const ExecOptions& opts, Completion done, void* ctx) {
  tl_shard->create(type_id, init_args, opts, done, ctx);
}

void remove(ObjectID target, const ExecOptions& opts, Completion done, void* ctx) {
  tl_shard->remove(target, opts, done, ctx);
}

void index_lookup(u32 index_id, ByteView key, u32 max_results, IndexCompletion done, void* ctx) {
  tl_shard->index_lookup(index_id, key, max_results, done, ctx);
}

u16 owner_of(const ObjectID& id) { return tl_shard->owner_of(id); }

}  // namespace snc
