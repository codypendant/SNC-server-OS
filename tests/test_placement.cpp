// Placement v2 (docs/MIGRATION_FAIL_CLOSED.md 7, 15; PR #39 review items 5,
// 14 and added test B): the text form's compatibility rules, and its size
// and cost with every slot carrying an arrival (the worst case).
//   - v1 round-trips unchanged, and a v1 placement never carries arrivals
//     (encode refuses: v2 is never silently written as v1);
//   - v2 round-trips its arrivals exactly, sparse, in slot order;
//   - decode refuses malformed arrivals: out of order or repeated slots, a
//     zero move or epoch, an epoch ahead of the placement's, arrivals in a
//     v1 text;
//   - a copy (the driver's rewrite: N = P, then its changes) keeps v2 and
//     every arrival.
//   - witnesses keep a v2 body opaque: what a CAS wrote is what SYNC and
//     GET return, byte for byte (with --cas).
// Usage: test_placement [--measure]   (prints the size and timings)
//        test_placement --cas HOST:PORT,...   (fresh witnesses: CAS latency
//            and the round trip of the worst-case v2 placement)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "../tools/control.h"
#include "../tools/migrate.h"
#include "check.h"
#include "snc/cluster.h"
#include "snc/placement.h"

using namespace snc;
using clk = std::chrono::steady_clock;

namespace {

Placement base() {
  std::vector<PlacementNode> nodes = {{1, "10.0.0.1", 7000}, {2, "10.0.0.2", 7000}, {3, "10.0.0.3", 7000},
                                      {4, "10.0.0.4", 7000}};
  std::vector<PlacementGroup> groups(2);
  groups[0].n_shards = 4;
  groups[0].members = {{1, 7100, 7200}, {2, 7100, 7200}};
  groups[1].n_shards = 4;
  groups[1].members = {{3, 7100, 7200}, {4, 7100, 7200}};
  Uuid u{};
  for (u32 i = 0; i < 16; ++i) u[i] = static_cast<u8>(i + 1);
  return Placement::initial(u, nodes, groups);
}

double us_since(clk::time_point t) { return std::chrono::duration<double, std::micro>(clk::now() - t).count(); }

}  // namespace

// Against fresh witnesses: init v2, then CAS the worst case (every slot an
// arrival) repeatedly, timing CAS and SYNC; every read returns the body
// exactly as written.
int cas_bench(const std::string& members) {
  control::Members ms;
  CHECK(control::parse_members(members, ms));
  Placement p = base();
  p.version = 2;
  CHECK_EQ(control::init(ms, p), std::string("OK"));
  u64 ver = 0;
  Placement cur;
  std::string why;
  CHECK(control::sync(ms, ver, cur, why));
  CHECK_EQ(cur.encode(), p.encode());
  auto run = [&](u32 arrivals, int reps) {
    double cas_us = 0, sync_us = 0;
    std::size_t bytes = 0;
    for (int i = 0; i < reps; ++i) {
      Placement n = cur;
      n.epoch = cur.epoch + 1;
      for (u32 s = 0; s < arrivals; ++s) n.arrived[s] = {0xf000000000000000ull + s + n.epoch, n.epoch};
      bytes = n.encode().size();
      auto t0 = clk::now();
      const std::string r = migrate::cas(ms, ver, n);
      cas_us += us_since(t0);
      CHECK_EQ(r, std::string("OK"));
      t0 = clk::now();
      CHECK(control::sync(ms, ver, cur, why));
      sync_us += us_since(t0);
      CHECK_EQ(cur.encode(), n.encode());  // opaque: byte for byte
      Placement g;
      CHECK(control::get(ms, g, why) == control::Got::MAP);
      CHECK(g.arrived == n.arrived && g.version == 2);
    }
    std::printf("CAS with %u arrivals (%zu bytes): CAS %.2f ms, SYNC %.2f ms (mean of %d)\n", arrivals, bytes,
                cas_us / reps / 1000, sync_us / reps / 1000, reps);
  };
  run(0, 20);
  run(256, 20);
  run(kSlots, 20);
  std::printf("test_placement --cas: ok\n");
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 2 && std::strcmp(argv[1], "--cas") == 0) return cas_bench(argv[2]);
  const bool measure = argc > 1 && std::strcmp(argv[1], "--measure") == 0;

  // v1: as before; no arrivals; v1 never carries one.
  Placement p = base();
  CHECK_EQ(p.version, 1);
  const std::string t1 = p.encode();
  CHECK(t1.rfind("v1 ", 0) == 0);
  CHECK(t1.find("arrived") == std::string::npos);
  auto d1 = Placement::decode(t1);
  CHECK(d1.has_value());
  CHECK_EQ(d1->version, 1);
  CHECK_EQ(d1->encode(), t1);
  Placement bad = p;
  bad.epoch = 5;
  bad.arrived[7] = {0x99, 3};
  bool threw = false;
  try {
    (void)bad.encode();
  } catch (const std::logic_error&) {
    threw = true;
  }
  CHECK(threw);  // a writer cannot drop arrivals by writing v1

  // v2: sparse arrivals in slot order, round-tripped exactly.
  Placement v = base();
  v.version = 2;
  v.epoch = 9;
  v.arrived[3] = {0x1a2b, 4};
  v.arrived[4095] = {0xffffffffffffffffull, 9};
  v.arrived[100] = {0x7, 2};
  const std::string t2 = v.encode();
  CHECK(t2.rfind("v2 ", 0) == 0);
  CHECK(t2.find(" arrived=3:1a2b:4,100:7:2,4095:ffffffffffffffff:9") != std::string::npos);
  auto d2 = Placement::decode(t2);
  CHECK(d2.has_value());
  CHECK_EQ(d2->version, 2);
  CHECK(d2->arrived == v.arrived);
  CHECK_EQ(d2->encode(), t2);
  // v2 with no arrival yet stays v2 (explicitly enabled, never downgraded).
  Placement v0 = base();
  v0.version = 2;
  auto d0 = Placement::decode(v0.encode());
  CHECK(d0.has_value() && d0->version == 2);

  // The driver's rewrite (a copy, then its own changes) keeps both.
  Placement n = *d2;
  n.epoch = 10;
  n.slots[5] = 1;
  n.arrived[5] = {0x2000, 10};
  auto dn = Placement::decode(n.encode());
  CHECK(dn.has_value() && dn->version == 2);
  CHECK(dn->arrived[3] == v.arrived[3] && dn->arrived[4095] == v.arrived[4095] && dn->arrived[100] == v.arrived[100]);
  CHECK(dn->arrived[5] == (SlotArrival{0x2000, 10}));
  // ClusterMap carries them (not in its crc: arrivals prove, not assign).
  ClusterMap m = dn->map();
  CHECK(m.arrived == dn->arrived);
  ClusterMap m2 = m;
  m2.arrived[3] = {};
  CHECK_EQ(m.crc(), m2.crc());

  // Malformed arrivals are refused, never half read.
  auto refuses = [&](const std::string& from, const std::string& to) {
    std::string t = t2;
    const auto at = t.find(from);
    CHECK(at != std::string::npos);
    t.replace(at, from.size(), to);
    return !Placement::decode(t).has_value();
  };
  CHECK(refuses("arrived=3:1a2b:4,100:7:2", "arrived=100:7:2,3:1a2b:4"));    // order
  CHECK(refuses("arrived=3:1a2b:4,100:7:2", "arrived=3:1a2b:4,3:7:2"));      // repeated
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:0:4"));                       // move 0
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:1a2b:0"));                    // epoch 0
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:1a2b:10"));                   // ahead of epoch 9
  CHECK(refuses("arrived=3:1a2b:4", "arrived=4096:1a2b:4"));                 // no such slot
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:1A2B:4"));                    // not canonical hex
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:11112222333344445:4"));       // 17 hex digits
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:1a2b:4 arrived=5:7:2"));     // the key twice
  CHECK(refuses("arrived=3:1a2b:4", "arrived=03:1a2b:4"));                   // leading zeros
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:01a2b:4"));
  CHECK(refuses("arrived=3:1a2b:4", "arrived=3:1a2b:04"));
  CHECK(refuses("v2 ", "v1 "));                                              // arrivals in v1
  CHECK(refuses("v2 ", "v3 "));                                              // unknown version

  // The worst case: every slot moved, the largest move IDs and epochs.
  Placement w = base();
  w.version = 2;
  w.epoch = 1ull << 40;
  for (u32 s = 0; s < kSlots; ++s) w.arrived[s] = {0xf000000000000000ull + s, (1ull << 40) - s};
  const int reps = measure ? 200 : 5;
  std::string tw;
  auto t0 = clk::now();
  for (int i = 0; i < reps; ++i) tw = w.encode();
  const double enc_us = us_since(t0) / reps;
  std::optional<Placement> dw;
  t0 = clk::now();
  for (int i = 0; i < reps; ++i) dw = Placement::decode(tw);
  const double dec_us = us_since(t0) / reps;
  CHECK(dw.has_value() && dw->arrived == w.arrived);
  t0 = clk::now();
  ClusterMap mw;
  for (int i = 0; i < reps; ++i) mw = dw->map();
  const double map_us = us_since(t0) / reps;
  CHECK(mw.arrived == w.arrived);
  if (measure) {
    std::printf("placement v1 (2 groups, 4 nodes): %zu bytes\n", t1.size());
    std::printf("placement v2, all %u slots with an arrival: %zu bytes (%.1f per arrival)\n", kSlots, tw.size(),
                double(tw.size() - t1.size()) / kSlots);
    std::printf("  encode %.0f us, decode %.0f us, to ClusterMap %.0f us (mean of %d)\n", enc_us, dec_us, map_us, reps);
    std::printf("  memory: arrivals %zu bytes per Placement and per ClusterMap\n", sizeof(w.arrived));
  }
  std::printf("test_placement: ok\n");
  return 0;
}
