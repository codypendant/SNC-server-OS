// Published gauges (docs/CHECKPOINT_BACKPRESSURE_DESIGN.md section 11, S0).
//
// Each shard owns one block. Only its own thread writes it, copying
// counters it already keeps into atomics (release/acquire, see
// ShardGauges), at most every
// kPublishNs (and never more often than 4x a publish's own duration) and
// on checkpoint phase changes. Any thread may read it
// without a shard task, a lock a shard holds or an allocation, so
// HEALTH and the GAUGES verb never wait on a shard (docs/HEALTH_DECOUPLING.md
// HL-1). A sequence number (odd while a publish is under way) lets a
// reader take a consistent snapshot of one block; ShardGauges states the
// memory-order contract.
//
// Observability only: nothing here changes an admission, checkpoint, I/O
// or recovery decision.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

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
  // Refusals by cause (cumulative). waiter and wal_buffer are S1's budgets
  // (docs/S1_ADMISSION.md); 0 with --admission legacy.
  G_REFUSED_FOLLOWUP,       // follow-up backlog above kEmitBacklog (shard.cpp execute)
  G_REFUSED_RING,           // forward or batch found the ring full
  G_REFUSED_WAITER,         // waiter budget (S1 admit)
  G_REFUSED_WAL_BUFFER,     // WAL buffer budget (S1 admit)
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
  // S1.1 (docs/S1_ADMISSION.md): forwarded client requests awaiting an
  // answer, and refusals by the pending budget (cumulative).
  G_REFUSED_PENDING,
  G_PENDING_NOW,
  G_PENDING_PEAK,
  G_REFUSED_REPLAY,          // S1.2: duplicates refused for waiter room (within refused_waiter)

  // Checkpoint walk pacing (S2, docs/S2_PACING.md section 4). Mode,
  // pressure, age state, past-target flag and budget are of the
  // checkpoint in progress (0 when idle); walk and step figures are of the
  // one in progress or the last; the two counts are since start.
  G_CKPT_PACING,             // 1 when the walk is time-budgeted (Config::ckpt_pacing)
  G_CKPT_WALK_BUDGET_NS,     // walk budget per loop iteration now (0 with pacing off)
  G_CKPT_MODE,               // 0 normal, 1 catch-up (behind the linear schedule to the target); 0 with pacing off
  G_CKPT_PRESSURE,           // 0 low, 1 hold, 2 high
  G_CKPT_AGE_STATE,          // 0 ok, 1 warn, 2 degraded (at or past the hard maximum age)
  G_CKPT_PAST_TARGET,        // 1 once the checkpoint in progress passed its target
  G_CKPT_PAST_TARGET_COUNT,
  G_CKPT_DEGRADED_COUNT,
  G_CKPT_TARGET_MS,          // configured
  G_CKPT_MAX_AGE_MS,         // configured
  G_CKPT_WALK_TOTAL,         // snapshot IDs to visit
  G_CKPT_WALK_DONE,          // snapshot IDs visited (the walk cursor)
  G_CKPT_WALK_STEPS,         // loop iterations that walked
  G_CKPT_CATCHUP_STEPS,      // of those, in catch-up
  G_CKPT_STEP_MAX_NS,        // longest walk step
  G_CKPT_STEP_MAX_OBJECTS,   // most IDs visited in one step
  G_CKPT_WAL_BASELINE_US,    // WAL flush EWMA learned before the checkpoint began
  // Bounded writeback (S3, docs/S3_WRITEBACK.md section 6). Per checkpoint
  // (reset at begin; kept after it ends) unless noted.
  G_CKPT_IO_DSYNC,           // 1 dsync (RWF_DSYNC chunks, windowed), 0 legacy (configured)
  G_CKPT_WINDOW,             // chunk writes allowed in flight (configured; 0 = no window, legacy)
  G_CKPT_WINDOW_FULL,        // 1 now: a full chunk waits for a window slot and the walk is stopped
  G_CKPT_FINAL_PENDING,      // 1 now: every ID visited, the last chunk waits for a slot
  G_CKPT_WINDOW_FULL_STEPS,  // loop iterations the walk was stopped by a full window
  G_CKPT_CHUNKS,             // chunk writes submitted
  G_CKPT_BUF_BYTES,          // chunk buffer bytes retained now (building + in flight); 0 when idle
  G_CKPT_BUF_BYTES_PEAK,
  G_CKPT_WRITE_LAT_LAST_US,  // chunk write: submit to completion processed
  G_CKPT_WRITE_LAT_EWMA_US,
  G_CKPT_WRITE_LAT_MAX_US,
  G_CKPT_WRITE_LAT_LT1MS,    // chunk write latency histogram: < 1 ms
  G_CKPT_WRITE_LAT_LT10MS,   //   1 to 10 ms
  G_CKPT_WRITE_LAT_LT100MS,  //   10 to 100 ms
  G_CKPT_WRITE_LAT_GE100MS,  //   100 ms or more
  G_CKPT_SYNC_WRITES,        // chunk writes that took the synchronous fallback (a run with any is not valid evidence)
  G_CKPT_SYNC_WRITES_TOTAL,  // the same since start
  // S4 (docs/S4_SYNC_TOKENS.md): the final fdatasync's node-level token.
  // The phase model is unchanged: a part waiting for a token is still in
  // DURABLE_WAIT, with sync_state 1; the node-wide counts are in
  // Engine::sync_tokens().
  G_CKPT_SYNC_TOKENS,          // K, final syncs allowed at once on the node (configured; 0 = unlimited)
  G_CKPT_SYNC_STATE,           // now: 0 not at the final sync, 1 waiting for a token, 2 syncing
  G_CKPT_TOKEN_WAIT_SINCE_NS,  // when the token wait in progress began (steady clock); 0 if none
  G_CKPT_TOKEN_WAIT_LAST_US,   // token request to grant, of the last grant
  G_CKPT_TOKEN_WAIT_MAX_US,    // since start
  G_CKPT_TOKEN_WAIT_EWMA_US,   // since start
  G_CKPT_TOKEN_ACQUIRED,       // tokens acquired since start (one per part sync)
  G_CKPT_TOKEN_TICKET,         // the latest request's place in the node's order, + 1 (0 = none yet)
  // Follow-up intake D.1 (docs/FOLLOWUP_INTAKE_DESIGN.md 3.9.6). The
  // effective settings first (after config, env overrides and defaults),
  // then state now, then counters since start.
  G_INTAKE_WAL_GATE,           // 1 when the WAL gate H is on
  G_INTAKE_WAL_CAP_BYTES,      // H, effective (max(2B, B + R) unless set)
  G_INTAKE_NEED_BYTES,         // R: the room one follow-up apply needs
  G_INTAKE_WINDOW,             // W per shard pair; 0 = no window
  G_INTAKE_EXCHANGE_GATE,      // 1 when the exchange budget X is on
  G_INTAKE_EXCHANGE_BUDGET,    // X (0 when the gate is off)
  G_INTAKE_OWNER_GATE,         // 1 when the owner gate is on
  G_INTAKE_LINK_HOLD_MAX,      // L; 0 = node-link reads never pause
  G_INTAKE_GATE_OPEN,          // now: 1 when a follow-up may apply (H has room)
  G_INTAKE_GATED,              // times H stopped follow-up applies
  G_INTAKE_REFUSED_EXCHANGE,   // client ops refused: X full
  G_INTAKE_REFUSED_OWNER,      // forwarded mutations refused by the owner gate
  G_INTAKE_EXCHANGES_LIVE,     // this shard's exchanges not yet ended (any shard of this node)
  G_INTAKE_EXCHANGES_LIVE_PEAK,
  G_INTAKE_EXCHANGES_STARTED,
  G_INTAKE_EXCHANGES_ENDED,    // exchanges (of any origin here) that ended on this shard
  G_INTAKE_STAGED_NOW,         // follow-ups staged here, waiting to apply
  G_INTAKE_STAGED_PEAK,
  G_INTAKE_WINDOW_OUTSTANDING, // sent to this node's other shards, not yet taken from their staging
  G_INTAKE_WINDOW_WAITS,       // sends stopped by a full window
  G_INTAKE_LINK_HELD_NOW,      // follow-ups from node links held here
  G_INTAKE_LINK_HELD_PEAK,
  G_INTAKE_LINK_PAUSED,        // now: 1 while node-link reads are paused
  G_INTAKE_LINK_PAUSES,
  G_INTAKE_RESUME_ACTIVE,      // now: 1 while a resume scan runs
  G_INTAKE_RESUME_OWED,        // now: 1 while another pass is owed after it
  G_INTAKE_RESUME_PASSES,
  G_INTAKE_RESUME_TRIGGERS,
  G_INTAKE_RESUME_COALESCED,
  G_INTAKE_RESUME_RESTARTS,
  G_INTAKE_RESUME_OBJECTS,
  // Cross-node exchange accounting (docs/CROSS_NODE_EXCHANGE_DESIGN.md 7):
  // configuration; state now, summed over entries (xnode_entries is
  // bounded by xnode_entry_slots, the members of every link); counters,
  // cumulative across links instances. xnode_end_underflow,
  // xnode_held_underflow, xnode_debt_increases, xnode_uncounted,
  // xnode_drops_to_resume and xnode_handoff_drops must stay 0 (the last two
  // count invariant checks I1 and I2, 2.9.1).
  G_XNODE_ACCOUNTING,
  G_XNODE_HB_MS,
  G_XNODE_DEAD_MS,
  G_XNODE_RELEASE_MS,
  G_XNODE_BLOCK_MS,
  G_XNODE_INCARNATION,
  G_XNODE_HIGHEST_INCARNATION,
  G_XNODE_REMOTE_LIVE,
  G_XNODE_DEBT,
  G_XNODE_HELD_FOR_PEERS,
  G_XNODE_END_OWED,
  G_XNODE_LINKS_UNBOUND,
  G_XNODE_LINKS_BOUND,
  G_XNODE_ENTRIES,
  G_XNODE_ENTRIES_QUARANTINED,
  G_XNODE_ENTRY_SLOTS,
  G_XNODE_BLOCKED,
  G_XNODE_LEFT,
  G_XNODE_BINDINGS,
  G_XNODE_IDENTIFIED,
  G_XNODE_BIND_MISMATCH,
  G_XNODE_SYNCS_SENT,
  G_XNODE_SYNCS_EXACT,
  G_XNODE_SYNCS_STALE,
  G_XNODE_SYNC_CORRECTION,
  G_XNODE_SYNC_R_ANOMALIES,
  G_XNODE_ENDS_SENT,
  G_XNODE_END_ITEMS_SENT,
  G_XNODE_ENDS_RECEIVED,
  G_XNODE_END_ITEMS_RECEIVED,
  G_XNODE_REPLIES_PUSHED,
  G_XNODE_REPLIES_RETURNED,
  G_XNODE_DISPOSITIONS,
  G_XNODE_STALE_TAGS,
  G_XNODE_END_UNDERFLOW,
  G_XNODE_HELD_UNDERFLOW,
  G_XNODE_DEBT_INCREASES,
  G_XNODE_QUARANTINES,
  G_XNODE_Q_IN_DOWN,
  G_XNODE_Q_BLOCKED,
  G_XNODE_Q_NOT_BOUND,
  G_XNODE_QUARANTINE_OUT_RESETS,
  G_XNODE_RETIREMENTS,
  G_XNODE_RETIRE_PURGED,
  G_XNODE_DROPS_TO_RESUME,
  G_XNODE_TAG_DROPS,
  G_XNODE_HANDOFF_DROPS,
  G_XNODE_REPLY_DROPS,
  G_XNODE_UNBOUND_PURGED,
  G_XNODE_UNCOUNTED,
  G_XNODE_IGNORED_FRAMES,
  G_XNODE_HEARTBEATS_SENT,
  G_XNODE_SILENCE_CLOSES,
  G_XNODE_OUT_RESETS,
  G_XNODE_STEERS,
  G_XNODE_FENCED,
  G_XNODE_M_CONFLICTS,
  G_XNODE_BLOCK_LEAVES,
  G_XNODE_REJOINS,
  G_LINK_VERSION_REFUSALS,
  G_LINK_ACCOUNTING_REFUSALS,
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
    "refused_pending", "pending_now", "pending_peak", "refused_replay",
    "ckpt_pacing", "ckpt_walk_budget_ns", "ckpt_mode", "ckpt_pressure", "ckpt_age_state",
    "ckpt_past_target", "ckpt_past_target_count", "ckpt_degraded_count", "ckpt_target_ms",
    "ckpt_max_age_ms", "ckpt_walk_total", "ckpt_walk_done", "ckpt_walk_steps", "ckpt_catchup_steps",
    "ckpt_step_max_ns", "ckpt_step_max_objects", "ckpt_wal_baseline_us",
    "ckpt_io_dsync", "ckpt_window", "ckpt_window_full", "ckpt_final_pending", "ckpt_window_full_steps",
    "ckpt_chunks", "ckpt_buf_bytes", "ckpt_buf_bytes_peak", "ckpt_write_lat_last_us",
    "ckpt_write_lat_ewma_us", "ckpt_write_lat_max_us", "ckpt_write_lat_lt1ms", "ckpt_write_lat_lt10ms",
    "ckpt_write_lat_lt100ms", "ckpt_write_lat_ge100ms", "ckpt_sync_writes", "ckpt_sync_writes_total",
    "ckpt_sync_tokens", "ckpt_sync_state", "ckpt_token_wait_since_ns", "ckpt_token_wait_last_us",
    "ckpt_token_wait_max_us", "ckpt_token_wait_ewma_us", "ckpt_token_acquired", "ckpt_token_ticket",
    "intake_wal_gate", "intake_wal_cap_bytes", "intake_need_bytes", "intake_window", "intake_exchange_gate",
    "intake_exchange_budget", "intake_owner_gate", "intake_link_hold_max", "intake_gate_open", "intake_gated",
    "intake_refused_exchange", "intake_refused_owner", "intake_exchanges_live", "intake_exchanges_live_peak",
    "intake_exchanges_started", "intake_exchanges_ended", "intake_staged_now", "intake_staged_peak",
    "intake_window_outstanding", "intake_window_waits", "intake_link_held_now", "intake_link_held_peak",
    "intake_link_paused", "intake_link_pauses", "intake_resume_active", "intake_resume_owed",
    "intake_resume_passes", "intake_resume_triggers", "intake_resume_coalesced", "intake_resume_restarts",
    "intake_resume_objects",
    "xnode_accounting", "xnode_hb_ms", "xnode_dead_ms", "xnode_release_ms", "xnode_block_ms",
    "xnode_incarnation", "xnode_highest_incarnation", "xnode_remote_live", "xnode_debt",
    "xnode_held_for_peers", "xnode_end_owed", "xnode_links_unbound", "xnode_links_bound", "xnode_entries",
    "xnode_entries_quarantined", "xnode_entry_slots", "xnode_blocked", "xnode_left", "xnode_bindings",
    "xnode_identified", "xnode_bind_mismatch", "xnode_syncs_sent", "xnode_syncs_exact", "xnode_syncs_stale",
    "xnode_sync_correction", "xnode_sync_r_anomalies", "xnode_ends_sent", "xnode_end_items_sent",
    "xnode_ends_received", "xnode_end_items_received", "xnode_replies_pushed", "xnode_replies_returned",
    "xnode_dispositions", "xnode_stale_tags", "xnode_end_underflow", "xnode_held_underflow",
    "xnode_debt_increases", "xnode_quarantines", "xnode_q_in_down", "xnode_q_blocked", "xnode_q_not_bound",
    "xnode_quarantine_out_resets", "xnode_retirements", "xnode_retire_purged", "xnode_drops_to_resume",
    "xnode_tag_drops", "xnode_handoff_drops", "xnode_reply_drops", "xnode_unbound_purged", "xnode_uncounted",
    "xnode_ignored_frames", "xnode_heartbeats_sent", "xnode_silence_closes", "xnode_out_resets",
    "xnode_steers", "xnode_fenced", "xnode_m_conflicts", "xnode_block_leaves", "xnode_rejoins",
    "link_version_refusals", "link_accounting_refusals",
    "published_ns",
};

// Every gauge has a name (a missing one would be a null %s in GAUGES).
inline constexpr bool gauge_names_complete() {
  for (const char* n : kGaugeNames) {
    if (n == nullptr || n[0] == '\0') return false;
  }
  return true;
}
static_assert(gauge_names_complete(), "kGaugeNames must name every Gauge");

// A consistent copy of one block (or the last attempt, consistent = false,
// if a writer kept publishing through every retry). Before the shard's
// first publish the block reads as zeros with consistent = true; tell that
// case by published_ns == 0.
struct GaugeSnapshot {
  std::array<u64, kGaugeCount> v{};
  // Written by the main thread at each checkpoint commit and at recovery:
  // the start LSN of this shard's stream in the active checkpoint (1 for a
  // space that has none: replay from the start), 0 before recovery or after
  // a remap until its checkpoint commits. A recovery now would replay from it.
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

// Publication contract (a single-writer seqlock; every field is an atomic,
// so no access is a data race and the argument below is the whole proof):
//
//   seq odd   a publish is in progress. seq even = 2k: generation k is
//             complete (k = 0 is the all-zero block before the first publish).
//   writer    seq.store(2k+1, release); each field.store(release);
//             seq.store(2k+2, release).
//   reader    s1 = seq.load(acquire); odd -> retry; each field.load(acquire);
//             s2 = seq.load(acquire); accept only if s1 == s2 (even).
//
// Why an accepted snapshot is exactly generation k (s1 == s2 == 2k):
//  1. No field is older than k. s1 read 2k, written by the final release
//     store of publish k (the last write to seq before 2k+1), so the field
//     stores of publish k happen-before the reader's field loads; by
//     coherence each load returns generation k's value or a later one.
//  2. No field is newer than k. Suppose a field load read the store made
//     by publish j > k (the argument is about which store a load reads,
//     since values can repeat across generations). That acquire load
//     synchronizes with the writer's release field store of publish j; the
//     store of 2j-1 to seq is sequenced before that field store, so it
//     happens-before the reader's later s2 load. By coherence s2 reads
//     2j-1 or a later value of seq; seq only grows (single writer) and
//     2j-1 > 2k, so s2 != s1 and the snapshot is rejected.
// The final release store of 2k+2 is what case 1 acquires; the per-field
// release/acquire pairs are what case 2 needs (a relaxed field load could
// read a newer store without making the odd seq visible, and an acquire
// on s2 alone orders only what follows it, not the earlier field loads).
// The release on the odd store and the acquire on s2 are not needed by
// this proof; they are kept so the pattern reads as the conventional one,
// at no cost on x86-64. On x86-64 every
// one of these is a plain load or store; no fence or locked instruction
// is emitted.
//
// committed_start_lsn is a separate atomic written by the main thread and
// is not covered by seq: it is read on its own, as of the same instant or
// close to it.
struct alignas(64) ShardGauges {
  static constexpr u64 kPublishNs = 50'000;  // at most every 50 us, plus phase changes

  // Shard thread only (the single writer).
  void publish(const std::array<u64, kGaugeCount>& vals) {
    const u64 s = seq_.load(std::memory_order_relaxed);  // own last write
    seq_.store(s + 1, std::memory_order_release);        // odd: in progress
    for (u32 i = 0; i < kGaugeCount; ++i) v_[i].store(vals[i], std::memory_order_release);
    seq_.store(s + 2, std::memory_order_release);        // even: generation complete
  }
  // Main thread only.
  void set_committed_start_lsn(u64 lsn) { committed_.store(lsn, std::memory_order_relaxed); }

  // Any thread but a shard's. Retries up to 64 times, yielding between
  // tries, then up to 64 more 100 us apart (about 10 ms in all), so a slow
  // publish or a slow reader (a sanitizer build) still gets a whole copy.
  // If the writer was mid-publish every time, returns consistent = false
  // with the fields of the last attempt that read an even seq (zeros if
  // none did).
  GaugeSnapshot read() const {
    GaugeSnapshot out;
    for (int attempt = 0; attempt < 128; ++attempt) {
      if (attempt >= 64) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      } else if (attempt != 0) {
        std::this_thread::yield();
      }
      const u64 s1 = seq_.load(std::memory_order_acquire);
      if (s1 & 1) continue;  // publish in progress
      for (u32 i = 0; i < kGaugeCount; ++i) out.v[i] = v_[i].load(std::memory_order_acquire);
      const u64 s2 = seq_.load(std::memory_order_acquire);
      if (s1 == s2) {
        out.consistent = true;
        break;
      }
    }
    out.committed_start_lsn = committed_.load(std::memory_order_relaxed);
    return out;
  }
  u64 publishes() const { return seq_.load(std::memory_order_acquire) / 2; }

 private:
  std::atomic<u64> seq_{0};
  std::array<std::atomic<u64>, kGaugeCount> v_{};
  std::atomic<u64> committed_{0};
};

// The node's checkpoint (Engine::checkpoint). Main thread writes, any
// thread reads. Each field is one atomic, consistent on its own but not
// with the others: a reader can see completed of checkpoint N+1 with
// last_* of N for an instant. Diagnostic use only.
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
