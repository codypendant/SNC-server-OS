// snc_witness: decides when a standby is promoted (docs/REPLICATION.md,
// "Automatic failover"). It polls the primary's and the standby's admin
// ports. If the primary has not answered for --failover-ms and the standby
// is up, it promotes the standby, unless the primary has been let continue
// without that standby (then the standby may lack acknowledged writes).
// A sync primary started with --witness asks before continuing without
// its standby; a primary whose term was replaced is refused.
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
// reply that depends on it. One failover per configuration: afterwards the
// witnesses serve the new primary's requests and watch nothing until they
// are restarted with a new standby.
//
// Usage: snc_witness --port P --primary HOST:ADMIN --standby HOST:ADMIN
//                    --state FILE [--failover-ms N] [--poll-ms N]
//                    [--id I --peers HOST:PORT,HOST:PORT,...]
// --peers lists every witness, this one included at index --id.
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
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

using namespace snc;

namespace {

using u64 = unsigned long long;

// The decision state. Version n is the value chosen by the n-th Paxos
// instance; version 0 is the empty state every witness starts from.
struct State {
  u64 version = 0;
  std::string uuid;  // the space; empty until learned
  u64 term = 0;      // the current primary's term
  bool degraded = false;     // the primary may run without the standby
  bool failed_over = false;  // the standby was promoted

  std::string str() const {
    std::ostringstream o;
    o << version << ' ' << (uuid.empty() ? "-" : uuid) << ' ' << term << ' ' << degraded << ' '
      << failed_over;
    return o.str();
  }
  bool parse(std::istream& in) {
    if (!(in >> version >> uuid >> term >> degraded >> failed_over)) return false;
    if (uuid == "-") uuid.clear();
    return true;
  }
  bool same_decision(const State& o) const {
    return uuid == o.uuid && term == o.term && degraded == o.degraded &&
           failed_over == o.failed_over;
  }
};

struct Peer {
  std::string host;
  unsigned port;
};

// This witness: the chosen state, and its acceptor state for one instance,
// open_v (normally chosen.version + 1; later when this witness has not yet
// learned the instances before it). Guarded by mu; saved before any reply
// that depends on it.
std::mutex mu;
State chosen;
u64 open_v = 1;       // the instance promised and accepted refer to
u64 promised = 0;     // highest ballot promised in it
u64 acc_ballot = 0;   // ballot of the value accepted in it (0: none)
State acc_value;
std::string state_path;
std::vector<Peer> peers;
unsigned self_id = 0;
u64 ballot_counter = 0;  // under mu

void save() {
  std::string tmp = state_path + ".tmp";
  std::ostringstream o;
  o << "chosen " << chosen.str() << "\nopen " << open_v << "\npromised " << promised
    << "\naccepted " << acc_ballot
    << ' ' << acc_value.str() << "\n";
  std::string s = o.str();
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0 || ::write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size()) || ::fsync(fd) != 0 ||
      ::close(fd) != 0 || ::rename(tmp.c_str(), state_path.c_str()) != 0) {
    std::perror("snc_witness: cannot save state");
    std::exit(1);
  }
  std::string dir = state_path.rfind('/') == std::string::npos
                        ? "."
                        : state_path.substr(0, state_path.rfind('/'));
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
}

void load() {
  std::ifstream f(state_path);
  std::string k;
  while (f >> k) {
    if (k == "chosen") chosen.parse(f);
    else if (k == "open") f >> open_v;
    else if (k == "promised") f >> promised;
    else if (k == "accepted") {
      f >> acc_ballot;
      acc_value.parse(f);
    }
  }
}

// Moves the acceptor to instance v, forgetting an earlier one. Only for an
// instance after open_v, which is then decided. Caller holds mu.
void open_instance(u64 v) {
  if (v <= open_v) return;
  open_v = v;
  promised = 0;
  acc_ballot = 0;
  acc_value = State{};
}

// A newer chosen state closes the instances up to it. Caller holds mu.
void learn(const State& s) {
  if (s.version <= chosen.version) return;
  chosen = s;
  open_instance(s.version + 1);
}

// ---- Acceptor -------------------------------------------------------------------

// PREPARE v ballot <state v-1>  -> PROMISE acc_ballot <acc_value> | NACK promised | LATE <chosen>
// ACCEPT v ballot <state v>     -> ACCEPTED | NACK promised | LATE <chosen>
// COMMIT <state>                -> OK
// READ                          -> STATE <chosen>
std::string acceptor(const std::string& verb, std::istream& in) {
  std::lock_guard<std::mutex> g(mu);
  if (verb == "READ") return "STATE " + chosen.str();
  if (verb == "COMMIT") {
    State s;
    if (!s.parse(in)) return "NO bad request";
    if (s.version > chosen.version) {
      learn(s);
      save();
    }
    return "OK";
  }
  u64 v = 0, ballot = 0;
  in >> v >> ballot;
  if (verb == "PREPARE") {
    State base;
    if (!base.parse(in) || base.version + 1 != v) return "NO bad request";
    bool changed = false;
    if (base.version > chosen.version) {  // a proposer only builds on a chosen state
      learn(base);
      changed = true;
    }
    if (v <= chosen.version) return "LATE " + chosen.str();
    if (v < open_v) return "NO behind";  // a later instance is open here: v was decided
    open_instance(v);
    if (ballot <= promised) {
      if (changed) save();
      return "NACK " + std::to_string(promised);
    }
    promised = ballot;
    save();
    return "PROMISE " + std::to_string(acc_ballot) + " " + acc_value.str();
  }
  if (verb == "ACCEPT") {
    State s;
    if (!s.parse(in) || s.version != v) return "NO bad request";
    if (v <= chosen.version) return "LATE " + chosen.str();
    if (v < open_v) return "NO behind";
    // Instance v-1 was decided (someone proposes v), even if this witness
    // has not learned it yet.
    open_instance(v);
    if (ballot < promised) return "NACK " + std::to_string(promised);
    promised = ballot;
    acc_ballot = ballot;
    acc_value = s;
    save();
    return "ACCEPTED";
  }
  return "NO unknown request";
}

// ---- Proposer -------------------------------------------------------------------

struct Reply {
  bool ok = false;
  std::string line;
};

// Sends `line` to every witness at once and returns their replies.
std::vector<Reply> broadcast(const std::string& line, int timeout_ms) {
  std::vector<Reply> r(peers.size());
  std::vector<std::thread> ts;
  for (std::size_t i = 0; i < peers.size(); ++i) {
    ts.emplace_back([&, i] { r[i].ok = ha::ask(peers[i].host, peers[i].port, line, r[i].line, timeout_ms); });
  }
  for (auto& t : ts) t.join();
  return r;
}

std::size_t majority() { return peers.size() / 2 + 1; }

// The newest state a majority reports: chosen, since only chosen states
// are reported. Fails without a majority.
bool read_latest(State& out) {
  std::size_t n = 0;
  out = State{};
  for (const Reply& r : broadcast("READ", 300)) {
    if (!r.ok) continue;
    std::istringstream in(r.line);
    std::string tag;
    State s;
    if (!(in >> tag) || tag != "STATE" || !s.parse(in)) continue;
    ++n;
    if (s.version > out.version || n == 1) out = s;
  }
  if (n < majority()) return false;
  std::lock_guard<std::mutex> g(mu);
  if (out.version > chosen.version) {
    learn(out);
    save();
  }
  if (chosen.version > out.version) out = chosen;
  return true;
}

u64 next_ballot() {
  std::lock_guard<std::mutex> g(mu);
  // Wall-clock based, so a restarted witness does not reuse its old ballots.
  u64 now_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
  ballot_counter = std::max(ballot_counter + 1, now_ms);
  return (ballot_counter << 6) | self_id;
}

void saw_ballot(u64 b) {
  std::lock_guard<std::mutex> g(mu);
  ballot_counter = std::max(ballot_counter, b >> 6);
}

// Change the state with `change`, which returns "" and sets the wanted
// state, or returns why the change is refused. Retries while other
// proposals win. Returns "OK" once the wanted decision is chosen (or
// already holds), "NO <why>" otherwise.
template <class F>
std::string propose(F change, int deadline_ms = 1000) {
  static std::mutex one_at_a_time;  // proposals from this witness do not race each other
  std::lock_guard<std::mutex> serial(one_at_a_time);
  thread_local std::mt19937 rng(std::random_device{}());
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
  auto backoff = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(2 + rng() % 20)); };
  while (std::chrono::steady_clock::now() < end) {
    State base;
    if (!read_latest(base)) {
      backoff();
      continue;
    }
    State want = base;
    std::string why = change(base, want);
    if (!why.empty()) return "NO " + why;
    if (want.same_decision(base)) return "OK";
    want.version = base.version + 1;
    const u64 b = next_ballot();
    // Phase 1.
    std::size_t promises = 0;
    u64 best = 0;
    State value = want;
    bool late = false;
    for (const Reply& r :
         broadcast("PREPARE " + std::to_string(want.version) + " " + std::to_string(b) + " " +
                       base.str(), 300)) {
      if (!r.ok) continue;
      std::istringstream in(r.line);
      std::string tag;
      in >> tag;
      if (tag == "PROMISE") {
        u64 ab = 0;
        State av;
        in >> ab;
        av.parse(in);
        ++promises;
        if (ab > best) {
          best = ab;
          value = av;
        }
      } else if (tag == "NACK") {
        u64 p = 0;
        in >> p;
        saw_ballot(p);
      } else if (tag == "LATE") {
        late = true;
      }
    }
    if (late || promises < majority()) {
      backoff();
      continue;
    }
    // Phase 2: our value, or the one a majority may already have chosen.
    std::size_t accepted = 0;
    for (const Reply& r : broadcast("ACCEPT " + std::to_string(want.version) + " " +
                                        std::to_string(b) + " " + value.str(), 300)) {
      if (r.ok && r.line == "ACCEPTED") ++accepted;
      else if (r.ok && r.line.rfind("NACK ", 0) == 0) saw_ballot(std::strtoull(r.line.c_str() + 5, nullptr, 10));
    }
    if (accepted < majority()) {
      backoff();
      continue;
    }
    // Chosen. Tell everyone (best effort: a witness that misses it learns
    // it from the next proposal or read).
    broadcast("COMMIT " + value.str(), 300);
    {
      std::lock_guard<std::mutex> g(mu);
      if (value.version > chosen.version) {
        learn(value);
        save();
      }
    }
    if (value.same_decision(want)) return "OK";
    // Another proposal was chosen in this instance; try again on top of it.
  }
  return "NO no quorum";
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
void serve(int fd) {
  std::string line, reply = "NO bad request";
  if (ha::read_line(fd, line, 500)) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "DEGRADE" || verb == "SYNCED") {
      std::string uuid;
      u64 term = 0;
      in >> uuid >> term;
      const bool d = verb == "DEGRADE";
      reply = propose([&](const State& base, State& want) -> std::string {
        if (!base.uuid.empty() && uuid != base.uuid) return "another space";
        if (term != base.term) {
          return "term " + std::to_string(term) + " was replaced by " + std::to_string(base.term);
        }
        want.uuid = uuid;
        want.degraded = d;
        return "";
      });
      std::printf("t=%.2f %s: primary term %llu %s: %s\n",
                  secs(t0, std::chrono::steady_clock::now()), verb.c_str(), term,
                  d ? "continues without its standby" : "has its standby back in sync",
                  reply.c_str());
      std::fflush(stdout);
    } else {
      reply = acceptor(verb, in);
    }
  }
  ha::write_all(fd, reply + "\n");
  ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  unsigned port = 0, pport = 0, sport = 0;
  std::string phost, shost, peer_list;
  int failover_ms = 1000, poll_ms = 100;
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
    else if (k == "--state") state_path = v;
    else if (k == "--failover-ms") failover_ms = std::atoi(v.c_str());
    else if (k == "--poll-ms") poll_ms = std::atoi(v.c_str());
    else if (k == "--id") self_id = static_cast<unsigned>(std::atoi(v.c_str()));
    else if (k == "--peers") peer_list = v;
    else {
      std::fprintf(stderr, "bad flag %s %s\n", k.c_str(), v.c_str());
      return 2;
    }
  }
  if (port == 0 || pport == 0 || sport == 0 || state_path.empty()) {
    std::fprintf(stderr, "usage: snc_witness --port P --primary HOST:ADMIN --standby HOST:ADMIN "
                         "--state FILE [--failover-ms N] [--poll-ms N] [--id I --peers H:P,...]\n");
    return 2;
  }
  if (peer_list.empty()) {
    peers.push_back({"127.0.0.1", port});
    self_id = 0;
  } else {
    std::istringstream in(peer_list);
    std::string hp;
    while (std::getline(in, hp, ',')) {
      Peer p;
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
  }
  ::signal(SIGPIPE, SIG_IGN);
  load();
  int lfd = ha::listen_on(port);
  if (lfd < 0) {
    std::fprintf(stderr, "snc_witness: cannot listen on %u\n", port);
    return 1;
  }
  std::printf("snc_witness: port %u, %zu witness%s, state version %llu, term %llu%s%s\n", port,
              peers.size(), peers.size() == 1 ? "" : "es", chosen.version, chosen.term,
              chosen.degraded ? ", primary running without its standby" : "",
              chosen.failed_over ? ", already failed over" : "");
  std::fflush(stdout);

  // Requests: Paxos messages from witnesses, and the primary's.
  std::thread([lfd] {
    while (true) {
      if (!ha::wait_fd(lfd, POLLIN, 1000)) continue;
      int fd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) std::thread(serve, fd).detach();
    }
  }).detach();

  using clock = std::chrono::steady_clock;
  auto primary_ok = t0;  // last time the primary answered
  bool primary_seen = false, promoted_seen = false, decided_here = false;
  auto t_decided = t0;
  while (true) {
    State st;
    {
      std::lock_guard<std::mutex> g(mu);
      st = chosen;
    }
    // Promoting: ask often until the standby serves.
    std::this_thread::sleep_for(std::chrono::milliseconds(st.failed_over && !promoted_seen ? 2 : poll_ms));
    {
      std::lock_guard<std::mutex> g(mu);
      st = chosen;
    }
    std::string reply;
    const auto now = clock::now();
    if (st.failed_over) {
      // Decided: never undone, since a lost reply may hide a promotion that
      // happened. Ask until the standby serves as primary.
      if (promoted_seen) continue;
      if (ha::ask(shost, sport, "STATUS", reply, poll_ms)) {
        auto f = fields(reply);
        if (f["role"] != "s") {
          promoted_seen = true;
          if (decided_here) {
            std::printf("t=%.2f failover: primary silent for %.2fs, standby promoted to term %llu "
                        "in %.3fs\n", secs(t0, clock::now()), secs(primary_ok, t_decided), st.term,
                        secs(t_decided, clock::now()));
          } else {
            std::printf("t=%.2f failover learned: standby serves as primary, term %llu\n",
                        secs(t0, clock::now()), st.term);
          }
          std::fflush(stdout);
          continue;
        }
      }
      ha::ask(shost, sport, "PROMOTE", reply, 1000);
      continue;
    }

    if (ha::ask(phost, pport, "STATUS", reply, poll_ms)) {
      auto f = fields(reply);
      if (f["role"] == "p" || f["role"] == "n") {
        primary_ok = now;
        if (!primary_seen) {
          const std::string uuid = f["uuid"];
          const u64 term = std::strtoull(f["term"].c_str(), nullptr, 10);
          std::string r = propose([&](const State& base, State& want) -> std::string {
            if (!base.uuid.empty()) return "";  // already learned
            want.uuid = uuid;
            want.term = term;
            return "";
          });
          if (r == "OK") {
            primary_seen = true;
            std::printf("t=%.2f primary up: term %s\n", secs(t0, now), f["term"].c_str());
            std::fflush(stdout);
          }
        }
      }
    }
    if (!primary_seen || secs(primary_ok, now) * 1000 < failover_ms) continue;
    // The primary is gone. Promote the standby if it has every acked write.
    if (st.degraded) continue;  // it may not: wait for the primary
    if (!ha::ask(shost, sport, "STATUS", reply, poll_ms)) continue;
    auto f = fields(reply);
    const std::string suuid = f["uuid"];
    const u64 sterm = std::strtoull(f["term"].c_str(), nullptr, 10);
    if (f["role"] != "s") continue;
    const auto tp = clock::now();
    bool ours = false;  // this witness's proposal made the change
    std::string r = propose([&](const State& base, State& want) -> std::string {
      ours = !base.failed_over;
      if (base.failed_over) return "";
      if (base.degraded) return "the primary runs without its standby";
      if (suuid != base.uuid || sterm != base.term) return "the standby is not the primary's";
      want.term = base.term + 1;  // promotion bumps the standby's term by one
      want.failed_over = true;
      return "";
    });
    if (r == "OK" && ours) {
      decided_here = true;
      t_decided = tp;
    }
  }
}
