// snc_load: the lab's transfer load and checker (docs/REAL_MACHINES.md).
//
//   snc_load create --connect HOST:PORT --accounts N [--in-slot M]
//                   [--near ID] [--start CENTS] --ids FILE
//     Creates N accounts spread over the groups, then M more in the slot of
//     the first one (a slot to move). Writes their IDs to FILE, and the
//     first plus the M to FILE.hot. --near ID: the N in ID's slot instead
//     (tests/test_fail_closed.py).
//   snc_load run --connect HOST:PORT --ids FILE --seconds T --out PREFIX
//                [--threads K] [--depth D] [--hot-pct P] [--report-ms MS]
//     Random transfers through a ClusterClient with sessions (it follows
//     failovers and slot moves). P% of sources come from FILE.hot. Prints a
//     line per interval; at the end writes PREFIX.delta (each account's net
//     acknowledged change), PREFIX.unknown (transfers decided before a
//     failover whose outcome was lost) and PREFIX.summary.
//   snc_load check --ids FILE --start CENTS --balances F1,F2,...
//                  --runs PREFIX1,PREFIX2,...
//     Checks AUDIT balance files (snc_server's AUDIT PATH, one per primary)
//     against the runs: every account served exactly once, and its balance
//     the start plus every acknowledged transfer plus some subset of the
//     in-doubt ones (as tests/test_migrate.cpp checks).
//
// Responses are handled as tests/test_migrate.cpp's Workload does: E_MOVED
// refreshes the map and resends, E_MIGRATING and E_OVERLOADED resend later.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../tools/ha.h"
#include "snc/net.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using clk = std::chrono::steady_clock;

namespace {

struct IdHash {
  size_t operator()(const ObjectID& id) const { return std::hash<std::string_view>()(
      std::string_view(reinterpret_cast<const char*>(&id), sizeof(id))); }
};

[[noreturn]] void die(const std::string& why) {
  std::fprintf(stderr, "snc_load: %s\n", why.c_str());
  std::exit(1);
}

double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

std::vector<ObjectID> read_ids(const std::string& path) {
  std::ifstream in(path);
  if (!in) die("cannot read " + path);
  std::vector<ObjectID> v;
  for (std::string s; in >> s;) {
    auto id = ObjectID::parse(s);
    if (!id) die("bad id in " + path + ": " + s);
    v.push_back(*id);
  }
  return v;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string p; std::getline(in, p, sep);) {
    if (!p.empty()) out.push_back(p);
  }
  return out;
}

bool connect(net::ClusterClient& cc, const std::string& host, unsigned port, double wait_s) {
  const auto t0 = clk::now();
  while (!cc.connect(host, static_cast<u16>(port))) {
    if (since(t0) > wait_s) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return true;
}

// ---- create -------------------------------------------------------------------------------

int cmd_create(const std::string& host, unsigned port, u64 n, u64 in_slot, i64 start, const std::string& ids_path,
               const ObjectID& near_id) {
  net::ClusterClient cc;
  cc.enable_sessions(std::random_device{}() | 1ull << 40, 30000, 5000);
  if (!connect(cc, host, port, 30)) die("connect: " + cc.error());
  CreateArgs a{};
  a.balance_cents = start;
  a.created_ns = 1;
  a.email_len = 1;
  a.email[0] = 'e';
  a.name_len = 1;
  a.name[0] = 'n';
  struct Ctx {
    std::vector<ObjectID> got;
    u64 outstanding = 0, retry = 0;
    std::string error;
  } ctx;
  auto on = [](void* p, const wire::RespHeader& h, ByteView) {
    auto* c = static_cast<Ctx*>(p);
    --c->outstanding;
    if (h.status == static_cast<u16>(Status::OK)) c->got.push_back(h.id);
    else if (h.status == static_cast<u16>(Status::E_OVERLOADED)) ++c->retry;
    else c->error = std::string("create: ") + status_name(static_cast<Status>(h.status));
  };
  // Phase 1: N spread over the groups; phase 2: M near the first.
  auto run = [&](u64 want, const ObjectID& near) {
    const size_t base = ctx.got.size();
    u64 sent = 0, req = 1;
    while (ctx.got.size() - base < want && ctx.error.empty()) {
      if (ctx.retry > 0) {
        sent -= ctx.retry;
        ctx.retry = 0;
      }
      while (ctx.outstanding < 256 && sent < want) {
        wire::ReqHeader q{};
        q.kind = wire::Kind::CREATE;
        q.op = kTypeId;
        q.req_id = req++;
        q.target = near;
        cc.send(q, ByteView(&a, sizeof(a)), static_cast<u16>(sent % cc.group_count()));
        ++sent;
        ++ctx.outstanding;
      }
      if (cc.poll(on, &ctx, 10) < 0) die("client failed: " + cc.error());
    }
    if (!ctx.error.empty()) die(ctx.error);
  };
  run(n, near_id);
  // By value: run() appends to ctx.got, which may reallocate. (A reference
  // to ctx.got[0] dangled after the first reallocation, and the "100K"
  // slots of docs/REAL_MACHINES.md P5 got about 31K: docs/EVIDENCE.md.)
  const ObjectID first = ctx.got.empty() ? ObjectID{} : ctx.got[0];
  if (in_slot > 0) run(in_slot, first);
  u64 in_first_slot = 0;
  for (const ObjectID& id : ctx.got) in_first_slot += slot_of(id) == slot_of(first);
  std::string all, hot;
  for (size_t i = 0; i < ctx.got.size(); ++i) {
    all += ctx.got[i].to_string() + "\n";
    if (in_slot > 0 && (i == 0 || i >= n)) hot += ctx.got[i].to_string() + "\n";
  }
  std::ofstream(ids_path) << all;
  std::ofstream(ids_path + ".hot") << hot;
  std::printf("created %zu accounts over %u groups%s\n", ctx.got.size(), cc.group_count(),
              in_slot ? (", " + std::to_string(in_first_slot) + " in slot " + std::to_string(slot_of(first))).c_str()
                      : "");
  return 0;
}

// ---- run ----------------------------------------------------------------------------------

// Latency histogram: exact below 64 us, then 16 buckets per power of two.
struct Hist {
  static constexpr int kN = 64 + 16 * 40;
  std::vector<u64> b = std::vector<u64>(kN);
  u64 n = 0;
  static int index(u64 us) {
    if (us < 64) return static_cast<int>(us);
    const int e = 63 - __builtin_clzll(us);
    return std::min(kN - 1, 64 + (e - 6) * 16 + static_cast<int>((us >> (e - 4)) & 15));
  }
  static u64 value(int i) {
    if (i < 64) return static_cast<u64>(i);
    const int e = (i - 64) / 16 + 6;
    return (u64{1} << e) + (static_cast<u64>((i - 64) % 16) << (e - 4));
  }
  void add(u64 us) {
    ++b[index(us)];
    ++n;
  }
  void merge(const Hist& o) {
    for (int i = 0; i < kN; ++i) b[i] += o.b[i];
    n += o.n;
  }
  u64 pct(double p) const {
    if (n == 0) return 0;
    const u64 want = static_cast<u64>(p * static_cast<double>(n - 1)) + 1;
    u64 seen = 0;
    for (int i = 0; i < kN; ++i) {
      seen += b[i];
      if (seen >= want) return value(i);
    }
    return value(kN - 1);
  }
};

struct Xfer {
  ObjectID from, to;
  i64 amount;
};

struct Counters {
  u64 acked = 0, rejected = 0, moved = 0, migrating = 0, overloaded = 0, unknown = 0;
};

// One connection set and its requests; one per thread.
struct Worker {
  std::string host;
  unsigned port = 0;
  const std::vector<ObjectID>* all = nullptr;
  const std::vector<ObjectID>* hot = nullptr;
  int hot_pct = 0;
  u32 depth = 12;
  std::atomic<bool>* stop = nullptr;

  std::mutex mu;  // guards hist, c (read by the reporter)
  Hist hist;
  Counters c;
  std::unordered_map<ObjectID, i64, IdHash> delta;
  std::vector<Xfer> unknown;
  u64 refreshes = 0, failovers = 0, epoch = 0;
  std::string error;

  struct Pending {
    Xfer x;
    clk::time_point sent;
  };
  std::unordered_map<u64, Pending> inflight;
  std::deque<std::pair<clk::time_point, Xfer>> retry;
  net::ClusterClient* cc = nullptr;
  bool want_refresh = false;

  static void on_response(void* p, const wire::RespHeader& h, ByteView body) {
    auto* w = static_cast<Worker*>(p);
    auto it = w->inflight.find(h.req_id);
    if (it == w->inflight.end()) {
      w->error = "an answer to no request";
      return;
    }
    const Pending pd = it->second;
    w->inflight.erase(it);
    const auto now = clk::now();
    std::lock_guard<std::mutex> g(w->mu);
    switch (static_cast<Status>(h.status)) {
      case Status::OK:
        w->delta[pd.x.from] -= pd.x.amount;
        w->delta[pd.x.to] += pd.x.amount;
        ++w->c.acked;
        w->hist.add(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now - pd.sent).count()));
        break;
      case Status::E_REPLAYED_OLD:
        w->unknown.push_back(pd.x);
        ++w->c.unknown;
        break;
      case Status::E_APP:
        if (h.app_code != kAppInsufficientFunds && h.app_code != kAppBusy) {
          w->error = "app code " + std::to_string(h.app_code);
        }
        ++w->c.rejected;
        break;
      case Status::E_MOVED:
        ++w->c.moved;
        if (!w->cc->on_moved(body)) w->want_refresh = true;
        w->retry.emplace_back(now + std::chrono::milliseconds(5), pd.x);
        break;
      case Status::E_MIGRATING:
        if (++w->c.migrating % 16 == 0) w->want_refresh = true;
        w->retry.emplace_back(now + std::chrono::milliseconds(20), pd.x);
        break;
      case Status::E_OVERLOADED:
        ++w->c.overloaded;
        w->retry.emplace_back(now + std::chrono::milliseconds(20), pd.x);
        break;
      default:
        w->error = std::string("unexpected status ") + status_name(static_cast<Status>(h.status));
    }
  }

  void run() {
    net::ClusterClient client;
    cc = &client;
    std::mt19937_64 rng(std::random_device{}() * 1000003ull + static_cast<u64>(clk::now().time_since_epoch().count()));
    client.enable_sessions(rng() | 1, 30000, 3000);
    if (!connect(client, host, port, 30)) {
      error = "connect: " + client.error();
      return;
    }
    u64 next_id = 1;
    std::optional<clk::time_point> stopped;
    while (error.empty()) {
      if (*stop && !stopped) stopped = clk::now();
      if (stopped && inflight.empty() && retry.empty()) break;
      if (stopped && since(*stopped) > 60) {
        error = "requests unanswered 60 s after the end: " + std::to_string(inflight.size()) + " in flight, " +
                std::to_string(retry.size()) + " to retry";
        break;
      }
      const auto now = clk::now();
      while (inflight.size() < depth) {
        Xfer x;
        if (!retry.empty() && retry.front().first <= now) {
          x = retry.front().second;
          retry.pop_front();
        } else if (!*stop && retry.size() < 4 * depth) {
          const bool h = !hot->empty() && static_cast<int>(rng() % 100) < hot_pct;
          x.from = h ? (*hot)[rng() % hot->size()] : (*all)[rng() % all->size()];
          do x.to = (*all)[rng() % all->size()];
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
        client.send(h, ByteView(&a, sizeof(a)));
        inflight[h.req_id] = Pending{x, clk::now()};
      }
      if (client.poll(on_response, this, 5) < 0) {
        error = "client failed: " + client.error();
        break;
      }
      if (want_refresh) {
        want_refresh = false;
        client.refresh();
      }
    }
    refreshes = client.refreshes();
    epoch = client.epoch();
    for (u16 g = 0; g < client.group_count(); ++g) failovers += client.group(g).failovers();
  }
};

int cmd_run(const std::string& host, unsigned port, const std::string& ids_path, double seconds, u32 threads,
            u32 depth, int hot_pct, int report_ms, const std::string& out) {
  const std::vector<ObjectID> all = read_ids(ids_path);
  std::vector<ObjectID> hot;
  if (std::ifstream(ids_path + ".hot")) hot = read_ids(ids_path + ".hot");
  if (all.size() < 2) die("need at least two accounts");
  std::atomic<bool> stop{false};
  std::vector<std::unique_ptr<Worker>> ws;
  std::vector<std::thread> ts;
  for (u32 i = 0; i < threads; ++i) {
    auto w = std::make_unique<Worker>();
    w->host = host;
    w->port = port;
    w->all = &all;
    w->hot = &hot;
    w->hot_pct = hot_pct;
    w->depth = depth;
    w->stop = &stop;
    ws.push_back(std::move(w));
  }
  for (auto& w : ws) ts.emplace_back([&w] { w->run(); });
  std::printf("# t_s ops_s p50_us p99_us p999_us max_us moved migrating overloaded unknown rejected\n");
  const auto t0 = clk::now();
  Hist total;
  Counters last{};
  auto t_last = t0;
  bool failed = false;
  while (since(t0) < seconds && !failed) {
    std::this_thread::sleep_for(std::chrono::milliseconds(report_ms));
    Hist h;
    Counters c{};
    for (auto& w : ws) {
      std::lock_guard<std::mutex> g(w->mu);
      h.merge(w->hist);
      w->hist = Hist{};
      c.acked += w->c.acked;
      c.moved += w->c.moved;
      c.migrating += w->c.migrating;
      c.overloaded += w->c.overloaded;
      c.unknown += w->c.unknown;
      c.rejected += w->c.rejected;
      failed = failed || !w->error.empty();
    }
    total.merge(h);
    const double dt = since(t_last);
    t_last = clk::now();
    std::printf("%.1f %.0f %llu %llu %llu %llu %llu %llu %llu %llu %llu\n", since(t0),
                static_cast<double>(c.acked - last.acked) / dt, (unsigned long long)h.pct(0.5),
                (unsigned long long)h.pct(0.99), (unsigned long long)h.pct(0.999), (unsigned long long)h.pct(1.0),
                (unsigned long long)(c.moved - last.moved), (unsigned long long)(c.migrating - last.migrating),
                (unsigned long long)(c.overloaded - last.overloaded), (unsigned long long)(c.unknown - last.unknown),
                (unsigned long long)(c.rejected - last.rejected));
    std::fflush(stdout);
    last = c;
  }
  stop = true;
  for (auto& t : ts) t.join();
  std::unordered_map<ObjectID, i64, IdHash> delta;
  std::vector<Xfer> unknown;
  Counters c{};
  u64 refreshes = 0, failovers = 0, epoch = 0;
  std::string errors;
  for (auto& w : ws) {
    total.merge(w->hist);
    for (auto& [id, d] : w->delta) delta[id] += d;
    unknown.insert(unknown.end(), w->unknown.begin(), w->unknown.end());
    c.acked += w->c.acked;
    c.rejected += w->c.rejected;
    c.moved += w->c.moved;
    c.migrating += w->c.migrating;
    c.overloaded += w->c.overloaded;
    refreshes += w->refreshes;
    failovers += w->failovers;
    epoch = std::max(epoch, w->epoch);
    if (!w->error.empty()) errors += (errors.empty() ? "" : "; ") + w->error;
  }
  {
    std::ofstream d(out + ".delta");
    for (auto& [id, v] : delta) d << id.to_string() << " " << v << "\n";
    std::ofstream u(out + ".unknown");
    for (const Xfer& x : unknown) u << x.from.to_string() << " " << x.to.to_string() << " " << x.amount << "\n";
  }
  std::ostringstream s;
  s << "seconds " << since(t0) << "\nacked " << c.acked << "\nops_s " << static_cast<double>(c.acked) / since(t0)
    << "\np50_us " << total.pct(0.5) << "\np99_us " << total.pct(0.99) << "\np999_us " << total.pct(0.999)
    << "\nmax_us " << total.pct(1.0) << "\nunknown " << unknown.size() << "\nrejected " << c.rejected << "\nmoved "
    << c.moved << "\nmigrating " << c.migrating << "\noverloaded " << c.overloaded << "\nrefreshes " << refreshes
    << "\nfailovers " << failovers << "\nepoch " << epoch << "\nerror " << (errors.empty() ? "-" : errors) << "\n";
  std::ofstream(out + ".summary") << s.str();
  std::printf("%s", s.str().c_str());
  return errors.empty() ? 0 : 1;
}

// ---- check --------------------------------------------------------------------------------

int cmd_check(const std::string& ids_path, i64 start, const std::string& balances, const std::string& runs) {
  const std::vector<ObjectID> ids = read_ids(ids_path);
  std::map<ObjectID, i64> residual;
  std::map<ObjectID, int> seen;
  for (const std::string& f : split(balances, ',')) {
    std::ifstream in(f);
    if (!in) die("cannot read " + f);
    std::string s;
    i64 b;
    while (in >> s >> b) {
      auto id = ObjectID::parse(s);
      if (!id) die("bad id in " + f);
      ++seen[*id];
      residual[*id] = b - start;
    }
  }
  int bad = 0;
  for (const ObjectID& id : ids) {
    const int n = seen.count(id) ? seen[id] : 0;
    if (n != 1 && bad++ < 5) std::printf("account %s served by %d nodes\n", id.to_string().c_str(), n);
  }
  if (bad) {
    std::printf("FAIL: %d accounts not served exactly once\n", bad);
    return 1;
  }
  std::vector<Xfer> unknown;
  for (const std::string& r : split(runs, ',')) {
    std::ifstream d(r + ".delta");
    if (!d) die("cannot read " + r + ".delta");
    std::string s;
    i64 v;
    while (d >> s >> v) residual[*ObjectID::parse(s)] -= v;
    std::ifstream u(r + ".unknown");
    std::string a, b2;
    while (u >> a >> b2 >> v) unknown.push_back({*ObjectID::parse(a), *ObjectID::parse(b2), v});
  }
  // Each in-doubt transfer committed or not: solved per set of them that
  // share accounts, by trying every subset (as tests/test_migrate.cpp).
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
    if (members.size() > 18) {
      std::printf("FAIL: %zu in-doubt transfers share accounts; too many to solve\n", members.size());
      return 1;
    }
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
    if (!found) {
      std::printf("FAIL: no subset of %zu in-doubt transfers explains their accounts\n", members.size());
      return 1;
    }
    for (const ObjectID& id : touched) residual[id] = 0;
  }
  int off = 0;
  i64 sum = 0;
  for (auto& [id, r] : residual) {
    sum += r;
    if (r != 0 && off++ < 5) std::printf("account %s off by %lld\n", id.to_string().c_str(), (long long)r);
  }
  if (off) {
    std::printf("FAIL: %d accounts are not their acknowledged transfers (net %lld)\n", off, (long long)sum);
    return 1;
  }
  std::printf("OK: %zu accounts each served once; balances = acknowledged transfers + %lld of %zu in doubt\n",
              ids.size(), committed, unknown.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) die("usage: snc_load create|run|check ... (see the header of bench/snc_load.cpp)");
  const std::string cmd = argv[1];
  std::map<std::string, std::string> f;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::strncmp(argv[i], "--", 2) != 0) die(std::string("bad argument ") + argv[i]);
    f[argv[i] + 2] = argv[i + 1];
  }
  if ((argc - 2) % 2 != 0) die("every flag takes a value");
  auto need = [&](const char* k) {
    if (!f.count(k)) die(std::string("missing --") + k);
    return f[k];
  };
  auto get = [&](const char* k, const char* d) { return f.count(k) ? f[k] : std::string(d); };
  std::string host;
  unsigned port = 0;
  if (cmd != "check" && !ha::parse_hostport(need("connect"), host, port)) die("bad --connect");
  const i64 start = std::atoll(get("start", "1000000").c_str());
  if (cmd == "create") {
    return cmd_create(host, port, std::strtoull(need("accounts").c_str(), nullptr, 10),
                      std::strtoull(get("in-slot", "0").c_str(), nullptr, 10), start, need("ids"),
                      f.count("near") ? ObjectID::parse(f["near"]).value_or(ObjectID{}) : ObjectID{});
  }
  if (cmd == "run") {
    return cmd_run(host, port, need("ids"), std::atof(need("seconds").c_str()),
                   static_cast<u32>(std::atoi(get("threads", "1").c_str())),
                   static_cast<u32>(std::atoi(get("depth", "12").c_str())), std::atoi(get("hot-pct", "0").c_str()),
                   std::atoi(get("report-ms", "1000").c_str()), need("out"));
  }
  if (cmd == "check") return cmd_check(need("ids"), start, need("balances"), need("runs"));
  die("unknown command " + cmd);
}
