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
// The operation mix. Either give it exactly, as shares of TOTAL operations
// that add up to 100:
//   --mix read=70,deposit=15,transfer=10,index=5
// (one draw per operation picks its kind), or use the older nested flags:
//   --index PCT     share of all ops that are INDEX lookups;
//   --transfer PCT  share of the remaining (non-index) ops that are transfers;
//   --reads PCT     share of the rest that are get_balance; the others deposit.
// The run prints the effective share of each kind of the total either way.
// INDEX ops look up a random loaded account's email (--objects and --threads
// must match the load). Each kind's latency is reported apart, and the total.
//
// Load model. By default the loop is closed: each thread keeps --depth
// requests in flight, and a slow answer delays the next send. --rate R makes
// it open-loop (bench/rate_schedule.h): R ops/s in total, on a fixed periodic
// schedule per thread. Latency runs from each op's INTENDED send time, so a
// stall is charged to every op it delays. --depth then only bounds the ops in
// flight: an op that is due while every slot is busy waits (it is owed), and
// its wait counts in its latency. Scheduled work is never dropped.
//   --warmup S   ops due in the first S seconds run but are not measured
//   --seconds S  the measured window, after the warm-up
//   --drain-s S  after the window, owed and in-flight ops get S seconds
//                (default 10) to finish; the run reports what was left
// Ops refused with E_OVERLOADED are counted as overloaded and not resent.
// Client threads poll without sleeping when the next op is due within 1.5 ms.

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
#include "rate_schedule.h"
#include "zipf.h"
#include "snc/net.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using snc::bench::RateSchedule;

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
  // --mix: exact shares of total ops (mix_set), instead of the nested flags.
  bool mix_set = false;
  int mix_read = 0, mix_deposit = 0, mix_transfer = 0, mix_index = 0;
  u64 rate = 0;          // --rate: open-loop total ops/s (0: closed loop)
  double warmup = 0;     // --warmup (with --rate)
  double drain_s = 10;   // --drain-s (with --rate)
  std::string ids_file;
  std::vector<std::pair<std::string, u16>> endpoints;
  u64 session = 0;
  int request_timeout_ms = 5000;
  bool verify = false;
  u64 retry_claim_ttl_s = 0;  // --retry-claim-ttl-s (0: no claim)
};

enum Phase : int { kLoad, kRun, kDrain };

// The kind of each operation, for its own latency histogram.
enum OpKind : u8 { kRead, kDeposit, kTransfer, kIndex, kKinds };
const char* const kKindName[kKinds] = {"read", "deposit", "transfer", "index"};

struct Shared {
  Args args;
  std::atomic<int> phase{kLoad};
  // --rate: the schedule's origin, the measured window and the drain limit.
  u64 t0_ns = 0, measure_ns = 0, end_ns = 0, deadline_ns = 0;
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
  // Each kind's latency, and per slot the kind and whether it is measured.
  Histogram kind_hist[kKinds];
  std::vector<u8> kind, measured;
  // --rate
  RateSchedule sched;
  u64 next_k = 0;               // next scheduled op to issue
  Histogram lateness;           // issue time minus intended time
  u64 late_1ms = 0;             // issued more than 1 ms after due
  u64 scheduled_by_end = 0, issued_by_end = 0, outstanding_at_end = 0, in_window = 0, in_window_index = 0;
  u64 scheduled_in_window = 0;  // due in [warm-up end, window end): the measured ops
  u64 served_all = 0;           // every served op, measured or not (for per-op costs)
  bool drained = false;
  std::vector<u32> deposits;  // --verify: deposits sent, by index into ids
  u32 outstanding = 0;
  std::vector<u64> start;  // per slot
  std::vector<u32> free_slots;
  u64 load_target = 0, load_next = 0;
  std::vector<u64> load_num;    // per slot: the account number being created
  std::vector<u64> load_retry;  // numbers refused with E_OVERLOADED, to resend
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
    // Resend this same account number, so every email user<i>.<n> with
    // n < load_target exists exactly once (INDEX lookups rely on it).
    t->load_retry.push_back(t->load_num[h.req_id]);
  } else {
    ++t->errors;
  }
}

void on_run(void* ctx, const wire::RespHeader& h, ByteView body) {
  auto* t = static_cast<Thread*>(ctx);
  const u32 slot = static_cast<u32>(h.req_id);
  const u64 now = now_ns();
  const u64 lat = now - t->start[slot];
  const bool measured = t->measured[slot];
  const Shared& sh = *t->sh;
  --t->outstanding;
  t->free_slots.push_back(slot);
  auto st = static_cast<Status>(h.status);
  // Errors and refusals count whether or not the op is measured: a warm-up
  // error still fails the run.
  if (t->is_index[slot]) {
    t->is_index[slot] = 0;
    bool served = false;
    if (st == Status::OK) {
      // Not an error: INDEX hides objects with writes in flight (X1).
      served = true;
      if (measured) {
        ++t->index_ok;
        if (body.len == 0) ++t->index_empty;
      }
    } else if (st == Status::E_OVERLOADED) {
      ++t->index_refused;
    } else {
      ++t->index_errors;
    }
    if (measured) {
      t->index_hist.add(lat);
      t->kind_hist[kIndex].add(lat);
      if (served && sh.args.rate && now < sh.end_ns) ++t->in_window, ++t->in_window_index;
    }
    t->served_all += served;
    return;
  }
  // Turned-away requests never ran, so they do not count as ops.
  if (h.flags & wire::kRespReplayed) ++t->replayed;
  bool served = false;
  if (st == Status::E_OVERLOADED) {
    ++t->overloaded;
  } else if (st == Status::E_STALE) {
    ++t->stale;  // a standby without a lease: not served
  } else {
    served = true;
    if (measured) ++t->ops;
    // E_APP: no funds. E_REPLAYED_OLD: a resent request already applied.
    if (st == Status::E_REPLAYED_OLD) ++t->replayed;
    else if (st != Status::OK && st != Status::E_APP) ++t->errors;
  }
  if (measured) {
    t->hist.add(lat);
    t->kind_hist[t->kind[slot]].add(lat);
    if (served && sh.args.rate && now < sh.end_ns) ++t->in_window;
  }
  t->served_all += served;
}

bool load(Thread& t) {
  u16 n = t.client.shard_count();
  u64 seq = 0;
  while (t.load_next < t.load_target || !t.load_retry.empty() || t.outstanding > 0) {
    while ((t.load_next < t.load_target || !t.load_retry.empty()) && !t.free_slots.empty()) {
      u32 slot = t.free_slots.back();
      t.free_slots.pop_back();
      u64 num;
      if (!t.load_retry.empty()) {
        num = t.load_retry.back();
        t.load_retry.pop_back();
      } else {
        num = t.load_next++;
      }
      t.load_num[slot] = num;
      CreateArgs a{};
      a.balance_cents = 1000;
      a.created_ns = num;
      int len = std::snprintf(a.email, sizeof(a.email), "user%u.%llu@example.com", t.index,
                              (unsigned long long)num);
      a.email_len = static_cast<u8>(len);
      a.name_len = 8;
      std::memcpy(a.name, "Customer", 8);
      wire::ReqHeader q{};
      q.kind = wire::Kind::CREATE;
      q.op = kTypeId;
      q.req_id = slot;
      t.client.send(static_cast<u16>(seq++ % n), q, ByteView(&a, sizeof(a)));
      ++t.outstanding;
    }
    if (t.client.poll(on_load, &t, 10) < 0) {
      t.error = t.client.error();
      return false;
    }
  }
  return true;
}

// --mix: one draw in [0, 100) picks the kind; the shares are of total ops.
OpKind mix_kind(const Args& a, u64 draw) {
  int r = static_cast<int>(draw % 100);
  if (r < a.mix_index) return kIndex;
  r -= a.mix_index;
  if (r < a.mix_transfer) return kTransfer;
  r -= a.mix_transfer;
  if (r < a.mix_read) return kRead;
  return kDeposit;
}

// Sends one operation on `slot`. Its latency runs from `start` (now, closed
// loop; the intended send time, open loop). With the nested flags the random
// draws come in the same order as before --mix existed, so old runs repeat.
void issue(Thread& t, u32 slot, u64 start, bool measured) {
  Shared& sh = *t.sh;
  const Args& a = sh.args;
  const u16 n = t.client.shard_count();
  const u64 n_ids = sh.ids.size();
  t.start[slot] = start;
  t.measured[slot] = measured;
  ++t.outstanding;
  OpKind mixed = kRead;
  if (a.mix_set) mixed = mix_kind(a, t.rng());
  if (a.mix_set ? mixed == kIndex
                : a.index_pct && static_cast<int>(t.rng() % 100) < a.index_pct) {
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
    t.is_index[slot] = 1;
    t.kind[slot] = kIndex;
    t.client.send(static_cast<u16>(t.rng() % n), q,
                  ByteView(body, static_cast<u32>(sizeof(b) + static_cast<std::size_t>(len))));
    return;
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
  const bool transfer =
      a.mix_set ? mixed == kTransfer
                : a.transfer_pct && static_cast<int>(t.rng() % 100) < a.transfer_pct;
  if (transfer) {
    q.op = kOpTransfer;
    t.kind[slot] = kTransfer;
    ObjectID to;
    if (a.xfer_local >= 0 && n > 1) {
      // Same owner with probability xfer_local, else another shard's account.
      u16 o = t.client.owner_of(id);
      u16 d = static_cast<int>(t.rng() % 100) < a.xfer_local
                  ? o
                  : static_cast<u16>((o + 1 + t.rng() % (n - 1)) % n);
      const auto& v = sh.by_owner[d];
      std::size_t k = t.rng() % v.size();
      if (v[k] == id) k = (k + 1) % v.size();
      to = v[k];
    } else {
      u64 k = t.rng() % n_ids;
      if (sh.ids[k] == id) k = (k + 1) % n_ids;
      to = sh.ids[k];
    }
    TransferArgs x{to, 1, start};
    t.client.send(dst, q, ByteView(&x, sizeof(x)));
  } else if (a.mix_set ? mixed == kRead : static_cast<int>(t.rng() % 100) < a.reads_pct) {
    q.op = kOpGetBalance;
    t.kind[slot] = kRead;
    t.client.send(dst, q, {});
  } else {
    q.op = kOpDeposit;
    t.kind[slot] = kDeposit;
    if (a.verify) ++t.deposits[j];
    AmountArgs d{1, start};
    t.client.send(dst, q, ByteView(&d, sizeof(d)));
  }
}

// Closed loop: keep every slot busy until the main thread ends the run.
void run(Thread& t) {
  Shared& sh = *t.sh;
  while (true) {
    bool running = sh.phase.load(std::memory_order_acquire) == kRun;
    if (!running && t.outstanding == 0) break;
    while (running && !t.free_slots.empty()) {
      u32 slot = t.free_slots.back();
      t.free_slots.pop_back();
      issue(t, slot, now_ns(), true);
    }
    if (t.client.poll(on_run, &t, running ? 1 : 10) < 0) {
      t.error = t.client.error();
      return;
    }
  }
}

// Open loop (--rate): issue each op when it is due, or as soon as a slot
// frees after that; never skip one. Ops due before the end of the window are
// all issued, during the drain if need be, until the drain limit.
void run_rate(Thread& t) {
  const Shared& sh = *t.sh;
  const u64 t0 = sh.t0_ns, end = sh.end_ns, deadline = sh.deadline_ns;
  const u64 total = t.sched.due_before(end - t0);  // ops due in warm-up + window
  bool past_end = false;
  while (true) {
    const u64 now = now_ns();
    if (!past_end && now >= end) {
      past_end = true;
      t.scheduled_by_end = total;
      t.scheduled_in_window = total - t.sched.due_before(sh.measure_ns - t0);
      t.issued_by_end = t.next_k;
      t.outstanding_at_end = t.outstanding;
    }
    const u64 due = past_end ? total : now <= t0 ? 0 : t.sched.due_before(now - t0);
    while (t.next_k < due && !t.free_slots.empty()) {
      const u32 slot = t.free_slots.back();
      t.free_slots.pop_back();
      const u64 intended = t0 + t.sched.at(t.next_k);
      const u64 late = now - intended;  // due means intended < now
      t.lateness.add(late);
      if (late > 1000000) ++t.late_1ms;
      issue(t, slot, intended, intended >= sh.measure_ns);
      ++t.next_k;
    }
    if (past_end && t.next_k == total && t.outstanding == 0) {
      t.drained = true;
      return;
    }
    if (now >= deadline) return;  // not drained: the report says what is left
    // Poll without waiting only when the next op is due within 1.5 ms and a
    // slot is free for it. Otherwise wait up to 1 ms: poll returns at the
    // first answer, which is all that can free a slot.
    int wait_ms = 1;
    if (t.next_k < total && t.next_k >= due && !t.free_slots.empty() &&
        t0 + t.sched.at(t.next_k) <= now + 1500000) {
      wait_ms = 0;
    }
    if (t.client.poll(on_run, &t, wait_ms) < 0) {
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

// --mix read=R,deposit=D,transfer=X,index=I: whole percentages of total ops,
// each kind at most once, adding up to exactly 100. Missing kinds are 0.
void parse_mix(const char* text, Args& a) {
  auto fail = [&](const char* why) {
    std::fprintf(stderr, "--mix %s: %s\n", text, why);
    std::exit(2);
  };
  int* field[kKinds] = {&a.mix_read, &a.mix_deposit, &a.mix_transfer, &a.mix_index};
  bool set[kKinds] = {};
  a.mix_read = a.mix_deposit = a.mix_transfer = a.mix_index = 0;
  std::string s = text;
  std::size_t pos = 0;
  while (pos <= s.size()) {
    std::size_t comma = s.find(',', pos);
    if (comma == std::string::npos) comma = s.size();
    const std::string item = s.substr(pos, comma - pos);
    const std::size_t eq = item.find('=');
    if (eq == std::string::npos) fail("each item is kind=PCT");
    const std::string name = item.substr(0, eq), num = item.substr(eq + 1);
    int kind = -1;
    for (int k = 0; k < kKinds; ++k) {
      if (name == kKindName[k]) kind = k;
    }
    if (kind < 0) fail("kinds are read, deposit, transfer, index");
    if (set[kind]) fail("a kind is given twice");
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos || num.size() > 3) {
      fail("a share is a whole number 0..100");
    }
    *field[kind] = std::atoi(num.c_str());
    set[kind] = true;
    pos = comma + 1;
  }
  if (a.mix_read + a.mix_deposit + a.mix_transfer + a.mix_index != 100) fail("the shares must add up to 100");
  a.mix_set = true;
}

Args parse(int argc, char** argv) {
  Args a;
  std::vector<std::string> seen;
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
    else if (k == "--mix") {
      if (a.mix_set) {
        std::fprintf(stderr, "--mix given twice\n");
        std::exit(2);
      }
      parse_mix(val(), a);
    } else if (k == "--rate") {
      const std::string r = val();
      if (r.empty() || r.size() > 12 || r.find_first_not_of("0123456789") != std::string::npos) {
        std::fprintf(stderr, "--rate takes a whole number of ops/s, digits only\n");
        std::exit(2);
      }
      a.rate = std::strtoull(r.c_str(), nullptr, 10);
    }
    else if (k == "--warmup") a.warmup = std::atof(val());
    else if (k == "--drain-s") a.drain_s = std::atof(val());
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
    seen.push_back(k);
  }
  auto given = [&](const char* f) { return std::find(seen.begin(), seen.end(), f) != seen.end(); };
  auto bad = [](const char* m) {
    std::fprintf(stderr, "%s\n", m);
    std::exit(2);
  };
  if (a.mix_set && (given("--index") || given("--transfer") || given("--reads"))) {
    bad("--mix replaces --index, --transfer and --reads; give one or the other");
  }
  if (!a.rate && (given("--warmup") || given("--drain-s"))) bad("--warmup and --drain-s need --rate");
  if (given("--rate") && a.rate == 0) bad("--rate must be at least 1 op/s");
  if (a.warmup < 0 || a.drain_s < 0 || a.seconds < 0) bad("durations must not be negative");
  for (int pct : {a.index_pct, a.transfer_pct, a.reads_pct, a.misroute_pct}) {
    if (pct < 0 || pct > 100) bad("percentages must be 0..100");
  }
  if (a.threads == 0 || a.depth == 0) bad("--threads and --depth must be at least 1");
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
    t.kind.assign(a.depth, kRead);
    t.measured.assign(a.depth, 1);
    t.load_num.assign(a.depth, 0);
    if (a.rate) t.sched = RateSchedule(a.rate, a.threads, i);
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
      if (!failed) {
        if (a.rate) run_rate(t);
        else run(t);
      }
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
  double run_s;
  if (a.rate) {
    // The schedule starts 5 ms from now, so every thread is past the barrier.
    auto ns = [](double sec) { return static_cast<u64>(sec * 1e9); };
    sh.t0_ns = now_ns() + 5000000;
    sh.measure_ns = sh.t0_ns + ns(a.warmup);
    sh.end_ns = sh.measure_ns + ns(a.seconds);
    sh.deadline_ns = sh.end_ns + ns(a.drain_s);
    sh.phase.store(kRun, std::memory_order_release);
    ready.arrive_and_wait();
    for (auto& th : ts) th.join();
    sh.phase.store(kDrain, std::memory_order_release);
    run_s = static_cast<double>(sh.end_ns - sh.measure_ns) / 1e9;  // the measured window
  } else {
    sh.phase.store(kRun, std::memory_order_release);
    auto t1 = std::chrono::steady_clock::now();
    ready.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::duration<double>(a.seconds));
    sh.phase.store(kDrain, std::memory_order_release);
    for (auto& th : ts) th.join();
    run_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  }

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
  // With --rate, the ops/s lines count ops answered within the window only.
  u64 in_window = 0, in_window_index = 0, served_all = 0;
  for (auto& t : threads) {
    in_window += t.in_window;
    in_window_index += t.in_window_index;
    served_all += t.served_all;
  }
  const u64 exec_shown = a.rate ? in_window - in_window_index : ops;
  if (a.mix_set) {
    std::printf("run: mix=read=%d,deposit=%d,transfer=%d,index=%d seconds=%.2f\n", a.mix_read, a.mix_deposit,
                a.mix_transfer, a.mix_index, run_s);
  } else {
    std::printf("run: reads=%d%% seconds=%.2f\n", a.reads_pct, run_s);
  }
  std::printf("  ops/s %.0f  errors %llu  overloaded %llu  stale %llu\n", exec_shown / run_s,
              (unsigned long long)errors, (unsigned long long)overloaded, (unsigned long long)stale);
  std::printf("  latency ns  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f\n", all.percentile(50),
              all.percentile(95), all.percentile(99), all.percentile(99.9));
  std::printf("  sent to non-owner %.1f%%  forwarded by server %.1f%%\n",
              100.0 * misrouted / std::max<u64>(1, ops), 100.0 * forwarded / std::max<u64>(1, ops));
  u64 index_errors = 0;
  if (a.index_pct || a.mix_index) {
    Histogram ih;
    u64 ok = 0, empty = 0, refused = 0;
    for (auto& t : threads) {
      ih.merge(t.index_hist);
      ok += t.index_ok;
      empty += t.index_empty;
      refused += t.index_refused;
      index_errors += t.index_errors;
    }
    std::printf("  index ops/s %.0f  ok %llu  empty %llu  refused %llu  errors %llu\n",
                (a.rate ? in_window_index : ok) / run_s,
                (unsigned long long)ok, (unsigned long long)empty, (unsigned long long)refused,
                (unsigned long long)index_errors);
    std::printf("  index latency ns  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f\n", ih.percentile(50),
                ih.percentile(95), ih.percentile(99), ih.percentile(99.9));
  }
  {
    // The effective share of each kind, of total ops.
    double share[kKinds];
    if (a.mix_set) {
      share[kRead] = a.mix_read, share[kDeposit] = a.mix_deposit;
      share[kTransfer] = a.mix_transfer, share[kIndex] = a.mix_index;
    } else {
      const double rest = 100.0 - a.index_pct;
      share[kIndex] = a.index_pct;
      share[kTransfer] = rest * a.transfer_pct / 100.0;
      share[kRead] = (rest - share[kTransfer]) * a.reads_pct / 100.0;
      share[kDeposit] = rest - share[kTransfer] - share[kRead];
    }
    std::printf("  mix of total ops (%s): read %.2f%%  deposit %.2f%%  transfer %.2f%%  index %.2f%%\n",
                a.mix_set ? "--mix" : "nested flags", share[kRead], share[kDeposit], share[kTransfer],
                share[kIndex]);
    // Every kind together: EXEC ops served (ops) plus INDEX lookups answered
    // OK. The latency covers every measured answer, refusals included.
    Histogram kinds[kKinds], total;
    u64 index_ok = 0;
    for (auto& t : threads) {
      for (int k = 0; k < kKinds; ++k) kinds[k].merge(t.kind_hist[k]);
      index_ok += t.index_ok;
    }
    for (auto& h : kinds) total.merge(h);
    // Closed loop: every op answered during the run. --rate: the ops
    // answered within the window. served_all also counts the warm-up and
    // drain: the denominator for per-op server costs, whose samples span
    // the whole run.
    const u64 idx_shown = a.rate ? in_window_index : index_ok;
    std::printf("  total ops/s %.0f  (exec %llu + index %llu in %.2fs)  served_all %llu\n",
                (exec_shown + idx_shown) / run_s, (unsigned long long)exec_shown,
                (unsigned long long)idx_shown, run_s, (unsigned long long)served_all);
    auto line = [](const char* what, const Histogram& h) {
      std::printf("  %s latency ns  n %llu  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f  max %llu\n", what,
                  (unsigned long long)h.count(), h.percentile(50), h.percentile(95), h.percentile(99),
                  h.percentile(99.9), (unsigned long long)h.max());
    };
    line("total", total);
    for (int k = 0; k < kKinds; ++k) {
      if (kinds[k].count()) line(kKindName[k], kinds[k]);
    }
  }
  bool undrained = false;
  if (a.rate) {
    Histogram late;
    u64 scheduled = 0, in_win_sched = 0, issued = 0, at_end = 0, in_window = 0, late_1ms = 0, unissued = 0,
        left = 0;
    for (auto& t : threads) {
      late.merge(t.lateness);
      // scheduled: every op due before the window's end, warm-up included.
      scheduled += t.scheduled_by_end;
      in_win_sched += t.scheduled_in_window;
      issued += t.issued_by_end;
      at_end += t.outstanding_at_end;
      in_window += t.in_window;
      late_1ms += t.late_1ms;
      unissued += t.scheduled_by_end - t.next_k;
      left += t.outstanding;
      undrained |= !t.drained;
    }
    std::printf("rate: offered %llu ops/s  warmup %.2fs  window %.2fs  drain limit %.2fs\n",
                (unsigned long long)a.rate, a.warmup, run_s, a.drain_s);
    // scheduled covers warm-up and window; scheduled_in_window only the
    // measured window, the denominator of the backlog share.
    std::printf("  scheduled %llu  scheduled_in_window %llu  issued_by_end %llu  completed_in_window %llu  "
                "achieved %.0f ops/s\n",
                (unsigned long long)scheduled, (unsigned long long)in_win_sched, (unsigned long long)issued,
                (unsigned long long)in_window, in_window / run_s);
    std::printf("  end_backlog %llu (%.4f%% of scheduled_in_window)  in_flight_at_end %llu\n",
                (unsigned long long)(scheduled - issued),
                100.0 * (scheduled - issued) / std::max<u64>(1, in_win_sched), (unsigned long long)at_end);
    std::printf("  issue lateness ns  p50 %.0f  p99 %.0f  p99.9 %.0f  max %llu  late_over_1ms %llu\n",
                late.percentile(50), late.percentile(99), late.percentile(99.9), (unsigned long long)late.max(),
                (unsigned long long)late_1ms);
    std::printf("  drained %s  remaining_unissued %llu  remaining_in_flight %llu\n", undrained ? "no" : "yes",
                (unsigned long long)unissued, (unsigned long long)left);
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
  // An undrained --rate run is a result (the rate was not sustained), not a
  // tool failure: it exits 0 and says "drained no".
  return errors || index_errors ? 1 : 0;
}
