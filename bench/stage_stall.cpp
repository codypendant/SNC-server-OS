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
//
// S1 (docs/MIGRATION_STAGING.md 3.2): --units on|off (the chain, or the
// whole request in one task), --unit-us N, --unit-bytes N, --window-bytes
// N. During the final stage a probe thread also runs a durable deposit on
// an account the destination shard serves (another slot) every 2 ms, and
// a status thread calls replication_status every 5 ms; both report
// p50/p95/p99/max. The deposit's latency is how long an ordinary client
// op on that shard waited behind the staging work. One "result" line.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
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
  bool units = true;
  u32 unit_us = 1000;
  u64 unit_bytes = 1u << 20, window_bytes = 8u << 20;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--objects") n = std::strtoull(v.c_str(), nullptr, 10);
    else if (k == "--changed") changed = std::strtoull(v.c_str(), nullptr, 10);
    else if (k == "--email") shared = v == "shared";
    else if (k == "--units") units = v == "on";
    else if (k == "--unit-us") unit_us = static_cast<u32>(std::strtoul(v.c_str(), nullptr, 10));
    else if (k == "--unit-bytes") unit_bytes = std::strtoull(v.c_str(), nullptr, 10);
    else if (k == "--window-bytes") window_bytes = std::strtoull(v.c_str(), nullptr, 10);
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
  c.move_units = units;
  c.move_unit_us = unit_us;
  c.move_unit_bytes = unit_bytes;
  c.move_window_bytes = window_bytes;
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
  // An account the destination's shard serves (minted by that shard, so
  // it owns it: another slot of group 0's), for the probe.
  struct Probe {
    std::atomic<bool> done{false};
    ObjectID id{};
    Status st = Status::OK;
  };
  std::atomic<int> owner{-1};
  // Held until the prober has finished (S3.3: data-plane work only under a lease).
  Engine::DataPlane dp = eng.data_plane();
  if (!dp) {
    std::fprintf(stderr, "engine not running\n");
    return 1;
  }
  dp.submit(0, [&] { owner = snc::owner_of(v1[0].id); });
  while (owner.load() < 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
  const u16 sh = static_cast<u16>(owner.load());
  Probe made;
  CreateArgs ca{};
  ca.created_ns = 1;
  ca.email_len = 1;
  ca.email[0] = 'p';
  ca.name_len = 1;
  ca.name[0] = 'p';
  dp.submit(sh, [&] {
    snc::create(kTypeId, ByteView{reinterpret_cast<const u8*>(&ca), sizeof(ca)}, ExecOptions{},
                [](void* ctx, const ExecResult& r) {
                  auto* p = static_cast<Probe*>(ctx);
                  p->id = r.id;
                  p->st = r.status;
                  p->done = true;
                },
                &made);
  });
  while (!made.done.load()) std::this_thread::sleep_for(std::chrono::microseconds(100));
  if (made.st != Status::OK) {
    std::fprintf(stderr, "probe account: %s\n", status_name(made.st));
    return 1;
  }
  SlotReport rep;
  auto t0 = clk::now();
  Status st = eng.stage_slot(slot, 1, 1, eng.cluster().epoch + 1, false, v1, 0, 0, rep, 60);
  std::printf("copy ahead: %llu accounts in one stage (not final): %.0f ms, %s\n", (unsigned long long)n,
              ms_since(t0), status_name(st));
  std::atomic<bool> done{false};
  std::vector<double> status_ms, op_ms;
  std::thread poller([&] {
    while (!done.load()) {
      auto p = clk::now();
      (void)eng.replication_status();
      status_ms.push_back(ms_since(p));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  std::thread prober([&] {
    AmountArgs aa{1, 1};
    while (!done.load()) {
      Probe p;
      const auto t = clk::now();
      dp.submit(sh, [&] {
        snc::execute(made.id, kOpDeposit, ByteView{reinterpret_cast<const u8*>(&aa), sizeof(aa)}, ExecOptions{},
                     [](void* ctx, const ExecResult& r) {
                       auto* q = static_cast<Probe*>(ctx);
                       q->st = r.status;
                       q->done = true;
                     },
                     &p);
      });
      while (!p.done.load()) std::this_thread::sleep_for(std::chrono::microseconds(50));
      op_ms.push_back(ms_since(t));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  t0 = clk::now();
  st = eng.stage_slot(slot, 1, 1, eng.cluster().epoch + 1, true, v2, count, digest, rep, 60);
  const double stage_ms = ms_since(t0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  done = true;
  poller.join();
  prober.join();
  dp.release();  // before stop(): stop waits for every lease
  auto pct = [](std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()))];
  };
  std::printf("final stage: %llu accounts, %llu changed, email %s, units %s: %.0f ms, %s\n", (unsigned long long)n,
              (unsigned long long)changed, shared ? "shared" : "unique", units ? "on" : "off", stage_ms,
              status_name(st));
  std::printf("result objects=%llu changed=%llu email=%s units=%s unit_us=%u unit_bytes=%llu window_bytes=%llu "
              "final_ms=%.1f status_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f n=%zu op_ms p50=%.2f p95=%.2f p99=%.2f "
              "max=%.2f n=%zu\n",
              (unsigned long long)n, (unsigned long long)changed, shared ? "shared" : "unique", units ? "on" : "off",
              unit_us, (unsigned long long)unit_bytes, (unsigned long long)window_bytes, stage_ms, pct(status_ms, 0.5),
              pct(status_ms, 0.95), pct(status_ms, 0.99), pct(status_ms, 1.0), status_ms.size(), pct(op_ms, 0.5),
              pct(op_ms, 0.95), pct(op_ms, 0.99), pct(op_ms, 1.0), op_ms.size());
  std::printf("chain: %s\n", eng.last_stage_report().c_str());
  eng.stop();
  std::string rm = "rm -rf " + dir;
  return std::system(rm.c_str()) == 0 && st == Status::OK ? 0 : 1;
}
