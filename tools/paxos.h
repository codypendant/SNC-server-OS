// Replicated state machines for the control plane (docs/CONTROL_PLANE.md
// section 10): N processes (3 or 5) keep several independent versioned
// values, each called a machine and named by a token ("ha/0", "placement").
// Version n of a machine is the value chosen by that machine's n-th
// instance of single-decree Paxos among the processes; version 0 is the
// empty value. A change needs a majority, so any minority may be down, and
// two changes proposed against the same version can never both be chosen.
//
// Machines share nothing but the processes, the ballot counter and the
// state file: no value spans two machines, and a decision in one never
// moves another's version.
//
// Wire (one line each way, through ha::ask):
//   READ m                      -> STATE <value>
//   PREPARE m v ballot <v-1>    -> PROMISE acc_ballot <acc_value> | NACK promised | LATE <chosen>
//   ACCEPT m v ballot <value>   -> ACCEPTED | NACK promised | LATE <chosen>
//   COMMIT m <value>            -> OK
// where <value> is "version body" and body is the rest of the line ("-"
// when empty). Each process saves its acceptor state, synced, before any
// reply that depends on it.
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ha.h"

namespace snc::paxos {

using u64 = unsigned long long;

struct Value {
  u64 version = 0;
  std::string body;  // one line; empty = nothing chosen yet

  std::string str() const { return std::to_string(version) + " " + (body.empty() ? "-" : body); }
  // "version body...", the body running to the end of the line.
  bool parse(std::istream& in) {
    if (!(in >> version)) return false;
    std::string rest;
    std::getline(in, rest);
    const auto b = rest.find_first_not_of(' ');
    body = b == std::string::npos ? "" : rest.substr(b);
    if (body == "-") body.clear();
    return true;
  }
};

struct Peer {
  std::string host;
  unsigned port;
};

class Node {
 public:
  // Tests only (snc_witness --debug 1, DEBUG SKIP-COMMIT n): the next n
  // values this process gets chosen are not announced with COMMIT, so the
  // other acceptors keep them accepted but unlearned.
  std::atomic<int> debug_skip_commits{0};

  // This process is peers[self]. The state file holds every machine; a
  // file from before machines had names holds one, read as `legacy`.
  void init(std::string state_path, std::vector<Peer> peers, unsigned self, const std::string& legacy = "") {
    path_ = std::move(state_path);
    legacy_ = legacy;
    peers_ = std::move(peers);
    self_ = self;
    load();
  }
  std::size_t majority() const { return peers_.size() / 2 + 1; }
  std::size_t size() const { return peers_.size(); }

  // This process's latest chosen value of machine m (may be behind).
  Value chosen(const std::string& m) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = acc_.find(m);
    return it == acc_.end() ? Value{} : it->second.chosen;
  }

  // The acceptor's side of a request; false if `verb` is not one of ours.
  bool handle(const std::string& verb, std::istream& in, std::string& reply) {
    if (verb != "READ" && verb != "PREPARE" && verb != "ACCEPT" && verb != "COMMIT") return false;
    std::string m;
    if (!(in >> m)) {
      reply = "NO bad request";
      return true;
    }
    std::lock_guard<std::mutex> g(mu_);
    Acceptor& a = acc_[m];
    if (verb == "READ") {
      reply = "STATE " + a.chosen.str();
      return true;
    }
    if (verb == "COMMIT") {
      Value s;
      if (!s.parse(in)) {
        reply = "NO bad request";
        return true;
      }
      if (s.version > a.chosen.version) {
        learn(a, s);
        save();
      }
      reply = "OK";
      return true;
    }
    u64 v = 0, ballot = 0;
    in >> v >> ballot;
    if (verb == "PREPARE") {
      Value base;
      if (!base.parse(in) || base.version + 1 != v) {
        reply = "NO bad request";
        return true;
      }
      bool changed = false;
      if (base.version > a.chosen.version) {  // a proposer only builds on a chosen value
        learn(a, base);
        changed = true;
      }
      if (v <= a.chosen.version) {
        reply = "LATE " + a.chosen.str();
      } else if (v < a.open_v) {
        reply = "NO behind";  // a later instance is open here: v was decided
      } else {
        open_instance(a, v);
        if (ballot <= a.promised) {
          if (changed) save();
          reply = "NACK " + std::to_string(a.promised);
        } else {
          a.promised = ballot;
          save();
          reply = "PROMISE " + std::to_string(a.acc_ballot) + " " + a.acc_value.str();
        }
      }
      return true;
    }
    // ACCEPT
    Value s;
    if (!s.parse(in) || s.version != v) {
      reply = "NO bad request";
    } else if (v <= a.chosen.version) {
      reply = "LATE " + a.chosen.str();
    } else if (v < a.open_v) {
      reply = "NO behind";
    } else {
      // Instance v-1 was decided (someone proposes v), even if this
      // process has not learned it yet.
      open_instance(a, v);
      if (ballot < a.promised) {
        reply = "NACK " + std::to_string(a.promised);
      } else {
        a.promised = ballot;
        a.acc_ballot = ballot;
        a.acc_value = s;
        save();
        reply = "ACCEPTED";
      }
    }
    return true;
  }

  // The newest value of m a majority reports: chosen, since only chosen
  // values are reported. Fails without a majority.
  bool read_latest(const std::string& m, Value& out) {
    std::size_t n = 0;
    out = Value{};
    for (const Reply& r : broadcast("READ " + m, 300)) {
      if (!r.ok) continue;
      std::istringstream in(r.line);
      std::string tag;
      Value s;
      if (!(in >> tag) || tag != "STATE" || !s.parse(in)) continue;
      ++n;
      if (s.version > out.version || n == 1) out = s;
    }
    if (n < majority()) return false;
    std::lock_guard<std::mutex> g(mu_);
    Acceptor& a = acc_[m];
    if (out.version > a.chosen.version) {
      learn(a, out);
      save();
    }
    if (a.chosen.version > out.version) out = a.chosen;
    return true;
  }

  // The latest value of m, linearizably: every value chosen before the
  // call began is this one or older. read_latest alone is not enough: a
  // value accepted by a majority whose COMMIT nobody received yet is chosen
  // but reported by no one. Phase 1 on the next instance finds it (some
  // acceptor of any majority accepted it, or has learned it: LATE), and
  // phase 2 finishes it before the read is answered. Costs a promise (one
  // synced write) per acceptor; for decisions that must not act on a stale
  // value (docs/MIGRATION.md), not for polling.
  bool read_linear(const std::string& m, Value& out, int deadline_ms = 3000) {
    std::lock_guard<std::mutex> serial(one_at_a_time_);
    thread_local std::mt19937 rng(std::random_device{}());
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    auto backoff = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(2 + rng() % 20)); };
    while (std::chrono::steady_clock::now() < end) {
      Value base;
      if (!read_latest(m, base)) {
        backoff();
        continue;
      }
      const u64 b = next_ballot();
      const std::string vb = " " + m + " " + std::to_string(base.version + 1) + " " + std::to_string(b) + " ";
      std::size_t promises = 0;
      u64 best = 0;
      Value value;
      bool late = false;
      for (const Reply& r : broadcast("PREPARE" + vb + base.str(), 300)) {
        if (!r.ok) continue;
        std::istringstream in(r.line);
        std::string tag;
        in >> tag;
        if (tag == "PROMISE") {
          u64 ab = 0;
          Value av;
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
      if (best == 0) {
        out = base;  // nothing newer is chosen, or can be without a later ballot
        return true;
      }
      std::size_t accepted = 0;
      for (const Reply& r : broadcast("ACCEPT" + vb + value.str(), 300)) {
        if (r.ok && r.line == "ACCEPTED") ++accepted;
      }
      if (accepted >= majority()) {
        broadcast("COMMIT " + m + " " + value.str(), 300);
        std::lock_guard<std::mutex> g(mu_);
        Acceptor& a = acc_[m];
        if (value.version > a.chosen.version) {
          learn(a, value);
          save();
        }
      } else {
        backoff();
      }
    }
    return false;
  }

  // Changes machine m with `change(base, want_body)`, which returns "" and
  // sets the wanted body, or returns why the change is refused. Retries
  // while other proposals win. Returns "OK" once the wanted body is chosen
  // (or already holds), "NO <why>" otherwise. `same(a, b)`: two bodies
  // record the same decision. `version`, if given, gets the version of the
  // chosen value that holds the wanted body (the base's when it already
  // held). `fresh`: always choose a new instance, even when the base
  // already holds the wanted body, and never with the version of a value
  // adopted in phase 1 (docs/SYNC_READINESS.md 7: a NOT READY must take a
  // version no earlier request was answered with).
  template <class F, class Same>
  std::string propose(const std::string& m, F change, Same same, int deadline_ms = 1000, u64* version = nullptr,
                      bool fresh = false) {
    std::lock_guard<std::mutex> serial(one_at_a_time_);  // ours do not race each other
    thread_local std::mt19937 rng(std::random_device{}());
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    auto backoff = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(2 + rng() % 20)); };
    while (std::chrono::steady_clock::now() < end) {
      Value base;
      if (!read_latest(m, base)) {
        backoff();
        continue;
      }
      Value want;
      want.body = base.body;
      std::string why = change(base, want.body);
      if (!why.empty()) return "NO " + why;
      if (!fresh && same(want.body, base.body)) {
        if (version) *version = base.version;
        return "OK";
      }
      want.version = base.version + 1;
      const u64 b = next_ballot();
      const std::string vb = " " + m + " " + std::to_string(want.version) + " " + std::to_string(b) + " ";
      // Phase 1.
      std::size_t promises = 0;
      u64 best = 0;
      Value value = want;
      bool late = false;
      for (const Reply& r : broadcast("PREPARE" + vb + base.str(), 300)) {
        if (!r.ok) continue;
        std::istringstream in(r.line);
        std::string tag;
        in >> tag;
        if (tag == "PROMISE") {
          u64 ab = 0;
          Value av;
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
      // A value accepted before in this instance, ours or another's: it may
      // already be chosen, and its proposer may have answered for it.
      const bool adopted = best != 0;
      // Phase 2: our value, or the one a majority may already have chosen.
      std::size_t accepted = 0;
      for (const Reply& r : broadcast("ACCEPT" + vb + value.str(), 300)) {
        if (r.ok && r.line == "ACCEPTED") ++accepted;
        else if (r.ok && r.line.rfind("NACK ", 0) == 0) saw_ballot(std::strtoull(r.line.c_str() + 5, nullptr, 10));
      }
      if (accepted < majority()) {
        backoff();
        continue;
      }
      // Chosen. Tell everyone (best effort: one that misses it learns it
      // from the next proposal or read). Tests may drop it (a lost COMMIT).
      if (debug_skip_commits.load() > 0) {
        debug_skip_commits.fetch_sub(1);
      } else {
        broadcast("COMMIT " + m + " " + value.str(), 300);
      }
      {
        std::lock_guard<std::mutex> g(mu_);
        Acceptor& a = acc_[m];
        if (value.version > a.chosen.version) {
          learn(a, value);
          save();
        }
      }
      // fresh: never answer with an adopted value's version, which another
      // request may already have been answered with (SYNC_READINESS 7.2):
      // build a value of our own on top of it.
      if (same(value.body, want.body) && !(fresh && adopted)) {
        if (version) *version = value.version;
        return "OK";
      }
      // Another proposal was chosen in this instance; try again on top of it.
    }
    return "NO no quorum";
  }

 private:
  // One machine's acceptor state for one instance, open_v (normally
  // chosen.version + 1; later when this process has not yet learned the
  // instances before it).
  struct Acceptor {
    Value chosen;
    u64 open_v = 1;
    u64 promised = 0;    // highest ballot promised in open_v
    u64 acc_ballot = 0;  // ballot of the value accepted in it (0: none)
    Value acc_value;
  };
  struct Reply {
    bool ok = false;
    std::string line;
  };

  // Moves the acceptor to instance v, forgetting an earlier one. Only for
  // an instance after open_v, which is then decided. Caller holds mu_.
  static void open_instance(Acceptor& a, u64 v) {
    if (v <= a.open_v) return;
    a.open_v = v;
    a.promised = 0;
    a.acc_ballot = 0;
    a.acc_value = Value{};
  }
  // A newer chosen value closes the instances up to it. Caller holds mu_.
  static void learn(Acceptor& a, const Value& s) {
    if (s.version <= a.chosen.version) return;
    a.chosen = s;
    open_instance(a, s.version + 1);
  }

  std::vector<Reply> broadcast(const std::string& line, int timeout_ms) {
    std::vector<Reply> r(peers_.size());
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < peers_.size(); ++i) {
      ts.emplace_back([&, i] { r[i].ok = ha::ask(peers_[i].host, peers_[i].port, line, r[i].line, timeout_ms); });
    }
    for (auto& t : ts) t.join();
    return r;
  }

  u64 next_ballot() {
    std::lock_guard<std::mutex> g(mu_);
    // Wall-clock based, so a restarted process does not reuse its old ballots.
    u64 now_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
    ballot_counter_ = std::max(ballot_counter_ + 1, now_ms);
    return (ballot_counter_ << 6) | self_;
  }
  void saw_ballot(u64 b) {
    std::lock_guard<std::mutex> g(mu_);
    ballot_counter_ = std::max(ballot_counter_, b >> 6);
  }

  // Lines "chosen m <value>", "open m v", "promised m b", "accepted m b
  // <value>". Caller holds mu_.
  void save() {
    std::ostringstream o;
    for (auto& [m, a] : acc_) {
      o << "chosen " << m << ' ' << a.chosen.str() << "\nopen " << m << ' ' << a.open_v << "\npromised " << m
        << ' ' << a.promised << "\naccepted " << m << ' ' << a.acc_ballot << ' ' << a.acc_value.str() << "\n";
    }
    const std::string s = o.str(), tmp = path_ + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || ::write(fd, s.data(), s.size()) != static_cast<ssize_t>(s.size()) || ::fsync(fd) != 0 ||
        ::close(fd) != 0 || ::rename(tmp.c_str(), path_.c_str()) != 0) {
      std::perror("paxos: cannot save state");
      std::exit(1);
    }
    std::string dir = path_.rfind('/') == std::string::npos ? "." : path_.substr(0, path_.rfind('/'));
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      ::fsync(dfd);
      ::close(dfd);
    }
  }

  void load() {
    FILE* f = std::fopen(path_.c_str(), "r");
    if (f == nullptr) return;
    char* buf = nullptr;
    std::size_t cap = 0;
    while (::getline(&buf, &cap, f) > 0) {
      std::string line(buf);
      std::istringstream probe(line);
      std::string k, m;
      if (!(probe >> k >> m)) continue;
      if (!legacy_.empty() && m.find_first_not_of("0123456789") == std::string::npos) {
        line = k + " " + legacy_ + line.substr(k.size());  // "chosen 3 ..." -> "chosen ha/0 3 ..."
      }
      std::istringstream in(line);
      in >> k >> m;
      Acceptor& a = acc_[m];
      if (k == "chosen") a.chosen.parse(in);
      else if (k == "open") in >> a.open_v;
      else if (k == "promised") in >> a.promised;
      else if (k == "accepted") {
        in >> a.acc_ballot;
        a.acc_value.parse(in);
      }
    }
    std::free(buf);
    std::fclose(f);
  }

  std::mutex mu_;
  std::mutex one_at_a_time_;
  std::map<std::string, Acceptor> acc_;
  std::string path_, legacy_;
  std::vector<Peer> peers_;
  unsigned self_ = 0;
  u64 ballot_counter_ = 0;
};

}  // namespace snc::paxos
