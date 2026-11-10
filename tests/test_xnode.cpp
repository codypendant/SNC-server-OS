// D.2: cross-node exchange accounting (docs/CROSS_NODE_EXCHANGE_DESIGN.md
// rev4, section 8). Engines in one process over loopback; every link
// between two engines goes through a LinkProxy, so a test can cut, hold,
// stall or drop either direction of any connection.
//
// Usage: test_xnode [NAME...]   (runs the named tests; none: all of them)
//
// Each test prints one line of evidence. Common to all: at quiescence every
// node's xnode counts are 0 (remote live, held for peers, END owed), its
// exchanges are 0, every transfer settled, money conserved, and the
// must-stay-0 counters (end and held underflows, debt increases, uncounted
// items) are 0.
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/links_generation.h"
#include "../src/core/node_link.h"
#include "../src/core/shard.h"
#include "harness.h"
#include "link_proxy.h"
#include "snc/cluster.h"
#include "snc/gauges.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::test;
using namespace snc::account;

#undef CHECK
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      std::fflush(stdout);                                                            \
      std::fflush(stderr);                                                            \
      std::_Exit(1);                                                                  \
    }                                                                                 \
  } while (0)
#define CHECK_MSG(cond, ...)                                                          \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #cond);   \
      std::fprintf(stderr, __VA_ARGS__);                                              \
      std::fprintf(stderr, "\n");                                                     \
      std::fflush(stdout);                                                            \
      std::fflush(stderr);                                                            \
      std::_Exit(1);                                                                  \
    }                                                                                 \
  } while (0)

namespace {

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
void sleep_ms(u32 ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Fast timers for tests (they meet the engine's ordering, X18).
constexpr u32 kHbMs = 20, kDeadMs = 200, kReleaseMs = 600, kBlockMs = 300;

std::string g_root;
void rm_rf(const std::string& d) { CHECK_EQ(std::system(("rm -rf '" + d + "'").c_str()), 0); }

// ---- SYNC probes (X3, X4, X24, X27, X28) ------------------------------------------------

std::mutex g_probe_mu;
std::vector<NodeLinks::SyncProbe> g_probes;
void on_probe(const NodeLinks::SyncProbe& p) {
  std::lock_guard<std::mutex> g(g_probe_mu);
  g_probes.push_back(p);
}
std::vector<NodeLinks::SyncProbe> probes() {
  std::lock_guard<std::mutex> g(g_probe_mu);
  return g_probes;
}
void clear_probes() {
  std::lock_guard<std::mutex> g(g_probe_mu);
  g_probes.clear();
}

// ---- A cluster of engines, every link through a proxy -----------------------------------

struct NodeSpec {
  u16 group = 0, member = 0;
  bool durable = true;
  bool accounting = true;
  int standby_of = -1;  // node index of the primary this node is the standby of
};

struct Node {
  NodeSpec spec;
  u16 index = 0;
  u16 port = 0;       // its own link port (shard 0; shard s at port + s)
  u16 repl_port = 0;  // as a standby: where it listens for its primary
  std::string dir;
  Engine::Config cfg;
  std::unique_ptr<Engine> eng;
  bool promoted = false;
  bool up() const { return eng != nullptr; }
  // Up and serving (a standby serves nothing until promoted).
  bool serving() const { return eng != nullptr && (spec.standby_of < 0 || promoted); }
};

struct Net {
  std::vector<Node> nodes;
  std::vector<u16> members;  // per group
  std::vector<u16> shards;   // per group
  // proxies[{d, t, s}]: what node d dials to reach node t's shard s.
  std::map<std::tuple<u16, u16, u16>, std::unique_ptr<LinkProxy>> proxies;
  std::map<std::tuple<u16, u16, u16>, u16> proxy_port;
  // repl[p]: primary p's replication connection to its standby, shard 0.
  std::map<u16, std::unique_ptr<LinkProxy>> repl;
  std::map<u16, u16> repl_proxy_port;
  u32 exchange_budget = 64;
  u32 link_hold_max = 1024;
  u32 hb_ms = kHbMs, dead_ms = kDeadMs, release_ms = kReleaseMs, block_ms = kBlockMs;
  std::function<void(Engine::Config&, const Node&)> tweak;
  std::string root;
  // Held shared by Load while it submits to an engine; exclusively while an
  // engine is replaced. restarts: how many times one was (requests in
  // flight on a stopped engine are never completed).
  std::shared_mutex life_mu;
  std::atomic<u64> restarts{0};
  // The placement every node is given at (re)start, and updated to by move().
  std::array<u8, kSlots> slots{};
  u64 epoch = 1;
  std::array<SlotArrival, kSlots> arrived{};

  LinkProxy& px(u16 dialer, u16 target, u16 shard = 0) { return *proxies.at({dialer, target, shard}); }
  // Every proxy carrying d's Out connections to t (all shards).
  void each(u16 d, u16 t, const std::function<void(LinkProxy&)>& f) {
    for (u16 s = 0; s < shards[nodes[t].spec.group]; ++s) f(px(d, t, s));
  }

  void build(const std::vector<NodeSpec>& specs, std::vector<u16> group_shards = {}, u32 rcvbuf = 0) {
    root = g_root + "/net" + std::to_string(++g_nets);
    CHECK_EQ(::mkdir(root.c_str(), 0755), 0);
    u16 ngroups = 0;
    for (const NodeSpec& s : specs) ngroups = std::max<u16>(ngroups, s.group + 1);
    members.assign(ngroups, 0);
    shards = group_shards;
    shards.resize(ngroups, 1);
    for (const NodeSpec& s : specs) members[s.group] = std::max<u16>(members[s.group], s.member + 1);
    const u16 n = static_cast<u16>(specs.size());
    const u16 base = free_ports(20000, 32000, static_cast<u16>(8 * n + 4 * n * n + 8));
    u16 next = base;
    nodes.resize(n);
    for (u16 i = 0; i < n; ++i) {
      nodes[i].spec = specs[i];
      nodes[i].index = i;
      nodes[i].port = next;
      next = static_cast<u16>(next + 4);
      nodes[i].repl_port = next;
      next = static_cast<u16>(next + 4);
      nodes[i].dir = root + "/n" + std::to_string(i);
    }
    for (u16 d = 0; d < n; ++d) {
      for (u16 t = 0; t < n; ++t) {
        if (nodes[d].spec.group == nodes[t].spec.group) continue;
        for (u16 s = 0; s < shards[nodes[t].spec.group]; ++s) {
          proxy_port[{d, t, s}] = next;
          proxies[{d, t, s}] = std::make_unique<LinkProxy>(next, static_cast<u16>(nodes[t].port + s), rcvbuf);
          ++next;
        }
        next = static_cast<u16>(next + 4 - shards[nodes[t].spec.group] % 4);
      }
    }
    for (u16 i = 0; i < n; ++i) {
      if (nodes[i].spec.standby_of < 0) continue;
      const u16 p = static_cast<u16>(nodes[i].spec.standby_of);
      CHECK_EQ(shards[nodes[i].spec.group], 1);
      repl_proxy_port[p] = next;
      repl[p] = std::make_unique<LinkProxy>(next, nodes[i].repl_port);
      ++next;
    }
    unused_port_ = next;
    const ClusterMap m = ClusterMap::make(config(nodes[0], true).cluster);
    slots = m.group_of;
  }

  Engine::Config config(const Node& nd, bool init) {
    Engine::Config c;
    c.runtime.n_shards = shards.empty() ? 1 : shards[nd.spec.group];
    c.expected_objects = 1 << 14;
    c.wal_segment_bytes = 4u << 20;
    c.exchange_budget = exchange_budget;
    c.link_hold_max = link_hold_max;
    c.xnode_accounting = nd.spec.accounting;
    c.xnode_hb_ms = hb_ms;
    c.xnode_dead_ms = dead_ms;
    c.xnode_release_ms = release_ms;
    c.xnode_block_ms = block_ms;
    if (nd.spec.durable) {
      c.data_dir = nd.dir;
      c.init = init;
    } else {
      c.links_generation_test = true;
    }
    c.group_id = nd.spec.group;
    c.member = nd.spec.member;
    // One space per group: members share its UUID (a standby's is its primary's).
    c.init_uuid = Uuid{static_cast<u8>(0xB0 + nd.spec.group), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (u16 g = 0; g < members.size(); ++g) {
      ClusterGroup grp;
      grp.n_shards = shards.empty() ? 1 : shards[g];
      for (u16 m = 0; m < members[g]; ++m) {
        u16 port = unused_port_;
        const Node* t = nullptr;
        for (const Node& x : nodes) {
          if (x.spec.group == g && x.spec.member == m) t = &x;
        }
        // Its own member: its own port (two engines may claim one member:
        // X13, X21). Another group's: through the proxy.
        if (g == nd.spec.group && m == nd.spec.member) port = nd.port;
        else if (t != nullptr) port = g == nd.spec.group ? t->port : proxy_port.at({nd.index, t->index, 0});
        grp.members.push_back({"127.0.0.1", port, 0});
      }
      c.cluster.push_back(grp);
    }
    if (epoch > 1) {
      c.cluster_slots.assign(slots.begin(), slots.end());
      c.cluster_epoch = epoch;
      c.cluster_arrived.assign(arrived.begin(), arrived.end());
    }
    for (const Node& x : nodes) {
      if (x.spec.standby_of == nd.index) {  // nd is a primary with a standby
        c.replica_host = "127.0.0.1";
        c.replica_port = repl_proxy_port.at(nd.index);
        c.replica_sync = true;
        c.replica_timeout_ms = 0;  // never degrade: blocked while the standby is gone
      }
    }
    if (nd.spec.standby_of >= 0) {
      c.standby = true;
      c.standby_host = "127.0.0.1";
      c.replica_port = nd.repl_port;
    }
    if (tweak) tweak(c, nd);
    return c;
  }

  void start(u16 i, bool init = true) {
    Node& nd = nodes[i];
    if (init && nd.spec.durable) CHECK_EQ(::mkdir(nd.dir.c_str(), 0755), 0);
    nd.cfg = config(nd, init);
    auto e = std::make_unique<Engine>(nd.cfg);
    e->start();
    std::unique_lock<std::shared_mutex> g(life_mu);
    nd.eng = std::move(e);
  }
  void start_all() {
    // Standbys first: their primaries connect to them.
    for (u16 i = 0; i < nodes.size(); ++i) {
      if (nodes[i].spec.standby_of >= 0) start(i);
    }
    for (u16 i = 0; i < nodes.size(); ++i) {
      if (nodes[i].spec.standby_of < 0) start(i);
    }
  }
  void stop(u16 i) {
    std::unique_lock<std::shared_mutex> g(life_mu);
    if (!nodes[i].eng) return;
    ++restarts;
    nodes[i].eng->stop();
    nodes[i].eng.reset();
  }
  // A restart of the same member: recovered from its data directory (or a
  // new in-memory instance), a new links incarnation either way.
  void restart(u16 i) {
    stop(i);
    start(i, /*init=*/!nodes[i].spec.durable);
  }
  // Adopt the current placement on every running node.
  void publish_placement() {
    for (Node& n : nodes) {
      if (n.serving()) n.eng->update_placement(slots, epoch, &arrived);
    }
  }
  ~Net() {
    for (u16 i = 0; i < nodes.size(); ++i) stop(i);
    proxies.clear();
    repl.clear();
    if (!root.empty()) rm_rf(root);
  }

 private:
  u16 unused_port_ = 0;
  static inline u32 g_nets = 0;
};

// ---- Reading a node -----------------------------------------------------------------------

void on_shard(Engine& eng, u16 shard, const std::function<void()>& fn) {
  std::latch l(1);
  const Engine::DataPlane dp = eng.data_plane();
  CHECK(dp);
  dp.submit(shard, [&] {
    fn();
    l.count_down();
  });
  l.wait();
}

GaugeSnapshot gauges(Engine& eng, u16 shard = 0) {
  on_shard(eng, shard, [] {
    const u64 now = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    current_shard()->publish_gauges(now, true);
  });
  for (int i = 0; i < 100000; ++i) {
    GaugeSnapshot g = eng.gauges(shard);
    if (g.consistent) return g;
  }
  CHECK(false);
  return {};
}

// One entry's state and our Out connection on its link, read on the shard.
struct EntryView {
  bool links = false;
  NodeLinks::Entry e{};
  NodeLinks::OutView out{};
  u32 ins = 0;
  u64 xnode_live = 0, debt = 0;
  u64 blocked_since = 0;  // the peer member's blocked bit, as we hold it
  NodeLinks::Counters c{};
};
EntryView entry(Engine& eng, u16 group, u16 member, u16 shard = 0, u16 at = 0) {
  EntryView v;
  on_shard(eng, at, [&] {
    const NodeLinks* L = current_shard()->links_for_test();
    if (L == nullptr) return;
    v.links = true;
    const u32 l = L->link_of(group, shard);
    v.e = L->entry(l, member);
    v.out = L->out_view(l);
    v.ins = L->ins_from(l, member);
    v.blocked_since = L->peer_blocked_since(l, member);
    v.xnode_live = L->xnode_live();
    v.debt = L->debt();
    v.c = L->totals();
  });
  return v;
}

// ---- Accounts and transfers ----------------------------------------------------------------

constexpr i64 kBalance = 1'000'000;

ObjectID make_account(Engine& eng, u16 shard = 0) {
  CreateArgs a{};
  a.balance_cents = kBalance;
  a.created_ns = 1;
  a.email_len = 1;
  a.email[0] = 'x';
  a.name_len = 1;
  a.name[0] = 'n';
  OneResult r =
      one(eng, shard, [&](Completion d, void* ctx) { create(kTypeId, ByteView(&a, sizeof(a)), {}, d, ctx); });
  CHECK_EQ(r.r.status, Status::OK);
  return r.r.id;
}

struct AccountState {
  bool found = false;
  i64 balance = 0;
  u16 n_out = 0, n_in = 0;
};
AccountState read_account(Engine& eng, const ObjectID& id) {
  OneResult r = one(eng, 0, [&](Completion d, void* ctx) {
    ExecOptions o;
    o.allow_undurable = true;
    execute(id, kOpGet, ByteView{}, o, d, ctx);
  });
  AccountState s;
  if (r.r.status != Status::OK) return s;
  CHECK(r.bytes.size() >= sizeof(AccountV1));
  AccountV1 a;
  std::memcpy(&a, r.bytes.data(), sizeof(a));
  s.found = true;
  s.balance = a.balance_cents;
  s.n_out = a.n_out;
  s.n_in = a.n_in;
  return s;
}

// Accounts per node, created at the start (on durable nodes they survive
// restarts).
struct Ledger {
  std::vector<std::vector<ObjectID>> acc;  // per node
};
// shard_of: the shard each node's accounts are made on (default 0).
Ledger make_ledger(Net& net, u32 per_node, std::vector<u16> shard_of = {}) {
  Ledger L;
  L.acc.resize(net.nodes.size());
  shard_of.resize(net.nodes.size(), 0);
  for (Node& n : net.nodes) {
    if (!n.serving()) continue;
    for (u32 i = 0; i < per_node; ++i) L.acc[n.index].push_back(make_account(*n.eng, shard_of[n.index]));
  }
  return L;
}

// Transfers from node `from`'s accounts to node `to`'s (or its own, when
// to == from: intra-node), with a window, until stopped.
class Load {
 public:
  // may_lose: requests may stay unanswered when stopped (a blocked primary
  // answers nothing until it unblocks).
  Load(Net& net, const Ledger& L, u16 from, u16 to, u32 window = 32, bool may_lose = false)
      : net_(net), L_(L), from_(from), to_(to), w_(window), may_lose_(may_lose) {
    th_ = std::thread([this] { run(); });
  }
  ~Load() { stop(); }
  void stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
  }
  u64 ok() const { return st_->ok; }
  u64 overloaded() const { return st_->overloaded; }
  u64 other() const { return st_->other; }
  u64 lost() const { return lost_; }

 private:
  void run() {
    std::atomic<u64>& outstanding = st_->outstanding;
    u64 k = 0;
    const u64 restarts0 = net_.restarts.load();
    while (!stop_) {
      std::shared_lock<std::shared_mutex> life(net_.life_mu);
      Node& n = net_.nodes[from_];
      if (!n.up() || !net_.nodes[to_].up()) {
        life.unlock();
        sleep_ms(2);
        continue;
      }
      if (outstanding.load() >= w_) {
        life.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      const auto& src = L_.acc[from_];
      const auto& dst = L_.acc[to_];
      const ObjectID a = src[k % src.size()];
      ObjectID b = dst[(k * 7 + 3) % dst.size()];
      if (b == a) b = dst[(k * 7 + 4) % dst.size()];
      ++k;
      ++outstanding;
      struct Ctx {
        std::shared_ptr<State> st;  // outlives this Load if a late completion comes
      };
      auto* c = new Ctx{st_};
      const Engine::DataPlane dp = n.eng->data_plane();
      if (!dp) {
        --outstanding;
        delete c;
        continue;
      }
      dp.submit(0, [a, b, c] {
        TransferArgs t{b, 1, 7};
        execute(a, kOpTransfer, ByteView(&t, sizeof(t)), {},
                [](void* x, const ExecResult& r) {
                  auto* cc = static_cast<Ctx*>(x);
                  if (r.status == Status::OK) ++cc->st->ok;
                  else if (r.status == Status::E_OVERLOADED) ++cc->st->overloaded;
                  else ++cc->st->other;
                  --cc->st->outstanding;
                  delete cc;
                },
                c);
      });
    }
    for (int i = 0; i < 20000 && outstanding.load() != 0; ++i) {
      if ((may_lose_ || net_.restarts.load() != restarts0) && i >= 3000) break;  // lost with a stopped engine
      sleep_ms(1);
    }
    lost_ = outstanding.load();
    CHECK(lost_ == 0 || may_lose_ || net_.restarts.load() != restarts0);
  }
  Net& net_;
  const Ledger& L_;
  u16 from_, to_;
  u64 w_;
  bool may_lose_ = false;
  std::thread th_;
  std::atomic<bool> stop_{false};
  struct State {
    std::atomic<u64> ok{0}, overloaded{0}, other{0}, outstanding{0};
  };
  std::shared_ptr<State> st_ = std::make_shared<State>();
  u64 lost_ = 0;
};

// ---- Quiescence ----------------------------------------------------------------------------

struct Totals {
  i64 money = 0;
  u64 entries = 0;  // outbox + inbox entries left
  u64 found = 0;
};
Totals ledger_totals(Net& net, const Ledger& L) {
  Totals t;
  for (Node& n : net.nodes) {
    if (!n.serving()) continue;
    for (const ObjectID& id : L.acc[n.index]) {
      const AccountState s = read_account(*n.eng, id);
      if (!s.found) continue;
      ++t.found;
      t.money += s.balance;
      t.entries += s.n_out + s.n_in;
    }
  }
  return t;
}

// Every must-stay-0 counter on node i.
void check_zero_counters(Node& n, const char* when) {
  for (u16 sh = 0; sh < n.eng->shard_count(); ++sh) {
    const GaugeSnapshot g = gauges(*n.eng, sh);
    CHECK_MSG(g[G_XNODE_END_UNDERFLOW] == 0, "%s: node %u end underflow %llu", when, n.index,
              (unsigned long long)g[G_XNODE_END_UNDERFLOW]);
    CHECK_MSG(g[G_XNODE_HELD_UNDERFLOW] == 0, "%s: node %u held underflow", when, n.index);
    CHECK_MSG(g[G_XNODE_DEBT_INCREASES] == 0, "%s: node %u debt increases", when, n.index);
    CHECK_MSG(g[G_XNODE_UNCOUNTED] == 0, "%s: node %u uncounted", when, n.index);
    CHECK_MSG(g[G_XNODE_ENTRIES] <= g[G_XNODE_ENTRY_SLOTS], "%s: node %u entries %llu > slots %llu", when, n.index,
              (unsigned long long)g[G_XNODE_ENTRIES], (unsigned long long)g[G_XNODE_ENTRY_SLOTS]);
  }
}

// Waits until every transfer settled and every node's counts are 0;
// checks money. `debt_zero`: quarantined debt must be 0 too (no dead
// instance holds one).
void settle(Net& net, const Ledger& L, const char* what, double timeout_s = 60, bool debt_zero = true) {
  const auto t0 = Clock::now();
  u64 expect_found = 0;
  for (Node& n : net.nodes) {
    if (n.serving()) expect_found += L.acc[n.index].size();
  }
  const i64 expect_money = static_cast<i64>(expect_found) * kBalance;
  int stable = 0;
  std::string why;
  while (true) {
    for (Node& n : net.nodes) {
      if (n.serving()) n.eng->wait_emits(1);
    }
    const Totals t = ledger_totals(net, L);
    bool quiet = t.entries == 0 && t.found == expect_found;
    why = "entries " + std::to_string(t.entries);
    for (Node& n : net.nodes) {
      if (!n.serving() || !quiet) continue;
      for (u16 sh = 0; sh < n.eng->shard_count() && quiet; ++sh) {
      const GaugeSnapshot g = gauges(*n.eng, sh);
      const ShardStats s = n.eng->stats(sh);
      const bool z = g[G_XNODE_REMOTE_LIVE] == 0 && g[G_XNODE_HELD_FOR_PEERS] == 0 && g[G_XNODE_END_OWED] == 0 &&
                     s.exchanges_live == 0 && s.emits_in_flight == 0 && (!debt_zero || g[G_XNODE_DEBT] == 0);
      if (!z) {
        quiet = false;
        why = "node " + std::to_string(n.index) + " remote_live " + std::to_string(g[G_XNODE_REMOTE_LIVE]) +
              " held " + std::to_string(g[G_XNODE_HELD_FOR_PEERS]) + " end_owed " +
              std::to_string(g[G_XNODE_END_OWED]) + " debt " + std::to_string(g[G_XNODE_DEBT]) + " live " +
              std::to_string(s.exchanges_live) + " in_flight " + std::to_string(s.emits_in_flight);
      }
      }
    }
    if (quiet) {
      CHECK_MSG(t.money == expect_money, "%s: money %lld, expected %lld", what, (long long)t.money,
                (long long)expect_money);
      if (++stable >= 3) break;
    } else {
      stable = 0;
    }
    CHECK_MSG(since(t0) < timeout_s, "%s: never settled (%s)", what, why.c_str());
    sleep_ms(20);
  }
  for (Node& n : net.nodes) {
    if (n.serving()) check_zero_counters(n, what);
  }
}

// Two live members of one group (split brain on purpose: a fenced primary
// alive, X13, X16, X20, X21, X26) hold different objects, so money is not
// checked there; only the accounting: every live node's counts return to
// 0 (remote live, held for peers, END owed; debt where `debt_zero`) and
// its exchanges end.
void settle_counts(Net& net, const char* what, double timeout_s = 60, bool debt_zero = false) {
  const auto t0 = Clock::now();
  int stable = 0;
  std::string why;
  while (true) {
    bool quiet = true;
    for (Node& n : net.nodes) {
      if (!n.serving()) continue;
      n.eng->wait_emits(1);
      for (u16 sh = 0; sh < n.eng->shard_count(); ++sh) {
        const GaugeSnapshot g = gauges(*n.eng, sh);
        const ShardStats st = n.eng->stats(sh);
        const bool z = g[G_XNODE_REMOTE_LIVE] == 0 && g[G_XNODE_HELD_FOR_PEERS] == 0 && g[G_XNODE_END_OWED] == 0 &&
                       st.exchanges_live == 0 && (!debt_zero || g[G_XNODE_DEBT] == 0);
        if (!z) {
          quiet = false;
          why = "node " + std::to_string(n.index) + " shard " + std::to_string(sh) + " remote_live " +
                std::to_string(g[G_XNODE_REMOTE_LIVE]) + " held " + std::to_string(g[G_XNODE_HELD_FOR_PEERS]) +
                " end_owed " + std::to_string(g[G_XNODE_END_OWED]) + " debt " + std::to_string(g[G_XNODE_DEBT]) +
                " live " + std::to_string(st.exchanges_live);
        }
      }
    }
    stable = quiet ? stable + 1 : 0;
    if (stable >= 3) break;
    CHECK_MSG(since(t0) < timeout_s, "%s: counts never settled (%s)", what, why.c_str());
    sleep_ms(20);
  }
  for (Node& n : net.nodes) {
    if (n.serving()) check_zero_counters(n, what);
  }
}

// Waits for `pred`, failing the test after `timeout_s`.
void wait_until(const std::function<bool()>& pred, const char* what, double timeout_s = 30) {
  const auto t0 = Clock::now();
  while (!pred()) {
    CHECK_MSG(since(t0) < timeout_s, "timed out: %s", what);
    sleep_ms(2);
  }
}

bool bound_to(Engine& eng, u16 group, u16 member) {
  const EntryView v = entry(eng, group, member);
  return v.links && v.out.bound && v.out.member == member;
}
// Bound again on a connection other than `old_conn`.
void wait_rebound(Engine& eng, u16 group, u16 member, u64 old_conn, const char* what, double timeout_s = 30) {
  wait_until([&] {
    const EntryView v = entry(eng, group, member);
    return v.links && v.out.bound && v.out.member == member && v.out.conn != old_conn;
  }, what, timeout_s);
}
u64 out_conn(Engine& eng, u16 group) { return entry(eng, group, 0).out.conn; }

// Every applied SYNC matches its barrier exactly and obeys
// count = k + pushed - R.
u64 check_probes(const char* what, u64* bindings_with_zero_pushed = nullptr) {
  const auto ps = probes();
  u64 applies = 0, zero = 0;
  for (const auto& a : ps) {
    if (a.barrier) continue;
    ++applies;
    bool found = false;
    for (const auto& b : ps) {
      if (b.barrier && b.conn == a.conn && b.conn_seq == a.conn_seq && b.k == a.k && b.r == a.r) found = true;
    }
    CHECK_MSG(found, "%s: an applied SYNC (conn %llx seq %u k %llu r %llu) has no barrier record with those values",
              what, (unsigned long long)a.conn, a.conn_seq, (unsigned long long)a.k, (unsigned long long)a.r);
    CHECK_MSG(a.r <= a.pushed, "%s: R %llu above pushed %llu", what, (unsigned long long)a.r,
              (unsigned long long)a.pushed);
    CHECK_MSG(a.count_after == a.k + a.pushed - a.r, "%s: count %llu != k %llu + pushed %llu - R %llu", what,
              (unsigned long long)a.count_after, (unsigned long long)a.k, (unsigned long long)a.pushed,
              (unsigned long long)a.r);
    if (a.pushed == 0) {
      CHECK_EQ(a.r, 0u);
      CHECK_EQ(a.count_after, a.k);
      ++zero;
    }
  }
  if (bindings_with_zero_pushed) *bindings_with_zero_pushed = zero;
  return applies;
}

// ---- Tests -----------------------------------------------------------------------------

struct Test {
  const char* name;
  void (*fn)();
};

// X3 forward drop: the proxy cuts o -> p (A's Out connection to B)
// repeatedly under load. Each new connection binds by a HelloAck and the
// exact SYNC naming it, with no counted DATA before (pushed = 0, so
// count = k), recorded at p's barrier and o's apply.
void x3_forward_drop() {
  Net net;
  net.build({{0, 0}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  wait_until([&] { return bound_to(*net.nodes[0].eng, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "bound");
  clear_probes();
  const u64 b0 = gauges(*net.nodes[0].eng)[G_XNODE_BINDINGS];
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    for (int cycle = 0; cycle < 10; ++cycle) {
      sleep_ms(80);
      const u64 old = out_conn(*net.nodes[0].eng, 1);
      net.px(0, 1).cut();
      wait_rebound(*net.nodes[0].eng, 1, 0, old, "rebound");
      check_zero_counters(net.nodes[0], "X3 cycle");
      check_zero_counters(net.nodes[1], "X3 cycle");
    }
  }
  settle(net, L, "X3");
  const u64 bindings = gauges(*net.nodes[0].eng)[G_XNODE_BINDINGS] - b0;
  CHECK(bindings >= 10);
  u64 zero = 0;
  const u64 applies = check_probes("X3", &zero);
  CHECK(zero >= 10);  // every rebinding: pushed 0, count = k
  std::printf("X3 forward drop: 10 cuts of o->p, %llu bindings, %llu SYNCs applied (%llu at binding with pushed=0, "
              "count=k), each equal to its barrier; settled, counts 0, no underflow\n",
              (unsigned long long)bindings, (unsigned long long)applies, (unsigned long long)zero);
}

// X4 return drop: the proxy cuts p -> o (B's Out connection to A)
// repeatedly. Each current SYNC on the new connection names o's still-open
// Out connection: count = k + pushed - R, checked against the barrier and
// against the proxy's count of counted frames delivered on it.
void x4_return_drop() {
  Net net;
  net.build({{0, 0}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  wait_until([&] { return bound_to(*net.nodes[0].eng, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "bound");
  clear_probes();
  u64 nonzero = 0;
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    for (int cycle = 0; cycle < 10; ++cycle) {
      sleep_ms(80);
      const u64 old = out_conn(*net.nodes[1].eng, 0);
      net.px(1, 0).cut();
      wait_rebound(*net.nodes[1].eng, 0, 0, old, "rebound");
      check_zero_counters(net.nodes[0], "X4 cycle");
    }
  }
  settle(net, L, "X4");
  check_probes("X4");
  // A's SYNCs applied at A over A's Out connections: R never above what the
  // proxy delivered on that connection, pushed never below.
  const auto conns = net.px(0, 1).conns();
  for (const auto& p : probes()) {
    if (p.barrier) continue;
    for (const auto& c : conns) {
      if (c.conn != p.conn) continue;
      CHECK_MSG(p.r <= c.counted_sender, "X4: R %llu above the %llu counted frames delivered",
                (unsigned long long)p.r, (unsigned long long)c.counted_sender);
      if (p.pushed != 0) ++nonzero;
    }
  }
  CHECK(nonzero >= 1);
  std::printf("X4 return drop: 10 cuts of p->o; %llu current SYNCs on a connection with counted frames pushed, each "
              "count = k + pushed - R and equal to its barrier, R within the proxy's delivered count; settled\n",
              (unsigned long long)nonzero);
}

// X10 rollback: accounting off on both: the D.1 behaviour on the v2 wire.
void x10_rollback() {
  Net net;
  net.build({{0, 0, true, false}, {1, 0, true, false}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  wait_until([&] { return entry(*net.nodes[0].eng, 1, 0).out.up && entry(*net.nodes[1].eng, 0, 0).out.up; }, "up");
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    sleep_ms(500);
  }
  settle(net, L, "X10");
  for (Node& n : net.nodes) {
    const GaugeSnapshot g = gauges(*n.eng);
    CHECK_EQ(g[G_XNODE_ACCOUNTING], 0u);
    CHECK_EQ(g[G_XNODE_BINDINGS], 0u);
    CHECK_EQ(g[G_XNODE_SYNCS_SENT], 0u);
    CHECK_EQ(g[G_XNODE_ENDS_SENT], 0u);
    CHECK_EQ(g[G_XNODE_REPLIES_PUSHED], 0u);
  }
  const Totals t = ledger_totals(net, L);
  std::printf("X10 rollback (accounting off): links up unbound, no SYNC/END/counted frame, xnode counts 0; "
              "settled, money %lld conserved\n",
              (long long)t.money);
}

// X18 timer constraints: the engine refuses bad orderings.
void x18_timers() {
  struct Bad {
    u32 hb, dead, rel, block;
  } bad[] = {{100, 100, 3000, 2000}, {100, 1000, 1000, 2000}, {100, 200, 3000, 2000}, {100, 1000, 3000, 500}};
  for (const Bad& b : bad) {
    Engine::Config c;
    c.runtime.n_shards = 1;
    c.xnode_hb_ms = b.hb;
    c.xnode_dead_ms = b.dead;
    c.xnode_release_ms = b.rel;
    c.xnode_block_ms = b.block;
    bool refused = false;
    try {
      Engine e(c);
    } catch (const std::exception& ex) {
      refused = std::strstr(ex.what(), "xnode") != nullptr;
    }
    CHECK(refused);
  }
  std::printf("X18 timer constraints: T_dead <= T_hb, T_release <= T_dead, T_dead < 3 T_hb and T_block < T_dead "
              "each refused at start with a message\n");
}

// X22 unbound link does not hold X: p accepts but never sends a HelloAck.
void x22_unbound() {
  Net net;
  net.exchange_budget = 16;
  net.build({{0, 0}, {1, 0}});
  net.px(0, 1).set(LinkProxy::PASS, LinkProxy::DROP);  // A's Out to B: no HelloAck ever reaches A
  net.start_all();
  Ledger L = make_ledger(net, 8);
  u64 purged = 0;
  {
    Load ab(net, L, 0, 1), aa(net, L, 0, 0);
    sleep_ms(800);
    const EntryView v = entry(*net.nodes[0].eng, 1, 0);
    CHECK(!v.out.bound && !v.out.identified);
    CHECK_EQ(v.out.pushed, 0u);  // no counted DATA on an unbound link
    const ShardStats s = net.nodes[0].eng->stats(0);
    CHECK(s.exchanges_live < net.exchange_budget);  // roots for B purged: X not held
    purged = s.xnode_unbound_purged;
    CHECK(purged > 0);
    CHECK(aa.ok() > 0);  // intra-node work served
    net.px(0, 1).set(LinkProxy::PASS, LinkProxy::PASS);
    net.px(0, 1).cut();  // reconnect: the HelloAck goes through now
    wait_until([&] { return bound_to(*net.nodes[0].eng, 1, 0); }, "bound after acks resume");
  }
  settle(net, L, "X22");
  std::printf("X22 unbound link: no HelloAck, %llu roots for B purged while unbound, no counted DATA, live below X; "
              "after acks resume the binding resumes all and everything settles\n",
              (unsigned long long)purged);
}

// X23 accounting mismatch: refused both ways, with message and gauge.
void x23_mismatch() {
  Net net;
  net.build({{0, 0, true, true}, {1, 0, true, false}});
  net.start_all();
  wait_until([&] {
    return gauges(*net.nodes[0].eng)[G_LINK_ACCOUNTING_REFUSALS] > 0 &&
           gauges(*net.nodes[1].eng)[G_LINK_ACCOUNTING_REFUSALS] > 0;
  }, "refusals both ways");
  CHECK(!entry(*net.nodes[0].eng, 1, 0).out.identified);
  std::printf("X23 accounting mismatch: links refused both ways (link_accounting_refusals > 0 on each)\n");
}

// X5 blackhole and heal: the proxies stop forwarding both ways. Silence
// closes (unpaused only), quarantine after T_release, the debt fixed, then
// repaid exactly at the binding; settles.
void x5_blackhole() {
  Net net;
  net.build({{0, 0}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  wait_until([&] { return bound_to(*net.nodes[0].eng, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "bound");
  u64 debt_at_q = 0, silence = 0;
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    sleep_ms(200);
    net.px(0, 1).set(LinkProxy::HOLD, LinkProxy::HOLD);
    net.px(1, 0).set(LinkProxy::HOLD, LinkProxy::HOLD);
    wait_until([&] { return entry(*net.nodes[0].eng, 1, 0).e.state == NodeLinks::EntryState::QUARANTINED; },
               "quarantine", 10);
    const EntryView v = entry(*net.nodes[0].eng, 1, 0);
    debt_at_q = v.e.count;
    CHECK(!v.e.gates);
    for (int i = 0; i < 20; ++i) {
      const EntryView w = entry(*net.nodes[0].eng, 1, 0);
      CHECK(w.e.count <= debt_at_q);  // non-increasing while quarantined
      if (w.e.state != NodeLinks::EntryState::QUARANTINED) break;
      sleep_ms(20);
    }
    silence = gauges(*net.nodes[0].eng)[G_XNODE_SILENCE_CLOSES];
    CHECK(silence > 0);
    net.px(0, 1).cut();
    net.px(1, 0).cut();
    net.px(0, 1).set(LinkProxy::PASS, LinkProxy::PASS);
    net.px(1, 0).set(LinkProxy::PASS, LinkProxy::PASS);
    wait_until([&] { return bound_to(*net.nodes[0].eng, 1, 0); }, "rebound after heal");
  }
  settle(net, L, "X5");
  check_probes("X5");
  std::printf("X5 blackhole and heal: silence closes %llu, quarantined with debt %llu (non-increasing), bound "
              "again with an exact SYNC; settled, counts 0\n",
              (unsigned long long)silence, (unsigned long long)debt_at_q);
}

bool quarantined(Engine& eng, u16 group, u16 member, u16 shard = 0) {
  const EntryView v = entry(eng, group, member, shard);
  return v.links && v.e.inc != 0 && v.e.state == NodeLinks::EntryState::QUARANTINED;
}

// X14 one-way partition, repeated releases: o -> p passes, p -> o is
// blackholed ((a) all of it, HelloAck included; (b) only p's own Out
// connection), under full load for 5 x T_release.
void x14_variant(bool ack_too) {
  Net net;
  net.build({{0, 0}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine& A = *net.nodes[0].eng;
  Engine& B = *net.nodes[1].eng;
  wait_until([&] { return bound_to(A, 1, 0) && bound_to(B, 0, 0); }, "bound");
  u64 phys0 = 0, debt0 = 0, served0 = 0, served1 = 0, samples = 0, releases = 0;
  {
    Load ab(net, L, 0, 1), aa(net, L, 0, 0);
    sleep_ms(200);
    net.px(1, 0).set(LinkProxy::DROP, LinkProxy::DROP);  // p's Out connection: nothing reaches o
    if (ack_too) net.px(0, 1).set(LinkProxy::PASS, LinkProxy::DROP);  // and no HelloAck back either
    wait_until([&] { return quarantined(A, 1, 0); }, "first quarantine", 10);
    phys0 = entry(B, 0, 0).e.held_for;
    debt0 = entry(A, 1, 0).e.count;
    const u64 bindings0 = gauges(A)[G_XNODE_BINDINGS];
    const u64 counted0 = net.px(0, 1).total().counted_sender;
    served0 = aa.ok();
    u64 prev = debt0;
    const auto t0 = Clock::now();
    while (since(t0) < 5.0 * kReleaseMs / 1000) {
      const EntryView a = entry(A, 1, 0);
      const EntryView b = entry(B, 0, 0);
      CHECK_MSG(b.e.held_for <= phys0, "X14: physical at p %llu above the first quarantine's %llu",
                (unsigned long long)b.e.held_for, (unsigned long long)phys0);
      CHECK_MSG(a.e.count <= prev, "X14: debt rose from %llu to %llu", (unsigned long long)prev,
                (unsigned long long)a.e.count);
      prev = a.e.count;
      CHECK(a.e.state == NodeLinks::EntryState::QUARANTINED && !a.out.bound);
      CHECK_EQ(gauges(A)[G_XNODE_BINDINGS], bindings0);                // no re-binding while the In side is down
      CHECK_EQ(net.px(0, 1).total().counted_sender, counted0);           // no counted DATA while unbound
      CHECK(A.stats(0).exchanges_live < net.exchange_budget + kMaxEntries);
      check_zero_counters(net.nodes[0], "X14");
      ++samples;
      sleep_ms(20);
    }
    served1 = aa.ok();
    CHECK(served1 > served0);  // o's intra-node clients served throughout
    releases = gauges(A)[G_XNODE_QUARANTINES];
    net.px(1, 0).set(LinkProxy::PASS, LinkProxy::PASS);
    net.px(0, 1).set(LinkProxy::PASS, LinkProxy::PASS);
    net.px(1, 0).cut();
    net.px(0, 1).cut();
    wait_until([&] { return bound_to(A, 1, 0) && bound_to(B, 0, 0); }, "bound after heal");
  }
  settle(net, L, "X14");
  check_probes("X14");
  std::printf("X14%s one-way partition (%s): quarantined once (%llu quarantine(s)), physical at p <= %llu and debt "
              "<= %llu non-increasing over %llu samples in 5 x T_release, no re-binding, no counted DATA while "
              "unbound, %llu intra-node transfers served meanwhile; healed exact and settled\n",
              ack_too ? "a" : "b", ack_too ? "all of p->o, HelloAck included" : "only p's Out connection",
              (unsigned long long)releases, (unsigned long long)phys0, (unsigned long long)debt0,
              (unsigned long long)samples, (unsigned long long)(served1 - served0));
}
void x14_one_way() {
  x14_variant(true);
  x14_variant(false);
}

// X24 repeated forward flaps before reverse reconciliation: p -> o held at
// the proxy while o -> p flaps 6 times; then released. Only the SYNC
// naming the current connection applies.
void x24_flaps() {
  Net net;
  net.hb_ms = 100;
  net.dead_ms = 2000;
  net.release_ms = 5000;
  net.block_ms = 2000;
  // X high enough that A keeps admitting while p -> o is held (its ENDs
  // with it): each identified-but-unbound window has traffic to refuse.
  net.exchange_budget = 1u << 16;
  net.build({{0, 0}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 256);  // many accounts: those with transfers lost on a cut stay full until resumed
  Engine& A = *net.nodes[0].eng;
  wait_until([&] { return bound_to(A, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "bound");
  u64 stale = 0, refused_unbound = 0;
  NodeLinks::SyncProbe applied{};
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    sleep_ms(150);
    const u64 stale0 = gauges(A)[G_XNODE_SYNCS_STALE];
    net.px(1, 0).set(LinkProxy::HOLD, LinkProxy::PASS);  // p -> o: frames (SYNCs among them) held
    for (int f = 0; f < 6; ++f) {
      const u64 old = out_conn(A, 1);
      if (f == 0) {
        // Counted items lost on the first wire: the count is above k.
        net.px(0, 1).set(LinkProxy::HOLD, LinkProxy::PASS);
        sleep_ms(40);
        net.px(0, 1).set(LinkProxy::PASS, LinkProxy::PASS, /*all=*/false);
      }
      net.px(0, 1).cut();
      wait_until([&] {
        const EntryView v = entry(A, 1, 0);
        return v.out.identified && v.out.conn != old;
      }, "reconnected");
      const EntryView v = entry(A, 1, 0);
      CHECK(!v.out.bound);
      CHECK_EQ(v.out.pushed, 0u);  // no counted DATA before its binding SYNC
      const u64 u0 = A.stats(0).xnode_unbound_purged;
      sleep_ms(20);
      const EntryView w = entry(A, 1, 0);
      CHECK(w.out.identified && !w.out.bound && w.out.conn == v.out.conn);
      CHECK_EQ(w.out.pushed, 0u);  // ... while follow-ups for it were being refused
      refused_unbound += A.stats(0).xnode_unbound_purged - u0;
    }
    CHECK_MSG(refused_unbound > 0, "X24: no follow-up reached the identified, unbound link (no traffic to test)");
    const u64 cur = out_conn(A, 1);
    net.px(1, 0).set(LinkProxy::PASS, LinkProxy::PASS);
    wait_until([&] { return bound_to(A, 1, 0); }, "bound after release");
    stale = gauges(A)[G_XNODE_SYNCS_STALE] - stale0;
    for (const auto& p : probes()) {
      if (!p.barrier && p.conn == cur) applied = p;
    }
    CHECK(applied.conn == cur);
    CHECK_EQ(applied.pushed, 0u);
    CHECK_EQ(applied.count_after, applied.k);
    CHECK(applied.count_after <= applied.count_before);  // no SYNC raises the count
    for (const auto& p : probes()) {
      CHECK(p.barrier || p.conn == cur || p.count_after <= p.count_before);
    }
  }
  CHECK_MSG(stale >= 5, "X24: %llu stale SYNCs, fewer than the 5 connections closed before any SYNC arrived",
            (unsigned long long)stale);
  settle(net, L, "X24");
  check_probes("X24");
  std::printf("X24 forward flaps: 6 reconnects while p -> o was held; %llu follow-ups refused (purged) on the "
              "identified, unbound connections, none pushed; %llu SYNCs naming earlier connections ignored, "
              "none applied; the first applied names the current connection: count %llu -> %llu = k at p, "
              "pushed 0; settled, no underflow\n",
              (unsigned long long)refused_unbound, (unsigned long long)stale, (unsigned long long)applied.count_before,
              (unsigned long long)applied.count_after);
}

// X27 SYNC before HelloAck: o's HelloAck held until p's binding SYNC has
// reached o, on each of 5 reconnects.
void x27_sync_first() {
  Net net;
  net.hb_ms = 50;
  net.dead_ms = 1000;
  net.release_ms = 3000;
  net.block_ms = 1000;
  net.build({{0, 0}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine& A = *net.nodes[0].eng;
  wait_until([&] { return bound_to(A, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "bound");
  u64 cycles = 0;
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    for (int c = 0; c < 5; ++c) {
      sleep_ms(60);
      const u64 old = out_conn(A, 1);
      const u64 syncs0 = net.px(1, 0).total().syncs;
      net.px(0, 1).set(LinkProxy::PASS, LinkProxy::HOLD, /*all=*/false);  // new connections: HelloAck held
      net.px(0, 1).cut();
      wait_until([&] {
        const EntryView v = entry(A, 1, 0);
        return v.out.up && v.out.conn != old && v.out.synced;
      }, "SYNC applied before the HelloAck");
      const EntryView v = entry(A, 1, 0);
      CHECK(!v.out.identified && !v.out.bound);
      CHECK_EQ(v.out.pushed, 0u);
      CHECK(net.px(1, 0).total().syncs > syncs0);
      net.px(0, 1).set(LinkProxy::PASS, LinkProxy::PASS);  // the HelloAck goes now
      wait_rebound(A, 1, 0, old, "bound at the HelloAck");
      ++cycles;
    }
  }
  CHECK_EQ(gauges(A)[G_XNODE_BIND_MISMATCH], 0u);
  settle(net, L, "X27");
  check_probes("X27");
  std::printf("X27 SYNC before HelloAck: %llu reconnects each SYNCED first (count = k, pushed 0, not identified), "
              "BOUND at the HelloAck's arrival; bind_mismatch 0; settled\n",
              (unsigned long long)cycles);
}

// X28 SYNC barrier with queued accounted frames: p's Out connection to o
// stalled at the proxy until counted replies and ENDs sit in Out::buf;
// then o reconnects (p owes a binding SYNC); then the stall ends. The
// queued frames reach o before the SYNC, and the SYNC applies exactly.
void x28_barrier() {
  Net net;
  net.hb_ms = 100;
  net.dead_ms = 4000;
  net.release_ms = 9000;
  net.block_ms = 4000;
  net.exchange_budget = 4096;
  NodeLinks::debug_out_sndbuf = 4096;  // until the end of the test
  net.build({{0, 0}, {1, 0}}, {}, /*rcvbuf=*/4096);
  net.start_all();
  Ledger L = make_ledger(net, 64);
  Engine& A = *net.nodes[0].eng;
  Engine& B = *net.nodes[1].eng;
  wait_until([&] { return bound_to(A, 1, 0) && bound_to(B, 0, 0); }, "bound");
  u64 queued = 0, stalled_conn = 0;
  std::size_t order0 = 0;
  std::string order;
  NodeLinks::SyncProbe bar{}, app{};
  {
    Load ab(net, L, 0, 1, 256), ab2(net, L, 0, 1, 256);
    sleep_ms(300);
    const EntryView v0 = entry(B, 0, 0);
    net.px(1, 0).set(LinkProxy::STALL, LinkProxy::PASS);  // p -> o: the proxy stops reading
    stalled_conn = v0.out.conn;
    for (const auto& c : net.px(1, 0).conns()) {
      if (c.conn == stalled_conn) order0 = c.order.size();
    }
    // Wait until p's kernel buffers are full and frames encoded after that
    // (counted replies and ENDs among them) sit in Out::buf.
    u64 rp = 0, es = 0;
    wait_until([&] {
      const EntryView v = entry(B, 0, 0);
      if (v.out.queued < 64 * 64) {
        rp = v.c.replies_pushed;
        es = v.c.ends_sent;
        return false;
      }
      return v.c.replies_pushed > rp && v.c.ends_sent > es && v.out.conn == v0.out.conn;
    }, "counted replies and ENDs encoded into p's Out::buf behind full kernel buffers", 60);
    queued = entry(B, 0, 0).out.queued;
    ab.stop();
    ab2.stop();
  }
  clear_probes();
  const u64 old = out_conn(A, 1);
  net.px(0, 1).cut();  // o reconnects: p owes it a binding SYNC, appended behind Out::buf
  wait_until([&] {
    for (const auto& p : probes()) {
      if (p.barrier) return true;
    }
    return false;
  }, "p's barrier", 30);
  net.px(1, 0).set(LinkProxy::PASS, LinkProxy::PASS);
  wait_rebound(A, 1, 0, old, "bound", 60);
  const u64 cur = out_conn(A, 1);
  for (const auto& p : probes()) {
    if (p.conn != cur) continue;  // (o's own SYNC to p names p's connection)
    if (p.barrier) bar = p;
    else app = p;
  }
  CHECK(bar.conn == cur && app.conn == cur);
  CHECK(bar.k == app.k && bar.r == app.r);
  CHECK_EQ(app.count_after, app.k);
  // The connection that carried them: replies and ENDs delivered before its SYNC.
  // On the stalled connection: what was delivered after the stall began
  // and before the SYNC that followed it.
  for (const auto& c : net.px(1, 0).conns()) {
    if (c.conn != stalled_conn) continue;
    const auto s = c.order.find('S', order0);
    CHECK_MSG(s != std::string::npos, "X28: the SYNC never arrived on the stalled connection");
    const std::string before = c.order.substr(order0, s - order0);
    if (before.find('R') != std::string::npos && before.find('E') != std::string::npos) order = before;
  }
  CHECK_MSG(!order.empty(), "X28: no connection delivered a counted reply and an END ahead of its SYNC");
  check_zero_counters(net.nodes[0], "X28");
  {
    Load ab(net, L, 0, 1);
    sleep_ms(300);
  }
  settle(net, L, "X28");
  NodeLinks::debug_out_sndbuf = 0;
  std::printf("X28 SYNC barrier: %llu bytes queued in p's Out::buf (counted replies and ENDs among them); o "
              "reconnected; %zu frames (R and E) delivered before the SYNC; at the SYNC count %llu -> %llu = k "
              "recorded at p's barrier (k %llu, R %llu); no underflow; settled\n",
              (unsigned long long)queued, order.size(), (unsigned long long)app.count_before,
              (unsigned long long)app.count_after, (unsigned long long)bar.k, (unsigned long long)bar.r);
}

// X26 not-bound quarantine: o's Out connection fails over from member 0
// to member 1 of p's group while member 0 stays connected and unblocked,
// with items lost on its last wire.
void x26_not_bound() {
  Net net;
  net.build({{0, 0}, {1, 0}, {1, 1}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine& A = *net.nodes[0].eng;
  u16 m0 = 0;
  wait_until([&] {
    for (u16 m = 0; m < 2; ++m) {
      if (bound_to(A, 1, m)) {
        m0 = m;
        return true;
      }
    }
    return false;
  }, "bound to a member");
  const u16 m1 = static_cast<u16>(1 - m0);
  double after = 0;
  u64 debt_q = 0, debt_end = 0;
  {
    Load ab(net, L, 0, static_cast<u16>(1 + m0));
    sleep_ms(200);
    LinkProxy& p0 = net.px(0, static_cast<u16>(1 + m0));
    p0.set(LinkProxy::HOLD, LinkProxy::PASS);  // items on member m0's wire, never delivered
    sleep_ms(50);
    p0.refuse(true);
    p0.cut();
    const auto t0 = Clock::now();
    wait_until([&] { return bound_to(A, 1, m1); }, "failed over to the other member");
    ab.stop();
    wait_until([&] { return quarantined(A, 1, m0); }, "the first member quarantined", 10);
    after = since(t0);
    const EntryView v = entry(A, 1, m0);
    // Still connected and unblocked: not the In-down or blocked trigger. (Its own
    // links quarantine us meanwhile, its In side from us being down, and
    // reconnect: that is the design, 2.8 trigger 1 at the peer.)
    CHECK(v.c.q_not_bound >= 1);  // by the not-bound trigger
    CHECK(v.c.q_in_down == 0 && v.c.q_blocked == 0);
    CHECK(v.e.count > 0);
    debt_q = v.e.count;
    sleep_ms(300);
    debt_end = entry(A, 1, m0).e.count;
    CHECK(debt_end <= debt_q);
  }
  CHECK_MSG(after * 1000 >= kReleaseMs * 0.9 && after * 1000 <= kReleaseMs + 600,
            "X26: quarantined %.3f s after the last binding (T_release %u ms)", after, kReleaseMs);
  check_zero_counters(net.nodes[0], "X26");
  std::printf("X26 not-bound quarantine: member %u (connected, unblocked, no longer bound) quarantined %.0f ms after "
              "its last binding (T_release %u ms) by the not-bound trigger, debt %llu (items lost on its wire); "
              "debt %llu after; no underflow\n",
              m0, after * 1000, kReleaseMs, (unsigned long long)debt_q, (unsigned long long)debt_end);
}

// X29 monotonic incarnation without clocks: restarts of one member under a
// faked wall clock (same millisecond twice, stepped back, stepped forward
// then corrected back, 20 rapid restarts), a crash injected at each step
// of the generation update, a corrupt, truncated and missing generation
// file, and --links-generation-min. Delayed old Hellos are replayed by the
// proxy after each restart.
std::atomic<u64> g_fake_ms{0};
u64 fake_ms() { return g_fake_ms.load(); }

bool starts(Net& net, u16 i) {
  try {
    net.start(i, /*init=*/false);
    return true;
  } catch (const std::exception& e) {
    std::printf("  (start refused as expected: %s)\n", e.what());
    net.nodes[i].eng.reset();
    return false;
  }
}

void x29_generation() {
  Net net;
  u64 floor = 0;
  net.tweak = [&](Engine::Config& c, const Node& nd) {
    if (nd.index == 0) c.links_generation_min = floor;
  };
  net.build({{0, 0}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine* B = net.nodes[1].eng.get();
  LinksGeneration::debug_clock_ms = &fake_ms;
  auto bound_both = [&] { return bound_to(*net.nodes[0].eng, 1, 0) && bound_to(*B, 0, 0); };
  wait_until(bound_both, "bound");
  u64 prev = entry(*B, 0, 0).e.inc;
  CHECK(prev != 0);
  u64 restarts = 0, replays = 0, max_bind_ms = 0;
  auto restart_and_check = [&](u64 clock_ms, const char* what) {
    g_fake_ms = clock_ms;
    // The Hellos of the instance about to be replaced, to replay later.
    std::vector<std::string> old;
    for (const auto& c : net.px(0, 1).conns()) {
      if (c.incarnation == prev && !c.hello_bytes.empty()) old.push_back(c.hello_bytes);
    }
    CHECK(!old.empty());
    net.restart(0);
    const auto t0 = Clock::now();
    wait_until(bound_both, what, 20);
    max_bind_ms = std::max<u64>(max_bind_ms, static_cast<u64>(since(t0) * 1000));
    const EntryView v = entry(*B, 0, 0);
    CHECK_MSG(v.e.inc > prev, "X29 %s: incarnation %llu not above %llu", what, (unsigned long long)v.e.inc,
              (unsigned long long)prev);
    prev = v.e.inc;
    ++restarts;
    // A delayed Hello of the retired instance arrives now: refused; it
    // retires nothing.
    const u64 mc0 = entry(*B, 0, 0).c.m_conflicts;
    const u32 serial0 = v.e.serial;
    net.px(0, 1).replay(old.back());
    ++replays;
    wait_until([&] { return entry(*B, 0, 0).c.m_conflicts > mc0; }, "old Hello refused", 10);
    const EntryView w = entry(*B, 0, 0);
    CHECK_EQ(w.e.inc, prev);
    CHECK_EQ(w.e.serial, serial0);
    CHECK(bound_both());
  };
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    restart_and_check(1'000'000, "clock at T");
    restart_and_check(1'000'000, "same millisecond");
    restart_and_check(990'000, "clock stepped back 10 s");
    restart_and_check(5'000'000, "clock stepped forward");
    restart_and_check(1'000'001, "clock corrected back");
    for (int i = 0; i < 20; ++i) restart_and_check(1'000'001, "rapid restart");
  }
  // A crash at each step of the generation update: that start allocates
  // nothing usable (no links); the next start is still above every
  // incarnation used.
  u64 crashes = 0;
  for (int stage = 1; stage <= 3; ++stage) {
    LinksGeneration::debug_crash_after(stage);
    net.restart(0);
    sleep_ms(300);
    CHECK(!entry(*net.nodes[0].eng, 1, 0).links);  // no incarnation: no links instance
    LinksGeneration::debug_crash_after(0);
    net.restart(0);
    wait_until(bound_both, "bound after a crash in the generation update", 20);
    const u64 inc = entry(*B, 0, 0).e.inc;
    CHECK_MSG(inc > prev, "X29 crash stage %d: incarnation %llu not above %llu", stage, (unsigned long long)inc,
              (unsigned long long)prev);
    prev = inc;
    ++crashes;
  }
  settle(net, L, "X29");
  // The file: corrupt, truncated, missing, in a directory holding state:
  // the engine refuses to start. --links-generation-min recovers it.
  const std::string f = net.nodes[0].dir + "/links_generation";
  net.stop(0);
  std::string good;
  {
    FILE* fp = std::fopen(f.c_str(), "rb");
    CHECK(fp != nullptr);
    char buf[64];
    good.assign(buf, std::fread(buf, 1, sizeof(buf), fp));
    std::fclose(fp);
    CHECK_EQ(good.size(), 24u);
  }
  auto write_file = [&](const std::string& bytes) {
    FILE* fp = std::fopen(f.c_str(), "wb");
    CHECK(fp != nullptr);
    CHECK_EQ(std::fwrite(bytes.data(), 1, bytes.size(), fp), bytes.size());
    std::fclose(fp);
  };
  std::string bad = good;
  bad[9] ^= 0x5a;  // a flipped bit in the generation
  write_file(bad);
  CHECK(!starts(net, 0));
  write_file(good.substr(0, 13));  // truncated
  CHECK(!starts(net, 0));
  CHECK_EQ(::unlink(f.c_str()), 0);  // missing, in a directory with state
  CHECK(!starts(net, 0));
  // The operator's floor: above what the peer reports.
  const u64 highest = gauges(*B)[G_XNODE_HIGHEST_INCARNATION];
  CHECK(highest >= prev);
  floor = highest + 1;
  CHECK(starts(net, 0));
  wait_until(bound_both, "bound after --links-generation-min", 20);
  const u64 inc = entry(*B, 0, 0).e.inc;
  CHECK(inc > highest);
  const u64 used_floor = floor;
  floor = 0;
  LinksGeneration::debug_clock_ms = nullptr;
  settle(net, L, "X29 after the floor");
  std::printf("X29 generation: %llu restarts under a faked clock (same ms, stepped back, forward and back, 20 "
              "rapid), each incarnation above the last; %llu replayed old Hellos refused, none retired the new "
              "entry; bind within %llu ms of each restart; a crash at each of %llu update steps never reused a "
              "value; corrupt, truncated and missing files refused at start; --links-generation-min %llu "
              "recovered (incarnation %llu); settled\n",
              (unsigned long long)restarts, (unsigned long long)replays, (unsigned long long)max_bind_ms,
              (unsigned long long)crashes, (unsigned long long)used_floor, (unsigned long long)inc);
}

// X21 M conflict: two in-memory engines claim member 0 of group 1. The
// second's Hellos are refused while the first is live; neither retires the
// other; once the first is gone the second is accepted.
void x21_m_conflict() {
  Net net;
  net.build({{0, 0}, {1, 0, false}, {1, 0, false}});
  net.px(0, 2).retarget(net.nodes[1].port);  // A's Out connection reaches the first instance
  net.start(0);
  net.start(1);
  Engine& A = *net.nodes[0].eng;
  wait_until([&] { return bound_to(A, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "the first instance bound");
  const u64 inc1 = entry(A, 1, 0).e.inc;
  const u64 ret0 = entry(A, 1, 0).c.retirements;
  net.start(2);
  wait_until([&] { return entry(A, 1, 0).c.m_conflicts > 0; }, "the second refused", 10);
  for (u32 i = 0; i < 5 * kDeadMs / 20; ++i) {
    const EntryView v = entry(A, 1, 0);
    CHECK_EQ(v.e.inc, inc1);
    CHECK_EQ(v.c.retirements, ret0);
    check_zero_counters(net.nodes[0], "X21");
    sleep_ms(20);
  }
  const u64 conflicts = entry(A, 1, 0).c.m_conflicts;
  net.px(0, 2).retarget(net.nodes[2].port);
  net.stop(1);
  wait_until([&] { return entry(A, 1, 0).e.inc > inc1; }, "the second accepted after the first is gone", 10);
  CHECK_EQ(entry(A, 1, 0).c.retirements, ret0 + 1);
  settle_counts(net, "X21");
  std::printf("X21 M conflict: the second instance's Hellos refused %llu times while the first was live (5 x "
              "T_dead), no retirement either way; accepted once the first stopped (one retirement); no underflow\n",
              (unsigned long long)conflicts);
}

// X13 reverse-Hello mismatch: o's Out connection pinned (by the proxy) to
// the old instance P1 of p's member while P1's and the new instance P2's
// reverse connections to o alternate.
void x13_reverse_mismatch() {
  Net net;
  net.build({{0, 0}, {1, 0, false}, {1, 0, false}});
  net.px(0, 2).retarget(net.nodes[1].port);  // o's Out connection goes to P1
  net.start(0);
  net.start(1);
  Engine& A = *net.nodes[0].eng;
  Ledger L = make_ledger(net, 8);
  wait_until([&] { return bound_to(A, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "bound to P1");
  const u64 inc1 = entry(A, 1, 0).e.inc;
  u64 samples = 0, cycles = 0;
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    sleep_ms(200);
    net.start(2);
    wait_until([&] { return entry(A, 1, 0).c.m_conflicts > 0; }, "P2 refused while P1 is live", 10);
    CHECK_EQ(entry(A, 1, 0).e.inc, inc1);
    ab.stop();
    ba.stop();
    u64 inc2 = 0;
    for (int c = 0; c < 3; ++c) {
      // P1's reverse connection goes: P2's is accepted, P1's entry retired.
      net.px(1, 0).refuse(true);
      net.px(1, 0).cut();
      wait_until([&] { return entry(A, 1, 0).e.inc > inc1; }, "P2 accepted", 10);
      inc2 = entry(A, 1, 0).e.inc;
      // P1 still answers o's Out connection; its HelloAck names a retired
      // instance: refused. Nothing from P1 changes P2's entry.
      const u64 mc = entry(A, 1, 0).c.m_conflicts;
      wait_until([&] { return entry(A, 1, 0).c.m_conflicts > mc; }, "P1's HelloAck refused", 10);
      for (int i = 0; i < 15; ++i) {
        const EntryView v = entry(A, 1, 0);
        CHECK_EQ(v.e.inc, inc2);
        CHECK_EQ(v.e.count, 0u);
        CHECK(!v.out.identified && !v.out.bound);
        check_zero_counters(net.nodes[0], "X13");
        ++samples;
        sleep_ms(20);
      }
      // Alternate: P2's reverse connection goes, P1's may come back: refused.
      net.px(1, 0).refuse(false);
      net.px(2, 0).refuse(true);
      net.px(2, 0).cut();
      const u64 mc2 = entry(A, 1, 0).c.m_conflicts;
      wait_until([&] { return entry(A, 1, 0).c.m_conflicts > mc2; }, "P1's reverse Hello refused", 10);
      for (int i = 0; i < 15; ++i) {
        const EntryView v = entry(A, 1, 0);
        CHECK_EQ(v.e.inc, inc2);
        CHECK_EQ(v.e.count, 0u);
        CHECK(!v.out.bound);
        ++samples;
        sleep_ms(20);
      }
      net.px(2, 0).refuse(false);
      ++cycles;
    }
    // Unpin: o's Out connection goes to P2 and binds to it with an exact k.
    net.px(0, 2).retarget(net.nodes[2].port);
    net.px(0, 2).cut();
    wait_until([&] {
      const EntryView v = entry(A, 1, 0);
      return v.out.bound && v.out.incarnation == inc2;
    }, "bound to P2", 20);
  }
  net.stop(1);
  settle_counts(net, "X13");
  check_probes("X13");
  std::printf("X13 reverse-Hello mismatch: %llu alternations of P1's and P2's reverse connections with o's Out "
              "pinned to P1; P1's HelloAck and Hellos refused after P2 was accepted; over %llu samples P2's entry "
              "kept its incarnation and count 0 (nothing from P1 changed it); bound to P2 once unpinned with an "
              "exact SYNC; counts 0, no underflow\n",
              (unsigned long long)cycles, (unsigned long long)samples);
}

// X15 incarnation alternation: o's member 0 restarted 10 times while its
// group's member 1 (a second, fenced-off primary) stays alive and linked.
void x15_alternation() {
  Net net;
  net.build({{0, 0}, {0, 1, false}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine& B = *net.nodes[2].eng;
  wait_until([&] { return entry(B, 0, 0).e.inc != 0 && entry(B, 0, 1).e.inc != 0; }, "both members admitted");
  wait_until([&] { return entry(*net.nodes[1].eng, 1, 0).out.identified; }, "member 1's connection to B up");
  // Member 1's own connection to B is another stream (2.7): member 0's
  // restarts must never close it.
  const u64 m1_conn = out_conn(*net.nodes[1].eng, 1);
  const u64 m1_inc = entry(B, 0, 1).e.inc;
  std::atomic<bool> stop{false};
  std::atomic<u64> samples{0}, max_entries{0};
  std::thread sampler([&] {
    while (!stop) {
      const GaugeSnapshot g = gauges(B);
      CHECK_MSG(g[G_XNODE_ENTRIES] <= 2 && g[G_XNODE_ENTRIES] <= g[G_XNODE_ENTRY_SLOTS],
                "X15: %llu entries on the link (2 members)", (unsigned long long)g[G_XNODE_ENTRIES]);
      CHECK_EQ(g[G_XNODE_END_UNDERFLOW], 0u);
      max_entries = std::max<u64>(max_entries, g[G_XNODE_ENTRIES]);
      ++samples;
      sleep_ms(5);
    }
  });
  const u64 ret0 = entry(B, 0, 0).c.retirements;
  u64 prev = entry(B, 0, 0).e.inc;
  {
    Load a0(net, L, 0, 2), a1(net, L, 1, 2);
    for (int k = 0; k < 10; ++k) {
      sleep_ms(100);
      net.restart(0);
      wait_until([&] { return entry(B, 0, 0).e.inc > prev; }, "the new incarnation admitted", 20);
      prev = entry(B, 0, 0).e.inc;
    }
    sleep_ms(200);
  }
  stop = true;
  sampler.join();
  const u64 rets = entry(B, 0, 0).c.retirements - ret0;
  CHECK(rets >= 10);
  CHECK_MSG(out_conn(*net.nodes[1].eng, 1) == m1_conn, "X15: member 1's connection to B was replaced");
  CHECK_EQ(entry(B, 0, 1).e.inc, m1_inc);
  CHECK_EQ(entry(B, 0, 1).ins, 1u);
  const u64 purged = B.stats(0).xnode_retire_purged;
  settle_counts(net, "X15");
  std::printf("X15 incarnation alternation: 10 restarts of member 0 with member 1 alive (its connection to B never "
              "replaced); entries <= 2 (members) at "
              "all %llu samples (max %llu); %llu retirements, %llu items purged by them; counts 0, no underflow\n",
              (unsigned long long)samples.load(), (unsigned long long)max_entries.load(), (unsigned long long)rets,
              (unsigned long long)purged);
}

// X16 END debt at a switch: p (B) owes ENDs to o's member 0 while its Out
// connection is bound to member 1; member 0 restarts. Its debt is
// discarded at the retirement, never sent to the new instance; member 1's
// ENDs go only on its binding; the new instance's count is exact.
void x16_end_switch() {
  Net net;
  net.build({{0, 0}, {0, 1, false}, {1, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine& B = *net.nodes[2].eng;
  net.px(2, 1).refuse(true);  // B's Out connection to member 0 first
  net.px(2, 1).cut();
  wait_until([&] { return bound_to(B, 0, 0) && bound_to(*net.nodes[0].eng, 1, 0); }, "B bound to member 0", 20);
  net.px(2, 1).refuse(false);
  u64 owed = 0, ends_m0 = 0, ends_m1 = 0;
  {
    Load a0(net, L, 0, 2), a1(net, L, 1, 2);
    sleep_ms(150);
    // Switch B's Out connection to member 1: member 0's items B holds or
    // still receives (until member 0 quarantines) end with ENDs owed to it.
    ends_m1 = net.px(2, 1).total().ends;
    net.px(2, 0).refuse(true);
    net.px(2, 0).cut();
    ends_m0 = net.px(2, 0).total().ends;
    wait_until([&] { return bound_to(B, 0, 1); }, "B bound to member 1", 20);
    wait_until([&] { return entry(B, 0, 0).e.end_owed > 0 && net.px(2, 1).total().ends > ends_m1; },
               "ENDs owed to member 0, ENDs sent to member 1", 20);
    a0.stop();
    owed = entry(B, 0, 0).e.end_owed;
    CHECK(owed > 0);
  }
  const u64 inc0 = entry(B, 0, 0).e.inc;
  net.restart(0);
  wait_until([&] { return entry(B, 0, 0).e.inc > inc0; }, "member 0's new instance admitted", 20);
  const EntryView v = entry(B, 0, 0);
  CHECK_EQ(v.e.end_owed, 0u);  // discarded at the retirement
  CHECK_EQ(net.px(2, 0).total().ends, ends_m0);  // never sent toward member 0
  // B's Out connection now moves to the new instance of member 0: it binds
  // with nothing owed from the old one; its count stays exact.
  net.px(2, 0).refuse(false);
  net.px(2, 1).refuse(true);
  net.px(2, 1).cut();
  wait_until([&] { return bound_to(B, 0, 0); }, "B bound to member 0's new instance", 20);
  {
    Load a0(net, L, 0, 2);
    sleep_ms(300);
  }
  net.px(2, 1).refuse(false);
  settle_counts(net, "X16");
  check_probes("X16");
  std::printf("X16 END debt at a switch: %llu ENDs owed to member 0 while bound to member 1 (whose ENDs were "
              "sent on its binding); member 0 restarted: the debt discarded at the retirement, no END toward it; "
              "its new instance bound with an exact SYNC and settled; no underflow\n",
              (unsigned long long)owed);
}

// X20 member steering: p's (A's) Out connection is attached to one member
// while only the other member's In connection to A is live. It moves to
// the live member within T_dead; ENDs reach it; counts settle.
void x20_steering() {
  Net net;
  net.build({{0, 0}, {1, 0}, {1, 1}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine& A = *net.nodes[0].eng;
  u16 old = 0;
  wait_until([&] {
    for (u16 m = 0; m < 2; ++m) {
      if (bound_to(A, 1, m)) {
        old = m;
        return true;
      }
    }
    return false;
  }, "bound to a member");
  const u16 nw = static_cast<u16>(1 - old);
  double took = 0;
  u64 ends0 = 0, steers = 0;
  {
    Load b(net, L, static_cast<u16>(1 + nw), 0);
    sleep_ms(100);
    ends0 = net.px(0, static_cast<u16>(1 + nw)).total().ends;
    LinkProxy& rev = net.px(static_cast<u16>(1 + old), 0);  // the old member's own connection to A
    rev.refuse(true);
    rev.cut();
    const auto t0 = Clock::now();
    wait_until([&] { return bound_to(A, 1, nw); }, "steered to the live member", 10);
    took = since(t0);
    wait_until([&] { return net.px(0, static_cast<u16>(1 + nw)).total().ends > ends0; }, "ENDs reach it", 10);
    steers = entry(A, 1, nw).c.steers;
    rev.refuse(false);
  }
  CHECK(steers >= 1);
  CHECK_MSG(took * 1000 <= kDeadMs + 400, "X20: steered %.0f ms after the old member's connection went", took * 1000);
  settle_counts(net, "X20");
  std::printf("X20 member steering: the Out connection moved from member %u to the live member %u %.0f ms after "
              "the old one's connection went (T_dead %u ms); ENDs reached it; counts 0\n",
              old, nw, took * 1000, kDeadMs);
}

// X25 delayed old frames after silent replacement: B's shard 1 has its
// link reads paused (backpressure: its WAL held, so a silent In there is
// not closed for silence) while the old instance of A, which sent to it,
// stops; its last frames are held in the proxy with B's side of the
// connection open. Past T_dead a new instance of A appears, first only
// through B's Out connection to it (its HelloAck: the new incarnation is
// admitted there, 2.12 C, with the old In still open at B). The old In
// must be fenced (closed) before the retirement; then the old frames are
// released and B's reads resume: B reads none of them.
void x25_delayed_old() {
  Net net;
  net.exchange_budget = 256;
  net.link_hold_max = 8;
  net.tweak = [](Engine::Config& c, const Node& nd) {
    if (nd.index == 1) c.wal_buffer_budget_bytes = 32u << 10;
  };
  net.build({{0, 0}, {1, 0}}, {1, 2});
  net.start_all();
  Ledger L = make_ledger(net, 8, {0, 1});
  Engine& B = *net.nodes[1].eng;
  LinkProxy& wire = net.px(0, 1, 1);  // A's Out connection to B's shard 1
  wait_until([&] { return entry(*net.nodes[0].eng, 1, 0, 1).out.bound && entry(B, 0, 0, 0, 1).out.bound; }, "bound");
  const u64 inc_old = entry(B, 0, 0, 0, 1).e.inc;
  const u64 ret0 = entry(B, 0, 0, 0, 1).c.retirements;
  u64 held = 0;
  {
    Load ab(net, L, 0, 1, 256, /*may_lose=*/true), ab2(net, L, 0, 1, 256, /*may_lose=*/true);
    sleep_ms(150);
    Engine::debug_hold_wal(1, true);  // B's shard 1 (A has no shard 1)
    wait_until([&] { return gauges(B, 1)[G_INTAKE_LINK_PAUSED] != 0; }, "B's shard 1 link reads paused", 20);
    wire.set(LinkProxy::HOLD, LinkProxy::PASS);  // A's further frames to B's shard 1: held, the connection open
    sleep_ms(60);
    ab.stop();
    ab2.stop();
  }
  held = wire.held_fwd();
  CHECK(held > 0);
  net.stop(0);  // the old instance is gone; its connection to B stays open at the proxy
  sleep_ms(kDeadMs + 100);  // silent past T_dead
  CHECK(gauges(B, 1)[G_INTAKE_LINK_PAUSED] != 0);
  CHECK_EQ(entry(B, 0, 0, 0, 1).ins, 1u);  // the old In, still open at B (paused: not closed for silence)
  // The new instance: its own connections to B's shard 1 refused for now,
  // so it reaches B's shard 1 only through B's Out connection (the
  // HelloAck).
  wire.refuse(true);
  net.start(0, /*init=*/false);
  wait_until([&] { return entry(B, 0, 0, 0, 1).e.inc > inc_old; }, "the new incarnation admitted by its HelloAck", 20);
  const EntryView v = entry(B, 0, 0, 0, 1);
  CHECK_EQ(v.c.retirements, ret0 + 1);
  CHECK_EQ(v.ins, 0u);  // fenced: the old In closed before the retirement
  // Now the old frames are released toward B and B's reads resume, with
  // nothing else moving toward B's shard 1 (its Out connection to A is not
  // bound: the new instance's SYNC cannot reach it yet). The old socket
  // is gone, so B reads none of them.
  const u64 rx0 = B.stats(1).node_emits_received;
  wire.set(LinkProxy::PASS, LinkProxy::PASS);  // the held connection, and new ones from now on
  Engine::debug_hold_wal(1, false);
  wait_until([&] { return wire.held_fwd() == 0; }, "old frames released", 10);
  wait_until([&] { return gauges(B, 1)[G_INTAKE_LINK_PAUSED] == 0; }, "B's reads resumed", 20);
  sleep_ms(300);
  const EntryView b1 = entry(B, 0, 0, 0, 1);
  CHECK_EQ(B.stats(1).node_emits_received, rx0);
  CHECK_EQ(b1.c.ends_received, v.c.ends_received);
  CHECK_EQ(b1.c.syncs_exact + b1.c.syncs_stale, v.c.syncs_exact + v.c.syncs_stale);
  CHECK_EQ(b1.c.replies_returned, v.c.replies_returned);
  CHECK_EQ(b1.c.ignored_frames, v.c.ignored_frames);
  CHECK_EQ(b1.c.stale_tags, v.c.stale_tags);
  CHECK_EQ(b1.c.retirements, ret0 + 1);  // purged exactly once
  CHECK_EQ(b1.e.inc, v.e.inc);
  wire.refuse(false);
  wire.refuse(false);
  wait_until([&] { return entry(*net.nodes[0].eng, 1, 0, 1).out.bound && entry(B, 0, 0, 0, 1).out.bound; },
             "the new instance bound", 20);
  settle(net, L, "X25");
  check_probes("X25");
  std::printf("X25 delayed old frames: B's shard 1 paused (WAL held), %llu bytes of the old instance's frames held "
              "past T_dead with B's side open; the new incarnation admitted by its HelloAck with the old In still "
              "open: the old In fenced (closed) and the old entry retired once; on release, B's reads resumed, B "
              "read none of the old frames (no DATA, END, SYNC or reply counted, none ignored or stale); the new "
              "entry bound with an exact SYNC; settled, money conserved, no underflow\n",
              (unsigned long long)held);
}

// X1 stalled remote (and X11, the stall held for 5 x T_dead): B's shard 1
// stops flushing its WAL while A's clients send cross-node transfers to
// accounts there flat out.
void stalled_remote(const char* name, u32 stall_ms) {
  Net net;
  net.exchange_budget = 256;
  net.link_hold_max = 8;
  // B: a small WAL buffer budget, so its follow-up gate H closes on the
  // stall and its link reads pause at L (backpressure, not death).
  net.tweak = [](Engine::Config& c, const Node& nd) {
    if (nd.index == 1) c.wal_buffer_budget_bytes = 32u << 10;
  };
  net.build({{0, 0}, {1, 0}}, {1, 2});
  net.start_all();
  Ledger L = make_ledger(net, 8, {0, 1});
  Engine& A = *net.nodes[0].eng;
  Engine& B = *net.nodes[1].eng;
  wait_until([&] { return entry(A, 1, 0, 1).out.bound && entry(B, 0, 0, 0, 1).out.bound; }, "bound");
  const u64 bound_live = net.exchange_budget + u64{kMaxEntries};  // E = Rr = kMaxEntries for the account type
  u64 samples = 0, max_live = 0, max_q = 0, max_held = 0, max_emit = 0, overloaded = 0;
  u64 closes0 = 0, resets0 = 0, pauses0 = 0, pauses = 0;
  auto closes = [&] {
    u64 c = 0;
    for (u16 sh = 0; sh < 2; ++sh) c += gauges(B, sh)[G_XNODE_SILENCE_CLOSES];
    return c + gauges(A)[G_XNODE_SILENCE_CLOSES];
  };
  auto resets = [&] {
    u64 c = 0;
    for (u16 sh = 0; sh < 2; ++sh) c += gauges(B, sh)[G_XNODE_OUT_RESETS];
    return c + gauges(A)[G_XNODE_OUT_RESETS];
  };
  {
    Load ab(net, L, 0, 1, 256), ab2(net, L, 0, 1, 256);
    sleep_ms(200);
    closes0 = closes();
    resets0 = resets();
    pauses0 = gauges(B, 1)[G_INTAKE_LINK_PAUSES];
    Engine::debug_hold_wal(1, true);  // B's shard 1 (A has no shard 1)
    const auto t0 = Clock::now();
    while (since(t0) * 1000 < stall_ms) {
      const ShardStats a = A.stats(0);
      const ShardStats b = B.stats(1);
      const EntryView v = entry(A, 1, 0, 1);
      const GaugeSnapshot gb = gauges(B, 1);
      CHECK_MSG(a.exchanges_live < bound_live, "%s: live_A %llu not below X + max(E, Rr) = %llu", name,
                (unsigned long long)a.exchanges_live, (unsigned long long)bound_live);
      CHECK_MSG(v.out.queued <= (1u << 20) + 4096, "%s: Out buffer %llu above kOutCap", name,
                (unsigned long long)v.out.queued);
      CHECK_MSG(gb[G_INTAKE_LINK_HELD_NOW] <= net.link_hold_max, "%s: B holds %llu > L", name,
                (unsigned long long)gb[G_INTAKE_LINK_HELD_NOW]);
      CHECK_MSG(a.emits_in_flight <= a.exchanges_live + b.exchanges_live + 1, "%s: A's queue %llu above live %llu + %llu",
                name, (unsigned long long)a.emits_in_flight, (unsigned long long)a.exchanges_live,
                (unsigned long long)b.exchanges_live);
      max_live = std::max(max_live, a.exchanges_live);
      max_q = std::max<u64>(max_q, v.out.queued);
      max_held = std::max<u64>(max_held, gb[G_INTAKE_LINK_HELD_NOW]);
      max_emit = std::max(max_emit, a.emits_in_flight);
      ++samples;
      sleep_ms(10);
    }
    CHECK_MSG(closes() == closes0 && resets() == resets0, "%s: %llu silence closes, %llu Out resets while paused",
              name, (unsigned long long)(closes() - closes0), (unsigned long long)(resets() - resets0));
    overloaded = ab.overloaded() + ab2.overloaded();
    CHECK_MSG(overloaded > 0, "%s: no E_OVERLOADED (live peak %llu, ok %llu, other %llu, held peak %llu, B pauses %llu)",
              name, (unsigned long long)max_live, (unsigned long long)(ab.ok() + ab2.ok()),
              (unsigned long long)(ab.other() + ab2.other()), (unsigned long long)max_held,
              (unsigned long long)(gauges(B, 1)[G_INTAKE_LINK_PAUSES] - pauses0));
    pauses = gauges(B, 1)[G_INTAKE_LINK_PAUSES] - pauses0;
    // X11: the pause is what must not be taken for death.
    if (stall_ms >= 5 * kDeadMs) CHECK_MSG(pauses >= 1, "%s: B's link reads never paused", name);
    Engine::debug_hold_wal(1, false);
  }
  settle(net, L, name);
  std::printf("%s stalled remote (%u ms, %llu samples): live_A peak %llu < X + max(E, Rr) = %llu; A's Out buffer "
              "peak %llu <= kOutCap; B held peak %llu <= L = %u, its link reads paused %llu time(s); A's queue peak "
              "%llu <= live_A + live_B; %llu E_OVERLOADED; no silence close or Out reset while paused; settled, "
              "money conserved\n",
              name, stall_ms, (unsigned long long)samples, (unsigned long long)max_live,
              (unsigned long long)bound_live, (unsigned long long)max_q, (unsigned long long)max_held, net.link_hold_max,
              (unsigned long long)pauses, (unsigned long long)max_emit, (unsigned long long)overloaded);
}
void x1_stalled() { stalled_remote("X1", 600); }
void x11_backpressure() { stalled_remote("X11", 5 * kDeadMs + 200); }

// X2 rate mismatch (F11 across nodes): B's shard 1 holds its replies
// while A fills X; then A's WAL is held and B's replies released into it.
// B's queue toward A stays within live_A + live_B at every sample.
void x2_rate_mismatch() {
  Net net;
  net.exchange_budget = 64;
  net.build({{0, 0}, {1, 0}}, {1, 2});
  net.start_all();
  Ledger L = make_ledger(net, 8, {0, 1});
  Engine& A = *net.nodes[0].eng;
  Engine& B = *net.nodes[1].eng;
  wait_until([&] { return entry(A, 1, 0, 1).out.bound && entry(B, 0, 0, 0, 1).out.bound; }, "bound");
  u64 peak = 0, samples = 0;
  {
    Load ab(net, L, 0, 1, 256);
    Engine::debug_hold_wal(1, true);  // B's shard 1: replies held
    wait_until([&] { return A.stats(0).exchanges_live + 2 >= net.exchange_budget; }, "A fills X", 20);
    Engine::debug_hold_wal(0, true);  // A's WAL (and B's shard 0, not involved)
    sleep_ms(50);
    Engine::debug_hold_wal(1, false);
    const auto t0 = Clock::now();
    while (since(t0) < 0.6) {
      const ShardStats a = A.stats(0);  // A first: both only fall here
      const ShardStats b = B.stats(1);
      CHECK_MSG(b.emits_in_flight <= a.exchanges_live + b.exchanges_live, "X2: B's queue %llu above %llu + %llu",
                (unsigned long long)b.emits_in_flight, (unsigned long long)a.exchanges_live,
                (unsigned long long)b.exchanges_live);
      peak = std::max(peak, b.emits_in_flight);
      ++samples;
      sleep_ms(2);
    }
    Engine::debug_hold_wal(0, false);
  }
  settle(net, L, "X2");
  std::printf("X2 rate mismatch: B's queue toward A peaked at %llu, within live_A + live_B at all %llu samples; "
              "settled, money conserved\n",
              (unsigned long long)peak, (unsigned long long)samples);
}

// ---- Slot moves (X8, X12, X17), driven in-process as the control plane would ----

// Moves `slot` from node `from` to node `to` (another group): seal, final
// export, final stage, then the placement with its arrival, on every node.
void move_slot(Net& net, u16 slot, u16 from, u16 to, u64 move_id) {
  Engine& F = *net.nodes[from].eng;
  Engine& T = *net.nodes[to].eng;
  const u16 fg = net.nodes[from].spec.group, tg = net.nodes[to].spec.group;
  CHECK_EQ(net.slots[slot], fg);
  const u64 ep = net.epoch + 1;
  SlotReport rep;
  CHECK_EQ(F.seal_slot(slot, move_id, tg, ep, rep), Status::OK);
  std::vector<SlotObject> objs;
  u64 count = 0, digest = 0;
  CHECK_EQ(F.export_slot(slot, move_id, true, {}, objs, count, digest), Status::OK);
  CHECK_EQ(T.stage_slot(slot, move_id, fg, ep, true, objs, count, digest, rep), Status::OK);
  net.slots[slot] = static_cast<u8>(tg);
  net.epoch = ep;
  net.arrived[slot] = {move_id, ep};
  net.publish_placement();
}
// The ledger follows: accounts of `slot` listed under node `from` move to `to`.
void ledger_move(Ledger& L, u16 slot, u16 from, u16 to) {
  auto& f = L.acc[from];
  for (auto it = f.begin(); it != f.end();) {
    if (slot_of(*it) == slot) {
      L.acc[to].push_back(*it);
      it = f.erase(it);
    } else {
      ++it;
    }
  }
}
struct MoveCounters {
  u64 drops_to_resume = 0, handoff_drops = 0, tag_drops = 0, reply_drops = 0, retire_purged = 0, ends_sent = 0,
      misrouted = 0, stale = 0;
};
MoveCounters move_counters(Net& net) {
  MoveCounters m;
  for (Node& n : net.nodes) {
    if (!n.serving()) continue;
    for (u16 sh = 0; sh < n.eng->shard_count(); ++sh) {
      const ShardStats st = n.eng->stats(sh);
      m.drops_to_resume += st.xnode_drops_to_resume;
      m.handoff_drops += st.xnode_handoff_drops;
      m.tag_drops += st.xnode_tag_drops;
      m.reply_drops += st.xnode_reply_drops;
      m.retire_purged += st.xnode_retire_purged;
      m.ends_sent += gauges(*n.eng, sh)[G_XNODE_ENDS_SENT];
      m.misrouted += st.node_emits_misrouted;
      m.stale += st.emits_stale;
    }
  }
  return m;
}

// X8 slot move: a slot of A's moves to C under load between A and B (its
// accounts among those transferred to and from).
void x8_slot_move() {
  Net net;
  net.build({{0, 0}, {1, 0}, {2, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 4);
  wait_until([&] {
    return bound_to(*net.nodes[0].eng, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0) &&
           bound_to(*net.nodes[1].eng, 2, 0) && bound_to(*net.nodes[2].eng, 1, 0);
  }, "bound");
  const u16 slot = slot_of(L.acc[0][0]);
  const MoveCounters m0 = move_counters(net);
  {
    Load ab(net, L, 0, 1), ba(net, L, 1, 0);
    sleep_ms(200);
    move_slot(net, slot, 0, 2, 0x81);
    sleep_ms(300);
  }
  ledger_move(L, slot, 0, 2);
  {
    Load cb(net, L, 2, 1), bc(net, L, 1, 2);  // the moved accounts serve from C
    sleep_ms(200);
  }
  settle(net, L, "X8");
  const MoveCounters m = move_counters(net);
  std::printf("X8 slot move: slot %u moved A -> C under load; drops to resume %llu, hand-off drops %llu, "
              "misrouted %llu, ENDs sent %llu; settled, money conserved, counts 0\n",
              slot, (unsigned long long)(m.drops_to_resume - m0.drops_to_resume),
              (unsigned long long)(m.handoff_drops - m0.handoff_drops),
              (unsigned long long)(m.misrouted - m0.misrouted), (unsigned long long)(m.ends_sent - m0.ends_sent));
}

// X12 foreign purge and hand-off: A's counted credits to accounts on B's
// shard 1 are held on the wire (the proxy) while the source accounts' slot
// moves: dest 1, into B, whose shard 0 then owns it: B's replies, tagged for
// A, are an in-node hand-off; dest 2, to C, with B's link to C down: they
// queue on a link that cannot carry them, and are purged. Either way they
// end by ENDs on the tag and A's count returns to 0.
void x12_variant(u16 dest) {
  Net net;
  net.build({{0, 0}, {1, 0}, {2, 0}}, {1, 2, 1});
  net.start_all();
  Ledger L = make_ledger(net, 16);
  Engine& A = *net.nodes[0].eng;
  Engine& B = *net.nodes[1].eng;
  wait_until([&] {
    return entry(A, 1, 0, 1).out.bound && entry(B, 0, 0, 0, 1).out.bound && bound_to(B, 2, 0) &&
           bound_to(*net.nodes[2].eng, 1, 0);
  }, "bound");
  // The source: A's accounts of one slot, owned at B by shard 0 once moved
  // there. The targets: B's accounts on its shard 1.
  u16 slot = 0;
  bool found = false;
  for (const ObjectID& id : L.acc[0]) {
    if (B.slot_owner(slot_of(id)) == 0) {
      slot = slot_of(id);
      found = true;
      break;
    }
  }
  CHECK(found);
  Ledger S;
  S.acc.resize(3);
  for (const ObjectID& id : L.acc[0]) {
    if (slot_of(id) == slot) S.acc[0].push_back(id);
  }
  for (const ObjectID& id : L.acc[1]) {
    if (B.slot_owner(slot_of(id)) == 1) S.acc[1].push_back(id);
  }
  CHECK(S.acc[1].size() >= 2);
  const MoveCounters m0 = move_counters(net);
  LinkProxy& wire = net.px(0, 1, 1);  // A's Out connection to B's shard 1
  u64 count_at_move = 0;
  {
    Load ab(net, S, 0, 1, 16);
    sleep_ms(100);
    wire.set(LinkProxy::HOLD, LinkProxy::PASS);
    wait_until([&] { return wire.held_fwd() >= 1024; }, "credits held on the wire", 20);
  }
  sleep_ms(50);
  count_at_move = entry(A, 1, 0, 1).e.count;
  CHECK(count_at_move > 0);
  if (dest == 2) {
    net.px(1, 2).refuse(true);
    net.px(1, 2).cut();
  }
  move_slot(net, slot, 0, dest, 0x120 + dest);
  wire.set(LinkProxy::PASS, LinkProxy::PASS);
  wait_until([&] { return entry(A, 1, 0, 1).e.count == 0; }, "A's count back to 0", 20);
  if (dest == 2) net.px(1, 2).refuse(false);
  const MoveCounters m = move_counters(net);
  // The held credits' source slot moved: B drops each as stale (its
  // source's map moved past it), with an END on its tag; the resume pass
  // at the new owner re-sends the transfer. (The in-node hand-off branch,
  // 2.6, cannot be reached with the account type: a tagged reply targets
  // its item's source, and a moved source makes the item itself stale
  // first. Disclosed in the evidence.)
  CHECK(m.stale > m0.stale);
  ledger_move(L, slot, 0, dest);
  settle(net, L, dest == 1 ? "X12 hand-off" : "X12 foreign");
  std::printf("X12%s %s: A's count %llu (credits held on the wire to B's shard 1) when the source slot moved %s; "
              "stale drops %llu, hand-off drops %llu, tag drops %llu, unbound purges at B %llu, ENDs sent %llu; A's "
              "count back to 0; settled, money conserved\n",
              dest == 1 ? "a" : "b", dest == 1 ? "hand-off" : "foreign", (unsigned long long)count_at_move,
              dest == 1 ? "into B" : "to C, B's link to C down", (unsigned long long)(m.stale - m0.stale),
              (unsigned long long)(m.handoff_drops - m0.handoff_drops),
              (unsigned long long)(m.tag_drops - m0.tag_drops), (unsigned long long)B.stats(1).xnode_unbound_purged,
              (unsigned long long)(m.ends_sent - m0.ends_sent));
}
void x12_purge_handoff() {
  x12_variant(1);
  x12_variant(2);
}

// X19 blocked primary leaves links; X19b the same with the blocked bit
// undeliverable (A stops reading B's primary first). B's primary P (group
// 1 member 0) is cut off from its sync standby S (member 1).
void x19_variant(bool undeliverable) {
  const char* name = undeliverable ? "X19b" : "X19";
  Net net;
  if (undeliverable) NodeLinks::debug_out_sndbuf = 4096;
  if (!undeliverable) {
    // T_hb well above the bit's own latency, so a bit that waited for an
    // idle heartbeat (ND34) is told apart from one sent at once.
    net.hb_ms = 80;
    net.dead_ms = 240;
  }
  net.build({{0, 0}, {1, 0}, {1, 1, true, true, 1}}, {}, undeliverable ? 4096 : 0);
  net.start_all();
  Ledger L = make_ledger(net, 8);
  Engine& A = *net.nodes[0].eng;
  Engine& P = *net.nodes[1].eng;
  wait_until([&] { return bound_to(A, 1, 0) && bound_to(P, 0, 0); }, "bound");
  const u64 inc_p = entry(A, 1, 0).e.inc;
  double t_pb = -1, t_ab = -1, t_left = -1, t_q = -1;
  bool bound_while_blocked = false, survived_leave = false, recut = false, reidentified = false;
  u64 cut_conn = 0;
  u64 bindings_at_bit = 0;
  EntryView at_q{};
  {
    Load ab(net, L, 0, 1), pa(net, L, 1, 0, 32, /*may_lose=*/true);
    sleep_ms(200);
    if (undeliverable) {
      net.px(1, 0).set(LinkProxy::STALL, LinkProxy::PASS);  // A reads nothing more from P
      sleep_ms(100);
    }
    bindings_at_bit = entry(A, 1, 0).c.bindings;  // (the bit may be seen on the first sample)
    net.repl[1]->refuse(true);
    net.repl[1]->cut();
    const auto t0 = Clock::now();
    while (t_q < 0) {
      CHECK_MSG(since(t0) < 10, "%s: no quarantine (pb %.3f ab %.3f left %.3f)", name, t_pb, t_ab, t_left);
      const double now = since(t0);
      if (t_pb < 0 && gauges(P)[G_XNODE_BLOCKED] != 0) t_pb = now;
      const EntryView v = entry(A, 1, 0);
      if (t_ab < 0 && v.blocked_since != 0) t_ab = now;
      // No new binding to the blocked entry (an existing one stands until
      // the quarantine).
      if (t_ab >= 0 && v.c.bindings > bindings_at_bit) bound_while_blocked = true;
      if (t_ab < 0) bindings_at_bit = v.c.bindings;
      if (!undeliverable && t_ab >= 0 && !recut && now - t_ab >= 0.05) {
        // A's Out connection to P goes once after the bit (and after A's
        // quarantine checks saw it bound with the bit set, so the blocked
        // trigger stays the first due): it reconnects (HelloAck and SYNC
        // from the blocked P) but must not bind.
        cut_conn = v.out.conn;
        net.px(0, 1).cut();
        recut = true;
      }
      if (recut && t_left < 0 && v.out.identified && v.out.conn != cut_conn) reidentified = true;
      if (t_left < 0 && gauges(P)[G_XNODE_LEFT] != 0) t_left = now;
      if (t_left >= 0 && !survived_leave && v.blocked_since != 0 && now > t_left + 0.02) survived_leave = true;
      if (v.e.inc == inc_p && v.e.state == NodeLinks::EntryState::QUARANTINED) {
        t_q = now;
        at_q = v;
      }
      sleep_ms(1);
    }
    CHECK(t_pb >= 0);
    if (!undeliverable) {
      CHECK_MSG(t_ab >= 0 && (t_ab - t_pb) * 1000 <= net.hb_ms / 2, "%s: blocked bit at A %.0f ms after the block "
                "(T_hb %u: not sent at once)", name, (t_ab - t_pb) * 1000, net.hb_ms);
      CHECK_MSG(!bound_while_blocked, "%s: A bound again to the blocked P", name);
      CHECK_MSG(reidentified, "%s: A's Out connection never reconnected to the blocked P", name);
      CHECK(at_q.c.q_blocked >= 1);
      CHECK_MSG((t_q - t_pb) * 1000 <= net.hb_ms + kReleaseMs + 100, "%s: quarantined %.0f ms after the block", name,
                (t_q - t_pb) * 1000);
    } else {
      CHECK(t_ab < 0 || !bound_while_blocked);
      CHECK_MSG((t_q - t_pb) * 1000 <= kBlockMs + kReleaseMs + 150, "%s: quarantined %.0f ms after the block", name,
                (t_q - t_pb) * 1000);
      CHECK(at_q.c.q_in_down + at_q.c.q_blocked >= 1);
    }
    wait_until([&] { return gauges(P)[G_XNODE_LEFT] != 0; }, "P leaves the links", 5);
    if (t_left < 0) t_left = since(t0);
    CHECK_MSG((t_left - t_pb) * 1000 >= kBlockMs - 30 && (t_left - t_pb) * 1000 <= kBlockMs + 150,
              "%s: P left %.0f ms after the block (T_block %u)", name, (t_left - t_pb) * 1000, kBlockMs);
    if (!undeliverable) CHECK(entry(A, 1, 0).blocked_since != 0 || entry(A, 1, 0).e.inc != inc_p);
    ab.stop();
    pa.stop();
  }
  u64 inc_after = 0;
  if (!undeliverable) {
    // Unblocked: P rejoins as a new links instance, binds exactly.
    net.repl[1]->refuse(false);
    wait_until([&] { return entry(A, 1, 0).e.inc > inc_p && bound_to(A, 1, 0); }, "P rejoined as a new instance",
               20);
    inc_after = entry(A, 1, 0).e.inc;
    CHECK(gauges(P)[G_XNODE_REJOINS] >= 1);
    { Load ab(net, L, 0, 1), pa(net, L, 1, 0); sleep_ms(200); }
    settle(net, L, name);
  } else {
    // S is promoted; A's Out connection moves to it and binds exactly. P,
    // unblocked, learns of the new term and withdraws.
    net.px(1, 0).set(LinkProxy::PASS, LinkProxy::PASS);
    net.nodes[2].eng->promote();
    net.nodes[2].promoted = true;
    wait_until([&] { return bound_to(A, 1, 1); }, "A bound to the promoted member", 20);
    inc_after = entry(A, 1, 1).e.inc;
    net.repl[1]->refuse(false);
    sleep_ms(300);
    net.stop(1);  // the withdrawn old primary, shut down
    L.acc[2] = L.acc[1];
    L.acc[1].clear();
    { Load ab(net, L, 0, 2), sa(net, L, 2, 0); sleep_ms(200); }
    settle(net, L, name, 60, /*debt_zero=*/false);  // the stopped P's quarantined debt stays, bounded (U)
    NodeLinks::debug_out_sndbuf = 0;
  }
  check_probes(name);
  if (!undeliverable) {
    std::printf("X19 blocked primary: blocked bit at A %.0f ms after P blocked (T_hb %u, under END traffic); A's "
                "Out connection to P cut after the bit: reconnected, not bound to it; P left the links %.0f ms after the block (T_block %u); A kept its blocked_since "
                "after; quarantined %.0f ms after the block (<= T_hb + T_release = %u), by the blocked trigger; "
                "unblocked, P rejoined as incarnation %llu (was %llu), bound exactly; settled, money conserved\n",
                (t_ab - t_pb) * 1000, net.hb_ms, (t_left - t_pb) * 1000, kBlockMs, (t_q - t_pb) * 1000,
                net.hb_ms + kReleaseMs, (unsigned long long)inc_after, (unsigned long long)inc_p);
  } else {
    std::printf("X19b blocked primary, bit undeliverable: A not reading P; P left %.0f ms after the block; A "
                "quarantined its entry %.0f ms after the block (<= T_block + T_release = %u) by the In-down "
                "trigger; S promoted, A's Out connection bound to it (incarnation %llu) exactly; settled, money "
                "conserved\n",
                (t_left - t_pb) * 1000, (t_q - t_pb) * 1000, kBlockMs + kReleaseMs, (unsigned long long)inc_after);
  }
}
void x19_blocked() { x19_variant(false); }
void x19b_undeliverable() { x19_variant(true); }

// X30 forged control frames (defense in depth behind the nonce and the
// fencing; ND3, ND12): frames a correct peer never sends, put into a live
// connection's stream by the proxy. Each must be ignored and change no
// count. (a) END and SYNC naming another incarnation of the receiver;
// (b) a SYNC naming the receiver's current Out connection exactly, but
// from another member than the one that connection is identified with.
void x30_forged() {
  {
    Net net;
    net.build({{0, 0}, {1, 0}});
    net.start_all();
    Engine& A = *net.nodes[0].eng;
    Engine& B = *net.nodes[1].eng;
    wait_until([&] { return bound_to(A, 1, 0) && bound_to(B, 0, 0); }, "bound");
    sleep_ms(100);
    const u64 inc_a = entry(B, 0, 0).e.inc;  // A's incarnation, as B holds it
    const EntryView v0 = entry(A, 1, 0);
    CHECK_EQ(v0.e.count, 0u);
    NodeLinks::Frame end{};
    end.pad[0] = NodeLinks::END;
    end.source.hi = 5;
    end.target.lo = inc_a + 7;  // not A's
    net.px(1, 0).inject(end);  // on B's Out connection to A
    wait_until([&] { return entry(A, 1, 0).c.ignored_frames > v0.c.ignored_frames; }, "the END ignored", 10);
    NodeLinks::Frame sync{};
    sync.pad[0] = NodeLinks::SYNC;
    sync.source.hi = v0.out.conn;  // A's current Out connection, exactly
    sync.op = v0.out.conn_seq;
    sync.source.lo = 0;
    sync.target.hi = 1000;
    sync.target.lo = inc_a - 1;  // not A's
    net.px(1, 0).inject(sync);
    wait_until([&] { return entry(A, 1, 0).c.ignored_frames > v0.c.ignored_frames + 1; }, "the SYNC ignored", 10);
    sleep_ms(50);
    const EntryView v1 = entry(A, 1, 0);
    CHECK_EQ(v1.e.count, 0u);
    CHECK_EQ(v1.c.ends_received, v0.c.ends_received);
    CHECK_EQ(v1.c.syncs_exact, v0.c.syncs_exact);
    CHECK_EQ(v1.c.syncs_stale, v0.c.syncs_stale);
    check_zero_counters(net.nodes[0], "X30a");
    settle_counts(net, "X30a");
  }
  {
    Net net;
    net.build({{0, 0}, {1, 0}, {1, 1}});
    net.start_all();
    Engine& A = *net.nodes[0].eng;
    u16 bm = 0;
    wait_until([&] {
      for (u16 m = 0; m < 2; ++m) {
        if (bound_to(A, 1, m) && entry(A, 1, static_cast<u16>(1 - m)).e.inc != 0) {
          bm = m;
          return true;
        }
      }
      return false;
    }, "bound to one member, the other admitted");
    const u16 other = static_cast<u16>(1 - bm);
    sleep_ms(100);
    const u64 inc_a = entry(*net.nodes[1 + other].eng, 0, 0).e.inc;
    const EntryView v0 = entry(A, 1, bm), w0 = entry(A, 1, other);
    NodeLinks::Frame sync{};
    sync.pad[0] = NodeLinks::SYNC;
    sync.source.hi = v0.out.conn;  // names A's current Out connection (to member bm) exactly
    sync.op = v0.out.conn_seq;
    sync.source.lo = 0;
    sync.target.hi = 1000;
    sync.target.lo = inc_a;
    net.px(static_cast<u16>(1 + other), 0).inject(sync);  // ... but on the other member's connection
    wait_until([&] { return entry(A, 1, other).c.syncs_stale > w0.c.syncs_stale; }, "the SYNC ignored as stale", 10);
    sleep_ms(50);
    const EntryView v1 = entry(A, 1, bm), w1 = entry(A, 1, other);
    CHECK_EQ(w1.c.syncs_exact, w0.c.syncs_exact);
    CHECK_EQ(w1.e.count, 0u);
    CHECK_EQ(v1.e.count, 0u);
    CHECK(v1.out.bound && v1.out.conn == v0.out.conn);
    check_zero_counters(net.nodes[0], "X30b");
    settle_counts(net, "X30b");
  }
  std::printf("X30 forged control frames: an END and a SYNC naming another incarnation of the receiver ignored; a "
              "SYNC naming the receiver's current Out connection exactly but sent by another member ignored as "
              "stale; no count changed, no underflow\n");
}

// X31 eager retirement purge (2.7, ND18): B's shard 1 holds A's tagged
// replies (its WAL held) when A restarts. The retirement of A's old entry
// purges them at once, while B is still held, not when they would next
// be pushed.
void x31_retire_purge() {
  Net net;
  net.build({{0, 0}, {1, 0}}, {1, 2});
  net.start_all();
  Ledger L = make_ledger(net, 8, {0, 1});
  Engine& B = *net.nodes[1].eng;
  wait_until([&] { return entry(*net.nodes[0].eng, 1, 0, 1).out.bound && entry(B, 0, 0, 0, 1).out.bound; }, "bound");
  const u64 inc_old = entry(B, 0, 0, 0, 1).e.inc;
  const u64 p0 = B.stats(1).xnode_retire_purged;
  u64 held = 0, purged = 0;
  {
    Load ab(net, L, 0, 1, 32, /*may_lose=*/true);
    sleep_ms(100);
    Engine::debug_hold_wal(1, true);  // B's shard 1 (A has no shard 1): its replies wait for durability
    sleep_ms(150);
    held = entry(B, 0, 0, 0, 1).e.held_for;
    ab.stop();
  }
  CHECK(held > 0);
  net.restart(0);
  wait_until([&] { return entry(B, 0, 0, 0, 1).e.inc > inc_old; }, "A's new instance admitted", 20);
  // Still held: what the retirement purged went at once.
  purged = B.stats(1).xnode_retire_purged - p0;
  CHECK_MSG(purged > 0, "X31: the retirement purged nothing of %llu items held for the old instance",
            (unsigned long long)held);
  Engine::debug_hold_wal(1, false);
  settle(net, L, "X31");
  std::printf("X31 eager retirement purge: %llu items held at B for A's old instance (B's WAL held); A restarted: "
              "the retirement purged %llu of B's queued items at once, B still held; settled, money conserved\n",
              (unsigned long long)held, (unsigned long long)purged);
}

// X17 churn: 20 cycles of slot moves, releases (a blackholed pair past
// T_release, then healed), reconnects and restarts under load on three
// nodes. At every sample: live_s < X + max(E, Rr) at every shard, and the
// physical items (held for peers plus queued, every node) within
// Σ_s (1 + 2 Q_s)(X + max(E, Rr)).
void x17_churn() {
  Net net;
  net.build({{0, 0}, {1, 0}, {2, 0}});
  net.start_all();
  Ledger L = make_ledger(net, 8);
  wait_until([&] {
    return bound_to(*net.nodes[0].eng, 1, 0) && bound_to(*net.nodes[1].eng, 2, 0) && bound_to(*net.nodes[2].eng, 0, 0);
  }, "bound");
  const u64 per = net.exchange_budget + u64{kMaxEntries};  // X + max(E, Rr)
  const u64 Q = 2;                                          // each shard: 2 peer shards, 1 member each
  const u64 phys_bound = 3 * (1 + 2 * Q) * per;
  std::atomic<bool> stop{false};
  std::atomic<u64> samples{0}, max_live{0}, max_phys{0};
  std::thread sampler([&] {
    while (!stop) {
      {
        std::shared_lock<std::shared_mutex> life(net.life_mu);
        u64 phys = 0;
        for (Node& n : net.nodes) {
          if (!n.serving()) continue;
          const ShardStats st = n.eng->stats(0);
          const GaugeSnapshot g = gauges(*n.eng);
          CHECK_MSG(st.exchanges_live < per, "X17: node %u live %llu not below X + max(E, Rr) = %llu", n.index,
                    (unsigned long long)st.exchanges_live, (unsigned long long)per);
          CHECK_EQ(g[G_XNODE_END_UNDERFLOW], 0u);
          CHECK_EQ(g[G_XNODE_DEBT_INCREASES], 0u);
          max_live = std::max<u64>(max_live, st.exchanges_live);
          phys += g[G_XNODE_HELD_FOR_PEERS] + st.emits_in_flight;
        }
        CHECK_MSG(phys <= phys_bound, "X17: physical %llu above %llu", (unsigned long long)phys,
                  (unsigned long long)phys_bound);
        max_phys = std::max(max_phys.load(), phys);
        ++samples;
      }
      sleep_ms(5);
    }
  });
  u64 moves = 0, releases = 0, reconnects = 0, restarts = 0;
  {
    Load ab(net, L, 0, 1), bc(net, L, 1, 2), ca(net, L, 2, 0);
    for (int c = 0; c < 20; ++c) {
      const u16 x = static_cast<u16>(c % 3), y = static_cast<u16>((c + 1) % 3);
      switch (c % 4) {
        case 0: {  // a slot of x's moves to y
          u16 slot = 0;
          bool found = false;
          for (const ObjectID& id : L.acc[x]) {
            if (net.slots[slot_of(id)] == net.nodes[x].spec.group) {
              slot = slot_of(id);
              found = true;
              break;
            }
          }
          if (found) {
            move_slot(net, slot, x, y, 0x1700 + c);
            ++moves;
          }
          break;
        }
        case 1:  // x's and y's connections both ways reset
          net.each(x, y, [](LinkProxy& p) { p.cut(); });
          net.each(y, x, [](LinkProxy& p) { p.cut(); });
          ++reconnects;
          break;
        case 2:  // x and y blackholed past T_release, then healed
          net.each(x, y, [](LinkProxy& p) { p.set(LinkProxy::HOLD, LinkProxy::HOLD); });
          net.each(y, x, [](LinkProxy& p) { p.set(LinkProxy::HOLD, LinkProxy::HOLD); });
          sleep_ms(kReleaseMs + 150);
          net.each(x, y, [](LinkProxy& p) { p.cut(); p.set(LinkProxy::PASS, LinkProxy::PASS); });
          net.each(y, x, [](LinkProxy& p) { p.cut(); p.set(LinkProxy::PASS, LinkProxy::PASS); });
          ++releases;
          break;
        case 3:
          net.restart(x);
          ++restarts;
          break;
      }
      sleep_ms(150);
    }
  }
  stop = true;
  sampler.join();
  // The ledger follows the moves.
  Ledger M;
  M.acc.resize(3);
  for (const auto& v : L.acc) {
    for (const ObjectID& id : v) {
      for (Node& n : net.nodes) {
        if (n.spec.group == net.slots[slot_of(id)]) M.acc[n.index].push_back(id);
      }
    }
  }
  settle(net, M, "X17", 120);
  std::printf("X17 churn: 20 cycles (%llu slot moves, %llu releases, %llu reconnects, %llu restarts) under load; at "
              "all %llu samples live_s < X + max(E, Rr) = %llu (peak %llu) and physical <= %llu (peak %llu); "
              "settled, money conserved, counts 0\n",
              (unsigned long long)moves, (unsigned long long)releases, (unsigned long long)reconnects,
              (unsigned long long)restarts, (unsigned long long)samples.load(), (unsigned long long)per,
              (unsigned long long)max_live.load(), (unsigned long long)phys_bound, (unsigned long long)max_phys.load());
}

// P2 (8.4, dev VM, not tuning): cross-node transfers on two engines, A <->
// B, accounting on against the rollback (off), each for `secs` seconds;
// the per-frame overhead (control frames per DATA frame, from the proxies'
// counts), the heartbeat rate (idle and under load), the binding latency
// after a connect (accounting on), and the drops to resume. Run by name.
struct P2Run {
  double tps = 0;
  u64 data = 0, ends = 0, end_items = 0, syncs = 0, hbs = 0, counted = 0, replies = 0;
  double hb_idle_per_s = 0;
  u64 drops_to_resume = 0, unbound_purged = 0;
};
P2Run p2_run(bool accounting, double secs) {
  Net net;
  net.hb_ms = 100;
  net.dead_ms = 1000;
  net.release_ms = 3000;
  net.block_ms = 2000;  // the engine defaults
  net.exchange_budget = 1024;
  net.build({{0, 0, true, accounting}, {1, 0, true, accounting}});
  net.start_all();
  Ledger L = make_ledger(net, 64);
  wait_until([&] {
    return accounting ? bound_to(*net.nodes[0].eng, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0)
                      : entry(*net.nodes[0].eng, 1, 0).out.up && entry(*net.nodes[1].eng, 0, 0).out.up;
  }, "links");
  P2Run r;
  // Idle heartbeat rate, one connection.
  {
    const u64 h0 = net.px(0, 1).total().heartbeats;
    sleep_ms(2000);
    r.hb_idle_per_s = (net.px(0, 1).total().heartbeats - h0) / 2.0;
  }
  auto tot = [&] {
    LinkProxy::Conn a = net.px(0, 1).total(), b = net.px(1, 0).total();
    a.data += b.data;
    a.ends += b.ends;
    a.end_items += b.end_items;
    a.syncs += b.syncs;
    a.heartbeats += b.heartbeats;
    a.counted_sender += b.counted_sender;
    a.counted_receiver += b.counted_receiver;
    return a;
  };
  const LinkProxy::Conn t0 = tot();
  u64 ok = 0;
  {
    Load ab(net, L, 0, 1, 256), ba(net, L, 1, 0, 256);
    sleep_ms(static_cast<u32>(secs * 1000));
    ok = ab.ok() + ba.ok();
  }
  const LinkProxy::Conn t1 = tot();
  r.tps = ok / secs;
  r.data = t1.data - t0.data;
  r.ends = t1.ends - t0.ends;
  r.end_items = t1.end_items - t0.end_items;
  r.syncs = t1.syncs - t0.syncs;
  r.hbs = t1.heartbeats - t0.heartbeats;
  r.counted = t1.counted_sender - t0.counted_sender;
  r.replies = t1.counted_receiver - t0.counted_receiver;
  for (Node& n : net.nodes) {
    const ShardStats st = n.eng->stats(0);
    r.drops_to_resume += st.xnode_drops_to_resume;
    r.unbound_purged += st.xnode_unbound_purged;
  }
  settle(net, L, accounting ? "P2 on" : "P2 off");
  return r;
}
void p2_bench() {
  const char* env = std::getenv("SNC_P2_SECS");
  const double secs = env ? std::atof(env) : 5.0;
  const char* rounds_env = std::getenv("SNC_P2_ROUNDS");
  const int rounds = rounds_env ? std::atoi(rounds_env) : 3;
  for (int round = 0; round < rounds; ++round) {
    for (bool acc : {true, false}) {
      const P2Run r = p2_run(acc, secs);
      const double ctl = static_cast<double>(r.ends + r.syncs + r.hbs);
      std::printf("P2 round %d accounting %s: %.0f transfers/s over %.1f s; DATA frames %llu (counted by sender %llu, "
                  "counted replies %llu); END frames %llu carrying %llu items (%.1f per frame); SYNC %llu; heartbeats "
                  "%llu under load, %.1f/s idle per connection; control frames per DATA frame %.4f; drops to resume "
                  "%llu, unbound purges %llu\n",
                  round, acc ? "on" : "off", r.tps, secs, (unsigned long long)r.data, (unsigned long long)r.counted,
                  (unsigned long long)r.replies, (unsigned long long)r.ends, (unsigned long long)r.end_items,
                  r.ends ? static_cast<double>(r.end_items) / r.ends : 0.0, (unsigned long long)r.syncs,
                  (unsigned long long)r.hbs, r.hb_idle_per_s, r.data ? ctl / r.data : 0.0,
                  (unsigned long long)r.drops_to_resume, (unsigned long long)r.unbound_purged);
      std::fflush(stdout);
    }
  }
  // Binding latency after a connect (accounting on): the Out connection
  // cut 20 times, idle and under load; from the cut to BOUND on the new one.
  for (bool loaded : {false, true}) {
    Net net;
    net.hb_ms = 100;
    net.dead_ms = 1000;
    net.release_ms = 3000;
    net.block_ms = 2000;
    net.build({{0, 0}, {1, 0}});
    net.start_all();
    Ledger L = make_ledger(net, 64);
    Engine& A = *net.nodes[0].eng;
    wait_until([&] { return bound_to(A, 1, 0) && bound_to(*net.nodes[1].eng, 0, 0); }, "bound");
    std::vector<double> ms;
    {
      std::unique_ptr<Load> ab, ba;
      if (loaded) {
        ab = std::make_unique<Load>(net, L, 0, 1, 256);
        ba = std::make_unique<Load>(net, L, 1, 0, 256);
      }
      for (int i = 0; i < 20; ++i) {
        sleep_ms(100);
        const u64 old = out_conn(A, 1);
        const auto t0 = Clock::now();
        net.px(0, 1).cut();
        wait_rebound(A, 1, 0, old, "rebound");
        ms.push_back(since(t0) * 1000);
      }
    }
    settle(net, L, "P2 binding");
    std::sort(ms.begin(), ms.end());
    std::printf("P2 binding latency (%s): cut to BOUND on the new connection, 20 cuts: min %.2f ms, median %.2f ms, "
                "max %.2f ms (reconnect backoff included)\n",
                loaded ? "under load" : "idle", ms.front(), ms[ms.size() / 2], ms.back());
  }
}

const Test kTests[] = {
    {"X3", x3_forward_drop}, {"X4", x4_return_drop}, {"X5", x5_blackhole}, {"X10", x10_rollback},
    {"X18", x18_timers},     {"X22", x22_unbound},   {"X23", x23_mismatch},
    {"X14", x14_one_way},    {"X24", x24_flaps},     {"X26", x26_not_bound}, {"X27", x27_sync_first},
    {"X28", x28_barrier},    {"X29", x29_generation}, {"X21", x21_m_conflict}, {"X13", x13_reverse_mismatch},
    {"X15", x15_alternation}, {"X16", x16_end_switch}, {"X20", x20_steering}, {"X25", x25_delayed_old},
    {"X1", x1_stalled},       {"X11", x11_backpressure}, {"X2", x2_rate_mismatch},
    {"X8", x8_slot_move},     {"X12", x12_purge_handoff},
    {"X19", x19_blocked},     {"X19b", x19b_undeliverable},
    {"X30", x30_forged},     {"X31", x31_retire_purge},
    {"X17", x17_churn},
};

}  // namespace

int main(int argc, char** argv) {
  account::register_type();
  NodeLinks::debug_sync_probe = &on_probe;
  // Under $TMPDIR when set (the negatives run each mutant there, so a
  // failing mutant's directory goes with its work directory).
  const char* td = std::getenv("TMPDIR");
  std::string tmpl = std::string(td != nullptr && td[0] != '\0' ? td : "/tmp") + "/snc-xnode-XXXXXX";
  CHECK(::mkdtemp(tmpl.data()) != nullptr);
  g_root = tmpl;
  std::set<std::string> want(argv + 1, argv + argc);
  if (want.count("P2")) {
    p2_bench();
    want.erase("P2");
    if (want.empty()) {
      rm_rf(g_root);
      std::printf("test_xnode: ok\n");
      return 0;
    }
  }
  for (const Test& t : kTests) {
    if (!want.empty() && !want.count(t.name)) continue;
    const auto t0 = Clock::now();
    clear_probes();
    t.fn();
    std::printf("  (%s: %.1f s)\n", t.name, since(t0));
    std::fflush(stdout);
  }
  rm_rf(g_root);
  std::printf("test_xnode: ok\n");
  return 0;
}
