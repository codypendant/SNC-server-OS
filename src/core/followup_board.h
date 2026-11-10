// Follow-up intake accounting shared by the shards of one Engine
// (docs/FOLLOWUP_INTAKE_DESIGN.md, D.1: sections 3.2, 3.4 and 3.9).
//
// Two N x N matrices of counters, both indexed [writer][other]:
// - taken[d][s]: follow-ups from shard s that shard d has taken out of its
//   staging (run, dropped or held). Written only by d; read by s, whose
//   per-pair intake window W is sent - taken.
// - ended[x][o]: exchanges started by shard o that ended on shard x.
//   Written only by x; read by o, whose live exchanges are
//   started - sum over x of ended[x][o].
// - wait[s][d]: s found its window to d full and waits for d to take
//   something (set by s, cleared by d, which then wakes s).
// Each writer's row is padded to its own cache lines, so a writer never
// shares a line with another writer. Counters only grow; a reader that
// sees an old value only overestimates what is in flight.
#pragma once

#include <atomic>
#include <memory>
#include <new>

#include "snc/types.h"

namespace snc {

class FollowupBoard {
 public:
  explicit FollowupBoard(u16 n) : n_(n), stride_(round_up(n)) {
    const std::size_t cells = std::size_t{n_} * stride_;
    taken_.reset(new (std::align_val_t{64}) std::atomic<u64>[cells]);
    ended_.reset(new (std::align_val_t{64}) std::atomic<u64>[cells]);
    wait_.reset(new (std::align_val_t{64}) std::atomic<u64>[cells]);
    for (std::size_t i = 0; i < cells; ++i) {
      taken_[i].store(0, std::memory_order_relaxed);
      ended_[i].store(0, std::memory_order_relaxed);
      wait_[i].store(0, std::memory_order_relaxed);
    }
  }
  u16 size() const { return n_; }
  std::atomic<u64>& taken(u16 writer, u16 other) { return taken_[std::size_t{writer} * stride_ + other]; }
  std::atomic<u64>& ended(u16 writer, u16 other) { return ended_[std::size_t{writer} * stride_ + other]; }
  std::atomic<u64>& wait(u16 writer, u16 other) { return wait_[std::size_t{writer} * stride_ + other]; }

 private:
  // A row of u64 counters, rounded to whole 64-byte lines (8 per line).
  static std::size_t round_up(u16 n) { return (std::size_t{n} + 7) & ~std::size_t{7}; }
  u16 n_;
  std::size_t stride_;
  struct Free {
    void operator()(std::atomic<u64>* p) const { ::operator delete[](p, std::align_val_t{64}); }
  };
  // Lines of 64 bytes: rows start on a line boundary.
  std::unique_ptr<std::atomic<u64>[], Free> taken_, ended_, wait_;
};

}  // namespace snc
