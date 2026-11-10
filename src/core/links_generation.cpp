#include "links_generation.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "snc/wal_format.h"

namespace snc {

namespace {
constexpr u32 kMagic = 0x47434e53;  // "SNCG"
constexpr u32 kVersion = 1;

struct GenFile {
  u32 magic;
  u32 version;
  u64 generation;
  u32 crc;  // crc32c over the bytes before it
  u32 zero;
};
static_assert(sizeof(GenFile) == 24);

std::atomic<int> g_crash_after{0};
std::atomic<u64> g_test_counter{0};

std::string dir_of(const std::string& path) {
  const auto k = path.rfind('/');
  return k == std::string::npos ? std::string(".") : path.substr(0, k);
}

bool fsync_dir(const std::string& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}
}  // namespace

void LinksGeneration::debug_crash_after(int stage) { g_crash_after.store(stage); }

std::unique_ptr<LinksGeneration> LinksGeneration::test_allocator() {
  std::unique_ptr<LinksGeneration> g(new LinksGeneration());
  g->source_ = Source::TEST;
  g->path_ = "(test allocator)";
  return g;
}

std::unique_ptr<LinksGeneration> LinksGeneration::open_file(const std::string& path, bool fresh, u64 min) {
  std::unique_ptr<LinksGeneration> g(new LinksGeneration());
  g->path_ = path;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno != ENOENT) throw std::runtime_error("links generation: cannot open " + path + ": " + std::strerror(errno));
    if (!fresh && min == 0) {
      throw std::runtime_error(
          "links generation: " + path +
          " is missing in a data directory that already holds state; refusing to guess a lower links incarnation "
          "(peers would refuse it). Restart with --links-generation-min G, G above the highest incarnation the "
          "peers report for this member (GAUGES xnode_highest_incarnation, or their refusal log)");
    }
    g->value_ = 0;
  } else {
    GenFile f{};
    const ssize_t n = ::read(fd, &f, sizeof(f));
    ::close(fd);
    if (n != static_cast<ssize_t>(sizeof(f)) || f.magic != kMagic || f.version != kVersion ||
        f.crc != crc32c(&f, offsetof(GenFile, crc)) || f.zero != 0) {
      throw std::runtime_error("links generation: " + path +
                               " is truncated or corrupt; refusing to start (never reused or reset: restart with "
                               "--links-generation-min G after establishing a safe floor)");
    }
    g->value_ = f.generation;
  }
  if (min > g->value_) {
    std::fprintf(stderr, "snc: links generation: raising %s from %llu to the operator floor %llu "
                 "(--links-generation-min)\n", path.c_str(), (unsigned long long)g->value_, (unsigned long long)min);
    if (!g->store(min)) throw std::runtime_error("links generation: cannot store the floor in " + path);
    g->value_ = min;
  } else if (fd < 0) {
    if (!g->store(0)) throw std::runtime_error("links generation: cannot create " + path);
  }
  return g;
}

// tmp, fsync, rename, fsync the directory: durable before it returns.
bool LinksGeneration::store(u64 v) {
  GenFile f{};
  f.magic = kMagic;
  f.version = kVersion;
  f.generation = v;
  f.crc = crc32c(&f, offsetof(GenFile, crc));
  const std::string tmp = path_ + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const bool wrote = ::write(fd, &f, sizeof(f)) == static_cast<ssize_t>(sizeof(f));
  if (g_crash_after.load() == 1) {
    ::close(fd);
    return false;
  }
  const bool synced = wrote && ::fsync(fd) == 0;
  ::close(fd);
  if (!synced || g_crash_after.load() == 2) return false;
  if (::rename(tmp.c_str(), path_.c_str()) != 0) return false;
  if (g_crash_after.load() == 3) return false;
  return fsync_dir(dir_of(path_));
}

bool LinksGeneration::next(u64& out) {
  std::lock_guard<std::mutex> g(mu_);
  if (source_ == Source::TEST) {
    out = g_test_counter.fetch_add(1) + 1;
    value_ = out;
    return true;
  }
  if (value_ == ~u64{0}) {
    std::fprintf(stderr, "snc: links generation: %s is exhausted\n", path_.c_str());
    return false;
  }
  const u64 v = value_ + 1;
  if (!store(v)) {
    std::fprintf(stderr, "snc: links generation: cannot store %llu in %s (%s): no links instance\n",
                 (unsigned long long)v, path_.c_str(), std::strerror(errno));
    return false;
  }
  value_ = v;
  out = v;
  return true;
}

u64 LinksGeneration::last() const {
  std::lock_guard<std::mutex> g(mu_);
  return value_;
}

}  // namespace snc
