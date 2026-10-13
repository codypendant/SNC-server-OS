// Replication (docs/REPLICATION.md): a standby's WAL matches its primary's
// record for record; a write acknowledged under sync replication survives
// SIGKILL of the primary and promotion of the standby, with transfers
// conserved and new IDs not colliding with replicated ones; a standby that
// starts late catches up; a sync primary whose standby is gone continues
// after the timeout; a standby that needs records no longer retained is
// reported lost. Every engine runs in a forked child (one engine per
// process); the parent only forks, waits and compares.
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
#include <latch>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>

#include "harness.h"
#include "snc/storage.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr u16 kShards = 4;
constexpr i64 kStart = 1000;
const Uuid kUuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
u16 g_port = 0;

void rm_rf(const std::string& dirs) { CHECK_EQ(std::system(("rm -rf " + dirs).c_str()), 0); }

std::string temp_dir(const char* tag) {
  std::string t = std::string("/tmp/snc-repl-") + tag + "-XXXXXX";
  std::vector<char> b(t.begin(), t.end());
  b.push_back(0);
  return mkdtemp(b.data());
}

Engine::Config config(const std::string& dir, bool init) {
  Engine::Config c;
  c.runtime.n_shards = kShards;
  c.runtime.ring_slots = 64;
  c.expected_objects = 4096;
  c.data_dir = dir;
  c.init = init;
  c.init_uuid = kUuid;
  c.wal_segment_bytes = 1u << 20;
  return c;
}

Engine::Config primary_config(const std::string& dir, bool init, bool sync, u32 timeout_ms) {
  Engine::Config c = config(dir, init);
  c.replica_host = "127.0.0.1";
  c.replica_port = g_port;
  c.replica_sync = sync;
  c.replica_timeout_ms = timeout_ms;
  return c;
}

// Blocks SIGTERM so the engine's threads inherit the mask; the child's main
// thread takes it with sigwait.
sigset_t block_term() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  return set;
}

// A standby in a child process; stops cleanly on SIGTERM. Writes one byte
// to ready_fd once listening.
pid_t start_standby(const std::string& dir, bool init, const std::string& seed_from = "",
                    u16 port = 0, u32 lease_ms = 0) {
  int fds[2];
  CHECK_EQ(::pipe(fds), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(fds[0]);
    sigset_t set = block_term();
    Engine::Config c = config(dir, init);
    c.standby = true;
    c.standby_host = "127.0.0.1";
    c.replica_port = port ? port : g_port;
    c.seed_from = seed_from;
    c.standby_read_lease_ms = lease_ms;
    Engine eng(c);
    eng.start();
    // It serves nothing (without reads on).
    OneResult r = one(eng, 0, [&](Completion d, void* ctx) {
      execute(ObjectID{}, kOpGetBalance, {}, {}, d, ctx);
    });
    if (r.r.status != (lease_ms ? Status::E_STALE : Status::E_STANDBY)) _exit(6);
    char b = 1;
    if (::write(fds[1], &b, 1) != 1) _exit(3);
    int sig = 0;
    sigwait(&set, &sig);
    eng.stop();
    _exit(0);
  }
  ::close(fds[1]);
  char b;
  CHECK_EQ(::read(fds[0], &b, 1), 1);
  ::close(fds[0]);
  return pid;
}

void stop_child(pid_t pid) {
  ::kill(pid, SIGTERM);
  int st = 0;
  CHECK_EQ(::waitpid(pid, &st, 0), pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

void wait_ok(pid_t pid) {
  int st = 0;
  CHECK_EQ(::waitpid(pid, &st, 0), pid);
  if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
    std::fprintf(stderr, "child status %d (exit %d, signal %d)\n", st,
                 WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st) ? WTERMSIG(st) : -1);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

std::vector<ObjectID> create_accounts(Engine& eng, u32 n) {
  std::vector<ObjectID> ids(n);
  u64 per = (n + kShards - 1) / kShards;
  std::mutex mu;
  run_all(
      eng, per,
      [&](u16, u64, Completion c, void* ctx) {
        CreateArgs a{};
        a.balance_cents = kStart;
        a.created_ns = 1;
        a.email_len = 1;
        a.email[0] = 'e';
        create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [&](u16 s, u64 i, const ExecResult& r) {
        if (r.status != Status::OK || !r.durable) _exit(4);
        u64 k = s * per + i;
        std::lock_guard<std::mutex> g(mu);
        if (k < n) ids[k] = r.id;
      });
  return ids;
}

// Random transfers among `ids` and deposits into `dep`; calls `acked` with
// every durable deposit result.
void workload(Engine& eng, const std::vector<ObjectID>& ids, const std::vector<ObjectID>& dep,
              u64 per_shard, u32 seed, const std::function<void(const ExecResult&)>& acked) {
  std::vector<std::mt19937_64> rng;
  for (u16 s = 0; s < kShards; ++s) rng.emplace_back(seed * 31 + s);
  struct Pick {
    bool deposit;
    ObjectID from, to;
    i64 amount;
  };
  std::vector<std::vector<Pick>> picks(kShards);
  for (u16 s = 0; s < kShards; ++s) {
    for (u64 i = 0; i < per_shard; ++i) {
      Pick p;
      p.deposit = i % 2 == 0;
      if (p.deposit) {
        p.from = dep[rng[s]() % dep.size()];
      } else {
        p.from = ids[rng[s]() % ids.size()];
        do p.to = ids[rng[s]() % ids.size()];
        while (p.to == p.from);
      }
      p.amount = 1 + static_cast<i64>(rng[s]() % 50);
      picks[s].push_back(p);
    }
  }
  run_all(
      eng, per_shard,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        const Pick& p = picks[s][i];
        if (p.deposit) {
          AmountArgs a{1, i};
          execute(p.from, kOpDeposit, ByteView(&a, sizeof(a)), {}, c, ctx);
        } else {
          TransferArgs t{p.to, p.amount, 7};
          execute(p.from, kOpTransfer, ByteView(&t, sizeof(t)), {}, c, ctx);
        }
      },
      [&](u16 s, u64 i, const ExecResult& r) {
        if (picks[s][i].deposit) {
          if (r.status == Status::OK && r.durable) acked(r);
          else if (r.status != Status::E_OVERLOADED) _exit(5);
        }
      });
}

// Waits until every shard's standby is connected and has acked all of its
// primary's durable records.
void wait_replicated(Engine& eng) {
  for (int i = 0; i < 20000; ++i) {
    bool all = true;
    for (u16 s = 0; s < kShards; ++s) {
      ShardStats st = eng.stats(s);
      all &= st.repl_live && st.repl_acked_lsn >= st.wal_durable_lsn;
    }
    if (all) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _exit(7);
}

// (lsn, crc) of every record of every stream.
std::vector<std::vector<std::pair<u64, u32>>> log_of(const std::string& dir) {
  std::vector<std::vector<std::pair<u64, u32>>> v(kShards);
  for (u16 s = 0; s < kShards; ++s) {
    scan_stream(dir, s, 0, kUuid,
                [&](const WalRecord& h, const u8*) { v[s].push_back({h.lsn, h.crc32c}); });
  }
  return v;
}

// The standby's log may start later than the primary's (its own checkpoint
// can truncate what it received); where both have a record, it is the same.
bool same_log(const std::vector<std::pair<u64, u32>>& p, const std::vector<std::pair<u64, u32>>& s) {
  if (s.empty()) return p.empty();
  if (p.empty() || s.back() != p.back() || s.size() > p.size()) return false;
  return std::equal(s.begin(), s.end(), p.end() - static_cast<long>(s.size()));
}

// Every object's (version, balance), read by recovering `dir` in a child.
std::map<ObjectID, std::pair<u64, i64>> state_of(const std::string& dir) {
  struct Row {
    ObjectID id;
    u64 version;
    i64 balance;
  };
  int fds[2];
  CHECK_EQ(::pipe(fds), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(fds[0]);
    Engine::Config c = config(dir, false);
    c.hold_emits = true;  // the state as recovered, before resumed follow-ups change it
    Engine eng(c);
    eng.start();
    for (u16 s = 0; s < kShards; ++s) {
      eng.for_each_object(s, [&](const ObjectHeader& h) {
        Row r{h.id, h.version, reinterpret_cast<const AccountV1*>(h.payload())->balance_cents};
        if (::write(fds[1], &r, sizeof(r)) != sizeof(r)) _exit(3);
      });
    }
    eng.stop();
    _exit(0);
  }
  ::close(fds[1]);
  std::map<ObjectID, std::pair<u64, i64>> m;
  Row r;
  while (::read(fds[0], &r, sizeof(r)) == sizeof(r)) m[r.id] = {r.version, r.balance};
  ::close(fds[0]);
  wait_ok(pid);
  return m;
}

u64 total_records(const std::vector<std::vector<std::pair<u64, u32>>>& v) {
  u64 n = 0;
  for (const auto& s : v) n += s.size();
  return n;
}

// 1. A clean run: the standby's log equals the primary's.
void test_identical() {
  std::string pd = temp_dir("p"), sd = temp_dir("s");
  pid_t sb = start_standby(sd, true);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine eng(primary_config(pd, true, true, 0));
    eng.start();
    auto ids = create_accounts(eng, 200);
    auto dep = create_accounts(eng, 64);
    for (u32 r = 0; r < 3; ++r) workload(eng, ids, dep, 2000, r, [](const ExecResult&) {});
    if (!eng.wait_emits()) _exit(8);
    wait_replicated(eng);
    eng.stop();
    _exit(0);
  }
  wait_ok(pid);
  stop_child(sb);
  auto a = log_of(pd), b = log_of(sd);
  for (u16 s = 0; s < kShards; ++s) CHECK(same_log(a[s], b[s]));
  CHECK(state_of(pd) == state_of(sd));
  std::printf("identical: logs agree, objects equal (%llu records)\n",
              (unsigned long long)total_records(a));
  rm_rf(pd + " " + sd);
}

struct Ack {
  ObjectID id;
  u64 version;
};

// 2. SIGKILL the primary mid-workload, promote the standby: every deposit
// acknowledged under sync replication is there, transfers are conserved,
// and the promoted primary mints IDs that collide with nothing replicated.
void test_failover() {
  for (u32 round = 0; round < 3; ++round) {
    std::string pd = temp_dir("p"), sd = temp_dir("s");
    pid_t sb = start_standby(sd, true);
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      ::close(fds[0]);
      Engine eng(primary_config(pd, true, true, 0));
      eng.start();
      auto ids = create_accounts(eng, 200);
      auto dep = create_accounts(eng, 64);
      for (u32 i = 0;; ++i) {
        workload(eng, ids, dep, 1000, 100 * round + i, [&](const ExecResult& r) {
          Ack a{r.id, r.version};
          if (::write(fds[1], &a, sizeof(a)) != sizeof(a)) _exit(3);
        });
      }
    }
    ::close(fds[1]);
    std::map<ObjectID, u64> acked;
    u64 got = 0, kill_after = 3000 + 4111ull * round;
    bool killed = false;
    Ack a;
    for (;;) {
      ssize_t n = ::read(fds[0], &a, sizeof(a));
      if (n == 0) break;
      CHECK_EQ(n, static_cast<ssize_t>(sizeof(a)));
      ++got;
      acked[a.id] = std::max(acked[a.id], a.version);
      if (!killed && got >= kill_after) {
        ::kill(pid, SIGKILL);
        killed = true;
      }
    }
    ::close(fds[0]);
    int st = 0;
    CHECK_EQ(::waitpid(pid, &st, 0), pid);
    CHECK(WIFSIGNALED(st));
    stop_child(sb);

    // Promote: start the standby's directory as a primary.
    pid_t vp = ::fork();
    CHECK(vp >= 0);
    if (vp == 0) {
      Engine eng(config(sd, false));
      eng.start();
      if (!eng.wait_emits()) _exit(8);
      std::map<ObjectID, std::pair<u64, i64>> seen;
      i64 total = 0;
      u64 entries = 0;
      for (u16 s = 0; s < kShards; ++s) {
        eng.for_each_object(s, [&](const ObjectHeader& h) {
          const auto* acc = reinterpret_cast<const AccountV1*>(h.payload());
          seen[h.id] = {h.version, acc->balance_cents};
          entries += acc->n_out + acc->n_in;
          if (!acked.count(h.id)) total += acc->balance_cents;  // transfer accounts
        });
      }
      CHECK_EQ(entries, 0u);
      u64 xfer_accounts = 0;
      for (const auto& [id, vb] : seen) xfer_accounts += !acked.count(id);
      // Deposit accounts never receive transfers: balance = start + deposits.
      for (const auto& [id, v] : acked) {
        auto it = seen.find(id);
        CHECK(it != seen.end());
        CHECK(it->second.first >= v);
        CHECK_EQ(it->second.second, kStart + static_cast<i64>(it->second.first - 1));
      }
      // Conservation over the 200 transfer accounts. Every one of the 64
      // deposit accounts has an acked deposit by the kill point, so the
      // accounts without one are exactly the transfer accounts.
      CHECK_EQ(xfer_accounts, 200u);
      CHECK_EQ(total, kStart * 200);
      // New IDs from the promoted primary collide with nothing replicated.
      auto fresh = create_accounts(eng, 400);
      for (const ObjectID& id : fresh) CHECK(!seen.count(id));
      std::printf("failover round %u: %zu deposit accounts at or past acked versions, "
                  "200 transfer accounts conserved, 400 new IDs unique\n",
                  round, acked.size());
      std::fflush(stdout);
      eng.stop();
      _exit(0);
    }
    wait_ok(vp);
    rm_rf(pd + " " + sd);
  }
}

// 3. A standby that starts after the primary has written catches up: from
// memory (LATE), from WAL files after a checkpoint (DISK), or from a seed of
// the primary's checkpoint when the WAL it needs is gone (SEED); without a
// seed it is reported lost (LOST).
enum class Late { LATE, DISK, LOST, SEED, SLOW };  // SLOW: DISK, rate-limited

void test_late(Late mode) {
  std::string pd = temp_dir("p"), sd = temp_dir("s");
  const u16 seed_port = static_cast<u16>(g_port + 100);
  int go[2], ready[2];
  CHECK_EQ(::pipe(go), 0);
  CHECK_EQ(::pipe(ready), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(go[1]);
    ::close(ready[0]);
    Engine::Config c = primary_config(pd, true, /*sync=*/false, 0);
    if (mode != Late::LATE) c.replica_retain_bytes = 64u << 10;  // memory holds almost nothing
    if (mode == Late::LOST || mode == Late::SEED) c.replica_retain_disk_bytes = 0;
    if (mode == Late::SEED) c.seed_port = seed_port;
    const u64 rate = 1u << 20;
    if (mode == Late::SLOW) c.replica_catchup_bytes_per_s = rate;
    Engine eng(c);
    eng.start();
    auto ids = create_accounts(eng, 200);
    auto dep = create_accounts(eng, 64);
    workload(eng, ids, dep, 2000, 1, [](const ExecResult&) {});
    if (!eng.wait_emits()) _exit(8);
    if (mode != Late::LATE) {
      eng.checkpoint();  // covers what was written so far
      workload(eng, ids, dep, 1000, 3, [](const ExecResult&) {});
      if (!eng.wait_emits()) _exit(8);
    }
    char b = 1;
    if (::write(ready[1], &b, 1) != 1) _exit(3);
    if (::read(go[0], &b, 1) != 1) _exit(3);  // the standby is up
    const auto t0 = std::chrono::steady_clock::now();
    if (mode == Late::LOST) {
      for (int i = 0; i < 5000; ++i) {
        bool all = true;
        for (u16 s = 0; s < kShards; ++s) all &= eng.stats(s).repl_lost;
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      for (u16 s = 0; s < kShards; ++s) {
        if (!eng.stats(s).repl_lost) _exit(9);
      }
    } else {
      workload(eng, ids, dep, 500, 2, [](const ExecResult&) {});  // live on top of catch-up
      if (!eng.wait_emits()) _exit(8);
      wait_replicated(eng);
      if (mode != Late::LATE) {
        u64 disk = 0, most = 0;
        for (u16 s = 0; s < kShards; ++s) {
          disk += eng.stats(s).repl_disk_bytes;
          most = std::max(most, eng.stats(s).repl_disk_bytes);
        }
        if (disk == 0) _exit(13);  // it did read WAL files
        if (mode == Late::SLOW) {
          // Each shard sent at no more than the rate (less a 64 KiB burst).
          double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
          double floor = static_cast<double>(most - (64u << 10)) / rate;
          std::printf("late slow: %.1f MiB from disk on the busiest shard in %.2fs (at least %.2fs)\n",
                      most / 1048576.0, secs, floor);
          if (secs < floor) _exit(14);
        }
      }
    }
    eng.stop();
    _exit(0);
  }
  ::close(go[0]);
  ::close(ready[1]);
  char b;
  CHECK_EQ(::read(ready[0], &b, 1), 1);
  pid_t sb = mode == Late::SEED
                 ? start_standby(sd, false, "127.0.0.1:" + std::to_string(seed_port))
                 : start_standby(sd, true);
  CHECK_EQ(::write(go[1], &b, 1), 1);
  wait_ok(pid);
  stop_child(sb);
  const char* name[] = {"late", "disk", "lost", "seed", "slow"};
  if (mode == Late::LOST) {
    CHECK_EQ(total_records(log_of(sd)), 0u);
    std::printf("lost: a standby needing WAL that is gone is refused on every shard\n");
  } else {
    auto a = log_of(pd), s = log_of(sd);
    for (u16 i = 0; i < kShards; ++i) CHECK(same_log(a[i], s[i]));
    CHECK(state_of(pd) == state_of(sd));
    std::printf("%s: the standby matches the primary (%llu records on the standby)\n",
                name[static_cast<int>(mode)], (unsigned long long)total_records(s));
  }
  rm_rf(pd + " " + sd);
}

// 4. A standby down while its primary writes and checkpoints past what
// memory holds catches up from the primary's WAL files when it returns.
void test_outage() {
  std::string pd = temp_dir("p"), sd = temp_dir("s");
  pid_t sb = start_standby(sd, true);
  int go[2], ready[2];
  CHECK_EQ(::pipe(go), 0);
  CHECK_EQ(::pipe(ready), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(go[1]);
    ::close(ready[0]);
    Engine::Config c = primary_config(pd, true, /*sync=*/false, 0);
    c.replica_retain_bytes = 64u << 10;
    Engine eng(c);
    eng.start();
    auto ids = create_accounts(eng, 200);
    auto dep = create_accounts(eng, 64);
    workload(eng, ids, dep, 1000, 1, [](const ExecResult&) {});
    if (!eng.wait_emits()) _exit(8);
    wait_replicated(eng);
    char b = 1;
    if (::write(ready[1], &b, 1) != 1) _exit(3);
    if (::read(go[0], &b, 1) != 1) _exit(3);  // the standby was killed
    workload(eng, ids, dep, 1500, 2, [](const ExecResult&) {});
    if (!eng.wait_emits()) _exit(8);
    eng.checkpoint();  // must keep the segments the standby lacks
    workload(eng, ids, dep, 500, 3, [](const ExecResult&) {});
    if (!eng.wait_emits()) _exit(8);
    if (::write(ready[1], &b, 1) != 1) _exit(3);
    if (::read(go[0], &b, 1) != 1) _exit(3);  // the standby is back
    wait_replicated(eng);
    eng.stop();
    _exit(0);
  }
  ::close(go[0]);
  ::close(ready[1]);
  char b = 1;
  CHECK_EQ(::read(ready[0], &b, 1), 1);
  ::kill(sb, SIGKILL);
  int st = 0;
  CHECK_EQ(::waitpid(sb, &st, 0), sb);
  CHECK_EQ(::write(go[1], &b, 1), 1);
  CHECK_EQ(::read(ready[0], &b, 1), 1);
  sb = start_standby(sd, false);
  CHECK_EQ(::write(go[1], &b, 1), 1);
  wait_ok(pid);
  stop_child(sb);
  auto a = log_of(pd), s = log_of(sd);
  for (u16 i = 0; i < kShards; ++i) CHECK(same_log(a[i], s[i]));
  CHECK(state_of(pd) == state_of(sd));
  std::printf("outage: standby killed, primary checkpointed, standby caught up from WAL files\n");
  rm_rf(pd + " " + sd);
}

// Every account, as 64 to deposit into and the rest to transfer between.
void split_accounts(Engine& eng, std::vector<ObjectID>& ids, std::vector<ObjectID>& dep) {
  std::vector<ObjectID> all;
  for (u16 s = 0; s < kShards; ++s) {
    eng.for_each_object(s, [&](const ObjectHeader& h) { all.push_back(h.id); });
  }
  dep.assign(all.end() - 64, all.end());
  ids.assign(all.begin(), all.end() - 64);
}

// Runs a primary on `pd` in a child: `per_shard` ops of workload, then
// either waits until its standby is replicated (expect_lost false) or until
// every shard reports it lost.
void run_primary(const std::string& pd, bool init, u64 per_shard, u32 seed, bool expect_lost) {
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine eng(primary_config(pd, init, /*sync=*/false, 0));
    eng.start();
    std::vector<ObjectID> ids, dep;
    if (init) {
      ids = create_accounts(eng, 200);
      dep = create_accounts(eng, 64);
    } else {
      split_accounts(eng, ids, dep);
    }
    if (per_shard) workload(eng, ids, dep, per_shard, seed, [](const ExecResult&) {});
    if (!eng.wait_emits()) _exit(8);
    if (expect_lost) {
      for (int i = 0; i < 10000; ++i) {
        bool all = true;
        for (u16 s = 0; s < kShards; ++s) all &= eng.stats(s).repl_lost;
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      for (u16 s = 0; s < kShards; ++s) {
        if (!eng.stats(s).repl_lost) _exit(9);
      }
    } else {
      wait_replicated(eng);
    }
    eng.stop();
    _exit(0);
  }
  wait_ok(pid);
}

// 5. A primary restarted from an older copy of its directory (as after a
// crash that lost records it had already shipped) refuses a standby whose
// log went further, whether the standby is ahead of it or the two wrote
// different records at the same LSNs.
void test_diverged() {
  std::string pd = temp_dir("p"), sd = temp_dir("s"), old = temp_dir("old");
  pid_t sb = start_standby(sd, true);
  run_primary(pd, true, 1000, 1, false);
  rm_rf(old);
  CHECK_EQ(std::system(("cp -a " + pd + " " + old).c_str()), 0);
  run_primary(pd, false, 300, 2, false);  // the standby gets these; the copy does not
  // Ahead: the old primary has written nothing since.
  rm_rf(pd);
  CHECK_EQ(std::system(("cp -a " + old + " " + pd).c_str()), 0);
  run_primary(pd, false, 0, 0, true);
  // Same LSNs, other records: the old primary writes more than the standby
  // got before the standby reconnects.
  stop_child(sb);
  rm_rf(pd);
  CHECK_EQ(std::system(("cp -a " + old + " " + pd).c_str()), 0);
  int go[2], ready[2];
  CHECK_EQ(::pipe(go), 0);
  CHECK_EQ(::pipe(ready), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    // Writes first with no standby, then lets it in.
    ::close(go[1]);
    ::close(ready[0]);
    Engine eng(primary_config(pd, false, false, 0));
    eng.start();
    std::vector<ObjectID> ids, dep;
    split_accounts(eng, ids, dep);
    workload(eng, ids, dep, 1000, 9, [](const ExecResult&) {});
    if (!eng.wait_emits()) _exit(8);
    char b = 1;
    if (::write(ready[1], &b, 1) != 1) _exit(3);
    if (::read(go[0], &b, 1) != 1) _exit(3);  // the standby is up
    for (int i = 0; i < 10000; ++i) {
      bool lost = true;
      for (u16 s = 0; s < kShards; ++s) lost &= eng.stats(s).repl_lost;
      if (lost) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (u16 s = 0; s < kShards; ++s) {
      if (!eng.stats(s).repl_lost) _exit(9);
    }
    eng.stop();
    _exit(0);
  }
  ::close(go[0]);
  ::close(ready[1]);
  char b;
  CHECK_EQ(::read(ready[0], &b, 1), 1);
  sb = start_standby(sd, false);
  CHECK_EQ(::write(go[1], &b, 1), 1);
  wait_ok(pid);
  stop_child(sb);
  std::printf("diverged: standby ahead of, or different from, its primary is refused\n");
  rm_rf(pd + " " + sd + " " + old);
}

// 7. Online promotion and fencing. A sync primary is SIGKILLed under
// transfers and deposits; its standby is promoted in place (SIGUSR1), keeps
// every acknowledged deposit, conserves transfers, mints unique IDs, and
// serves. The old primary, restarted, can acknowledge nothing: its standby
// is gone, and a new standby seeded from the promoted one refuses its older
// term.
struct Row {
  ObjectID id;
  u64 version;
  i64 balance;
  u64 entries;
};

// The old primary, restarted on its directory: checks that a deposit is not
// acknowledged within a second and that its link never comes up.
void check_fenced(const std::string& pd, u16 port, const ObjectID& target) {
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine::Config c = primary_config(pd, false, /*sync=*/true, /*timeout_ms=*/0);
    c.replica_port = port;
    Engine eng(c);
    eng.start();
    std::atomic<bool> done{false};
    eng.runtime().submit(0, [&] {
      AmountArgs a{1, 0};
      execute(target, kOpDeposit, ByteView(&a, sizeof(a)), {},
              [](void* ctx, const ExecResult&) { static_cast<std::atomic<bool>*>(ctx)->store(true); },
              &done);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (done.load()) _exit(20);
    for (u16 s = 0; s < kShards; ++s) {
      if (eng.stats(s).repl_live) _exit(21);
    }
    _exit(0);  // not stop(): it would wait for the deposit forever
  }
  wait_ok(pid);
}

void test_promote() {
  std::string pd = temp_dir("p"), sd = temp_dir("s"), s2d = temp_dir("s2");
  const u16 seed_port = static_cast<u16>(g_port + 100), s2_port = static_cast<u16>(g_port + 200);
  // The standby: promoted on SIGUSR1, then reports and serves until SIGTERM.
  int ready[2], rows[2];
  CHECK_EQ(::pipe(ready), 0);
  CHECK_EQ(::pipe(rows), 0);
  pid_t sb = ::fork();
  CHECK(sb >= 0);
  if (sb == 0) {
    ::close(ready[0]);
    ::close(rows[0]);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    Engine::Config c = config(sd, true);
    c.standby = true;
    c.standby_host = "127.0.0.1";
    c.replica_port = g_port;
    c.seed_port = seed_port;
    Engine eng(c);
    eng.start();
    char b = 1;
    if (::write(ready[1], &b, 1) != 1) _exit(3);
    int sig = 0;
    sigwait(&set, &sig);
    if (sig != SIGUSR1) _exit(22);
    double secs = eng.promote();
    if (eng.term() != 1) _exit(23);
    if (!eng.wait_emits()) _exit(8);
    // The index came over live: 244 accounts keep "e", 20 moved.
    if (lookup(eng, 0, kEmailIndex, "e").size() != 244) _exit(25);
    for (char c = 'a'; c < 'a' + 20; ++c) {
      if (lookup(eng, 1, kEmailIndex, std::string("x") + c).size() != 1) _exit(26);
    }
    std::set<ObjectID> seen;
    for (u16 s = 0; s < kShards; ++s) {
      eng.for_each_object(s, [&](const ObjectHeader& h) {
        const auto* acc = reinterpret_cast<const AccountV1*>(h.payload());
        Row r{h.id, h.version, acc->balance_cents, u64{acc->n_out} + acc->n_in};
        seen.insert(h.id);
        if (::write(rows[1], &r, sizeof(r)) != sizeof(r)) _exit(3);
      });
    }
    ::close(rows[1]);
    auto fresh = create_accounts(eng, 400);  // serves, and mints new IDs
    for (const ObjectID& id : fresh) {
      if (seen.count(id)) _exit(24);
    }
    std::printf("promote: in place in %.3fs, term 1\n", secs);
    sigwait(&set, &sig);
    eng.stop();
    _exit(0);
  }
  ::close(ready[1]);
  ::close(rows[1]);
  char b;
  CHECK_EQ(::read(ready[0], &b, 1), 1);

  int acks[2];
  CHECK_EQ(::pipe(acks), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(acks[0]);
    ::close(rows[0]);
    Engine eng(primary_config(pd, true, true, 0));
    eng.start();
    auto ids = create_accounts(eng, 200);
    auto dep = create_accounts(eng, 64);
    // New index keys the standby must carry into promotion.
    for (u32 i = 0; i < 20; ++i) {
      SetEmailArgs a{1, 2, {'x', static_cast<char>('a' + i)}};
      auto res = one(eng, 0, [&](Completion c, void* ctx) {
        execute(ids[i], kOpSetEmail, ByteView(&a, sizeof(a)), {}, c, ctx);
      });
      if (res.r.status != Status::OK) _exit(5);
    }
    for (u32 i = 0;; ++i) {
      workload(eng, ids, dep, 1000, 50 + i, [&](const ExecResult& r) {
        Ack a{r.id, r.version};
        if (::write(acks[1], &a, sizeof(a)) != sizeof(a)) _exit(3);
      });
    }
  }
  ::close(acks[1]);
  std::map<ObjectID, u64> acked;
  u64 got = 0;
  bool killed = false;
  Ack a;
  while (::read(acks[0], &a, sizeof(a)) == sizeof(a)) {
    acked[a.id] = std::max(acked[a.id], a.version);
    if (!killed && ++got >= 5000) {
      ::kill(pid, SIGKILL);
      killed = true;
    }
  }
  ::close(acks[0]);
  int st = 0;
  CHECK_EQ(::waitpid(pid, &st, 0), pid);

  ::kill(sb, SIGUSR1);
  std::map<ObjectID, Row> seen;
  Row r;
  while (::read(rows[0], &r, sizeof(r)) == sizeof(r)) seen[r.id] = r;
  ::close(rows[0]);
  i64 total = 0;
  u64 entries = 0, xfer = 0;
  for (const auto& [id, row] : seen) {
    entries += row.entries;
    if (!acked.count(id)) {
      total += row.balance;
      ++xfer;
    }
  }
  for (const auto& [id, v] : acked) {
    auto it = seen.find(id);
    CHECK(it != seen.end());
    CHECK(it->second.version >= v);
    CHECK_EQ(it->second.balance, kStart + static_cast<i64>(it->second.version - 1));
  }
  CHECK_EQ(entries, 0u);
  CHECK_EQ(xfer, 200u);
  CHECK_EQ(total, kStart * 200);

  // Fenced: the old primary's standby is gone...
  check_fenced(pd, g_port, acked.begin()->first);
  // ...and a standby seeded from the new primary refuses its older term.
  pid_t s2 = start_standby(s2d, false, "127.0.0.1:" + std::to_string(seed_port), s2_port);
  check_fenced(pd, s2_port, acked.begin()->first);
  stop_child(s2);
  stop_child(sb);
  std::printf("promote: %zu acked deposit accounts kept, transfers conserved; old primary fenced\n",
              acked.size());
  rm_rf(pd + " " + sd + " " + s2d);
}

// 6. Sync with no standby: durable acks wait for the timeout, then the
// primary continues and reports itself degraded.
void test_degrade() {
  std::string pd = temp_dir("p");
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine eng(primary_config(pd, true, true, 300));
    eng.start();
    auto t0 = std::chrono::steady_clock::now();
    auto ids = create_accounts(eng, 8);
    double first = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (first < 0.15) _exit(10);  // the first acks waited for the timeout
    t0 = std::chrono::steady_clock::now();
    create_accounts(eng, 64);
    double after = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (after > 0.2) _exit(11);  // and later ones do not
    for (u16 s = 0; s < kShards; ++s) {
      if (!eng.stats(s).repl_degraded) _exit(12);
    }
    std::printf("degrade: first acks after %.2fs, then local durability (%.3fs for 64)\n", first,
                after);
    std::fflush(stdout);
    eng.stop();
    _exit(0);
  }
  wait_ok(pid);
  rm_rf(pd);
}

// 8. A gated sync primary (a witness decides) waits past the timeout until
// allow_degrade(true).
void test_gated() {
  std::string pd = temp_dir("p");
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine::Config c = primary_config(pd, true, true, 100);
    c.replica_degrade_gated = true;
    Engine eng(c);
    eng.start();
    std::atomic<bool> done{false};
    std::thread t([&] {
      create_accounts(eng, 8);
      done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (done.load()) _exit(30);  // past the timeout, but not granted
    if (eng.replication_status().blocked != kShards) _exit(31);
    auto t0 = std::chrono::steady_clock::now();
    eng.allow_degrade(true);
    t.join();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (s > 0.2 || eng.replication_status().degraded != kShards) _exit(32);
    std::printf("gated: no acks past the timeout until granted, then %.3fs\n", s);
    eng.stop();
    _exit(0);
  }
  wait_ok(pid);
  rm_rf(pd);
}

// 9. A hung standby (SIGSTOP) keeps its socket open; the primary treats it
// as down once it acks nothing for the timeout, and degrades.
void test_hung() {
  std::string pd = temp_dir("p"), sd = temp_dir("s");
  pid_t sb = start_standby(sd, true);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine eng(primary_config(pd, true, true, 300));
    eng.start();
    create_accounts(eng, 8);
    wait_replicated(eng);
    ::kill(sb, SIGSTOP);
    auto t0 = std::chrono::steady_clock::now();
    create_accounts(eng, 8);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (s < 0.25 || eng.replication_status().degraded == 0) {  // shards that had writes
      std::printf("hung: %.3fs, %u degraded\n", s, eng.replication_status().degraded);
      _exit(33);
    }
    std::printf("hung: a stopped standby is dropped after %.2fs\n", s);
    ::kill(sb, SIGCONT);
    _exit(0);  // not stop(): it would wait for the standby
  }
  wait_ok(pid);
  ::kill(sb, SIGCONT);
  stop_child(sb);
  rm_rf(pd + " " + sd);
}

// 10. Reads on the standby: under a lease from its sync primary, a read on
// the standby sees every deposit the primary acknowledged before it; with
// the primary stopped the lease runs out and reads answer E_STALE.
void test_standby_reads() {
  std::string pd = temp_dir("p"), sd = temp_dir("s");
  int ready[2], chk[2];
  CHECK_EQ(::pipe(ready), 0);
  CHECK_EQ(::pipe(chk), 0);
  pid_t sb = ::fork();
  CHECK(sb >= 0);
  if (sb == 0) {
    ::close(ready[0]);
    ::close(chk[1]);
    Engine::Config c = config(sd, true);
    c.standby = true;
    c.standby_host = "127.0.0.1";
    c.replica_port = g_port;
    c.standby_read_lease_ms = 200;
    Engine eng(c);
    eng.start();
    char b = 1;
    if (::write(ready[1], &b, 1) != 1) _exit(3);
    auto read = [&](const ObjectID& id, u16 shard) {
      return one(eng, shard, [&](Completion cb, void* ctx) {
        execute(id, kOpGetBalance, {}, {}, cb, ctx);
      });
    };
    // Writes stay refused.
    Ack a;
    u64 n = 0, retried = 0;
    while (::read(chk[0], &a, sizeof(a)) == sizeof(a)) {
      if (a.version == 0) break;
      auto r = read(a.id, static_cast<u16>(n++ % kShards));
      for (int i = 0; r.r.status == Status::E_STALE && i < 500; ++i) {  // the first lease
        ++retried;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        r = read(a.id, 0);
      }
      if (r.r.status != Status::OK) _exit(40);
      if (r.r.version < a.version) _exit(41);  // missed an acknowledged write
    }
    auto w = one(eng, 0, [&](Completion cb, void* ctx) {
      AmountArgs d{1, 0};
      execute(a.id, kOpDeposit, ByteView(&d, sizeof(d)), {}, cb, ctx);
    });
    if (w.r.status != Status::E_STANDBY) _exit(42);
    // The primary is stopped: within a lease, reads turn stale.
    auto t0 = std::chrono::steady_clock::now();
    while (read(a.id, 1).r.status != Status::E_STALE) {
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) _exit(43);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("standby reads: %llu reads saw every acked deposit (%llu retries before the first "
                "lease); stale %.2fs after the primary stopped\n",
                (unsigned long long)n, (unsigned long long)retried, s);
    eng.stop();
    _exit(0);
  }
  ::close(ready[1]);
  ::close(chk[0]);
  char b;
  CHECK_EQ(::read(ready[0], &b, 1), 1);
  int acks[2];
  CHECK_EQ(::pipe(acks), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(acks[0]);
    ::close(chk[1]);
    Engine eng(primary_config(pd, true, true, 1000));
    eng.start();
    auto dep = create_accounts(eng, 64);
    for (u32 i = 0; i < 3000; ++i) {
      auto r = one(eng, static_cast<u16>(i % kShards), [&](Completion cb, void* ctx) {
        AmountArgs d{1, 0};
        execute(dep[i % dep.size()], kOpDeposit, ByteView(&d, sizeof(d)), {}, cb, ctx);
      });
      if (r.r.status != Status::OK || !r.r.durable) _exit(4);
      Ack a{r.r.id, r.r.version};
      if (::write(acks[1], &a, sizeof(a)) != sizeof(a)) _exit(3);
    }
    Ack end{dep[0], 0};
    if (::write(acks[1], &end, sizeof(end)) != sizeof(end)) _exit(3);
    ::pause();
    _exit(0);
  }
  ::close(acks[1]);
  Ack a;
  while (::read(acks[0], &a, sizeof(a)) == sizeof(a)) {
    if (a.version == 0) ::kill(pid, SIGSTOP);
    CHECK_EQ(::write(chk[1], &a, sizeof(a)), static_cast<ssize_t>(sizeof(a)));
    if (a.version == 0) break;
  }
  ::close(chk[1]);
  wait_ok(sb);
  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
  ::close(acks[0]);
  rm_rf(pd + " " + sd);
}

// 11. A sync primary does not continue without a standby that holds a read
// lease until the lease runs out, even past its own timeout.
void test_lease_hold() {
  std::string pd = temp_dir("p"), sd = temp_dir("s");
  pid_t sb = start_standby(sd, true, "", 0, /*lease_ms=*/1500);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine eng(primary_config(pd, true, true, 100));
    eng.start();
    create_accounts(eng, 8);
    wait_replicated(eng);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // leases renewed while idle
    ::kill(sb, SIGSTOP);
    auto t0 = std::chrono::steady_clock::now();
    create_accounts(eng, 8);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("lease hold: a 100 ms timeout waited %.2fs for a 1.5 s read lease to run out\n", s);
    ::kill(sb, SIGCONT);
    if (s < 1.0) _exit(34);
    _exit(0);
  }
  wait_ok(pid);
  ::kill(sb, SIGCONT);
  stop_child(sb);
  rm_rf(pd + " " + sd);
}

// The default slot map's owner, usable off the shard threads.
u16 test_owner(const ObjectID& id) {
  static const SlotMap m = SlotMap::make_default(kShards);
  return m.owner_of(id);
}

// 12. Safe retries (docs/RETRIES.md), on one engine: a sequenced deposit
// or create sent twice runs once, an older sequence is refused, a
// misrouted one is refused, and sessions survive a checkpoint and restart.
void test_retries_local() {
  std::string d = temp_dir("r");
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ObjectID acc, made;
    u16 own = 0;
    for (int round = 0; round < 2; ++round) {
      Engine eng(config(d, round == 0));
      eng.start();
      auto seq_op = [&](u16 shard, u64 seq, bool mk) {
        return one(eng, shard, [&](Completion cb, void* ctx) {
          ExecOptions o;
          o.client_id = 42;
          o.seq = seq;
          if (mk) {
            CreateArgs a{};
            a.balance_cents = kStart;
            create(kTypeId, ByteView(&a, sizeof(a)), o, cb, ctx);
          } else {
            AmountArgs a{1, 0};
            execute(acc, kOpDeposit, ByteView(&a, sizeof(a)), o, cb, ctx);
          }
        });
      };
      auto balance = [&] {
        return one(eng, own, [&](Completion cb, void* ctx) {
          execute(acc, kOpGetBalance, {}, {}, cb, ctx);
        }).as<BalanceResult>().balance_cents;
      };
      if (round == 0) {
        acc = create_accounts(eng, 1)[0];
        own = test_owner(acc);
        auto a = seq_op(own, 1, false);
        auto b = seq_op(own, 1, false);  // a retry
        if (a.r.status != Status::OK || a.r.replayed) _exit(50);
        if (b.r.status != Status::OK || !b.r.replayed || b.r.version != a.r.version) _exit(51);
        if (balance() != kStart + 1) _exit(52);
        if (seq_op(own, 2, false).r.replayed) _exit(53);
        if (seq_op(own, 1, false).r.status != Status::E_REPLAYED_OLD) _exit(54);
        if (seq_op(static_cast<u16>((own + 1) % kShards), 3, false).r.status != Status::E_NOT_OWNER) _exit(55);
        auto c1 = seq_op(own, 3, true), c2 = seq_op(own, 3, true);
        if (c1.r.status != Status::OK || !c2.r.replayed || !(c2.r.id == c1.r.id)) _exit(56);
        made = c1.r.id;
        eng.checkpoint();
        if (seq_op(own, 4, false).r.replayed) _exit(57);  // after the checkpoint: WAL only
        if (balance() != kStart + 3) _exit(58);
      } else {
        // Recovered: the session remembers seq 4 and the create's ID.
        auto r4 = seq_op(own, 4, false);
        if (!r4.r.replayed || r4.r.status != Status::OK) _exit(59);
        auto c = seq_op(own, 3, true);
        if (c.r.status != Status::E_REPLAYED_OLD) _exit(60);
        if (balance() != kStart + 3) _exit(61);
        if (eng.stats(own).replayed == 0) _exit(62);
        (void)made;
      }
      eng.stop();
    }
    std::printf("retries: duplicates answered from the session, kept across checkpoint and restart\n");
    _exit(0);
  }
  wait_ok(pid);
  rm_rf(d);
}

// 12b. Sessions idle past their TTL are deleted at a checkpoint; the
// client's old sequences are then forgotten.
void test_session_ttl() {
  std::string d = temp_dir("t");
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    Engine::Config c = config(d, true);
    c.session_ttl_s = 1;
    Engine eng(c);
    eng.start();
    ObjectID acc = create_accounts(eng, 1)[0];
    u16 own = test_owner(acc);
    auto dep = [&] {
      return one(eng, own, [&](Completion cb, void* ctx) {
        ExecOptions o;
        o.client_id = 9;
        o.seq = 1;
        AmountArgs a{1, 0};
        execute(acc, kOpDeposit, ByteView(&a, sizeof(a)), o, cb, ctx);
      });
    };
    if (dep().r.replayed || !dep().r.replayed) _exit(80);
    eng.checkpoint();  // not idle long enough yet
    if (eng.stats(own).sessions_expired != 0 || !dep().r.replayed) _exit(81);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    eng.checkpoint();
    if (eng.stats(own).sessions_expired != 1) _exit(82);
    if (dep().r.replayed) _exit(83);  // forgotten: runs again
    std::printf("session ttl: an idle session expires at the checkpoint after its TTL\n");
    eng.stop();
    _exit(0);
  }
  wait_ok(pid);
  rm_rf(d);
}

// 13. Safe retries across a failover: a sync primary runs sequenced
// deposits, pipelined, and is SIGKILLed; its standby is promoted and the
// client resends every request it sent, with the same sequences. Each
// account ends at its start plus exactly the deposits sent to it.
struct Sent {
  u64 shard, i;
};
std::vector<std::vector<ObjectID>> retry_picks(const std::vector<ObjectID>& accs, u64 per_shard) {
  std::vector<std::vector<ObjectID>> mine(kShards), picks(kShards);
  for (const ObjectID& a : accs) mine[test_owner(a)].push_back(a);
  for (u16 s = 0; s < kShards; ++s) {
    std::mt19937_64 rng(99 + s);
    for (u64 i = 0; i < per_shard && !mine[s].empty(); ++i) picks[s].push_back(mine[s][rng() % mine[s].size()]);
  }
  return picks;
}

void test_retries_failover() {
  std::string pd = temp_dir("p"), sd = temp_dir("s");
  const u64 kPer = 20000;
  int ready[2], plan[2];
  CHECK_EQ(::pipe(ready), 0);
  CHECK_EQ(::pipe(plan), 0);
  pid_t sb = ::fork();
  CHECK(sb >= 0);
  if (sb == 0) {
    ::close(ready[0]);
    ::close(plan[1]);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    Engine::Config c = config(sd, true);
    c.standby = true;
    c.standby_host = "127.0.0.1";
    c.replica_port = g_port;
    Engine eng(c);
    eng.start();
    char b = 1;
    if (::write(ready[1], &b, 1) != 1) _exit(3);
    int sig = 0;
    sigwait(&set, &sig);
    eng.promote();
    // The plan: the accounts, then how many requests each shard sent.
    std::vector<ObjectID> accs(64);
    u64 sent[kShards];
    if (::read(plan[0], accs.data(), accs.size() * sizeof(ObjectID)) !=
            static_cast<ssize_t>(accs.size() * sizeof(ObjectID)) ||
        ::read(plan[0], sent, sizeof(sent)) != sizeof(sent)) {
      _exit(3);
    }
    auto picks = retry_picks(accs, kPer);
    std::atomic<u64> replayed{0}, applied{0}, old{0}, bad{0};
    std::latch done(kShards);
    std::vector<Window> ws(kShards);
    for (u16 s = 0; s < kShards; ++s) {
      Window& w = ws[s];
      w.eng = &eng;
      w.shard = s;
      w.total = sent[s];
      w.window = 256;
      w.done = &done;
      w.issue = [&, s](u64 i, Completion cb, void* ctx) {
        ExecOptions o;
        o.client_id = 7;
        o.seq = i + 1;
        AmountArgs a{1, 0};
        execute(picks[s][i], kOpDeposit, ByteView(&a, sizeof(a)), o, cb, ctx);
      };
      w.check = [&](u64, const ExecResult& r) {
        if (r.status == Status::E_REPLAYED_OLD) ++old;
        else if (r.status != Status::OK) ++bad;
        else if (r.replayed) ++replayed;
        else ++applied;
      };
      if (w.total == 0) {
        done.count_down();
        continue;
      }
      eng.runtime().submit(s, [&w] { pump(&w); });
    }
    done.wait();
    if (bad) _exit(70);
    std::map<ObjectID, i64> expect;
    for (u16 s = 0; s < kShards; ++s) {
      for (u64 i = 0; i < sent[s]; ++i) ++expect[picks[s][i]];
    }
    for (const ObjectID& a : accs) {
      i64 bal = one(eng, test_owner(a), [&](Completion cb, void* ctx) {
        execute(a, kOpGetBalance, {}, {}, cb, ctx);
      }).as<BalanceResult>().balance_cents;
      if (bal != kStart + expect[a]) {
        std::printf("account %s: %lld, expected %lld\n", a.to_string().c_str(), (long long)bal,
                    (long long)(kStart + expect[a]));
        _exit(71);
      }
    }
    std::printf("retries after failover: %llu resent; %llu answered from sessions, %llu older, "
                "%llu applied now; every account exact\n",
                (unsigned long long)(sent[0] + sent[1] + sent[2] + sent[3]),
                (unsigned long long)replayed.load(), (unsigned long long)old.load(),
                (unsigned long long)applied.load());
    eng.stop();
    _exit(0);
  }
  ::close(ready[1]);
  ::close(plan[0]);
  char b;
  CHECK_EQ(::read(ready[0], &b, 1), 1);
  int out[2];
  CHECK_EQ(::pipe(out), 0);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(out[0]);
    Engine eng(primary_config(pd, true, true, 0));
    eng.start();
    auto accs = create_accounts(eng, 64);
    if (::write(out[1], accs.data(), accs.size() * sizeof(ObjectID)) !=
        static_cast<ssize_t>(accs.size() * sizeof(ObjectID))) {
      _exit(3);
    }
    auto picks = retry_picks(accs, kPer);
    run_all(
        eng, kPer,
        [&](u16 s, u64 i, Completion cb, void* ctx) {
          Sent x{s, i};
          if (::write(out[1], &x, sizeof(x)) != sizeof(x)) _exit(3);  // before it can apply
          ExecOptions o;
          o.client_id = 7;
          o.seq = i + 1;
          AmountArgs a{1, 0};
          execute(picks[s][i], kOpDeposit, ByteView(&a, sizeof(a)), o, cb, ctx);
        },
        [](u16, u64, const ExecResult& r) {
          if (r.status != Status::OK || r.replayed) _exit(4);
        });
    _exit(5);  // killed before it gets here
  }
  ::close(out[1]);
  std::vector<ObjectID> accs(64);
  CHECK_EQ(::read(out[0], accs.data(), accs.size() * sizeof(ObjectID)),
           static_cast<ssize_t>(accs.size() * sizeof(ObjectID)));
  u64 sent[kShards] = {}, total = 0;
  bool killed = false;
  Sent x;
  while (::read(out[0], &x, sizeof(x)) == sizeof(x)) {
    sent[x.shard] = std::max(sent[x.shard], x.i + 1);
    if (!killed && ++total >= 30000) {
      ::kill(pid, SIGKILL);
      killed = true;
    }
  }
  CHECK(killed);
  int st = 0;
  ::waitpid(pid, &st, 0);
  ::close(out[0]);
  ::kill(sb, SIGUSR1);
  CHECK_EQ(::write(plan[1], accs.data(), accs.size() * sizeof(ObjectID)),
           static_cast<ssize_t>(accs.size() * sizeof(ObjectID)));
  CHECK_EQ(::write(plan[1], sent, sizeof(sent)), static_cast<ssize_t>(sizeof(sent)));
  ::close(plan[1]);
  wait_ok(sb);
  rm_rf(pd + " " + sd);
}

}  // namespace

int main() {
  register_type();
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // nothing buffered across fork
  g_port = static_cast<u16>(21000 + (::getpid() % 2000) * 8);
  test_identical();
  test_failover();
  test_late(Late::LATE);
  test_late(Late::DISK);
  test_late(Late::LOST);
  test_late(Late::SEED);
  test_late(Late::SLOW);
  test_outage();
  test_diverged();
  test_promote();
  test_gated();
  test_hung();
  test_standby_reads();
  test_lease_hold();
  test_retries_local();
  test_session_ttl();
  test_retries_failover();
  test_degrade();
  std::printf("test_replication: ok\n");
  return 0;
}
