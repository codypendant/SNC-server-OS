// S4: the node's final-sync tokens (docs/S4_SYNC_TOKENS.md).
//
// At most K shards may have a checkpoint part's final fdatasync in flight
// at once. A shard ready to sync takes a ticket once, then polls: it may
// start when its ticket is below releases + K. Nothing blocks: the shard
// keeps running its loop between polls (S4-I4).
//
// Bound (S4-I1): every granted ticket t satisfied t < R + K for the
// release count R it read, and R only grows, so all granted tickets are
// below R_now + K: at most R_now + K grants, of which R_now are released,
// leaves at most K active.
//
// Order and progress (S4-I3): tickets are handed out in request order.
// A ticket is eligible once releases + K passes it, which happens in
// ticket order, so a later request is never eligible before an earlier
// one. A later ticket may enter before an earlier eligible one has polled
// again, but only within the same window of K: no ticket waits for more
// than (its position in the queue) releases, and every active sync
// completes (it holds no other resource), so every waiter is eventually
// granted. A shard takes one ticket per checkpoint and cannot reacquire
// before every earlier ticket is eligible.
//
// Counters are monotonic for the life of the Engine and never reset, so
// nothing drifts across checkpoints (S4-T9). K = 0: unlimited, every
// request is granted at its first poll, so a shard syncs exactly when it
// did in S3 (S4-I6); the counts are kept so the concurrency the bound
// would have limited stays observable for the differential test (S4-T7).
//
// Tickets are never abandoned: there is no cancellation path once a
// shard has taken one (stop() waits for the checkpoint, and a storage
// fatal ends the process), so a waiting ticket is always granted later.
#pragma once

#include <atomic>

#include "snc/types.h"

namespace snc {

struct SyncTokens {
  explicit SyncTokens(u32 k) : limit(k) {}
  SyncTokens(const SyncTokens&) = delete;
  SyncTokens& operator=(const SyncTokens&) = delete;

  const u32 limit;                // K; 0: unlimited
  std::atomic<u64> requests{0};   // tickets handed out
  std::atomic<u64> grants{0};     // tokens acquired
  std::atomic<u64> releases{0};   // tokens released
  std::atomic<u64> active_peak{0};
  std::atomic<u64> waiting_peak{0};
  std::atomic<u64> wait_max_us{0};
  // Final fdatasyncs actually in flight, counted by the shards around the
  // sync itself, independently of the tokens: the quantity S4-I1 bounds.
  // A shard that bypassed its token would show here and not in grants.
  std::atomic<u64> inflight{0};
  std::atomic<u64> inflight_peak{0};

  bool limited() const { return limit != 0; }

  u64 take_ticket() {
    const u64 t = requests.fetch_add(1, std::memory_order_acq_rel);
    const u64 g = grants.load(std::memory_order_acquire);
    note_max(waiting_peak, t + 1 > g ? t + 1 - g : 0);
    return t;
  }

  // Nonblocking: true if `ticket` may start its sync now (then counted).
  bool try_acquire(u64 ticket) {
    if (limit != 0 && ticket >= releases.load(std::memory_order_acquire) + limit) return false;
    const u64 g = grants.fetch_add(1, std::memory_order_acq_rel) + 1;
    const u64 r = releases.load(std::memory_order_acquire);
    note_max(active_peak, g > r ? g - r : 0);  // never above the true count
    return true;
  }

  void release() { releases.fetch_add(1, std::memory_order_acq_rel); }

  void note_wait(u64 us) { note_max(wait_max_us, us); }
  void sync_started() { note_max(inflight_peak, inflight.fetch_add(1, std::memory_order_acq_rel) + 1); }
  void sync_finished() { inflight.fetch_sub(1, std::memory_order_acq_rel); }

  // A consistent reading: grants unchanged across the read of the others.
  struct Snapshot {
    u64 requests = 0, grants = 0, releases = 0, active = 0, waiting = 0;
  };
  Snapshot snapshot() const {
    Snapshot s;
    for (int i = 0; i < 64; ++i) {
      const u64 g1 = grants.load(std::memory_order_acquire);
      const u64 r = releases.load(std::memory_order_acquire);
      const u64 q = requests.load(std::memory_order_acquire);
      const u64 g2 = grants.load(std::memory_order_acquire);
      s = Snapshot{q, g1, r, g1 >= r ? g1 - r : 0, q >= g1 ? q - g1 : 0};
      if (g1 == g2) break;
    }
    return s;
  }

 private:
  static void note_max(std::atomic<u64>& m, u64 v) {
    u64 cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
  }
};

}  // namespace snc
