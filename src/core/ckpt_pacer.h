// Checkpoint walk pacing (slice S2; docs/S2_PACING.md;
// docs/CHECKPOINT_BACKPRESSURE_DESIGN.md 7.2 and 7.3).
//
// A pure controller: step() reads only its arguments, so synthetic traces
// drive it in tests (S2 test P1). It sets one thing, the walk budget (ns
// of checkpoint walk per shard loop iteration), and reports the target and
// hard-maximum-age state. Nothing here changes what the walk visits:
// pacing changes visit times, never the visit set (CC-1).
//
// Every number in PacerConfig is a configurable HYPOTHESIS
// (docs/S2_PACING.md section 5); none is frozen.
#pragma once

#include "snc/types.h"

namespace snc::ckpt {

struct PacerConfig {
  u64 walk_min_ns = 0;      // floor: the walk never gets less per iteration
  u64 walk_max_ns = 0;      // top of the pressure-driven (normal) range
  u64 walk_ceiling_ns = 0;  // foreground-protection ceiling; catch-up never exceeds it
  u64 walk_step_ns = 0;     // additive increase per update
  u64 update_ns = 0;        // at most one budget change per this interval
  // Pressure, with hysteresis (low < high). WAL: flush-latency EWMA as a
  // percent of the baseline learned outside checkpoints. Backlog: the
  // follow-up backlog as a percent of its gate (kEmitBacklog).
  u32 wal_high_pct = 0, wal_low_pct = 0;
  u32 backlog_high_pct = 0, backlog_low_pct = 0;
  u64 target_ns = 0;   // target completion time; 0 = none (never behind, never past target)
  u64 max_age_ns = 0;  // hard maximum checkpoint age; 0 = none
  u32 warn_pct = 0;    // warn at this percent of max_age_ns
};

enum class Pressure : u8 { LOW = 0, HOLD = 1, HIGH = 2 };
enum class Mode : u8 { NORMAL = 0, CATCH_UP = 1 };
enum class AgeState : u8 { OK = 0, WARN = 1, DEGRADED = 2 };

struct Signals {
  u64 now_ns = 0;
  u64 wal_ewma_us = 0;      // WAL flush latency EWMA now
  u64 wal_baseline_us = 0;  // learned while no checkpoint was active; 0 = unknown
  u64 backlog = 0, backlog_cap = 0;
  u64 done = 0, total = 0;  // snapshot IDs visited, snapshot size
};

struct PacerState {
  u64 walk_ns = 0;
  Mode mode = Mode::NORMAL;
  Pressure pressure = Pressure::HOLD;
  AgeState age = AgeState::OK;
  bool past_target = false;
  u64 started_ns = 0, last_update_ns = 0;
  u64 changes = 0;  // budget changes made
};

// Upper limit on any budget, step or update interval: far above any useful
// value, far below where `now + budget` or `budget + step` could wrap.
inline constexpr u64 kMaxIntervalNs = 60'000'000'000ull;  // 60 s

// The rules every config must meet; Engine refuses a config that fails.
inline bool valid(const PacerConfig& c) {
  return c.walk_min_ns >= 1 && c.walk_min_ns <= c.walk_max_ns && c.walk_max_ns <= c.walk_ceiling_ns &&
         c.walk_ceiling_ns <= kMaxIntervalNs && c.walk_step_ns >= 1 && c.walk_step_ns <= kMaxIntervalNs &&
         c.update_ns <= kMaxIntervalNs && c.wal_low_pct < c.wal_high_pct &&
         c.backlog_low_pct < c.backlog_high_pct && c.warn_pct >= 1 && c.warn_pct <= 100 &&
         (c.max_age_ns == 0 || c.target_ns <= c.max_age_ns);
}

// At checkpoint begin: the floor budget, normal mode. Nothing carries over
// from the previous checkpoint except the caller's learned WAL baseline.
inline PacerState reset(const PacerConfig& c, u64 now_ns) {
  PacerState s;
  s.walk_ns = c.walk_min_ns;
  s.started_ns = now_ns;
  s.last_update_ns = now_ns;
  return s;
}

// High if either signal is above its high mark; low only if both are
// below their low marks; otherwise hold. An unknown baseline (no flush yet
// outside a checkpoint) leaves the WAL signal out: it reads as low, and
// the backlog alone decides.
inline Pressure pressure(const PacerConfig& c, const Signals& g) {
  const u64 wal_pct = g.wal_baseline_us == 0 ? 0 : g.wal_ewma_us * 100 / g.wal_baseline_us;
  const u64 bl_pct = g.backlog_cap == 0 ? 0 : g.backlog * 100 / g.backlog_cap;
  if (wal_pct > c.wal_high_pct || bl_pct > c.backlog_high_pct) return Pressure::HIGH;
  if (wal_pct < c.wal_low_pct && bl_pct < c.backlog_low_pct) return Pressure::LOW;
  return Pressure::HOLD;
}

// Behind the linear schedule to the target: done / total < elapsed / target.
// Unfinished work at or past the target is always behind.
inline bool behind(const PacerConfig& c, u64 started_ns, const Signals& g) {
  if (c.target_ns == 0 || g.done >= g.total) return false;
  const u64 elapsed = g.now_ns > started_ns ? g.now_ns - started_ns : 0;
  if (elapsed >= c.target_ns) return true;
  return static_cast<unsigned __int128>(g.done) * c.target_ns <
         static_cast<unsigned __int128>(g.total) * elapsed;
}

// One controller step. Age, target and pressure are evaluated on every
// call (they are reports); the budget changes at most once per update_ns:
//  - catch-up (behind the schedule): +step, up to the ceiling, whatever
//    the pressure (DESIGN-R 7.2: catch-up may exceed what pressure allows,
//    never the ceiling);
//  - normal: first back within [min, max]; then HIGH halves (not below
//    min), LOW adds a step (not above max), HOLD keeps it.
// So the budget is always within [walk_min_ns, walk_ceiling_ns].
inline PacerState step(const PacerConfig& c, PacerState s, const Signals& g) {
  const u64 elapsed = g.now_ns > s.started_ns ? g.now_ns - s.started_ns : 0;
  if (c.target_ns != 0 && elapsed > c.target_ns) s.past_target = true;
  if (c.max_age_ns != 0 && elapsed >= c.max_age_ns) {
    s.age = AgeState::DEGRADED;
  } else if (c.max_age_ns != 0 &&
             static_cast<unsigned __int128>(elapsed) * 100 >= static_cast<unsigned __int128>(c.max_age_ns) * c.warn_pct) {
    s.age = AgeState::WARN;
  } else {
    s.age = AgeState::OK;
  }
  s.pressure = pressure(c, g);
  s.mode = behind(c, s.started_ns, g) ? Mode::CATCH_UP : Mode::NORMAL;
  if (g.now_ns - s.last_update_ns < c.update_ns) return s;
  const u64 before = s.walk_ns;
  if (s.mode == Mode::CATCH_UP) {
    s.walk_ns = s.walk_ns + c.walk_step_ns < c.walk_ceiling_ns ? s.walk_ns + c.walk_step_ns : c.walk_ceiling_ns;
  } else {
    if (s.walk_ns > c.walk_max_ns) s.walk_ns = c.walk_max_ns;
    if (s.pressure == Pressure::HIGH) {
      s.walk_ns = s.walk_ns / 2 > c.walk_min_ns ? s.walk_ns / 2 : c.walk_min_ns;
    } else if (s.pressure == Pressure::LOW) {
      s.walk_ns = s.walk_ns + c.walk_step_ns < c.walk_max_ns ? s.walk_ns + c.walk_step_ns : c.walk_max_ns;
    }
  }
  if (s.walk_ns < c.walk_min_ns) s.walk_ns = c.walk_min_ns;
  s.last_update_ns = g.now_ns;
  if (s.walk_ns != before) ++s.changes;
  return s;
}

}  // namespace snc::ckpt
