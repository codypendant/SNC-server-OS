// Published gauges (docs/CHECKPOINT_BACKPRESSURE_DESIGN.md section 11, S0).
//
// Each shard owns one block. Only its own thread writes it, copying
// counters it already keeps into relaxed atomics, at most every
// kPublishNs and on checkpoint phase changes. Any thread may read it
// without a shard task, a lock a shard holds or an allocation, so
// HEALTH and the GAUGES verb never wait on a shard (docs/HEALTH_DECOUPLING.md
// HL-1). A sequence number (odd while a publish is under way) lets a
// reader take a consistent snapshot of one block.
//
// Observability only: nothing here changes an admission, checkpoint, I/O
// or recovery decision.
#pragma once

#include <array>
#include <atomic>

#include "snc/types.h"

namespace snc {

// A shard's checkpoint phase (spec section 12; shard.cpp checkpoint_step).
enum class CkptPhase : u8 {
  IDLE = 0,
  SNAPSHOT = 1,      // begin: the ID snapshot (one step, never published mid-way)
  WALK = 2,          // visiting snapshot IDs, writing chunks
  DRAIN = 3,         // walk done; chunk writes still in flight
  DURABLE_WAIT = 4,  // writes done; waiting for max_lsn to be locally durable (C5)
  SYNC = 5,          // fdatasync in flight
};
const char* ckpt_phase_name(u64 phase);

// The node's checkpoint phase (Engine::checkpoint, main thread).
enum class NodeCkptPhase : u8 {
  IDLE = 0,
  SHARDS = 1,   // waiting for every shard's part
  COMMIT = 2,   // directory sync, manifest, superblock
  RECLAIM = 3,  // old checkpoint and WAL segments removed
};
const char* node_ckpt_phase_name(u64 phase);

// Exponentially weighted moving average with alpha = 1/8, kept in
// sixteenths. The first sample sets it. Pure, so tests feed it exact
// sequences (S0-T4).
struct Ewma {
  u64 x16 = 0;  // value * 16
  u64 n = 0;    // samples seen
  void add(u64 sample) {
    const i64 s16 = static_cast<i64>(sample) * 16;
    if (n++ == 0) {
      x16 = static_cast<u64>(s16);
      return;
    }
    const i64 cur = static_cast<i64>(x16);
    x16 = static_cast<u64>(cur + (s16 - cur) / 8);
  }
  u64 value() const { return x16 / 16; }
  void reset() { *this = Ewma{}; }
};

// Field ids of a shard's block. Names in kGaugeNames, same order.
enum Gauge : u32 {
  // Refusals by cause (cumulative). waiter and wal_buffer have no refusal
  // path before S1 and stay 0.
  G_REFUSED_FOLLOWUP,       // follow-up backlog above kEmitBacklog (shard.cpp execute)
  G_REFUSED_RING,           // forward or batch found the ring full
  G_REFUSED_WAITER,         // S1 (always 0 in S0)
  G_REFUSED_WAL_BUFFER,     // S1 (always 0 in S0)
  G_GATHER_OVERLOADED,      // index gather legs dropped: ring full
  // Durability waiters.
  G_WAITERS_NOW,
  G_WAITERS_PEAK,
  G_WAITER_BYTES_NOW,       // sizeof(Waiter) + held result bytes, summed
  G_WAITER_BYTES_PEAK,
  // Follow-ups.
  G_EMITS_NOW,              // emits_in_flight()
  G_EMITS_PEAK,             // as seen at admission checks and publishes
  // WAL.
  G_WAL_PRESENT,            // 1 when this shard has a WAL
  G_WAL_DIRECT,             // the open segment is O_DIRECT (actual, not configured)
  G_WAL_ACTIVE_BYTES_NOW,   // unflushed record bytes in the active buffer
  G_WAL_ACTIVE_BYTES_PEAK,
  G_WAL_BUFFER_CAP_BYTES,   // active + flushing buffer capacity
  G_WAL_NEXT_LSN,
  G_WAL_DURABLE_LSN,
  G_WAL_UNDURABLE,          // next_lsn - 1 - durable_lsn
  G_WAL_FLUSHES,
  G_WAL_FLUSH_EWMA_US,      // submit to completion
  G_WAL_FLUSH_MAX_US,
  G_WAL_FLUSH_SINCE_NS,     // start of the flush in flight, 0 if none
  // Checkpoint, this shard's part.
  G_CKPT_PHASE,             // CkptPhase
  G_CKPT_SEQ,               // of the checkpoint in progress or last finished
  G_CKPT_STARTED_NS,        // begin of the checkpoint in progress, 0 if idle
  G_CKPT_PHASE_SINCE_NS,    // entry into the current phase, 0 if idle
  G_CKPT_OBJECTS,           // images written so far
  G_CKPT_BYTES_ISSUED,      // part bytes handed to write
  G_CKPT_BYTES_WRITTEN,     // part bytes whose write completed
  G_CKPT_WRITES_INFLIGHT,
  G_CKPT_WRITES_INFLIGHT_PEAK,  // within the checkpoint in progress or last
  G_CKPT_LAST_SNAPSHOT_US,  // phase durations of the last finished part
  G_CKPT_LAST_WALK_US,
  G_CKPT_LAST_DRAIN_US,
  G_CKPT_LAST_DURABLE_WAIT_US,
  G_CKPT_LAST_SYNC_US,
  G_CKPT_LAST_TOTAL_US,
  G_CKPT_LAST_RESULT,       // 0 none yet, 1 ok (a failed write or sync is fatal, spec C7)
  G_CKPT_COMPLETED,
  // When this block was last published (steady clock, ns).
  G_PUBLISHED_NS,
  kGaugeCount
};

inline constexpr std::array<const char*, kGaugeCount> kGaugeNames = {
    "refused_followup", "refused_ring", "refused_waiter", "refused_wal_buffer", "gather_overloaded",
    "waiters_now", "waiters_peak", "waiter_bytes_now", "waiter_bytes_peak",
    "emits_now", "emits_peak",
    "wal_present", "wal_direct", "wal_active_bytes_now", "wal_active_bytes_peak", "wal_buffer_cap_bytes",
    "wal_next_lsn", "wal_durable_lsn", "wal_undurable", "wal_flushes", "wal_flush_ewma_us",
    "wal_flush_max_us", "wal_flush_since_ns",
    "ckpt_phase", "ckpt_seq", "ckpt_started_ns", "ckpt_phase_since_ns", "ckpt_objects",
    "ckpt_bytes_issued", "ckpt_bytes_written", "ckpt_writes_inflight", "ckpt_writes_inflight_peak",
    "ckpt_last_snapshot_us", "ckpt_last_walk_us", "ckpt_last_drain_us", "ckpt_last_durable_wait_us",
    "ckpt_last_sync_us", "ckpt_last_total_us", "ckpt_last_result", "ckpt_completed",
    "published_ns",
};

// A consistent copy of one block (or the last attempt, consistent = false,
// if a writer kept publishing through every retry).
struct GaugeSnapshot {
  std::array<u64, kGaugeCount> v{};
  // Written by the main thread at each checkpoint commit and at recovery:
  // the start LSN of this shard's stream in the active checkpoint, 0 if
  // none is known. A recovery now would replay from it.
  u64 committed_start_lsn = 0;
  bool consistent = false;
  u64 operator[](Gauge g) const { return v[g]; }
  // WAL records a recovery now would replay (0 if no checkpoint is known).
  u64 replay_records() const {
    return committed_start_lsn != 0 && v[G_WAL_NEXT_LSN] > committed_start_lsn
               ? v[G_WAL_NEXT_LSN] - committed_start_lsn
               : 0;
  }
};

struct alignas(64) ShardGauges {
  static constexpr u64 kPublishNs = 50'000;  // at most every 50 us, plus phase changes

  // Shard thread only.
  void publish(const std::array<u64, kGaugeCount>& vals) {
    const u64 s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (u32 i = 0; i < kGaugeCount; ++i) v_[i].store(vals[i], std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
  }
  // Main thread only.
  void set_committed_start_lsn(u64 lsn) { committed_.store(lsn, std::memory_order_relaxed); }

  // Any thread.
  GaugeSnapshot read() const {
    GaugeSnapshot out;
    for (int attempt = 0; attempt < 64; ++attempt) {
      const u64 s1 = seq_.load(std::memory_order_acquire);
      for (u32 i = 0; i < kGaugeCount; ++i) out.v[i] = v_[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      const u64 s2 = seq_.load(std::memory_order_relaxed);
      if (s1 == s2 && (s1 & 1) == 0) {
        out.consistent = true;
        break;
      }
    }
    out.committed_start_lsn = committed_.load(std::memory_order_relaxed);
    return out;
  }
  u64 publishes() const { return seq_.load(std::memory_order_relaxed) / 2; }

 private:
  std::atomic<u64> seq_{0};
  std::array<std::atomic<u64>, kGaugeCount> v_{};
  std::atomic<u64> committed_{0};
};

// The node's checkpoint (Engine::checkpoint). Main thread writes, any
// thread reads; each field is one atomic.
struct NodeCkptGauges {
  std::atomic<u64> phase{0};        // NodeCkptPhase
  std::atomic<u64> seq{0};          // in progress or last finished
  std::atomic<u64> started_ns{0};   // 0 if idle
  std::atomic<u64> last_total_us{0};
  std::atomic<u64> last_shards_us{0};   // until every part reported
  std::atomic<u64> last_commit_us{0};   // directory sync, manifest, superblock
  std::atomic<u64> last_reclaim_us{0};
  std::atomic<u64> completed{0};
};

}  // namespace snc
