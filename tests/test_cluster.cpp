// Cluster (docs/CLUSTER.md): a fixed slot map across replication groups.
// test_kill_restart: three single-member groups on one machine run random
// transfers between all their accounts; one is SIGKILLed mid-run and
// restarted. test_group_failover: three groups of a primary and a sync
// standby; one primary is SIGKILLed mid-run and its standby promoted
// (failover is not migration: the placement epoch never changes). Each
// checks that money is conserved, nothing is left half done, and every
// account matches the transfers acknowledged.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
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
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "../tools/control.h"
#include "harness.h"
#include "snc/cluster.h"
#include "snc/net.h"
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

// Group g, member m: node links on base + 40g + 10m + shard, clients on
// base + 40g + 10m + 5 + shard; the group's replication on base + 40g + 20.
std::vector<ClusterGroup> cluster(u16 port_base, u32 members = 1) {
  std::vector<ClusterGroup> c;
  for (u32 g = 0; g < kNodes; ++g) {
    ClusterGroup grp;
    grp.n_shards = kShards;
    for (u32 m = 0; m < members; ++m) {
      const u16 at = static_cast<u16>(port_base + g * 40 + m * 10);
      grp.members.push_back({"127.0.0.1", at, static_cast<u16>(at + 5)});
    }
    c.push_back(grp);
  }
  return c;
}
u16 repl_port(u16 port_base, u32 g) { return static_cast<u16>(port_base + g * 40 + 20); }

void test_map() {
  std::vector<ClusterGroup> groups = {{4, {{"a", 1000}}}, {4, {{"b", 2000}, {"b2", 2100}}}, {8, {{"c", 3000}}}};
  ClusterMap m = ClusterMap::make(groups);
  u32 count[3] = {};
  for (u32 s = 0; s < kSlots; ++s) ++count[m.group_of[s]];
  CHECK_EQ(count[0], 1024u);
  CHECK_EQ(count[1], 1024u);
  CHECK_EQ(count[2], 2048u);
  CHECK_EQ(m.group_of[0], 0);
  CHECK_EQ(m.group_of[kSlots - 1], 2);
  CHECK_EQ(m.epoch, 1u);
  // Members and addresses do not change the crc (a failover, or a standby
  // added, is not a placement change); the assignment does.
  std::vector<ClusterGroup> moved = groups;
  moved[0].members[0].host = "elsewhere";
  moved[1].members.pop_back();
  moved[2].members.push_back({"c2", 3100});
  CHECK_EQ(ClusterMap::make(moved).crc(), m.crc());
  moved[2].n_shards = 4;
  CHECK(ClusterMap::make(moved).crc() != m.crc());
  std::vector<ClusterGroup> parsed = parse_cluster("10.0.0.1:7000:4,host-b:7100:8:7400|host-c:7100:8:7500");
  CHECK_EQ(parsed.size(), 2u);
  CHECK_EQ(parsed[0].members.size(), 1u);
  CHECK_EQ(parsed[0].members[0].client_port, 0);
  CHECK_EQ(parsed[1].members.size(), 2u);
  CHECK_EQ(parsed[1].members[0].client_port, 7400);
  CHECK(parsed[1].members[0].host == "host-b");
  CHECK(parsed[1].members[1].host == "host-c");
  CHECK_EQ(parsed[1].members[1].link_port, 7100);
  CHECK_EQ(parsed[1].members[1].client_port, 7500);
  CHECK_EQ(parsed[1].n_shards, 8);
  for (const char* bad : {"10.0.0.1:7000", "a:1:4|b:1:8", "a:1:4,"}) {
    bool threw = false;
    try {
      parse_cluster(bad);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  std::printf("map: ok\n");
}

// ---- One node process -------------------------------------------------------------------

struct Node {
  std::string dir;
  u32 id;
  u16 port_base;
  // Non-empty: the node takes its map from these control members as
  // NodeID kNodeIdBase + id (docs/CONTROL_PLANE.md), not from cluster().
  std::string control;
};
constexpr u32 kNodeIdBase = 10;

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
  if (nd.control.empty()) {
    c.cluster = cluster(nd.port_base);
    c.group_id = static_cast<u16>(nd.id);
  } else {
    control::Members ms;
    CHECK(control::parse_members(nd.control, ms));
    control::NodeSetup setup;
    std::string why;
    if (!control::resolve(ms, c.data_dir, kNodeIdBase + nd.id, 2000, setup, why)) {
      std::fprintf(stderr, "node %u: %s\n", nd.id, why.c_str());
      _exit(3);
    }
    control::apply(setup, ms, c);
    CHECK_EQ(setup.group, nd.id);
    CHECK_EQ(setup.member, 0u);
    write_file(nd.dir + "/setup." + std::to_string(nd.id) + (init ? "" : ".restart"),
               std::to_string(setup.stored) + " " + std::to_string(setup.placement.epoch));
  }
  ::mkdir(c.data_dir.c_str(), 0755);
  net::NetServer::Config nc;
  nc.base_port = c.cluster[nd.id].members[0].client_port;
  net::NetServer server(nc, kShards);
  c.app_poll = net::NetServer::poll_hook;
  c.app_user = &server;
  Engine eng(c);
  eng.start();
  const std::string me = std::to_string(nd.id);
  if (!nd.control.empty()) CHECK(eng.space_uuid() == c.init_uuid);  // the group's space, from the map

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
      CHECK_EQ(m.group_of_id(id), nd.id);  // minted only in this group's slots
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
  CHECK_EQ(wrong.r.status, Status::E_MOVED);
  const MovedResult moved = wrong.as<MovedResult>();
  CHECK_EQ(moved.epoch, eng.cluster().epoch);
  CHECK_EQ(moved.group, (nd.id + 1) % kNodes);

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
               std::to_string(m.group_of_id(out[i].to)) + "\n";
        for (u16 i = 0; i < a->n_in; ++i)
          d += "in " + h.id.to_string() + " seq " + std::to_string(in[i].seq) + " from node " +
               std::to_string(m.group_of_id(in[i].from)) + "\n";
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

// A client that fetches the cluster map from one node reads every account
// from its owner: never refused, never forwarded within the group. With
// `members` > 1 it connects with sessions, so each group's client finds the
// member that is primary now. The map it gets is placement epoch 1.
void check_routing(const std::string& dir, u16 port_base, u32 members = 1, u32 via_group = 2) {
  std::vector<ObjectID> ids;
  for (u32 n = 0; n < kNodes; ++n) {
    std::vector<ObjectID> v = read_ids(dir, n);
    ids.insert(ids.end(), v.begin(), v.end());
  }
  net::ClusterClient cc;
  if (members > 1) cc.enable_sessions(0xC1u, 10000, 2000);
  const std::vector<ClusterGroup> groups = cluster(port_base, members);
  CHECK(cc.connect("127.0.0.1", groups[via_group].members[0].client_port));
  CHECK_EQ(cc.group_count(), kNodes);
  CHECK_EQ(cc.map_crc(), ClusterMap::make(groups).crc());
  CHECK_EQ(cc.epoch(), 1u);
  struct Seen {
    u64 ok = 0, forwarded = 0, wrong = 0;
    u64 epoch;
    u16 owner;
  } seen;
  seen.epoch = cc.epoch();
  for (u64 i = 0; i < ids.size(); ++i) {
    wire::ReqHeader h{};
    h.kind = wire::Kind::EXEC;
    h.op = kOpGetBalance;
    h.req_id = i;
    h.target = ids[i];
    cc.send(h, ByteView());
  }
  // And one to a group that does not own its target.
  wire::ReqHeader wrong{};
  wrong.kind = wire::Kind::EXEC;
  wrong.op = kOpGetBalance;
  wrong.req_id = ~u64{0};
  wrong.target = ids[0];
  seen.owner = cc.group_of(ids[0]);
  if (members == 1) cc.group(static_cast<u16>((cc.group_of(ids[0]) + 1) % kNodes)).send(0, wrong, ByteView());
  const u64 want = ids.size() + (members == 1 ? 1 : 0);
  u64 got = 0;
  while (got < want) {
    int r = cc.poll(
        [](void* ctx, const wire::RespHeader& h, ByteView body) {
          auto* s = static_cast<Seen*>(ctx);
          if (h.req_id == ~u64{0}) {
            MovedResult m{};
            if (static_cast<Status>(h.status) == Status::E_MOVED && body.len == sizeof(m)) {
              std::memcpy(&m, body.data, sizeof(m));
              s->wrong += m.epoch == s->epoch && m.group == s->owner;
            }
            return;
          }
          s->ok += static_cast<Status>(h.status) == Status::OK;
          s->forwarded += (h.flags & wire::kRespForwarded) != 0;
        },
        &seen, 5000);
    CHECK(r > 0);
    got += static_cast<u64>(r);
  }
  CHECK_EQ(seen.ok, ids.size());
  CHECK_EQ(seen.forwarded, 0u);
  CHECK_EQ(seen.wrong, members == 1 ? 1u : 0u);
  std::printf("routing: %zu reads, each sent to its owner's primary by the cluster map (epoch %llu)%s\n",
              ids.size(), (unsigned long long)cc.epoch(), members == 1 ? "; a misrouted one refused with E_MOVED (its epoch and owner)" : "");
}

// ---- Control plane (docs/CONTROL_PLANE.md, step 2) ------------------------------------

std::string build_dir() {
  char b[4096];
  const ssize_t n = ::readlink("/proc/self/exe", b, sizeof(b) - 1);
  CHECK(n > 0);
  std::string p(b, static_cast<std::size_t>(n));
  return p.substr(0, p.rfind('/'));
}

// Three witnesses holding placement only (no pair to watch).
std::vector<pid_t> start_control(const std::string& dir, u16 port0, std::string& members) {
  members.clear();
  for (u32 i = 0; i < 3; ++i) members += (i ? "," : "") + std::string("127.0.0.1:") + std::to_string(port0 + i);
  const std::string exe = build_dir() + "/snc_witness";
  std::vector<pid_t> pids;
  for (u32 i = 0; i < 3; ++i) {
    std::fflush(stdout);
    pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      const int null = ::open("/dev/null", O_WRONLY);
      ::dup2(null, 1);
      const std::string port = std::to_string(port0 + i), state = dir + "/witness" + std::to_string(i),
                        id = std::to_string(i);
      ::execl(exe.c_str(), exe.c_str(), "--port", port.c_str(), "--state", state.c_str(), "--id", id.c_str(),
              "--peers", members.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    pids.push_back(pid);
  }
  return pids;
}

// The placement chosen once, read back by any member; what may not start.
Placement init_control(const std::string& dir, const control::Members& ms, u16 port_base) {
  std::vector<PlacementNode> nodes;
  std::vector<PlacementGroup> groups;
  const std::vector<ClusterGroup> cg = cluster(port_base);
  for (u32 g = 0; g < kNodes; ++g) {
    nodes.push_back({kNodeIdBase + g, "127.0.0.1", 0});
    PlacementGroup pg;
    pg.n_shards = kShards;
    pg.space = random_uuid();
    pg.members.push_back({kNodeIdBase + g, cg[g].members[0].link_port, cg[g].members[0].client_port});
    groups.push_back(pg);
  }
  const Placement p = Placement::initial(random_uuid(), nodes, groups);
  CHECK(Placement::decode(p.encode()).has_value());
  CHECK_EQ(Placement::decode(p.encode())->encode(), p.encode());
  CHECK_EQ(p.map().crc(), ClusterMap::make(cg).crc());
  // Nothing yet: a node may not start.
  Placement got;
  std::string why;
  {
    const auto t0 = std::chrono::steady_clock::now();
    control::Got g;
    while ((g = control::get(ms, got, why)) == control::Got::UNREACHABLE) {
      CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g == control::Got::NONE);
  }
  control::NodeSetup setup;
  CHECK(!control::resolve(ms, dir + "/early", kNodeIdBase, 0, setup, why));
  // Chosen once: the same map again is accepted, another is refused.
  CHECK_EQ(control::init(ms, p), "OK");
  CHECK_EQ(control::init(ms, p), "OK");
  Placement other = Placement::initial(random_uuid(), nodes, groups);
  CHECK(control::init(ms, other).rfind("NO already", 0) == 0);
  // Every member serves the same map, the first version of the machine.
  for (const auto& [h, port] : ms) {
    CHECK(control::get({{h, port}}, got, why) == control::Got::MAP);
    CHECK_EQ(got.encode(), p.encode());
    std::string reply;
    CHECK(ha::ask(h, port, "READ placement", reply, 1000));
    CHECK_EQ(reply, "STATE 1 " + p.encode());
  }
  // A node the map does not name may not start, nor one on another node's
  // data directory.
  CHECK(!control::resolve(ms, dir + "/stranger", 99, 1000, setup, why));
  CHECK(why.find("in no group") != std::string::npos);
  ::mkdir((dir + "/taken").c_str(), 0755);
  write_file(dir + "/taken/node", std::to_string(kNodeIdBase + 1));
  CHECK(!control::resolve(ms, dir + "/taken", kNodeIdBase, 1000, setup, why));
  CHECK(why.find("belongs to node") != std::string::npos);
  std::printf("control plane: placement epoch 1 chosen by 3 witnesses (once; a second map refused), the same on "
              "each; unknown nodes and foreign data directories refused\n");
  return p;
}

// use_control: the nodes take the map from the control plane by NodeID,
// and node 1 restarts while every control member is down: from the map it
// stored at its first start (provisional, docs/CONTROL_PLANE.md section 9).
void test_kill_restart(bool use_control) {
  char tmpl[] = "/tmp/snc-cluster-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  const u16 port_base = free_ports(26000, 32000, kNodes * 40 + 3);
  std::string control_list;
  std::vector<pid_t> witnesses;
  if (use_control) {
    witnesses = start_control(dir, static_cast<u16>(port_base + kNodes * 40), control_list);
    control::Members ms;
    CHECK(control::parse_members(control_list, ms));
    init_control(dir, ms, port_base);
  }
  std::vector<Node> nodes;
  for (u32 n = 0; n < kNodes; ++n) nodes.push_back({dir, n, port_base, control_list});
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
  if (use_control) {
    for (u32 n = 0; n < kNodes; ++n) CHECK_EQ(read_file(dir + "/setup." + std::to_string(n)), "0 1");
    // Mint leases are cluster-wide (docs/CONTROL_PLANE.md section 11): each
    // node took its own block from the control plane, at or above 2^32, and
    // no lease appears in two groups' IDs.
    std::map<u64, u32> lease_group;
    for (u32 n = 0; n < kNodes; ++n) {
      for (const ObjectID& id : read_ids(dir, n)) {
        CHECK(id.lease() >= (u64{1} << 32));
        auto [it, fresh] = lease_group.emplace(id.lease(), n);
        CHECK(fresh || it->second == n);
      }
    }
    std::set<u64> blocks;
    for (auto& [lease, g] : lease_group) blocks.insert((lease - (u64{1} << 32)) >> 16);
    CHECK_EQ(blocks.size(), u64{kNodes});
    control::Members ms;
    CHECK(control::parse_members(control_list, ms));
    std::string reply;
    CHECK(ha::ask(ms[0].first, ms[0].second, "READ leases", reply, 1000));
    CHECK(reply.rfind("STATE " + std::to_string(kNodes) + " next " + std::to_string((u64{1} << 32) + kNodes * 65536), 0) == 0);
    std::printf("mint leases: %zu leases in use, from %zu disjoint blocks taken from the control plane (%s)\n",
                lease_group.size(), blocks.size(), reply.c_str());
    for (pid_t w : witnesses) {
      ::kill(w, SIGKILL);
      ::waitpid(w, &st, 0);
    }
    witnesses.clear();
  }
  pids[1] = spawn(nodes[1], false, 5);

  for (u32 n = 0; n < kNodes; ++n) wait_for(dir + "/done." + std::to_string(n), 60);
  if (use_control) {
    CHECK_EQ(read_file(dir + "/setup.1.restart"), "1 1");  // stored map, epoch 1
    std::printf("control plane down: node 1 restarted from its stored map (placement epoch 1)\n");
  }
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
  check_routing(dir, port_base);
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
  std::printf("kill and restart%s: %llu transfers acknowledged, %u accounts, %llu off the log\n",
              use_control ? " (map from the control plane)" : "", (unsigned long long)acked, kNodes * kAccounts,
              (unsigned long long)off);
  CHECK(off <= 2u * 64 * kShards);
  std::string cmd = "rm -rf '" + dir + "'";
  CHECK_EQ(std::system(cmd.c_str()), 0);
}

// ---- Replication groups: a group's primary fails over -----------------------------------
//
// Three groups of two members each (docs/CONTROL_PLANE.md section 12, step
// 1). Member 0 starts as primary, member 1 as its sync standby (acks wait
// for it, and it never degrades). Every primary runs transfers from its
// accounts to accounts in all groups until told to finish. Group 1's
// primary is SIGKILLed mid-run; its standby is promoted (here by the test,
// in a deployment by that group's witnesses) and runs transfers of its own.
// Failover is not migration: the placement map and its epoch never change.

struct Member {
  std::string dir;
  u32 group, member;
  u16 port_base;
  bool live_fence = false;  // group 1's primary stays alive and is fenced
};

std::string tag(u32 g, u32 m) { return std::to_string(g) + "." + std::to_string(m); }

// Every count a primary reports, written as one line to status.G.M.
struct Report {
  Totals t;
  ShardStats st;
  u64 acked;
  u64 epoch;
  u32 crc;
};

void write_report(const std::string& path, Engine& eng, u64 acked) {
  Report r{};
  r.t = totals(eng);
  for (u16 s = 0; s < kShards; ++s) {
    ShardStats x = eng.stats(s);
    r.st.node_emits_sent += x.node_emits_sent;
    r.st.node_emits_received += x.node_emits_received;
    r.st.node_emits_dropped += x.node_emits_dropped;
    r.st.node_resumes += x.node_resumes;
    r.st.node_connects += x.node_connects;
  }
  std::ostringstream o;
  o << r.t.total << " " << r.t.entries << " " << r.t.accounts << " " << r.st.node_emits_sent << " "
    << r.st.node_emits_received << " " << r.st.node_emits_dropped << " " << r.st.node_resumes << " "
    << r.st.node_connects << " " << acked << " " << eng.cluster().epoch << " " << eng.cluster().crc() << " "
    << eng.fenced();
  write_file(path, o.str());
}

// total, entries, accounts, sent, received, dropped, resumes, connects,
// acked, epoch, crc, fenced
std::vector<long long> read_report(const std::string& path) {
  std::istringstream in(read_file(path));
  std::vector<long long> v(12, -1);
  for (auto& x : v) in >> x;
  return v;
}

std::string transfer_line(const ObjectID& from, const ObjectID& to, i64 amount) {
  return from.to_string() + " " + to.to_string() + " " + std::to_string(amount) + "\n";
}

// Transfers from `mine` to any account, in rounds, until `finish` exists
// (and at least min_rounds). Each round's transfers are written to
// round.G.M before it starts; each acknowledged one is appended to `log`,
// each refused one to rejected.G.M. So after a kill, the transfers whose
// outcome is unknown are the last round's, less those in either file;
// `per` (per shard) bounds how many that can be. A primary that is fenced
// stops after the round it learns it in: what it had not acknowledged
// then is in doubt (E_FENCED), and stays among the unknown.
u64 transfer_rounds(Engine& eng, const Member& me, const std::vector<ObjectID>& mine,
                    const std::vector<ObjectID>& all, int log, u32 min_rounds, u64 per,
                    const std::string& progress) {
  const std::string t = tag(me.group, me.member);
  const int rejected = ::open((me.dir + "/rejected." + t).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  CHECK(rejected >= 0);
  std::mt19937_64 rng(me.group * 7919 + me.member * 31 + 1);
  u64 acked = 0;
  for (u32 round = 0; !eng.fenced() && (round < min_rounds || !exists(me.dir + "/finish")); ++round) {
    struct Pick {
      ObjectID from, to;
      i64 amount;
    };
    std::vector<std::vector<Pick>> picks(kShards);
    for (u16 s = 0; s < kShards; ++s) {
      for (u64 i = 0; i < per; ++i) {
        Pick p;
        p.from = mine[rng() % mine.size()];
        do p.to = all[rng() % all.size()];
        while (p.to == p.from);
        p.amount = 1 + static_cast<i64>(rng() % 50);
        picks[s].push_back(p);
      }
    }
    std::string round_text;
    for (auto& v : picks) {
      for (const Pick& p : v) round_text += transfer_line(p.from, p.to, p.amount);
    }
    write_file(me.dir + "/round." + t, round_text);
    run_all(
        eng, per,
        [&](u16 s, u64 i, Completion done, void* ctx) {
          const Pick& p = picks[s][i];
          TransferArgs t{p.to, p.amount, 7};
          execute(p.from, kOpTransfer, ByteView(&t, sizeof(t)), {}, done, ctx);
        },
        [&](u16 s, u64 i, const ExecResult& r) {
          const Pick& p = picks[s][i];
          const std::string line = transfer_line(p.from, p.to, p.amount);
          if (r.status == Status::OK) {
            CHECK_EQ(::write(log, line.data(), line.size()), static_cast<ssize_t>(line.size()));
            ++acked;
            return;
          }
          if (r.status == Status::E_FENCED) return;  // in doubt: unknown
          if (r.status != Status::E_OVERLOADED && r.status != Status::E_STANDBY) {
            CHECK_EQ(r.status, Status::E_APP);
            CHECK(r.app_code == kAppInsufficientFunds || r.app_code == kAppBusy);
          }
          CHECK_EQ(::write(rejected, line.data(), line.size()), static_cast<ssize_t>(line.size()));
        });
    write_file(progress, std::to_string(acked));
  }
  ::close(rejected);
  return acked;
}

struct Xfer {
  ObjectID from, to;
  i64 amount;
};

std::vector<Xfer> read_transfers(const std::string& path) {
  std::vector<Xfer> v;
  std::istringstream in(read_file(path));
  std::string from, to;
  i64 amount;
  while (in >> from >> to >> amount) v.push_back({*ObjectID::parse(from), *ObjectID::parse(to), amount});
  return v;
}

// Every account must equal its start plus every acknowledged transfer, plus
// some subset of the `unknown` ones (in flight when a primary was killed:
// each either committed or not, unacknowledged either way). Solved per
// set of unknown transfers that share accounts, by trying every subset.
// Returns how many unknown transfers committed; fails the test if no
// subset explains the balances (an acknowledged transfer lost, or money
// from nowhere).
u64 check_balances(const std::map<ObjectID, i64>& got, const std::vector<Xfer>& acked,
                   const std::vector<Xfer>& unknown) {
  std::map<ObjectID, i64> residual;
  for (auto& [id, bal] : got) residual[id] = bal - kStart;
  for (const Xfer& x : acked) {
    residual[x.from] += x.amount;
    residual[x.to] -= x.amount;
  }
  std::vector<u32> comp(unknown.size());
  for (u32 i = 0; i < unknown.size(); ++i) comp[i] = i;
  auto find = [&](u32 i) {
    while (comp[i] != i) i = comp[i] = comp[comp[i]];
    return i;
  };
  std::map<ObjectID, u32> first;
  for (u32 i = 0; i < unknown.size(); ++i) {
    for (const ObjectID& id : {unknown[i].from, unknown[i].to}) {
      auto [it, fresh] = first.emplace(id, i);
      if (!fresh) comp[find(i)] = find(it->second);
    }
  }
  std::map<u32, std::vector<u32>> parts;
  for (u32 i = 0; i < unknown.size(); ++i) parts[find(i)].push_back(i);
  u64 committed = 0;
  for (auto& [root, members] : parts) {
    CHECK(members.size() <= 16);
    std::set<ObjectID> touched;
    for (u32 i : members) {
      touched.insert(unknown[i].from);
      touched.insert(unknown[i].to);
    }
    bool found = false;
    for (u32 mask = 0; mask < (1u << members.size()) && !found; ++mask) {
      std::map<ObjectID, i64> d;
      for (u32 k = 0; k < members.size(); ++k) {
        if (!(mask >> k & 1)) continue;
        d[unknown[members[k]].from] -= unknown[members[k]].amount;
        d[unknown[members[k]].to] += unknown[members[k]].amount;
      }
      found = true;
      for (const ObjectID& id : touched) found = found && residual[id] == d[id];
      if (found) committed += static_cast<u64>(__builtin_popcount(mask));
    }
    CHECK(found);
    for (const ObjectID& id : touched) residual[id] = 0;
  }
  for (auto& [id, r] : residual) CHECK_EQ(r, 0);
  return committed;
}

[[noreturn]] void member_main(const Member& me) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);  // a standby: promote
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  const bool standby = me.member != 0;
  const std::string t = tag(me.group, me.member);
  Engine::Config c;
  c.runtime.n_shards = kShards;
  c.expected_objects = 4096;
  c.data_dir = me.dir + "/m" + t;
  c.init = true;
  c.init_uuid = Uuid{static_cast<u8>(0xA0 + me.group), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  c.wal_segment_bytes = 1u << 20;
  c.cluster = cluster(me.port_base, 2);
  c.group_id = static_cast<u16>(me.group);
  c.member = static_cast<u16>(me.member);
  if (standby) {
    c.standby = true;
    c.standby_host = "127.0.0.1";
    c.replica_port = repl_port(me.port_base, me.group);
  } else {
    c.replica_host = "127.0.0.1";
    c.replica_port = repl_port(me.port_base, me.group);
    c.replica_sync = true;
    c.replica_timeout_ms = 0;  // acks always wait for the standby: never degraded
  }
  ::mkdir(c.data_dir.c_str(), 0755);
  net::NetServer::Config nc;
  nc.base_port = c.cluster[me.group].members[me.member].client_port;
  net::NetServer server(nc, kShards);
  c.app_poll = net::NetServer::poll_hook;
  c.app_user = &server;
  Engine eng(c);
  eng.start();
  const u64 epoch0 = eng.cluster().epoch;
  const u32 crc0 = eng.cluster().crc();
  write_file(me.dir + "/up." + t, "");

  if (standby) {
    // Wait to be promoted, or to be stopped as a standby.
    timespec tick{0, 20'000'000};
    while (sigtimedwait(&set, nullptr, &tick) != SIGUSR1) {
      if (exists(me.dir + "/stop")) {
        eng.stop();
        _exit(0);
      }
    }
    const double s = eng.promote();
    write_file(me.dir + "/promoted." + t, std::to_string(s));
  } else {
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
    std::string text;
    for (const ObjectID& id : ids) {
      CHECK_EQ(eng.cluster().group_of_id(id), me.group);
      text += id.to_string() + "\n";
    }
    write_file(me.dir + "/ids." + std::to_string(me.group), text);
  }

  std::vector<std::vector<ObjectID>> ids(kNodes);
  for (u32 n = 0; n < kNodes; ++n) {
    while (!exists(me.dir + "/ids." + std::to_string(n))) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ids[n] = read_ids(me.dir, n);
    CHECK_EQ(ids[n].size(), kAccounts);
  }
  std::vector<ObjectID> all;
  for (auto& v : ids) all.insert(all.end(), v.begin(), v.end());

  const int log = ::open((me.dir + "/acked." + t).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  CHECK(log >= 0);
  write_file(me.dir + "/busy." + t, "");
  // Group 1's first primary, the one killed, keeps few transfers in flight,
  // so the check below can try every outcome of those it never reported.
  const u64 per = me.group == 1 && me.member == 0 ? 6 : 100;
  const u64 acked =
      transfer_rounds(eng, me, ids[me.group], all, log, standby ? 10 : 20, per, me.dir + "/progress." + t);
  ::close(log);
  if (me.live_fence && me.group == 1 && me.member == 0) {
    // Still running, but replaced: it must have left the data plane, and
    // it refuses work of any kind from here on.
    CHECK(eng.fenced());
    std::atomic<u32> refused{0};
    run_all(
        eng, 4,
        [&](u16, u64 i, Completion done, void* ctx) {
          if (i % 2) {
            execute(ids[1][i], kOpGetBalance, ByteView(), {}, done, ctx);
          } else {
            CreateArgs a{};
            a.balance_cents = 1;
            create(kTypeId, ByteView(&a, sizeof(a)), {}, done, ctx);
          }
        },
        [&](u16, u64, const ExecResult& r) { refused += r.status == Status::E_STANDBY; });
    CHECK_EQ(refused.load(), 4u * kShards);
    write_file(me.dir + "/fenced." + t, std::to_string(acked));
  }
  write_file(me.dir + "/done." + t, "");
  while (!exists(me.dir + "/stop")) {
    write_report(me.dir + "/status." + t, eng, acked);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  write_report(me.dir + "/status." + t, eng, acked);
  // The placement never moved under this node, failover or not.
  CHECK_EQ(eng.cluster().epoch, epoch0);
  CHECK_EQ(eng.cluster().crc(), crc0);
  std::map<ObjectID, i64> balance;
  totals(eng, &balance);
  std::string b;
  for (auto& [id, v] : balance) b += id.to_string() + " " + std::to_string(v) + "\n";
  write_file(me.dir + "/balances." + t, b);
  eng.stop();
  _exit(0);
}

pid_t spawn_member(const Member& me) {
  std::fflush(stdout);
  pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) member_main(me);
  return pid;
}

u64 progress_of(const std::string& dir, u32 g, u32 m) {
  const std::string text = read_file(dir + "/progress." + tag(g, m));
  return text.empty() ? 0 : std::stoull(text);
}

// live_fence: group 1's primary is not killed. Its standby is promoted
// while it runs; the promoted member refuses its term on the replication
// port, and it withdraws from the data plane (docs/CLUSTER.md, "A fenced
// primary withdraws"). The other groups' links must then find the new
// primary by themselves, as after a crash.
bool can_connect(u16 port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
  ::close(fd);
  return ok;
}

void test_group_failover(bool live_fence) {
  char tmpl[] = "/tmp/snc-groups-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  const u16 port_base = free_ports(26000, 32000, kNodes * 40);
  const ClusterMap map = ClusterMap::make(cluster(port_base, 2));
  // pids[g][m]; standbys first, so every primary's first ack has one.
  std::vector<std::vector<pid_t>> pids(kNodes, std::vector<pid_t>(2, -1));
  for (u32 g = 0; g < kNodes; ++g) {
    pids[g][1] = spawn_member({dir, g, 1, port_base, live_fence});
    wait_for(dir + "/up." + tag(g, 1), 30);
  }
  for (u32 g = 0; g < kNodes; ++g) pids[g][0] = spawn_member({dir, g, 0, port_base, live_fence});

  // Group 1's primary dies in the middle of its transfers, and of the
  // others' transfers to it.
  wait_for(dir + "/busy.1.0", 30);
  for (u32 g = 0; g < kNodes; ++g) wait_for(dir + "/progress." + tag(g, 0), 30);
  int st = 0;
  if (!live_fence) {
    ::kill(pids[1][0], SIGKILL);
    ::waitpid(pids[1][0], &st, 0);
    CHECK(WIFSIGNALED(st));
    pids[1][0] = -1;
    // The other groups carry on without it, and without its slots' owner.
    const u64 before0 = progress_of(dir, 0, 0), before2 = progress_of(dir, 2, 0);
    ::usleep(400000);
    const u64 during0 = progress_of(dir, 0, 0), during2 = progress_of(dir, 2, 0);
    std::printf("group 1's primary killed: groups 0 and 2 acknowledged %llu and %llu more transfers while it was down\n",
                (unsigned long long)(during0 - before0), (unsigned long long)(during2 - before2));
    CHECK(during0 > before0);
    CHECK(during2 > before2);
  }

  const auto t_promote = std::chrono::steady_clock::now();
  ::kill(pids[1][1], SIGUSR1);
  wait_for(dir + "/promoted.1.1", 30);
  std::printf("group 1's standby promoted in %s s%s\n", read_file(dir + "/promoted.1.1").c_str(),
              live_fence ? ", its old primary still running" : "");
  if (live_fence) {
    wait_for(dir + "/fenced.1.0", 30);
    std::printf("old primary fenced and withdrawn %.3f s after the promotion signal, having acknowledged %s transfers\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_promote).count(),
                read_file(dir + "/fenced.1.0").c_str());
    // Its node-link listeners are closed (the others' links can only find
    // the new primary), while the groups that never failed keep working.
    for (u16 s = 0; s < kShards; ++s) CHECK(!can_connect(static_cast<u16>(map.groups[1].members[0].link_port + s)));
    const u64 before0 = progress_of(dir, 0, 0), before2 = progress_of(dir, 2, 0);
    ::usleep(300000);
    CHECK(progress_of(dir, 0, 0) > before0);
    CHECK(progress_of(dir, 2, 0) > before2);
  }
  // Its new primary runs transfers of its own, then everyone finishes.
  wait_for(dir + "/progress.1.1", 60);
  write_file(dir + "/finish", "");
  const std::vector<std::pair<u32, u32>> primaries = {{0, 0}, {1, 1}, {2, 0}};
  for (auto [g, m] : primaries) wait_for(dir + "/done." + tag(g, m), 120);

  // Settled: no transfer half done anywhere, twice in a row.
  const auto t0 = std::chrono::steady_clock::now();
  u32 settled = 0;
  std::vector<std::vector<long long>> rep(kNodes);
  while (settled < 2) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) {
      write_file(dir + "/stop", "");
      std::fprintf(stderr, "never settled; see %s\n", dir.c_str());
      CHECK(false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    u64 entries = 0;
    for (auto [g, m] : primaries) {
      rep[g] = read_report(dir + "/status." + tag(g, m));
      entries += rep[g][1] < 0 ? 1 : static_cast<u64>(rep[g][1]);
    }
    settled = entries == 0 ? settled + 1 : 0;
  }
  // A client that knew nothing of the failover reads every account from
  // the primary of its group now, by the same map.
  check_routing(dir, port_base, 2, 0);
  if (live_fence) {
    // The old primary is still running and still fenced: links closed,
    // clients refused with E_STANDBY (a session client moves on to the new
    // primary), placement untouched.
    std::vector<long long> old = read_report(dir + "/status.1.0");
    CHECK_EQ(old[11], 1);
    CHECK_EQ(old[9], 1);
    CHECK_EQ(static_cast<u32>(old[10]), map.crc());
    for (u16 s = 0; s < kShards; ++s) CHECK(!can_connect(static_cast<u16>(map.groups[1].members[0].link_port + s)));
    net::Client c;
    CHECK(c.connect("127.0.0.1", map.groups[1].members[0].client_port));
    const ObjectID target = read_ids(dir, 1)[0];
    wire::ReqHeader h{};
    h.kind = wire::Kind::EXEC;
    h.op = kOpGetBalance;
    h.req_id = 1;
    h.target = target;
    c.send(c.owner_of(target), h, ByteView());
    Status got = Status::OK;
    int r = 0;
    for (int i = 0; i < 50 && r == 0; ++i) {
      r = c.poll([](void* ctx, const wire::RespHeader& rh, ByteView) { *static_cast<Status*>(ctx) = static_cast<Status>(rh.status); },
                 &got, 100);
    }
    CHECK_EQ(r, 1);
    CHECK_EQ(got, Status::E_STANDBY);
    std::printf("old primary still running: fenced, node links closed, clients answered E_STANDBY\n");
  }
  write_file(dir + "/stop", "");
  for (auto& v : pids) {
    for (pid_t p : v) {
      if (p < 0) continue;
      ::waitpid(p, &st, 0);
      CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
  }
  i64 total = 0;
  u64 accounts = 0;
  for (auto [g, m] : primaries) {
    rep[g] = read_report(dir + "/status." + tag(g, m));
    total += rep[g][0];
    accounts += static_cast<u64>(rep[g][2]);
    std::printf("group %u (member %u): %lld transfers acknowledged, %lld follow-ups sent to other groups, %lld "
                "received, %lld dropped, %lld resumes, %lld connects\n",
                g, m, rep[g][8], rep[g][3], rep[g][4], rep[g][5], rep[g][6], rep[g][7]);
    // Placement epoch 1 and the same map throughout, on every primary.
    CHECK_EQ(rep[g][9], 1);
    CHECK_EQ(static_cast<u32>(rep[g][10]), map.crc());
  }
  // The promoted member took follow-ups from the other groups' links and
  // ran transfers of its own.
  CHECK(rep[1][4] > 0);
  CHECK(rep[1][8] > 0);
  CHECK_EQ(accounts, u64{kNodes} * kAccounts);
  CHECK_EQ(total, kStart * static_cast<i64>(kNodes * kAccounts));

  // Every account: its start plus every transfer acknowledged, by any
  // primary of any group, the killed one included (what it acknowledged was
  // on its sync standby). The only freedom is the killed primary's
  // transfers whose outcome it never reported: each committed or not.
  std::vector<Xfer> acked;
  for (u32 g = 0; g < kNodes; ++g) {
    for (u32 m = 0; m < 2; ++m) {
      std::vector<Xfer> v = read_transfers(dir + "/acked." + tag(g, m));
      acked.insert(acked.end(), v.begin(), v.end());
    }
  }
  std::map<std::string, i64> reported;
  for (const char* f : {"/acked.1.0", "/rejected.1.0"}) {
    for (const Xfer& x : read_transfers(dir + f)) ++reported[transfer_line(x.from, x.to, x.amount)];
  }
  std::vector<Xfer> unknown;
  for (const Xfer& x : read_transfers(dir + "/round.1.0")) {
    i64& n = reported[transfer_line(x.from, x.to, x.amount)];
    if (n > 0) --n;
    else unknown.push_back(x);
  }
  std::map<ObjectID, i64> got;
  for (auto [g, m] : primaries) {
    std::istringstream b(read_file(dir + "/balances." + tag(g, m)));
    std::string id;
    i64 bal;
    while (b >> id >> bal) got[*ObjectID::parse(id)] = bal;
  }
  CHECK_EQ(got.size(), u64{kNodes} * kAccounts);
  const u64 committed = check_balances(got, acked, unknown);
  std::printf("group failover (%s): %zu transfers acknowledged, every one present; %zu in flight at the %s, %llu "
              "of them committed; money conserved; placement epoch 1 throughout\n",
              live_fence ? "old primary alive, fenced" : "old primary killed", acked.size(), unknown.size(),
              live_fence ? "fence" : "kill", (unsigned long long)committed);
  std::string cmd = "rm -rf '" + dir + "'";
  CHECK_EQ(std::system(cmd.c_str()), 0);
}

}  // namespace

int main() {
  register_type();
  test_map();
  test_kill_restart(false);
  test_kill_restart(true);
  test_group_failover(false);
  test_group_failover(true);
  std::printf("test_cluster: ok\n");
  return 0;
}
