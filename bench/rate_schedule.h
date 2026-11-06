// Open-loop arrival schedule for net_bench --rate (docs/CP1A_DRAFT_REVIEW.md
// section 4; the PR #61 review). Pure integer arithmetic, so the schedule is
// exact and the same on every run; tests/test_rate_schedule.cpp checks it.
//
// The total offered rate R (operations per second, all threads together) is
// divided over T threads: thread i gets R / T, plus one if i < R % T, so the
// thread rates add up to R exactly. Thread i's k-th operation (k = 0, 1, ...)
// is due at
//
//     at(k) = phase + floor(k * 1e9 / rate)        nanoseconds after t0
//
// with phase = floor(i * 1e9 / (rate * T)): the threads' periodic streams are
// spread evenly over one period instead of all arriving together. Arrivals are
// deterministic and periodic; Poisson arrivals would be a separate, named
// workload dimension.
#pragma once

#include <cstdint>

namespace snc::bench {

constexpr std::uint64_t kNsPerS = 1000000000ull;

// Thread i's share of a total rate.
constexpr std::uint64_t thread_rate(std::uint64_t total, std::uint32_t threads, std::uint32_t i) {
  return threads == 0 ? 0 : total / threads + (i < total % threads ? 1 : 0);
}

struct RateSchedule {
  std::uint64_t rate = 0;   // this thread's operations per second (0: none)
  std::uint64_t phase = 0;  // ns after t0 of operation 0

  RateSchedule() = default;
  RateSchedule(std::uint64_t total, std::uint32_t threads, std::uint32_t i)
      : rate(thread_rate(total, threads, i)),
        phase(rate == 0 ? 0
                        : static_cast<std::uint64_t>((unsigned __int128)i * kNsPerS /
                                                     ((unsigned __int128)rate * threads))) {}

  // When operation k is due, in ns after t0.
  std::uint64_t at(std::uint64_t k) const {
    return phase + static_cast<std::uint64_t>((unsigned __int128)k * kNsPerS / rate);
  }

  // How many operations are due strictly before t ns after t0: the number of
  // k with at(k) < t. Since at(k) < t  <=>  k * 1e9 < (t - phase) * rate,
  // that is ceil((t - phase) * rate / 1e9).
  std::uint64_t due_before(std::uint64_t t) const {
    if (rate == 0 || t <= phase) return 0;
    const unsigned __int128 d = t - phase;
    return static_cast<std::uint64_t>((d * rate + kNsPerS - 1) / kNsPerS);
  }
};

}  // namespace snc::bench
