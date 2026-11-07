// S1 admission (docs/S1_ADMISSION.md; docs/CHECKPOINT_BACKPRESSURE_DESIGN.md
// section 6; work order docs/handoff/PR71_Harness_Review.txt section 12).
//
//   A1 follow-up backlog above kEmitBacklog: an owner-local durable READ is
//      answered; a new mutation, create or delete is refused; a READ
//      forwarded to another shard keeps the gate (refused_followup)
//   A2 waiter budget: refuses before admission (refused_waiter); a refused
//      mutation changes nothing; a mutation forwarded to the shard is refused
//      there; a READ that would wait is refused; a durable delete (two
//      waiters) is refused where an APPLIED one (one waiter) is admitted
//   A3 WAL buffer budget: refuses before the first mutation
//      (refused_wal_buffer); object, version, WAL, session and follow-ups
//      unchanged; a create is refused too
//   A4 sequenced retry: a sequenced mutation refused by a budget runs once
//      on retry with the same sequence number; a resend is replayed
//   A5 WAL sizing: a sequenced transfer between two accounts of one shard
//      (local peer + session + object) appends exactly its three records,
//      within the reservation (debug builds also check every admitted op)
//   A6 internal work drains while client admission is closed: follow-ups
//      are applied on a shard whose WAL budget refuses clients (its WAL held,
//      so each credit grows the buffer past the budget), and everything
//      settles once durability resumes; money is conserved
//   A7 read flood: shard 0's follow-up backlog above the gate drains while
//      owner-local reads flood both shards, and every read is answered
//   A8 --admission legacy: the gate as before S1 (local reads refused,
//      creates and deletes not gated, tiny budgets ignored with the WAL held)
//   A9 session expiry is the shard's own work: its deletes run while the
//      WAL budget refuses clients, and count as no refusal
//
// Every refusal is checked against exactly one refused_* cause in both
// ShardStats and the published gauges.
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/session.h"  // SessionV1: the session record admit() reserves
#include "harness.h"
#include "snc/gauges.h"
#include "snc/slot_map.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr i64 kStart = 1000;
constexpr u64 kGate = 1024;  // Shard::kEmitBacklog

void rm_rf(const std::string& d) { CHECK_EQ(std::system(("rm -rf " + d).c_str()), 0); }
std::string temp_dir() {
  std::string t = "/tmp/snc-s1-XXXXXX";
  std::vector<char> b(t.begin(), t.end());
  b.push_back(0);
  char* d = mkdtemp(b.data());
  CHECK(d != nullptr);
  return std::string(d);
}

Engine::Config config(u16 shards, const std::string& dir) {
  Engine::Config c;
  c.runtime.n_shards = shards;
  c.expected_objects = 1 << 16;
  c.data_dir = dir;
  c.init = !dir.empty();
  c.wal_segment_bytes = 4u << 20;
  return c;
}

CreateArgs account_args() {
  CreateArgs a{};
  a.balance_cents = kStart;
  a.created_ns = 1;
  a.email_len = 1;
  a.email[0] = 'e';
  a.name_len = 1;
  a.name[0] = 'n';
  return a;
}

// Creates accounts until each shard owns `per_shard` of them.
std::vector<std::vector<ObjectID>> accounts(Engine& eng, u32 per_shard) {
  const u16 n = eng.shard_count();
  const SlotMap map = SlotMap::make_default(n);
  std::vector<std::vector<ObjectID>> mine(n);
  for (u32 guard = 0; guard < 100000; ++guard) {
    bool full = true;
    for (u16 s = 0; s < n; ++s) full &= mine[s].size() >= per_shard;
    if (full) return mine;
    OneResult r = one(eng, static_cast<u16>(guard % n), [&](Completion cb, void* ctx) {
      CreateArgs a = account_args();
      create(kTypeId, ByteView(&a, sizeof(a)), {}, cb, ctx);
    });
    CHECK_EQ(r.r.status, Status::OK);
    mine[map.owner_of(r.r.id)].push_back(r.r.id);
  }
  CHECK(false);
  return mine;
}

// An empty account (balance 0, so it can be deleted) that shard s owns.
ObjectID empty_account(Engine& eng, u16 s) {
  const SlotMap map = SlotMap::make_default(eng.shard_count());
  for (u32 guard = 0; guard < 10000; ++guard) {
    OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
      CreateArgs a = account_args();
      a.balance_cents = 0;
      create(kTypeId, ByteView(&a, sizeof(a)), {}, cb, ctx);
    });
    CHECK_EQ(r.r.status, Status::OK);
    if (map.owner_of(r.r.id) == s) return r.r.id;
  }
  CHECK(false);
  return {};
}

GaugeSnapshot fresh(Engine& eng, u16 s) {
  const u64 after = sri_now_ns();
  for (int i = 0; i < 4000; ++i) {
    GaugeSnapshot g = eng.gauges(s);
    if (g.consistent && g[G_PUBLISHED_NS] > after) return g;
    ::usleep(500);
  }
  CHECK(false);
  return {};
}

// The refusal causes of one shard, from STATS and from its gauges.
struct Causes {
  u64 followup, ring, waiter, wal, pending, overloaded;
};
Causes causes(Engine& eng, u16 s) {
  ShardStats st = eng.stats(s);
  CHECK_EQ(st.overloaded,
           st.refused_followup + st.refused_ring + st.refused_waiter + st.refused_wal_buffer + st.refused_pending);
  GaugeSnapshot g = fresh(eng, s);
  CHECK_EQ(g[G_REFUSED_FOLLOWUP], st.refused_followup);
  CHECK_EQ(g[G_REFUSED_RING], st.refused_ring);
  CHECK_EQ(g[G_REFUSED_WAITER], st.refused_waiter);
  CHECK_EQ(g[G_REFUSED_WAL_BUFFER], st.refused_wal_buffer);
  CHECK_EQ(g[G_REFUSED_PENDING], st.refused_pending);
  return {st.refused_followup, st.refused_ring,    st.refused_waiter,
          st.refused_wal_buffer, st.refused_pending, st.overloaded};
}
// Exactly `n` new refusals since `before`, all of cause `which`.
enum Which { FOLLOWUP, RING, WAITER, WAL, PENDING };
void expect_causes(const Causes& before, const Causes& now, Which which, u64 n) {
  CHECK_EQ(now.followup - before.followup, which == FOLLOWUP ? n : 0u);
  CHECK_EQ(now.ring - before.ring, which == RING ? n : 0u);
  CHECK_EQ(now.waiter - before.waiter, which == WAITER ? n : 0u);
  CHECK_EQ(now.wal - before.wal, which == WAL ? n : 0u);
  CHECK_EQ(now.pending - before.pending, which == PENDING ? n : 0u);
  CHECK_EQ(now.overloaded - before.overloaded, n);
}

// Ops issued from a shard task without waiting: refusals and immediate
// answers arrive inline; the rest when durable.
struct Async {
  std::mutex mu;
  std::vector<ExecResult> results;  // in completion order
  std::atomic<u32> done{0};
  static void cb(void* p, const ExecResult& r) {
    auto* a = static_cast<Async*>(p);
    std::lock_guard<std::mutex> g(a->mu);
    ExecResult c = r;
    c.result = ByteView{};
    a->results.push_back(c);
    ++a->done;
  }
  u32 count(Status s) {
    std::lock_guard<std::mutex> g(mu);
    u32 n = 0;
    for (const ExecResult& r : results) n += r.status == s;
    return n;
  }
  bool wait(u32 n, int ms = 20000) {
    for (int i = 0; i < ms && done.load() < n; ++i) ::usleep(1000);
    return done.load() >= n;
  }
};

// Runs fn on shard s and waits for it to return (not for its ops).
void on_shard(Engine& eng, u16 s, const std::function<void()>& fn) {
  std::latch l(1);
  eng.runtime().submit(s, [&] {
    fn();
    l.count_down();
  });
  l.wait();
}

BalanceResult balance(Engine& eng, u16 s, const ObjectID& id, bool undurable = false) {
  OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.allow_undurable = undurable;
    execute(id, kOpGetBalance, ByteView{}, o, cb, ctx);
  });
  CHECK_EQ(r.r.status, Status::OK);
  return r.as<BalanceResult>();
}
u64 version_of(Engine& eng, u16 s, const ObjectID& id) {
  OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.allow_undurable = true;
    execute(id, kOpGetBalance, ByteView{}, o, cb, ctx);
  });
  CHECK_EQ(r.r.status, Status::OK);
  return r.r.version;
}

void deposit(const ObjectID& id, i64 amount, const ExecOptions& o, Async& a) {
  AmountArgs d{amount, 7};
  execute(id, kOpDeposit, ByteView(&d, sizeof(d)), o, &Async::cb, &a);
}

// The worst case admit() reserves for a deposit (shard.cpp run_exec).
u64 mutation_wal_need(bool sequenced, u32 type = kTypeId) {
  u64 peer = 0;
  for (const TypeDescriptor& t : registry().types()) peer = std::max<u64>(peer, record_size(t.max_size));
  return record_size(registry().type(type)->max_size) + peer + (sequenced ? record_size(sizeof(SessionV1)) : 0);
}
constexpr u32 kDepositRecord = 256;  // record_size(200): an account without entries

// ---- A1 -------------------------------------------------------------------------------

// A backlog above the gate: shard 0's transfers to shard 1 with follow-ups
// held (one in flight each).
void build_backlog(Engine& eng, const std::vector<ObjectID>& from, const std::vector<ObjectID>& to) {
  u64 queued = 0;
  for (u64 i = 0; eng.stats(0).emits_in_flight <= kGate; ++i) {
    CHECK(i < 100000);
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
      TransferArgs t{to[i % to.size()], 1, 7};
      execute(from[i % from.size()], kOpTransfer, ByteView(&t, sizeof(t)), {}, cb, ctx);
    });
    if (r.r.status == Status::OK) ++queued;
    else CHECK(r.r.status == Status::E_APP);  // an account's outbox full (kAppBusy)
  }
  CHECK(queued > 1024);
}

void test_followup_gate(bool legacy) {
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  c.hold_emits = true;
  c.admission_legacy = legacy;
  if (legacy) {
    c.waiter_budget_bytes = 1;  // would refuse almost everything under s1
    c.wal_buffer_budget_bytes = 1;
  }
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 40);
  const ObjectID spare = empty_account(eng, 0);
  build_backlog(eng, own[0], own[1]);
  CHECK(eng.stats(0).emits_in_flight > 1024);
  const Causes before = causes(eng, 0);

  // Owner-local durable reads.
  u32 read_ok = 0, read_refused = 0;
  for (u32 i = 0; i < 20; ++i) {
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
      execute(own[0][i], kOpGetBalance, ByteView{}, {}, cb, ctx);
    });
    if (r.r.status == Status::OK) ++read_ok;
    else if (r.r.status == Status::E_OVERLOADED) ++read_refused;
    else CHECK(false);
  }
  // A new mutation, a create and a delete on shard 0.
  OneResult dep = one(eng, 0, [&](Completion cb, void* ctx) {
    AmountArgs d{5, 7};
    execute(own[0][0], kOpDeposit, ByteView(&d, sizeof(d)), {}, cb, ctx);
  });
  OneResult cre = one(eng, 0, [&](Completion cb, void* ctx) {
    CreateArgs a = account_args();
    create(kTypeId, ByteView(&a, sizeof(a)), {}, cb, ctx);
  });
  OneResult del = one(eng, 0, [&](Completion cb, void* ctx) { remove(spare, {}, cb, ctx); });
  // A READ forwarded from shard 0 to an account shard 1 owns.
  OneResult fwd = one(eng, 0, [&](Completion cb, void* ctx) {
    execute(own[1][0], kOpGetBalance, ByteView{}, {}, cb, ctx);
  });
  CHECK_EQ(dep.r.status, Status::E_OVERLOADED);
  CHECK_EQ(fwd.r.status, Status::E_OVERLOADED);
  const Causes after = causes(eng, 0);
  if (!legacy) {
    CHECK_EQ(read_ok, 20u);
    CHECK_EQ(cre.r.status, Status::E_OVERLOADED);
    CHECK_EQ(del.r.status, Status::E_OVERLOADED);
    expect_causes(before, after, FOLLOWUP, 4);  // deposit, create, delete, forwarded read
  } else {
    // A8: as before S1.
    CHECK_EQ(read_refused, 20u);
    CHECK_EQ(cre.r.status, Status::OK);  // no gate on create before S1
    CHECK_EQ(del.r.status, Status::OK);  // nor on delete
    expect_causes(before, after, FOLLOWUP, 22);  // 20 reads, deposit, forwarded read
  }
  eng.release_emits();
  CHECK(eng.wait_emits());
  if (legacy) {
    // A8: with the WAL held, waiters and active bytes pile up past the tiny
    // budgets; legacy admission refuses none of these.
    const Causes held = causes(eng, 0);
    Engine::debug_hold_wal(0, true);
    Async as;
    on_shard(eng, 0, [&] {
      for (int i = 0; i < 8; ++i) deposit(own[0][i], 1, {}, as);
    });
    CHECK_EQ(as.done.load(), 0u);  // all admitted, all waiting
    CHECK(fresh(eng, 0)[G_WAITERS_NOW] >= 8u);
    expect_causes(held, causes(eng, 0), WAITER, 0);
    Engine::debug_hold_wal(0, false);
    CHECK(as.wait(8));
    CHECK_EQ(as.count(Status::OK), 8u);
  }
  eng.stop();
  rm_rf(dir);
  std::printf("A%s follow-up gate (%s): %u local reads answered, %u refused; deposit %s, create %s, delete %s, "
              "forwarded read %s\n",
              legacy ? "8" : "1", legacy ? "legacy" : "s1", read_ok, read_refused, status_name(dep.r.status),
              status_name(cre.r.status), status_name(del.r.status), status_name(fwd.r.status));
}

// ---- A2 -------------------------------------------------------------------------------

void test_waiter_budget() {
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  // Room for 4 deposit waiters (256 + 16 bytes each) plus 260 bytes: the
  // 5th deposit (272) is refused, a delete's one ERASE waiter (256) fits,
  // a durable delete's two (512) do not.
  c.waiter_budget_bytes = 4 * (256 + sizeof(BalanceResult)) + 260;
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 8);
  const ObjectID gone = empty_account(eng, 0);
  const ObjectID gone2 = empty_account(eng, 0);
  const ObjectID a = own[0][0];
  const BalanceResult b0 = balance(eng, 0, a);
  const u64 v0 = version_of(eng, 0, a);
  const u64 lsn0 = fresh(eng, 0)[G_WAL_NEXT_LSN];
  const Causes before = causes(eng, 0);
  const Causes before1 = causes(eng, 1);

  Engine::debug_hold_wal(0, true);
  Async as;
  on_shard(eng, 0, [&] {
    for (int i = 0; i < 6; ++i) deposit(a, 10, {}, as);
  });
  // Refusals answer at once; the 4 admitted wait for durability.
  CHECK_EQ(as.count(Status::E_OVERLOADED), 2u);
  CHECK_EQ(as.done.load(), 2u);
  Causes mid = causes(eng, 0);
  expect_causes(before, mid, WAITER, 2);
  GaugeSnapshot g = fresh(eng, 0);
  CHECK_EQ(g[G_WAITERS_NOW], 4u);
  CHECK_EQ(g[G_WAL_NEXT_LSN] - lsn0, 4u);  // the refused ones logged nothing
  // The object holds exactly the admitted deposits.
  CHECK_EQ(version_of(eng, 0, a), v0 + 4);
  CHECK_EQ(balance(eng, 0, a, /*undurable=*/true).balance_cents, b0.balance_cents + 40);
  // A READ of the undurable object would wait: refused; allow_undurable is answered.
  OneResult rd = one(eng, 0, [&](Completion cb, void* ctx) { execute(a, kOpGetBalance, ByteView{}, {}, cb, ctx); });
  CHECK_EQ(rd.r.status, Status::E_OVERLOADED);
  // A mutation forwarded from shard 1 is refused by shard 0, the owner.
  OneResult fw = one(eng, 1, [&](Completion cb, void* ctx) {
    AmountArgs d{10, 7};
    execute(a, kOpDeposit, ByteView(&d, sizeof(d)), {}, cb, ctx);
  });
  CHECK_EQ(fw.r.status, Status::E_OVERLOADED);
  // A delete needs two waiters (the tombstone's and the answer's): refused.
  OneResult del = one(eng, 0, [&](Completion cb, void* ctx) { remove(gone, {}, cb, ctx); });
  CHECK_EQ(del.r.status, Status::E_OVERLOADED);
  // An APPLIED delete needs only the tombstone's waiter: admitted.
  const Causes pre_del1 = causes(eng, 0);
  OneResult del1 = one(eng, 0, [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.durability = Durability::APPLIED;
    remove(gone2, o, cb, ctx);
  });
  CHECK_EQ(del1.r.status, Status::OK);
  expect_causes(pre_del1, causes(eng, 0), WAITER, 0);
  CHECK_EQ(fresh(eng, 0)[G_WAITERS_NOW], 5u);  // 4 deposits + the tombstone
  // A sequenced create (it runs here) waits too: refused, with no object or session.
  const u64 objects0 = eng.stats(0).objects;
  OneResult cr = one(eng, 0, [&](Completion cb, void* ctx) {
    ExecOptions so;
    so.client_id = 77;
    so.seq = 1;
    CreateArgs ca = account_args();
    create(kTypeId, ByteView(&ca, sizeof(ca)), so, cb, ctx);
  });
  CHECK_EQ(cr.r.status, Status::E_OVERLOADED);
  CHECK_EQ(eng.stats(0).objects, objects0);
  Causes after = causes(eng, 0);
  expect_causes(before, after, WAITER, 6);
  expect_causes(before1, causes(eng, 1), WAITER, 0);  // shard 1 refused nothing

  Engine::debug_hold_wal(0, false);
  CHECK(as.wait(6));
  CHECK_EQ(as.count(Status::OK), 4u);
  CHECK_EQ(balance(eng, 0, a).balance_cents, b0.balance_cents + 40);
  CHECK_EQ(version_of(eng, 0, a), v0 + 4);
  // Admission reopens once the waiters are released.
  OneResult again = one(eng, 0, [&](Completion cb, void* ctx) { remove(gone, {}, cb, ctx); });
  CHECK_EQ(again.r.status, Status::OK);
  // A4: the refused sequenced create runs once on retry (a new op, not a replay).
  OneResult cr2 = one(eng, 0, [&](Completion cb, void* ctx) {
    ExecOptions so;
    so.client_id = 77;
    so.seq = 1;
    CreateArgs ca = account_args();
    create(kTypeId, ByteView(&ca, sizeof(ca)), so, cb, ctx);
  });
  CHECK_EQ(cr2.r.status, Status::OK);
  CHECK(!cr2.r.replayed);
  eng.stop();
  rm_rf(dir);
  std::printf("A2 waiter budget: 4 admitted, 2 refused before any change; undurable read, forwarded deposit, "
              "durable delete and sequenced create refused (all refused_waiter); APPLIED delete admitted; "
              "A4 create retry ran once\n");
}

// ---- A3, A4 ---------------------------------------------------------------------------

void test_wal_budget() {
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  c.hold_emits = true;  // a refused transfer must queue no follow-up
  // Admits while active + worst case <= budget: active 0 (always), 256,
  // 512, 768; refuses at 1024.
  c.wal_buffer_budget_bytes = mutation_wal_need(false) + 1000;
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 8);
  const ObjectID a = own[0][0];
  const BalanceResult b0 = balance(eng, 0, a);
  const u64 v0 = version_of(eng, 0, a);
  const u64 lsn0 = fresh(eng, 0)[G_WAL_NEXT_LSN];
  const u64 objects0 = eng.stats(0).objects;
  const Causes before = causes(eng, 0);

  Engine::debug_hold_wal(0, true);
  Async as;
  on_shard(eng, 0, [&] {
    for (int i = 0; i < 4; ++i) deposit(a, 10, {}, as);
  });
  CHECK_EQ(as.done.load(), 0u);
  CHECK_EQ(fresh(eng, 0)[G_WAL_ACTIVE_BYTES_NOW], 4u * kDepositRecord);
  // Now everything new on shard 0 is refused, and changes nothing.
  const u64 emits0 = eng.stats(0).emits_in_flight;
  Async refused;
  on_shard(eng, 0, [&] {
    deposit(a, 10, {}, refused);
    TransferArgs t{own[1][0], 1, 7};
    execute(a, kOpTransfer, ByteView(&t, sizeof(t)), {}, &Async::cb, &refused);
    ExecOptions sd;
    sd.client_id = 78;
    sd.seq = 1;
    deposit(a, 1000, sd, refused);
  });
  CHECK_EQ(refused.done.load(), 3u);
  CHECK_EQ(refused.count(Status::E_OVERLOADED), 3u);
  expect_causes(before, causes(eng, 0), WAL, 3);
  GaugeSnapshot g = fresh(eng, 0);
  CHECK_EQ(g[G_WAL_NEXT_LSN] - lsn0, 4u);
  CHECK_EQ(g[G_WAL_ACTIVE_BYTES_NOW], 4u * kDepositRecord);
  CHECK_EQ(eng.stats(0).emits_in_flight, emits0);
  CHECK_EQ(eng.stats(0).objects, objects0);
  CHECK_EQ(version_of(eng, 0, a), v0 + 4);
  CHECK_EQ(balance(eng, 0, a, true).balance_cents, b0.balance_cents + 40);

  Engine::debug_hold_wal(0, false);
  CHECK(as.wait(4));
  CHECK_EQ(as.count(Status::OK), 4u);

  // A4: the refused sequenced deposit (client 78, seq 1) has no session:
  // its retry runs once; a resend is replayed, not applied again.
  OneResult r1 = one(eng, 0, [&](Completion cb, void* ctx) {
    ExecOptions sd;
    sd.client_id = 78;
    sd.seq = 1;
    AmountArgs d{1000, 7};
    execute(a, kOpDeposit, ByteView(&d, sizeof(d)), sd, cb, ctx);
  });
  CHECK_EQ(r1.r.status, Status::OK);
  CHECK(!r1.r.replayed);
  OneResult r2 = one(eng, 0, [&](Completion cb, void* ctx) {
    ExecOptions sd;
    sd.client_id = 78;
    sd.seq = 1;
    AmountArgs d{1000, 7};
    execute(a, kOpDeposit, ByteView(&d, sizeof(d)), sd, cb, ctx);
  });
  CHECK_EQ(r2.r.status, Status::OK);
  CHECK(r2.r.replayed);
  CHECK_EQ(balance(eng, 0, a).balance_cents, b0.balance_cents + 40 + 1000);
  CHECK_EQ(eng.stats(0).objects, objects0 + 1);  // client 78's session, made by the retry
  eng.release_emits();
  CHECK(eng.wait_emits());
  eng.stop();
  rm_rf(dir);
  std::printf("A3 WAL budget: 4 admitted (%u bytes active); deposit, transfer, sequenced deposit refused "
              "with no record, version, object, session or follow-up; A4 retry ran once, resend replayed\n",
              4 * kDepositRecord);
}

// ---- A5 -------------------------------------------------------------------------------

void test_wal_sizing() {
  const std::string dir = temp_dir();
  Engine::Config c = config(1, dir);  // one shard: every transfer is local
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 2);
  Engine::debug_hold_wal(0, true);
  const u64 active0 = fresh(eng, 0)[G_WAL_ACTIVE_BYTES_NOW];
  const u64 lsn0 = fresh(eng, 0)[G_WAL_NEXT_LSN];
  Async as;
  on_shard(eng, 0, [&] {
    ExecOptions o;
    o.client_id = 9;
    o.seq = 1;
    TransferArgs t{own[0][1], 5, 7};
    execute(own[0][0], kOpTransfer, ByteView(&t, sizeof(t)), o, &Async::cb, &as);
  });
  GaugeSnapshot g = fresh(eng, 0);
  const u64 appended = g[G_WAL_ACTIVE_BYTES_NOW] - active0;
  // Peer (UPDATE), session (CREATE), object (UPDATE).
  CHECK_EQ(g[G_WAL_NEXT_LSN] - lsn0, 3u);
  CHECK_EQ(appended, u64{kDepositRecord} + kDepositRecord + record_size(sizeof(SessionV1)));
  CHECK(appended <= mutation_wal_need(true));
  Engine::debug_hold_wal(0, false);
  CHECK(as.wait(1));
  CHECK_EQ(as.count(Status::OK), 1u);
  CHECK_EQ(balance(eng, 0, own[0][0]).balance_cents + balance(eng, 0, own[0][1]).balance_cents, 2 * kStart);
  eng.stop();
  rm_rf(dir);
  std::printf("A5 WAL sizing: local transfer + session appended %llu bytes in 3 records, reserved %llu\n",
              (unsigned long long)appended, (unsigned long long)mutation_wal_need(true));
}

// ---- A6 -------------------------------------------------------------------------------

void test_internal_drains() {
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  c.hold_emits = true;
  // Shard 1's clients are closed by the WAL budget: with its WAL held,
  // deposits are admitted at 0 and 256 active bytes and refused from 512.
  const u64 budget = mutation_wal_need(false) + 300;
  c.wal_buffer_budget_bytes = budget;
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 16);
  i64 total = 0;
  for (auto& v : own)
    for (auto& id : v) total += balance(eng, 0, id).balance_cents;
  // kN transfers from shard 0 to shard 1, their credits held. 16 sources
  // with 64 outbox entries each and a gate of 1,024 take all of them.
  constexpr u32 kN = 400;
  const ShardStats s0 = eng.stats(0), s1 = eng.stats(1);
  for (u32 i = 0; i < kN; ++i) {
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
      TransferArgs t{own[1][i % 16], 1, 7};
      execute(own[0][i % 16], kOpTransfer, ByteView(&t, sizeof(t)), {}, cb, ctx);
    });
    CHECK_EQ(r.r.status, Status::OK);  // every transfer sent: no partial-send pass
  }
  CHECK_EQ(eng.stats(0).emits - s0.emits, u64{kN});  // one credit each
  // Close shard 1's client admission: its WAL held, its WAL budget full.
  Engine::debug_hold_wal(1, true);
  CHECK_EQ(fresh(eng, 1)[G_WAL_ACTIVE_BYTES_NOW], 0u);  // nothing appended since the accounts
  Async fill;
  on_shard(eng, 1, [&] {
    for (int i = 0; i < 4; ++i) deposit(own[1][i], 1, {}, fill);
  });
  CHECK_EQ(fill.count(Status::E_OVERLOADED), 2u);
  const Causes closed = causes(eng, 1);
  CHECK_EQ(closed.wal, 2u);
  CHECK_EQ(closed.overloaded, 2u);
  // Release the follow-ups: shard 1 applies every credit (never refused)
  // although its clients are refused. Its settles wait for its WAL, so
  // nothing else is logged there meanwhile.
  const u64 lsn1 = fresh(eng, 1)[G_WAL_NEXT_LSN];
  eng.release_emits();
  for (int i = 0; i < 40000 && eng.stats(1).emits_received - s1.emits_received < kN; ++i) ::usleep(500);
  CHECK_EQ(eng.stats(0).emits_sent - s0.emits_sent, u64{kN});
  CHECK_EQ(eng.stats(1).emits_received - s1.emits_received, u64{kN});
  GaugeSnapshot g = fresh(eng, 1);
  CHECK_EQ(g[G_WAL_NEXT_LSN] - lsn1, u64{kN});  // exactly one credit record each
  // The internal records alone went past the client budget.
  CHECK(g[G_WAL_ACTIVE_BYTES_NOW] > budget);
  const u64 active_held = g[G_WAL_ACTIVE_BYTES_NOW];
  OneResult refused = one(eng, 1, [&](Completion cb, void* ctx) {
    AmountArgs d{1, 7};
    execute(own[1][5], kOpDeposit, ByteView(&d, sizeof(d)), {}, cb, ctx);
  });
  CHECK_EQ(refused.r.status, Status::E_OVERLOADED);
  expect_causes(closed, causes(eng, 1), WAL, 1);
  // Durability resumes: everything settles, nothing is lost or doubled.
  Engine::debug_hold_wal(1, false);
  CHECK(fill.wait(4));
  CHECK_EQ(fill.count(Status::OK), 2u);
  CHECK(eng.wait_emits());
  CHECK_EQ(eng.stats(0).emits_in_flight + eng.stats(1).emits_in_flight, 0u);
  i64 after = 0;
  for (auto& v : own)
    for (auto& id : v) after += balance(eng, 0, id).balance_cents;
  CHECK_EQ(after, total + 2);  // the 2 admitted deposits of 1
  i64 dst = 0;
  for (auto& id : own[1]) dst += balance(eng, 0, id).balance_cents;
  CHECK_EQ(dst, 16 * kStart + i64{kN} + 2);  // every credit applied exactly once
  eng.stop();
  rm_rf(dir);
  std::printf("A6 internal work: %u transfers sent, %u credits applied on a shard refusing clients; "
              "%llu internal bytes active, past the %llu-byte client budget; all settled exactly\n",
              kN, kN, (unsigned long long)active_held, (unsigned long long)budget);
}

// ---- A7 -------------------------------------------------------------------------------

void test_read_flood() {
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  c.hold_emits = true;
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 40);
  build_backlog(eng, own[0], own[1]);
  const u64 backlog = eng.stats(0).emits_in_flight;
  std::atomic<bool> stop{false};
  std::atomic<u64> reads{0}, refused{0};
  std::thread flood([&] {
    while (!stop.load()) {
      run_all(
          eng, 2000,
          [&](u16 s, u64 i, Completion cb, void* ctx) {
            execute(own[s][i % own[s].size()], kOpGetBalance, ByteView{}, {}, cb, ctx);
          },
          [&](u16, u64, const ExecResult& r) {
            ++reads;
            if (r.status != Status::OK) ++refused;
          });
    }
  });
  while (reads.load() < 4000) std::this_thread::yield();
  const u64 at_release = reads.load();
  const u64 t0 = sri_now_ns();
  eng.release_emits();
  CHECK(eng.wait_emits(60));
  const double drain_ms = (sri_now_ns() - t0) / 1e6;
  const u64 at_drained = reads.load();
  // The flood was still running when the backlog had drained.
  while (reads.load() < at_drained + 4000) std::this_thread::yield();
  stop = true;
  flood.join();
  CHECK_EQ(refused.load(), 0u);
  const u64 during = at_drained - at_release;
  eng.stop();
  rm_rf(dir);
  std::printf("A7 read flood: backlog of %llu drained in %.1f ms with the flood running throughout (%llu reads "
              "answered in that window), 0 refused\n",
              (unsigned long long)backlog, drain_ms, (unsigned long long)during);
}

// ---- A9 -------------------------------------------------------------------------------

void test_expiry_internal() {
  const std::string dir = temp_dir();
  Engine::Config c = config(1, dir);
  c.session_ttl_s = 1;
  // Below one DELETE record: once anything is active, every client op,
  // a client delete included, is refused.
  c.wal_buffer_budget_bytes = 100;
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 4);
  for (u64 client = 100; client < 104; ++client) {
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) {
      ExecOptions o;
      o.client_id = client;
      o.seq = 1;
      AmountArgs d{1, 7};
      execute(own[0][0], kOpDeposit, ByteView(&d, sizeof(d)), o, cb, ctx);
    });
    CHECK_EQ(r.r.status, Status::OK);
  }
  ::usleep(2500 * 1000);  // past the TTL (expiry needs touched + ttl < now)
  Engine::debug_hold_wal(0, true);
  Async fill;
  on_shard(eng, 0, [&] {
    for (int i = 0; i < 4; ++i) deposit(own[0][1], 1, {}, fill);
  });
  CHECK_EQ(fill.count(Status::E_OVERLOADED), 3u);  // client admission closed (the first ran at 0 active)
  const Causes closed = causes(eng, 0);
  const ShardStats st0 = eng.stats(0);
  // The checkpoint expires the 4 sessions first; it may then wait for the
  // held WAL, so it runs on its own thread.
  std::thread ck([&] { eng.checkpoint(); });
  for (int i = 0; i < 20000 && eng.stats(0).sessions_expired < st0.sessions_expired + 4; ++i) ::usleep(500);
  const ShardStats st1 = eng.stats(0);
  CHECK_EQ(st1.sessions_expired - st0.sessions_expired, 4u);
  CHECK_EQ(st1.deletes - st0.deletes, 4u);  // every expiry delete ran
  expect_causes(closed, causes(eng, 0), WAL, 0);
  Engine::debug_hold_wal(0, false);
  ck.join();
  CHECK(fill.wait(4));
  CHECK_EQ(fill.count(Status::OK), 1u);
  eng.stop();
  rm_rf(dir);
  std::printf("A9 session expiry: 4 sessions expired while the WAL budget refused clients; 0 refusals counted\n");
}

// ---- S1.1: a test type that grows to its bounds --------------------------------------

// Blob: a payload of `len` bytes, grown by its ops. kBlobMax is chosen so
// the allocator rounds an object of that size up to a larger capacity
// (checked in A10), the case S1 under-reserved.
namespace blob {
constexpr u32 kType = 900;
constexpr u32 kMax = 33000;
enum : OpID { kCreate = 9001, kGrow = 9002, kGrowRaw = 9003, kLink = 9004, kInfo = 9005 };
struct LenArgs {
  u32 len;
};
struct LinkArgs {
  ObjectID peer;
  u32 len;
  u32 pad;
};
struct Info {
  u32 length, capacity;
};
template <typename T>
T arg(ByteView a) {
  T t{};
  std::memcpy(&t, a.data, std::min<std::size_t>(a.len, sizeof(T)));
  return t;
}
Status reserve_v(OpCtx& c, const ObjectHeader&, ByteView a) { return c.reserve(arg<LenArgs>(a).len); }
void resize_a(OpCtx& c, ByteView a) { c.resize(arg<LenArgs>(a).len); }
Status ok_v(OpCtx&, const ObjectHeader&, ByteView) { return Status::OK; }
Status link_v(OpCtx& c, const ObjectHeader&, ByteView a) {
  const LinkArgs l = arg<LinkArgs>(a);
  if (c.local(l.peer, kType) == nullptr) return Status::E_BAD_ARGS;
  return c.reserve(l.len);
}
void link_a(OpCtx& c, ByteView a) {
  const LinkArgs l = arg<LinkArgs>(a);
  ObjectHeader* p = c.local(l.peer, kType);
  if (p->length) p->payload()[0] ^= 1;  // changes the peer, not its length
  c.resize(l.len);
}
void info_a(OpCtx& c, ByteView) {
  Info i{c.obj()->length, c.obj()->capacity};
  c.set_result(&i, sizeof(i));
}
void register_blob() {
  Registry& r = registry();
  if (r.type(kType)) return;
  TypeDescriptor t;
  t.type_id = kType;
  t.name = "Blob";
  t.initial_size = 16;
  t.max_size = kMax;
  t.create_op = kCreate;
  r.add_type(t);
  auto op = [&](OpID id, const char* name, OpKind kind, u32 max_args, u32 max_result,
                Status (*v)(OpCtx&, const ObjectHeader&, ByteView), void (*a)(OpCtx&, ByteView)) {
    OpDescriptor d;
    d.op_id = id;
    d.type_id = kType;
    d.name = name;
    d.kind = kind;
    d.max_args = max_args;
    d.max_result = max_result;
    d.validate = v;
    d.apply = a;
    r.add_op(d);
  };
  op(kCreate, "Blob.create", OpKind::CREATE, sizeof(LenArgs), 0, reserve_v, resize_a);
  op(kGrow, "Blob.grow", OpKind::MUTATE, sizeof(LenArgs), 0, reserve_v, resize_a);
  op(kGrowRaw, "Blob.grow_raw", OpKind::MUTATE, sizeof(LenArgs), 0, ok_v, resize_a);  // no reserve()
  op(kLink, "Blob.link", OpKind::MUTATE, sizeof(LinkArgs), 0, link_v, link_a);
  op(kInfo, "Blob.info", OpKind::READ, 0, sizeof(Info), ok_v, info_a);
}
}  // namespace blob

ObjectID blob_create(Engine& eng, u16 s, u32 len, const ExecOptions& o = {}) {
  OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
    blob::LenArgs a{len};
    create(blob::kType, ByteView(&a, sizeof(a)), o, cb, ctx);
  });
  CHECK_EQ(r.r.status, Status::OK);
  return r.r.id;
}
Status blob_op(Engine& eng, u16 s, const ObjectID& id, OpID op, u32 len) {
  OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
    blob::LenArgs a{len};
    execute(id, op, ByteView(&a, sizeof(a)), {}, cb, ctx);
  });
  return r.r.status;
}
blob::Info blob_info(Engine& eng, u16 s, const ObjectID& id) {
  OneResult r = one(eng, s, [&](Completion cb, void* ctx) { execute(id, blob::kInfo, ByteView{}, {}, cb, ctx); });
  CHECK_EQ(r.r.status, Status::OK);
  return r.as<blob::Info>();
}
// What admit() reserves for a create of `type` (shard.cpp run_create).
u64 create_wal_need(u32 type, bool sequenced) {
  return record_size(registry().type(type)->max_size) + (sequenced ? record_size(sizeof(SessionV1)) : 0);
}
// WAL bytes and records one op appends on shard s (its WAL held: nothing
// flushes, so the active buffer only grows).
struct Appended {
  u64 bytes, records;
};
Appended appended_by(Engine& eng, u16 s, const std::function<void()>& issue_on_shard) {
  const GaugeSnapshot g0 = fresh(eng, s);
  on_shard(eng, s, issue_on_shard);
  const GaugeSnapshot g1 = fresh(eng, s);
  return {g1[G_WAL_ACTIVE_BYTES_NOW] - g0[G_WAL_ACTIVE_BYTES_NOW], g1[G_WAL_NEXT_LSN] - g0[G_WAL_NEXT_LSN]};
}

// The runtime's own threads: TSan starts one background thread with the
// first user thread and keeps it (it is stopped around fork).
#if defined(__SANITIZE_THREAD__)
constexpr u32 kRuntimeThreads = 1;
#else
constexpr u32 kRuntimeThreads = 0;
#endif

u32 thread_count() {
  u32 n = 0;
  if (DIR* d = ::opendir("/proc/self/task")) {
    while (dirent* e = ::readdir(d)) n += e->d_name[0] != '.';
    ::closedir(d);
  }
  return n;
}

// ---- A10 ------------------------------------------------------------------------------

// Child process: grow a Blob past max_size within its capacity, without
// reserve(). S1 allowed this (reserved = capacity), appending more than the
// reservation and writing an image recovery rejects. S1.1 caps it.
[[noreturn]] void grow_past_max_child(const std::string& dir) {
  Engine::Config c = config(1, dir);
  Engine eng(c);
  eng.start();
  const ObjectID b = blob_create(eng, 0, 16);
  CHECK_EQ(blob_op(eng, 0, b, blob::kGrow, blob::kMax), Status::OK);
  (void)blob_op(eng, 0, b, blob::kGrowRaw, blob::kMax + 32);  // aborts here
  std::_Exit(0);
}

void test_reservation_boundary() {
  const std::string dir = temp_dir();
  Engine::Config c = config(1, dir);
  Engine eng(c);
  eng.start();
  const ObjectID b = blob_create(eng, 0, 16);
  CHECK_EQ(blob_op(eng, 0, b, blob::kGrow, blob::kMax), Status::OK);
  const blob::Info i = blob_info(eng, 0, b);
  CHECK_EQ(i.length, blob::kMax);
  CHECK(i.capacity > blob::kMax);  // the allocator rounded up: the S1 shortfall case
  // reserve() past max_size is refused (unchanged).
  CHECK_EQ(blob_op(eng, 0, b, blob::kGrow, blob::kMax + 1), Status::E_TOO_LARGE);
  Engine::debug_hold_wal(0, true);
  // The largest legal record: the object at max_size, logged by a mutation
  // with and without reserve().
  for (OpID op : {blob::kGrow, blob::kGrowRaw}) {
    Async as;
    const Appended a = appended_by(eng, 0, [&] {
      blob::LenArgs x{blob::kMax};
      execute(b, op, ByteView(&x, sizeof(x)), {}, &Async::cb, &as);
    });
    CHECK_EQ(a.records, 1u);
    CHECK_EQ(a.bytes, record_size(blob::kMax));
    CHECK(a.bytes <= mutation_wal_need(false, blob::kType));
    Engine::debug_hold_wal(0, false);
    CHECK(as.wait(1));
    CHECK_EQ(as.count(Status::OK), 1u);
    Engine::debug_hold_wal(0, true);
  }
  Engine::debug_hold_wal(0, false);
  eng.stop();
  // An object at max_size recovers.
  Engine::Config rc = config(1, dir);
  rc.init = false;
  Engine again(rc);
  again.start();
  CHECK_EQ(blob_info(again, 0, b).length, blob::kMax);
  again.stop();
  rm_rf(dir);
  // Growing past max_size within the capacity is a handler contract
  // violation now, caught at resize, before anything is logged.
  const std::string cdir = temp_dir();
  const std::string err = cdir + ".stderr";
  // Fork only with no thread left from the engines above (all stopped and
  // joined): TSan's die_after_fork would otherwise kill the child before it
  // aborts where this test expects.
  CHECK_EQ(thread_count(), 1u + kRuntimeThreads);
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    const int fd = ::open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ::dup2(fd, 2) < 0) std::_Exit(3);
    grow_past_max_child(cdir);
  }
  int st = 0;
  CHECK_EQ(::waitpid(pid, &st, 0), pid);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
  std::string text;
  if (FILE* f = std::fopen(err.c_str(), "r")) {
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) text += buf;
    std::fclose(f);
  }
  CHECK(text.find("resize beyond reserved length") != std::string::npos);
  rm_rf(cdir);
  rm_rf(err);
  std::printf("A10 reservation boundary: Blob at max_size %u with capacity %u appends %llu bytes, within %llu "
              "reserved; it recovers; growing past max_size inside the capacity aborts at resize\n",
              blob::kMax, i.capacity, (unsigned long long)record_size(blob::kMax),
              (unsigned long long)mutation_wal_need(false, blob::kType));
}

// ---- A11 ------------------------------------------------------------------------------

void test_create_boundary() {
  const std::string dir = temp_dir();
  Engine::Config c = config(1, dir);
  Engine eng(c);
  eng.start();
  Engine::debug_hold_wal(0, true);
  // A sequenced create that grows to max_size in its apply: the object's
  // CREATE record and the session's.
  Async as;
  const Appended a = appended_by(eng, 0, [&] {
    ExecOptions o;
    o.client_id = 31;
    o.seq = 1;
    blob::LenArgs x{blob::kMax};
    create(blob::kType, ByteView(&x, sizeof(x)), o, &Async::cb, &as);
  });
  CHECK_EQ(a.records, 2u);
  CHECK_EQ(a.bytes, record_size(blob::kMax) + record_size(sizeof(SessionV1)));
  CHECK(a.bytes <= create_wal_need(blob::kType, true));
  // A create past max_size is refused in validate, logging nothing.
  Async big;
  const Appended b = appended_by(eng, 0, [&] {
    blob::LenArgs x{blob::kMax + 1};
    create(blob::kType, ByteView(&x, sizeof(x)), {}, &Async::cb, &big);
  });
  CHECK_EQ(b.records, 0u);
  CHECK_EQ(big.count(Status::E_TOO_LARGE), 1u);
  Engine::debug_hold_wal(0, false);
  CHECK(as.wait(1));
  CHECK_EQ(as.count(Status::OK), 1u);
  eng.stop();
  rm_rf(dir);
  std::printf("A11 create boundary: sequenced create to max_size appends %llu bytes in 2 records, within %llu "
              "reserved; past max_size refused with nothing logged\n",
              (unsigned long long)a.bytes, (unsigned long long)create_wal_need(blob::kType, true));
}

// ---- A12 ------------------------------------------------------------------------------

void test_peer_session_boundary() {
  const std::string dir = temp_dir();
  Engine::Config c = config(1, dir);
  Engine eng(c);
  eng.start();
  const ObjectID self = blob_create(eng, 0, 16), peer = blob_create(eng, 0, 16);
  CHECK_EQ(blob_op(eng, 0, peer, blob::kGrow, blob::kMax), Status::OK);  // near-max peer
  CHECK_EQ(blob_op(eng, 0, self, blob::kGrow, blob::kMax - 64), Status::OK);
  Engine::debug_hold_wal(0, true);
  Async as;
  const Appended a = appended_by(eng, 0, [&] {
    ExecOptions o;
    o.client_id = 32;
    o.seq = 1;
    blob::LinkArgs x{peer, blob::kMax, 0};
    execute(self, blob::kLink, ByteView(&x, sizeof(x)), o, &Async::cb, &as);
  });
  // Peer UPDATE, session CREATE, object UPDATE: every term of R2 at its maximum.
  CHECK_EQ(a.records, 3u);
  CHECK_EQ(a.bytes, 2 * record_size(blob::kMax) + record_size(sizeof(SessionV1)));
  CHECK_EQ(a.bytes, mutation_wal_need(true, blob::kType));  // the reservation is exact here
  Engine::debug_hold_wal(0, false);
  CHECK(as.wait(1));
  CHECK_EQ(as.count(Status::OK), 1u);
  eng.stop();
  rm_rf(dir);
  std::printf("A12 peer + session boundary: max object + max peer + session append %llu bytes in 3 records, "
              "exactly the %llu reserved\n",
              (unsigned long long)a.bytes, (unsigned long long)mutation_wal_need(true, blob::kType));
}

// ---- A13, A14 -------------------------------------------------------------------------

void test_remote_read_bound() {
  const std::string dir = temp_dir();
  Engine::Config c = config(2, dir);
  c.pending_budget = 8;
  // Room for 5 waiters of 256 + 16 bytes: 2 deposits and 3 REMOTE reads.
  c.waiter_budget_bytes = 5 * (256 + sizeof(BalanceResult)) + 8;
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 4);
  const ObjectID a = own[1][0];
  const BalanceResult b0 = balance(eng, 1, a);
  const Causes c0 = causes(eng, 0), c1 = causes(eng, 1);
  // Make the account undurable on shard 1: 2 deposits wait.
  Engine::debug_hold_wal(1, true);
  Async deps;
  on_shard(eng, 1, [&] {
    for (int i = 0; i < 2; ++i) deposit(a, 10, {}, deps);
  });
  CHECK_EQ(deps.done.load(), 0u);
  // A13: 20 durable reads of it forwarded from shard 0.
  constexpr u32 kReads = 20;
  Async reads;
  on_shard(eng, 0, [&] {
    for (u32 i = 0; i < kReads; ++i) execute(a, kOpGetBalance, ByteView{}, {}, &Async::cb, &reads);
  });
  // 12 refused at the origin at once (8 pending slots); of the 8 forwarded,
  // the owner holds 3 (its waiter budget) and answers 5 E_OVERLOADED.
  for (int i = 0; i < 20000 && reads.done.load() < 17; ++i) ::usleep(500);
  CHECK_EQ(reads.done.load(), 17u);
  CHECK_EQ(reads.count(Status::E_OVERLOADED), 17u);
  expect_causes(c0, causes(eng, 0), PENDING, 12);
  expect_causes(c1, causes(eng, 1), WAITER, 5);
  GaugeSnapshot g0 = fresh(eng, 0), g1 = fresh(eng, 1);
  CHECK_EQ(g0[G_PENDING_NOW], 3u);
  CHECK_EQ(g0[G_PENDING_PEAK], 8u);
  CHECK_EQ(g1[G_WAITERS_NOW], 5u);  // 2 deposits + 3 REMOTE reads: the bound
  // More reads now: the 5 free slots fill, the owner refuses them all.
  Async more;
  on_shard(eng, 0, [&] {
    for (u32 i = 0; i < 7; ++i) execute(a, kOpGetBalance, ByteView{}, {}, &Async::cb, &more);
  });
  for (int i = 0; i < 20000 && more.done.load() < 7; ++i) ::usleep(500);
  CHECK_EQ(more.count(Status::E_OVERLOADED), 7u);
  expect_causes(c0, causes(eng, 0), PENDING, 14);  // 2 more at the origin
  expect_causes(c1, causes(eng, 1), WAITER, 10);   // 5 more at the owner
  CHECK_EQ(fresh(eng, 1)[G_WAITERS_NOW], 5u);
  CHECK_EQ(fresh(eng, 0)[G_PENDING_NOW], 3u);
  // A14: durability resumes; the held answers arrive once each.
  Engine::debug_hold_wal(1, false);
  CHECK(reads.wait(kReads));
  CHECK(deps.wait(2));
  for (int i = 0; i < 4000 && fresh(eng, 0)[G_PENDING_NOW] != 0; ++i) ::usleep(500);
  ::usleep(20000);  // no late or duplicate answer arrives
  CHECK_EQ(reads.done.load(), kReads);
  CHECK_EQ(reads.count(Status::OK), 3u);
  CHECK_EQ(deps.count(Status::OK), 2u);
  {
    std::lock_guard<std::mutex> lk(reads.mu);
    for (const ExecResult& r : reads.results) {
      if (r.status == Status::OK) CHECK(r.durable);
    }
  }
  CHECK_EQ(fresh(eng, 0)[G_PENDING_NOW], 0u);
  CHECK_EQ(fresh(eng, 1)[G_WAITERS_NOW], 0u);
  // Admission is open again: 20 forwarded reads all answered.
  const Causes c2 = causes(eng, 0), c3 = causes(eng, 1);
  for (u32 i = 0; i < kReads; ++i) {
    OneResult r = one(eng, 0, [&](Completion cb, void* ctx) { execute(a, kOpGetBalance, ByteView{}, {}, cb, ctx); });
    CHECK_EQ(r.r.status, Status::OK);
    CHECK_EQ(r.as<BalanceResult>().balance_cents, b0.balance_cents + 20);
  }
  expect_causes(c2, causes(eng, 0), PENDING, 0);
  expect_causes(c3, causes(eng, 1), WAITER, 0);
  eng.stop();
  rm_rf(dir);
  std::printf("A13 remote-read bound: pending stopped at 8 (12+2 refused_pending at the origin), REMOTE "
              "waiters at 3 (5+5 refused_waiter at the owner)\n"
              "A14 recovery: 3 held reads and 2 deposits answered once each after durability resumed; "
              "pending and waiters back to 0; 20 reads admitted again\n");
}

// ---- A15 ------------------------------------------------------------------------------

// Internal follow-ups are never refused, so a shard whose WAL stalls keeps
// appending what its senders already admitted. This characterizes that
// overshoot with large records: two sender shards fill one destination
// account's inbox to kMaxEntries (records up to max_size), and the credits
// past it are logged as "retry" at max_size. The destination must never grow
// past max_size (S1 allowed it: the S1.1 cap) and must recover.
void test_internal_overshoot() {
  const std::string dir = temp_dir();
  Engine::Config c = config(3, dir);
  c.hold_emits = true;
  Engine eng(c);
  eng.start();
  auto own = accounts(eng, 40);
  const ObjectID dst = own[1][0];
  i64 total = 0;
  for (auto& v : own)
    for (auto& id : v) total += balance(eng, 0, id).balance_cents;
  // Shards 0 and 2 send transfers to dst until their gates close.
  u64 sent = 0;
  for (u16 s : {u16{0}, u16{2}}) {
    for (u64 i = 0; eng.stats(s).emits_in_flight <= kGate; ++i) {
      CHECK(i < 100000);
      OneResult r = one(eng, s, [&](Completion cb, void* ctx) {
        TransferArgs t{dst, 1, 7};
        execute(own[s][i % own[s].size()], kOpTransfer, ByteView(&t, sizeof(t)), {}, cb, ctx);
      });
      if (r.r.status == Status::OK) ++sent;
      else CHECK(r.r.status == Status::E_APP);  // a source's outbox full
    }
  }
  const u64 peak0 = eng.stats(0).emits_in_flight, peak2 = eng.stats(2).emits_in_flight;
  CHECK(peak0 <= kGate + 1 && peak2 <= kGate + 1);  // the gate, plus one op's single credit
  CHECK_EQ(peak0 + peak2, sent);
  CHECK(sent > kMaxEntries);
  // Stall shard 1's WAL and release the credits.
  Engine::debug_hold_wal(1, true);
  const ShardStats s1 = eng.stats(1);
  const GaugeSnapshot g0 = fresh(eng, 1);
  eng.release_emits();
  for (int i = 0; i < 60000 && eng.stats(1).emits_received - s1.emits_received < sent; ++i) ::usleep(500);
  const u64 received = eng.stats(1).emits_received - s1.emits_received;
  CHECK_EQ(received, sent);  // nothing beyond what the senders admitted
  const GaugeSnapshot g1 = fresh(eng, 1);
  const u64 records = g1[G_WAL_NEXT_LSN] - g0[G_WAL_NEXT_LSN];
  const u64 bytes = g1[G_WAL_ACTIVE_BYTES_NOW] - g0[G_WAL_ACTIVE_BYTES_NOW];
  CHECK_EQ(records, sent);  // one record per credit (settles wait on the WAL)
  // Every record is at most the account's max_size: the characterized
  // bound is sent x record_size(max_size).
  const u64 rmax = record_size(registry().type(kTypeId)->max_size);
  CHECK(bytes <= sent * rmax);
  OneResult info = one(eng, 1, [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.allow_undurable = true;
    execute(dst, kOpGet, ByteView{}, o, cb, ctx);
  });
  CHECK_EQ(info.r.status, Status::OK);
  CHECK(info.bytes.size() >= sizeof(AccountV1));
  AccountV1 acc;
  std::memcpy(&acc, info.bytes.data(), sizeof(acc));
  CHECK_EQ(u32{acc.n_in} + acc.n_out, kMaxEntries);  // full, and no further
  const u64 budget = c.wal_buffer_budget_bytes;
  Engine::debug_hold_wal(1, false);
  CHECK(eng.wait_emits(120));
  i64 after = 0;
  for (auto& v : own)
    for (auto& id : v) after += balance(eng, 0, id).balance_cents;
  CHECK_EQ(after, total);
  CHECK_EQ(balance(eng, 1, dst).balance_cents, kStart + static_cast<i64>(sent));
  eng.stop();
  // Recovery accepts everything logged.
  Engine::Config rc = config(3, dir);
  rc.init = false;
  Engine again(rc);
  again.start();
  CHECK_EQ(balance(again, 1, dst).balance_cents, kStart + static_cast<i64>(sent));
  again.stop();
  rm_rf(dir);
  std::printf("A15 internal overshoot: %llu credits (gates %llu + %llu) applied on a stalled shard: %llu records, "
              "%llu bytes (%.1fx the %llu-byte client budget; bound %llu x %llu); inbox stopped at %u; "
              "settled, conserved, recovered\n",
              (unsigned long long)sent, (unsigned long long)peak0, (unsigned long long)peak2,
              (unsigned long long)records, (unsigned long long)bytes, double(bytes) / double(budget),
              (unsigned long long)budget, (unsigned long long)sent, (unsigned long long)rmax, kMaxEntries);
}

}  // namespace

// With arguments, runs only the named tests (A1 ... A15), for negative
// checks against a reverted engine; ctest runs them all.
int main(int argc, char** argv) {
  register_type();
  blob::register_blob();
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"A1", [] { test_followup_gate(false); }},
      {"A8", [] { test_followup_gate(true); }},
      {"A2", test_waiter_budget},
      {"A3", test_wal_budget},
      {"A5", test_wal_sizing},
      {"A6", test_internal_drains},
      {"A7", test_read_flood},
      {"A9", test_expiry_internal},
      {"A10", test_reservation_boundary},
      {"A11", test_create_boundary},
      {"A12", test_peer_session_boundary},
      {"A13", test_remote_read_bound},
      {"A15", test_internal_overshoot},
  };
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  std::printf("test_s1_admission: all passed\n");
  return 0;
}
