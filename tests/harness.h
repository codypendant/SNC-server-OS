// Test helpers: run ops on shard threads from the test's main thread.
#pragma once

#include <functional>
#include <latch>
#include <memory>
#include <vector>

#include "check.h"
#include "snc/snc.h"

namespace snc::test {

// Issues `total` ops from one shard with at most `window` outstanding,
// without recursing from completions (local completions run inline).
struct Window {
  Engine* eng;
  u16 shard;
  u64 next = 0, total = 0, outstanding = 0, window = 64;
  std::function<void(u64 i, Completion, void* ctx)> issue;
  std::function<void(u64 i, const ExecResult&)> check;
  std::latch* done;
};

struct WindowOp {
  Window* w;
  u64 i;
};

inline void pump(Window* w);

inline void window_done(void* ctx, const ExecResult& r) {
  auto* box = static_cast<WindowOp*>(ctx);
  Window* w = box->w;
  w->check(box->i, r);
  delete box;
  if (--w->outstanding == 0) {
    if (w->next == w->total) {
      w->done->count_down();
    } else {
      w->eng->runtime().submit(w->shard, [w] { pump(w); });
    }
  }
}

inline void pump(Window* w) {
  u64 end = std::min(w->total, w->next + w->window);
  u64 start = w->next;
  w->next = end;
  w->outstanding += end - start;
  for (u64 i = start; i < end; ++i) w->issue(i, window_done, new WindowOp{w, i});
}

// Runs `per_shard` ops on every shard and waits for all of them.
inline void run_all(Engine& eng, u64 per_shard,
                    const std::function<void(u16 shard, u64 i, Completion, void*)>& issue,
                    const std::function<void(u16 shard, u64 i, const ExecResult&)>& check) {
  u16 n = eng.shard_count();
  std::latch done(n);
  std::vector<Window> ws(n);
  for (u16 s = 0; s < n; ++s) {
    Window& w = ws[s];
    w.eng = &eng;
    w.shard = s;
    w.total = per_shard;
    w.done = &done;
    w.issue = [&issue, s](u64 i, Completion c, void* ctx) { issue(s, i, c, ctx); };
    w.check = [&check, s](u64 i, const ExecResult& r) { check(s, i, r); };
    if (per_shard == 0) {
      done.count_down();
      continue;
    }
    eng.runtime().submit(s, [&w] { pump(&w); });
  }
  done.wait();
}

// Result of one synchronous op, with its result bytes copied out.
struct OneResult {
  ExecResult r;
  std::vector<u8> bytes;
  template <typename T>
  T as() const {
    T t{};
    CHECK(bytes.size() == sizeof(T));
    std::memcpy(&t, bytes.data(), sizeof(T));
    return t;
  }
};

inline OneResult one(Engine& eng, u16 shard, const std::function<void(Completion, void*)>& issue) {
  OneResult out;
  std::latch done(1);
  struct Ctx {
    OneResult* out;
    std::latch* done;
  } ctx{&out, &done};
  eng.runtime().submit(shard, [&] {
    issue(
        [](void* c, const ExecResult& r) {
          auto* x = static_cast<Ctx*>(c);
          x->out->r = r;
          x->out->bytes.assign(r.result.data, r.result.data + r.result.len);
          x->out->r.result = ByteView{};
          x->done->count_down();
        },
        &ctx);
  });
  done.wait();
  return out;
}

inline std::vector<ObjectID> lookup(Engine& eng, u16 shard, u32 index_id, const std::string& key,
                                    Status* status = nullptr) {
  std::vector<ObjectID> out;
  Status st = Status::OK;
  std::latch done(1);
  struct Ctx {
    std::vector<ObjectID>* out;
    Status* st;
    std::latch* done;
  } ctx{&out, &st, &done};
  eng.runtime().submit(shard, [&] {
    index_lookup(
        index_id, ByteView(key.data(), static_cast<u32>(key.size())), 1000,
        [](void* c, Status s, const ObjectID* ids, u32 n) {
          auto* x = static_cast<Ctx*>(c);
          x->out->assign(ids, ids + n);
          *x->st = s;
          x->done->count_down();
        },
        &ctx);
  });
  done.wait();
  if (status) *status = st;
  return out;
}

}  // namespace snc::test
