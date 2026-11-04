// Invariant 18 (AGENTS.md, docs/EVIDENCE.md section 1): a resend of a
// sequenced request already applied is answered from its session before any
// refusal that would invite another resend. test_transfer covers
// E_OVERLOADED; this covers the two slot-move refusals, deterministically
// and in one process:
//
//   E_MIGRATING  the slot is sealed here for a move (docs/MIGRATION.md)
//   E_MOVED      this node's map gives the slot to another group
//
// One clustered engine (group 0 of 2; group 1 never runs). On an account in
// slot S, client 5 applies two deposits (seq 1, 2) and client 6 a
// create_near (seq 1). Then S is sealed, then the placement gives S to
// group 1. At each stage, and again after a restart from the data
// directory, the resends must be answered from the sessions, never
// refused: each client's latest with its first result, client 5's older
// seq 1 with E_REPLAYED_OLD (decided, outcome not kept; docs/RETRIES.md).
// New sequences and unsequenced requests are refused. Nothing runs twice:
// the account's version and balance do not move.
#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "harness.h"
#include "snc/cluster.h"
#include "snc/slot_map.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr u16 kShards = 2;
constexpr i64 kStart = 1000;
constexpr u64 kClient = 5, kCreator = 6;

std::string g_dir;
void rm_rf(const std::string& d) { CHECK_EQ(std::system(("rm -rf " + d).c_str()), 0); }
std::string temp_dir() {
  std::string t = "/tmp/snc-resend-XXXXXX";
  std::vector<char> b(t.begin(), t.end());
  b.push_back(0);
  return mkdtemp(b.data());
}
u16 g_ports = 0;

Engine::Config config(bool init) {
  Engine::Config c;
  c.runtime.n_shards = kShards;
  c.expected_objects = 4096;
  c.data_dir = g_dir;
  c.init = init;
  c.wal_segment_bytes = 1u << 20;
  for (u16 g = 0; g < 2; ++g) {
    ClusterGroup grp;
    grp.n_shards = kShards;
    const u16 at = static_cast<u16>(g_ports + g * 10);
    grp.members.push_back({"127.0.0.1", at, static_cast<u16>(at + 5)});
    c.cluster.push_back(grp);
  }
  c.group_id = 0;
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

struct Fixture {
  ObjectID acc, made;  // the account; the create_near's result
  u16 own = 0, slot = 0;
  u64 version = 0;  // the account's after the second deposit
};

OneResult deposit(Engine& eng, const Fixture& f, u64 client, u64 seq) {
  return one(eng, f.own, [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.client_id = client;
    o.seq = seq;
    AmountArgs a{1, 0};
    execute(f.acc, kOpDeposit, ByteView(&a, sizeof(a)), o, cb, ctx);
  });
}

OneResult make_near(Engine& eng, const Fixture& f, u64 client, u64 seq) {
  return one(eng, f.own, [&](Completion cb, void* ctx) {
    ExecOptions o;
    o.client_id = client;
    o.seq = seq;
    CreateArgs a = account_args();
    create_near(f.acc, kTypeId, ByteView(&a, sizeof(a)), o, cb, ctx);
  });
}

// The account as stored, whether or not this node serves its slot.
std::pair<u64, i64> stored(Engine& eng, const Fixture& f) {
  std::pair<u64, i64> out{0, 0};
  eng.for_each_object(f.own, [&](const ObjectHeader& h) {
    if (h.id == f.acc) {
      out.first = h.version;
      out.second = reinterpret_cast<const AccountV1*>(h.payload())->balance_cents;
    }
  });
  return out;
}

// The resends are answered from the session; new work gets `refusal`.
void check_stage(Engine& eng, const Fixture& f, Status refusal, const char* stage, u64& next_seq) {
  OneResult d = deposit(eng, f, kClient, 2);
  if (d.r.status != Status::OK) std::printf("%s resend of seq 2: %s\n", stage, status_name(d.r.status));
  CHECK_EQ(d.r.status, Status::OK);
  CHECK(d.r.replayed);
  CHECK_EQ(d.r.version, f.version);
  CHECK_EQ(deposit(eng, f, kClient, 1).r.status, Status::E_REPLAYED_OLD);
  OneResult c = make_near(eng, f, kCreator, 1);
  CHECK_EQ(c.r.status, Status::OK);
  CHECK(c.r.replayed);
  CHECK(c.r.id == f.made);
  // New work on the slot is refused, sequenced or not.
  CHECK_EQ(deposit(eng, f, kClient, next_seq++).r.status, refusal);
  CHECK_EQ(make_near(eng, f, kCreator, next_seq++).r.status, refusal);
  CHECK_EQ(deposit(eng, f, 0, 0).r.status, refusal);
  // Applied once.
  auto [version, balance] = stored(eng, f);
  CHECK_EQ(version, f.version);
  CHECK_EQ(balance, kStart + 2);
  std::printf("%-34s resends answered from sessions (latest replayed, older E_REPLAYED_OLD); new work %s\n",
              stage, status_name(refusal));
}

}  // namespace

int main() {
  register_type();
  g_dir = temp_dir();
  g_ports = free_ports(24000, 30000, 20);
  Fixture f;
  u64 next_seq = 3;
  {
    Engine eng(config(true));
    eng.start();
    OneResult a = one(eng, 0, [&](Completion cb, void* ctx) {
      CreateArgs args = account_args();
      create(kTypeId, ByteView(&args, sizeof(args)), {}, cb, ctx);
    });
    CHECK_EQ(a.r.status, Status::OK);
    f.acc = a.r.id;
    f.slot = slot_of(f.acc);
    f.own = SlotMap::make_default(kShards).owner_of(f.acc);
    CHECK_EQ(eng.cluster().group_of[f.slot], 0);

    CHECK_EQ(deposit(eng, f, kClient, 1).r.status, Status::OK);
    OneResult d = deposit(eng, f, kClient, 2);
    CHECK_EQ(d.r.status, Status::OK);
    CHECK(!d.r.replayed);
    f.version = d.r.version;
    OneResult c = make_near(eng, f, kCreator, 1);
    CHECK_EQ(c.r.status, Status::OK);
    CHECK(!c.r.replayed);
    f.made = c.r.id;
    CHECK_EQ(slot_of(f.made), f.slot);
    // Served: the resends are replayed and new work runs.
    CHECK(deposit(eng, f, kClient, 2).r.replayed);
    CHECK(make_near(eng, f, kCreator, 1).r.replayed);

    // Sealed for a move to group 1: E_MIGRATING.
    SlotReport rep;
    CHECK_EQ(eng.seal_slot(f.slot, 1, 1, eng.cluster().epoch + 1, rep), Status::OK);
    check_stage(eng, f, Status::E_MIGRATING, "sealed (E_MIGRATING):", next_seq);

    // The placement gives the slot to group 1: E_MOVED.
    std::array<u8, kSlots> slots = eng.cluster().group_of;
    slots[f.slot] = 1;
    CHECK(eng.update_placement(slots, eng.cluster().epoch + 1));
    check_stage(eng, f, Status::E_MOVED, "moved (E_MOVED):", next_seq);
    eng.stop();
  }
  {
    // Recovered from the data directory. The sessions and the seal come
    // back; the placement is the configured one (a real node fetches the
    // newer one from its witnesses, Config::placement_fetch), so the slot
    // is sealed again until the newer placement is adopted.
    Engine eng(config(false));
    eng.start();
    CHECK_EQ(eng.cluster().group_of[f.slot], 0);
    check_stage(eng, f, Status::E_MIGRATING, "restarted, sealed (E_MIGRATING):", next_seq);
    std::array<u8, kSlots> slots = eng.cluster().group_of;
    slots[f.slot] = 1;
    CHECK(eng.update_placement(slots, eng.cluster().epoch + 1));
    check_stage(eng, f, Status::E_MOVED, "restarted, moved (E_MOVED):", next_seq);
    eng.stop();
  }
  rm_rf(g_dir);
  std::printf("test_resend_refusals: ok\n");
  return 0;
}
