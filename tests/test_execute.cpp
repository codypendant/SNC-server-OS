// End-to-end execute(): creates, local and cross-shard mutations, reads,
// rejections, conditional ops, deletes, and the large-argument path.
#include <atomic>
#include <functional>
#include <latch>
#include <mutex>
#include <vector>

#include "check.h"
#include "snc/snc.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;

namespace {

constexpr u16 kShards = 4;

// Issues `total` ops from one shard, at most `window` outstanding, without
// recursing from completions (local completions run inline).
struct Window {
  Engine* eng;
  u16 shard;
  u64 next = 0, total = 0, outstanding = 0, window = 64;
  std::function<void(u64 i, Completion, void* ctx)> issue;
  std::function<void(u64 i, const ExecResult&)> check;
  std::latch* done;
};

struct OpCtxBox {
  Window* w;
  u64 i;
};

void pump(Window* w);

void on_done(void* ctx, const ExecResult& r) {
  auto* box = static_cast<OpCtxBox*>(ctx);
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

void pump(Window* w) {
  u64 end = std::min(w->total, w->next + w->window);
  u64 start = w->next;
  w->next = end;
  w->outstanding += end - start;
  for (u64 i = start; i < end; ++i) w->issue(i, on_done, new OpCtxBox{w, i});
}

// Runs one windowed batch per shard and waits for all of them.
void run_all(Engine& eng, u64 per_shard,
             const std::function<void(u16 shard, u64 i, Completion, void*)>& issue,
             const std::function<void(u16 shard, u64 i, const ExecResult&)>& check) {
  std::latch done(kShards);
  std::vector<Window> ws(kShards);
  for (u16 s = 0; s < kShards; ++s) {
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

AmountArgs amount(i64 cents) { return AmountArgs{cents, 1}; }

// One synchronous op on a given shard, for single-shot checks.
ExecResult one(Engine& eng, u16 shard, const std::function<void(Completion, void*)>& issue) {
  ExecResult out;
  std::vector<u8> bytes;
  std::latch done(1);
  struct Ctx {
    ExecResult* out;
    std::vector<u8>* bytes;
    std::latch* done;
  } ctx{&out, &bytes, &done};
  eng.runtime().submit(shard, [&] {
    issue(
        [](void* c, const ExecResult& r) {
          auto* x = static_cast<Ctx*>(c);
          *x->out = r;
          x->bytes->assign(r.result.data, r.result.data + r.result.len);
          x->done->count_down();
        },
        &ctx);
  });
  done.wait();
  static thread_local std::vector<u8> keep;
  keep = bytes;
  out.result = ByteView(keep.data(), static_cast<u32>(keep.size()));
  return out;
}

}  // namespace

int main() {
  register_type();
  Engine::Config cfg;
  cfg.runtime.n_shards = kShards;
  cfg.runtime.ring_slots = 64;
  cfg.expected_objects = 40000;
  Engine eng(cfg);
  eng.start();

  // 1. Create 10,000 accounts from each shard (create args are 184 bytes:
  //    the external-buffer path for remote creates).
  const u64 per_shard = 10000;
  std::mutex mu;
  std::vector<ObjectID> ids;
  run_all(
      eng, per_shard,
      [](u16 s, u64 i, Completion c, void* ctx) {
        CreateArgs a{};
        a.balance_cents = 100;
        a.created_ns = 1;
        int n = std::snprintf(a.email, sizeof(a.email), "u%u-%llu@example.com", s,
                              (unsigned long long)i);
        a.email_len = static_cast<u8>(n);
        a.name_len = 4;
        std::memcpy(a.name, "Test", 4);
        create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [&](u16, u64, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        CHECK_EQ(r.version, 1u);
        CHECK(r.durable);
        std::lock_guard<std::mutex> g(mu);
        ids.push_back(r.id);
      });
  CHECK_EQ(ids.size(), per_shard * kShards);

  u64 owned = 0;
  for (u16 s = 0; s < kShards; ++s) {
    ShardStats st = eng.stats(s);
    owned += st.objects;
    // Hash placement spreads objects; each shard owns roughly a quarter.
    CHECK(st.objects > ids.size() / kShards * 9 / 10);
  }
  CHECK_EQ(owned, ids.size());

  // Each object is on exactly its owner shard, with a valid payload.
  u64 seen = 0;
  for (u16 s = 0; s < kShards; ++s) {
    eng.for_each_object(s, [&](const ObjectHeader& h) {
      ++seen;
      CHECK_EQ(h.type_id, kTypeId);
      CHECK_EQ(h.length, sizeof(AccountV1));
      CHECK(registry().type(kTypeId)->check(h.payload(), h.length));
    });
  }
  CHECK_EQ(seen, ids.size());

  // 2. Every shard deposits 1 cent into every account: 75% of ops cross shards.
  run_all(
      eng, ids.size(),
      [&](u16, u64 i, Completion c, void* ctx) {
        AmountArgs a = amount(1);
        execute(ids[i], kOpDeposit, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [](u16, u64, const ExecResult& r) { CHECK_EQ(r.status, Status::OK); });

  // Balances and versions reflect exactly kShards deposits each.
  run_all(
      eng, ids.size() / kShards,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        execute(ids[s * (ids.size() / kShards) + i], kOpGetBalance, {}, {}, c, ctx);
      },
      [](u16, u64, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        CHECK_EQ(r.version, 1u + kShards);
        BalanceResult b;
        CHECK_EQ(r.result.len, sizeof(b));
        std::memcpy(&b, r.result.data, sizeof(b));
        CHECK_EQ(b.balance_cents, 100 + kShards);
        CHECK_EQ(b.op_count, (u64)kShards);
      });

  u64 local = 0, remote = 0, served = 0;
  for (u16 s = 0; s < kShards; ++s) {
    ShardStats st = eng.stats(s);
    local += st.ops_local;
    remote += st.ops_remote;
    served += st.ops_served;
  }
  CHECK_EQ(remote, served);
  CHECK(remote > local * 2);  // roughly 3:1 with 4 shards

  // 3. Single-op semantics, issued from a shard that does not own the target
  //    (remote path) and from the owner (local path).
  ObjectID x = ids[123];
  u16 owner = 0;
  {
    ExecResult r = one(eng, 0, [&](Completion c, void* ctx) { execute(x, kOpGet, {}, {}, c, ctx); });
    CHECK_EQ(r.status, Status::OK);
    CHECK_EQ(r.result.len, sizeof(AccountV1));  // 200 bytes: external result buffer if remote
    AccountV1 a;
    std::memcpy(&a, r.result.data, sizeof(a));
    CHECK_EQ(a.balance_cents, 100 + kShards);
  }
  for (u16 s = 0; s < kShards; ++s) {
    eng.for_each_object(s, [&](const ObjectHeader& h) {
      if (h.id == x) owner = s;
    });
  }
  for (u16 from : {owner, static_cast<u16>((owner + 1) % kShards)}) {
    // Rejection leaves state unchanged.
    AmountArgs big = amount(1000000);
    ExecResult r = one(eng, from, [&](Completion c, void* ctx) {
      execute(x, kOpWithdraw, ByteView(&big, sizeof(big)), {}, c, ctx);
    });
    CHECK_EQ(r.status, Status::E_APP);
    CHECK_EQ(r.app_code, kAppInsufficientFunds);
    u64 v = r.version;

    // Conditional op: stale version fails, current version succeeds.
    AmountArgs one_cent = amount(1);
    ExecOptions stale;
    stale.expected_version = v - 1;
    r = one(eng, from, [&](Completion c, void* ctx) {
      execute(x, kOpDeposit, ByteView(&one_cent, sizeof(one_cent)), stale, c, ctx);
    });
    CHECK_EQ(r.status, Status::E_VERSION_CONFLICT);
    ExecOptions current;
    current.expected_version = v;
    r = one(eng, from, [&](Completion c, void* ctx) {
      execute(x, kOpDeposit, ByteView(&one_cent, sizeof(one_cent)), current, c, ctx);
    });
    CHECK_EQ(r.status, Status::OK);
    CHECK_EQ(r.version, v + 1);

    // Bad args, unknown op, and an op of the wrong kind.
    r = one(eng, from, [&](Completion c, void* ctx) { execute(x, kOpDeposit, {}, {}, c, ctx); });
    CHECK_EQ(r.status, Status::E_BAD_ARGS);
    r = one(eng, from, [&](Completion c, void* ctx) { execute(x, 9999, {}, {}, c, ctx); });
    CHECK_EQ(r.status, Status::E_UNKNOWN_OP);
    r = one(eng, from, [&](Completion c, void* ctx) { execute(x, kOpCreate, {}, {}, c, ctx); });
    CHECK_EQ(r.status, Status::E_UNKNOWN_OP);
  }

  // 4. Delete: refused while the balance is non-zero, then allowed.
  {
    ExecResult r = one(eng, 0, [&](Completion c, void* ctx) { remove(x, {}, c, ctx); });
    CHECK_EQ(r.status, Status::E_APP);
    CHECK_EQ(r.app_code, kAppNotEmpty);
    r = one(eng, 0, [&](Completion c, void* ctx) { execute(x, kOpGetBalance, {}, {}, c, ctx); });
    BalanceResult b;
    std::memcpy(&b, r.result.data, sizeof(b));
    AmountArgs all = amount(b.balance_cents);
    r = one(eng, 0, [&](Completion c, void* ctx) {
      execute(x, kOpWithdraw, ByteView(&all, sizeof(all)), {}, c, ctx);
    });
    CHECK_EQ(r.status, Status::OK);
    u64 v = r.version;
    r = one(eng, 0, [&](Completion c, void* ctx) { remove(x, {}, c, ctx); });
    CHECK_EQ(r.status, Status::OK);
    CHECK_EQ(r.version, v + 1);
    // A deleted ID stays dead and never aliases another object.
    r = one(eng, 1, [&](Completion c, void* ctx) { execute(x, kOpGet, {}, {}, c, ctx); });
    CHECK_EQ(r.status, Status::E_NOT_FOUND);
    r = one(eng, 1, [&](Completion c, void* ctx) { remove(x, {}, c, ctx); });
    CHECK_EQ(r.status, Status::E_NOT_FOUND);
    ObjectID never = ObjectID::make(999, 1);
    r = one(eng, 2, [&](Completion c, void* ctx) { execute(never, kOpGet, {}, {}, c, ctx); });
    CHECK_EQ(r.status, Status::E_NOT_FOUND);
  }

  // 5. Bad create args are rejected and leave no object behind.
  {
    u64 before = 0;
    for (u16 s = 0; s < kShards; ++s) before += eng.stats(s).objects;
    ExecResult r = one(eng, 3, [&](Completion c, void* ctx) {
      u8 junk[3] = {1, 2, 3};
      create(kTypeId, ByteView(junk, 3), {}, c, ctx);
    });
    CHECK_EQ(r.status, Status::E_BAD_ARGS);
    u64 after = 0;
    for (u16 s = 0; s < kShards; ++s) after += eng.stats(s).objects;
    CHECK_EQ(after, before);
  }

  eng.stop();
  // Every external buffer was returned to and freed by its origin: only
  // live objects remain in the heaps.
  u64 objects = 0, heap = 0;
  for (u16 s = 0; s < kShards; ++s) {
    objects += eng.stats(s).objects;
    heap += eng.stats(s).heap_live_bytes;
  }
  CHECK_EQ(objects, ids.size() - 1);
  std::printf("test_execute: ok (%llu objects, %llu local ops, %llu remote ops, %.1f heap bytes/object)\n",
              (unsigned long long)objects, (unsigned long long)local, (unsigned long long)remote,
              double(heap) / double(objects));
  return 0;
}
