// Reproduces, in one process and without the lab, the mechanism behind the
// 100K false failover (docs/STABILIZATION_REPORT.md, report D): one final
// stage_slot of N accounts runs as a single task on the destination's
// shard thread, and Engine::replication_status (what snc_server's STATUS
// answers from, for the witnesses) waits for every shard.
//
//   stage_stall [--objects N] [--changed M] [--email shared|unique]
//
// One clustered engine (group 0 of 2). A slot S of group 1's is staged
// here (not final) with N accounts at version 1, then staged final with M
// of them at version 2: M put_image calls on existing objects, each of
// which updates the Account.email index. With --email shared every
// account has the same email (as snc_load and the lab create them), so
// the index has one posting list of N entries. Meanwhile a thread calls
// replication_status every 5 ms and reports the slowest answer.
//
// Prints: final stage ms, slowest replication_status ms. Timing, so not a
// ctest: it is evidence for the mechanism, and the baseline any bounded
// final stage must beat.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "snc/cluster.h"
#include "snc/slot_map.h"
#include "snc/snc.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;
using clk = std::chrono::steady_clock;

namespace {

u32 crc_of(u32 type_id, u16 schema_ver, const std::vector<u8>& p) {
  u8 head[6];
  std::memcpy(head, &type_id, 4);
  std::memcpy(head + 4, &schema_ver, 2);
  return crc32c(p.data(), p.size(), crc32c(head, sizeof(head)));
}

double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

}  // namespace

int main(int argc, char** argv) {
  u64 n = 100000, changed = 20000;
  bool shared = true;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--objects") n = std::strtoull(v.c_str(), nullptr, 10);
    else if (k == "--changed") changed = std::strtoull(v.c_str(), nullptr, 10);
    else if (k == "--email") shared = v == "shared";
  }
  if (changed > n) changed = n;
  register_type();
  char tmpl[] = "/tmp/snc-stall-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  Engine::Config c;
  c.runtime.n_shards = 2;
  c.expected_objects = n * 2 + 1024;
  c.data_dir = dir;
  c.init = true;
  for (u16 g = 0; g < 2; ++g) {
    ClusterGroup grp;
    grp.n_shards = 2;
    grp.members.push_back({"127.0.0.1", static_cast<u16>(29000 + g * 10), static_cast<u16>(29005 + g * 10)});
    c.cluster.push_back(grp);
  }
  c.group_id = 0;
  Engine eng(c);
  eng.start();
  // A slot of group 1's (foreign here), and N IDs in it.
  u16 slot = 0;
  while (eng.cluster().group_of[slot] != 1) ++slot;
  const TypeDescriptor* t = registry().type(kTypeId);
  std::vector<SlotObject> v1, v2;
  for (u64 seq = 1; v1.size() < n; ++seq) {
    const ObjectID id = ObjectID::make(0x7777, seq);
    if (slot_of(id) != slot) continue;
    AccountV1 a{};
    a.balance_cents = 1000;
    a.created_ns = 1;
    const std::string email = shared ? "e" : "u" + std::to_string(seq);
    a.email_len = static_cast<u8>(email.size());
    std::memcpy(a.email, email.data(), email.size());
    a.name_len = 1;
    a.name[0] = 'n';
    SlotObject o;
    o.id = id;
    o.version = 1;
    o.type_id = kTypeId;
    o.schema_ver = t->schema_ver;
    o.has_payload = true;
    o.payload.resize(sizeof(a));
    std::memcpy(o.payload.data(), &a, sizeof(a));
    o.crc = crc_of(o.type_id, o.schema_ver, o.payload);
    v1.push_back(o);
  }
  u64 count = 0, digest = 0;
  for (u64 i = 0; i < n; ++i) {
    SlotObject o = v1[i];
    if (i < changed) {
      AccountV1 a;
      std::memcpy(&a, o.payload.data(), sizeof(a));
      a.balance_cents += 1;
      std::memcpy(o.payload.data(), &a, sizeof(a));
      o.version = 2;
      o.crc = crc_of(o.type_id, o.schema_ver, o.payload);
    }
    ++count;
    digest += slot_object_digest(o.id, o.version, o.crc);
    v2.push_back(std::move(o));
  }
  SlotReport rep;
  auto t0 = clk::now();
  Status st = eng.stage_slot(slot, 1, 1, eng.cluster().epoch + 1, false, v1, 0, 0, rep, 60);
  std::printf("copy ahead: %llu accounts in one stage (not final): %.0f ms, %s\n", (unsigned long long)n,
              ms_since(t0), status_name(st));
  std::atomic<bool> done{false};
  double worst = 0;
  u64 polls = 0;
  std::thread poller([&] {
    while (!done.load()) {
      auto p = clk::now();
      (void)eng.replication_status();
      worst = std::max(worst, ms_since(p));
      ++polls;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  t0 = clk::now();
  st = eng.stage_slot(slot, 1, 1, eng.cluster().epoch + 1, true, v2, count, digest, rep, 60);
  const double stage_ms = ms_since(t0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  done = true;
  poller.join();
  std::printf("final stage: %llu accounts, %llu changed, email %s: %.0f ms, %s; slowest replication_status "
              "during it %.0f ms (%llu polls)\n",
              (unsigned long long)n, (unsigned long long)changed, shared ? "shared" : "unique", stage_ms,
              status_name(st), worst, (unsigned long long)polls);
  eng.stop();
  std::string rm = "rm -rf " + dir;
  return std::system(rm.c_str()) == 0 && st == Status::OK ? 0 : 1;
}
