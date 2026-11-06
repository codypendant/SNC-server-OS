// Helpers shared by snc_bench and baseline_bench.
#pragma once

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "snc/types.h"

namespace snc {

// Log-linear latency histogram. Values under 32 ns are exact; above that each power of
// two is split into 32 buckets, so a bucket spans at most 1/32 (3.125%) of its
// lower edge. percentile() returns the LOWER edge of the bucket that holds the
// percentile: the true value v satisfies r <= v < r * (1 + 1/32). For
// example a reported p99 of 1,015,808 ns means 1,015,808 <= p99 < 1,032,192.
// max() is exact.
class Histogram {
 public:
  Histogram() : counts_(64 * 32, 0) {}
  void add(u64 ns) {
    if (ns < 32) {
      ++counts_[ns];
    } else {
      int msb = 63 - __builtin_clzll(ns);
      u64 sub = (ns >> (msb - 5)) & 31;
      ++counts_[(msb - 4) * 32 + sub];
    }
    ++n_;
    if (ns > max_) max_ = ns;
  }
  void merge(const Histogram& o) {
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
    n_ += o.n_;
    if (o.max_ > max_) max_ = o.max_;
  }
  u64 count() const { return n_; }
  u64 max() const { return max_; }
  double percentile(double p) const {
    u64 target = static_cast<u64>(std::ceil(p / 100.0 * n_));
    u64 acc = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      acc += counts_[i];
      if (acc >= target && counts_[i]) return bucket_value(i);
    }
    return 0;
  }

 private:
  static double bucket_value(std::size_t i) {
    if (i < 32) return double(i);
    int msb = int(i / 32) + 4;
    u64 sub = i % 32;
    return double((u64{32} + sub) << (msb - 5));
  }
  std::vector<u64> counts_;
  u64 n_ = 0;
  u64 max_ = 0;
};

// Resident set size of this process, from /proc/self/statm.
inline u64 rss_bytes() {
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  unsigned long pages = 0, rss = 0;
  int n = std::fscanf(f, "%lu %lu", &pages, &rss);
  std::fclose(f);
  return n == 2 ? rss * static_cast<u64>(sysconf(_SC_PAGESIZE)) : 0;
}

}  // namespace snc
