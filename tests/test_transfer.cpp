// Transfers between accounts (docs/TRANSFERS.md): money is conserved and
// every acknowledged transfer lands exactly once, in memory, durable, across
// a clean stop with transfers in flight, across a change of shard count, and
// across SIGKILL. A transfer to a missing account is refunded.
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>

#include "harness.h"
#include "snc/slot_map.h"
#include "snc/storage.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr i64 kStart = 1000;

Engine::Config config(u16 shards, const std::string& dir, bool init) {
  Engine::Config c;
  c.runtime.n_shards = shards;
  c.runtime.ring_slots = 64;  // small rings: exercise the overloaded retry path
  c.expected_objects = 4096;
  c.data_dir = dir;
  c.init = init;
  c.wal_segment_bytes = 1u << 20;
  return c;
}

std::vector<ObjectID> create_accounts(Engine& eng, u32 n) {
  std::vector<ObjectID> ids(n);
  u16 shards = eng.shard_count();
  u64 per = (n + shards - 1) / shards;
  std::mutex mu;
  run_all(
      eng, per,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        CreateArgs a{};
        a.balance_cents = kStart;
        a.created_ns = 1;
        a.email_len = 1;
        a.email[0] = 'e';
        a.name_len = 1;
        a.name[0] = 'n';
        (void)s;
        (void)i;
        create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [&](u16 s, u64 i, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        u64 k = s * per + i;
        std::lock_guard<std::mutex> g(mu);
        if (k < n) ids[k] = r.id;
      });
  return ids;
}

struct Snapshot {
  std::map<ObjectID, i64> balance;
  u64 entries = 0;
  i64 total = 0;
};

Snapshot snapshot(Engine& eng) {
  Snapshot s;
  for (u16 sh = 0; sh < eng.shard_count(); ++sh) {
    eng.for_each_object(sh, [&](const ObjectHeader& h) {
      const auto* a = reinterpret_cast<const AccountV1*>(h.payload());
      s.balance[h.id] = a->balance_cents;
      s.entries += a->n_out + a->n_in;
      s.total += a->balance_cents;
    });
  }
  return s;
}

// Random transfers from every shard; `net` gets the effect of every one
// that was acknowledged.
void transfers(Engine& eng, const std::vector<ObjectID>& ids, u64 per_shard, u32 seed,
               std::map<ObjectID, i64>& net, u64& ok, u64& rejected) {
  std::mutex mu;
  std::vector<std::mt19937_64> rng;
  for (u16 s = 0; s < eng.shard_count(); ++s) rng.emplace_back(seed + s);
  struct Pick {
    ObjectID from, to;
    i64 amount;
  };
  std::vector<std::vector<Pick>> picks(eng.shard_count());
  for (u16 s = 0; s < eng.shard_count(); ++s) {
    for (u64 i = 0; i < per_shard; ++i) {
      Pick p;
      p.from = ids[rng[s]() % ids.size()];
      do p.to = ids[rng[s]() % ids.size()];
      while (p.to == p.from);
      p.amount = 1 + static_cast<i64>(rng[s]() % 50);
      picks[s].push_back(p);
    }
  }
  run_all(
      eng, per_shard,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        const Pick& p = picks[s][i];
        TransferArgs t{p.to, p.amount, 7};
        execute(p.from, kOpTransfer, ByteView(&t, sizeof(t)), {}, c, ctx);
      },
      [&](u16 s, u64 i, const ExecResult& r) {
        const Pick& p = picks[s][i];
        std::lock_guard<std::mutex> g(mu);
        if (r.status == Status::OK) {
          ++ok;
          net[p.from] -= p.amount;
          net[p.to] += p.amount;
        } else if (r.status == Status::E_OVERLOADED) {
          ++rejected;  // the ring to the source's owner was full: never ran
        } else {
          if (r.status != Status::E_APP) std::fprintf(stderr, "transfer: %s\n", status_name(r.status));
          CHECK_EQ(r.status, Status::E_APP);
          CHECK(r.app_code == kAppInsufficientFunds || r.app_code == kAppBusy);
          ++rejected;
        }
      });
}

void verify(Engine& eng, const std::vector<ObjectID>& ids, const std::map<ObjectID, i64>& net) {
  CHECK(eng.wait_emits());
  Snapshot s = snapshot(eng);
  CHECK_EQ(s.entries, 0u);
  CHECK_EQ(s.total, kStart * static_cast<i64>(ids.size()));
  for (const ObjectID& id : ids) {
    auto it = net.find(id);
    CHECK_EQ(s.balance.at(id), kStart + (it == net.end() ? 0 : it->second));
  }
}

void test_memory() {
  Engine eng(config(4, "", false));
  eng.start();
  auto ids = create_accounts(eng, 200);
  std::map<ObjectID, i64> net;
  u64 ok = 0, rejected = 0;
  transfers(eng, ids, 5000, 1, net, ok, rejected);
  CHECK(ok > 10000);
  verify(eng, ids, net);

  // A transfer to an ID nobody owns is refunded.
  OneResult r = one(eng, 0, [&](Completion c, void* ctx) {
    TransferArgs t{ObjectID::make(999999, 1), 10, 7};
    execute(ids[0], kOpTransfer, ByteView(&t, sizeof(t)), {}, c, ctx);
  });
  CHECK_EQ(r.r.status, Status::OK);
  CHECK(eng.wait_emits());
  CHECK_EQ(snapshot(eng).balance.at(ids[0]), kStart + net[ids[0]]);
  CHECK(eng.stats(0).emits_failed_over + eng.stats(1).emits_failed_over +
            eng.stats(2).emits_failed_over + eng.stats(3).emits_failed_over >=
        1);

  // Bad args and self-transfers are rejected up front.
  r = one(eng, 0, [&](Completion c, void* ctx) {
    TransferArgs t{ids[0], 10, 7};
    execute(ids[0], kOpTransfer, ByteView(&t, sizeof(t)), {}, c, ctx);
  });
  CHECK_EQ(r.r.status, Status::E_BAD_ARGS);

  // Clients cannot call the follow-ups (a direct credit would mint money).
  r = one(eng, 0, [&](Completion c, void* ctx) {
    CreditArgs ca{1, 1000000};
    execute(ids[1], kOpXferCredit, ByteView(&ca, sizeof(ca)), {}, c, ctx);
  });
  CHECK_EQ(r.r.status, Status::E_BAD_ARGS);
  eng.stop();
  std::printf("memory: %llu transfers ok, %llu rejected\n", (unsigned long long)ok,
              (unsigned long long)rejected);
}

void test_durable() {
  char tmpl[] = "/tmp/snc-xfer-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::vector<ObjectID> ids;
  std::map<ObjectID, i64> net;
  u64 ok = 0, rejected = 0;
  {
    Engine eng(config(4, dir, true));
    eng.start();
    ids = create_accounts(eng, 200);
    transfers(eng, ids, 3000, 2, net, ok, rejected);
    verify(eng, ids, net);
    // Stop with transfers in flight: stop() pauses follow-ups, so entries
    // are left in the objects for resume to finish.
    transfers(eng, ids, 3000, 3, net, ok, rejected);
    eng.stop();
  }
  {
    Engine eng(config(4, dir, false));
    eng.start();
    u64 pending = snapshot(eng).entries;  // may already be settling
    std::printf("durable: restart found %llu entries in flight\n", (unsigned long long)pending);
    verify(eng, ids, net);
    transfers(eng, ids, 3000, 4, net, ok, rejected);
    eng.stop();
  }
  {
    // Shard-count change with entries in flight: they move with their objects.
    Engine eng(config(3, dir, false));
    eng.start();
    CHECK(eng.recovery().remapped);
    verify(eng, ids, net);
    transfers(eng, ids, 2000, 5, net, ok, rejected);
    eng.stop();
  }
  {
    Engine eng(config(4, dir, false));
    eng.start();
    verify(eng, ids, net);
    eng.stop();
  }
  std::printf("durable: %llu transfers ok, %llu rejected\n", (unsigned long long)ok,
              (unsigned long long)rejected);
}

// One shard owns everything, so every transfer is one op changing both
// accounts (OpCtx::local), logged as one atomic WAL group.
void test_local() {
  char tmpl[] = "/tmp/snc-xfer-local-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::vector<ObjectID> ids;
  std::map<ObjectID, i64> net;
  u64 ok = 0, rejected = 0;
  {
    Engine eng(config(1, dir, true));
    eng.start();
    ids = create_accounts(eng, 100);
    transfers(eng, ids, 20000, 6, net, ok, rejected);
    verify(eng, ids, net);
    CHECK_EQ(eng.stats(0).emits, 0u);  // no follow-ups: both sides in one op
    eng.stop();
  }
  {
    Engine eng(config(1, dir, false));
    eng.start();
    verify(eng, ids, net);
    // A transfer whose WAL group is cut before its last record is lost
    // whole: neither the credit (logged first) nor the debit survives.
    OneResult r = one(eng, 0, [&](Completion c, void* ctx) {
      TransferArgs t{ids[1], 5, 7};
      execute(ids[0], kOpTransfer, ByteView(&t, sizeof(t)), {}, c, ctx);
    });
    CHECK_EQ(r.r.status, Status::OK);
    eng.stop();
  }
  {
    // Tear the last record (the debit closing the group).
    std::vector<u64> segs = list_segments(dir, 0);
    std::string path = segment_path(dir, 0, segs.back());
    FILE* f = std::fopen(path.c_str(), "r+b");
    CHECK(f != nullptr);
    std::vector<u8> buf(1u << 20);
    std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::size_t off = kBlock, group_at = 0, last_at = 0;
    while (off + sizeof(WalRecord) <= n) {
      u32 len = check_record(buf.data() + off, n - off);
      if (len == 0) {
        std::size_t next = (off / kBlock + 1) * kBlock;
        if (off % kBlock == 0) break;
        off = next;
        continue;
      }
      WalRecord rec;
      std::memcpy(&rec, buf.data() + off, sizeof(rec));
      if (rec.flags & kRecGroupNext) group_at = off;
      last_at = off;
      off += len;
    }
    CHECK(group_at != 0 && last_at > group_at);
    u32 bad = 0xdeadbeef;
    std::fseek(f, static_cast<long>(last_at), SEEK_SET);
    CHECK_EQ(std::fwrite(&bad, 4, 1, f), 1u);
    std::fclose(f);
  }
  {
    Engine eng(config(1, dir, false));
    eng.start();
    verify(eng, ids, net);  // the torn transfer is absent from both accounts
    CHECK(eng.recovery().torn_tails >= 1);
    eng.stop();
  }
  std::printf("local: %llu transfers ok, %llu rejected\n", (unsigned long long)ok,
              (unsigned long long)rejected);
}

// create_near puts accounts in their group's slot, so transfers inside a
// group stay one op, under 4 shards and after a change to 3.
void test_placement() {
  char tmpl[] = "/tmp/snc-xfer-place-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  constexpr u32 kGroups = 20, kPer = 4;
  std::vector<std::vector<ObjectID>> groups(kGroups);
  std::map<ObjectID, i64> net;
  auto emits = [](Engine& eng) {
    u64 n = 0;
    for (u16 s = 0; s < eng.shard_count(); ++s) n += eng.stats(s).emits;
    return n;
  };
  auto run = [&](Engine& eng, u32 seed) {
    std::mt19937_64 rng(seed);
    u64 before = emits(eng);
    for (u32 i = 0; i < 400; ++i) {
      const auto& g = groups[rng() % kGroups];
      u32 a = rng() % kPer, b = (a + 1 + rng() % (kPer - 1)) % kPer;
      i64 amount = 1 + static_cast<i64>(rng() % 20);
      OneResult r = one(eng, static_cast<u16>(i % eng.shard_count()), [&](Completion c, void* ctx) {
        TransferArgs t{g[b], amount, 7};
        execute(g[a], kOpTransfer, ByteView(&t, sizeof(t)), {}, c, ctx);
      });
      if (r.r.status == Status::OK) {
        net[g[a]] -= amount;
        net[g[b]] += amount;
      } else {
        CHECK_EQ(r.r.status, Status::E_APP);
        CHECK_EQ(r.r.app_code, kAppInsufficientFunds);
      }
    }
    CHECK_EQ(emits(eng), before);  // every transfer was one op
  };
  std::vector<ObjectID> all;
  {
    Engine eng(config(4, dir, true));
    eng.start();
    for (u32 gi = 0; gi < kGroups; ++gi) {
      for (u32 k = 0; k < kPer; ++k) {
        OneResult r = one(eng, static_cast<u16>((gi + k) % 4), [&](Completion c, void* ctx) {
          CreateArgs a{};
          a.balance_cents = kStart;
          a.created_ns = 1;
          a.email_len = 1;
          a.email[0] = 'p';
          a.name_len = 1;
          a.name[0] = 'n';
          if (k == 0) {
            create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
          } else {
            create_near(groups[gi][0], kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
          }
        });
        CHECK_EQ(r.r.status, Status::OK);
        if (k > 0) CHECK_EQ(slot_of(r.r.id), slot_of(groups[gi][0]));
        groups[gi].push_back(r.r.id);
        all.push_back(r.r.id);
      }
    }
    run(eng, 11);
    verify(eng, all, net);
    eng.stop();
  }
  {
    Engine eng(config(3, dir, false));
    eng.start();
    CHECK(eng.recovery().remapped);
    verify(eng, all, net);
    run(eng, 12);
    verify(eng, all, net);
    eng.stop();
  }
  std::printf("placement: %u groups of %u, transfers inside groups stayed local\n", kGroups, kPer);
}

// The child runs transfers until killed. Without the acks we cannot know
// which in-flight transfers committed, so the parent checks conservation
// and that every exchange finishes; test_crash covers acknowledged
// durability for single-object ops.
[[noreturn]] void crash_child(const std::string& dir, bool init, u32 round, int ready_fd) {
  Engine eng(config(4, dir, init));
  eng.start();
  std::vector<ObjectID> ids;
  if (init) {
    ids = create_accounts(eng, 200);
  } else {
    for (u16 s = 0; s < eng.shard_count(); ++s) {
      eng.for_each_object(s, [&](const ObjectHeader& h) { ids.push_back(h.id); });
    }
  }
  char c = 1;
  if (::write(ready_fd, &c, 1) != 1) _exit(3);
  std::map<ObjectID, i64> net;
  u64 ok = 0, rejected = 0;
  for (u32 i = 0;; ++i) transfers(eng, ids, 2000, 100 * round + i, net, ok, rejected);
}

void test_crash() {
  char tmpl[] = "/tmp/snc-xfer-crash-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  for (u32 round = 0; round < 4; ++round) {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      ::close(fds[0]);
      crash_child(dir, round == 0, round, fds[1]);
    }
    ::close(fds[1]);
    char c;
    CHECK_EQ(::read(fds[0], &c, 1), 1);
    ::close(fds[0]);
    ::usleep(150000 + 70000 * round);
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st));

    Engine::Config rc = config(4, dir, false);
    rc.hold_emits = true;
    Engine eng(rc);
    eng.start();
    u64 pending = snapshot(eng).entries;
    // The recovered state already conserves money: balances plus what is
    // in flight (outbox entries the destination has not credited).
    {
      std::map<ObjectID, std::vector<u8>> objs;
      for (u16 sh = 0; sh < eng.shard_count(); ++sh) {
        eng.for_each_object(sh, [&](const ObjectHeader& h) {
          objs[h.id].assign(h.payload(), h.payload() + h.length);
        });
      }
      std::set<std::pair<ObjectID, u64>> credited;  // (from, seq) per destination, below
      std::map<ObjectID, std::set<std::pair<ObjectID, u64>>> inbox_of;
      i64 total = 0, flying = 0;
      for (auto& [id, p] : objs) {
        const auto* a = reinterpret_cast<const AccountV1*>(p.data());
        const auto* in = reinterpret_cast<const InEntry*>(reinterpret_cast<const OutEntry*>(a + 1) + a->n_out);
        for (u16 i = 0; i < a->n_in; ++i) {
          if (in[i].ok) inbox_of[id].insert({in[i].from, in[i].seq});
        }
        total += a->balance_cents;
      }
      for (auto& [id, p] : objs) {
        const auto* a = reinterpret_cast<const AccountV1*>(p.data());
        const auto* out = reinterpret_cast<const OutEntry*>(a + 1);
        for (u16 i = 0; i < a->n_out; ++i) {
          if (!inbox_of[out[i].to].count({id, out[i].seq})) flying += out[i].amount;
        }
      }
      if (total + flying != kStart * 200) {
        std::fprintf(stderr, "round %u: recovered state: balances %lld + in flight %lld != %lld\n", round,
                     (long long)total, (long long)flying, (long long)(kStart * 200));
      }
      CHECK_EQ(total + flying, kStart * 200);
    }
    eng.release_emits();
    CHECK(eng.wait_emits());
    Snapshot s = snapshot(eng);
    std::printf("crash round %u: %llu entries in flight at recovery\n", round,
                (unsigned long long)pending);
    CHECK_EQ(s.balance.size(), 200u);
    CHECK_EQ(s.entries, 0u);
    CHECK_EQ(s.total, kStart * 200);
    eng.stop();
  }
  std::printf("crash: 4 rounds conserved\n");
}

}  // namespace

int main() {
  register_type();
  test_memory();
  test_durable();
  test_local();
  test_placement();
  test_crash();
  std::printf("test_transfer: ok\n");
  return 0;
}
