// QA1 P8: in-process cross-shard reordering. Usage: order_exp (in-memory engine, 2 shards)
#include <atomic>
#include <cstdio>
#include <cstring>
#include <latch>

#include "harness.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

struct Res { std::atomic<int> order{0}; int seq_a = -1, seq_b = -1; ExecResult a, b; std::latch* l; };
static Res g;
static u16 own(Engine& eng, ObjectID x) { u16 o = 0; std::latch l(1); eng.data_plane().submit(0, [&] { o = owner_of(x); l.count_down(); }); l.wait(); return o; }
static void done_a(void*, const ExecResult& r) { g.a = r; g.seq_a = g.order++; g.l->count_down(); }
static void done_b(void*, const ExecResult& r) { g.b = r; g.seq_b = g.order++; g.l->count_down(); }

int main() {
  register_type();
  Engine::Config c;
  c.runtime.n_shards = 2;
  c.runtime.ring_slots = 64;
  c.expected_objects = 1000;
  Engine eng(c);
  eng.start();
  int reorder_del = 0, reorder_ver = 0, trials = 20;
  for (int t = 0; t < trials; ++t) {
    // An account owned by shard 1, balance 5.
    ObjectID X;
    for (;;) {
      CreateArgs a{};
      a.balance_cents = 5;
      std::string e = "x" + std::to_string(t) + "_" + std::to_string(rand()) + "@x";
      a.email_len = (u8)e.size();
      std::memcpy(a.email, e.data(), e.size());
      OneResult r = one(eng, 1, [&](Completion cc, void* ctx) {
        create(kTypeId, ByteView{&a, sizeof(a)}, ExecOptions{}, cc, ctx);
      });
      X = r.r.id;
      if (own(eng, X) == 1) break;
    }
    // Case 1: from shard 0, withdraw 5 (batchable EXEC, 16-byte args) then delete (non-batchable).
    {
      std::latch l(2);
      g.l = &l;
      g.order = 0;
      static AmountArgs w{5, 1};
      eng.data_plane().submit(0, [X] {
        execute(X, kOpWithdraw, ByteView{&w, sizeof(w)}, ExecOptions{}, done_a, nullptr);
        remove(X, ExecOptions{}, done_b, nullptr);
      });
      l.wait();
      bool bad = g.b.status != Status::OK;
      reorder_del += bad;
      if (t == 0 || bad)
        std::printf("trial %d case1: withdraw(issued 1st)=%s answered#%d, delete(issued 2nd)=%s answered#%d\n", t,
                    status_name(g.a.status), g.seq_a, status_name(g.b.status), g.seq_b);
    }
  }
  // Case 2: deposit (batchable) then deposit with expected_version = version after the first.
  for (int t = 0; t < trials; ++t) {
    ObjectID X;
    u64 v0 = 0;
    for (;;) {
      CreateArgs a{};
      a.balance_cents = 5;
      std::string e = "v" + std::to_string(t) + "_" + std::to_string(rand()) + "@x";
      a.email_len = (u8)e.size();
      std::memcpy(a.email, e.data(), e.size());
      OneResult r = one(eng, 1, [&](Completion cc, void* ctx) {
        create(kTypeId, ByteView{&a, sizeof(a)}, ExecOptions{}, cc, ctx);
      });
      X = r.r.id;
      v0 = r.r.version;
      if (own(eng, X) == 1) break;
    }
    std::latch l(2);
    g.l = &l;
    g.order = 0;
    static AmountArgs d{1, 1};
    eng.data_plane().submit(0, [X, v0] {
      execute(X, kOpDeposit, ByteView{&d, sizeof(d)}, ExecOptions{}, done_a, nullptr);
      ExecOptions o;
      o.expected_version = v0 + 1;
      execute(X, kOpDeposit, ByteView{&d, sizeof(d)}, o, done_b, nullptr);
    });
    l.wait();
    bool bad = g.b.status != Status::OK;
    reorder_ver += bad;
    if (t == 0 || bad)
      std::printf("trial %d case2: deposit(1st)=%s ver=%llu answered#%d, deposit@v%llu(2nd)=%s answered#%d\n", t,
                  status_name(g.a.status), (unsigned long long)g.a.version, g.seq_a, (unsigned long long)(v0 + 1),
                  status_name(g.b.status), g.seq_b);
  }
  std::printf("case1 delete overtook withdraw: %d/%d; case2 versioned overtook plain: %d/%d\n", reorder_del,
              trials, reorder_ver, trials);
  eng.stop();
  return 0;
}
