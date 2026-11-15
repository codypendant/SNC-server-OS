// Slot moves (docs/MIGRATION.md) as a safety protocol, under failures.
//
// Each scenario: three witnesses holding the placement (snc_witness), two
// replication groups of a primary and a sync standby (node processes of
// this binary, started by NodeID from the control plane), and one slot S
// of group 0 holding most of the accounts. A client (ClusterClient, with a
// session) runs transfers between all accounts, half of them from S, for
// the whole scenario. A move of S to group 1 is driven until a step
// (the driver "dies" there); then one party fails; then a new driver
// carries the move on (or aborts it). Checked:
//   - never two serving owners of S: a probe asks every node, group X then
//     group Y then X again, throughout; a node of X serving both times and
//     one of Y in between would prove an overlap;
//   - every acknowledged transfer is in the final balances, exactly (the
//     session client gets a definite answer for every request, so nothing
//     is in doubt), and money is conserved;
//   - ObjectIDs unchanged: the accounts served at the end are exactly the
//     ones created, each by one primary only (no duplicate authority);
//   - the placement epoch is 1 until the move commits and 2 (3 for a move
//     back) after; no node's map is ahead of the placement;
//   - every transfer settles (no outbox or inbox entry left);
//   - the second driver resolves the move: DONE, or ABORTED, or (after the
//     commit) refuses to abort and finishes it.
// Stop points: prepared (before copy), copying (half the copy staged),
// copied (before the seal), sealed (before STAGED), staged (before the
// commit), committed (before activation), activated (before the move is
// cleared). Failures: none (driver only), source or destination primary
// SIGKILLed and its standby promoted, source or destination standby
// SIGKILLed, a witness SIGKILLed (a control-plane minority), and source or
// destination primary SIGKILLed and restarted from its stored map while
// every witness is down.
//
// Usage: test_migrate [FILTER]   (runs scenarios whose name contains FILTER)
//        test_migrate node DIR K [restart]   (internal: a node process)
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "../tools/control.h"
#include "../tools/migrate.h"
#include "harness.h"
#include "snc/cluster.h"
#include "snc/net.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;
using clk = std::chrono::steady_clock;

namespace {

constexpr u16 kShards = 2;
constexpr i64 kStart = 1000;
constexpr u32 kInSlot = 24;  // group 0's accounts in S
constexpr u32 kOther = 16;   // each group's accounts elsewhere

std::string g_exe;

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

double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

// Ports: witnesses base+0..2; node k (group k/2, member k%2): admin
// base+10+10k, links base+11+10k (one per shard), clients base+15+10k;
// group g's replication base+60+2g.
struct Env {
  std::string dir;
  u16 base = 0;
  u32 poll_ms = 300;
  u32 degrade_ms[2] = {0, 0};

  u16 witness(u32 i) const { return static_cast<u16>(base + i); }
  u16 admin(u32 k) const { return static_cast<u16>(base + 10 + 10 * k); }
  u16 link(u32 k) const { return static_cast<u16>(base + 11 + 10 * k); }
  u16 client(u32 k) const { return static_cast<u16>(base + 15 + 10 * k); }
  u16 repl(u32 g) const { return static_cast<u16>(base + 60 + 2 * g); }
  std::string control() const {
    std::string s;
    for (u32 i = 0; i < 3; ++i) s += (i ? "," : "") + std::string("127.0.0.1:") + std::to_string(witness(i));
    return s;
  }
  control::Members members() const {
    control::Members ms;
    CHECK(control::parse_members(control(), ms));
    return ms;
  }
  void save() const {
    write_file(dir + "/env", std::to_string(base) + " " + std::to_string(poll_ms) + " " +
                                 std::to_string(degrade_ms[0]) + " " + std::to_string(degrade_ms[1]));
  }
  static Env load(const std::string& dir) {
    Env e;
    e.dir = dir;
    std::istringstream in(read_file(dir + "/env"));
    in >> e.base >> e.poll_ms >> e.degrade_ms[0] >> e.degrade_ms[1];
    CHECK(e.base != 0);
    return e;
  }
};

std::vector<ObjectID> read_ids(const std::string& path) {
  std::vector<ObjectID> v;
  std::istringstream in(read_file(path));
  for (std::string s; in >> s;) v.push_back(*ObjectID::parse(s));
  return v;
}

// ---- A node ---------------------------------------------------------------------------

void create_accounts(Engine& eng, const Env& e, u32 g) {
  CreateArgs a{};
  a.balance_cents = kStart;
  a.created_ns = 1;
  a.email_len = 1;
  a.email[0] = 'e';
  a.name_len = 1;
  a.name[0] = 'n';
  std::vector<ObjectID> ids(kOther);
  run_all(
      eng, kOther / kShards,
      [&](u16, u64, Completion done, void* ctx) { create(kTypeId, ByteView(&a, sizeof(a)), {}, done, ctx); },
      [&](u16 s, u64 i, const ExecResult& r) {
        CHECK_EQ(r.status, Status::OK);
        ids[s * (kOther / kShards) + i] = r.id;
      });
  std::string text;
  for (const ObjectID& id : ids) text += id.to_string() + "\n";
  if (g == 0) {
    // S is the slot of the first account; most of group 0's go there too.
    const ObjectID near = ids[0];
    std::vector<ObjectID> in(kInSlot);
    run_all(
        eng, kInSlot / kShards,
        [&](u16, u64, Completion done, void* ctx) {
          create_near(near, kTypeId, ByteView(&a, sizeof(a)), {}, done, ctx);
        },
        [&](u16 s, u64 i, const ExecResult& r) {
          CHECK_EQ(r.status, Status::OK);
          in[s * (kInSlot / kShards) + i] = r.id;
        });
    std::string t2 = near.to_string() + "\n";
    for (const ObjectID& id : in) {
      CHECK_EQ(slot_of(id), slot_of(near));
      text += id.to_string() + "\n";
      t2 += id.to_string() + "\n";
    }
    write_file(e.dir + "/inslot", t2);
    write_file(e.dir + "/slot", std::to_string(slot_of(near)));
  }
  write_file(e.dir + "/ids." + std::to_string(g), text);
}

// status.K: total entries accounts stale serving phase map_epoch held misrouted
// (accounts counted only where this node serves their slot).
void report(Engine& eng, const Env& e, u32 k, u32 g, int slot, bool final) {
  const ClusterMap& map = eng.cluster();
  SlotReport rs;
  if (slot >= 0) rs = eng.slot_report(static_cast<u16>(slot));
  i64 total = 0;
  u64 entries = 0, accounts = 0, stale = 0;
  std::string bal, copies, pending;
  for (u16 sh = 0; sh < eng.shard_count(); ++sh) {
    eng.for_each_object(sh, [&](const ObjectHeader& h) {
      if (h.type_id != kTypeId) return;
      const u16 s = slot_of(h.id);
      const bool served = static_cast<int>(s) == slot ? rs.serving : map.group_of[s] == g;
      const auto* a = reinterpret_cast<const AccountV1*>(h.payload());
      if (!served) {
        ++stale;
        if (final) copies += h.id.to_string() + "\n";
        return;
      }
      total += a->balance_cents;
      entries += a->n_out + a->n_in;
      ++accounts;
      const auto* out = reinterpret_cast<const OutEntry*>(a + 1);
      const auto* inb = reinterpret_cast<const InEntry*>(out + a->n_out);
      for (u16 i = 0; i < a->n_out; ++i) {
        pending += "out " + h.id.to_string() + " seq " + std::to_string(out[i].seq) + " to " + out[i].to.to_string() +
                   " amount " + std::to_string(out[i].amount) + "\n";
      }
      for (u16 i = 0; i < a->n_in; ++i) {
        pending += "in " + h.id.to_string() + " from " + inb[i].from.to_string() + " seq " +
                   std::to_string(inb[i].seq) + "\n";
      }
      if (final) bal += h.id.to_string() + " " + std::to_string(a->balance_cents) + "\n";
    });
  }
  u64 held = 0, misrouted = 0;
  for (u16 sh = 0; sh < eng.shard_count(); ++sh) {
    const ShardStats st = eng.stats(sh);
    held += st.emits_held;
    misrouted += st.node_emits_misrouted;
  }
  const std::string t = std::to_string(k);
  std::ostringstream o;
  o << total << " " << entries << " " << accounts << " " << stale << " " << rs.serving << " " << unsigned(rs.phase)
    << " " << map.epoch << " " << held << " " << misrouted;
  write_file(e.dir + "/status." + t, o.str());
  write_file(e.dir + "/pending." + t, pending);
  if (final) {
    write_file(e.dir + "/balances." + t, bal);
    write_file(e.dir + "/copies." + t, copies);
  }
}

int node_main(const std::string& dir, u32 k, bool restart) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);  // a standby: promote
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  register_type();
  const Env e = Env::load(dir);
  const u32 g = k / 2, m = k % 2;
  const std::string t = std::to_string(k);
  Engine::Config c;
  c.runtime.n_shards = kShards;
  c.expected_objects = 4096;
  c.data_dir = e.dir + "/n" + t;
  c.init = !restart;
  c.wal_segment_bytes = 1u << 20;
  const control::Members ms = e.members();
  control::NodeSetup setup;
  std::string why;
  if (!control::resolve(ms, c.data_dir, k + 1, restart ? 1500 : 10000, setup, why)) {
    std::fprintf(stderr, "node %u: %s\n", k, why.c_str());
    return 3;
  }
  control::apply(setup, ms, c);
  CHECK_EQ(setup.group, g);
  CHECK_EQ(setup.member, m);
  c.placement_poll_ms = e.poll_ms;
  if (m == 1) {
    c.standby = true;
    c.standby_host = "127.0.0.1";
    c.replica_port = e.repl(g);
  } else {
    c.replica_host = "127.0.0.1";
    c.replica_port = e.repl(g);
    c.replica_sync = true;
    c.replica_timeout_ms = e.degrade_ms[g];  // 0: acks always wait for the standby
  }
  net::NetServer::Config nc;
  nc.base_port = e.client(k);
  net::NetServer server(nc, kShards);
  c.app_poll = net::NetServer::poll_hook;
  c.app_user = &server;
  Engine eng(c);
  eng.start();
  std::atomic<bool> standby{m == 1};
  migrate::NodeHandler handler(eng, ms, static_cast<u16>(g),
                               [&] { return standby.load() ? 's' : eng.fenced() ? 'f' : 'p'; });
  std::optional<migrate::AdminServer> admin;
  admin.emplace(e.admin(k), [&](const std::string& l) { return handler.handle(l); });
  CHECK(admin->ok());
  write_file(e.dir + "/setup." + t + (restart ? ".restart" : ""),
             std::to_string(setup.stored) + " " + std::to_string(setup.placement.epoch));
  if (m == 0 && !restart) create_accounts(eng, e, g);
  write_file(e.dir + "/up." + t, "");

  int slot = -1;
  bool want_promote = false;
  auto last = clk::now() - std::chrono::hours(1);
  while (!exists(e.dir + "/stop")) {
    timespec tick{0, 10'000'000};
    if (sigtimedwait(&set, nullptr, &tick) == SIGUSR1) want_promote = standby.load();
    if (want_promote) {
      try {
        const double s = eng.promote();
        standby = false;
        want_promote = false;
        write_file(e.dir + "/promoted." + t, std::to_string(s));
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "node %u: promotion refused: %s\n", k, ex.what());
      }
    }
    if (slot < 0 && exists(e.dir + "/slot")) slot = std::stoi(read_file(e.dir + "/slot"));
    if (!standby && clk::now() - last >= std::chrono::milliseconds(40)) {
      last = clk::now();
      report(eng, e, k, g, slot, false);
    }
  }
  if (!standby) report(eng, e, k, g, slot, true);
  admin.reset();
  eng.stop();
  return 0;
}

// ---- The scenario's processes -----------------------------------------------------------

pid_t spawn(const std::vector<std::string>& args, const std::string& log) {
  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      ::dup2(fd, 1);
      ::dup2(fd, 2);
    }
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    _exit(127);
  }
  return pid;
}

std::string build_dir() { return g_exe.substr(0, g_exe.rfind('/')); }

pid_t spawn_witness(const Env& e, u32 i) {
  const std::string state = e.dir + "/w" + std::to_string(i);
  std::vector<std::string> args{build_dir() + "/snc_witness", "--port", std::to_string(e.witness(i)), "--state",
                                state, "--id", std::to_string(i), "--peers", e.control()};
  if (!exists(state)) args.insert(args.end(), {"--new-state", "1"});  // first start only
  return spawn(args, e.dir + "/w" + std::to_string(i) + ".log");
}

pid_t spawn_node(const Env& e, u32 k, bool restart) {
  std::vector<std::string> a = {g_exe, "node", e.dir, std::to_string(k)};
  if (restart) a.push_back("restart");
  return spawn(a, e.dir + "/n" + std::to_string(k) + ".log");
}

// ---- Failing a scenario -------------------------------------------------------------------

std::string g_scenario, g_dir;

[[noreturn]] void fail(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char b[2048];
  std::vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "test_migrate: scenario %s: %s (kept %s)\n", g_scenario.c_str(), b, g_dir.c_str());
  std::fflush(stderr);
  ::kill(0, SIGKILL);  // the whole process group: every lane, witness and node
  _exit(1);
}

#define REQUIRE(cond, ...)                            \
  do {                                                \
    if (!(cond)) fail("%s:%d: " #cond, __FILE__, __LINE__); \
  } while (0)

void wait_for(const std::string& path, double timeout_s) {
  const auto t0 = clk::now();
  while (!exists(path)) {
    if (since(t0) > timeout_s) fail("timed out waiting for %s", path.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void kill_wait(pid_t& pid) {
  if (pid <= 0) return;
  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
  pid = -1;
}

// ---- The client's transfers ---------------------------------------------------------------

struct Xfer {
  ObjectID from, to;
  i64 amount;
};

struct Workload {
  std::vector<ObjectID> all, inslot;
  u16 port = 0;
  std::atomic<bool> stop{false};
  std::atomic<u64> acked_n{0};
  std::vector<Xfer> acked;
  std::vector<Xfer> unknown;  // E_REPLAYED_OLD: decided before a failover, outcome not kept
  u64 sent = 0, rejected = 0, moved = 0, migrating = 0, overloaded = 0, refreshes = 0, failovers = 0;
  u64 epoch_end = 0;
  std::string error;

  struct Ctx {
    Workload* w;
    std::unordered_map<u64, Xfer>* inflight;
    std::deque<std::pair<clk::time_point, Xfer>>* retry;
    bool want_refresh = false;
    net::ClusterClient* cc;
  };

  static void on_response(void* p, const wire::RespHeader& h, ByteView body) {
    auto* c = static_cast<Ctx*>(p);
    Workload* w = c->w;
    auto it = c->inflight->find(h.req_id);
    if (it == c->inflight->end()) {
      w->error = "an answer to no request";
      return;
    }
    const Xfer x = it->second;
    c->inflight->erase(it);
    const auto st = static_cast<Status>(h.status);
    const auto now = clk::now();
    switch (st) {
      case Status::OK:
        w->acked.push_back(x);
        ++w->acked_n;
        break;
      case Status::E_REPLAYED_OLD:
        // Resent after a failover, and an older request than the session
        // keeps: decided (applied or not), its outcome lost with the old
        // primary. Solved for below.
        w->unknown.push_back(x);
        break;
      case Status::E_APP:
        if (h.app_code != kAppInsufficientFunds && h.app_code != kAppBusy) {
          w->error = "app code " + std::to_string(h.app_code);
        }
        ++w->rejected;
        break;
      case Status::E_MOVED:
        // Another group's by the node's map: a newer map, then resend.
        ++w->moved;
        if (!c->cc->on_moved(body)) c->want_refresh = true;
        c->retry->emplace_back(now + std::chrono::milliseconds(5), x);
        break;
      case Status::E_MIGRATING:
        if (++w->migrating % 16 == 0) c->want_refresh = true;
        c->retry->emplace_back(now + std::chrono::milliseconds(20), x);
        break;
      case Status::E_OVERLOADED:
        ++w->overloaded;
        c->retry->emplace_back(now + std::chrono::milliseconds(20), x);
        break;
      default:
        w->error = std::string("unexpected status ") + status_name(st);
    }
  }

  void run() {
    net::ClusterClient cc;
    std::mt19937_64 rng(static_cast<u64>(::getpid()) * 1000003 + static_cast<u64>(clk::now().time_since_epoch().count()));
    cc.enable_sessions(rng() | 1, 30000, 3000);
    const auto t0 = clk::now();
    while (!cc.connect("127.0.0.1", port)) {
      if (since(t0) > 10) {
        error = "connect: " + cc.error();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::unordered_map<u64, Xfer> inflight;
    std::deque<std::pair<clk::time_point, Xfer>> retry;
    Ctx ctx{this, &inflight, &retry, false, &cc};
    u64 next_id = 1;
    std::optional<clk::time_point> stopped;
    while (error.empty()) {
      if (stop && !stopped) stopped = clk::now();
      if (stopped && inflight.empty() && retry.empty()) break;
      if (stopped && since(*stopped) > 60) {
        error = "requests unanswered 60 s after the end: " + std::to_string(inflight.size()) + " in flight, " +
                std::to_string(retry.size()) + " to retry";
        break;
      }
      const auto now = clk::now();
      while (inflight.size() < 12) {
        Xfer x;
        if (!retry.empty() && retry.front().first <= now) {
          x = retry.front().second;
          retry.pop_front();
        } else if (!stop && retry.size() < 64) {
          x.from = rng() % 2 ? inslot[rng() % inslot.size()] : all[rng() % all.size()];
          do x.to = all[rng() % all.size()];
          while (x.to == x.from);
          x.amount = 1 + static_cast<i64>(rng() % 50);
        } else {
          break;
        }
        wire::ReqHeader h{};
        h.kind = wire::Kind::EXEC;
        h.op = kOpTransfer;
        h.req_id = next_id++;
        h.target = x.from;
        TransferArgs a{x.to, x.amount, 7};
        cc.send(h, ByteView(&a, sizeof(a)));
        inflight[h.req_id] = x;
        ++sent;
      }
      if (cc.poll(on_response, &ctx, 10) < 0) {
        error = "client failed: " + cc.error();
        break;
      }
      if (ctx.want_refresh) {
        ctx.want_refresh = false;
        cc.refresh();
      }
    }
    refreshes = cc.refreshes();
    epoch_end = cc.epoch();
    for (u16 g = 0; g < cc.group_count(); ++g) failovers += cc.group(g).failovers();
  }
};

// Every account must equal its start plus every acknowledged transfer, plus
// some subset of the `unknown` ones (each committed or not). Solved per set
// of unknown transfers that share accounts, by trying every subset. Returns
// how many committed, or -1 if no subset explains the balances.
long long solve_balances(const std::map<ObjectID, i64>& got, const std::vector<Xfer>& acked,
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
  long long committed = 0;
  for (auto& [root, members] : parts) {
    if (members.size() > 18) return -1;
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
      if (found) committed += __builtin_popcount(mask);
    }
    if (!found) return -1;
    for (const ObjectID& id : touched) residual[id] = 0;
  }
  for (auto& [id, r] : residual) {
    if (r != 0) return -1;
  }
  return committed;
}

// ---- The probe: never two serving owners --------------------------------------------------

struct Probe {
  const Env* e;
  u16 slot;
  std::atomic<bool> stop{false};
  u64 rounds = 0, violations = 0, max_epoch = 0;
  std::string what;

  // serving, map epoch; false if the node does not answer.
  bool info(u32 k, bool& serving, u64& epoch) {
    std::string r;
    if (!ha::ask("127.0.0.1", e->admin(k), "MOVE INFO " + std::to_string(slot), r, 1000)) return false;
    std::istringstream in(r);
    std::string tag;
    SlotReport rep;
    if (!(in >> tag) || tag != "INFO" || !migrate::parse_report(in, rep)) return false;
    serving = rep.serving;
    epoch = rep.map_epoch;
    return true;
  }
  bool serving(u32 k) {
    bool s = false;
    u64 ep = 0;
    if (!info(k, s, ep)) return false;
    max_epoch = std::max(max_epoch, ep);
    return s;
  }
  void run() {
    while (!stop) {
      for (u32 x = 0; x < 2; ++x) {
        const u32 y = 1 - x;
        bool a1[2], a2[2];
        for (u32 m = 0; m < 2; ++m) a1[m] = serving(2 * x + m);
        const bool b = serving(2 * y) || serving(2 * y + 1);
        for (u32 m = 0; m < 2; ++m) a2[m] = serving(2 * x + m);
        if (b && ((a1[0] && a2[0]) || (a1[1] && a2[1]))) {
          ++violations;
          what = "group " + std::to_string(x) + " served S before and after group " + std::to_string(y) + " did";
        }
      }
      ++rounds;
    }
  }
};

// ---- Scenarios ------------------------------------------------------------------------------

enum class Fail { NONE, SRC_PRIMARY, SRC_STANDBY, DST_PRIMARY, DST_STANDBY, WITNESS, SRC_RESTART, DST_RESTART };

const char* fail_name(Fail f) {
  switch (f) {
    case Fail::NONE: return "driver";
    case Fail::SRC_PRIMARY: return "src-primary";
    case Fail::SRC_STANDBY: return "src-standby";
    case Fail::DST_PRIMARY: return "dst-primary";
    case Fail::DST_STANDBY: return "dst-standby";
    case Fail::WITNESS: return "witness";
    case Fail::SRC_RESTART: return "src-restart";
    case Fail::DST_RESTART: return "dst-restart";
  }
  return "?";
}

struct Scenario {
  std::string stop;  // the step the first driver dies after
  Fail fail = Fail::NONE;
  bool abort = false;  // the second driver aborts
  bool hint = false;   // no pushed refresh, 60 s polls: maps refresh on a link's newer epoch
  bool back = false;   // then move S back to group 0
  std::string name() const {
    return stop + "/" + fail_name(fail) + (abort ? "/abort" : "") + (hint ? "/hint" : "") + (back ? "/back" : "");
  }
};

int step_index(const std::string& s) {
  static const char* steps[] = {"prepared", "copying", "copied", "sealed", "staged", "committed", "activated"};
  for (int i = 0; i < 7; ++i) {
    if (s == steps[i]) return i;
  }
  return -1;
}

PlacementMove::State state_after(const std::string& s) {
  switch (step_index(s)) {
    case 0: case 1: case 2: return PlacementMove::State::PREPARED;
    case 3: return PlacementMove::State::SEALED;
    case 4: return PlacementMove::State::STAGED;
    default: return PlacementMove::State::COMMITTED;
  }
}

struct NodeInfo {
  bool up = false;
  SlotReport r;
};

NodeInfo node_info(const Env& e, u32 k, u16 slot) {
  NodeInfo n;
  std::string reply;
  if (!ha::ask("127.0.0.1", e.admin(k), "MOVE INFO " + std::to_string(slot), reply, 2000)) return n;
  std::istringstream in(reply);
  std::string tag;
  n.up = (in >> tag) && tag == "INFO" && migrate::parse_report(in, n.r);
  return n;
}

std::vector<long long> read_status(const std::string& path) {
  std::istringstream in(read_file(path));
  std::vector<long long> v(9, -1);
  for (auto& x : v) in >> x;
  return v;
}

std::string run_scenario(const Scenario& sc, u16 port_lo, u16 port_hi) {
  char tmpl[] = "/tmp/snc-migrate-XXXXXX";
  Env e;
  e.dir = mkdtemp(tmpl);
  g_scenario = sc.name();
  g_dir = e.dir;
  e.base = free_ports(port_lo, port_hi, 64);
  e.poll_ms = sc.hint ? 60000 : 300;
  // A standby that dies (or a primary restarted while its standby may hold
  // records it lost) leaves the primary to continue alone after 300 ms;
  // otherwise acks always wait for the standby, so a promoted one has them.
  if (sc.fail == Fail::SRC_STANDBY || sc.fail == Fail::SRC_RESTART) e.degrade_ms[0] = 300;
  if (sc.fail == Fail::DST_STANDBY || sc.fail == Fail::DST_RESTART) e.degrade_ms[1] = 300;
  e.save();
  const control::Members ms = e.members();
  const int at = step_index(sc.stop);
  REQUIRE(at >= 0);
  const bool committed_at_stop = at >= 5;

  std::vector<pid_t> w(3), node(4, -1);
  for (u32 i = 0; i < 3; ++i) w[i] = spawn_witness(e, i);
  std::vector<PlacementNode> nodes;
  std::vector<PlacementGroup> groups;
  for (u32 k = 0; k < 4; ++k) nodes.push_back({k + 1, "127.0.0.1", e.admin(k)});
  for (u32 g = 0; g < 2; ++g) {
    PlacementGroup pg;
    pg.n_shards = kShards;
    pg.space = random_uuid();
    for (u32 m = 0; m < 2; ++m) pg.members.push_back({2 * g + m + 1, e.link(2 * g + m), e.client(2 * g + m)});
    groups.push_back(pg);
  }
  const Placement p0 = Placement::initial(random_uuid(), nodes, groups);
  {
    const auto t0 = clk::now();
    std::string r;
    while ((r = control::init(ms, p0)) != "OK") {
      if (since(t0) > 10) fail("placement init: %s", r.c_str());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  for (u32 k : {1u, 3u}) node[k] = spawn_node(e, k, false);
  for (u32 k : {1u, 3u}) wait_for(e.dir + "/up." + std::to_string(k), 30);
  for (u32 k : {0u, 2u}) node[k] = spawn_node(e, k, false);
  for (u32 k : {0u, 2u}) wait_for(e.dir + "/up." + std::to_string(k), 30);
  const u16 S = static_cast<u16>(std::stoi(read_file(e.dir + "/slot")));
  REQUIRE(p0.slots[S] == 0);

  Workload wl;
  for (u32 g = 0; g < 2; ++g) {
    for (const ObjectID& id : read_ids(e.dir + "/ids." + std::to_string(g))) wl.all.push_back(id);
  }
  wl.inslot = read_ids(e.dir + "/inslot");
  REQUIRE(wl.all.size() == 2 * kOther + kInSlot && wl.inslot.size() == kInSlot + 1);
  wl.port = e.client(0);
  std::thread wl_thread([&] { wl.run(); });
  Probe probe;
  probe.e = &e;
  probe.slot = S;
  std::thread probe_thread([&] { probe.run(); });
  {
    const auto t0 = clk::now();
    while (wl.acked_n < 100 && wl.error.empty()) {
      if (since(t0) > 30) fail("no transfers acknowledged");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // The first driver, until it dies after the step.
  migrate::Driver d1(ms);
  d1.stop_at = [&](const std::string& s) { return s == sc.stop; };
  d1.push_refresh = !sc.hint;
  const std::string r1 = d1.move(S, 1);
  if (r1 != "STOPPED " + sc.stop) fail("first driver: %s\n%s", r1.c_str(), d1.log.c_str());
  u64 move_id = 0;
  {
    migrate::Snapshot sn;
    std::string why;
    REQUIRE(migrate::sync(ms, sn, why));
    REQUIRE(sn.p.move.state == state_after(sc.stop));
    REQUIRE(sn.p.move.slot == S && sn.p.move.from == 0 && sn.p.move.to == 1 && sn.p.move.epoch == 2);
    move_id = sn.p.move.id;
    // The epoch moves with the commit, not before.
    REQUIRE(sn.p.epoch == (committed_at_stop ? 2u : 1u));
    REQUIRE(sn.p.slots[S] == (committed_at_stop ? 1 : 0));
    for (u32 k = 0; k < 4; ++k) {
      const NodeInfo n = node_info(e, k, S);
      REQUIRE(n.up && n.r.map_epoch <= sn.p.epoch);
    }
    const NodeInfo src = node_info(e, 0, S), dst = node_info(e, 2, S);
    REQUIRE(src.r.phase == (at >= 3 ? 2 : 0));         // sealed from "sealed" on
    // Staged from "staged" on; active once activated (or already, at
    // "committed", if its map refreshed by itself in between).
    if (at == 6) REQUIRE(dst.r.phase == 1);
    else if (at == 5) REQUIRE(dst.r.phase == 3 || dst.r.phase == 1);
    else REQUIRE(dst.r.phase == (at == 4 ? 3 : 0));
  }

  // The failure.
  std::string note;
  auto promote = [&](u32 k) {
    ::kill(node[k], SIGUSR1);
    wait_for(e.dir + "/promoted." + std::to_string(k), 30);
  };
  auto restart = [&](u32 k) {
    for (pid_t& x : w) kill_wait(x);
    kill_wait(node[k]);
    ::unlink((e.dir + "/up." + std::to_string(k)).c_str());
    node[k] = spawn_node(e, k, true);
    wait_for(e.dir + "/up." + std::to_string(k), 30);
    // Started from its stored map, the control plane being down.
    std::istringstream in(read_file(e.dir + "/setup." + std::to_string(k) + ".restart"));
    int stored = -1;
    u64 epoch = 0;
    in >> stored >> epoch;
    REQUIRE(stored == 1);
    const NodeInfo n = node_info(e, k, S);
    REQUIRE(n.up && n.r.map_epoch == epoch);
    // What it serves from the stored map: the source nothing once sealed;
    // the destination only with a map at the commit's epoch.
    if (k == 0) REQUIRE(n.r.serving == (at < 3));
    else REQUIRE(n.r.serving == (n.r.phase == 1 && epoch >= 2));
    if (k == 2 && at == 6) REQUIRE(n.r.serving);  // activation kept its map
    note = " restarted with stored map epoch " + std::to_string(epoch) + (n.r.serving ? " (serving)" : " (not serving)");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (u32 i = 0; i < 3; ++i) w[i] = spawn_witness(e, i);
  };
  switch (sc.fail) {
    case Fail::NONE: break;
    case Fail::SRC_PRIMARY: kill_wait(node[0]); promote(1); break;
    case Fail::SRC_STANDBY: kill_wait(node[1]); break;
    case Fail::DST_PRIMARY: kill_wait(node[2]); promote(3); break;
    case Fail::DST_STANDBY: kill_wait(node[3]); break;
    case Fail::WITNESS: kill_wait(w[0]); break;
    case Fail::SRC_RESTART: restart(0); break;
    case Fail::DST_RESTART: restart(2); break;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // A new driver resolves it.
  migrate::Driver d2(ms);
  d2.push_refresh = !sc.hint;
  bool moved = true;
  std::string r2;
  if (sc.abort) {
    r2 = d2.abort(S);
    if (committed_at_stop) {
      if (r2 != "NO committed") fail("abort after the commit: %s\n%s", r2.c_str(), d2.log.c_str());
      r2 = d2.move(S, 1);
      if (r2 != "DONE") fail("finishing after a refused abort: %s\n%s", r2.c_str(), d2.log.c_str());
    } else {
      if (r2 != "ABORTED") fail("abort: %s\n%s", r2.c_str(), d2.log.c_str());
      moved = false;
    }
  } else {
    r2 = d2.move(S, 1);
    if (r2 != "DONE") fail("second driver: %s\n%s", r2.c_str(), d2.log.c_str());
  }
  // A driver that stalled and was overtaken (here: requests for the first
  // move, after it was aborted or finished) changes nothing.
  auto late = [&](u32 k, const std::string& line) {
    std::string reply;
    REQUIRE(ha::ask("127.0.0.1", e.admin(k), line, reply, 15000));
    if (reply.rfind("NO", 0) != 0) fail("late request '%s' accepted: %s", line.c_str(), reply.c_str());
  };
  char idhex[17];
  std::snprintf(idhex, sizeof(idhex), "%016llx", (unsigned long long)move_id);
  const std::string sid = std::to_string(S), mid = idhex;
  {
    const u32 src = exists(e.dir + "/promoted.1") ? 1 : 0, dst = exists(e.dir + "/promoted.3") ? 3 : 2;
    late(src, "MOVE SEAL " + sid + " " + mid + " 1 2");
    late(dst, "MOVE STAGE " + sid + " " + mid + " 0 2 0 0 0 0");
    late(dst, "MOVE STAGE " + sid + " " + mid + " 0 2 1 0 0 0");
  }
  u64 epoch_final = moved ? 2 : 1;
  u16 owner = moved ? 1 : 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (sc.back) {
    migrate::Driver d3(ms);
    const std::string r3 = d3.move(S, 0);
    if (r3 != "DONE") fail("move back: %s\n%s", r3.c_str(), d3.log.c_str());
    epoch_final = 3;
    owner = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  {
    migrate::Snapshot sn;
    std::string why;
    REQUIRE(migrate::sync(ms, sn, why));
    REQUIRE(sn.p.move.state == PlacementMove::State::NONE);
    REQUIRE(sn.p.epoch == epoch_final && sn.p.slots[S] == owner);
    for (u32 s = 0; s < kSlots; ++s) REQUIRE(s == S || sn.p.slots[s] == p0.slots[s]);
  }

  // Wind down: the client gets an answer to everything, then the
  // primaries settle at the final epoch.
  wl.stop = true;
  wl_thread.join();
  if (!wl.error.empty()) fail("client: %s", wl.error.c_str());
  std::vector<u32> primaries;
  for (u32 g = 0; g < 2; ++g) {
    const bool promoted = exists(e.dir + "/promoted." + std::to_string(2 * g + 1));
    primaries.push_back(promoted ? 2 * g + 1 : 2 * g);
  }
  {
    const auto t0 = clk::now();
    u32 settled = 0;
    while (settled < 2) {
      // With hint, nodes poll the control plane every 60 s and nobody pushes
      // the new map: settling sooner proves the source refreshed because a
      // node link showed it a newer epoch.
      if (since(t0) > (sc.hint ? 15 : 60)) {
        std::string s;
        for (u32 k : primaries) s += " [" + read_file(e.dir + "/status." + std::to_string(k)) + "]";
        fail("never settled at epoch %llu:%s", (unsigned long long)epoch_final, s.c_str());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      bool ok = true;
      for (u32 k : primaries) {
        const auto v = read_status(e.dir + "/status." + std::to_string(k));
        ok = ok && v[1] == 0 && v[6] == static_cast<long long>(epoch_final);
      }
      settled = ok ? settled + 1 : 0;
    }
  }
  // S: served by the owner's primary only; the other group's primary holds
  // it sealed (a source) or staged/none (an aborted destination); standbys
  // hold what their primaries hold.
  for (u32 g = 0; g < 2; ++g) {
    const NodeInfo n = node_info(e, primaries[g], S);
    REQUIRE(n.up && n.r.map_epoch == epoch_final);
    REQUIRE(n.r.serving == (g == owner));
    if (g == owner) REQUIRE(n.r.live_count == kInSlot + 1);
    if (g != owner && moved) REQUIRE(n.r.phase == 2);
    const u32 sb = 2 * g + 1;
    if (sb != primaries[g] && node[sb] > 0) {
      const auto t0 = clk::now();
      NodeInfo s;
      do {
        s = node_info(e, sb, S);
        if (s.up && s.r.phase == n.r.phase && !s.r.serving) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } while (since(t0) < 5);
      REQUIRE(s.up && s.r.phase == n.r.phase && !s.r.serving);
    }
  }
  probe.stop = true;
  probe_thread.join();
  if (probe.violations) fail("two serving owners: %s", probe.what.c_str());
  REQUIRE(probe.max_epoch <= epoch_final);

  write_file(e.dir + "/stop", "");
  for (u32 k = 0; k < 4; ++k) {
    if (node[k] <= 0) continue;
    int st = 0;
    ::waitpid(node[k], &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) fail("node %u exited with status %d", k, st);
  }
  for (pid_t& x : w) kill_wait(x);

  // Every account once, by its original ID, at its start plus every
  // acknowledged transfer; money conserved.
  std::map<ObjectID, i64> got;
  u64 stale = 0;
  for (u32 k : primaries) {
    std::istringstream b(read_file(e.dir + "/balances." + std::to_string(k)));
    std::string id;
    i64 bal;
    while (b >> id >> bal) {
      const auto [it, fresh] = got.emplace(*ObjectID::parse(id), bal);
      if (!fresh) fail("account %s served by two primaries", id.c_str());
    }
    stale += read_ids(e.dir + "/copies." + std::to_string(k)).size();
  }
  REQUIRE(got.size() == wl.all.size());
  for (const ObjectID& id : wl.all) REQUIRE(got.count(id) == 1);
  {
    std::string t;
    for (const Xfer& x : wl.acked) t += x.from.to_string() + " " + x.to.to_string() + " " + std::to_string(x.amount) + "\n";
    write_file(e.dir + "/acked", t);
    t.clear();
    for (const Xfer& x : wl.unknown) t += x.from.to_string() + " " + x.to.to_string() + " " + std::to_string(x.amount) + "\n";
    write_file(e.dir + "/unknown", t);
  }
  i64 total = 0;
  for (auto& [id, bal] : got) total += bal;
  if (total != kStart * static_cast<i64>(wl.all.size())) {
    fail("money not conserved: %lld, not %lld", (long long)total, (long long)(kStart * static_cast<i64>(wl.all.size())));
  }
  const long long committed = solve_balances(got, wl.acked, wl.unknown);
  if (committed < 0) {
    fail("the balances are not the acknowledged transfers (%zu, plus a subset of %zu in doubt)", wl.acked.size(),
         wl.unknown.size());
  }
  if (sc.hint) REQUIRE(wl.refreshes > 0 && wl.epoch_end == epoch_final);

  char line[1024];
  std::snprintf(line, sizeof(line),
                "%-28s %s; %zu transfers acked (%zu in doubt, %lld of them applied; %llu refused, %llu E_MOVED, %llu E_MIGRATING, %llu client "
                "refreshes), all acked present; epoch %llu; %llu probe rounds; %llu stale copies%s",
                sc.name().c_str(), r2.c_str(), wl.acked.size(), wl.unknown.size(), committed, (unsigned long long)wl.rejected,
                (unsigned long long)wl.moved, (unsigned long long)wl.migrating, (unsigned long long)wl.refreshes,
                (unsigned long long)epoch_final, (unsigned long long)probe.rounds, (unsigned long long)stale,
                note.c_str());
  std::string cmd = "rm -rf '" + e.dir + "'";
  CHECK_EQ(std::system(cmd.c_str()), 0);
  return line;
}

std::vector<Scenario> all_scenarios() {
  std::vector<Scenario> v;
  const char* steps[] = {"prepared", "copying", "copied", "sealed", "staged", "committed", "activated"};
  for (Fail f : {Fail::NONE, Fail::SRC_PRIMARY, Fail::SRC_STANDBY, Fail::DST_PRIMARY, Fail::DST_STANDBY,
                 Fail::WITNESS, Fail::SRC_RESTART, Fail::DST_RESTART}) {
    for (const char* s : steps) v.push_back({s, f});
  }
  for (const char* s : steps) v.push_back({s, Fail::NONE, true});
  v.push_back({"sealed", Fail::SRC_PRIMARY, true});
  v.push_back({"staged", Fail::DST_PRIMARY, true});
  v.push_back({"activated", Fail::NONE, false, true});
  v.push_back({"committed", Fail::NONE, false, false, true});
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  {
    char b[4096];
    const ssize_t n = ::readlink("/proc/self/exe", b, sizeof(b) - 1);
    CHECK(n > 0);
    g_exe.assign(b, static_cast<std::size_t>(n));
  }
  if (argc >= 4 && std::string(argv[1]) == "node") {
    return node_main(argv[2], static_cast<u32>(std::stoul(argv[3])), argc >= 5 && std::string(argv[4]) == "restart");
  }
  register_type();
  const std::string filter = argc >= 2 ? argv[1] : "";
  std::vector<Scenario> todo;
  for (const Scenario& s : all_scenarios()) {
    if (s.name().find(filter) != std::string::npos) todo.push_back(s);
  }
  // Lanes: scenarios in parallel, each lane its own port range. One
  // process group, so a failure anywhere stops everything (fail()).
  ::setpgid(0, 0);
  const u32 lanes = std::min<u32>(static_cast<u32>(todo.size()), getenv("SNC_MIGRATE_LANES")
                                                                      ? static_cast<u32>(std::atoi(getenv("SNC_MIGRATE_LANES")))
                                                                      : 3);
  const auto t0 = clk::now();
  std::vector<pid_t> pids;
  for (u32 l = 0; l < lanes; ++l) {
    std::fflush(stdout);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      const u16 lo = static_cast<u16>(20000 + l * 2000), hi = static_cast<u16>(lo + 2000);
      for (std::size_t i = l; i < todo.size(); i += lanes) {
        const std::string line = run_scenario(todo[i], lo, hi);
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
      }
      _exit(0);
    }
    pids.push_back(pid);
  }
  bool ok = true;
  for (pid_t p : pids) {
    int st = 0;
    ::waitpid(p, &st, 0);
    ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
  }
  CHECK(ok);
  std::printf("test_migrate: %zu scenarios in %.1f s: ok\n", todo.size(), since(t0));
  return 0;
}
