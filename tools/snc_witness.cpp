// snc_witness: decides when a standby is promoted (docs/REPLICATION.md,
// "Automatic failover"). It polls two nodes' admin ports: --primary and
// --standby name them as they start; the roles then follow the promotions.
// If the current primary has not answered for --failover-ms and the other
// node is its standby (same space and term), it promotes that standby,
// only from READY: the primary of the current term has proven with READY
// that its standby holds every acknowledged write (docs/SYNC_READINESS.md).
// A primary first learned, or just promoted, is NOT READY until then, and
// so is one let continue without its standby (DEGRADE, NOTREADY).
// Causal order (docs/SYNC_READINESS.md 7): every NOT READY is chosen at a
// fresh version, which NOTREADY answers ("OK v=N"); READY names the
// version its proof follows ("after=N") and is chosen only on that very
// state, so a READY delayed past a later NOT READY is refused. SYNCED
// (older primaries) names none and is refused. A sync primary
// started with --witness asks before continuing without its standby; a
// primary whose term was replaced is refused.
//
// Re-armed after each failover: the old primary, restarted as a standby of
// the new one (seeded from it), is promoted in turn if the new primary
// fails once it has proven the standby back in sync. Until then the
// witnesses do not promote: the group is unavailable rather than wrong.
//
// Replicated (--peers): N witnesses (3 or 5) keep one decision state, and
// every change to it (learning the primary, a grant to run alone, the
// standby back in sync, a promotion) is chosen by single-decree Paxos among
// them, one Paxos instance per state version. A change needs a majority, so
// a grant and a promotion can never both be chosen against the same state,
// and any minority of witnesses may be down. Any witness serves the
// primary's requests and may propose the promotion; whichever witnesses
// learn it promote the standby (PROMOTE is idempotent).
//
// Each witness records its state in --state, written and synced before any
// reply that depends on it.
//
// The same witnesses also hold the cluster's placement map
// (docs/CONTROL_PLANE.md): a second, independent state machine on the same
// Paxos library (paxos.h), written once by `snc_ctl init` and read by
// nodes at start (PLACEMENT INIT / GET). A failover never touches it.
// Without --primary and --standby a witness serves placement only.
//
// Several groups (docs/CONTROL_PLANE.md section 10): --group
// G=PRIMARY:ADMIN,STANDBY:ADMIN, repeated, watches each group's pair on its
// own machine, "ha/G", with its own versions and terms (--primary and
// --standby are group 0). A failover in one group never changes another
// group's machine, nor placement. A primary names its group in DEGRADE and
// SYNCED (else the group whose space it is). The same witnesses also hand
// out cluster-wide mint lease blocks (the `leases` machine).
//
// Liveness (docs/HEALTH_DECOUPLING.md): the witness polls each node with
// HEALTH, which a node answers without waiting on its shard threads, so a
// long shard task no longer reads as silence. A node that does not know
// HEALTH (any reply not starting "role=") is polled with STATUS, and HEALTH
// is tried again every 10 s. --health-verb status polls STATUS only (the
// behavior before HEALTH; rollback). The protection against false
// failovers holds only when EVERY witness of a group polls HEALTH: each
// witness decides alone that a primary is silent and proposes the
// promotion. Shard stall reported by HEALTH (stall_ms) is diagnostic and
// drives no decision here (review of PR #36, option B): a live process
// whose shard is stuck is not failed over. Promotion eligibility is
// unchanged: the standby is still checked with STATUS before a promotion
// is proposed, so it must still answer from every shard within --poll-ms.
//
// Usage: snc_witness --port P [--primary HOST:ADMIN --standby HOST:ADMIN]
//                    [--group G=HOST:ADMIN,HOST:ADMIN ...]
//                    --state FILE [--failover-ms N] [--poll-ms N]
//                    [--health-verb health|status]
//                    [--id I --peers HOST:PORT,HOST:PORT,...] [--new-state 1]
//                    [--debug 1]
// --peers lists every witness, this one included at index --id.
// --new-state 1: this witness's first start (the state file must not
// exist); without it the state file must exist (docs/PAXOS_BALLOTS.md).
// --debug 1 (tests only) adds DEBUG requests; --debug-clock-offset-ms N
// (with it) shifts the clock ballots are taken from (docs/PAXOS_BALLOTS.md).
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ha.h"
#include "snc/trace.h"
#include "paxos.h"

using namespace snc;

namespace {

using u64 = unsigned long long;

// The decision state. Version n is the value chosen by the n-th Paxos
// instance; version 0 is the empty state every witness starts from.
struct State {
  u64 version = 0;
  std::string uuid;  // the space; empty until learned
  u64 term = 0;      // the current primary's term
  // NOT READY (docs/SYNC_READINESS.md): the primary may run without the
  // standby, or has not yet proven it in sync (first learned, or just
  // promoted). The witnesses promote only from false (READY), which only a
  // READY of the current term, naming the NOT READY it follows, records
  // (docs/SYNC_READINESS.md 7).
  bool degraded = false;
  unsigned primary = 0;   // which node (0: --primary, 1: --standby) holds `term`

  std::string str() const {
    std::ostringstream o;
    o << version << ' ' << (uuid.empty() ? "-" : uuid) << ' ' << term << ' ' << degraded << ' '
      << primary;
    return o.str();
  }
  bool parse(std::istream& in) {
    if (!(in >> version >> uuid >> term >> degraded >> primary) || primary > 1) return false;
    if (uuid == "-") uuid.clear();
    return true;
  }
  bool same_decision(const State& o) const {
    return uuid == o.uuid && term == o.term && degraded == o.degraded && primary == o.primary;
  }
};

// This process's member of the witnesses' consensus (tools/paxos.h). Two
// independent machines run on it: "ha/0", the pair's decision state
// (State above), and "placement", the cluster's placement map
// (docs/CONTROL_PLANE.md), which the witnesses store but do not interpret.
paxos::Node px;
// One HA machine per replication group watched: "ha/<group>".
std::string ha_machine(unsigned group) { return "ha/" + std::to_string(group); }
const std::string kPlacement = "placement";

State to_state(const paxos::Value& v) {
  State s;
  if (!v.body.empty()) {
    std::istringstream in(std::to_string(v.version) + " " + v.body);
    s.parse(in);
  }
  s.version = v.version;
  return s;
}

std::string body_of(const State& s) {
  const std::string full = s.str();
  return full.substr(full.find(' ') + 1);
}

State chosen_state(unsigned group) { return to_state(px.chosen(ha_machine(group))); }

// Changes the pair's state with `change`, which returns "" and sets the
// wanted state, or returns why the change is refused. "OK" once the wanted
// decision is chosen (or already holds), "NO <why>" otherwise.
template <class F>
std::string propose(unsigned group, F change, u64* version = nullptr, bool fresh = false) {
  return px.propose(
      ha_machine(group),
      [&](const paxos::Value& base, std::string& want_body) -> std::string {
        const State b = to_state(base);
        State w = b;
        std::string why = change(b, w);
        if (why.empty()) want_body = body_of(w);
        return why;
      },
      [](const std::string& a, const std::string& b) {
        return to_state({1, a}).same_decision(to_state({1, b}));
      },
      // 3 s, as placement: with a witness that accepts connections but
      // does not answer (stopped), each round takes about 1.2 s, and a NOT
      // READY that adopts a value needs two (SYNC_READINESS 7.2).
      3000, version, fresh);
}

// Placement requests (snc_ctl, nodes):
//   PLACEMENT GET          -> MAP <body> | NONE | NO <why>   (a majority read: may be behind)
//   PLACEMENT SYNC         -> STATE <version> <body>         (linearizable, docs/MIGRATION.md)
//   PLACEMENT INIT <body>  -> OK | NO <why>   (only once; the same body again is OK)
//   PLACEMENT CAS V <body> -> OK | NO version <current> | NO <why>
//       the body becomes version V+1 if V is the latest (slot moves; the
//       body is opaque here, its rules are the mover's)
std::string placement(std::istream& in) {
  std::string what;
  in >> what;
  if (what == "GET") {
    paxos::Value v;
    if (!px.read_latest(kPlacement, v)) return "NO no quorum";
    return v.body.empty() ? "NONE" : "MAP " + v.body;
  }
  if (what == "SYNC") {
    paxos::Value v;
    if (!px.read_linear(kPlacement, v)) return "NO no quorum";
    return "STATE " + v.str();
  }
  if (what == "CAS") {
    u64 at = 0;
    std::string body;
    if (!(in >> at)) return "NO bad request";
    std::getline(in, body);
    const auto b = body.find_first_not_of(' ');
    body = b == std::string::npos ? "" : body.substr(b);
    if (body.empty() || at == 0) return "NO bad request";
    return px.propose(
        kPlacement,
        [&](const paxos::Value& base, std::string& want) -> std::string {
          if (base.version != at) return "version " + std::to_string(base.version);
          want = body;
          return "";
        },
        [](const std::string& a, const std::string& b) { return a == b; }, 3000);
  }
  if (what == "INIT") {
    std::string body;
    std::getline(in, body);
    const auto b = body.find_first_not_of(' ');
    body = b == std::string::npos ? "" : body.substr(b);
    if (body.empty()) return "NO bad request";
    return px.propose(
        kPlacement,
        [&](const paxos::Value& base, std::string& want) -> std::string {
          if (!base.body.empty() && base.body != body) return "already initialised";
          want = body;
          return "";
        },
        [](const std::string& a, const std::string& b) { return a == b; }, 3000);
  }
  return "NO bad request";
}

// Mint leases (docs/CONTROL_PLANE.md section 11), a third machine: its
// body is "next N", the first lease never handed out. Blocks start at
// 2^32, above any lease a space minted from its own counter before blocks.
//   LEASES TAKE COUNT  -> BLOCK LO HI | NO <why>
const std::string kLeases = "leases";
constexpr u64 kFirstBlockLease = u64{1} << 32;
constexpr u64 kLeaseLimit = (u64{1} << 56) - 1;

std::string leases(std::istream& in) {
  std::string what;
  u64 count = 0;
  if (!(in >> what >> count) || what != "TAKE" || count == 0 || count > (u64{1} << 32)) return "NO bad request";
  u64 lo = 0;
  thread_local std::mt19937_64 rng(std::random_device{}());
  const std::string tag = std::to_string(rng());
  const std::string r = px.propose(
      kLeases,
      [&](const paxos::Value& base, std::string& want) -> std::string {
        lo = kFirstBlockLease;
        if (!base.body.empty() && std::sscanf(base.body.c_str(), "next %llu", &lo) != 1) return "bad state";
        if (lo + count > kLeaseLimit) return "lease space exhausted";
        // Tagged, so two proposals of the same next value from the same
        // base never both count as chosen: each must win its own instance.
        want = "next " + std::to_string(lo + count) + " by " + tag;
        return "";
      },
      [](const std::string& a, const std::string& b) { return a == b; }, 3000);
  if (r != "OK") return r;
  return "BLOCK " + std::to_string(lo) + " " + std::to_string(lo + count);
}

std::map<std::string, std::string> fields(const std::string& line) {
  std::map<std::string, std::string> m;
  std::istringstream in(line);
  std::string kv;
  while (in >> kv) {
    auto eq = kv.find('=');
    if (eq != std::string::npos) m[kv.substr(0, eq)] = kv.substr(eq + 1);
  }
  return m;
}

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

const auto t0 = std::chrono::steady_clock::now();

// One request on an accepted connection.
std::vector<unsigned> watched_groups;  // set before any request is served

// --debug 1 (tests only, docs/SYNC_READINESS.md 7): DEBUG HOLD makes every
// readiness request (SYNCED, READY) that arrives wait, before its
// proposal, until DEBUG RELEASE: the deterministic form of a request
// delayed in this witness's proposal queue. DEBUG HELD reports how many
// wait now and how many have waited.
bool g_debug = false;
std::mutex hold_mu;
std::condition_variable hold_cv;
bool hold_on = false;
unsigned held_now = 0, held_total = 0;

void hold_if_asked() {
  if (!g_debug) return;
  std::unique_lock<std::mutex> g(hold_mu);
  if (!hold_on) return;
  ++held_now;
  ++held_total;
  hold_cv.wait(g, [] { return !hold_on; });
  --held_now;
}

// The IPv4 address a peer's host names (the witnesses speak IPv4).
bool resolve_ipv4(const std::string& host, in_addr& out) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
  out = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
  ::freeaddrinfo(res);
  return true;
}
// Whether this machine has the address (a socket can be bound to it).
bool is_local_ipv4(const in_addr& ip) {
  int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr = ip;
  const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
  ::close(fd);
  return ok;
}

std::string debug_verb(std::istream& in) {
  if (!g_debug) return "NO unknown request";
  std::string what;
  in >> what;
  std::lock_guard<std::mutex> g(hold_mu);
  if (what == "HOLD") {
    hold_on = true;
    held_total = 0;
    return "OK";
  }
  if (what == "RELEASE") {
    hold_on = false;
    hold_cv.notify_all();
    return "OK released " + std::to_string(held_now);
  }
  if (what == "HELD") return "HELD " + std::to_string(held_now) + " " + std::to_string(held_total);
  if (what == "SKIP-COMMIT") {
    int n = 0;
    in >> n;
    px.debug_skip_commits.store(n);
    return "OK";
  }
  // F8 characterization (docs/PAXOS_BALLOTS.md).
  if (what == "CLOCK-OFFSET-MS") {
    long long ms = 0;
    if (!(in >> ms)) return "NO bad request";
    px.debug_clock_offset_ms.store(ms);
    return "OK";
  }
  if (what == "SET-COUNTER") {
    paxos::u64 c = 0;
    if (!(in >> c)) return "NO bad request";
    px.debug_set_counter(c);
    return "OK";
  }
  if (what == "FAIL-PERSIST") {
    // FAIL-PERSIST STEP [SKIP]: the save after SKIP more fails at STEP.
    static const char* steps[] = {"tmp-open", "tmp-write", "tmp-fsync", "rename", "dir-open", "dir-fsync"};
    std::string step;
    int skip = 0;
    if (!(in >> step)) return "NO bad request";
    in >> skip;
    for (int i = 0; i < 6; ++i) {
      if (step == steps[i]) {
        px.debug_fail_skip.store(skip);
        px.debug_fail_step.store(i + 1);
        return "OK";
      }
    }
    return "NO bad request";
  }
  if (what == "NEXT-BALLOT") return "BALLOT " + std::to_string(px.debug_next_ballot());
  if (what == "PROPOSE") {
    // PROPOSE debug/NAME BODY: choose BODY in a test machine (never a real
    // one); answers with the version and every ballot used on the wire.
    std::string m, body;
    if (!(in >> m >> body) || m.rfind("debug/", 0) != 0) return "NO bad request";
    paxos::u64 v = 0;
    px.debug_take_ballots();
    px.debug_record.store(true);
    std::string r = px.propose(
        m, [&](const paxos::Value&, std::string& want) { want = body; return std::string(); },
        [](const std::string& a, const std::string& b) { return a == b; }, 3000, &v);
    px.debug_record.store(false);
    std::string bs;
    for (paxos::u64 b : px.debug_take_ballots()) bs += (bs.empty() ? "" : ",") + std::to_string(b);
    if (r == "OK") r += " v=" + std::to_string(v);
    return r + " ballots=" + bs;
  }
  return "NO bad request";
}

void serve(int fd) {
  std::string line, reply = "NO bad request";
  if (ha::read_line(fd, line, 500)) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "DEGRADE" || verb == "NOTREADY" || verb == "SYNCED" || verb == "READY") {
      // VERB uuid term [group] [after=N] (docs/SYNC_READINESS.md 7).
      std::string uuid, tok;
      u64 term = 0, after = 0;
      long long g = -1;
      bool has_after = false, bad = false;
      // A whole unsigned number, or false.
      auto number = [](const std::string& t, u64& out) {
        if (t.empty() || !std::isdigit(static_cast<unsigned char>(t[0]))) return false;
        char* end = nullptr;
        errno = 0;
        out = std::strtoull(t.c_str(), &end, 10);
        return errno == 0 && end && *end == '\0';
      };
      if (!(in >> uuid >> tok) || !number(tok, term)) bad = true;
      while (!bad && in >> tok) {
        u64 n = 0;
        if (tok.rfind("after=", 0) == 0 && !has_after && number(tok.substr(6), n)) {
          has_after = true;
          after = n;
        } else if (g < 0 && number(tok, n) && n < (u64{1} << 31)) {
          g = static_cast<long long>(n);
        } else {
          bad = true;
        }
      }
      // The group: given (a clustered node says which), or the one whose
      // space this is, or group 0 (a lone pair).
      if (g < 0) {
        g = 0;
        for (unsigned w : watched_groups) {
          if (chosen_state(w).uuid == uuid) g = w;
        }
      }
      const unsigned group = static_cast<unsigned>(g);
      // NOT READY (DEGRADE, NOTREADY) always chooses a fresh version: any
      // READY computed from an older one can then never be chosen.
      const bool d = verb == "DEGRADE" || verb == "NOTREADY";
      if (bad || (verb == "READY" && !has_after)) {
        reply = "NO bad request";
      } else if (std::find(watched_groups.begin(), watched_groups.end(), group) == watched_groups.end()) {
        reply = "NO group " + std::to_string(group) + " is not watched here";
      } else {
        if (!d) hold_if_asked();
        u64 v = 0;
        paxos::Value latest;
        // The base must be the latest chosen value, not the newest this
        // witness or a majority has learned: a NOT READY accepted by a
        // majority but announced to nobody (a lost COMMIT) is finished
        // first, else READY could read an older READY and answer OK while
        // NOT READY is chosen. If that read fails, READY is not answered.
        if (verb == "READY" && !px.read_linear(ha_machine(group), latest)) {
          reply = "NO no quorum";
        } else {
          reply = propose(
              group,
              [&](const State& base, State& want) -> std::string {
                // Not before the primary is learned: learning records which
                // node is primary (and NOT READY); a grant or READY first
                // would leave that unset. The primary asks again.
                if (base.uuid.empty()) return "not learned yet";
                if (uuid != base.uuid) return "another space";
                if (term != base.term) {
                  return "term " + std::to_string(term) + " was replaced by " + std::to_string(base.term);
                }
                if (verb == "SYNCED") {
                  // It names no causal base, so a delayed one could restore
                  // READY over a later NOT READY (an older primary binary).
                  return "SYNCED has no causal base: READY uuid term [group] after=N is required";
                }
                if (verb == "READY" && base.version < after) {
                  // Not yet learned here (the linear read failed): ask again.
                  return "behind v=" + std::to_string(base.version);
                }
                if (verb == "READY" && base.degraded && base.version != after) {
                  // A NOT READY was chosen after the primary's base N: its
                  // proof does not cover what was acknowledged since.
                  return "not ready since v=" + std::to_string(base.version);
                }
                // READY on a READY state: nothing to choose (want == base).
                want.uuid = uuid;
                want.degraded = d;
                return "";
              },
              &v, d);
          if (reply == "OK" && verb != "DEGRADE") reply += " v=" + std::to_string(v);
        }
      }
      std::printf("t=%.2f group %u %s: primary term %llu %s: %s\n",
                  secs(t0, std::chrono::steady_clock::now()), group, verb.c_str(), term,
                  d ? (verb == "DEGRADE" ? "continues without its standby" : "NOT READY")
                    : (verb == "READY" ? "READY after=" + std::to_string(after) : "SYNCED (no causal base)").c_str(),
                  reply.c_str());
      std::fflush(stdout);
    } else if (verb == "DEBUG") {
      reply = debug_verb(in);
    } else if (verb == "PLACEMENT") {
      reply = placement(in);
    } else if (verb == "LEASES") {
      reply = leases(in);
    } else if (!px.handle(verb, in, reply)) {
      reply = "NO unknown request";
    }
  }
  ha::write_all(fd, reply + "\n");
  ::close(fd);
}

struct Pair {
  std::string host[2];  // node 0 started as primary, node 1 as its standby
  unsigned port[2];
};
void watch(unsigned group, Pair pair, int failover_ms, int poll_ms);
// Poll liveness with HEALTH (false: STATUS only, --health-verb status).
bool g_use_health = true;

}  // namespace

int main(int argc, char** argv) {
  unsigned port = 0, pport = 0, sport = 0;
  std::string phost, shost, peer_list, state_path;
  std::map<unsigned, Pair> pairs;
  unsigned self_id = 0;
  int failover_ms = 1000, poll_ms = 100;
  bool new_state = false;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", k.c_str());
      return 2;
    }
    std::string v = argv[++i];
    if (k == "--port") port = static_cast<unsigned>(std::atoi(v.c_str()));
    else if (k == "--primary" && ha::parse_hostport(v, phost, pport)) {}
    else if (k == "--standby" && ha::parse_hostport(v, shost, sport)) {}
    else if (k == "--group") {
      // G=PRIMARY_HOST:ADMIN,STANDBY_HOST:ADMIN
      const auto eq = v.find('='), comma = v.find(',');
      Pair pr;
      if (eq == std::string::npos || comma == std::string::npos || comma < eq ||
          !ha::parse_hostport(v.substr(eq + 1, comma - eq - 1), pr.host[0], pr.port[0]) ||
          !ha::parse_hostport(v.substr(comma + 1), pr.host[1], pr.port[1])) {
        std::fprintf(stderr, "--group takes G=PRIMARY_HOST:ADMIN,STANDBY_HOST:ADMIN\n");
        return 2;
      }
      pairs[static_cast<unsigned>(std::atoi(v.c_str()))] = pr;
    }
    else if (k == "--state") state_path = v;
    else if (k == "--failover-ms") failover_ms = std::atoi(v.c_str());
    else if (k == "--poll-ms") poll_ms = std::atoi(v.c_str());
    else if (k == "--id") self_id = static_cast<unsigned>(std::atoi(v.c_str()));
    else if (k == "--peers") peer_list = v;
    else if (k == "--health-verb" && (v == "health" || v == "status")) g_use_health = v == "health";
    else if (k == "--debug") g_debug = v == "1";
    else if (k == "--debug-clock-offset-ms") px.debug_clock_offset_ms.store(std::atoll(v.c_str()));
    else if (k == "--new-state" && (v == "0" || v == "1")) new_state = v == "1";
    else {
      std::fprintf(stderr, "bad flag %s %s\n", k.c_str(), v.c_str());
      return 2;
    }
  }
  if (port == 0 || (pport == 0) != (sport == 0) || state_path.empty() || (pport != 0 && pairs.count(0))) {
    std::fprintf(stderr, "usage: snc_witness --port P [--primary HOST:ADMIN --standby HOST:ADMIN] "
                         "[--group G=PHOST:ADMIN,SHOST:ADMIN ...] --state FILE [--failover-ms N] "
                         "[--health-verb health|status] "
                         "[--poll-ms N] [--id I --peers H:P,...] [--new-state 1]\n");
    return 2;
  }
  if (px.debug_clock_offset_ms.load() != 0 && !g_debug) {
    std::fprintf(stderr, "--debug-clock-offset-ms needs --debug 1 (tests only)\n");
    return 2;
  }
  if (pport != 0) pairs[0] = Pair{{phost, shost}, {pport, sport}};
  for (auto& [g, pr] : pairs) watched_groups.push_back(g);
  std::vector<paxos::Peer> peers;
  if (peer_list.empty()) {
    peers.push_back({"127.0.0.1", port});
    self_id = 0;
  } else {
    std::istringstream in(peer_list);
    std::string hp;
    while (std::getline(in, hp, ',')) {
      paxos::Peer p;
      if (!ha::parse_hostport(hp, p.host, p.port)) {
        std::fprintf(stderr, "bad peer %s\n", hp.c_str());
        return 2;
      }
      peers.push_back(p);
    }
    if (self_id >= peers.size() || peers[self_id].port != port || peers.size() > 63) {
      std::fprintf(stderr, "--id must index this witness (port %u) in --peers\n", port);
      return 2;
    }
    // One acceptor, one place in the quorum (docs/PAXOS_BALLOTS.md 9):
    // every entry names a distinct address and port, this witness's entry
    // is one of this machine's addresses, and no other entry reaches this
    // witness's port on this machine.
    std::vector<std::string> norm(peers.size());
    std::vector<bool> local(peers.size());
    for (std::size_t i = 0; i < peers.size(); ++i) {
      in_addr ip{};
      if (!resolve_ipv4(peers[i].host, ip)) {
        std::fprintf(stderr, "peer %s:%u: cannot resolve its host\n", peers[i].host.c_str(), peers[i].port);
        return 2;
      }
      char txt[INET_ADDRSTRLEN] = {};
      ::inet_ntop(AF_INET, &ip, txt, sizeof(txt));
      norm[i] = std::string(txt) + ":" + std::to_string(peers[i].port);
      local[i] = is_local_ipv4(ip);
      for (std::size_t j = 0; j < i; ++j) {
        if (norm[j] == norm[i]) {
          std::fprintf(stderr, "--peers names %s twice (entries %zu and %zu): one witness would count twice "
                               "toward a majority\n", norm[i].c_str(), j, i);
          return 2;
        }
      }
    }
    if (!local[self_id]) {
      std::fprintf(stderr, "--id %u names %s, which is not an address of this machine\n", self_id,
                   norm[self_id].c_str());
      return 2;
    }
    for (std::size_t i = 0; i < peers.size(); ++i) {
      if (i != self_id && local[i] && peers[i].port == port) {
        std::fprintf(stderr, "--peers entry %zu (%s) reaches this witness (port %u on this machine) too: "
                             "it would count twice toward a majority\n", i, norm[i].c_str(), port);
        return 2;
      }
    }
  }
  // The state file: created only on a witness's first start, never
  // recreated (docs/PAXOS_BALLOTS.md 6 and 9). A witness that answered
  // Paxos requests and lost its state must not come back as itself.
  {
    struct stat st{};
    const bool exists = ::stat(state_path.c_str(), &st) == 0;
    if (!exists && errno != ENOENT) {
      std::fprintf(stderr, "snc_witness: state file %s: %s\n", state_path.c_str(), std::strerror(errno));
      return 2;
    }
    if (exists && !S_ISREG(st.st_mode)) {
      std::fprintf(stderr, "snc_witness: state file %s is not a regular file\n", state_path.c_str());
      return 2;
    }
    if (exists && new_state) {
      std::fprintf(stderr, "snc_witness: --new-state 1 but %s exists: refusing to replace a witness's state "
                           "(a witness that lost its state must not rejoin as itself)\n", state_path.c_str());
      return 2;
    }
    if (!exists && !new_state) {
      std::fprintf(stderr, "snc_witness: state file %s does not exist. A witness's first start says so with "
                           "--new-state 1; a witness that ran before and lost its state must not be restarted "
                           "as itself (docs/PAXOS_BALLOTS.md)\n", state_path.c_str());
      return 2;
    }
  }
  ::signal(SIGPIPE, SIG_IGN);
  px.init(state_path, peers, self_id, /*legacy=*/ha_machine(0));  // older state files hold one pair's
  if (new_state) px.create();
  int lfd = ha::listen_on(port);
  if (lfd < 0) {
    std::fprintf(stderr, "snc_witness: cannot listen on %u\n", port);
    return 1;
  }
  std::printf("snc_witness: port %u, %zu witness%s\n", port, peers.size(), peers.size() == 1 ? "" : "es");
  for (unsigned g : watched_groups) {
    const State chosen = chosen_state(g);
    std::printf("snc_witness: group %u: state version %llu, term %llu, primary node %u%s\n", g, chosen.version,
                chosen.term, chosen.primary, chosen.degraded ? ", running without its standby" : "");
  }
  if (pairs.empty()) std::printf("snc_witness: no pair to watch: placement and leases only\n");
  else std::printf("snc_witness: liveness polled with %s\n", g_use_health ? "HEALTH (STATUS for nodes without it)"
                                                                         : "STATUS (--health-verb status)");
  std::fflush(stdout);

  // Requests: Paxos messages from witnesses, and the primary's.
  std::thread([lfd] {
    while (true) {
      if (!ha::wait_fd(lfd, POLLIN, 1000)) continue;
      int fd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) std::thread(serve, fd).detach();
    }
  }).detach();
  // One watcher per group, each on its own machine: a failover in one
  // group never touches another's state, nor placement.
  for (auto& [g, pr] : pairs) {
    std::thread(watch, g, pr, failover_ms, poll_ms).detach();
  }
  while (true) std::this_thread::sleep_for(std::chrono::hours(1));
}

namespace {

// Watches one group's pair (its HA machine): learns the primary, and
// promotes the standby when the primary has been silent for failover_ms,
// only from READY (docs/SYNC_READINESS.md).
void watch(unsigned group, Pair pair, int failover_ms, int poll_ms) {
  using clock = std::chrono::steady_clock;
  const std::string* host = pair.host;
  const unsigned* aport = pair.port;
  // Asks one node `verb`, within poll_ms; false is silence.
  auto ask_node = [&](unsigned node, const char* verb, std::map<std::string, std::string>& f) {
    std::string reply;
    const u64 t_ask = trace::enabled() ? trace::now_ns() : 0;
    const bool ok = ha::ask(host[node], aport[node], verb, reply, poll_ms);
    if (trace::enabled()) {
      const double ms = (trace::now_ns() - t_ask) / 1e6;
      if (!ok || ms >= 20) {
        SNC_TRACE("witness_ask group=%u node=%u port=%u verb=%s ok=%d ms=%.1f reply=%.60s", group, node, aport[node],
                  verb, ok ? 1 : 0, ms, reply.c_str());
      }
    }
    if (!ok) return false;
    f = fields(reply);
    // Every answer this witness reads starts with role= (an old node
    // answers HEALTH "NO unknown request").
    if (reply.rfind("role=", 0) != 0) f["role"] = "";
    return true;
  };
  // Per node: polled with STATUS because it did not know HEALTH, and when
  // to try HEALTH again (an upgraded node is picked up without a restart).
  bool fallback[2] = {false, false};
  clock::time_point retry_health[2] = {t0, t0};
  // Liveness: HEALTH, or STATUS for a node that does not know HEALTH.
  auto status = [&](unsigned node, std::map<std::string, std::string>& f) {
    const auto now = clock::now();
    if (!g_use_health || (fallback[node] && now < retry_health[node])) return ask_node(node, "STATUS", f);
    if (!ask_node(node, "HEALTH", f)) return false;  // silence: no fallback
    if (!f["role"].empty()) {
      if (fallback[node]) {
        fallback[node] = false;
        std::printf("t=%.2f group %u node %u answers HEALTH: polled with HEALTH again\n", secs(t0, now), group, node);
        std::fflush(stdout);
      }
      return true;
    }
    if (!fallback[node]) {
      std::printf("t=%.2f group %u node %u does not know HEALTH: polled with STATUS (a busy shard there still "
                  "reads as silence)\n", secs(t0, now), group, node);
      std::fflush(stdout);
    }
    fallback[node] = true;
    retry_health[node] = now + std::chrono::seconds(10);
    return ask_node(node, "STATUS", f);
  };
  auto primary_ok = t0;  // last time the current primary answered as primary
  u64 watched = 0;       // the state version primary_ok refers to
  u64 announced = ~0ull;  // the term last reported serving (~0: none yet)
  auto t_decided = t0;
  double silent_s = 0;   // how long the old primary had been silent then
  u64 decided_term = ~0ull;  // the term of a promotion this witness's proposal made
  auto last_promote = t0 - std::chrono::seconds(1);  // when this witness last sent PROMOTE
  while (true) {
    const State st = chosen_state(group);
    const auto now = clock::now();
    std::map<std::string, std::string> f;
    if (st.uuid.empty()) {
      // Learn the space from whichever node serves as primary.
      for (unsigned n = 0; n < 2; ++n) {
        if (!status(n, f) || (f["role"] != "p" && f["role"] != "n")) continue;
        const std::string uuid = f["uuid"];
        const u64 term = std::strtoull(f["term"].c_str(), nullptr, 10);
        if (propose(group, [&](const State& base, State& want) -> std::string {
              if (!base.uuid.empty()) return "";  // already learned
              want.uuid = uuid;
              want.term = term;
              want.primary = n;
              // NOT READY until the primary proves its standby in sync
              // (READY; docs/SYNC_READINESS.md 2, 7).
              want.degraded = true;
              return "";
            }) == "OK") {
          std::printf("t=%.2f group %u primary up: term %llu\n", secs(t0, now), group, term);
          std::fflush(stdout);
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
      continue;
    }
    if (st.version != watched) {
      // A new decision: the primary it names gets a full timeout from now.
      watched = st.version;
      primary_ok = now;
    }
    const unsigned p = st.primary, o = 1 - p;
    bool promoting = false;
    if (status(p, f)) {
      const u64 term = std::strtoull(f["term"].c_str(), nullptr, 10);
      if ((f["role"] == "p" || f["role"] == "n") && term == st.term) {
        primary_ok = now;
        if (announced != st.term && st.term == decided_term) {
          std::printf("t=%.2f group %u failover: primary silent for %.2fs, standby promoted to term %llu "
                      "in %.3fs\n", secs(t0, now), group, silent_s, st.term,
                      secs(t_decided, now));
          std::fflush(stdout);
        } else if (announced != st.term && announced != ~0ull) {
          std::printf("t=%.2f group %u failover learned: node %u serves as primary, term %llu\n",
                      secs(t0, now), group, p, st.term);
          std::fflush(stdout);
        }
        announced = st.term;
      } else if (f["role"] == "s" && term + 1 == st.term && f["uuid"] == st.uuid) {
        // Decided: never undone, since a lost reply may hide a promotion
        // that happened. Ask until it serves as primary.
        // At most every 50 ms: HEALTH answers at once while the promotion
        // runs, so it no longer paces these requests as STATUS did.
        if (clock::now() - last_promote >= std::chrono::milliseconds(50)) {
          std::string reply;
          ha::ask(host[p], aport[p], "PROMOTE", reply, 1000);
          last_promote = clock::now();
        }
        promoting = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(promoting ? 2 : poll_ms));
    if (promoting || secs(primary_ok, clock::now()) * 1000 < failover_ms) continue;
    // The primary is gone. Promote its standby if it has every acked write.
    SNC_TRACE("witness_suspect group=%u primary=%u term=%llu silent_ms=%.1f degraded=%d", group, p,
              (unsigned long long)st.term, secs(primary_ok, clock::now()) * 1000, st.degraded ? 1 : 0);
    if (st.degraded) continue;  // it may not: wait for the primary
    // Eligibility: STATUS, as before HEALTH, never HEALTH. STATUS needs
    // every standby shard to run a task within poll_ms; HEALTH would drop
    // that requirement and so widen eligibility (a standby with a stuck
    // shard would be promoted, and Engine::promote would hang on it).
    if (!ask_node(o, "STATUS", f) || f["role"] != "s") continue;
    const std::string suuid = f["uuid"];
    const u64 sterm = std::strtoull(f["term"].c_str(), nullptr, 10);
    const auto tp = clock::now();
    bool ours = false;  // this witness's proposal made the change
    std::string r = propose(group, [&](const State& base, State& want) -> std::string {
      ours = false;
      if (base.version != st.version) return "";  // decided meanwhile; look again
      if (base.degraded) return "the primary runs without its standby";
      if (suuid != base.uuid || sterm != base.term) return "the standby is not the primary's";
      want.term = base.term + 1;  // promotion bumps the standby's term by one
      want.primary = o;
      // The new primary has no standby proven in sync: NOT READY until its
      // SYNCED (docs/SYNC_READINESS.md 2). Never copied from the old term.
      want.degraded = true;
      ours = true;
      return "";
    });
    SNC_TRACE("witness_propose_promote group=%u to=%u term=%llu result=%.40s ours=%d propose_ms=%.1f", group, o,
              (unsigned long long)(st.term + 1), r.c_str(), ours ? 1 : 0, secs(tp, clock::now()) * 1000);
    if (r == "OK" && ours) {
      silent_s = secs(primary_ok, tp);
      t_decided = tp;
      decided_term = st.term + 1;
    }
  }
}

}  // namespace
