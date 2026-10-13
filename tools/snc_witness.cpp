// snc_witness: decides when a standby is promoted (docs/REPLICATION.md,
// "Automatic failover"). It polls two nodes' admin ports: --primary and
// --standby name them as they start; the roles then follow the promotions.
// If the current primary has not answered for --failover-ms and the other
// node is its standby (same space and term), it promotes that standby,
// unless the primary has been let continue without it (then the standby
// may lack acknowledged writes). A sync primary started with --witness asks
// before continuing without its standby; a primary whose term was replaced
// is refused.
//
// Re-armed after each failover: the old primary, restarted as a standby of
// the new one (seeded from it), is promoted in turn if the new primary
// fails once it is back in sync. Until then the new primary runs alone
// under a grant, and the witnesses do not promote.
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
// Usage: snc_witness --port P [--primary HOST:ADMIN --standby HOST:ADMIN]
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
  bool degraded = false;  // the primary may run without the standby
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
const std::string kHa = "ha/0";
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

State chosen_state() { return to_state(px.chosen(kHa)); }

// Changes the pair's state with `change`, which returns "" and sets the
// wanted state, or returns why the change is refused. "OK" once the wanted
// decision is chosen (or already holds), "NO <why>" otherwise.
template <class F>
std::string propose(F change) {
  return px.propose(
      kHa,
      [&](const paxos::Value& base, std::string& want_body) -> std::string {
        const State b = to_state(base);
        State w = b;
        std::string why = change(b, w);
        if (why.empty()) want_body = body_of(w);
        return why;
      },
      [](const std::string& a, const std::string& b) {
        return to_state({1, a}).same_decision(to_state({1, b}));
      });
}

// Placement requests (snc_ctl, nodes):
//   PLACEMENT GET          -> MAP <body> | NONE | NO <why>
//   PLACEMENT INIT <body>  -> OK | NO <why>   (only once; the same body again is OK)
std::string placement(std::istream& in) {
  std::string what;
  in >> what;
  if (what == "GET") {
    paxos::Value v;
    if (!px.read_latest(kPlacement, v)) return "NO no quorum";
    return v.body.empty() ? "NONE" : "MAP " + v.body;
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
    } else if (verb == "PLACEMENT") {
      reply = placement(in);
    } else if (!px.handle(verb, in, reply)) {
      reply = "NO unknown request";
    }
  }
  ha::write_all(fd, reply + "\n");
  ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  unsigned port = 0, pport = 0, sport = 0;
  std::string phost, shost, peer_list, state_path;
  unsigned self_id = 0;
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
  if (port == 0 || (pport == 0) != (sport == 0) || state_path.empty()) {
    std::fprintf(stderr, "usage: snc_witness --port P [--primary HOST:ADMIN --standby HOST:ADMIN] "
                         "--state FILE [--failover-ms N] [--poll-ms N] [--id I --peers H:P,...]\n");
    return 2;
  }
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
  }
  ::signal(SIGPIPE, SIG_IGN);
  px.init(state_path, peers, self_id, /*legacy=*/kHa);  // older state files hold the pair's state
  const State chosen = chosen_state();
  int lfd = ha::listen_on(port);
  if (lfd < 0) {
    std::fprintf(stderr, "snc_witness: cannot listen on %u\n", port);
    return 1;
  }
  std::printf("snc_witness: port %u, %zu witness%s, state version %llu, term %llu, primary "
              "node %u%s\n", port, peers.size(), peers.size() == 1 ? "" : "es", chosen.version,
              chosen.term, chosen.primary,
              chosen.degraded ? ", running without its standby" : "");
  if (pport == 0) std::printf("snc_witness: no pair to watch: placement only\n");
  std::fflush(stdout);

  // Requests: Paxos messages from witnesses, and the primary's.
  std::thread([lfd] {
    while (true) {
      if (!ha::wait_fd(lfd, POLLIN, 1000)) continue;
      int fd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) std::thread(serve, fd).detach();
    }
  }).detach();
  if (pport == 0) {
    while (true) std::this_thread::sleep_for(std::chrono::hours(1));
  }

  using clock = std::chrono::steady_clock;
  const std::string host[2] = {phost, shost};
  const unsigned aport[2] = {pport, sport};
  auto status = [&](unsigned node, std::map<std::string, std::string>& f) {
    std::string reply;
    if (!ha::ask(host[node], aport[node], "STATUS", reply, poll_ms)) return false;
    f = fields(reply);
    return true;
  };
  auto primary_ok = t0;  // last time the current primary answered as primary
  u64 watched = 0;       // the state version primary_ok refers to
  u64 announced = ~0ull;  // the term last reported serving (~0: none yet)
  auto t_decided = t0;
  double silent_s = 0;   // how long the old primary had been silent then
  u64 decided_term = ~0ull;  // the term of a promotion this witness's proposal made
  while (true) {
    const State st = chosen_state();
    const auto now = clock::now();
    std::map<std::string, std::string> f;
    if (st.uuid.empty()) {
      // Learn the space from whichever node serves as primary.
      for (unsigned n = 0; n < 2; ++n) {
        if (!status(n, f) || (f["role"] != "p" && f["role"] != "n")) continue;
        const std::string uuid = f["uuid"];
        const u64 term = std::strtoull(f["term"].c_str(), nullptr, 10);
        if (propose([&](const State& base, State& want) -> std::string {
              if (!base.uuid.empty()) return "";  // already learned
              want.uuid = uuid;
              want.term = term;
              want.primary = n;
              return "";
            }) == "OK") {
          std::printf("t=%.2f primary up: term %llu\n", secs(t0, now), term);
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
          std::printf("t=%.2f failover: primary silent for %.2fs, standby promoted to term %llu "
                      "in %.3fs\n", secs(t0, now), silent_s, st.term,
                      secs(t_decided, now));
          std::fflush(stdout);
        } else if (announced != st.term && announced != ~0ull) {
          std::printf("t=%.2f failover learned: node %u serves as primary, term %llu\n",
                      secs(t0, now), p, st.term);
          std::fflush(stdout);
        }
        announced = st.term;
      } else if (f["role"] == "s" && term + 1 == st.term && f["uuid"] == st.uuid) {
        // Decided: never undone, since a lost reply may hide a promotion
        // that happened. Ask until it serves as primary.
        std::string reply;
        ha::ask(host[p], aport[p], "PROMOTE", reply, 1000);
        promoting = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(promoting ? 2 : poll_ms));
    if (promoting || secs(primary_ok, clock::now()) * 1000 < failover_ms) continue;
    // The primary is gone. Promote its standby if it has every acked write.
    if (st.degraded) continue;  // it may not: wait for the primary
    if (!status(o, f) || f["role"] != "s") continue;
    const std::string suuid = f["uuid"];
    const u64 sterm = std::strtoull(f["term"].c_str(), nullptr, 10);
    const auto tp = clock::now();
    bool ours = false;  // this witness's proposal made the change
    std::string r = propose([&](const State& base, State& want) -> std::string {
      ours = false;
      if (base.version != st.version) return "";  // decided meanwhile; look again
      if (base.degraded) return "the primary runs without its standby";
      if (suuid != base.uuid || sterm != base.term) return "the standby is not the primary's";
      want.term = base.term + 1;  // promotion bumps the standby's term by one
      want.primary = o;
      ours = true;
      return "";
    });
    if (r == "OK" && ours) {
      silent_s = secs(primary_ok, tp);
      t_decided = tp;
      decided_term = st.term + 1;
    }
  }
}
