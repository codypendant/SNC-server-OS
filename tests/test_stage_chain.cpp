// S1 bounded destination staging (docs/MIGRATION_STAGING.md 3.2-3.7), in
// one process: one node of group 0 of a two-group cluster; a slot S of
// group 1's (foreign here) is staged through Engine::stage_slot, the
// chain (Config::move_units) and the whole-request task (off). The objects
// are made here, as a source's export would list them.
//
//   B  equivalence: seeded rounds of copy-ahead and final lists (changed,
//      unchanged, without payload, leftover objects the final drops, new
//      ones) give the same objects, versions, CRCs, slot state, count and
//      digest with the chain (one object per unit) as with one task;
//   A  the bound: a final stage of 20K accounts with a shared email runs in
//      many units, none much longer than the unit budget;
//   W  the window: with a 1-byte window every put waits for durability;
//      what the chain appended and not yet durable stays within one record;
//   I  a duplicate final while the chain runs: E_OVERLOADED, no work; the
//      chain finishes; the retry answers OK at once (STAGED);
//   SU a newer move's stage supersedes the chain: it stops E_MIGRATING and
//      records nothing as staged;
//   G  fencing between units (withdraw): the chain stops E_FENCED, no STAGED;
//   M  the map grows during the scan: the walk restarts, the result is exact;
//   R  durable progress across a restart that changes the mode: copy ahead
//      with the chain, restart with one task, final; and the reverse.
// Usage: test_stage_chain
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <tuple>

#include "check.h"
#include "snc/cluster.h"
#include "snc/snc.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;

namespace {

struct Opts {
  bool units = true;
  u32 unit_us = 1;
  u64 unit_bytes = 1;
  u64 window_bytes = 8u << 20;
  u64 expected = 4096;
};

Engine::Config config(const std::string& dir, const Opts& o) {
  Engine::Config c;
  c.runtime.n_shards = 2;
  c.runtime.pin = false;
  c.expected_objects = o.expected;
  c.data_dir = dir;
  c.init = true;
  c.move_units = o.units;
  c.move_unit_us = o.unit_us;
  c.move_unit_bytes = o.unit_bytes;
  c.move_window_bytes = o.window_bytes;
  for (u16 g = 0; g < 2; ++g) {
    ClusterGroup grp;
    grp.n_shards = 2;
    grp.members.push_back({"127.0.0.1", static_cast<u16>(1 + g), static_cast<u16>(3 + g)});
    c.cluster.push_back(grp);
  }
  c.group_id = 0;
  return c;
}

std::vector<std::string> made_dirs;  // removed at the end (each holds preallocated WAL segments)

std::string fresh_dir() {
  char tmpl[] = "/tmp/snc-chain-XXXXXX";
  CHECK(::mkdtemp(tmpl) != nullptr);
  made_dirs.push_back(tmpl);
  return tmpl;
}

u32 crc_of(const SlotObject& o) {
  u8 head[6];
  std::memcpy(head, &o.type_id, 4);
  std::memcpy(head + 4, &o.schema_ver, 2);
  return crc32c(o.payload.data(), o.payload.size(), crc32c(head, sizeof(head)));
}

// The n-th ID (from `seq`) whose slot is `slot`.
std::vector<ObjectID> ids_in(u16 slot, std::size_t n, u64 tag = 0x5151) {
  std::vector<ObjectID> v;
  for (u64 seq = 1; v.size() < n; ++seq) {
    const ObjectID id = ObjectID::make(tag, seq);
    if (slot_of(id) == slot) v.push_back(id);
  }
  return v;
}

SlotObject account(const ObjectID& id, u64 version, const std::string& email) {
  AccountV1 a{};
  a.balance_cents = static_cast<i64>(1000 + version);
  a.created_ns = 1;
  a.email_len = static_cast<u8>(email.size());
  std::memcpy(a.email, email.data(), email.size());
  a.name_len = 1;
  a.name[0] = 'n';
  SlotObject o;
  o.id = id;
  o.version = version;
  o.type_id = kTypeId;
  o.schema_ver = registry().type(kTypeId)->schema_ver;
  o.has_payload = true;
  o.payload.resize(sizeof(a));
  std::memcpy(o.payload.data(), &a, sizeof(a));
  o.crc = crc_of(o);
  return o;
}

void sums(const std::vector<SlotObject>& v, u64& count, u64& digest) {
  count = digest = 0;
  for (const SlotObject& o : v) {
    ++count;
    digest += slot_object_digest(o.id, o.version, o.crc);
  }
}

// (version, crc) of every object of `slot` here.
std::map<ObjectID, std::pair<u64, u32>> held(Engine& eng, u16 slot) {
  std::vector<SlotObject> v;
  CHECK_EQ(eng.slot_objects(slot, v), Status::OK);
  std::map<ObjectID, std::pair<u64, u32>> m;
  for (const SlotObject& o : v) m[o.id] = {o.version, o.crc};
  return m;
}

u16 foreign_slot(Engine& eng, u16 skip = kSlots) {
  for (u16 s = 0; s < kSlots; ++s) {
    if (eng.cluster().group_of[s] == 1 && s != skip) return s;
  }
  CHECK(false);
  return 0;
}

// A foreign slot other than `s` that the same shard owns.
u16 sibling_slot(Engine& eng, u16 s) {
  for (u16 t = 0; t < kSlots; ++t) {
    if (t != s && eng.cluster().group_of[t] == 1 && eng.slot_owner(t) == eng.slot_owner(s)) return t;
  }
  CHECK(false);
  return 0;
}

u64 field(const std::string& report, const std::string& key) {
  const std::size_t at = report.find(" " + key + "=");
  if (at == std::string::npos) return ~u64{0};
  return std::strtoull(report.c_str() + at + key.size() + 2, nullptr, 10);
}

double field_ms(const std::string& report, const std::string& key) {
  const std::size_t at = report.find(" " + key + "=");
  CHECK(at != std::string::npos);
  return std::strtod(report.c_str() + at + key.size() + 2, nullptr);
}

Status stage(Engine& eng, u16 slot, u64 move, bool final, const std::vector<SlotObject>& objs, SlotReport* out = nullptr) {
  u64 count = 0, digest = 0;
  sums(objs, count, digest);
  SlotReport r;
  const Status st = eng.stage_slot(slot, move, 1, eng.cluster().epoch + 1, final, objs, count, digest, r, 30);
  if (out) *out = r;
  return st;
}

// ---- B ----
void equivalence() {
  std::mt19937_64 rng(20261104);
  for (int round = 0; round < 6; ++round) {
    const std::size_t n = 150 + rng() % 200;
    // The two engines get the same requests.
    std::vector<std::vector<SlotObject>> ahead;  // non-final requests, in order
    std::vector<SlotObject> final_list;
    u16 slot = 0;
    {
      const std::string probe_dir = fresh_dir();
      Engine probe(config(probe_dir, Opts{}));
      slot = foreign_slot(probe, static_cast<u16>(round));
      probe.stop();
    }
    const std::vector<ObjectID> ids = ids_in(slot, n + 40, 0x1000 + round);
    std::vector<u64> version(ids.size(), 0);
    // Copy ahead: two requests of random subsets, the second with newer
    // versions of some; the last 40 IDs only ever ahead (leftovers: gone
    // at the source by the final).
    for (int req = 0; req < 2; ++req) {
      std::vector<SlotObject> part;
      for (std::size_t i = 0; i < ids.size(); ++i) {
        if (rng() % 3 == 0) continue;
        version[i] += 1 + rng() % 2;
        part.push_back(account(ids[i], version[i], (rng() % 2) ? "e" : "u" + std::to_string(i)));
      }
      ahead.push_back(part);
    }
    // What the receiver holds at the end of the copy ahead.
    std::map<ObjectID, SlotObject> dst;
    for (const auto& part : ahead) {
      for (const SlotObject& o : part) dst[o.id] = o;
    }
    // The final: the first n IDs; some unchanged since the copy ahead (sent
    // without payload, as the source does), some changed, some new.
    for (std::size_t i = 0; i < n; ++i) {
      auto it = dst.find(ids[i]);
      const int pick = static_cast<int>(rng() % 3);
      if (it != dst.end() && pick == 0) {
        SlotObject bare = it->second;
        bare.has_payload = false;
        bare.payload.clear();
        final_list.push_back(bare);
      } else {
        const u64 v = (it != dst.end() ? it->second.version : 0) + 1 + rng() % 3;
        final_list.push_back(account(ids[i], v, pick == 1 ? "e" : "f" + std::to_string(i)));
      }
    }
    std::map<ObjectID, std::pair<u64, u32>> result[2];
    SlotReport rep[2];
    std::string chain;
    for (int mode = 0; mode < 2; ++mode) {
      Opts o;
      o.units = mode == 1;
      o.window_bytes = (round % 2) ? 1 : 8u << 20;
      const std::string dir = fresh_dir();
      Engine eng(config(dir, o));
      eng.start();
      for (const auto& part : ahead) CHECK_EQ(stage(eng, slot, 7, false, part), Status::OK);
      CHECK_EQ(stage(eng, slot, 7, true, final_list, &rep[mode]), Status::OK);
      result[mode] = held(eng, slot);
      if (mode == 1) chain = eng.last_stage_report();
      eng.stop();
    }
    u64 count = 0, digest = 0;
    {
      std::vector<SlotObject> full;
      for (const SlotObject& o : final_list) full.push_back(o.has_payload ? o : dst[o.id]);
      sums(full, count, digest);
    }
    CHECK(result[0] == result[1]);
    CHECK_EQ(result[1].size(), n);
    for (int mode = 0; mode < 2; ++mode) {
      CHECK_EQ(rep[mode].phase, u8{3});
      CHECK_EQ(rep[mode].count, count);
      CHECK_EQ(rep[mode].digest, digest);
      CHECK_EQ(rep[mode].live_count, count);
      CHECK_EQ(rep[mode].live_digest, digest);
      CHECK(!rep[mode].serving);
    }
    CHECK(field(chain, "units") > n);  // one object per unit
    CHECK_EQ(field(chain, "dropped"), u64{40} - [&] {
      u64 never = 0;  // leftovers never sent ahead are not there to drop
      for (std::size_t i = n; i < ids.size(); ++i) never += dst.count(ids[i]) ? 0 : 1;
      return never;
    }());
    std::printf("B round %d: %zu objects, chain %llu units, dropped %llu, equal\n", round, n,
                (unsigned long long)field(chain, "units"), (unsigned long long)field(chain, "dropped"));
  }
}

// ---- A and W ----
void bound_and_window() {
  {
    Opts o;
    o.unit_us = 1000;
    o.unit_bytes = 1u << 20;
    o.expected = 50000;
    const std::string dir = fresh_dir();
    Engine eng(config(dir, o));
    eng.start();
    const u16 slot = foreign_slot(eng);
    const std::vector<ObjectID> ids = ids_in(slot, 20000);
    std::vector<SlotObject> v1, v2;
    for (const ObjectID& id : ids) v1.push_back(account(id, 1, "e"));
    for (std::size_t i = 0; i < ids.size(); ++i) v2.push_back(i % 5 == 0 ? account(ids[i], 2, "e") : v1[i]);
    CHECK_EQ(stage(eng, slot, 9, false, v1), Status::OK);
    CHECK_EQ(stage(eng, slot, 9, true, v2), Status::OK);
    const std::string r = eng.last_stage_report();
    std::printf("A: %s\n", r.c_str());
    CHECK(field(r, "units") > 10);
    // The bound is the budget plus one object; generous for sanitizer builds.
    CHECK(field_ms(r, "max") < 250.0);
    eng.stop();
  }
  {
    Opts o;
    o.unit_us = 1000;
    o.unit_bytes = 1u << 20;
    o.window_bytes = 1;
    const std::string dir = fresh_dir();
    Engine eng(config(dir, o));
    eng.start();
    const u16 slot = foreign_slot(eng);
    std::vector<SlotObject> v;
    for (const ObjectID& id : ids_in(slot, 300)) v.push_back(account(id, 1, "w"));
    CHECK_EQ(stage(eng, slot, 9, true, v), Status::OK);
    const std::string r = eng.last_stage_report();
    std::printf("W: %s\n", r.c_str());
    CHECK(field(r, "window_waits") >= 299);
    CHECK(field(r, "window_hw") < 4096);  // one record
    eng.stop();
  }
}

// A final stage of `n` objects on its own thread, held before put unit 2.
struct Held {
  std::thread t;
  std::atomic<int> st{-1};
  SlotReport rep;
  void start(Engine& eng, u16 slot, u64 move, const std::vector<SlotObject>& v, int phase = 2, u32 unit = 2,
             u32 hold_ms = 800) {
    eng.debug_stage_stop(phase, unit, false, hold_ms);
    t = std::thread([&eng, slot, move, &v, this] { st = static_cast<int>(stage(eng, slot, move, true, v, &rep)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // inside the hold
  }
  Status join() {
    t.join();
    return static_cast<Status>(st.load());
  }
};

void duplicate_supersede_fence() {
  // I
  {
    const std::string dir = fresh_dir();
    Engine eng(config(dir, Opts{}));
    eng.start();
    const u16 slot = foreign_slot(eng);
    std::vector<SlotObject> v;
    for (const ObjectID& id : ids_in(slot, 50)) v.push_back(account(id, 1, "d"));
    Held h;
    h.start(eng, slot, 0x21, v);
    CHECK_EQ(stage(eng, slot, 0x21, true, v), Status::E_OVERLOADED);
    CHECK(h.join() == Status::OK);
    CHECK_EQ(h.rep.phase, u8{3});
    SlotReport again;
    CHECK_EQ(stage(eng, slot, 0x21, true, v, &again), Status::OK);  // STAGED: at once
    CHECK_EQ(again.live_digest, h.rep.live_digest);
    CHECK_EQ(held(eng, slot).size(), 50u);
    std::printf("I: duplicate E_OVERLOADED, retry OK\n");
    eng.stop();
  }
  // SU: a newer move supersedes; an older one is refused.
  {
    const std::string dir = fresh_dir();
    Engine eng(config(dir, Opts{}));
    eng.start();
    const u16 slot = foreign_slot(eng);
    std::vector<SlotObject> v;
    for (const ObjectID& id : ids_in(slot, 50)) v.push_back(account(id, 1, "s"));
    Held h;
    h.start(eng, slot, 0x30, v);
    CHECK_EQ(stage(eng, slot, 0x20, false, {v[0]}), Status::E_OVERLOADED);  // older
    CHECK_EQ(stage(eng, slot, 0x40, false, {v[1]}), Status::OK);             // newer
    CHECK(h.join() == Status::E_MIGRATING);
    const SlotReport r = eng.slot_report(slot);
    CHECK(r.phase != 3);
    std::printf("SU: superseded E_MIGRATING, phase %u\n", r.phase);
    eng.stop();
  }
  // G
  {
    const std::string dir = fresh_dir();
    Engine eng(config(dir, Opts{}));
    eng.start();
    const u16 slot = foreign_slot(eng);
    std::vector<SlotObject> v;
    for (const ObjectID& id : ids_in(slot, 50)) v.push_back(account(id, 1, "g"));
    Held h;
    h.start(eng, slot, 0x50, v);
    eng.withdraw();
    CHECK(h.join() == Status::E_FENCED);
    CHECK(eng.slot_report(slot).phase != 3);
    const std::string r = eng.last_stage_report();
    CHECK(r.find("status=E_FENCED") != std::string::npos);
    CHECK(field(r, "put") <= 3);  // nothing after the fence
    std::printf("G: %s\n", r.c_str());
    eng.stop();
  }
}

// ---- M ----
void rehash_during_scan() {
  Opts o;
  o.expected = 1024;  // small maps: they grow
  const std::string dir = fresh_dir();
  Engine eng(config(dir, o));
  eng.start();
  const u16 slot = foreign_slot(eng);
  const u16 other = sibling_slot(eng, slot);
  std::vector<SlotObject> v, more;
  for (const ObjectID& id : ids_in(slot, 400)) v.push_back(account(id, 1, "m"));
  for (const ObjectID& id : ids_in(other, 6000, 0x9999)) more.push_back(account(id, 1, "o"));
  Held h;
  h.start(eng, slot, 0x60, v, 3 /* scan */, 3, 1200);
  CHECK_EQ(stage(eng, other, 0x61, false, more), Status::OK);  // the same shard's map grows
  CHECK(h.join() == Status::OK);
  const std::string r = eng.last_stage_report();
  std::printf("M: %s\n", r.c_str());
  CHECK(field(r, "scan_restarts") >= 1);
  u64 count = 0, digest = 0;
  sums(v, count, digest);
  CHECK_EQ(h.rep.live_count, count);
  CHECK_EQ(h.rep.live_digest, digest);
  CHECK_EQ(held(eng, slot).size(), 400u);
  eng.stop();
}

// ---- R ----
void restart_changes_mode() {
  for (int first = 0; first < 2; ++first) {
    const std::string dir = fresh_dir();
    u16 slot = 0;
    std::vector<SlotObject> v1, v2;
    {
      Opts o;
      o.units = first == 1;
      Engine eng(config(dir, o));
      eng.start();
      slot = foreign_slot(eng);
      const std::vector<ObjectID> ids = ids_in(slot, 200);
      for (const ObjectID& id : ids) v1.push_back(account(id, 1, "r"));
      for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i % 2) {
          SlotObject bare = v1[i];
          bare.has_payload = false;
          bare.payload.clear();
          v2.push_back(bare);
        } else {
          v2.push_back(account(ids[i], 3, "r"));
        }
      }
      CHECK_EQ(stage(eng, slot, 0x70, false, v1), Status::OK);
      eng.stop();
    }
    Opts o;
    o.units = first == 0;
    Engine::Config c = config(dir, o);
    c.init = false;
    Engine eng(c);
    eng.start();
    SlotReport r;
    CHECK_EQ(stage(eng, slot, 0x70, true, v2, &r), Status::OK);  // the durable copy ahead is reused
    CHECK_EQ(r.phase, u8{3});
    CHECK_EQ(r.live_count, 200u);
    std::printf("R: copy ahead with units %s, final with units %s: OK\n", first ? "on" : "off",
                first ? "off" : "on");
    eng.stop();
  }
}

}  // namespace

int main() {
  register_type();
  equivalence();
  bound_and_window();
  duplicate_supersede_fence();
  rehash_during_scan();
  restart_changes_mode();
  for (const std::string& d : made_dirs) std::filesystem::remove_all(d);
  std::printf("test_stage_chain: OK\n");
  return 0;
}
