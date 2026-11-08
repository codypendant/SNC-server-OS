// S3-D micro-characterization (docs/S3D_WRITEBACK.md): how each candidate
// checkpoint writeback mechanism interacts with WAL durability latency.
// Architectural evidence only, not campaign evidence: no engine code, no
// SNC format, plain syscalls on threads.
//
//   ckpt_writeback_char run DIR MODE RATE_MBPS TOTAL_MIB [WINDOW] [CHUNK_KIB]
//     MODE: none | legacy | c1 | c2 | c3
//       none    WAL writer only (baseline)
//       legacy  buffered pwrite, no early writeback, final fdatasync (today)
//       c1      buffered pwrite + sync_file_range(WRITE) per chunk, and
//               sync_file_range(WAIT_BEFORE|WRITE|WAIT_AFTER) on the chunk
//               WINDOW back (caps dirty bytes), final fdatasync
//       c2      O_DIRECT aligned pwrite, last chunk padded, ftruncate to the
//               logical length, final fdatasync
//       c3      buffered pwritev2(RWF_DSYNC) per chunk, final fdatasync
//     RATE_MBPS 0 = as fast as the mechanism allows; else chunk k is not
//     issued before start + k * chunk / rate (the walk's production rate).
//     WINDOW = checkpoint writes in flight (writer threads), default 4.
//
//   ckpt_writeback_char probe DIR   error and filesystem behaviour
//
// The WAL writer models the engine's WAL (O_DIRECT + RWF_DSYNC, 4 KiB
// appends, one record slot every 250 us, group commit) on the same device. Each WAL sample is
// tagged with the checkpoint phase it started in: pre, write, sync, post.
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using u64 = unsigned long long;

u64 now_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

void sleep_until_ns(u64 t) {
  for (;;) {
    const u64 n = now_ns();
    if (n >= t) return;
    const u64 d = t - n;
    if (d > 200'000) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(d - 100'000));
    } else {
      std::this_thread::yield();
    }
  }
}

[[noreturn]] void die(const std::string& what) {
  std::fprintf(stderr, "ckpt_writeback_char: %s: %s\n", what.c_str(), std::strerror(errno));
  std::exit(1);
}

void* aligned(std::size_t n) {
  void* p = std::aligned_alloc(4096, (n + 4095) / 4096 * 4096);
  if (!p) die("aligned_alloc");
  std::memset(p, 0x5a, (n + 4095) / 4096 * 4096);
  return p;
}

// Dirty + Writeback from /proc/meminfo, in KiB (system-wide; the VM is
// otherwise idle during a run).
u64 dirty_kib() {
  std::ifstream f("/proc/meminfo");
  std::string k;
  u64 v = 0, sum = 0;
  std::string unit;
  while (f >> k >> v >> unit) {
    if (k == "Dirty:" || k == "Writeback:") sum += v;
  }
  return sum;
}

enum Phase : int { PRE = 0, WRITE = 1, SYNC = 2, POST = 3, kPhases = 4 };
const char* kPhaseName[kPhases] = {"pre", "write", "sync", "post"};

std::atomic<int> g_phase{PRE};
std::atomic<bool> g_stop{false};

struct WalStats {
  std::vector<u64> lat[kPhases];
  u64 flushes[kPhases] = {0, 0, 0, 0};
  bool direct = false;
};

void wal_thread(const std::string& dir, WalStats* st) {
  const std::string path = dir + "/wal";
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_DIRECT, 0644);
  st->direct = fd >= 0;
  if (fd < 0 && errno == EINVAL) fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) die("open wal");
  constexpr u64 kSpan = 64ull << 20;
  if (::posix_fallocate(fd, 0, kSpan) != 0) die("fallocate wal");
  ::fdatasync(fd);
  void* blk = aligned(4096);
  constexpr u64 kPeriod = 250'000;
  u64 off = 0;
  u64 next = now_ns();
  while (!g_stop.load(std::memory_order_relaxed)) {
    sleep_until_ns(next);
    const int ph = g_phase.load(std::memory_order_relaxed);
    iovec iov{blk, 4096};
    const u64 t0 = now_ns();
    // Group commit: one write covers every record slot due by now, as the
    // engine's WAL batches whatever accumulated while the last flush ran.
    u64 last = next;
    while (last + kPeriod <= t0) last += kPeriod;
    if (::pwritev2(fd, &iov, 1, static_cast<off_t>(off), RWF_DSYNC) != 4096) die("wal pwritev2");
    const u64 t1 = now_ns();
    // Open loop: each record's latency counts from its scheduled slot, so
    // a stalled flush charges every record that waited behind it.
    for (u64 s = next; s <= last; s += kPeriod) st->lat[ph].push_back(t1 - s);
    ++st->flushes[ph];
    off = (off + 4096) % kSpan;
    next = last + kPeriod;
  }
  ::close(fd);
  ::unlink(path.c_str());
  std::free(blk);
}

struct Sampler {
  u64 max_kib[kPhases] = {0, 0, 0, 0};
};

void sampler_thread(Sampler* s) {
  while (!g_stop.load(std::memory_order_relaxed)) {
    const int ph = g_phase.load(std::memory_order_relaxed);
    const u64 d = dirty_kib();
    if (d > s->max_kib[ph]) s->max_kib[ph] = d;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

u64 pct(std::vector<u64>& v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  std::size_t i = static_cast<std::size_t>(p * (v.size() - 1));
  return v[i];
}

u64 thread_cpu_us() {
  rusage r{};
  ::getrusage(RUSAGE_THREAD, &r);
  return static_cast<u64>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1'000'000 + r.ru_utime.tv_usec + r.ru_stime.tv_usec;
}

int run(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: ckpt_writeback_char run DIR MODE RATE_MBPS TOTAL_MIB [WINDOW] [CHUNK_KIB]\n");
    return 2;
  }
  const std::string dir = argv[2];
  const std::string mode = argv[3];
  const u64 rate = std::strtoull(argv[4], nullptr, 10);
  const u64 total_mib = std::strtoull(argv[5], nullptr, 10);
  const u64 window = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 4;
  const u64 chunk = (argc > 7 ? std::strtoull(argv[7], nullptr, 10) : 1024) * 1024;
  if (mode != "none" && mode != "legacy" && mode != "c1" && mode != "c2" && mode != "c3") {
    std::fprintf(stderr, "bad MODE\n");
    return 2;
  }
  if (window < 1 || chunk % 4096 != 0 || chunk == 0) {
    std::fprintf(stderr, "WINDOW >= 1, CHUNK_KIB a multiple of 4\n");
    return 2;
  }
  // The logical length is deliberately not a block multiple, so C2 must
  // pad the last chunk and truncate (docs/CHECKPOINT_BACKPRESSURE_DESIGN.md 5.3).
  const u64 logical = total_mib * (1ull << 20) - 123;
  const u64 n_chunks = (logical + chunk - 1) / chunk;

  ::sync();
  WalStats wal;
  Sampler smp;
  std::thread wt(wal_thread, dir, &wal);
  std::thread st(sampler_thread, &smp);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const std::string path = dir + "/part";
  u64 write_ns = 0, sync_ns = 0, cpu_us = 0;
  std::atomic<u64> cpu_sum{0};
  bool direct = false;
  if (mode != "none") {
    int flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    int fd = -1;
    if (mode == "c2") {
      fd = ::open(path.c_str(), flags | O_DIRECT, 0644);
      direct = fd >= 0;
      if (fd < 0 && errno == EINVAL) {
        std::fprintf(stderr, "c2: O_DIRECT refused (EINVAL); buffered fallback\n");
        fd = ::open(path.c_str(), flags, 0644);
      }
    } else {
      fd = ::open(path.c_str(), flags, 0644);
    }
    if (fd < 0) die("open part");
    g_phase.store(WRITE);
    const u64 t0 = now_ns();
    std::atomic<u64> next_chunk{0};
    auto writer = [&]() {
      void* buf = aligned(chunk);
      const u64 c0 = thread_cpu_us();
      for (;;) {
        const u64 k = next_chunk.fetch_add(1);
        if (k >= n_chunks) break;
        if (rate) sleep_until_ns(t0 + k * chunk * 1000 / rate);  // ns: bytes * 1e3 / (MB/s)
        const u64 off = k * chunk;
        u64 len = std::min(chunk, logical - off);
        if (mode == "c2" && direct) len = (len + 4095) / 4096 * 4096;  // pad the tail block
        iovec iov{buf, static_cast<std::size_t>(len)};
        const int rwf = mode == "c3" ? RWF_DSYNC : 0;
        const ssize_t w = ::pwritev2(fd, &iov, 1, static_cast<off_t>(off), rwf);
        if (w != static_cast<ssize_t>(len)) die("part pwritev2");
        if (mode == "c1") {
          if (::sync_file_range(fd, static_cast<off_t>(off), static_cast<off_t>(len), SYNC_FILE_RANGE_WRITE) != 0)
            die("sync_file_range WRITE");
          if (k >= window) {
            const u64 back = (k - window) * chunk;
            if (::sync_file_range(fd, static_cast<off_t>(back), static_cast<off_t>(chunk),
                                  SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) != 0)
              die("sync_file_range WAIT");
          }
        }
      }
      cpu_sum.fetch_add(thread_cpu_us() - c0);
      std::free(buf);
    };
    std::vector<std::thread> ws;
    for (u64 i = 0; i < window; ++i) ws.emplace_back(writer);
    for (auto& t : ws) t.join();
    if (mode == "c2" && direct) {
      if (::ftruncate(fd, static_cast<off_t>(logical)) != 0) die("ftruncate");
    }
    const u64 t1 = now_ns();
    g_phase.store(SYNC);
    if (::fdatasync(fd) != 0) die("fdatasync part");
    const u64 t2 = now_ns();
    g_phase.store(POST);
    write_ns = t1 - t0;
    sync_ns = t2 - t1;
    cpu_us = cpu_sum.load();
    struct stat sb{};
    ::fstat(fd, &sb);
    if (static_cast<u64>(sb.st_size) != logical) {
      std::fprintf(stderr, "size %lld != logical %llu\n", static_cast<long long>(sb.st_size), logical);
      return 1;
    }
    ::close(fd);
  } else {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    g_phase.store(POST);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  g_stop.store(true);
  wt.join();
  st.join();
  if (mode != "none") ::unlink(path.c_str());

  std::printf("mode=%s rate_mbps=%llu total_mib=%llu window=%llu chunk_kib=%llu wal_direct=%d part_direct=%d "
              "write_ms=%.1f sync_ms=%.1f mbps=%.0f writer_cpu_ms=%.1f",
              mode.c_str(), rate, total_mib, window, chunk / 1024, wal.direct ? 1 : 0, direct ? 1 : 0, write_ns / 1e6,
              sync_ns / 1e6, write_ns ? (logical / 1e6) / ((write_ns + sync_ns) / 1e9) : 0.0, cpu_us / 1e3);
  for (int p = 0; p < kPhases; ++p) {
    auto& v = wal.lat[p];
    if (v.empty()) continue;
    const std::size_t n = v.size();
    std::printf(" %s_flushes=%llu", kPhaseName[p], wal.flushes[p]);
    std::printf(" %s_n=%zu %s_p50us=%.0f %s_p99us=%.0f %s_p999us=%.0f %s_maxus=%.0f %s_dirty_max_mib=%.0f", kPhaseName[p],
                n, kPhaseName[p], pct(v, 0.5) / 1e3, kPhaseName[p], pct(v, 0.99) / 1e3, kPhaseName[p], pct(v, 0.999) / 1e3,
                kPhaseName[p], pct(v, 1.0) / 1e3, kPhaseName[p], smp.max_kib[p] / 1024.0);
  }
  std::printf("\n");
  return 0;
}

// What each mechanism does on a filesystem, and how its errors surface.
void probe_dir(const std::string& dir) {
  const std::string p = dir + "/probe";
  std::printf("dir=%s\n", dir.c_str());
  int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_DIRECT, 0644);
  std::printf("  c2 open O_DIRECT: %s\n", fd >= 0 ? "ok" : std::strerror(errno));
  if (fd >= 0) {
    void* a = aligned(8192);
    ssize_t w = ::pwrite(fd, static_cast<char*>(a) + 1, 4096, 0);
    std::printf("  c2 misaligned buffer: %s\n", w < 0 ? std::strerror(errno) : "accepted");
    w = ::pwrite(fd, a, 100, 0);
    std::printf("  c2 short length (100 B): %s\n", w < 0 ? std::strerror(errno) : "accepted");
    w = ::pwrite(fd, a, 4096, 1);
    std::printf("  c2 misaligned offset: %s\n", w < 0 ? std::strerror(errno) : "accepted");
    w = ::pwrite(fd, a, 4096, 0);
    std::printf("  c2 aligned 4096: %s\n", w == 4096 ? "ok" : std::strerror(errno));
    std::printf("  c2 ftruncate to 1000: %s\n", ::ftruncate(fd, 1000) == 0 ? "ok" : std::strerror(errno));
    std::free(a);
    ::close(fd);
  }
  fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) die("open probe");
  char b[4096];
  std::memset(b, 1, sizeof(b));
  iovec iov{b, sizeof(b)};
  ssize_t w = ::pwritev2(fd, &iov, 1, 0, RWF_DSYNC);
  std::printf("  c3 pwritev2 RWF_DSYNC: %s\n", w == 4096 ? "ok" : std::strerror(errno));
  int r = ::sync_file_range(fd, 0, 4096, SYNC_FILE_RANGE_WRITE);
  std::printf("  c1 sync_file_range WRITE: %s\n", r == 0 ? "ok" : std::strerror(errno));
  r = ::sync_file_range(fd, 0, 4096, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
  std::printf("  c1 sync_file_range WAIT: %s\n", r == 0 ? "ok" : std::strerror(errno));
  ::close(fd);
  ::unlink(p.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "probe") {
    for (int i = 2; i < argc; ++i) probe_dir(argv[i]);
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "run") return run(argc, argv);
  std::fprintf(stderr, "usage: ckpt_writeback_char run|probe ...\n");
  return 2;
}
