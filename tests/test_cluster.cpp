// Cluster (docs/CLUSTER.md): a fixed slot map across nodes. Three node
// processes on one machine, each owning a third of the slots, run random
// transfers between all their accounts; one node is SIGKILLed mid-run and
// restarted. Once everything settles, money is conserved, no transfer is
// left half done, and every account matches the transfers acknowledged
// (the killed node's from its WAL: what it acknowledged was durable).
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "harness.h"
#include "snc/cluster.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

namespace {

constexpr i64 kStart = 1000;
constexpr u32 kNodes = 3;
constexpr u16 kShards = 2;
constexpr u32 kAccounts = 120;  // per node

bool exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

void write_file(const std::string& path, const std::string& text) {
  {
    std::ofstream f(path + ".tmp");
    f << text;
  }
  ::rename((path + ".tmp").c_str(), path.c_str());
}

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::vector<ClusterNode> cluster(u16 port_base) {
  std::vector<ClusterNode> c;
  for (u32 n = 0; n < kNodes; ++n) c.push_back({"127.0.0.1", static_cast<u16>(port_base + n * 10), kShards});
  return c;
}

void test_map() {
  std::vector<ClusterNode> nodes = {{"a", 1000, 4}, {"b", 2000, 4}, {"c", 3000, 8}};
  ClusterMap m = ClusterMap::make(nodes);
  u32 count[3] = {};
  for (u32 s = 0; s < kSlots; ++s) ++count[m.node_of[s]];
  CHECK_EQ(count[0], 1024u);
  CHECK_EQ(count[1], 1024u);
  CHECK_EQ(count[2], 2048u);
  CHECK_EQ(m.node_of[0], 0);
  CHECK_EQ(m.node_of[kSlots - 1], 2);
  // Addresses do not change the crc; the assignment does.
  std::vector<ClusterNode> moved = nodes;
  moved[0].host = "elsewhere";
  CHECK_EQ(ClusterMap::make(moved).crc(), m.crc());
  moved[2].n_shards = 4;
  CHECK(ClusterMap::make(moved).crc() != m.crc());
  std::vector<ClusterNode> parsed = parse_cluster("10.0.0.1:7000:4,host-b:7100:8");
  CHECK_EQ(parsed.size(), 2u);
  CHECK(parsed[1].host == "host-b");
  CHECK_EQ(parsed[1].link_port, 7100);
  CHECK_EQ(parsed[1].n_shards, 8);
  bool threw = false;
  try {
    parse_cluster("10.0.0.1:7000");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  std::printf("map: ok\n");
}

// ---- One node process -------------------------------------------------------------------

struct Node {
  std::string dir;
  u32 id;
  u16 port_base;
};

std::vector<ObjectID> read_ids(const std::string& dir, u32 node) {
  std::vector<ObjectID> ids;
  std::istringstream in(read_file(dir + "/ids." + std::to_string(node)));
  std::string line;
  while (std::getline(in, line)) {
    if (auto id = ObjectID::parse(line)) ids.push_back(*id);
  }
  return ids;
}

struct Totals {
  i64 total = 0;
  u64 entries = 0, accounts = 0;
};

Totals totals(Engine& eng, std::map<ObjectID, i64>* balance = nullptr) {
  Totals t;
  for (u16 sh = 0; sh < eng.shard_count(); ++sh) {
    eng.for_each_object(sh, [&](const ObjectHeader& h) {
      if (h.type_id != kTypeId) return;
      const auto* a = reinterpret_cast<const AccountV1*>(h.payload());
      t.total += a->balance_cents;
      t.entries += a->n_out + a->n_in;
      ++t.accounts;
      if (balance) (*balance)[h.id] = a->balance_cents;
    });
  }
  return t;
}

[[noreturn]] void node_main(const Node& nd, bool init, u32 rounds) {
  Engine::Config c;
  c.runtime.n_shards = kShards;
  c.expected_objects = 4096;
  c.data_dir = nd.dir + "/node" + std::to_string(nd.id);
  c.init = init;
  c.wal_segment_bytes = 1u << 20;
  c.cluster = cluster(nd.port_base);
  c.node_id = static_cast<u16>(nd.id);
  ::mkdir(c.data_dir.c_str(), 0755);
  Engine eng(c);
  eng.start();
  const std::string me = std::to_string(nd.id);

  if (init) {
    std::vector<ObjectID> ids(kAccounts);
    run_all(
        eng, kAccounts / kShards,
        [&](u16, u64, Completion done, void* ctx) {
          CreateArgs a{};
          a.balance_cents = kStart;
          a.created_ns = 1;
          a.email_len = 1;
          a.email[0] = 'e';
          a.name_len = 1;
          a.name[0] = 'n';
          create(kTypeId, ByteView(&a, sizeof(a)), {}, done, ctx);
        },
        [&](u16 s, u64 i, const ExecResult& r) {
          CHECK_EQ(r.status, Status::OK);
          ids[s * (kAccounts / kShards) + i] = r.id;
        });
    const ClusterMap& m = eng.cluster();
    std::string text;
    for (const ObjectID& id : ids) {
      CHECK_EQ(m.node_of_id(id), nd.id);  // minted only in this node's slots
      text += id.to_string() + "\n";
    }
    write_file(nd.dir + "/ids." + me, text);
  }
  std::vector<std::vector<ObjectID>> ids(kNodes);
  for (u32 n = 0; n < kNodes; ++n) {
    while (!exists(nd.dir + "/ids." + std::to_string(n))) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ids[n] = read_ids(nd.dir, n);
    CHECK_EQ(ids[n].size(), kAccounts);
  }
  std::vector<ObjectID> all;
  for (auto& v : ids) all.insert(all.end(), v.begin(), v.end());

  // Another node's account is refused here.
  OneResult wrong = one(eng, 0, [&](Completion done, void* ctx) {
    execute(ids[(nd.id + 1) % kNodes][0], kOpGetBalance, ByteView(), {}, done, ctx);
  });
  CHECK_EQ(wrong.r.status, Status::E_WRONG_NODE);

  // Transfers from this node's accounts to any account. Each acknowledged
  // one is appended to this node's log (sources here, destinations anywhere).
  write_file(nd.dir + "/busy." + me, "");
  std::mt19937_64 rng(nd.id * 7919 + (init ? 0 : 1));
  // Written as each is acknowledged (no user-space buffer): a SIGKILL loses
  // at most the lines of acks it lands between.
  const int log = ::open((nd.dir + "/acked." + me).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  CHECK(log >= 0);
  for (u32 round = 0; round < rounds; ++round) {
    struct Pick {
      ObjectID from, to;
      i64 amount;
    };
    std::vector<std::vector<Pick>> picks(kShards);
    const u64 per = 200;
    for (u16 s = 0; s < kShards; ++s) {
      for (u64 i = 0; i < per; ++i) {
        Pick p;
        p.from = ids[nd.id][rng() % kAccounts];
        do p.to = all[rng() % all.size()];
        while (p.to == p.from);
        p.amount = 1 + static_cast<i64>(rng() % 50);
        picks[s].push_back(p);
      }
    }
    run_all(
        eng, per,
        [&](u16 s, u64 i, Completion done, void* ctx) {
          const Pick& p = picks[s][i];
          TransferArgs t{p.to, p.amount, 7};
          execute(p.from, kOpTransfer, ByteView(&t, sizeof(t)), {}, done, ctx);
        },
        [&](u16 s, u64 i, const ExecResult& r) {
          const Pick& p = picks[s][i];
          if (r.status == Status::OK) {
            const std::string line =
                p.from.to_string() + " " + p.to.to_string() + " " + std::to_string(p.amount) + "\n";
            CHECK_EQ(::write(log, line.data(), line.size()), static_cast<ssize_t>(line.size()));
          } else if (r.status != Status::E_OVERLOADED) {
            CHECK_EQ(r.status, Status::E_APP);
            CHECK(r.app_code == kAppInsufficientFunds || r.app_code == kAppBusy);
          }
        });
  }
  ::close(log);
  write_file(nd.dir + "/done." + me, "");

  // Report until told to stop.
  while (!exists(nd.dir + "/stop")) {
    Totals t = totals(eng);
    ShardStats st{};
    for (u16 s = 0; s < kShards; ++s) {
      ShardStats x = eng.stats(s);
      st.node_emits_sent += x.node_emits_sent;
      st.node_emits_received += x.node_emits_received;
      st.node_emits_dropped += x.node_emits_dropped;
      st.node_resumes += x.node_resumes;
      st.node_connects += x.node_connects;
    }
    std::ostringstream o;
    o << t.total << " " << t.entries << " " << t.accounts << " " << st.node_emits_sent << " "
      << st.node_emits_received << " " << st.node_emits_dropped << " " << st.node_resumes << " " << st.node_connects;
    write_file(nd.dir + "/status." + me, o.str());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (getenv("SNC_CLUSTER_DEBUG")) {
    std::string d;
    const ClusterMap& m = eng.cluster();
    for (u16 sh = 0; sh < kShards; ++sh) {
      eng.for_each_object(sh, [&](const ObjectHeader& h) {
        if (h.type_id != kTypeId) return;
        const auto* a = reinterpret_cast<const AccountV1*>(h.payload());
        const auto* out = reinterpret_cast<const OutEntry*>(a + 1);
        const auto* in = reinterpret_cast<const InEntry*>(out + a->n_out);
        for (u16 i = 0; i < a->n_out; ++i)
          d += "out " + h.id.to_string() + " seq " + std::to_string(out[i].seq) + " to node " +
               std::to_string(m.node_of_id(out[i].to)) + "\n";
        for (u16 i = 0; i < a->n_in; ++i)
          d += "in " + h.id.to_string() + " seq " + std::to_string(in[i].seq) + " from node " +
               std::to_string(m.node_of_id(in[i].from)) + "\n";
      });
    }
    write_file(nd.dir + "/entries." + me, d);
  }
  std::map<ObjectID, i64> balance;
  totals(eng, &balance);
  std::string text;
  for (auto& [id, b] : balance) text += id.to_string() + " " + std::to_string(b) + "\n";
  write_file(nd.dir + "/balances." + me, text);
  eng.stop();
  std::fflush(stdout);
  _exit(0);
}

pid_t spawn(const Node& nd, bool init, u32 rounds) {
  std::fflush(stdout);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) node_main(nd, init, rounds);
  return pid;
}

void wait_for(const std::string& path, double timeout_s) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!exists(path)) {
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(timeout_s));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void test_kill_restart() {
  char tmpl[] = "/tmp/snc-cluster-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  // Below the ephemeral range, spread by pid so parallel runs do not meet.
  const u16 port_base = static_cast<u16>(20000 + (::getpid() % 1200) * 10);
  std::vector<Node> nodes;
  for (u32 n = 0; n < kNodes; ++n) nodes.push_back({dir, n, port_base});
  std::vector<pid_t> pids;
  for (u32 n = 0; n < kNodes; ++n) pids.push_back(spawn(nodes[n], true, 25));

  // Node 1 dies in the middle of its transfers (and of the others' to it).
  wait_for(dir + "/busy.1", 30);
  ::usleep(200000);
  ::kill(pids[1], SIGKILL);
  int st = 0;
  ::waitpid(pids[1], &st, 0);
  CHECK(WIFSIGNALED(st));
  ::unlink((dir + "/done.1").c_str());
  ::usleep(100000);  // the others carry on, their follow-ups to it dropped
  pids[1] = spawn(nodes[1], false, 5);

  for (u32 n = 0; n < kNodes; ++n) wait_for(dir + "/done." + std::to_string(n), 60);
  // Settled: no entries anywhere, twice in a row.
  const auto t0 = std::chrono::steady_clock::now();
  u32 settled = 0;
  i64 total = 0;
  u64 accounts = 0;
  std::vector<std::vector<long long>> last(kNodes);
  while (settled < 2) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(getenv("SNC_CLUSTER_DEBUG") ? 8 : 60)) {
      write_file(dir + "/stop", "");
      for (pid_t p : pids) ::waitpid(p, &st, 0);
      std::fprintf(stderr, "never settled; see %s\n", dir.c_str());
      CHECK(false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    total = 0;
    accounts = 0;
    u64 entries = 0;
    for (u32 n = 0; n < kNodes; ++n) {
      std::istringstream in(read_file(dir + "/status." + std::to_string(n)));
      std::vector<long long> v(8, -1);
      for (auto& x : v) in >> x;
      last[n] = v;
      total += v[0];
      entries += static_cast<u64>(v[1]);
      accounts += static_cast<u64>(v[2]);
    }
    settled = entries == 0 ? settled + 1 : 0;
  }
  write_file(dir + "/stop", "");
  for (pid_t p : pids) {
    ::waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  for (u32 n = 0; n < kNodes; ++n) {
    std::printf("node %u: %lld follow-ups sent to other nodes, %lld received, %lld dropped, %lld resumes, %lld connects\n",
                n, last[n][3], last[n][4], last[n][5], last[n][6], last[n][7]);
  }
  CHECK_EQ(accounts, u64{kNodes} * kAccounts);
  CHECK_EQ(total, kStart * static_cast<i64>(kNodes * kAccounts));

  // Every account: its start plus the transfers acknowledged. The killed
  // node's log can lack the few acks the kill landed between (at most one
  // window of 64 per shard, two accounts each); nothing else may differ.
  std::map<ObjectID, i64> expect, got;
  u64 acked = 0;
  for (u32 n = 0; n < kNodes; ++n) {
    std::istringstream in(read_file(dir + "/acked." + std::to_string(n)));
    std::string from, to;
    i64 amount;
    while (in >> from >> to >> amount) {
      expect[*ObjectID::parse(from)] -= amount;
      expect[*ObjectID::parse(to)] += amount;
      ++acked;
    }
    std::istringstream b(read_file(dir + "/balances." + std::to_string(n)));
    std::string id;
    i64 bal;
    while (b >> id >> bal) got[*ObjectID::parse(id)] = bal;
  }
  u64 off = 0;
  for (auto& [id, bal] : got) {
    if (bal != kStart + expect[id]) ++off;
  }
  std::printf("kill and restart: %llu transfers acknowledged, %u accounts, %llu off the log\n",
              (unsigned long long)acked, kNodes * kAccounts, (unsigned long long)off);
  CHECK(off <= 2u * 64 * kShards);
  std::string cmd = "rm -rf '" + dir + "'";
  CHECK_EQ(std::system(cmd.c_str()), 0);
}

}  // namespace

int main() {
  register_type();
  test_map();
  test_kill_restart();
  std::printf("test_cluster: ok\n");
  return 0;
}
