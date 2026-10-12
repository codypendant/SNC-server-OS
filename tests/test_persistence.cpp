// WAL, checkpoint and recovery: clean restarts, restart after a checkpoint,
// restart with a different shard count (slot remap), index rebuild, deletes
// and mint-lease uniqueness across restarts.
#include <stdlib.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <set>
#include <string>

#include "harness.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

struct Expect {
  u64 version;
  i64 balance;
};

std::string g_dir;
std::vector<ObjectID> g_ids;            // creation order
std::map<ObjectID, Expect> g_expect;    // live objects
std::set<ObjectID> g_deleted;
std::map<std::string, ObjectID> g_email;

Engine::Config config(u16 shards, bool init) {
  Engine::Config c;
  c.runtime.n_shards = shards;
  c.runtime.ring_slots = 64;
  c.expected_objects = 20000;
  c.data_dir = g_dir;
  c.init = init;
  c.wal_segment_bytes = 1u << 20;  // small segments: exercise rollover and deletion
  return c;
}

CreateArgs account_args(const std::string& email, i64 balance) {
  CreateArgs a{};
  a.balance_cents = balance;
  a.created_ns = 1;
  a.email_len = static_cast<u8>(email.size());
  std::memcpy(a.email, email.data(), email.size());
  a.name_len = 1;
  a.name[0] = 'x';
  return a;
}

void verify(Engine& eng) {
  std::vector<ObjectID> all(g_ids.begin(), g_ids.end());
  u16 n = eng.shard_count();
  u64 per = (all.size() + n - 1) / n;
  std::mutex mu;
  u64 checked = 0;
  run_all(
      eng, per,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        u64 k = s * per + i;
        ObjectID id = k < all.size() ? all[k] : ObjectID::make(999999, 1);
        execute(id, kOpGetBalance, {}, {}, c, ctx);
      },
      [&](u16 s, u64 i, const ExecResult& r) {
        u64 k = s * per + i;
        if (k >= all.size()) {
          CHECK_EQ(r.status, Status::E_NOT_FOUND);
          return;
        }
        ObjectID id = all[k];
        std::lock_guard<std::mutex> g(mu);
        ++checked;
        if (g_deleted.count(id)) {
          CHECK_EQ(r.status, Status::E_NOT_FOUND);
          return;
        }
        CHECK_EQ(r.status, Status::OK);
        CHECK(r.durable);
        const Expect& e = g_expect.at(id);
        CHECK_EQ(r.version, e.version);
        BalanceResult b;
        std::memcpy(&b, r.result.data, sizeof(b));
        CHECK_EQ(b.balance_cents, e.balance);
      });
  CHECK_EQ(checked, all.size());
  u64 objects = 0;
  for (u16 s = 0; s < n; ++s) objects += eng.stats(s).objects;
  CHECK_EQ(objects, g_expect.size());
  // Index: every live email resolves to exactly its object, from any shard.
  int k = 0;
  for (const auto& [email, id] : g_email) {
    if (++k % 97) continue;  // sample
    auto got = lookup(eng, static_cast<u16>(k % n), kEmailIndex, email);
    if (g_deleted.count(id)) {
      CHECK(got.empty());
    } else {
      CHECK_EQ(got.size(), 1u);
      CHECK(got[0] == id);
    }
  }
}

void deposit_all(Engine& eng, const std::vector<ObjectID>& ids, i64 amount) {
  u16 n = eng.shard_count();
  u64 per = (ids.size() + n - 1) / n;
  std::mutex mu;
  run_all(
      eng, per,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        u64 k = std::min<u64>(s * per + i, ids.size() - 1);
        AmountArgs a{amount, 2};
        execute(ids[k], kOpDeposit, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [&](u16 s, u64 i, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        CHECK(r.durable);  // DURABLE is the default: completion waited for the WAL
        std::lock_guard<std::mutex> g(mu);
        Expect& e = g_expect.at(r.id);
        (void)s;
        (void)i;
        e.version = std::max(e.version, r.version);
      });
  // Recompute expected balances from versions: every deposit here adds `amount`.
  (void)amount;
}

}  // namespace

int main() {
  register_type();
  char tmpl[] = "/tmp/snc-persist-XXXXXX";
  g_dir = mkdtemp(tmpl);

  // Starting without init on an empty directory is refused.
  {
    Engine eng(config(4, false));
    bool threw = false;
    try {
      eng.start();
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }

  // A. Fresh space: create, mutate, re-key, delete.
  std::set<u64> leases;
  {
    Engine eng(config(4, true));
    eng.start();
    CHECK(eng.recovery().fresh);
    std::mutex mu;
    run_all(
        eng, 2000,
        [&](u16 s, u64 i, Completion c, void* ctx) {
          std::string email = "a" + std::to_string(s * 2000 + i) + "@x";
          CreateArgs a = account_args(email, 100);
          create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
        },
        [&](u16 s, u64 i, const ExecResult& r) {
          CHECK_EQ(r.status, Status::OK);
          CHECK(r.durable);
          std::lock_guard<std::mutex> g(mu);
          g_ids.push_back(r.id);
          g_expect[r.id] = Expect{1, 100};
          g_email["a" + std::to_string(s * 2000 + i) + "@x"] = r.id;
          leases.insert(r.id.lease());
        });
    CHECK_EQ(g_ids.size(), 8000u);

    deposit_all(eng, g_ids, 5);
    for (auto& [id, e] : g_expect) e.balance = 100 + 5 * static_cast<i64>(e.version - 1);

    // Re-key 100 emails: the index must follow.
    for (int i = 0; i < 100; ++i) {
      std::string old_email = "a" + std::to_string(i) + "@x";
      ObjectID id = g_email.at(old_email);
      SetEmailArgs a{};
      std::string ne = "new" + std::to_string(i) + "@x";
      a.email_len = static_cast<u8>(ne.size());
      std::memcpy(a.email, ne.data(), ne.size());
      auto res = one(eng, static_cast<u16>(i % 4), [&](Completion c, void* ctx) {
        execute(id, kOpSetEmail, ByteView(&a, sizeof(a)), {}, c, ctx);
      });
      CHECK_EQ(res.r.status, Status::OK);
      g_expect.at(id).version = res.r.version;
      g_email.erase(old_email);
      g_email[ne] = id;
    }
    CHECK(lookup(eng, 1, kEmailIndex, "a3@x").empty());
    auto got = lookup(eng, 2, kEmailIndex, "new3@x");
    CHECK(got.size() == 1 && got[0] == g_email.at("new3@x"));

    // Delete 50 accounts (empty them first).
    for (int i = 200; i < 250; ++i) {
      ObjectID id = g_ids[i];
      AmountArgs all{g_expect.at(id).balance, 3};
      auto w = one(eng, 0, [&](Completion c, void* ctx) {
        execute(id, kOpWithdraw, ByteView(&all, sizeof(all)), {}, c, ctx);
      });
      CHECK_EQ(w.r.status, Status::OK);
      auto d = one(eng, 1, [&](Completion c, void* ctx) { remove(id, {}, c, ctx); });
      CHECK_EQ(d.r.status, Status::OK);
      CHECK(d.r.durable);
      g_expect.erase(id);
      g_deleted.insert(id);
    }
    verify(eng);
    eng.stop();
    ShardStats st = eng.stats(0);
    CHECK(st.wal.records > 0);
    CHECK(st.wal.segments >= 1);
    std::printf("A: wal records(shard0) %llu flushes %llu written %llu B direct=%d\n",
                (unsigned long long)st.wal.records, (unsigned long long)st.wal.flushes,
                (unsigned long long)st.wal.written_bytes, st.wal_direct);
  }

  // B. Clean restart: everything comes back from the WAL alone.
  {
    Engine eng(config(4, false));
    eng.start();
    const RecoveryReport& rr = eng.recovery();
    CHECK(!rr.fresh && !rr.remapped);
    CHECK(rr.wal_records_replayed > 0);
    CHECK_EQ(rr.objects, g_expect.size());
    std::printf("B: recovered %llu objects, %llu WAL records in %.3fs\n",
                (unsigned long long)rr.objects, (unsigned long long)rr.wal_records_replayed,
                rr.total_s);
    verify(eng);

    // New IDs never collide with old ones: fresh leases (spec C12).
    auto res = one(eng, 0, [&](Completion c, void* ctx) {
      CreateArgs a = account_args("late@x", 7);
      create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
    });
    CHECK_EQ(res.r.status, Status::OK);
    CHECK(!leases.count(res.r.id.lease()));
    g_ids.push_back(res.r.id);
    g_expect[res.r.id] = Expect{1, 7};
    g_email["late@x"] = res.r.id;

    CheckpointReport cp = eng.checkpoint();
    CHECK_EQ(cp.objects, g_expect.size());
    std::printf("B: checkpoint %llu: %llu objects, %llu bytes, %llu segments deleted, %.3fs\n",
                (unsigned long long)cp.seq, (unsigned long long)cp.objects,
                (unsigned long long)cp.bytes, (unsigned long long)cp.segments_deleted, cp.seconds);

    // Writes after the checkpoint must replay on top of it.
    std::vector<ObjectID> some(g_ids.begin() + 1000, g_ids.begin() + 1200);
    deposit_all(eng, some, 1);
    for (const ObjectID& id : some) g_expect.at(id).balance += 1;
    verify(eng);
    eng.stop();
  }

  // C. Restart with 3 shards: slot map rebuilt, objects move owners, a
  // checkpoint is forced before READY (spec C9).
  {
    Engine eng(config(3, false));
    eng.start();
    const RecoveryReport& rr = eng.recovery();
    CHECK(rr.remapped);
    CHECK(rr.images_loaded > 0);
    CHECK(rr.final_checkpoint_s > 0);
    std::printf("C: remapped to 3 shards: %llu images + %llu WAL records in %.3fs\n",
                (unsigned long long)rr.images_loaded, (unsigned long long)rr.wal_records_replayed,
                rr.total_s);
    verify(eng);
    eng.stop();
  }
  // The orphaned stream of shard 3 is gone after that checkpoint.
  CHECK(list_streams(g_dir).size() == 3);

  // D. Back to 4 shards.
  {
    Engine eng(config(4, false));
    eng.start();
    CHECK(eng.recovery().remapped);
    verify(eng);
    eng.stop();
  }

  // E. A torn tail: garbage after the last record of the newest segment (a
  // partly written group commit that was never acknowledged) is cut off, and
  // the stream continues in a new segment.
  {
    std::vector<u64> segs = list_segments(g_dir, 0);
    CHECK(!segs.empty());
    std::string path = segment_path(g_dir, 0, segs.back());
    FILE* f = std::fopen(path.c_str(), "r+b");
    CHECK(f != nullptr);
    std::vector<u8> block(4096);
    long off = 4096;  // skip the file header block
    for (;; off += 4096) {
      CHECK_EQ(std::fseek(f, off, SEEK_SET), 0);
      if (std::fread(block.data(), 1, block.size(), f) != block.size()) break;
      bool zero = true;
      for (u8 b : block) zero &= b == 0;
      if (zero) break;
    }
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<u8>(i * 131 + 7);
    CHECK_EQ(std::fseek(f, off, SEEK_SET), 0);
    CHECK_EQ(std::fwrite(block.data(), 1, block.size(), f), block.size());
    std::fclose(f);

    Engine eng(config(4, false));
    eng.start();
    CHECK(eng.recovery().torn_tails >= 1);
    verify(eng);
    std::vector<ObjectID> some(g_ids.begin() + 300, g_ids.begin() + 700);
    deposit_all(eng, some, 2);
    for (const ObjectID& id : some) g_expect.at(id).balance += 2;
    eng.stop();
  }
  {
    Engine eng(config(4, false));
    eng.start();
    verify(eng);
    eng.stop();
  }

  std::string cmd = "rm -rf " + g_dir;
  CHECK_EQ(std::system(cmd.c_str()), 0);
  std::printf("test_persistence: ok\n");
  return 0;
}
