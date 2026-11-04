// Slot moves (docs/MIGRATION.md): the node side of a move (requests on a
// node's admin port, "MOVE ..."), and the driver that runs a move by
// talking to the control plane and the two groups' primaries.
//
// The driver keeps no state of its own. The move's progress is in the
// placement machine (PlacementMove) and in the groups' slot states; each
// step is idempotent, so a driver that dies anywhere is replaced by running
// it again, which reads where the move is and carries on (or aborts it, up
// to the commit).
//
// Node requests (one line each way):
//   MOVE ROLE                    -> ROLE p|s|f|m EPOCH   (m: promoting, the start barrier)
//   MOVE INFO SLOT               -> INFO <report>
//   MOVE SEAL SLOT MOVE TO EPOCH -> OK <report> | NO <status> <report>
//   MOVE UNSEAL SLOT             -> OK <report> | NO <why>   (checks the control plane)
//   MOVE HAVE SLOT               -> HAVE N OBJ...
//   MOVE EXPORT SLOT MOVE FINAL N OBJ...  -> OBJS COUNT DIGEST N OBJ... | NO <status>
//   MOVE STAGE SLOT MOVE FROM EPOCH FINAL COUNT DIGEST N OBJ... -> OK <report> | NO <status> <report>
//   MOVE ACTIVATE SLOT           -> OK <report> | NO <report>  (refreshes the map first)
//   MOVE REFRESH                 -> OK EPOCH
//   MOVE STATES                  -> STATES N SLOT:PHASE:FLAGS:MOVE:EPOCH:DURABLE ...
// OBJ is ID:VERSION:TYPE:SCHEMA:CRC:PAYLOAD, PAYLOAD hex, "-" when not sent.
// <report> is the slot's state: phase group move epoch count digest durable
// serving map_group map_epoch live_count live_digest, then (fail-closed
// serving, docs/MIGRATION_FAIL_CLOSED.md 13) flags=F proof=WHY
// arrived=MOVE:EPOCH, which older parsers ignore.
#pragma once

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "control.h"
#include "ha.h"
#include "snc/trace.h"

namespace snc::migrate {

// ---- Encoding ---------------------------------------------------------------------------

inline std::string hex(const u8* p, std::size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (std::size_t i = 0; i < n; ++i) {
    s[2 * i] = d[p[i] >> 4];
    s[2 * i + 1] = d[p[i] & 15];
  }
  return s;
}

inline bool unhex(const std::string& s, std::vector<u8>& out) {
  if (s.size() % 2) return false;
  out.resize(s.size() / 2);
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int a = v(s[2 * i]), b = v(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = static_cast<u8>(a * 16 + b);
  }
  return true;
}

inline std::string encode(const SlotObject& o) {
  std::ostringstream s;
  s << o.id.to_string() << ':' << o.version << ':' << o.type_id << ':' << o.schema_ver << ':' << o.crc << ':'
    << (o.has_payload ? (o.payload.empty() ? std::string("~") : hex(o.payload.data(), o.payload.size())) : "-");
  return s.str();
}

inline bool decode(const std::string& text, SlotObject& o) {
  std::vector<std::string> f;
  std::istringstream in(text);
  for (std::string x; std::getline(in, x, ':');) f.push_back(x);
  if (f.size() != 6) return false;
  const auto id = ObjectID::parse(f[0]);
  if (!id) return false;
  o.id = *id;
  o.version = std::strtoull(f[1].c_str(), nullptr, 10);
  o.type_id = static_cast<u32>(std::strtoul(f[2].c_str(), nullptr, 10));
  o.schema_ver = static_cast<u16>(std::strtoul(f[3].c_str(), nullptr, 10));
  o.crc = static_cast<u32>(std::strtoul(f[4].c_str(), nullptr, 10));
  o.payload.clear();
  o.has_payload = f[5] != "-";
  if (f[5] == "~") return true;
  return !o.has_payload || unhex(f[5], o.payload);
}

inline std::string encode_list(const std::vector<SlotObject>& v) {
  std::string s = std::to_string(v.size());
  for (const SlotObject& o : v) s += " " + encode(o);
  return s;
}

inline bool decode_list(std::istream& in, std::vector<SlotObject>& v) {
  std::size_t n = 0;
  if (!(in >> n)) return false;
  v.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string t;
    if (!(in >> t) || !decode(t, v[i])) return false;
  }
  return true;
}

inline std::string report_text(const SlotReport& r) {
  std::ostringstream s;
  s << unsigned(r.phase) << ' ' << r.group << ' ' << std::hex << r.move_id << std::dec << ' ' << r.epoch << ' '
    << r.count << ' ' << r.digest << ' ' << r.durable << ' ' << r.serving << ' ' << r.map_group << ' '
    << r.map_epoch << ' ' << r.live_count << ' ' << r.live_digest << " flags=" << r.flags
    << " proof=" << slot_proof_name(r.proof) << " arrived=" << std::hex << r.arrived.move << std::dec << ':'
    << r.arrived.epoch;
  return s.str();
}

inline bool parse_report(std::istream& in, SlotReport& r) {
  unsigned phase = 0;
  int durable = 0, serving = 0;
  if (!(in >> phase >> r.group >> std::hex >> r.move_id >> std::dec >> r.epoch >> r.count >> r.digest >> durable >>
        serving >> r.map_group >> r.map_epoch >> r.live_count >> r.live_digest)) {
    return false;
  }
  r.phase = static_cast<u8>(phase);
  r.durable = durable != 0;
  r.serving = serving != 0;
  // The fail-closed fields, if the node sends them (the rest of the line).
  for (std::string kv; in >> kv;) {
    if (kv.rfind("flags=", 0) == 0) {
      r.flags = static_cast<u16>(std::strtoul(kv.c_str() + 6, nullptr, 10));
    } else if (kv.rfind("proof=", 0) == 0) {
      const std::string v = kv.substr(6);
      for (u8 p = 0; p <= static_cast<u8>(SlotProof::STALE_MAP); ++p) {
        if (v == slot_proof_name(static_cast<SlotProof>(p))) r.proof = static_cast<SlotProof>(p);
      }
    } else if (kv.rfind("arrived=", 0) == 0) {
      const auto c = kv.find(':');
      if (c != std::string::npos) {
        r.arrived.move = std::strtoull(kv.c_str() + 8, nullptr, 16);
        r.arrived.epoch = std::strtoull(kv.c_str() + c + 1, nullptr, 10);
      }
    }
  }
  return true;
}

// ---- The control plane ------------------------------------------------------------------

struct Snapshot {
  u64 version = 0;
  Placement p;
};

// The placement, linearizably (PLACEMENT SYNC): decisions that must not
// rest on a stale view (unsealing, the driver's next step) use this.
inline bool sync(const control::Members& ms, Snapshot& out, std::string& why) {
  return control::sync(ms, out.version, out.p, why);
}

// "OK" once `next` is version `at` + 1.
inline std::string cas(const control::Members& ms, u64 at, const Placement& next) {
  std::string last = "NO no control member answered";
  for (const auto& [h, port] : ms) {
    std::string reply;
    if (!ha::ask(h, port, "PLACEMENT CAS " + std::to_string(at) + " " + next.encode(), reply, 5000)) continue;
    return reply;
  }
  return last;
}

// ---- The node side ----------------------------------------------------------------------

class NodeHandler {
 public:
  // `role`: 'p', 's' or 'f' (fenced) for this node now.
  NodeHandler(Engine& eng, control::Members ms, u16 group, std::function<char()> role)
      : eng_(eng), ms_(std::move(ms)), group_(group), role_(std::move(role)) {}

  std::string handle(const std::string& line) {
    std::istringstream in(line);
    std::string verb, what;
    in >> verb >> what;
    if (verb != "MOVE") return "NO unknown request";
    unsigned slot = 0;
    if (what == "ROLE") return std::string("ROLE ") + role_() + " " + std::to_string(eng_.cluster().epoch);
    if (what == "REFRESH") {
      refresh();
      return "OK " + std::to_string(eng_.cluster().epoch);
    }
    if (what == "STATES") {
      std::string out;
      std::size_t n = 0;
      for (const SlotReport& r : eng_.slot_states()) {
        char b[96];
        std::snprintf(b, sizeof(b), " %u:%u:%u:%llx:%llu:%d", r.slot, unsigned(r.phase), unsigned(r.flags),
                      (unsigned long long)r.move_id, (unsigned long long)r.epoch, r.durable ? 1 : 0);
        out += b;
        ++n;
      }
      return "STATES " + std::to_string(n) + out;
    }
    if (!(in >> slot) || slot >= kSlots) return "NO bad request";
    const u16 s = static_cast<u16>(slot);
    if (what == "INFO") return "INFO " + report_text(eng_.slot_report(s));
    if (what == "HAVE") {
      std::vector<SlotObject> v;
      eng_.slot_objects(s, v);
      return "HAVE " + encode_list(v);
    }
    if (what == "SEAL") {
      u64 move = 0, epoch = 0;
      unsigned to = 0;
      if (!(in >> std::hex >> move >> std::dec >> to >> epoch)) return "NO bad request";
      std::string why;
      if (!current(s, move, PlacementMove::State::PREPARED, why)) return "NO " + why;
      SlotReport r;
      const Status st = eng_.seal_slot(s, move, static_cast<u16>(to), epoch, r);
      return (st == Status::OK ? "OK " : std::string("NO ") + status_name(st) + " ") + report_text(r);
    }
    if (what == "UNSEAL") return unseal(s);
    if (what == "EXPORT") {
      u64 move = 0;
      int final = 0;
      std::vector<SlotObject> have, out;
      if (!(in >> std::hex >> move >> std::dec >> final) || !decode_list(in, have)) return "NO bad request";
      u64 count = 0, digest = 0;
      const Status st = eng_.export_slot(s, move, final != 0, have, out, count, digest);
      if (st != Status::OK) return std::string("NO ") + status_name(st);
      return "OBJS " + std::to_string(count) + " " + std::to_string(digest) + " " + encode_list(out);
    }
    if (what == "STAGE") {
      u64 move = 0, epoch = 0, count = 0, digest = 0;
      unsigned from = 0;
      int final = 0;
      std::vector<SlotObject> objs;
      const u64 t0 = trace::enabled() ? trace::now_ns() : 0;
      if (!(in >> std::hex >> move >> std::dec >> from >> epoch >> final >> count >> digest) ||
          !decode_list(in, objs)) {
        return "NO bad request";
      }
      const u64 t1 = trace::enabled() ? trace::now_ns() : 0;
      std::string why;
      const bool cur = current(s, move, final ? PlacementMove::State::SEALED : PlacementMove::State::PREPARED, why);
      SNC_TRACE("move_stage_req slot=%u final=%d objs=%zu decode_ms=%.1f control_check_ms=%.1f current=%d", slot,
                final, objs.size(), (t1 - t0) / 1e6, trace::enabled() ? (trace::now_ns() - t1) / 1e6 : 0.0,
                cur ? 1 : 0);
      if (!cur) return "NO " + why;
      SlotReport r;
      const Status st = eng_.stage_slot(s, move, static_cast<u16>(from), epoch, final != 0, objs, count, digest, r);
      return (st == Status::OK ? "OK " : std::string("NO ") + status_name(st) + " ") + report_text(r);
    }
    if (what == "ACTIVATE") {
      refresh();
      const SlotReport r = eng_.slot_report(s);
      return (r.serving ? "OK " : "NO ") + report_text(r);
    }
    return "NO unknown request";
  }

 private:
  // Through the engine's placement_fetch (control::apply), which also keeps
  // the newer map in the data directory.
  void refresh() { eng_.refresh_placement(); }

  // A seal or a stage only for the move in progress, at the step that asks
  // for it: a request from a driver that stalled and was overtaken (its move
  // aborted, or finished by another) is refused. The groups also refuse
  // older moves by ID (Shard::seal_slot, stage_slot); this check keeps an
  // aborted move from leaving a seal behind (only a request already past it
  // when the move was aborted can, and an abort or move run again clears
  // that: Driver::settle_owner).
  bool current(u16 slot, u64 move, PlacementMove::State want, std::string& why) {
    Snapshot sn;
    if (!sync(ms_, sn, why)) return false;
    const PlacementMove& m = sn.p.move;
    if (m.id != move || m.slot != slot || m.state != want) {
      why = "not the step of the move in progress";
      return false;
    }
    return true;
  }

  // A seal stays until its move is aborted: the control plane's latest
  // placement (read linearizably: a stale one could predate the move) gives
  // this group the slot and has no move of it under this seal's ID. Then no
  // commit of that move can ever be chosen.
  std::string unseal(u16 slot) {
    const SlotReport r = eng_.slot_report(slot);
    if (r.phase != 2) return "OK " + report_text(r);  // not sealed
    Snapshot sn;
    std::string why;
    if (!sync(ms_, sn, why)) return "NO " + why;
    if (sn.p.slots[slot] != group_) return "NO the slot is another group's";
    if (sn.p.move.state != PlacementMove::State::NONE && sn.p.move.id == r.move_id) {
      return "NO its move is in progress";
    }
    SlotReport out;
    const Status st = eng_.unseal_slot(slot, r.move_id, out);
    return (st == Status::OK ? "OK " : std::string("NO ") + status_name(st) + " ") + report_text(out);
  }

  Engine& eng_;
  control::Members ms_;
  u16 group_;
  std::function<char()> role_;
};

// An admin port for processes that have no other (tests): each connection
// one request, answered on its own thread (a seal waits for durability).
class AdminServer {
 public:
  AdminServer(unsigned port, std::function<std::string(const std::string&)> fn) : fn_(std::move(fn)) {
    fd_ = ha::listen_on(port);
    if (fd_ >= 0) thread_ = std::thread([this] { loop(); });
  }
  ~AdminServer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    std::unique_lock<std::mutex> g(mu_);
    cv_.wait(g, [&] { return busy_ == 0; });
    if (fd_ >= 0) ::close(fd_);
  }
  bool ok() const { return fd_ >= 0; }

 private:
  void loop() {
    while (!stop_) {
      if (!ha::wait_fd(fd_, POLLIN, 50)) continue;
      const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (c < 0) continue;
      {
        std::lock_guard<std::mutex> g(mu_);
        ++busy_;
      }
      std::thread([this, c] {
        std::string line;
        if (ha::read_line(c, line, 5000)) ha::write_all(c, fn_(line) + "\n");
        ::close(c);
        std::lock_guard<std::mutex> g(mu_);
        --busy_;
        cv_.notify_all();
      }).detach();
    }
  }
  std::function<std::string(const std::string&)> fn_;
  int fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  int busy_ = 0;
};

// ---- The driver -------------------------------------------------------------------------

class Driver {
 public:
  explicit Driver(control::Members ms) : ms_(std::move(ms)) {}

  // Called after each step completes, with its name: prepared, copying
  // (half the copy ahead of the seal staged), copied, sealed, staged,
  // committed, activated, aborted. True: stop there, as if the driver
  // had died (tests).
  std::function<bool(const std::string&)> stop_at;
  // After the commit, ask every node to refresh its map at once (else
  // they find out from node links or their next poll).
  bool push_refresh = true;
  int deadline_ms = 30000;
  int chunk = 8;  // objects per STAGE request ahead of the seal
  // Write placement v2 (docs/MIGRATION_FAIL_CLOSED.md 15): the commit
  // records the slot's arrival. Only once every node, driver and snc_ctl
  // reads v2. A placement already v2 is always written as v2, with or
  // without this: nothing here ever lowers the version or drops arrivals.
  bool placement_v2 = false;
  // The commit gate (5b): how many times a source primary may be seen
  // serving the slot at placement STAGED before the move is aborted.
  int gate_serving_limit = 3;
  // Final stages refused E_VERSION_CONFLICT before the move is aborted.
  int version_conflict_limit = 3;
  std::string log;

  // Moves `slot` to group `to`, or carries on the move of it in progress.
  // DONE, ABORTED (someone else aborted it), STOPPED <step>, BUSY <move>
  // (another slot's move is in progress), STUCK <why>.
  std::string move(u16 slot, u16 to) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    gate_serving_ = version_conflicts_ = 0;
    gate_wait_.clear();
    aborted_.clear();
    u64 mine = 0;
    std::string why, last_state = "none";
    while (std::chrono::steady_clock::now() < end) {
      Snapshot sn;
      if (!sync(ms_, sn, why)) {
        pause();
        continue;
      }
      // sync() sets `why` before it tries; once it succeeds that text is
      // stale, and a STUCK at the deadline must not report it.
      why.clear();
      const Placement& P = sn.p;
      last_state = PlacementMove::name(P.move.state);
      if (P.move.state == PlacementMove::State::NONE) {
        if (mine != 0 || P.slots[slot] == to) {
          settle_owner(P, slot, end);
          return P.slots[slot] == to ? "DONE" : aborted_.empty() ? "ABORTED" : "ABORTED " + aborted_;
        }
        if (to >= P.groups.size()) return "STUCK no group " + std::to_string(to);
        Placement N = P;
        N.move.id = sn.version + 1;  // the version this proposal is chosen at, if it is
        N.move.slot = slot;
        N.move.from = P.slots[slot];
        N.move.to = to;
        N.move.epoch = P.epoch + 1;
        N.move.state = PlacementMove::State::PREPARED;
        const std::string r = cas(ms_, sn.version, N);
        note("prepare move " + hexid(N.move.id) + ": " + r);
        if (r == "OK") {
          mine = N.move.id;
          if (stop("prepared")) return "STOPPED prepared";
        }
        continue;
      }
      const PlacementMove m = P.move;
      if (m.slot != slot || m.to != to) return "BUSY move " + hexid(m.id) + " of slot " + std::to_string(m.slot);
      mine = m.id;
      std::string r;
      switch (m.state) {
        case PlacementMove::State::PREPARED: r = step_prepared(sn, why); break;
        case PlacementMove::State::SEALED: r = step_sealed(sn, why); break;
        case PlacementMove::State::STAGED: r = step_staged(sn, why); break;
        case PlacementMove::State::COMMITTED: r = step_committed(sn, why); break;
        default: break;
      }
      if (r.rfind("STOPPED", 0) == 0 || r.rfind("STUCK", 0) == 0) return r;
      if (r == "retry") pause();
    }
    return "STUCK " + (why.empty() ? std::string("deadline") : why) + " (after " +
           std::to_string(deadline_ms / 1000) + " s, move state " + last_state + ")";
  }

  // Aborts the move of `slot` if it has not committed: ABORTED, NONE (no
  // move of it), NO committed (it can only be finished: run move), STOPPED
  // aborted, STUCK <why>. Either way the slot's owner is left unsealed.
  std::string abort(u16 slot) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    bool did = false;
    std::string why;
    while (std::chrono::steady_clock::now() < end) {
      Snapshot sn;
      if (!sync(ms_, sn, why)) {
        pause();
        continue;
      }
      const Placement& P = sn.p;
      if (P.move.state == PlacementMove::State::NONE || P.move.slot != slot) {
        settle_owner(P, slot, end);
        return did ? "ABORTED" : "NONE";
      }
      if (P.move.state == PlacementMove::State::COMMITTED) return "NO committed";
      Placement N = P;
      N.move = PlacementMove{};
      const std::string r = cas(ms_, sn.version, N);
      note("abort move " + hexid(P.move.id) + " (" + PlacementMove::name(P.move.state) + "): " + r);
      if (r == "OK") {
        did = true;
        if (stop("aborted")) return "STOPPED aborted";
      }
    }
    return "STUCK " + why;
  }

  // The conservative backfill (docs/MIGRATION_FAIL_CLOSED.md 15, review
  // of PR #39 item 6): arrivals for slots that moved before placement v2,
  // from the evidence the groups hold. Writes an arrival only where it is
  // unambiguous: the owner's primary holds ACTIVE(M) for the slot, not an
  // undone seal, durable, at an epoch the placement has reached, and no
  // other group holds a record of a later move of it (a seal of M itself
  // at its source is expected). Every group's primary must answer. Slots
  // with no record anywhere never moved and need none. Anything else is
  // left without an arrival and reported. One CAS, raising the epoch so
  // running nodes adopt it; idempotent (slots with an arrival are skipped).
  // "BACKFILLED N at epoch E", or "NOTHING", then "; UNRESOLVED K: ..."; or
  // "NO <why>" (a move in progress, a group without a primary).
  std::string backfill() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    std::string why = "deadline";
    while (std::chrono::steady_clock::now() < end) {
      Snapshot sn;
      if (!sync(ms_, sn, why)) {
        pause();
        continue;
      }
      const Placement& P = sn.p;
      if (P.move.state != PlacementMove::State::NONE) return "NO a move is in progress";
      struct Rec {
        bool have = false;
        SlotReport r;
      };
      std::vector<std::vector<Rec>> recs(P.groups.size(), std::vector<Rec>(kSlots));
      for (u16 g = 0; g < P.groups.size(); ++g) {
        Node n;
        if (!primary(P, g, n, why)) return "NO " + why;
        const std::string a = ask_node(n, "MOVE STATES");
        std::istringstream in(a);
        std::string tag;
        std::size_t cnt = 0;
        if (!(in >> tag) || tag != "STATES" || !(in >> cnt)) return "NO group " + std::to_string(g) + " STATES: " + a;
        for (std::size_t i = 0; i < cnt; ++i) {
          std::string t;
          unsigned slot = 0, phase = 0, flags = 0;
          unsigned long long mv = 0, ep = 0;
          int durable = 0;
          if (!(in >> t) || std::sscanf(t.c_str(), "%u:%u:%u:%llx:%llu:%d", &slot, &phase, &flags, &mv, &ep,
                                         &durable) != 6 || slot >= kSlots) {
            return "NO group " + std::to_string(g) + " sent a malformed state";
          }
          Rec& x = recs[g][slot];
          x.have = true;
          x.r.slot = static_cast<u16>(slot);
          x.r.phase = static_cast<u8>(phase);
          x.r.flags = static_cast<u16>(flags);
          x.r.move_id = mv;
          x.r.epoch = ep;
          x.r.durable = durable != 0;
        }
      }
      Placement N = P;
      std::size_t added = 0;
      std::string unresolved;
      std::size_t n_unresolved = 0;
      for (u32 s = 0; s < kSlots; ++s) {
        if (P.arrived[s].move != 0) continue;
        const u16 owner = P.slots[s];
        bool any = false;
        for (const auto& gr : recs) any |= gr[s].have;
        if (!any) continue;  // never moved
        auto refuse = [&](const std::string& w) {
          unresolved += (n_unresolved ? "," : " ") + std::to_string(s) + ":" + w;
          ++n_unresolved;
        };
        const Rec& o = recs[owner][s];
        if (!o.have) {
          refuse("owner-has-no-record");
          continue;
        }
        if (o.r.phase != 1 || (o.r.flags & 1) || o.r.move_id == 0 || o.r.epoch == 0 || o.r.epoch > P.epoch ||
            !o.r.durable) {
          refuse(o.r.phase != 1 ? "owner-not-active" : (o.r.flags & 1) ? "owner-unsealed" : "owner-record-unusable");
          continue;
        }
        bool later = false, odd = false;
        for (u16 g = 0; g < recs.size(); ++g) {
          if (g == owner || !recs[g][s].have) continue;
          const SlotReport& x = recs[g][s].r;
          if (x.move_id > o.r.move_id) later = true;
          if (x.move_id == o.r.move_id && x.phase != 2) odd = true;  // only its source's seal is expected
        }
        if (later || odd) {
          refuse(later ? "later-move-elsewhere" : "ambiguous");
          continue;
        }
        N.arrived[s] = SlotArrival{o.r.move_id, o.r.epoch};
        ++added;
      }
      const std::string tail =
          n_unresolved ? "; UNRESOLVED " + std::to_string(n_unresolved) + unresolved : std::string();
      if (added == 0) return "NOTHING" + tail;
      N.version = 2;
      N.epoch = P.epoch + 1;  // so running nodes adopt it (they adopt by epoch)
      const std::string r = cas(ms_, sn.version, N);
      note("backfill " + std::to_string(added) + " arrivals at epoch " + std::to_string(N.epoch) + ": " + r);
      if (r == "OK") {
        if (push_refresh) {
          for (const PlacementNode& nd : N.nodes) {
            if (nd.admin_port) ask_node(Node{nd.host, nd.admin_port}, "MOVE REFRESH");
          }
        }
        return "BACKFILLED " + std::to_string(added) + " at epoch " + std::to_string(N.epoch) + tail;
      }
      why = "placement changed: " + r;  // retried from a fresh read
    }
    return "STUCK " + why;
  }

 private:
  struct Node {
    std::string host;
    unsigned port = 0;
  };

  static std::string hexid(u64 id) {
    char b[17];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)id);
    return b;
  }
  bool stop(const std::string& step) { return stop_at && stop_at(step); }
  void note(const std::string& s) {
    char t[32];
    std::snprintf(t, sizeof(t), "[%8.3f] ",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count());
    log += t + s + "\n";
    SNC_TRACE("driver %.200s", s.c_str());
  }
  const std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
  static void pause() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

  std::string ask_node(const Node& n, const std::string& line) {
    std::string reply;
    const u64 t0 = trace::enabled() ? trace::now_ns() : 0;
    const bool ok = ha::ask(n.host, n.port, line, reply, 15000);
    SNC_TRACE("driver_ask port=%u req=%.32s bytes=%zu ok=%d ms=%.1f reply_bytes=%zu reply=%.40s", n.port,
              line.c_str() + (line.rfind("MOVE ", 0) == 0 ? 5 : 0), line.size(), ok ? 1 : 0,
              trace::enabled() ? (trace::now_ns() - t0) / 1e6 : 0.0, reply.size(), reply.c_str());
    if (!ok) return "";
    return reply;
  }

  // Whichever member of `group` answers as primary now.
  bool primary(const Placement& P, u16 group, Node& out, std::string& why) {
    for (const PlacementMember& m : P.groups[group].members) {
      const PlacementNode* nd = P.node(m.node);
      if (nd == nullptr || nd->admin_port == 0) continue;
      Node n{nd->host, nd->admin_port};
      const std::string r = ask_node(n, "MOVE ROLE");
      if (r.rfind("ROLE p", 0) == 0) {
        out = n;
        return true;
      }
    }
    why = "no primary answers in group " + std::to_string(group);
    return false;
  }

  bool info(const Node& n, u16 slot, SlotReport& r) {
    const std::string a = ask_node(n, "MOVE INFO " + std::to_string(slot));
    std::istringstream in(a);
    std::string tag;
    return (in >> tag) && tag == "INFO" && parse_report(in, r);
  }

  // The commit gate (docs/MIGRATION_FAIL_CLOSED.md 5b), at placement STAGED,
  // before the commit: every source member that answers is asked. OK only
  // if none is promoting (ROLE m: its start barrier may have read the
  // placement before STAGED), at least one is a primary, and no primary
  // serves the slot. A member that does not answer is not seen: a second
  // primary the driver cannot reach is the HA problem (R08), not closed here.
  enum class Gate { OK, WAIT, SERVING };
  Gate gate(const Placement& P, const PlacementMove& m, std::string& why) {
    int primaries = 0;
    for (const PlacementMember& mb : P.groups[m.from].members) {
      const PlacementNode* nd = P.node(mb.node);
      if (nd == nullptr || nd->admin_port == 0) continue;
      const Node n{nd->host, nd->admin_port};
      const std::string r = ask_node(n, "MOVE ROLE");
      if (r.rfind("ROLE m", 0) == 0) {
        why = "commit gate: source node " + std::to_string(mb.node) + " is promoting";
        return Gate::WAIT;
      }
      if (r.rfind("ROLE p", 0) != 0) continue;
      SlotReport rep;
      if (!info(n, m.slot, rep)) {
        why = "commit gate: no INFO from source node " + std::to_string(mb.node);
        return Gate::WAIT;
      }
      if (rep.serving) {
        why = "commit gate: source node " + std::to_string(mb.node) + " serves the slot";
        note(why);
        return Gate::SERVING;
      }
      ++primaries;
    }
    if (primaries == 0) {
      why = "commit gate: no source primary answers";
      return Gate::WAIT;
    }
    return Gate::OK;
  }

  // Aborts the move in progress (before its commit): the move leaves the
  // placement; the next pass of move() unseals the owner (settle_owner).
  std::string abort_move(const Snapshot& sn, const std::string& reason) {
    Placement N = sn.p;
    N.move = PlacementMove{};
    const std::string r = cas(ms_, sn.version, N);
    note("abort move " + hexid(sn.p.move.id) + " (" + reason + "): " + r);
    if (r == "OK") aborted_ = reason;
    return "next";
  }
  int gate_serving_ = 0;
  int version_conflicts_ = 0;
  std::string gate_wait_;  // the last reason the gate gave to wait
  std::string aborted_;  // why this driver aborted the move, if it did

  bool have(const Node& dst, u16 slot, std::vector<SlotObject>& v, std::string& why) {
    const std::string r = ask_node(dst, "MOVE HAVE " + std::to_string(slot));
    std::istringstream in(r);
    std::string tag;
    if (!(in >> tag) || tag != "HAVE" || !decode_list(in, v)) {
      why = "destination HAVE: " + (r.empty() ? std::string("no answer") : r);
      return false;
    }
    return true;
  }

  bool export_from(const Node& src, u16 slot, u64 move, bool final, const std::vector<SlotObject>& have,
                   std::vector<SlotObject>& out, u64& count, u64& digest, std::string& why) {
    std::vector<SlotObject> bare;
    for (const SlotObject& h : have) {
      SlotObject b = h;
      b.has_payload = false;
      b.payload.clear();
      bare.push_back(std::move(b));
    }
    const std::string r = ask_node(src, "MOVE EXPORT " + std::to_string(slot) + " " + hexid(move) + " " +
                                            (final ? "1 " : "0 ") + encode_list(bare));
    std::istringstream in(r);
    std::string tag;
    if (!(in >> tag) || tag != "OBJS" || !(in >> count >> digest) || !decode_list(in, out)) {
      why = "source EXPORT: " + (r.empty() ? std::string("no answer") : r);
      return false;
    }
    return true;
  }

  std::string stage_at(const Node& dst, u16 slot, const PlacementMove& m, bool final, u64 count, u64 digest,
                        const std::vector<SlotObject>& objs) {
    return ask_node(dst, "MOVE STAGE " + std::to_string(slot) + " " + hexid(m.id) + " " + std::to_string(m.from) +
                             " " + std::to_string(m.epoch) + " " + (final ? "1 " : "0 ") + std::to_string(count) +
                             " " + std::to_string(digest) + " " + encode_list(objs));
  }

  std::string advance(const Snapshot& sn, PlacementMove::State to) {
    Placement N = sn.p;
    N.move.state = to;
    const std::string r = cas(ms_, sn.version, N);
    note(std::string("move ") + hexid(sn.p.move.id) + " -> " + PlacementMove::name(to) + ": " + r);
    return r;
  }

  // Copy ahead of the seal (the slot still takes writes), then seal.
  std::string step_prepared(const Snapshot& sn, std::string& why) {
    const PlacementMove& m = sn.p.move;
    Node src, dst;
    if (!primary(sn.p, m.from, src, why) || !primary(sn.p, m.to, dst, why)) return "retry";
    std::vector<SlotObject> h, objs;
    u64 count = 0, digest = 0;
    if (!have(dst, m.slot, h, why) || !export_from(src, m.slot, m.id, false, h, objs, count, digest, why)) {
      return "retry";
    }
    std::vector<SlotObject> send;
    for (SlotObject& o : objs) {
      if (o.has_payload) send.push_back(std::move(o));
    }
    const std::size_t step = static_cast<std::size_t>(std::max(1, chunk));
    for (std::size_t at = 0; at < send.size(); at += step) {
      std::vector<SlotObject> part(send.begin() + static_cast<std::ptrdiff_t>(at),
                                   send.begin() + static_cast<std::ptrdiff_t>(std::min(send.size(), at + step)));
      const std::string r = stage_at(dst, m.slot, m, false, 0, 0, part);
      if (r.rfind("OK", 0) != 0) {
        why = "destination STAGE: " + (r.empty() ? std::string("no answer") : r);
        return "retry";
      }
      if (at < send.size() / 2 && at + step >= send.size() / 2 && stop("copying")) return "STOPPED copying";
    }
    note("copied " + std::to_string(send.size()) + " objects ahead of the seal");
    if (stop("copied")) return "STOPPED copied";
    const std::string r = ask_node(src, "MOVE SEAL " + std::to_string(m.slot) + " " + hexid(m.id) + " " +
                                            std::to_string(m.to) + " " + std::to_string(m.epoch));
    note("seal: " + r);
    if (r.rfind("OK", 0) != 0) {
      why = "source SEAL: " + (r.empty() ? std::string("no answer") : r);
      return "retry";
    }
    if (advance(sn, PlacementMove::State::SEALED) == "OK" && stop("sealed")) return "STOPPED sealed";
    return "next";
  }

  // The sealed state, final: the destination's copy made equal to it.
  std::string step_sealed(const Snapshot& sn, std::string& why) {
    const PlacementMove& m = sn.p.move;
    Node src, dst;
    if (!primary(sn.p, m.from, src, why) || !primary(sn.p, m.to, dst, why)) return "retry";
    // The destination already holds a final copy of this move's sealed
    // source (its STAGED, durable): it came from a sealed source, which
    // changes no more, so it stays the move's image even if the source's
    // primary has since been replaced by one with an older prefix
    // (docs/MIGRATION_FAIL_CLOSED.md 5a, "What the re-sealed Q_A means").
    SlotReport d;
    if (info(dst, m.slot, d) && d.phase == 3 && d.move_id == m.id && d.durable) {
      note("destination already staged for move " + hexid(m.id));
      if (advance(sn, PlacementMove::State::STAGED) == "OK" && stop("staged")) return "STOPPED staged";
      return "next";
    }
    // No seal for this move at the source's primary (a promoted standby
    // whose prefix lacks it, and that read the placement before the seal):
    // no final export can ever be made there. Abort (5c), do not loop.
    SlotReport s;
    if (info(src, m.slot, s) && !(s.phase == 2 && s.move_id == m.id)) {
      return abort_move(sn, "the source's primary holds no seal for this move");
    }
    std::vector<SlotObject> h, objs;
    u64 count = 0, digest = 0;
    if (!have(dst, m.slot, h, why) || !export_from(src, m.slot, m.id, true, h, objs, count, digest, why)) {
      return "retry";
    }
    const std::string r = stage_at(dst, m.slot, m, true, count, digest, objs);
    note("final copy (" + std::to_string(count) + " objects): " + r.substr(0, r.find(' ', 3)));
    if (r.rfind("NO E_VERSION_CONFLICT", 0) == 0) {
      // Either a race (another driver's copy, a retried request: a fresh
      // HAVE settles it) or the destination holds newer versions than this
      // sealed source (its primary lost records it had copied ahead, a
      // lossy promotion), which no retry settles. Abort only when it
      // persists.
      if (++version_conflicts_ < version_conflict_limit) {
        why = "final stage: E_VERSION_CONFLICT (retrying)";
        return "retry";
      }
      return abort_move(sn, "the source's sealed image is older than the destination's copy");
    }
    if (r.rfind("OK", 0) != 0) {
      why = "destination STAGE final: " + (r.empty() ? std::string("no answer") : r);
      return "retry";
    }
    if (advance(sn, PlacementMove::State::STAGED) == "OK" && stop("staged")) return "STOPPED staged";
    return "next";
  }

  // The commit: the slot is the destination's, at the move's epoch.
  std::string step_staged(const Snapshot& sn, std::string& why) {
    const PlacementMove& m = sn.p.move;
    if (m.epoch != sn.p.epoch + 1) {
      why = "move epoch " + std::to_string(m.epoch) + " is not placement epoch + 1";
      return "retry";
    }
    // The commit gate (docs/MIGRATION_FAIL_CLOSED.md 5b).
    const Gate g = gate(sn.p, m, why);
    if (g == Gate::WAIT) {
      if (why != gate_wait_) note(why + " (waiting)");  // once per reason
      gate_wait_ = why;
      return "retry";
    }
    if (g == Gate::SERVING) {
      if (++gate_serving_ < gate_serving_limit) return "retry";
      return abort_move(sn, why);
    }
    Placement N = sn.p;
    N.slots[m.slot] = static_cast<u8>(m.to);
    N.epoch = m.epoch;
    N.move.state = PlacementMove::State::COMMITTED;
    if (N.version >= 2 || placement_v2) {
      N.version = 2;
      N.arrived[m.slot] = SlotArrival{m.id, m.epoch};  // in the same CAS as the commit
    }
    const std::string r = cas(ms_, sn.version, N);
    note("commit move " + hexid(m.id) + " at epoch " + std::to_string(m.epoch) + ": " + r);
    if (r == "OK" && stop("committed")) return "STOPPED committed";
    return "next";
  }

  // Activation at the destination, then the move leaves the placement.
  std::string step_committed(const Snapshot& sn, std::string& why) {
    const PlacementMove& m = sn.p.move;
    Node dst;
    if (!primary(sn.p, m.to, dst, why)) return "retry";
    const std::string r = ask_node(dst, "MOVE ACTIVATE " + std::to_string(m.slot));
    if (r.rfind("OK", 0) != 0) {
      why = "destination ACTIVATE: " + (r.empty() ? std::string("no answer") : r);
      // Committed, but the destination's primary holds no evidence of this
      // move (a lossy promotion there): nothing a driver does can activate
      // it (repair is a later slice). The move record is cleared, so other
      // moves are not blocked: the arrival (M) stays in the placement and
      // the source keeps SEALED(M), which is what a repair needs; with no
      // move on record nothing unseals the source (it is not the owner).
      if (r.find("proof=missing") != std::string::npos) {
        Placement N = sn.p;
        N.move = PlacementMove{};
        const std::string c = cas(ms_, sn.version, N);
        note("clear committed move " + hexid(m.id) + " (destination unproven): " + c);
        if (c != "OK") return "retry";
        return "STUCK the destination holds no evidence of move " + hexid(m.id) +
               " (proof=missing): slot " + std::to_string(m.slot) + " stays unavailable until repaired";
      }
      return "retry";
    }
    note("activated at group " + std::to_string(m.to));
    if (stop("activated")) return "STOPPED activated";
    if (push_refresh) {
      for (const PlacementNode& n : sn.p.nodes) {
        if (n.admin_port) ask_node(Node{n.host, n.admin_port}, "MOVE REFRESH");
      }
    }
    Placement N = sn.p;
    N.move = PlacementMove{};
    const std::string c = cas(ms_, sn.version, N);
    note("move " + hexid(m.id) + " done: " + c);
    return "next";
  }

  // No move of the slot: its owner must not keep a seal (an abort whose
  // driver died before it unsealed, or a seal that arrived late).
  void settle_owner(const Placement& P, u16 slot, std::chrono::steady_clock::time_point end) {
    std::string why;
    while (std::chrono::steady_clock::now() < end) {
      Node n;
      if (primary(P, P.slots[slot], n, why)) {
        const std::string r = ask_node(n, "MOVE UNSEAL " + std::to_string(slot));
        if (r.rfind("OK", 0) == 0) return;
        note("unseal at the owner: " + r);
      }
      pause();
    }
  }

  control::Members ms_;
};

}  // namespace snc::migrate
