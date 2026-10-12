// Helpers shared by snc_bench and baseline_bench.
#pragma once

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "snc/types.h"

namespace snc {

// Log-linear latency histogram: 2^-5 relative precision, ns resolution.
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
  }
  void merge(const Histogram& o) {
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
    n_ += o.n_;
  }
  u64 count() const { return n_; }
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
