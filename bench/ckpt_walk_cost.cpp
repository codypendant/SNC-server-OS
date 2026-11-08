// Checkpoint walk cost (S2, docs/S2_PACING.md section 5): measures what
// the pacing defaults are derived from, on an idle engine, from the
// published gauges only.
//
//   ckpt_walk_cost DIR N        creates N accounts on one shard in DIR
//   ckpt_walk_cost DIR 0 MODE   recovers DIR and runs three checkpoints with
//                               MODE = off (the walk before S2) or a walk
//                               budget in microseconds (pacing on, the
//                               budget pinned: min = max = ceiling)
//
// Prints, per checkpoint: objects, walk phase time, loop iterations that
// walked, the longest step (ns and objects), and ns per object.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../tests/harness.h"
#include "snc/gauges.h"
#include "snc/types/account.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: ckpt_walk_cost DIR N [off|BUDGET_US]\n");
    return 2;
  }
  register_type();
  const std::string dir = argv[1];
  const u64 n = std::strtoull(argv[2], nullptr, 10);
  Engine::Config c;
  c.runtime.n_shards = 1;
  c.runtime.pin = false;
  c.expected_objects = n ? n : 2'000'000;
  c.data_dir = dir;
  c.init = n != 0;
  if (argc > 3) {
    const std::string m = argv[3];
    if (m == "off") {
      c.ckpt_pacing = false;
    } else {
      const u64 ns = std::strtoull(m.c_str(), nullptr, 10) * 1000;
      c.ckpt_walk_min_ns = c.ckpt_walk_max_ns = c.ckpt_walk_ceiling_ns = ns;
    }
  }
  Engine eng(c);
  eng.start();
  if (n != 0) {
    run_all(
        eng, n,
        [&](u16, u64 i, Completion cb, void* ctx) {
          CreateArgs a{};
          a.balance_cents = 100;
          std::string email = "w" + std::to_string(i) + "@x";
          a.email_len = static_cast<u8>(email.size());
          std::memcpy(a.email, email.data(), email.size());
          create(kTypeId, ByteView(&a, sizeof(a)), {}, cb, ctx);
        },
        [&](u16, u64, const ExecResult& r) {
          if (r.status != Status::OK) {
            std::fprintf(stderr, "create failed: status %u\n", unsigned(r.status));
            std::abort();
          }
        });
    eng.checkpoint();
    std::printf("created %llu\n", (unsigned long long)n);
    eng.stop();
    return 0;
  }
  for (int k = 0; k < 3; ++k) {
    eng.checkpoint();
    const GaugeSnapshot g = eng.gauges(0);
    const u64 objects = g[G_CKPT_OBJECTS];
    std::printf("mode=%s objects=%llu walk_us=%llu steps=%llu step_max_ns=%llu step_max_objects=%llu "
                "walk_ns_per_object=%.1f step_ns_per_object=%.1f\n",
                argc > 3 ? argv[3] : "default", (unsigned long long)objects,
                (unsigned long long)g[G_CKPT_LAST_WALK_US], (unsigned long long)g[G_CKPT_WALK_STEPS],
                (unsigned long long)g[G_CKPT_STEP_MAX_NS], (unsigned long long)g[G_CKPT_STEP_MAX_OBJECTS],
                objects ? 1000.0 * g[G_CKPT_LAST_WALK_US] / objects : 0.0,
                g[G_CKPT_STEP_MAX_OBJECTS] ? 1.0 * g[G_CKPT_STEP_MAX_NS] / g[G_CKPT_STEP_MAX_OBJECTS] : 0.0);
  }
  eng.stop();
  return 0;
}
