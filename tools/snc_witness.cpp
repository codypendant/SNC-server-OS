// snc_witness: decides when a standby is promoted (docs/REPLICATION.md,
// "Automatic failover"). It polls the primary's and the standby's admin
// ports. If the primary has not answered for --failover-ms and the standby
// is up, it promotes the standby, unless it has let the primary continue
// without that standby (then the standby may lack acknowledged writes).
// A sync primary started with --witness asks before continuing without
// its standby; the witness refuses a primary whose term it has replaced.
//
// Its decisions are recorded in --state (written and synced before they
// are acted on), so a restarted witness keeps them. One failover per
// configuration: afterwards it serves the new primary's requests and
// watches nothing until it is restarted with a new standby.
//
// Usage: snc_witness --port P --primary HOST:ADMIN --standby HOST:ADMIN
//                    --state FILE [--failover-ms N] [--poll-ms N]
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "ha.h"

using namespace snc;

namespace {

struct State {
  std::string uuid;     // the space; empty until learned
  unsigned long long term = 0;  // the current primary's term
  bool degraded = false;        // the primary may run without the standby
  bool failed_over = false;     // the standby was promoted
};

bool load(const std::string& path, State& st) {
  std::ifstream f(path);
  if (!f) return false;
  std::string k;
  while (f >> k) {
    if (k == "uuid") f >> st.uuid;
    else if (k == "term") f >> st.term;
    else if (k == "degraded") f >> st.degraded;
    else if (k == "failed_over") f >> st.failed_over;
  }
  return true;
}

void save(const std::string& path, const State& st) {
  std::string tmp = path + ".tmp";
  std::ostringstream o;
  o << "uuid " << (st.uuid.empty() ? "-" : st.uuid) << "\nterm " << st.term << "\ndegraded "
    << st.degraded << "\nfailed_over " << st.failed_over << "\n";
  std::string s = o.str();
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0 || ::write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size()) || ::fsync(fd) != 0 ||
      ::close(fd) != 0 || ::rename(tmp.c_str(), path.c_str()) != 0) {
    std::perror("snc_witness: cannot save state");
    std::exit(1);
  }
  std::string dir = path.rfind('/') == std::string::npos ? "." : path.substr(0, path.rfind('/'));
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
}

// "role=p term=3 ..." -> map.
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

}  // namespace

int main(int argc, char** argv) {
  unsigned port = 0, pport = 0, sport = 0;
  std::string phost, shost, state_path;
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
    else {
      std::fprintf(stderr, "bad flag %s %s\n", k.c_str(), v.c_str());
      return 2;
    }
  }
  if (port == 0 || pport == 0 || sport == 0 || state_path.empty()) {
    std::fprintf(stderr, "usage: snc_witness --port P --primary HOST:ADMIN --standby HOST:ADMIN "
                         "--state FILE [--failover-ms N] [--poll-ms N]\n");
    return 2;
  }
  ::signal(SIGPIPE, SIG_IGN);
  State st;
  if (load(state_path, st) && st.uuid == "-") st.uuid.clear();
  int lfd = ha::listen_on(port);
  if (lfd < 0) {
    std::fprintf(stderr, "snc_witness: cannot listen on %u\n", port);
    return 1;
  }
  std::printf("snc_witness: port %u, term %llu%s%s\n", port, st.term,
              st.degraded ? ", primary running without its standby" : "",
              st.failed_over ? ", already failed over" : "");
  std::fflush(stdout);

  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  auto primary_ok = t0;  // last time the primary answered
  bool primary_seen = false;
  auto next_poll = t0;
  while (true) {
    // Requests from the primary (or, after a failover, the new one).
    int wait = static_cast<int>(std::max<long long>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(next_poll - clock::now()).count()));
    if (ha::wait_fd(lfd, POLLIN, wait)) {
      int fd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) {
        std::string line, reply = "NO bad request";
        if (ha::read_line(fd, line, 500)) {
          std::istringstream in(line);
          std::string verb, uuid;
          unsigned long long term = 0;
          in >> verb >> uuid >> term;
          if ((verb == "DEGRADE" || verb == "SYNCED") && !st.uuid.empty() && uuid != st.uuid) {
            reply = "NO another space";
          } else if (verb == "DEGRADE" || verb == "SYNCED") {
            if (term != st.term) {
              reply = "NO term " + std::to_string(term) + " was replaced by " + std::to_string(st.term);
            } else {
              bool d = verb == "DEGRADE";
              if (st.degraded != d) {
                st.degraded = d;
                save(state_path, st);  // before the primary acts on it
              }
              std::printf("t=%.2f %s: primary term %llu %s\n", secs(t0, clock::now()),
                          verb.c_str(), term,
                          d ? "continues without its standby" : "has its standby back in sync");
              reply = "OK";
            }
          }
          ha::write_all(fd, reply + "\n");
        }
        ::close(fd);
      }
      std::fflush(stdout);
      continue;
    }
    next_poll = clock::now() + std::chrono::milliseconds(poll_ms);
    if (st.failed_over) continue;

    std::string reply;
    const auto now = clock::now();
    if (ha::ask(phost, pport, "STATUS", reply, poll_ms)) {
      auto f = fields(reply);
      if (f["role"] == "p" || f["role"] == "n") {
        primary_ok = now;
        if (!primary_seen) {
          primary_seen = true;
          if (st.uuid.empty()) {
            st.uuid = f["uuid"];
            st.term = std::strtoull(f["term"].c_str(), nullptr, 10);
            save(state_path, st);
          }
          std::printf("t=%.2f primary up: term %s\n", secs(t0, now), f["term"].c_str());
          std::fflush(stdout);
        }
      }
    }
    if (!primary_seen || secs(primary_ok, now) * 1000 < failover_ms) continue;
    // The primary is gone. Promote the standby if it has every acked write.
    if (st.degraded) continue;  // it may not: wait for the primary
    if (!ha::ask(shost, sport, "STATUS", reply, poll_ms)) continue;
    auto f = fields(reply);
    if (f["role"] != "s" || f["uuid"] != st.uuid ||
        std::strtoull(f["term"].c_str(), nullptr, 10) != st.term) {
      continue;
    }
    st.term += 1;  // promotion bumps the standby's term by one
    st.failed_over = true;
    save(state_path, st);  // from here the old primary's requests are refused
    const auto tp = clock::now();
    // Decided: never undone, since a lost reply may hide a promotion that
    // happened. Ask until the standby serves as primary.
    while (true) {
      if (ha::ask(shost, sport, "STATUS", reply, poll_ms) && fields(reply)["role"] != "s") break;
      ha::ask(shost, sport, "PROMOTE", reply, 1000);
      ::usleep(2000);
    }
    std::printf("t=%.2f failover: primary silent for %.2fs, standby promoted to term %llu "
                "in %.3fs\n", secs(t0, clock::now()), secs(primary_ok, tp), st.term,
                secs(tp, clock::now()));
    std::fflush(stdout);
  }
}
