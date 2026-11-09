// WalStream active-buffer growth: checked wide arithmetic, an explicit
// supported maximum, and a fail-stop before any u32 size could wrap
// (docs/FOLLOWUP_INTAKE_DESIGN.md section 1, the u32 finding; review of
// PR #83). No test allocates more than a few MiB: the boundaries near 2^31
// and 2^32 are checked on the pure growth function.
//
//   C1 grown_capacity at the boundaries (no allocation): doubling, exact
//      fit, need = max, need = max + 1, operands near 2^32 and 2^64, a
//      capacity whose next doubling passes max, a non-power-of-two max
//   C2 append() fills a 4 MiB-max stream exactly to its maximum
//   C3 append() past the maximum is a fail-stop (SIGABRT, a message), and
//      the old u32 doubling would have wrapped for the same sizes
//   C4 append_raw() (the standby's path) past the maximum: the same
//   C5 a max_active_bytes outside [initial, hard max] is refused at open
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "check.h"
#include "snc/storage.h"
#include "snc/wal_format.h"

using namespace snc;

namespace {

constexpr u64 kMax = 4ull << 20;  // the tests' stream maximum
constexpr u32 kPayload = 64u << 10;

std::string fresh_dir() {
  char t[] = "/tmp/snc-walcap-XXXXXX";
  CHECK(::mkdtemp(t) != nullptr);
  return t;
}

void rm_rf(const std::string& d) {
  const std::string cmd = "rm -rf '" + d + "'";
  CHECK_EQ(std::system(cmd.c_str()), 0);
}

WalConfig config(const std::string& dir, u64 max) {
  WalConfig c;
  c.data_dir = dir;
  c.segment_bytes = 8ull << 20;
  c.sync = false;
  c.direct = false;
  c.max_active_bytes = max;
  return c;
}

// Runs `child` in a fork with stderr captured; returns (signal, stderr).
std::pair<int, std::string> in_child(const std::string& dir, const std::function<void()>& child) {
  std::fflush(stdout);
  const std::string err = dir + "/stderr";
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    const int efd = ::open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (efd < 0 || ::dup2(efd, 2) < 0) std::_Exit(3);
    ::alarm(60);
    child();
    std::_Exit(0);
  }
  int ws = 0;
  CHECK_EQ(::waitpid(pid, &ws, 0), pid);
  std::string text;
  if (FILE* f = std::fopen(err.c_str(), "r")) {
    char buf[4096];
    std::size_t k;
    while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, k);
    std::fclose(f);
  }
  return {WIFSIGNALED(ws) ? WTERMSIG(ws) : 0, text};
}

// The growth before the fix, in u32, as storage.cpp had it: true if it
// would wrap (or loop forever on 0) for these sizes.
bool old_u32_wraps(u32 cap, u32 len, u32 extra) {
  const u32 need = len + extra + kBlock;  // the old expression, in u32
  if (u64{len} + extra + kBlock != need) return true;
  for (int i = 0; i < 40 && need > cap; ++i) {
    if (cap == 0) return true;
    cap *= 2;
  }
  return need > cap;
}

void c1_pure_boundaries() {
  using W = WalStream;
  const u64 two_mib = 2ull << 20, one_gib = 1ull << 30;
  // Fits as is: unchanged.
  CHECK_EQ(W::grown_capacity(two_mib, 0, 1u << 20, one_gib), two_mib);
  // Doubles to the first power of two that fits.
  CHECK_EQ(W::grown_capacity(two_mib, two_mib, 1, one_gib), 4ull << 20);
  CHECK_EQ(W::grown_capacity(two_mib, 100ull << 20, 0, one_gib), 128ull << 20);
  // need == max: fits exactly, at max.
  CHECK_EQ(W::grown_capacity(two_mib, one_gib - kBlock - 8, 8, one_gib), one_gib);
  // need == max + 1: refused.
  CHECK_EQ(W::grown_capacity(two_mib, one_gib - kBlock - 8, 9, one_gib), 0u);
  // A capacity whose next doubling passes max (cap = 2^30, max = 2^30 + 4 MiB).
  CHECK_EQ(W::grown_capacity(one_gib, one_gib, 1u << 20, one_gib + (4ull << 20)), one_gib + (4ull << 20));
  // Non-power-of-two max: clamped to max, not past it.
  CHECK_EQ(W::grown_capacity(two_mib, 5ull << 20, 0, 6ull << 20), 6ull << 20);
  // Operands near 2^32 (the old u32 wrap) and 2^64: refused, no wrap.
  CHECK_EQ(W::grown_capacity(1ull << 31, 0xFFFFFF00ull, 0x200, one_gib), 0u);
  CHECK_EQ(W::grown_capacity(two_mib, 0, 0xFFFFFFFFull, one_gib), 0u);
  CHECK_EQ(W::grown_capacity(two_mib, ~u64{0}, ~u64{0}, one_gib), 0u);
  CHECK_EQ(W::grown_capacity(two_mib, ~u64{0} - kBlock, 1, ~u64{0}), 0u);
  // cap 0 (never in WalStream): still terminates.
  CHECK_EQ(W::grown_capacity(0, 10, 10, one_gib), 8192u);
  // The old u32 code wrapped exactly where these refuse.
  CHECK(old_u32_wraps(1u << 31, (1u << 31) - 1, 2u << 20));   // cap *= 2 -> 0, loops forever
  CHECK(old_u32_wraps(2u << 20, 0xFFFFFF00u, 0x200u));        // len + extra + kBlock wraps
  CHECK(!old_u32_wraps(2u << 20, 2u << 20, 1));               // an ordinary growth did not
  std::printf("C1 growth boundaries: exact fit at max, max+1 refused, no wrap near 2^31/2^32/2^64, clamped to "
              "a non-power-of-two max; the old u32 growth wraps on the same inputs\n");
}

// Appends kPayload records until `max` would be passed; returns the count
// and the buffered bytes when the next one would not fit.
void fill_to_max(WalStream& w, u64 max, u64& n, u64& bytes) {
  std::vector<u8> payload(kPayload, 0x5a);
  const u64 rec = record_size(kPayload);
  n = 0;
  bytes = 0;
  while (bytes + rec + kBlock <= max) {
    w.append(RecType::UPDATE, 300, ObjectID{1, n + 1}, n + 1, 1, payload.data(), kPayload);
    ++n;
    bytes += rec;
    CHECK_EQ(u64{w.active_bytes()}, bytes);
  }
}

void c2_fills_to_max() {
  const std::string dir = fresh_dir();
  {
    WalStream w(config(dir, kMax), 0, 1);
    u64 n = 0, bytes = 0;
    fill_to_max(w, kMax, n, bytes);
    CHECK(n > 0);
    CHECK(w.buffer_cap_bytes() <= 2 * kMax);  // active + flushing, each at most max
    // One more small record that still fits (exactly up to max) is accepted.
    const u64 room = kMax - kBlock - bytes;
    if (room >= record_size(0)) {
      const u32 p = static_cast<u32>((room - sizeof(WalRecord)) & ~u64{7});
      std::vector<u8> payload(p, 1);
      w.append(RecType::UPDATE, 300, ObjectID{2, 1}, 1, 1, payload.data(), p);
      CHECK(u64{w.active_bytes()} + kBlock <= kMax);
    }
    std::printf("C2 append: %llu records (%llu bytes) buffered, up to the %llu-byte maximum\n",
                (unsigned long long)n, (unsigned long long)w.active_bytes(), (unsigned long long)kMax);
  }
  rm_rf(dir);
}

void c3_append_past_max() {
  const std::string dir = fresh_dir();
  auto [sig, err] = in_child(dir, [&] {
    WalStream w(config(dir, kMax), 0, 1);
    u64 n = 0, bytes = 0;
    fill_to_max(w, kMax, n, bytes);
    std::vector<u8> payload(kPayload, 0x5a);
    w.append(RecType::UPDATE, 300, ObjectID{1, n + 1}, n + 1, 1, payload.data(), kPayload);  // past max
  });
  CHECK_EQ(sig, SIGABRT);
  CHECK(err.find("active buffer would exceed its supported maximum") != std::string::npos);
  // A payload near 4 GiB: record_size() in u32 would wrap to a small size;
  // the wide size is refused instead. No allocation of that size happens.
  auto [sig2, err2] = in_child(dir, [&] {
    WalStream w(config(dir, kMax), 1, 1);
    static u8 dummy[8];
    w.append(RecType::UPDATE, 300, ObjectID{1, 1}, 1, 1, dummy, 0xFFFFFFF8u);
  });
  CHECK_EQ(sig2, SIGABRT);
  CHECK(err2.find("active buffer would exceed its supported maximum") != std::string::npos);
  CHECK(record_size(0xFFFFFFF8u) < 64);  // the u32 size the old code would have used
  rm_rf(dir);
  std::printf("C3 append past the maximum: fail-stop (SIGABRT, message); a ~4 GiB payload too (u32 size "
              "would have wrapped to %u bytes)\n",
              record_size(0xFFFFFFF8u));
}

void c4_append_raw_past_max() {
  const std::string dir = fresh_dir();
  auto [sig, err] = in_child(dir, [&] {
    WalStream w(config(dir, kMax), 0, 1);
    std::vector<u8> payload(kPayload, 0x33);
    std::vector<u8> rec(record_size(kPayload));
    u64 lsn = 1;
    // A standby appends what it receives, verbatim, until the maximum.
    for (int i = 0; i < 1000; ++i) {
      encode_record(rec.data(), RecType::UPDATE, lsn, 300, ObjectID{3, lsn}, lsn, 1, payload.data(), kPayload);
      CHECK(w.append_raw(rec.data()));
      ++lsn;
      if (u64{w.active_bytes()} + rec.size() + kBlock > kMax) break;
    }
    std::printf("C4 child: %llu raw records buffered (%u bytes)\n", (unsigned long long)(lsn - 1), w.active_bytes());
    std::fflush(stdout);
    encode_record(rec.data(), RecType::UPDATE, lsn, 300, ObjectID{3, lsn}, lsn, 1, payload.data(), kPayload);
    w.append_raw(rec.data());  // past max
  });
  CHECK_EQ(sig, SIGABRT);
  CHECK(err.find("active buffer would exceed its supported maximum") != std::string::npos);
  rm_rf(dir);
  std::printf("C4 append_raw past the maximum: fail-stop (SIGABRT, message)\n");
}

void c5_config_refused() {
  const std::string dir = fresh_dir();
  for (u64 bad : {kWalActiveInitial - 1, kWalActiveHardMax + 1, u64{0}}) {
    auto [sig, err] = in_child(dir, [&] { WalStream w(config(dir, bad), 0, 1); });
    CHECK_EQ(sig, SIGABRT);
    CHECK(err.find("max_active_bytes") != std::string::npos);
  }
  CHECK_EQ(WalConfig{}.max_active_bytes, kWalActiveMaxDefault);
  CHECK(kWalActiveHardMax + kBlock < (1ull << 31));  // padded sizes stay far from the u32 limits
  rm_rf(dir);
  std::printf("C5 max_active_bytes outside [%llu, %llu] refused at open; default %llu\n",
              (unsigned long long)kWalActiveInitial, (unsigned long long)kWalActiveHardMax,
              (unsigned long long)kWalActiveMaxDefault);
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"C1", c1_pure_boundaries}, {"C2", c2_fills_to_max}, {"C3", c3_append_past_max},
      {"C4", c4_append_raw_past_max}, {"C5", c5_config_refused}};
  for (const auto& [name, fn] : tests) {
    bool run = argc == 1;
    for (int k = 1; k < argc; ++k) run |= std::string(argv[k]) == name;
    if (run) fn();
  }
  std::printf("test_wal_capacity: all passed\n");
  return 0;
}
