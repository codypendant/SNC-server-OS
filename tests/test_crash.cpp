// Crash test: a child process creates accounts and deposits into them,
// reporting every acknowledged (DURABLE) result to the parent over a pipe.
// The parent kills it with SIGKILL at an arbitrary point, then recovers the
// space in a fresh process and checks that nothing acknowledged was lost:
// for every acked (id, version, balance), the recovered version is >= the
// acked version, and every recovered account is internally consistent
// (balance == 100 + deposits). Repeated over several rounds, so later rounds
// recover from a WAL that already has a crash tail in it.
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <map>
#include <string>

#include "harness.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

struct Ack {
  ObjectID id;
  u64 version;
  i64 balance;
};
static_assert(sizeof(Ack) <= PIPE_BUF);

std::string g_dir;
int g_pipe = -1;

Engine::Config config(bool init) {
  Engine::Config c;
  c.runtime.n_shards = 4;
  c.runtime.ring_slots = 64;
  c.expected_objects = 50000;
  c.data_dir = g_dir;
  c.init = init;
  c.wal_segment_bytes = 1u << 20;
  return c;
}

void send_ack(const ExecResult& r) {
  Ack a{r.id, r.version, 0};
  if (r.result.len == sizeof(BalanceResult)) {
    BalanceResult b;
    std::memcpy(&b, r.result.data, sizeof(b));
    a.balance = b.balance_cents;
  } else {
    a.balance = 100;  // create
  }
  ssize_t n = ::write(g_pipe, &a, sizeof(a));  // atomic: < PIPE_BUF
  if (n != sizeof(a)) _exit(3);
}

[[noreturn]] void writer(bool init, u32 round) {
  Engine eng(config(init));
  eng.start();
  constexpr u64 kPerShard = 500;
  std::vector<std::vector<ObjectID>> mine(eng.shard_count());
  run_all(
      eng, kPerShard,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        CreateArgs a{};
        a.balance_cents = 100;
        a.created_ns = round;
        std::string email = "r" + std::to_string(round) + "-" + std::to_string(s) + "-" +
                            std::to_string(i) + "@x";
        a.email_len = static_cast<u8>(email.size());
        std::memcpy(a.email, email.data(), email.size());
        create(kTypeId, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [&](u16 s, u64, const ExecResult& r) {
        if (r.status != Status::OK || !r.durable) _exit(4);
        mine[s].push_back(r.id);  // per-shard vector: only touched by shard s
        send_ack(r);
      });
  // Deposit forever (until killed), spread over this round's accounts.
  run_all(
      eng, ~0ull >> 1,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        const auto& v = mine[(s + i) % mine.size()];
        AmountArgs a{1, i};
        execute(v[(i * 7919) % v.size()], kOpDeposit, ByteView(&a, sizeof(a)), {}, c, ctx);
      },
      [&](u16, u64, const ExecResult& r) {
        if (r.status != Status::OK || !r.durable) _exit(5);
        send_ack(r);
      });
  _exit(0);
}

// Runs in a forked child (the parent stays single-threaded so it can fork).
[[noreturn]] void verifier(const std::map<ObjectID, Ack>& acked, u32 round) {
  Engine eng(config(false));
  eng.start();
  const RecoveryReport& rr = eng.recovery();
  std::vector<ObjectID> ids;
  for (const auto& [id, a] : acked) ids.push_back(id);
  u16 n = eng.shard_count();
  u64 per = (ids.size() + n - 1) / n;
  std::atomic<u64> ahead{0}, exact{0};
  run_all(
      eng, per,
      [&](u16 s, u64 i, Completion c, void* ctx) {
        u64 k = std::min<u64>(s * per + i, ids.size() - 1);
        execute(ids[k], kOpGetBalance, {}, {}, c, ctx);
      },
      [&](u16, u64, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        const Ack& a = acked.at(r.id);
        BalanceResult b;
        std::memcpy(&b, r.result.data, sizeof(b));
        CHECK(r.version >= a.version);
        CHECK_EQ(b.balance_cents, static_cast<i64>(100 + (r.version - 1)));
        if (r.version == a.version) {
          CHECK_EQ(b.balance_cents, a.balance);
          exact.fetch_add(1);
        } else {
          ahead.fetch_add(1);
        }
      });
  u64 objects = 0;
  for (u16 s = 0; s < n; ++s) objects += eng.stats(s).objects;
  // Unacked creates may also have survived: never fewer objects than acked.
  CHECK(objects >= acked.size());
  std::printf(
      "round %u: %zu acked objects ok (%llu at acked version, %llu ahead); "
      "recovered %llu objects, %llu WAL records, torn tails %llu, %.3fs\n",
      round, acked.size(), (unsigned long long)exact.load(), (unsigned long long)ahead.load(),
      (unsigned long long)objects, (unsigned long long)rr.wal_records_replayed,
      (unsigned long long)rr.torn_tails, rr.total_s);
  std::fflush(stdout);
  eng.stop();
  _exit(0);
}

}  // namespace

int main(int argc, char** argv) {
  register_type();
  char tmpl[] = "/tmp/snc-crash-XXXXXX";
  g_dir = mkdtemp(tmpl);
  u32 rounds = argc > 1 ? static_cast<u32>(std::atoi(argv[1])) : 4;

  std::map<ObjectID, Ack> acked;
  u64 total_acks = 0;
  for (u32 round = 0; round < rounds; ++round) {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      ::close(fds[0]);
      g_pipe = fds[1];
      writer(round == 0, round);
    }
    ::close(fds[1]);
    // Kill after a round-dependent number of acks, so the crash lands at a
    // different point of the group-commit cycle each time.
    u64 kill_after = 4000 + 9973ull * (round + 1);
    u64 got = 0;
    bool killed = false;
    Ack a;
    for (;;) {
      ssize_t n = ::read(fds[0], &a, sizeof(a));
      if (n == 0) break;  // EOF: writer is gone
      CHECK_EQ(n, static_cast<ssize_t>(sizeof(a)));
      ++got;
      Ack& cur = acked[a.id];
      if (a.version >= cur.version) cur = a;
      if (!killed && got >= kill_after) {
        ::kill(pid, SIGKILL);
        killed = true;  // keep draining what was acked before the kill
      }
    }
    ::close(fds[0]);
    int st = 0;
    CHECK_EQ(::waitpid(pid, &st, 0), pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    total_acks += got;

    pid_t vp = ::fork();
    CHECK(vp >= 0);
    if (vp == 0) verifier(acked, round);
    CHECK_EQ(::waitpid(vp, &st, 0), vp);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  std::string cmd = "rm -rf " + g_dir;
  CHECK_EQ(std::system(cmd.c_str()), 0);
  std::printf("test_crash: ok (%u rounds, %llu acks)\n", rounds, (unsigned long long)total_acks);
  return 0;
}
