// Open-loop load generator for the B1 Garnet baseline (bench/garnet,
// docs/DIRECTION_COMPARISON_PLAN.md 4.2), on net_bench's schedule
// (bench/rate_schedule.h) and with its op mix, verify and ack log.
//
// Accounts are 16-byte keys {u64 1, u64 n + 1} (little-endian) holding
// 200-byte values whose first 8 bytes are the balance. Reads are GET,
// deposits ACCT.DEP key 1, transfers ACCT.XFER src dst 1 between uniformly
// chosen accounts; creates are SET.
//
// Usage: resp_bench [--host H] [--port P] [--threads T] [--conns C] [--cpus L]
//                   [--objects N] [--depth D] --rate R [--warmup S]
//                   [--seconds S] [--drain-s S] [--transfer PCT] [--reads PCT]
//                   [--start-balance C] [--verify] [--ack-log FILE] [--no-load]
//                   [--probe-pct P --probe-slo-us S [--probe-grace-us G]
//                    [--probe-accounts K]]
// --transfer PCT of ops are transfers; --reads PCT of the rest are reads; the
// others deposit. Each thread holds C connections for writes and C for reads,
// so a read never waits behind a write's AOF commit. --depth bounds each
// thread's ops in flight. --no-load uses existing accounts 0..N-1 (and the
// probe accounts N..N+K-1).
// --probe-pct: P% of transfers credit one of K extra accounts that nothing
// else touches, as in net_bench. A probe is settled only if the transfer's
// commit-waited reply came back by intended send + S + G and one GET of the
// target at intended send + S answers by then with at least every probe credit
// sent to it (docs/DIRECTION_COMPARISON_PLAN.md 4.1); otherwise it is a miss.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "rate_schedule.h"

using namespace snc;
using snc::bench::RateSchedule;

namespace {

u64 now_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

struct Args {
  std::string host = "127.0.0.1";
  u16 port = 6379;
  u16 threads = 1, conns = 1;
  std::vector<int> cpus;
  u64 objects = 1000;
  u32 depth = 64;
  u64 rate = 0;
  double warmup = 0, seconds = 4, drain_s = 10;
  int transfer_pct = 0, reads_pct = 50;
  i64 start_balance = 1000;
  bool verify = false, load = true;
  std::string ack_log;
  double probe_pct = 0;
  u64 probe_slo_ns = 0, probe_grace_ns = 0;
  u64 probe_accounts = 1024;
  u64 accounts() const { return objects + (probe_pct > 0 ? probe_accounts : 0); }
};

enum Kind : u8 { kRead, kDeposit, kTransfer, kCreate, kProbe, kKinds };
const char* const kKindName[kKinds] = {"read", "deposit", "transfer", "create", "probe read"};
enum Applied : u8 { kApplied, kNotApplied, kInDoubt };
constexpr u64 kNone = ~0ull;
constexpr u32 kNoProbe = ~0u;

struct Op {
  u64 start, from, to;
  Kind kind;
  bool measured;
  u32 probe = kNoProbe;  // the probe record of a probed transfer or a probe read
};

struct Probe {
  u64 read_at, due_by, p;
  i64 want;
  bool measured, rejected = false, acked = false, seen = false;
  u8 pending = 2;  // the transfer's reply and the read's; p is reused after both
};

struct Ack {
  u64 from, to, sent, acked;
  char kind;
};

// One RESP connection: requests go out in order and replies come back in the
// same order, so each reply belongs to the oldest pending op.
struct Conn {
  int fd = -1;
  std::string out, in;
  std::size_t in_pos = 0;
  std::deque<Op> pending;
};

struct Reply {
  char type;   // + - : $ (bulk) or 0 for a null bulk
  i64 num;     // : value, or the balance in a $ value
  std::string text;  // + and - text
};

// Parses one reply at c.in[in_pos...]; false if it is not complete yet.
bool parse(Conn& c, Reply& r) {
  const std::string& s = c.in;
  std::size_t p = c.in_pos;
  std::size_t eol = s.find("\r\n", p);
  if (eol == std::string::npos) return false;
  r.type = s[p];
  if (r.type == '$') {
    long n = std::strtol(s.c_str() + p + 1, nullptr, 10);
    if (n < 0) {
      r.type = 0;
      c.in_pos = eol + 2;
      return true;
    }
    if (s.size() < eol + 2 + static_cast<std::size_t>(n) + 2) return false;
    r.num = 0;
    if (n >= 8) std::memcpy(&r.num, s.data() + eol + 2, 8);
    c.in_pos = eol + 2 + static_cast<std::size_t>(n) + 2;
    return true;
  }
  if (r.type == ':') r.num = std::strtoll(s.c_str() + p + 1, nullptr, 10);
  else r.text.assign(s, p + 1, eol - p - 1);
  c.in_pos = eol + 2;
  return true;
}

void key(char* out, u64 n) {
  const u64 k[2] = {1, n + 1};
  std::memcpy(out, k, 16);
}

void bulk(std::string& o, const char* p, std::size_t n) {
  o += '$';
  o += std::to_string(n);
  o += "\r\n";
  o.append(p, n);
  o += "\r\n";
}

void command(std::string& o, std::initializer_list<std::pair<const char*, std::size_t>> parts) {
  o += '*';
  o += std::to_string(parts.size());
  o += "\r\n";
  for (const auto& [p, n] : parts) bulk(o, p, n);
}

struct Shared {
  Args a;
  u64 t0 = 0, measure = 0, end = 0, deadline = 0;
};

struct Thread {
  u16 index = 0;
  Shared* sh = nullptr;
  std::vector<Conn> conns;  // the first half carry writes, the second half reads
  std::size_t next_write = 0, next_read = 0;
  std::vector<pollfd> pf;
  std::mt19937_64 rng;
  RateSchedule sched;
  u64 next_k = 0, outstanding = 0;
  Histogram all, kind[kKinds], lateness;
  u64 ops = 0, errors = 0, rejected = 0, in_window = 0, late_1ms = 0;
  u64 scheduled_in_window = 0, issued_by_end = 0, outstanding_at_end = 0;
  bool drained = false;
  std::vector<i64> delta;
  std::vector<u32> doubt_down, doubt_up;
  std::vector<Ack> acks;
  std::vector<i64>* scan = nullptr;  // verify: where reads land
  std::vector<Probe> probes;
  std::size_t probe_head = 0;
  std::vector<u64> probe_free;
  std::vector<i64> probe_expected;  // start plus every probe credit sent, by p
  u64 probe_n = 0, probe_miss = 0, probe_skipped = 0, probe_reads = 0;
  std::string error;
};

bool connect_all(Thread& t) {
  const Args& a = t.sh->a;
  for (u16 i = 0; i < 2 * a.conns; ++i) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(a.port);
    inet_pton(AF_INET, a.host.c_str(), &sa.sin_addr);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
      t.error = std::string("connect: ") + std::strerror(errno);
      return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    t.conns.push_back({});
    t.conns.back().fd = fd;
  }
  return true;
}

void send_op(Thread& t, const Op& op) {
  const std::size_t half = t.conns.size() / 2;
  Conn& c = op.kind == kRead || op.kind == kProbe ? t.conns[half + t.next_read++ % half] : t.conns[t.next_write++ % half];
  char k1[16], k2[16];
  key(k1, op.from);
  switch (op.kind) {
    case kRead:
    case kProbe:
      command(c.out, {{"GET", 3}, {k1, 16}});
      break;
    case kDeposit:
      command(c.out, {{"ACCT.DEP", 8}, {k1, 16}, {"1", 1}});
      break;
    case kTransfer:
      key(k2, op.to);
      command(c.out, {{"ACCT.XFER", 9}, {k1, 16}, {k2, 16}, {"1", 1}});
      break;
    case kCreate: {
      char v[200] = {};
      std::memcpy(v, &t.sh->a.start_balance, 8);
      command(c.out, {{"SET", 3}, {k1, 16}, {v, 200}});
      break;
    }
    default:
      break;
  }
  c.pending.push_back(op);
  ++t.outstanding;
}

void probe_done(Thread& t, Probe& r) {
  if (--r.pending) return;
  t.probe_free.push_back(r.p);
  if (!r.measured || r.rejected) return;
  ++t.probe_n;
  t.probe_miss += !(r.acked && r.seen);
}

void on_reply(Thread& t, const Op& op, const Reply& r, u64 now) {
  --t.outstanding;
  if (op.kind == kProbe) {
    Probe& pr = t.probes[op.probe];
    if (r.type != '$') ++t.errors;
    pr.seen = now <= pr.due_by && r.type == '$' && r.num >= pr.want;
    if (op.measured) t.kind[kProbe].add(now - op.start);
    probe_done(t, pr);
    return;
  }
  Applied ap = kInDoubt;
  bool served = true;
  switch (op.kind) {
    case kRead:
      ap = kNotApplied;
      if (t.scan) {
        if (r.type == '$') (*t.scan)[op.from] = r.num;
        return;
      }
      if (r.type != '$') ++t.errors, served = false;
      break;
    case kCreate:
    case kDeposit:
      if (r.type == (op.kind == kCreate ? '+' : ':')) ap = kApplied;
      else ++t.errors, served = false;
      break;
    case kTransfer:
      if (r.type == '+') {
        ap = kApplied;
      } else if (r.type == '-' && r.text == "ERR NOFUNDS") {
        ap = kNotApplied;
        t.rejected += op.measured;
      } else {
        ++t.errors;
        served = false;
      }
      break;
    default:
      break;
  }
  if (op.probe != kNoProbe) {
    Probe& pr = t.probes[op.probe];
    if (ap == kNotApplied) --t.probe_expected[pr.p], pr.rejected = true;
    pr.acked = ap == kApplied && now <= pr.due_by;
    probe_done(t, pr);
  }
  const Args& a = t.sh->a;
  if (!a.ack_log.empty() && op.kind != kRead) {
    t.acks.push_back({op.from, op.to, op.kind == kCreate ? 0 : op.start, now,
                      op.kind == kCreate ? 'c' : "and"[ap]});
  }
  if (!t.delta.empty() && op.kind != kRead && op.kind != kCreate && ap != kNotApplied) {
    if (ap == kApplied) {
      if (op.kind == kDeposit) ++t.delta[op.from];
      else --t.delta[op.from], ++t.delta[op.to];
    } else {
      if (op.kind == kDeposit) ++t.doubt_up[op.from];
      else ++t.doubt_down[op.from], ++t.doubt_up[op.to];
    }
  }
  if (op.kind == kCreate || !op.measured) return;
  if (served) ++t.ops;
  const u64 lat = now - op.start;
  t.all.add(lat);
  t.kind[op.kind].add(lat);
  if (served && now < t.sh->end) ++t.in_window;
}

// Sends what is buffered and handles every complete reply; waits up to
// wait_ms for the first. False on a connection error.
bool poll_conns(Thread& t, int wait_ms) {
  std::vector<pollfd>& pf = t.pf;
  pf.resize(t.conns.size());
  for (std::size_t i = 0; i < t.conns.size(); ++i) {
    pf[i] = {t.conns[i].fd, static_cast<short>(POLLIN | (t.conns[i].out.empty() ? 0 : POLLOUT)), 0};
  }
  if (::poll(pf.data(), pf.size(), wait_ms) < 0) return errno == EINTR;
  char buf[1 << 16];
  for (std::size_t i = 0; i < t.conns.size(); ++i) {
    Conn& c = t.conns[i];
    if (pf[i].revents & POLLOUT) {
      ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return t.error = "send failed", false;
      if (n > 0) c.out.erase(0, static_cast<std::size_t>(n));
    }
    if (pf[i].revents & (POLLIN | POLLERR | POLLHUP)) {
      ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) return t.error = "connection closed", false;
      if (n > 0) c.in.append(buf, static_cast<std::size_t>(n));
      const u64 now = now_ns();
      Reply r;
      while (!c.pending.empty() && parse(c, r)) {
        Op op = c.pending.front();
        c.pending.pop_front();
        on_reply(t, op, r, now);
      }
      c.in.erase(0, c.in_pos);
      c.in_pos = 0;
    }
  }
  return true;
}

bool load(Thread& t) {
  const Args& a = t.sh->a;
  for (u64 n = t.index; n < a.accounts() || t.outstanding;) {
    while (n < a.accounts() && t.outstanding < a.depth) {
      send_op(t, {0, n, kNone, kCreate, false});
      n += a.threads;
    }
    if (!poll_conns(t, 10)) return false;
  }
  return t.errors == 0;
}

void issue(Thread& t, u64 start, bool measured) {
  const Args& a = t.sh->a;
  Op op{start, t.rng() % a.objects, kNone, kRead, measured};
  if (a.transfer_pct && static_cast<int>(t.rng() % 100) < a.transfer_pct) {
    op.kind = kTransfer;
    op.to = t.rng() % a.objects;
    if (op.to == op.from) op.to = (op.to + 1) % a.objects;
    if (a.probe_pct > 0 && static_cast<double>(t.rng() % 1000000) < a.probe_pct * 10000) {
      if (t.probe_free.empty()) {
        t.probe_skipped += measured;
      } else {
        const u64 p = t.probe_free.back();
        t.probe_free.pop_back();
        op.probe = static_cast<u32>(t.probes.size());
        op.to = a.objects + p;
        t.probes.push_back({start + a.probe_slo_ns, start + a.probe_slo_ns + a.probe_grace_ns, p,
                            ++t.probe_expected[p], measured});
      }
    }
  } else if (static_cast<int>(t.rng() % 100) >= a.reads_pct) {
    op.kind = kDeposit;
  }
  send_op(t, op);
}

void run_rate(Thread& t) {
  const Shared& sh = *t.sh;
  const u64 total = t.sched.due_before(sh.end - sh.t0);
  bool past_end = false;
  while (true) {
    const u64 now = now_ns();
    if (!past_end && now >= sh.end) {
      past_end = true;
      t.scheduled_in_window = total - t.sched.due_before(sh.measure - sh.t0);
      t.issued_by_end = t.next_k;
      t.outstanding_at_end = t.outstanding;
    }
    const u64 due = past_end ? total : now <= sh.t0 ? 0 : t.sched.due_before(now - sh.t0);
    while (t.probe_head < t.probes.size() && t.probes[t.probe_head].read_at <= now && t.outstanding < sh.a.depth) {
      const Probe& pr = t.probes[t.probe_head];
      t.probe_reads += pr.measured;
      send_op(t, {pr.read_at, sh.a.objects + pr.p, kNone, kProbe, pr.measured, static_cast<u32>(t.probe_head++)});
    }
    while (t.next_k < due && t.outstanding < sh.a.depth) {
      const u64 intended = sh.t0 + t.sched.at(t.next_k);
      t.lateness.add(now - intended);
      t.late_1ms += now - intended > 1000000;
      issue(t, intended, intended >= sh.measure);
      ++t.next_k;
    }
    if (past_end && t.next_k == total && t.probe_head == t.probes.size() && t.outstanding == 0) {
      t.drained = true;
      return;
    }
    if (now >= sh.deadline) return;
    int wait_ms = 1;
    if (t.next_k < total && t.outstanding < sh.a.depth && sh.t0 + t.sched.at(t.next_k) <= now + 1500000) wait_ms = 0;
    if (t.probe_head < t.probes.size() && t.probes[t.probe_head].read_at <= now + 1500000) wait_ms = 0;
    if (!poll_conns(t, wait_ms)) return;
  }
}

void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Reads every account until two scans agree, and checks each against its
// start plus acknowledged changes, within the ops in doubt.
bool verify(std::vector<Thread>& threads, const Args& a, bool undrained) {
  const u64 n = a.accounts();
  std::vector<i64> want(n, a.start_balance);
  std::vector<u64> down(n, 0), up(n, 0);
  u64 doubt = 0;
  for (auto& t : threads) {
    for (u64 j = 0; j < n; ++j) {
      want[j] += t.delta[j];
      down[j] += t.doubt_down[j];
      up[j] += t.doubt_up[j];
    }
  }
  i64 sum = 0, expected = 0;
  u64 bad = 0, scans = 0;
  Thread& r = threads[0];
  std::vector<i64> got, prev;
  for (; scans < 50; ++scans) {
    got.assign(n, INT64_MIN);
    r.scan = &got;
    for (u64 j = 0; j < n || r.outstanding;) {
      for (; j < n && r.outstanding < a.depth; ++j) send_op(r, {0, j, kNone, kRead, false});
      if (!poll_conns(r, 10)) return false;
    }
    if (got == prev) break;
    prev = got;
  }
  r.scan = nullptr;
  for (u64 j = 0; j < n; ++j) {
    doubt += down[j] + up[j] > 0;
    expected += want[j];
    if (got[j] == INT64_MIN) {
      ++bad;
      continue;
    }
    sum += got[j];
    bad += got[j] < want[j] - static_cast<i64>(down[j]) || got[j] > want[j] + static_cast<i64>(up[j]);
  }
  const bool ok = !bad && !undrained;
  std::printf("verify: %llu accounts, %llu mismatches  scans %llu  in_doubt %llu  sum %lld expected %lld: %s\n",
              (unsigned long long)n, (unsigned long long)bad, (unsigned long long)scans + 1,
              (unsigned long long)doubt, (long long)sum, (long long)expected, ok ? "ok" : "FAILED");
  return ok;
}

bool write_acks(const std::vector<Thread>& threads, const Args& a) {
  FILE* f = std::fopen(a.ack_log.c_str(), "w");
  if (!f) return false;
  auto put = [&](u64 from, u64 to, u64 sent, u64 acked, char k) {
    std::fprintf(f, "%c %016llx%016llx %016llx%016llx %llu %llu\n", k, 1ull, (unsigned long long)from + 1,
                 to == kNone ? 0ull : 1ull, to == kNone ? 0ull : (unsigned long long)to + 1,
                 (unsigned long long)sent, (unsigned long long)acked);
  };
  for (const auto& t : threads) {
    for (const auto& x : t.acks) put(x.from, x.to, x.sent, x.acked, x.kind);
    for (const auto& c : t.conns) {
      for (const auto& op : c.pending) {
        if (op.kind == kDeposit || op.kind == kTransfer) put(op.from, op.to, op.start, 0, 'd');
      }
    }
  }
  return std::fclose(f) == 0;
}

void print_hist(const char* name, const Histogram& h) {
  std::printf("  %s latency ns  n %llu  p50 %.0f  p95 %.0f  p99 %.0f  p99.9 %.0f  max %llu\n", name,
              (unsigned long long)h.count(), h.percentile(50), h.percentile(95), h.percentile(99),
              h.percentile(99.9), (unsigned long long)h.max());
}

[[noreturn]] void bad(const char* why) {
  std::fprintf(stderr, "resp_bench: %s\n", why);
  std::exit(2);
}

Args parse_args(int argc, char** argv) {
  Args a;
  bool grace = false;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) bad("missing value");
      return argv[++i];
    };
    if (k == "--host") a.host = val();
    else if (k == "--port") a.port = static_cast<u16>(std::atoi(val()));
    else if (k == "--threads") a.threads = static_cast<u16>(std::atoi(val()));
    else if (k == "--conns") a.conns = static_cast<u16>(std::atoi(val()));
    else if (k == "--cpus") {
      for (const char* p = val(); *p;) {
        a.cpus.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
      }
    } else if (k == "--objects") a.objects = std::strtoull(val(), nullptr, 10);
    else if (k == "--depth") a.depth = static_cast<u32>(std::atoi(val()));
    else if (k == "--rate") a.rate = std::strtoull(val(), nullptr, 10);
    else if (k == "--warmup") a.warmup = std::atof(val());
    else if (k == "--seconds") a.seconds = std::atof(val());
    else if (k == "--drain-s") a.drain_s = std::atof(val());
    else if (k == "--transfer") a.transfer_pct = std::atoi(val());
    else if (k == "--reads") a.reads_pct = std::atoi(val());
    else if (k == "--start-balance") a.start_balance = std::strtoll(val(), nullptr, 10);
    else if (k == "--verify") a.verify = true;
    else if (k == "--no-load") a.load = false;
    else if (k == "--ack-log") a.ack_log = val();
    else if (k == "--probe-pct") a.probe_pct = std::atof(val());
    else if (k == "--probe-slo-us") a.probe_slo_ns = std::strtoull(val(), nullptr, 10) * 1000;
    else if (k == "--probe-grace-us") a.probe_grace_ns = std::strtoull(val(), nullptr, 10) * 1000, grace = true;
    else if (k == "--probe-accounts") a.probe_accounts = std::strtoull(val(), nullptr, 10);
    else bad(("unknown flag " + k).c_str());
  }
  if (a.probe_pct < 0 || a.probe_pct > 100) bad("--probe-pct must be 0..100");
  if (a.probe_pct > 0) {
    if (!a.probe_slo_ns) bad("--probe-pct needs --probe-slo-us");
    if (!grace) a.probe_grace_ns = a.probe_slo_ns / 10;
    if (a.probe_slo_ns + a.probe_grace_ns >= a.drain_s * 1e9) bad("--probe-slo-us plus grace must be under --drain-s");
    if (a.probe_accounts < a.threads) bad("--probe-accounts must be at least --threads");
  }
  if (!a.rate) bad("--rate is required");
  if (!a.threads || !a.conns || !a.depth || a.objects < 2) bad("threads, conns, depth must be >= 1 and objects >= 2");
  if (a.start_balance < 0) bad("--start-balance must be >= 0");
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  Shared sh;
  sh.a = parse_args(argc, argv);
  const Args& a = sh.a;
  std::vector<Thread> threads(a.threads);
  std::barrier<> loaded(a.threads + 1), ready(a.threads + 1);
  std::atomic<bool> failed{false};
  std::vector<std::thread> ts;
  for (u16 i = 0; i < a.threads; ++i) {
    Thread& t = threads[i];
    t.index = i;
    t.sh = &sh;
    t.rng.seed(4321 + i);
    t.sched = RateSchedule(a.rate, a.threads, i);
    if (a.verify) {
      t.delta.assign(a.accounts(), 0);
      t.doubt_down.assign(a.accounts(), 0);
      t.doubt_up.assign(a.accounts(), 0);
    }
    if (a.probe_pct > 0) {
      for (u64 p = i; p < a.probe_accounts; p += a.threads) t.probe_free.push_back(p);
      t.probe_expected.assign(a.probe_accounts, a.start_balance);
      t.probes.reserve(1 << 16);
    }
    ts.emplace_back([&, i] {
      Thread& t = threads[i];
      if (!a.cpus.empty()) pin(a.cpus[i % a.cpus.size()]);
      bool ok = connect_all(t) && (!a.load || load(t));
      if (!ok) failed = true;
      loaded.arrive_and_wait();
      ready.arrive_and_wait();
      if (!failed) run_rate(t);
    });
  }
  auto t0 = std::chrono::steady_clock::now();
  loaded.arrive_and_wait();
  const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  auto ns = [](double sec) { return static_cast<u64>(sec * 1e9); };
  sh.t0 = now_ns() + 5000000;
  sh.measure = sh.t0 + ns(a.warmup);
  sh.end = sh.measure + ns(a.seconds);
  sh.deadline = sh.end + ns(a.drain_s);
  ready.arrive_and_wait();
  for (auto& th : ts) th.join();
  if (!a.ack_log.empty() && !write_acks(threads, a)) return 1;
  for (auto& t : threads) {
    if (!t.error.empty()) {
      std::fprintf(stderr, "thread %u: %s\n", t.index, t.error.c_str());
      return 1;
    }
  }
  if (failed) {
    std::fprintf(stderr, "load failed\n");
    return 1;
  }
  Histogram all, kinds[kKinds], lateness;
  u64 ops = 0, errors = 0, rejected = 0, in_window = 0, sched = 0, issued = 0, at_end = 0, late = 0;
  bool drained = true;
  for (auto& t : threads) {
    all.merge(t.all);
    lateness.merge(t.lateness);
    for (int k = 0; k < kKinds; ++k) kinds[k].merge(t.kind[k]);
    ops += t.ops;
    errors += t.errors;
    rejected += t.rejected;
    in_window += t.in_window;
    sched += t.scheduled_in_window;
    issued += t.issued_by_end;
    at_end += t.outstanding_at_end;
    late += t.late_1ms;
    drained &= t.drained;
  }
  std::printf("resp_bench threads=%u conns=%u depth=%u/thread\n", a.threads, a.conns, a.depth);
  std::printf("load: %llu objects in %.2fs\n", (unsigned long long)(a.load ? a.accounts() : 0), load_s);
  std::printf("run: transfer=%d%% reads=%d%% of the rest  seconds=%.2f\n", a.transfer_pct, a.reads_pct, a.seconds);
  std::printf("  ops/s %.0f  errors %llu\n", ops / a.seconds, (unsigned long long)errors);
  std::printf("  rejected %llu (%.4f%% of ops)\n", (unsigned long long)rejected, ops ? 100.0 * rejected / ops : 0.0);
  print_hist("total", all);
  for (int k = 0; k < kCreate; ++k) print_hist(kKindName[k], kinds[k]);
  if (a.probe_pct > 0) print_hist(kKindName[kProbe], kinds[kProbe]);
  std::printf("rate: offered %llu ops/s  warmup %.2fs  window %.2fs  drain limit %.2fs\n", (unsigned long long)a.rate,
              a.warmup, a.seconds, a.drain_s);
  std::printf("  scheduled_in_window %llu  issued_by_end %llu  completed_in_window %llu  achieved %.0f ops/s\n",
              (unsigned long long)sched, (unsigned long long)issued, (unsigned long long)in_window,
              in_window / a.seconds);
  std::printf("  in_flight_at_end %llu  late_over_1ms %llu  lateness p99 %.0f ns\n", (unsigned long long)at_end,
              (unsigned long long)late, lateness.percentile(99));
  std::printf("  drained %s\n", drained ? "yes" : "no");
  if (a.probe_pct > 0) {
    u64 n = 0, miss = 0, skipped = 0, reads = 0;
    for (const auto& t : threads) {
      u64 open = 0;  // never settled before the drain limit: a miss
      for (const auto& r : t.probes) open += r.pending && r.measured && !r.rejected;
      n += open + t.probe_n;
      miss += open;
      miss += t.probe_miss;
      skipped += t.probe_skipped;
      reads += t.probe_reads;
    }
    std::printf("probe: pct %.4f  slo_us %llu  grace_us %llu  accounts %llu  probed %llu  misses %llu  "
                "miss_pct %.4f  skipped %llu  reads %llu\n",
                a.probe_pct, (unsigned long long)(a.probe_slo_ns / 1000), (unsigned long long)(a.probe_grace_ns / 1000),
                (unsigned long long)a.probe_accounts, (unsigned long long)n, (unsigned long long)miss,
                100.0 * miss / std::max<u64>(1, n), (unsigned long long)skipped, (unsigned long long)reads);
  }
  if (a.verify && !verify(threads, a, !drained)) return 1;
  return errors ? 1 : 0;
}
