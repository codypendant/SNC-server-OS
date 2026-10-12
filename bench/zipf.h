// Zipf key choice for the benchmarks (`--zipf THETA`), as in YCSB: rank r of
// n is drawn with probability proportional to 1 / r^theta, using Gray et
// al.'s constant-time method ("Quickly generating billion-record synthetic
// databases", SIGMOD 1994). Ranks map to objects through one shuffled list
// built with a fixed seed, so every driver agrees on which objects are hot
// and the hot ones fall on owners in proportion to how many objects each
// owns.
#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "snc/object_id.h"

namespace snc::bench {

class Zipf {
 public:
  Zipf() = default;
  Zipf(u64 n, double theta) : n_(n), theta_(theta) {
    double zeta2 = 0;
    for (u64 i = 1; i <= n; ++i) {
      double t = 1.0 / std::pow(static_cast<double>(i), theta);
      zetan_ += t;
      if (i <= 2) zeta2 += t;
    }
    alpha_ = 1.0 / (1.0 - theta);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n), 1.0 - theta)) / (1.0 - zeta2 / zetan_);
  }

  // A rank in [0, n): 0 is the hottest.
  template <class Rng>
  u64 next(Rng& rng) const {
    double u = static_cast<double>(rng() >> 11) * 0x1.0p-53;
    double uz = u * zetan_;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
    u64 r = static_cast<u64>(static_cast<double>(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
    return std::min(r, n_ - 1);
  }

  // Share of draws that land on the hottest rank.
  double top_share() const { return 1.0 / zetan_; }

 private:
  u64 n_ = 1;
  double theta_ = 0;
  double zetan_ = 0, alpha_ = 0, eta_ = 0;
};

// All IDs in one list, shuffled with a fixed seed: index = Zipf rank.
inline std::vector<ObjectID> zipf_order(const std::vector<std::vector<ObjectID>>& groups) {
  std::vector<ObjectID> all;
  for (const auto& g : groups) all.insert(all.end(), g.begin(), g.end());
  std::sort(all.begin(), all.end(), [](const ObjectID& a, const ObjectID& b) {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
  });
  std::mt19937_64 rng(42);
  std::shuffle(all.begin(), all.end(), rng);
  return all;
}

}  // namespace snc::bench
