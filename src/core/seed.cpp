// Seeding a standby (docs/REPLICATION.md). The primary's seed server runs on
// its own thread; the standby fetches a seed during recovery, before its
// shards start.
#include "seed.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "snc/storage.h"

namespace snc {

namespace {

bool send_all(int fd, const void* p, std::size_t n) {
  const u8* b = static_cast<const u8*>(p);
  while (n > 0) {
    ssize_t w = ::send(fd, b, n, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    b += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

bool recv_all(int fd, void* p, std::size_t n) {
  u8* b = static_cast<u8*>(p);
  while (n > 0) {
    ssize_t r = ::recv(fd, b, n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    b += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}

void set_timeouts(int fd, int seconds) {
  timeval tv{seconds, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

std::vector<std::string> files_in(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return out;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n != "." && n != ".." && n.find(".tmp") == std::string::npos) out.push_back(n);
  }
  ::closedir(d);
  return out;
}

// Streams one file. Returns false if the connection failed.
bool send_file(int sock, const std::string& dir, const std::string& name) {
  std::string path = dir + "/" + name;
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  ::fstat(fd, &st);
  SeedFile f{static_cast<u32>(name.size()), 0, static_cast<u64>(st.st_size)};
  bool ok = send_all(sock, &f, sizeof(f)) && send_all(sock, name.data(), name.size());
  std::vector<u8> buf(1u << 20);
  u64 left = f.size;
  while (ok && left > 0) {
    ssize_t r = ::read(fd, buf.data(), std::min<u64>(left, buf.size()));
    if (r <= 0) {
      ok = false;
      break;
    }
    ok = send_all(sock, buf.data(), static_cast<std::size_t>(r));
    left -= static_cast<u64>(r);
  }
  ::close(fd);
  return ok;
}

void remove_ckpt_dir(const std::string& dir) {
  for (const std::string& f : files_in(dir)) ::unlink((dir + "/" + f).c_str());
  ::rmdir(dir.c_str());
}

struct FdGuard {
  int fd;
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
};

}  // namespace

// ---- Primary: the seed server ---------------------------------------------------

void Engine::serve_seed() {
  Seeding& sd = *seed_;
  while (!sd.stop.load()) {
    pollfd p{sd.listen_fd, POLLIN, 0};
    if (::poll(&p, 1, 200) <= 0) continue;
    int c = ::accept(sd.listen_fd, nullptr, nullptr);
    if (c < 0) continue;
    set_timeouts(c, 60);
    SeedRequest req;
    SeedHeader h{};
    std::memcpy(h.magic, kSeedMagic, 8);
    std::memcpy(h.space_uuid, uuid_.data(), 16);
    if (!recv_all(c, &req, sizeof(req)) || std::memcmp(req.magic, kSeedMagic, 8) != 0) {
      ::close(c);
      continue;
    }
    static const u8 zero[16] = {};
    u64 seq;
    {
      std::lock_guard<std::mutex> g(sd.mu);
      seq = sb_.active_ckpt;
      h.seq = seq;
      h.next_lease = sb_.next_lease;
      if (std::memcmp(req.space_uuid, zero, 16) != 0 &&
          std::memcmp(req.space_uuid, uuid_.data(), 16) != 0) {
        h.status = 1;
      } else if (seq == 0) {
        h.status = 2;
      } else {
        // Keep this checkpoint, and the WAL from its start, until the
        // standby has caught up past it (or for an hour).
        ++sd.ckpt_pins[seq];
        std::optional<Manifest> m = read_manifest(cfg_.data_dir, seq);
        for (u16 s = 0; m && s < sd.wal_pin.size() && s < m->start_lsn.size(); ++s) {
          u64 pin = m->start_lsn[s];
          if (sd.wal_pin[s] == 0 || pin < sd.wal_pin[s]) sd.wal_pin[s] = pin;
        }
        sd.wal_pin_until = std::chrono::steady_clock::now() + std::chrono::hours(1);
      }
    }
    if (h.status != 0) {
      send_all(c, &h, sizeof(h));
      ::close(c);
      continue;
    }
    const std::string dir = ckpt_dir(cfg_.data_dir, seq);
    std::vector<std::string> files = files_in(dir);
    h.n_files = static_cast<u32>(files.size());
    bool ok = send_all(c, &h, sizeof(h));
    for (const std::string& f : files) {
      if (!ok) break;
      ok = send_file(c, dir, f);
    }
    u8 done = 0;
    if (ok) ok = recv_all(c, &done, 1);  // the standby has written and synced it all
    std::fprintf(stderr, "snc: seed of checkpoint %llu %s\n", (unsigned long long)seq,
                 ok ? "sent" : "failed");
    ::close(c);
    std::lock_guard<std::mutex> g(sd.mu);
    if (--sd.ckpt_pins[seq] == 0) {
      sd.ckpt_pins.erase(seq);
      for (std::size_t i = 0; i < sd.deferred.size(); ++i) {
        if (sd.deferred[i] != seq) continue;
        remove_ckpt_dir(ckpt_dir(cfg_.data_dir, seq));
        sd.deferred.erase(sd.deferred.begin() + static_cast<long>(i));
        break;
      }
    }
  }
}

// ---- Standby: fetching a seed ------------------------------------------------

void Engine::seed_fetch() {
  const std::string& dir = cfg_.data_dir;
  const std::string& from = cfg_.seed_from;
  auto colon = from.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("seed_from takes host:port");
  std::string host = from.substr(0, colon), port = from.substr(colon + 1);
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
    throw std::runtime_error("seed: cannot resolve " + from);
  }
  int c = -1;
  // The primary may still be starting: retry for a while.
  for (int attempt = 0; attempt < 600; ++attempt) {
    c = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (::connect(c, res->ai_addr, res->ai_addrlen) == 0) break;
    ::close(c);
    c = -1;
    ::usleep(100000);
  }
  ::freeaddrinfo(res);
  if (c < 0) throw std::runtime_error("seed: cannot connect to " + from);
  FdGuard guard{c};
  set_timeouts(c, 60);
  SeedRequest req{};
  std::memcpy(req.magic, kSeedMagic, 8);
  std::memcpy(req.space_uuid, cfg_.init_uuid.data(), 16);
  SeedHeader h;
  if (!send_all(c, &req, sizeof(req)) || !recv_all(c, &h, sizeof(h)) ||
      std::memcmp(h.magic, kSeedMagic, 8) != 0) {
    throw std::runtime_error("seed: no answer from " + from);
  }
  if (h.status != 0) {
    throw std::runtime_error(h.status == 1 ? "seed: the primary is another space"
                                           : "seed: the primary has no checkpoint yet");
  }
  const std::string cdir = ckpt_dir(dir, h.seq);
  make_dirs(cdir);
  std::vector<u8> buf(1u << 20);
  u64 total = 0;
  for (u32 i = 0; i < h.n_files; ++i) {
    SeedFile f;
    std::string name;
    if (!recv_all(c, &f, sizeof(f)) || f.name_len == 0 || f.name_len > 255) {
      throw std::runtime_error("seed: transfer failed");
    }
    name.resize(f.name_len);
    if (!recv_all(c, name.data(), name.size()) || name.find('/') != std::string::npos) {
      throw std::runtime_error("seed: transfer failed");
    }
    std::string path = cdir + "/" + name;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) storage_fatal("seed: cannot create " + path);
    u64 left = f.size;
    while (left > 0) {
      std::size_t n = static_cast<std::size_t>(std::min<u64>(left, buf.size()));
      if (!recv_all(c, buf.data(), n)) throw std::runtime_error("seed: transfer failed");
      if (::write(fd, buf.data(), n) != static_cast<ssize_t>(n)) storage_fatal("seed: write failed");
      left -= n;
    }
    if (::fsync(fd) != 0) storage_fatal("seed: fsync failed");
    ::close(fd);
    total += f.size;
  }
  fsync_dir(cdir);
  fsync_dir(dir + "/ckpt");
  // Only now does the directory become a space: a crash before this leaves
  // no superblock, and the next start fetches again.
  Uuid uuid;
  std::memcpy(uuid.data(), h.space_uuid, 16);
  Superblock sb = new_superblock(uuid);
  sb.next_lease = h.next_lease;
  sb.active_ckpt = h.seq;
  sb.generation = 1;
  write_superblock(dir, sb);
  u8 done = 1;
  send_all(c, &done, 1);
  std::fprintf(stderr, "snc: seeded from %s: checkpoint %llu, %u files, %.1f MiB\n", from.c_str(),
               (unsigned long long)h.seq, h.n_files, total / 1048576.0);
}

}  // namespace snc
