// Load generator for snc_server and baseline_server over TCP (docs/ROUTING.md).
// Each client thread holds one connection per shard and keeps --depth
// requests outstanding. It first creates its share of accounts, then runs
// get_balance/deposit against targets drawn uniformly from all accounts.
//
// Routing (where each request is sent):
//   --route owner     to the shard that owns the target (client-side routing)
//   --route any       to a uniformly random shard (ownership-blind ingress)
//   --misroute PCT    with --route owner: PCT% go to a random non-owner shard
//
// Usage: net_bench [--host H] [--port P] [--threads T] [--cpus 2,3]
//                  [--objects N] [--seconds S] [--depth D] [--reads PCT]
//                  [--route owner|any] [--misroute PCT] [--ids FILE]
//
// --endpoint H:P (repeatable) and --session ID: fail over to another node
// and resend unanswered requests with per-shard sequences (docs/RETRIES.md).
// --request-timeout-ms MS (default 5000, 0 = off): with --session, fail over
// when a node leaves a request unanswered that long (a hung primary).
// --verify: after the run, read every account and check its balance is its
// start plus the deposits sent to it (no transfers, one client process).
// --retry-claim-ttl-s S (with --session): the run claims session
// deduplication against a server session TTL of S seconds. It is valid only
// if every in-doubt resend happened well before session expiry:
// max_resend_age_ms at most a tenth of S (docs/EXPERIMENTAL_BASELINE.md 7,
// R11). Otherwise it prints "retry claim: INVALID" and exits 3.
//
// --index PCT: that share of ops are INDEX lookups of a random loaded account's
// email (--objects and --threads must match the load). They are counted and
// reported apart from the rest of the mix.
//
// --ids FILE: if FILE exists, use the accounts listed in it instead of
// creating any (for example on a standby serving reads); otherwise create
// them and write their IDs there.
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "zipf.h"
#include "snc/net.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;

namespace {

u64 now_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

struct Args {
  std::string host = "127.0.0.1";
  u16 port = 7400;
  u16 threads = 1;
  std::vector<int> cpus;
  u64 objects = 1000000;
  double seconds = 4;
  u32 depth = 64;
  int reads_pct = 50;
  bool route_owner = true;
  int misroute_pct = 0;
  double zipf = 0;  // > 0: Zipf key choice over all objects
  int transfer_pct = 0;  // ops that move 1 cent to a random account
  int xfer_local = -1;   // % of transfers to an account the source's owner also owns (-1: uniform)
  int index_pct = 0;     // --index: ops that are INDEX lookups on the email index
  std::string ids_file;
  std::vector<std::pair<std::string, u16>> endpoints;
  u64 session = 0;
  int request_timeout_ms = 5000;
  bool verify = false;
  u64 retry_claim_ttl_s = 0;  // --retry-claim-ttl-s (0: no claim)
};

enum Phase : int { kLoad, kRun, kDrain };

struct Shared {
  Args args;
  std::atomic<int> phase{kLoad};
  std::vector<ObjectID> ids;  // all accounts, filled between load and run
  bench::Zipf zipf;           // used when args.zipf > 0; ids is then in rank order
  std::vector<std::vector<ObjectID>> by_owner;  // --xfer-local: ids by owning shard
};

struct Thread {
  u16 index = 0;
  Shared* sh = nullptr;
  net::Client client;
  std::mt19937_64 rng;
  std::vector<ObjectID> created;
  Histogram hist;
  u64 ops = 0, errors = 0, overloaded = 0, misrouted = 0, stale = 0, replayed = 0;
  // --index: reported apart from the rest of the mix (docs/CAMPAIGN_PREP.md G5).
  Histogram index_hist;
  std::vector<u8> is_index;  // per slot
  u64 index_ok = 0, index_empty = 0, index_refused = 0, index_errors = 0;
  std::vector<u32> deposits;  // --verify: deposits sent, by index into ids
  u32 outstanding = 0;
  std::vector<u64> start;  // per slot
  std::vector<u32> free_slots;
  u64 load_target = 0, load_next = 0;
  std::string error;
};

void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

void on_load(void* ctx, const wire::RespHeader& h, ByteView) {
  auto* t = static_cast<Thread*>(ctx);
  --t->outstanding;
  t->free_slots.push_back(static_cast<u32>(h.req_id));
  if (h.status == static_cast<u16>(Status::OK)) {
    t->created.push_back(h.id);
  } else if (h.status == static_cast<u16>(Status::E_OVERLOADED)) {
    ++t->overloaded;
    --t->load_next;  // issue one more
  } else {
    ++t->errors;
  }
}

void on_run(void* ctx, const wire::RespHeader& h, ByteView body) {
  auto* t = static_cast<Thread*>(ctx);
  u32 slot = static_cast<u32>(h.req_id);
  if (t->is_index[slot]) {
    t->is_index[slot] = 0;
    t->index_hist.add(now_ns() - t->start[slot]);
    auto st = static_cast<Status>(h.status);
    if (st == Status::OK) {
      ++t->index_ok;
      // Not an error: INDEX hides objects with writes in flight (X1).
      if (body.len == 0) ++t->index_empty;
    } else if (st == Status::E_OVERLOADED) {
      ++t->index_refused;
    } else {
      ++t->index_errors;
    }
    --t->outstanding;
    t->free_slots.push_back(slot);
    return;
  }
  t->hist.add(now_ns() - t->start[slot]);
  // Turned-away requests never ran, so they do not count as ops.
  auto st = static_cast<Status>(h.status);
  if (h.flags & wire::kRespReplayed) ++t->replayed;
  if (st == Status::E_OVERLOADED) {
    ++t->overloaded;
  } else if (st == Status::E_STALE) {
    ++t->stale;  // a standby without a lease: not served
  } else {
    ++t->ops;
    // E_APP: no funds. E_REPLAYED_OLD: a resent request already applied.
    if (st == Status::E_REPLAYED_OLD) ++t->replayed;
    else if (st != Status::OK && st != Status::E_APP) ++t->errors;
  }
  --t->outstanding;
  t->free_slots.push_back(slot);
}

bool load(Thread& t) {
  u16 n = t.client.shard_count();
  u64 seq = 0;
  while (t.load_next < t.load_target || t.outstanding > 0) {
    while (t.load_next < t.load_target && !t.free_slots.empty()) {
      u32 slot = t.free_slots.back();
      t.free_slots.pop_back();
      CreateArgs a{};
      a.balance_cents = 1000;
      a.created_ns = t.load_next;
      int len = std::snprintf(a.email, sizeof(a.email), "user%u.%llu@example.com", t.index,
                              (unsigned long long)t.load_next);
      a.email_len = static_cast<u8>(len);
      a.name_len = 8;
      std::memcpy(a.name, "Customer", 8);
      wire::ReqHeader q{};
      q.kind = wire::Kind::CREATE;
      q.op = kTypeId;
      q.req_id = slot;
      t.client.send(static_cast<u16>(seq++ % n), q, ByteView(&a, sizeof(a)));
      ++t.load_next;
      ++t.outstanding;
    }
    if (t.client.poll(on_load, &t, 10) < 0) {
      t.error = t.client.error();
      return false;
    }
  }
  return true;
}

void run(Thread& t) {
  Shared& sh = *t.sh;
  const Args& a = sh.args;
  u16 n = t.client.shard_count();
  const u64 n_ids = sh.ids.size();
  while (true) {
    bool running = sh.phase.load(std::memory_order_acquire) == kRun;
    if (!running && t.outstanding == 0) break;
    while (running && !t.free_slots.empty()) {
      u32 slot = t.free_slots.back();
      t.free_slots.pop_back();
      if (a.index_pct && static_cast<int>(t.rng() % 100) < a.index_pct) {
        // A random loaded account's email, as load() built it. Any shard
        // answers for the whole node.
        const u32 who = static_cast<u32>(t.rng() % a.threads);
        const u64 per = a.objects / a.threads + (who < a.objects % a.threads);
        char body[sizeof(wire::IndexQueryBody) + 64];
        const wire::IndexQueryBody b{8, 0};
        std::memcpy(body, &b, sizeof(b));
        const int len = std::snprintf(body + sizeof(b), 64, "user%u.%llu@example.com", who,
                                      (unsigned long long)(t.rng() % std::max<u64>(1, per)));
        wire::ReqHeader q{};
        q.kind = wire::Kind::INDEX;
        q.op = kEmailIndex;
        q.req_id = slot;
        t.start[slot] = now_ns();
        t.is_index[slot] = 1;
        ++t.outstanding;
        t.client.send(static_cast<u16>(t.rng() % n), q,
                      ByteView(body, static_cast<u32>(sizeof(b) + static_cast<std::size_t>(len))));
        continue;
      }
      const u64 j = a.zipf > 0 ? sh.zipf.next(t.rng) : t.rng() % n_ids;
      const ObjectID& id = sh.ids[j];
      u16 dst;
      if (!a.route_owner) {
        dst = static_cast<u16>(t.rng() % n);
      } else {
        dst = t.client.owner_of(id);
        if (n > 1 && a.misroute_pct > 0 && static_cast<int>(t.rng() % 100) < a.misroute_pct) {
          dst = static_cast<u16>((dst + 1 + t.rng() % (n - 1)) % n);
        }
      }
      t.misrouted += dst != t.client.owner_of(id);
      wire::ReqHeader q{};
      q.kind = wire::Kind::EXEC;
      q.req_id = slot;
      q.target = id;
      t.start[slot] = now_ns();
      ++t.outstanding;
      if (a.transfer_pct && static_cast<int>(t.rng() % 100) < a.transfer_pct) {
        q.op = kOpTransfer;
        ObjectID to;
        if (a.xfer_local >= 0 && n > 1) {
          // Same owner with probability xfer_local, else another shard's account.
          u16 o = t.client.owner_of(id);
          u16 d = static_cast<int>(t.rng() % 100) < a.xfer_local
                      ? o
                      : static_cast<u16>((o + 1 + t.rng() % (n - 1)) % n);
          const auto& v = sh.by_owner[d];
          std::size_t j = t.rng() % v.size();
          if (v[j] == id) j = (j + 1) % v.size();
          to = v[j];
        } else {
          u64 j = t.rng() % n_ids;
          if (sh.ids[j] == id) j = (j + 1) % n_ids;
          to = sh.ids[j];
        }
        TransferArgs x{to, 1, t.start[slot]};
        t.client.send(dst, q, ByteView(&x, sizeof(x)));
      } else if (static_cast<int>(t.rng() % 100) < a.reads_pct) {
        q.op = kOpGetBalance;
        t.client.send(dst, q, {});
      } else {
        q.op = kOpDeposit;
        if (a.verify) ++t.deposits[j];
        AmountArgs d{1, t.start[slot]};
        t.client.send(dst, q, ByteView(&d, sizeof(d)));
      }
    }
    if (t.client.poll(on_run, &t, running ? 1 : 10) < 0) {
      t.error = t.client.error();
      return;
    }
  }
}

std::vector<int> parse_cpus(const char* s) {
  std::vector<int> v;
  for (const char* p = s; *p;) {
    v.push_back(std::atoi(p));
    while (*p && *p != ',') ++p;
    if (*p == ',') ++p;
  }
  return v;
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&] {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", k.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--host") a.host = val();
    else if (k == "--port") a.port = static_cast<u16>(std::atoi(val()));
    else if (k == "--threads") a.threads = static_cast<u16>(std::atoi(val()));
    else if (k == "--cpus") a.cpus = parse_cpus(val());
    else if (k == "--objects") a.objects = std::strtoull(val(), nullptr, 10);
    else if (k == "--seconds") a.seconds = std::atof(val());
    else if (k == "--depth") a.depth = static_cast<u32>(std::atoi(val()));
    else if (k == "--reads") a.reads_pct = std::atoi(val());
    else if (k == "--route") {
      std::string r = val();
      if (r != "owner" && r != "any") {
        std::fprintf(stderr, "--route must be owner or any\n");
        std::exit(2);
      }
      a.route_owner = r == "owner";
    } else if (k == "--misroute") a.misroute_pct = std::atoi(val());
    else if (k == "--zipf") a.zipf = std::atof(val());
    else if (k == "--transfer") a.transfer_pct = std::atoi(val());
    else if (k == "--xfer-local") a.xfer_local = std::atoi(val());
    else if (k == "--index") a.index_pct = std::atoi(val());
    else if (k == "--ids") a.ids_file = val();
    else if (k == "--endpoint") {
      std::string v = val();
      auto c = v.rfind(':');
      a.endpoints.emplace_back(v.substr(0, c), static_cast<u16>(std::atoi(v.c_str() + c + 1)));
    } else if (k == "--session") a.session = std::strtoull(val(), nullptr, 10);
    else if (k == "--request-timeout-ms") a.request_timeout_ms = std::atoi(val());
    else if (k == "--verify") a.verify = true;
    else if (k == "--retry-claim-ttl-s") a.retry_claim_ttl_s = std::strtoull(val(), nullptr, 10);
    else {
      std::fprintf(stderr, "unknown flag %s\n", k.c_str());
      std::exit(2);
    }
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  Shared sh;
  sh.args = parse(argc, argv);
  const Args& a = sh.args;
  std::vector<ObjectID> given;
  if (!a.ids_file.empty()) {
    std::ifstream f(a.ids_file, std::ios::binary);
    ObjectID id;
    while (f.read(reinterpret_cast<char*>(&id), sizeof(id))) given.push_back(id);
  }
  std::vector<Thread> threads(a.threads);
  std::barrier<> loaded(a.threads + 1), ready(a.threads + 1);
  std::vector<std::thread> ts;
  std::atomic<bool> failed{false};
  for (u16 i = 0; i < a.threads; ++i) {
    Thread& t = threads[i];
    t.index = i;
    t.sh = &sh;
    t.rng.seed(4321 + i);
    t.start.resize(a.depth);
    t.is_index.assign(a.depth, 0);
    for (u32 s = 0; s < a.depth; ++s) t.free_slots.push_back(s);
    t.load_target = given.empty() ? a.objects / a.threads + (i < a.objects % a.threads) : 0;
    ts.emplace_back([&, i] {
      Thread& t = threads[i];
      if (!a.cpus.empty()) pin(a.cpus[i % a.cpus.size()]);
      bool ok = t.client.connect(a.host, a.port);
      if (!ok) t.error = t.client.error();
      for (const auto& [h, p] : a.endpoints) t.client.add_endpoint(h, p);
      if (a.session) t.client.enable_session(a.session + i, 30000, a.request_timeout_ms);
      if (ok) ok = load(t);
      if (!ok) failed = true;
      loaded.arrive_and_wait();
      ready.arrive_and_wait();
      if (!failed) run(t);
    });
  }
  auto t0 = std::chrono::steady_clock::now();
  loaded.arrive_and_wait();
  double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  u64 load_errors = 0;
  sh.ids = given;
  for (auto& t : threads) {
    sh.ids.insert(sh.ids.end(), t.created.begin(), t.created.end());
    load_errors += t.errors;
    t.errors = 0;
    t.overloaded = 0;
  }
  if (failed || load_errors) {
    sh.phase = kDrain;
    ready.arrive_and_wait();
    for (auto& th : ts) th.join();
    for (auto& t : threads) {
      if (!t.error.empty()) std::fprintf(stderr, "thread %u: %s\n", t.index, t.error.c_str());
    }
    std::fprintf(stderr, "load failed (%llu create errors)\n", (unsigned long long)load_errors);
    return 1;
  }
  if (given.empty() && !a.ids_file.empty()) {
    std::ofstream f(a.ids_file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(sh.ids.data()),
            static_cast<std::streamsize>(sh.ids.size() * sizeof(ObjectID)));
  }
  if (a.zipf > 0) {
    sh.ids = bench::zipf_order({sh.ids});
    sh.zipf = bench::Zipf(sh.ids.size(), a.zipf);
  }
  if (a.xfer_local >= 0) {
    const net::Client& c = threads[0].client;
    sh.by_owner.resize(c.shard_count());
    for (const ObjectID& id : sh.ids) sh.by_owner[c.owner_of(id)].push_back(id);
  }
  if (a.verify) {
    for (auto& t : threads) t.deposits.assign(sh.ids.size(), 0);
  }
  sh.phase.store(kRun, std::memory_order_release);
  auto t1 = std::chrono::steady_clock::now();
  ready.arrive_and_wait();
  std::this_thread::sleep_for(std::chrono::duration<double>(a.seconds));
  sh.phase.store(kDrain, std::memory_order_release);
  for (auto& th : ts) th.join();
  double run_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();

  Histogram all;
  u64 ops = 0, errors = 0, overloaded = 0, misrouted = 0, forwarded = 0, stale = 0, replayed = 0,
      failovers = 0, timeouts = 0, resends = 0, max_resend_age_ms = 0;
  for (auto& t : threads) {
    if (!t.error.empty()) {
      std::fprintf(stderr, "thread %u: %s\n", t.index, t.error.c_str());
      return 1;
    }
    all.merge(t.hist);
    ops += t.ops;
    errors += t.errors;
    overloaded += t.overloaded;
    stale += t.stale;
    replayed += t.replayed;
    failovers += t.client.failovers();
    timeouts += t.client.timeouts();
    resends += t.client.resends();
    max_resend_age_ms = std::max(max_resend_age_ms, t.client.max_resend_age_ms());
    misrouted += t.misrouted;
    forwarded += t.client.forwarded();
  }
  u16 n = threads[0].client.shard_count();
  std::printf("net_bench shards=%u threads=%u depth=%u/thread route=%s misroute=%d%%\n", n,
              a.threads, a.depth, a.route_owner ? "owner" : "any", a.misroute_pct);
  std::printf("load: %zu objects in %.2fs (%.0f creates/s)\n", sh.ids.size(), load_s,
              sh.ids.size() / load_s);
  std::printf("run: reads=%d%% seconds=%.2f\n", a.reads_pct, run_s);
  std::printf("  ops/s %.0f  errors %llu  overloaded %llu  stale %llu\n", ops / run_s,
              (unsigned long long)errors, (unsigned long long)overloaded, (unsigned long long)stale);
  std::printf("  latency ns  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f\n", all.percentile(50),
              all.percentile(95), all.percentile(99), all.percentile(99.9));
  std::printf("  sent to non-owner %.1f%%  forwarded by server %.1f%%\n",
              100.0 * misrouted / std::max<u64>(1, ops), 100.0 * forwarded / std::max<u64>(1, ops));
  u64 index_errors = 0;
  if (a.index_pct) {
    Histogram ih;
    u64 ok = 0, empty = 0, refused = 0;
    for (auto& t : threads) {
      ih.merge(t.index_hist);
      ok += t.index_ok;
      empty += t.index_empty;
      refused += t.index_refused;
      index_errors += t.index_errors;
    }
    std::printf("  index ops/s %.0f  ok %llu  empty %llu  refused %llu  errors %llu\n", ok / run_s,
                (unsigned long long)ok, (unsigned long long)empty, (unsigned long long)refused,
                (unsigned long long)index_errors);
    std::printf("  index latency ns  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f\n", ih.percentile(50),
                ih.percentile(95), ih.percentile(99), ih.percentile(99.9));
  }
  if (a.session) {
    std::printf("  failovers %llu  replayed %llu  timeouts %llu  resends %llu  max_resend_age_ms %llu\n",
                (unsigned long long)failovers, (unsigned long long)replayed, (unsigned long long)timeouts,
                (unsigned long long)resends, (unsigned long long)max_resend_age_ms);
  }
  // R11 run validity: the margin is 10x. The real horizon is longer than the
  // TTL (it runs from the session's last applied request, and expiry runs
  // only at a checkpoint), so a resend inside a tenth of it is before expiry
  // with room for clock slew and checkpoint timing.
  bool retry_claim_invalid = false;
  if (a.retry_claim_ttl_s) {
    const u64 limit_ms = a.retry_claim_ttl_s * 1000 / 10;
    retry_claim_invalid = !a.session || max_resend_age_ms > limit_ms;
    std::printf("  retry claim: max_resend_age_ms %llu, limit %llu (TTL %llu s / 10): %s\n",
                (unsigned long long)max_resend_age_ms, (unsigned long long)limit_ms,
                (unsigned long long)a.retry_claim_ttl_s,
                retry_claim_invalid ? (a.session ? "INVALID" : "INVALID (no --session)") : "valid");
  }
  if (a.verify) {
    // Every account's balance is its start (1000) plus the deposits sent.
    net::Client& c = threads[0].client;
    struct V {
      const std::vector<ObjectID>* ids;
      std::vector<i64> bal;
      u64 got = 0;
    } v{&sh.ids, std::vector<i64>(sh.ids.size(), -1)};
    auto on = [](void* ctx, const wire::RespHeader& h, ByteView body) {
      auto* x = static_cast<V*>(ctx);
      BalanceResult b{};
      if (static_cast<Status>(h.status) == Status::OK && body.len == sizeof(b)) {
        std::memcpy(&b, body.data, sizeof(b));
        x->bal[h.req_id] = b.balance_cents;
      }
      ++x->got;
    };
    for (u64 k = 0; k < sh.ids.size();) {
      u64 end = std::min<u64>(sh.ids.size(), k + 512);
      for (u64 j = k; j < end; ++j) {
        wire::ReqHeader q{};
        q.kind = wire::Kind::EXEC;
        q.op = kOpGetBalance;
        q.req_id = j;
        q.target = sh.ids[j];
        c.send(c.owner_of(sh.ids[j]), q, {});
      }
      while (v.got < end) {
        if (c.poll(on, &v, 100) < 0) {
          std::fprintf(stderr, "verify: %s\n", c.error().c_str());
          return 1;
        }
      }
      k = end;
    }
    u64 bad = 0, sent = 0;
    for (u64 j = 0; j < sh.ids.size(); ++j) {
      i64 want = 1000;
      for (auto& t : threads) want += t.deposits[j];
      sent += static_cast<u64>(want - 1000);
      if (v.bal[j] != want) {
        if (bad < 5) {
          std::printf("  verify: account %llu has %lld, expected %lld\n", (unsigned long long)j,
                      (long long)v.bal[j], (long long)want);
        }
        ++bad;
      }
    }
    std::printf("  verify: %zu accounts, %llu deposits, %llu mismatches\n", sh.ids.size(),
                (unsigned long long)sent, (unsigned long long)bad);
    if (bad) return 1;
  }
  if (retry_claim_invalid) return 3;
  return errors || index_errors ? 1 : 0;
}
