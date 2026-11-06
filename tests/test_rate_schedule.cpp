// The open-loop schedule of net_bench --rate (bench/rate_schedule.h).
#include <cstdint>
#include <vector>

#include "../bench/rate_schedule.h"
#include "check.h"

using snc::bench::kNsPerS;
using snc::bench::RateSchedule;
using snc::bench::thread_rate;

int main() {
  // Division over threads: the remainder goes to the lowest threads, and the
  // shares add up to the total exactly.
  for (std::uint64_t total : {0ull, 1ull, 7ull, 999ull, 1000ull, 1234567ull, 3000001ull}) {
    for (std::uint32_t threads : {1u, 2u, 3u, 8u, 16u}) {
      std::uint64_t sum = 0;
      for (std::uint32_t i = 0; i < threads; ++i) {
        const std::uint64_t r = thread_rate(total, threads, i);
        CHECK(r == total / threads || r == total / threads + 1);
        CHECK_EQ(r == total / threads + 1, i < total % threads);
        sum += r;
      }
      CHECK_EQ(sum, total);
    }
  }
  CHECK_EQ(thread_rate(10, 0, 0), 0u);

  // Exact due times: 3 ops/s on one thread are due at 0, 333333333, 666666666 ns.
  RateSchedule s3(3, 1, 0);
  CHECK_EQ(s3.phase, 0u);
  CHECK_EQ(s3.at(0), 0u);
  CHECK_EQ(s3.at(1), 333333333u);
  CHECK_EQ(s3.at(2), 666666666u);
  CHECK_EQ(s3.at(3), kNsPerS);
  CHECK_EQ(s3.at(3000), 1000 * kNsPerS);  // no drift over many periods

  // due_before(t) counts exactly the k with at(k) < t, for many rates,
  // threads and times (including each due time and the ns either side).
  for (std::uint64_t total : {1ull, 3ull, 1000ull, 77777ull, 1000000ull, 2500000ull}) {
    for (std::uint32_t threads : {1u, 3u, 8u}) {
      for (std::uint32_t i = 0; i < threads; ++i) {
        RateSchedule s(total, threads, i);
        if (s.rate == 0) {
          CHECK_EQ(s.due_before(kNsPerS * 100), 0u);
          continue;
        }
        CHECK(s.phase < kNsPerS / s.rate + 1);  // within one period
        for (std::uint64_t k = 0; k < 2000; ++k) {
          const std::uint64_t t = s.at(k);
          CHECK(k == 0 || s.at(k - 1) <= t);  // non-decreasing
          CHECK_EQ(s.due_before(t), k);       // op k is not yet due at its own time...
          CHECK_EQ(s.due_before(t + 1), k + 1);  // ...and is one ns later
          if (t > 0) CHECK(s.due_before(t - 1) <= k);
        }
        // Over one second exactly `rate` ops are due (phase < period).
        CHECK_EQ(s.due_before(s.phase + kNsPerS), s.rate);
        CHECK_EQ(s.due_before(s.phase + 10 * kNsPerS), 10 * s.rate);
      }
    }
  }

  // Phase offsets spread the threads over one period: 4 threads at 4000
  // ops/s total (1000 each, period 1 ms) start at 0, 250, 500, 750 us.
  std::vector<std::uint64_t> want = {0, 250000, 500000, 750000};
  for (std::uint32_t i = 0; i < 4; ++i) CHECK_EQ(RateSchedule(4000, 4, i).phase, want[i]);

  // Totals over a window add up across threads: for a window of W seconds,
  // the threads' due counts add to R * W.
  for (std::uint64_t total : {1000ull, 999983ull, 1250000ull}) {
    const std::uint32_t threads = 8;
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < threads; ++i) sum += RateSchedule(total, threads, i).due_before(30 * kNsPerS);
    // Each phase is under one period, so ceil((30 s - phase) * rate / 1e9)
    // is exactly 30 * rate: the shares add up to R * W with nothing lost.
    CHECK_EQ(sum, total * 30);
  }

  // Large values do not overflow: 10M ops/s for a day.
  RateSchedule big(10000000, 1, 0);
  const std::uint64_t day = 86400ull * kNsPerS;
  CHECK_EQ(big.due_before(day), 864000000000ull);
  CHECK_EQ(big.at(864000000000ull), day);

  std::puts("test_rate_schedule ok");
  return 0;
}
