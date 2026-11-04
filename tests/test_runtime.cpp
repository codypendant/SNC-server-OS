// Native runtime: every shard streams sequence-numbered messages to every
// other shard through deliberately tiny rings, forcing overflow. Checks
// per-pair FIFO order and that nothing is lost or duplicated.
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "check.h"
#include "snc/native_runtime.h"

using namespace snc;

namespace {
constexpr u16 kShards = 4;
constexpr u64 kPerPair = 200000;

struct PerShard {
  std::vector<u64> next_send;
  std::vector<u64> next_expect;
};
std::vector<PerShard> g_state(kShards);
std::atomic<u64> g_received{0};
std::atomic<bool> g_order_ok{true};

void init(void*) {
  auto& s = g_state[sri_shard_id()];
  s.next_send.assign(kShards, 0);
  s.next_expect.assign(kShards, 0);
}

void on_message(void*, const Msg& m) {
  auto& s = g_state[sri_shard_id()];
  if (m.req_id != s.next_expect[m.src_shard]) g_order_ok = false;
  ++s.next_expect[m.src_shard];
  g_received.fetch_add(1, std::memory_order_relaxed);
}

bool poll(void*) {
  u16 me = sri_shard_id();
  auto& s = g_state[me];
  bool did = false;
  for (u16 d = 0; d < kShards; ++d) {
    if (d == me) continue;
    for (int k = 0; k < 16 && s.next_send[d] < kPerPair; ++k) {
      Msg m{};
      m.kind = MsgKind::CTRL;
      m.src_shard = me;
      m.req_id = s.next_send[d];
      if (!sri_send(d, m)) break;  // overflow full: back off, retry next iteration
      ++s.next_send[d];
      did = true;
    }
  }
  return did;
}
}  // namespace

int main() {
  RuntimeConfig cfg;
  cfg.n_shards = kShards;
  cfg.ring_slots = 8;
  cfg.overflow_limit = 32;
  ShardHooks h;
  h.init = init;
  h.on_message = on_message;
  h.poll = poll;
  NativeRuntime rt(cfg, h);
  rt.start();

  const u64 total = kPerPair * kShards * (kShards - 1);
  // The deadline is a safety net, not a throughput assertion. With more
  // shards than cores the pinned workers time-share, so delivery of this
  // deliberately overflow-heavy stream slows roughly in proportion to the
  // oversubscription. 60 s was tuned on the 4-vCPU loopback reference; on a
  // 2-vCPU host the four shards take ~100 s. Scale by the oversubscription
  // factor so the net stays the same on >= kShards cores (docs/REAL_MACHINES.md).
  unsigned ncpu = std::thread::hardware_concurrency();
  if (ncpu == 0) ncpu = kShards;
  unsigned waves = (kShards + ncpu - 1) / ncpu;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60) * waves;
  while (g_received.load() < total && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Control path works on every shard.
  std::atomic<int> ran{0};
  rt.run_on_all([&](u16) { ran.fetch_add(1); });
  CHECK_EQ(ran.load(), kShards);

  rt.stop();
  CHECK_EQ(g_received.load(), total);
  CHECK(g_order_ok.load());
  u64 full = 0, refused = 0;
  for (u16 s = 0; s < kShards; ++s) {
    for (u16 d = 0; d < kShards; ++d) {
      RingStats st = rt.ring_stats(s, d);
      if (s != d) CHECK_EQ(st.msgs, kPerPair);
      full += st.full_events;
      refused += st.refused;
    }
  }
  std::printf("test_runtime: ok (ring-full events %llu, refused %llu, pinned %d)\n",
              (unsigned long long)full, (unsigned long long)refused, rt.topology(0).pinned);
  return 0;
}
