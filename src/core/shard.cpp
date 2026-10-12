// Shard: owns a local object map, a heap, a mint lease and its share of the
// slot map, and executes ops against its objects in place (spec sections
// 3 to 9). Only this shard's worker thread ever touches a Shard.
#include <xxhash.h>

#include <cstdio>
#include <cstdlib>
#include <latch>

#include "snc/snc.h"

namespace snc {

namespace {
constexpr u8 kMsgRespDurable = 1u << 4;  // RESP flag: resulting version is durable

[[noreturn]] void contract_violation(const char* what, const OpDescriptor* op) {
  std::fprintf(stderr, "snc: handler contract violated: %s (op %s)\n", what, op ? op->name : "?");
  std::abort();
}
}  // namespace

class Shard {
 public:
  Shard(u16 id, u16 n_shards, u64 lease, u64 expected_objects)
      : id_(id),
        slots_(SlotMap::make_default(n_shards)),
        map_(expected_objects / (n_shards ? n_shards : 1) + 1024),
        minter_(lease),
        alloc_(sri_allocator()),
        result_scratch_(kMaxResult) {}

  ~Shard() {
    // Objects live in the shard heap, which the backend destroys with the
    // worker; nothing to free one by one here.
  }

  u16 owner_of(const ObjectID& id) const { return slots_.owner_of(id); }

  void execute(ObjectID target, OpID op, ByteView args, const ExecOptions& opts, Completion done,
               void* ctx) {
    u16 owner = owner_of(target);
    if (owner == id_) {
      ++stats_.ops_local;
      ExecResult r;
      run(MsgKind::EXEC_REQ, target, op, args, opts, r);
      done(ctx, r);
      return;
    }
    ++stats_.ops_remote;
    send_request(MsgKind::EXEC_REQ, owner, target, op, args, opts, done, ctx);
  }

  void create(u32 type_id, ByteView args, const ExecOptions& opts, Completion done, void* ctx) {
    ObjectID id = minter_.next();
    u16 owner = owner_of(id);
    if (owner == id_) {
      ++stats_.ops_local;
      ExecResult r;
      run(MsgKind::CREATE_REQ, id, type_id, args, opts, r);
      done(ctx, r);
      return;
    }
    ++stats_.ops_remote;
    send_request(MsgKind::CREATE_REQ, owner, id, type_id, args, opts, done, ctx);
  }

  void remove(ObjectID target, const ExecOptions& opts, Completion done, void* ctx) {
    u16 owner = owner_of(target);
    if (owner == id_) {
      ++stats_.ops_local;
      ExecResult r;
      run(MsgKind::DELETE_REQ, target, 0, ByteView{}, opts, r);
      done(ctx, r);
      return;
    }
    ++stats_.ops_remote;
    send_request(MsgKind::DELETE_REQ, owner, target, 0, ByteView{}, opts, done, ctx);
  }

  void on_message(const Msg& m) {
    switch (m.kind) {
      case MsgKind::EXEC_REQ:
      case MsgKind::CREATE_REQ:
      case MsgKind::DELETE_REQ:
        serve(m);
        break;
      case MsgKind::RESP:
        complete(m);
        break;
      case MsgKind::BUF_RETURN:
        alloc_.free(m.ext.ptr);
        break;
      default:
        std::fprintf(stderr, "snc: shard %u got unexpected message kind %u\n", id_,
                     static_cast<unsigned>(m.kind));
        std::abort();
    }
  }

  ShardStats stats() const {
    ShardStats s = stats_;
    s.objects = map_.size();
    s.local_map_bytes = map_.memory_bytes();
    s.heap_live_bytes = alloc_.stats().live_bytes;
    return s;
  }

  template <typename F>
  void for_each_object(F&& fn) const {
    map_.for_each([&](const ObjectHeader* h) {
      if (!h->deleted()) fn(*h);
    });
  }

 private:
  struct Pending {
    Completion done = nullptr;
    void* ctx = nullptr;
    u32 gen = 0;
  };

  // ---- Owner-side execution (spec section 6, "Execution sequence") ---------

  void run(MsgKind kind, ObjectID target, u32 op_or_type, ByteView args, const ExecOptions& opts,
           ExecResult& out) {
    out = ExecResult{};
    out.id = target;
    switch (kind) {
      case MsgKind::EXEC_REQ: run_exec(target, op_or_type, args, opts, out); break;
      case MsgKind::CREATE_REQ: run_create(target, op_or_type, args, out); break;
      case MsgKind::DELETE_REQ: run_delete(target, opts, out); break;
      default: std::abort();
    }
    // No WAL yet (handoff step 10): every applied state counts as durable.
    out.durable = true;
  }

  void prepare_ctx(OpCtx& c, ObjectHeader* obj, const TypeDescriptor* t, const OpDescriptor* op) {
    c.obj_ = obj;
    c.type_ = t;
    c.op_ = op;
    c.reserved_len_ = obj->capacity;
    c.app_code_ = 0;
    c.result_buf_ = result_scratch_.data();
    c.result_len_ = 0;
    c.result_overflow_ = false;
  }

  Status validate_checked(OpCtx& c, const OpDescriptor* op, ByteView args) {
#ifndef NDEBUG
    const ObjectHeader* h = c.obj_;
    u64 before = XXH3_64bits(h, sizeof(ObjectHeader) + h->length);
    Status s = op->validate(c, *h, args);
    if (XXH3_64bits(c.obj_, sizeof(ObjectHeader) + c.obj_->length) != before || c.obj_ != h) {
      contract_violation("validate modified the object", op);
    }
    return s;
#else
    return op->validate(c, *c.obj_, args);
#endif
  }

  void finish_result(OpCtx& c, const OpDescriptor* op, ExecResult& out) {
    if (c.result_overflow_) contract_violation("result larger than max_result", op);
    out.result = ByteView(c.result_buf_, c.result_len_);
  }

  void run_exec(ObjectID target, OpID op_id, ByteView args, const ExecOptions& opts,
                ExecResult& out) {
    ObjectHeader* obj = map_.find(target);
    if (obj == nullptr || obj->deleted()) {
      out.status = Status::E_NOT_FOUND;
      return;
    }
    const OpDescriptor* op = registry().op(op_id);
    if (op == nullptr || (op->kind != OpKind::READ && op->kind != OpKind::MUTATE)) {
      out.status = Status::E_UNKNOWN_OP;
      return;
    }
    if (op->type_id != obj->type_id) {
      out.status = Status::E_TYPE_MISMATCH;
      return;
    }
    if (args.len > op->max_args) {
      out.status = Status::E_BAD_ARGS;
      return;
    }
    out.version = obj->version;
    if (opts.expected_version != 0 && opts.expected_version != obj->version) {
      out.status = Status::E_VERSION_CONFLICT;
      return;
    }
    OpCtx c;
    prepare_ctx(c, obj, registry().type(obj->type_id), op);
    Status s = validate_checked(c, op, args);
    if (s != Status::OK) {
      out.status = s;
      out.app_code = c.app_code_;
      return;
    }
    if (op->apply) op->apply(c, args);
    if (op->kind == OpKind::MUTATE) {
      ++c.obj_->version;
      // Step 9 (index) and step 10 (WAL append, last_lsn) hook in here.
    }
    out.version = c.obj_->version;
    finish_result(c, op, out);
  }

  void run_create(ObjectID id, u32 type_id, ByteView args, ExecResult& out) {
    const TypeDescriptor* t = registry().type(type_id);
    if (t == nullptr) {
      out.status = Status::E_TYPE_MISMATCH;
      return;
    }
    const OpDescriptor* op = registry().op(t->create_op);
    if (args.len > op->max_args) {
      out.status = Status::E_BAD_ARGS;
      return;
    }
    if (map_.find(id) != nullptr) {
      std::fprintf(stderr, "snc: ObjectID minted twice: %s\n", id.to_string().c_str());
      std::abort();
    }
    std::size_t bytes = sizeof(ObjectHeader) + t->initial_size;
    auto* obj = static_cast<ObjectHeader*>(alloc_.alloc(bytes, kObjectAlign));
    if (obj == nullptr) {
      out.status = Status::E_NO_MEMORY;
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
      out.status = s;
      out.app_code = c.app_code_;
      return;
    }
    if (op->apply) op->apply(c, args);
    c.obj_->version = 1;
    ++stats_.creates;
    out.version = 1;
    finish_result(c, op, out);
  }

  void run_delete(ObjectID target, const ExecOptions& opts, ExecResult& out) {
    ObjectHeader* obj = map_.find(target);
    if (obj == nullptr || obj->deleted()) {
      out.status = Status::E_NOT_FOUND;
      return;
    }
    out.version = obj->version;
    if (opts.expected_version != 0 && opts.expected_version != obj->version) {
      out.status = Status::E_VERSION_CONFLICT;
      return;
    }
    const TypeDescriptor* t = registry().type(obj->type_id);
    if (t->delete_op != 0) {
      const OpDescriptor* op = registry().op(t->delete_op);
      OpCtx c;
      prepare_ctx(c, obj, t, op);
      Status s = validate_checked(c, op, ByteView{});
      if (s != Status::OK) {
        out.status = s;
        out.app_code = c.app_code_;
        return;
      }
      if (op->apply) op->apply(c, ByteView{});
      obj = c.obj_;
    }
    // Without a WAL the delete is durable at once, so no tombstone is kept
    // (spec section 8: tombstones live only until the DELETE record is durable).
    out.version = obj->version + 1;
    map_.erase(target);
    alloc_.free(obj);
    ++stats_.deletes;
  }

  // ---- Cross-shard dispatch (spec section 7) -------------------------------

  void send_request(MsgKind kind, u16 dst, ObjectID target, u32 op_or_type, ByteView args,
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
    std::memset(static_cast<void*>(&m), 0, 48);
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

  void fail_pending(u32 slot, ObjectID target, Status s) {
    Pending p = pending_[slot];
    free_pending_.push_back(slot);
    ExecResult r;
    r.status = s;
    r.id = target;
    p.done(p.ctx, r);
  }

  void serve(const Msg& m) {
    ++stats_.ops_served;
    ExecOptions opts;
    opts.expected_version = m.expected_version;
    opts.allow_undurable = m.flags & kMsgAllowUndurable;
    opts.durability = (m.flags & kMsgDurApplied) ? Durability::APPLIED : Durability::DURABLE;
    ExecResult r;
    run(m.kind, m.target, m.op_id, ByteView(m.data(), m.len), opts, r);

    Msg resp;
    std::memset(static_cast<void*>(&resp), 0, 48);
    resp.kind = MsgKind::RESP;
    resp.src_shard = id_;
    resp.req_id = m.req_id;
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
        resp.flags = (resp.flags & ~kMsgArgsExternal) | kMsgArgsInline;
        resp.len = 0;
        resp.status = static_cast<u16>(Status::E_NO_MEMORY);
      } else {
        std::memcpy(resp.ext.ptr, r.result.data, r.result.len);
      }
    }
    sri_send(m.src_shard, resp, /*must_deliver=*/true);

    if (m.flags & kMsgArgsExternal) return_buffer(m.src_shard, m.ext.ptr);
  }

  void complete(const Msg& m) {
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

  void return_buffer(u16 owner, u8* ptr) {
    Msg b;
    std::memset(static_cast<void*>(&b), 0, 48);
    b.kind = MsgKind::BUF_RETURN;
    b.src_shard = id_;
    b.flags = kMsgArgsExternal;
    b.ext.ptr = ptr;
    sri_send(owner, b, /*must_deliver=*/true);
  }

  friend class OpCtx;
  friend Shard* current_shard();

  u16 id_;
  SlotMap slots_;
  LocalMap map_;
  Minter minter_;
  ShardAllocator& alloc_;
  ShardStats stats_;
  std::vector<Pending> pending_;
  std::vector<u32> free_pending_;
  std::vector<u8> result_scratch_;

 public:
  LocalMap& map() { return map_; }
  ShardAllocator& alloc() { return alloc_; }
};

namespace {
thread_local Shard* tl_shard = nullptr;
}

// ---- OpCtx ------------------------------------------------------------------

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
    auto* moved = static_cast<ObjectHeader*>(
        tl_shard->alloc().resize(obj_, sizeof(ObjectHeader) + new_len, kObjectAlign));
    if (moved == nullptr) contract_violation("reserved memory unavailable at apply", op_);
    moved->capacity = static_cast<u32>(tl_shard->alloc().usable(moved) - sizeof(ObjectHeader));
    tl_shard->map().upsert(moved);
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

// ---- Public API ---------------------------------------------------------------

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

u16 owner_of(const ObjectID& id) { return tl_shard->owner_of(id); }

// ---- Engine -------------------------------------------------------------------

namespace {
struct EngineHooks {
  Engine::Config* cfg;
  std::vector<Shard*>* shards;
  std::vector<ShardStats>* final_stats;
};

void hook_init(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  u16 id = sri_shard_id();
  // Mint leases are in-memory until the superblock exists (handoff step 10):
  // lease = shard + 1 for this process. Spec C12 is not met yet.
  tl_shard = new Shard(id, sri_shard_count(), u64{id} + 1, h->cfg->expected_objects);
  (*h->shards)[id] = tl_shard;
}

void hook_message(void*, const Msg& m) { tl_shard->on_message(m); }

bool hook_poll(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  return h->cfg->app_poll ? h->cfg->app_poll(h->cfg->app_user, sri_shard_id()) : false;
}

void hook_fini(void* user) {
  auto* h = static_cast<EngineHooks*>(user);
  u16 id = sri_shard_id();
  (*h->final_stats)[id] = tl_shard->stats();
  (*h->shards)[id] = nullptr;
  delete tl_shard;
  tl_shard = nullptr;
}
}  // namespace

struct EngineState {
  EngineHooks hooks;
  std::vector<ShardStats> final_stats;
};

static std::unique_ptr<EngineState> g_state;

Engine::Engine(const Config& cfg) : cfg_(cfg) {
  shards_.assign(cfg.runtime.n_shards, nullptr);
  g_state = std::make_unique<EngineState>();
  g_state->final_stats.assign(cfg.runtime.n_shards, ShardStats{});
  g_state->hooks = EngineHooks{&cfg_, &shards_, &g_state->final_stats};
  ShardHooks h;
  h.init = hook_init;
  h.on_message = hook_message;
  h.poll = hook_poll;
  h.fini = hook_fini;
  h.user = &g_state->hooks;
  rt_ = std::make_unique<NativeRuntime>(cfg.runtime, h);
}

Engine::~Engine() {
  stop();
  rt_.reset();
  g_state.reset();
}

void Engine::start() {
  rt_->start();
  running_ = true;
}

void Engine::stop() {
  rt_->stop();
  running_ = false;
}

ShardStats Engine::stats(u16 shard) const {
  if (!running_) return g_state->final_stats.at(shard);
  // Read on the owning thread: shard state is single-writer.
  ShardStats out;
  std::latch done(1);
  rt_->submit(shard, [&] {
    out = tl_shard->stats();
    done.count_down();
  });
  done.wait();
  return out;
}

void Engine::for_each_object(u16 shard, const std::function<void(const ObjectHeader&)>& fn) {
  std::latch done(1);
  rt_->submit(shard, [&] {
    tl_shard->for_each_object(fn);
    done.count_down();
  });
  done.wait();
}

}  // namespace snc
