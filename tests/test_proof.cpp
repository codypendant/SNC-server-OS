// The serving proof (docs/MIGRATION_FAIL_CLOSED.md 4, 6, 12), in one
// process: one node of group 0 of a two-group cluster (group 1 is never
// started; nothing here needs it), driven through the engine's move API.
// Covers the PR #39 review's added tests:
//   C  proof-cache ordering: a map adoption that leaves a slot unproven
//      leaves no window: the first op after update_placement returns is
//      refused, and so are index lookups, creates in the slot and the
//      credit of a transfer into it (held, applied once the slot is proven);
//   D  exact activation: STAGED(M1) never activates for an arrival of M3,
//      even though the epoch condition holds; STAGED(M) activates for M;
//   E  a stale local placement (restart with a map whose arrival is older
//      than the local ACTIVE record): STALE_MAP, not served, until the
//      current placement is adopted;
//   and an UNSEAL that leaves a slot unproven (D1d) re-emits nothing from
//   its stale objects.
// Usage: test_proof
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "harness.h"
#include "snc/cluster.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr u16 kShards = 2;

CreateArgs account_args(const char* email) {
  CreateArgs a{};
  a.balance_cents = 1000;
  a.created_ns = 1;
  a.email_len = static_cast<u8>(std::strlen(email));
  std::memcpy(a.email, email, a.email_len);
  a.name_len = 1;
  a.name[0] = 'n';
  return a;
}

Engine::Config config(const std::string& dir) {
  Engine::Config c;
  c.runtime.n_shards = kShards;
  c.runtime.pin = false;
  c.data_dir = dir;
  c.init = true;
  ClusterGroup g;
  g.n_shards = kShards;
  g.members.push_back(ClusterMember{"127.0.0.1", 1, 0});  // node links unused: no follow-up leaves group 0
  c.cluster = {g, g};
  c.group_id = 0;
  return c;
}

u16 owner_shard(Engine& eng, u16 slot) {
  (void)eng;
  return static_cast<u16>(slot % kShards);  // only to pick a thread to issue from; any shard forwards
}

Status deposit(Engine& eng, const ObjectID& id, i64 cents) {
  AmountArgs a{cents, 2};
  return one(eng, 0, [&](Completion d, void* ctx) {
           execute(id, kOpDeposit, ByteView(&a, sizeof(a)), {}, d, ctx);
         }).r.status;
}

Status create_in(Engine& eng, const ObjectID& near) {
  CreateArgs a = account_args("y");
  return one(eng, 1, [&](Completion d, void* ctx) {
           create_near(near, kTypeId, ByteView(&a, sizeof(a)), {}, d, ctx);
         }).r.status;
}

i64 stored_balance(Engine& eng, const ObjectID& id) {
  i64 b = -1;
  for (u16 s = 0; s < eng.shard_count(); ++s) {
    eng.for_each_object(s, [&](const ObjectHeader& h) {
      if (h.id == id) {
        AccountV1 v;
        std::memcpy(&v, h.payload(), sizeof(v));
        b = v.balance_cents;
      }
    });
  }
  return b;
}

std::array<u8, kSlots> slots_with(u16 slot, u8 group) {
  ClusterMap base = ClusterMap::make(config("").cluster);
  std::array<u8, kSlots> s = base.group_of;
  s[slot] = group;
  return s;
}

void wait_for(const std::function<bool()>& f, const char* what) {
  for (int i = 0; i < 2000; ++i) {
    if (f()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::fprintf(stderr, "timed out: %s\n", what);
  CHECK(false);
}

}  // namespace

int main() {
  account::register_type();
  char tmpl[] = "/tmp/snc-proof-XXXXXX";
  CHECK(::mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  ClusterMap genesis = ClusterMap::make(config("").cluster);

  ObjectID x0, y0;
  u16 X = 0;
  {
    Engine eng(config(dir));
    eng.start();
    // An account in a slot of group 0 (X, to be moved), more in X, and one
    // elsewhere in group 0 (Y, never moved) to transfer from.
    for (int tries = 0; tries < 64; ++tries) {
      CreateArgs a = account_args("x");
      OneResult r = one(eng, 0, [&](Completion d, void* ctx) { create(kTypeId, ByteView(&a, sizeof(a)), {}, d, ctx); });
      CHECK_EQ(r.r.status, Status::OK);
      if (genesis.group_of[slot_of(r.r.id)] != 0) continue;
      if (x0 == ObjectID{}) {
        x0 = r.r.id;
        X = slot_of(x0);
      } else if (slot_of(r.r.id) != X) {
        y0 = r.r.id;
        break;
      }
    }
    CHECK(y0 != ObjectID{});
    for (int i = 0; i < 5; ++i) CHECK_EQ(create_in(eng, x0), Status::OK);
    CHECK_EQ(lookup(eng, 0, kEmailIndex, "y").size(), 5u);
    std::printf("slot X=%u\n", X);

    // X leaves: sealed for M 0x10, the export, then the commit to group 1.
    SlotReport rep;
    CHECK_EQ(eng.seal_slot(X, 0x10, 1, 2, rep), Status::OK);
    std::vector<SlotObject> objs;
    u64 count = 0, digest = 0;
    CHECK_EQ(eng.export_slot(X, 0x10, true, {}, objs, count, digest), Status::OK);
    CHECK_EQ(count, 6u);
    std::array<SlotArrival, kSlots> arr{};
    arr[X] = {0x10, 2};
    CHECK(eng.update_placement(slots_with(X, 1), 2, &arr));
    CHECK_EQ(eng.slot_report(X).proof, SlotProof::FOREIGN);

    // It comes back: staged here for M 0x20 (from group 1, epoch 3).
    CHECK_EQ(eng.stage_slot(X, 0x20, 1, 3, true, objs, count, digest, rep), Status::OK);
    CHECK_EQ(rep.phase, u8{3});

    // ---- D: the placement says a different move (0x30) gave X to group 0
    // at epoch 3; STAGED(0x20) satisfies the epoch condition but not the
    // move: it must not activate.
    arr[X] = {0x30, 3};
    CHECK(eng.update_placement(slots_with(X, 0), 3, &arr));
    // ---- C: no window. The first op after the adoption is refused.
    CHECK_EQ(deposit(eng, x0, 1), Status::E_MIGRATING);
    SlotReport r = eng.slot_report(X);
    CHECK_EQ(r.phase, u8{3});
    CHECK_EQ(r.move_id, 0x20u);
    CHECK(!r.serving);
    CHECK_EQ(r.proof, SlotProof::MISSING);
    CHECK_EQ(lookup(eng, 0, kEmailIndex, "y").size(), 0u);  // no index hit in an unproven slot
    CHECK_EQ(create_in(eng, x0), Status::E_MIGRATING);       // no create (or mint) into it
    u64 unproven = 0;
    for (u16 s = 0; s < kShards; ++s) unproven += eng.stats(s).slots_unproven;
    CHECK_EQ(unproven, 1u);
    // A transfer from Y into X: the debit is Y's; the credit is a
    // follow-up to X, held while X is unproven.
    TransferArgs t{x0, 50, 3};
    OneResult tr = one(eng, 0, [&](Completion d, void* ctx) {
      execute(y0, kOpTransfer, ByteView(&t, sizeof(t)), {}, d, ctx);
    });
    CHECK_EQ(tr.r.status, Status::OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(stored_balance(eng, x0), 1000);  // not credited
    u64 held = 0;
    for (u16 s = 0; s < kShards; ++s) {
      const ShardStats st = eng.stats(s);
      held += st.emits_held_now;
    }
    CHECK(held >= 1);  // the credit (and its resend), held

    // A newer placement that still names 0x30 changes nothing.
    CHECK(eng.update_placement(slots_with(X, 0), 4, &arr));
    CHECK_EQ(eng.slot_report(X).proof, SlotProof::MISSING);

    // ---- D (positive): the arrival is 0x20 (the move staged here): it activates.
    arr[X] = {0x20, 3};
    CHECK(eng.update_placement(slots_with(X, 0), 5, &arr));
    r = eng.slot_report(X);
    CHECK_EQ(r.phase, u8{1});
    CHECK(r.serving);
    CHECK_EQ(r.proof, SlotProof::OK);
    CHECK_EQ(deposit(eng, x0, 1), Status::OK);
    CHECK_EQ(lookup(eng, 0, kEmailIndex, "y").size(), 5u);
    CHECK(eng.wait_emits(10));
    wait_for([&] { return stored_balance(eng, x0) == 1000 + 50 + 1; }, "the held credit ran");
    eng.stop();
  }

  // ---- E: a restart from a stale map: X is group 0's, but the arrival on
  // record (0x18) is older than the local ACTIVE(0x20): not proof.
  {
    Engine::Config c = config(dir);
    c.init = false;
    std::array<u8, kSlots> s = slots_with(X, 0);
    c.cluster_slots.assign(s.begin(), s.end());
    c.cluster_epoch = 4;
    c.cluster_arrived.assign(kSlots, SlotArrival{});
    c.cluster_arrived[X] = {0x18, 3};
    Engine eng(c);
    eng.start();
    SlotReport r = eng.slot_report(X);
    CHECK_EQ(r.phase, u8{1});
    CHECK(!r.serving);
    CHECK_EQ(r.proof, SlotProof::STALE_MAP);
    CHECK_EQ(deposit(eng, x0, 1), Status::E_MIGRATING);
    CHECK_EQ(lookup(eng, 0, kEmailIndex, "y").size(), 0u);
    // The current placement adopted: served.
    std::array<SlotArrival, kSlots> arr{};
    arr[X] = {0x20, 3};
    CHECK(eng.update_placement(s, 5, &arr));
    wait_for([&] { return eng.slot_report(X).serving; }, "X served after the current placement");
    CHECK_EQ(deposit(eng, x0, 1), Status::OK);
    CHECK_EQ(stored_balance(eng, x0), 1000 + 50 + 2);
    // Y never moved (no arrival): served throughout.
    CHECK_EQ(deposit(eng, y0, 1), Status::OK);

    // ---- An UNSEAL that leaves X unproven (D1d) resumes nothing. X's
    // account owes a credit to group 1 (no node there: it stays in the
    // outbox, re-emitted by any resume of X). X is sealed for 0x40, the
    // placement then says X came back by 0x50 (this node missed it), and
    // the 0x40 seal is undone: unsealed(0x40) proves nothing for 0x50.
    ObjectID remote;
    for (u64 seq = 1; remote.is_null(); ++seq) {
      const ObjectID id = ObjectID::make(0x777, seq);
      if (genesis.group_of[slot_of(id)] == 1 && slot_of(id) != X) remote = id;
    }
    TransferArgs t{remote, 10, 4};
    CHECK_EQ(one(eng, 0, [&](Completion d, void* ctx) {
               execute(x0, kOpTransfer, ByteView(&t, sizeof(t)), {}, d, ctx);
             }).r.status,
             Status::OK);
    SlotReport rep;
    CHECK_EQ(eng.seal_slot(X, 0x40, 1, 6, rep), Status::OK);
    arr[X] = {0x50, 7};
    CHECK(eng.update_placement(s, 7, &arr));
    auto emits = [&] {
      u64 n = 0;
      for (u16 sh = 0; sh < kShards; ++sh) n += eng.stats(sh).emits;
      return n;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const u64 before = emits();
    CHECK_EQ(eng.unseal_slot(X, 0x40, rep), Status::OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    SlotReport u = eng.slot_report(X);
    CHECK(!u.serving);
    CHECK_EQ(u.proof, SlotProof::MISSING);
    CHECK_EQ(emits(), before);  // the stale outbox was not re-emitted
    CHECK_EQ(deposit(eng, x0, 1), Status::E_MIGRATING);
    eng.stop();
  }
  (void)owner_shard;
  std::printf("test_proof: ok\n");
  std::system(("rm -rf " + dir).c_str());
  return 0;
}
