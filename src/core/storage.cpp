#include "snc/storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "snc/sri.h"

namespace snc {

namespace {

constexpr char kSuperMagic[8] = {'S', 'N', 'C', 'S', 'U', 'P', 'R', '1'};
constexpr char kWalMagic[8] = {'S', 'N', 'C', 'W', 'A', 'L', '0', '1'};
constexpr char kCkptMagic[8] = {'S', 'N', 'C', 'C', 'K', 'P', '0', '1'};
constexpr char kManifestMagic[8] = {'S', 'N', 'C', 'M', 'A', 'N', 'I', '1'};

std::string hex16(u64 v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

u8* aligned_block(std::size_t bytes) {
  void* p = std::aligned_alloc(kBlock, (bytes + kBlock - 1) / kBlock * kBlock);
  if (p == nullptr) storage_fatal("out of memory for I/O buffer");
  return static_cast<u8*>(p);
}

bool read_file(const std::string& path, std::vector<u8>& out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return false;
  }
  out.resize(static_cast<std::size_t>(st.st_size));
  std::size_t got = 0;
  while (got < out.size()) {
    ssize_t n = ::pread(fd, out.data() + got, out.size() - got, static_cast<off_t>(got));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      ::close(fd);
      storage_fatal("read failed: " + path);
    }
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return true;
}

void write_all_sync(int fd, const void* data, std::size_t len, off_t off, const std::string& what) {
  const u8* p = static_cast<const u8*>(data);
  while (len > 0) {
    ssize_t n = ::pwrite(fd, p, len, off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) storage_fatal("write failed: " + what);
    p += n;
    len -= static_cast<std::size_t>(n);
    off += n;
  }
  if (::fdatasync(fd) != 0) storage_fatal("fdatasync failed: " + what);
}

bool header_ok(const FileHeader& h, const char (&magic)[8], u16 stream, const Uuid& uuid) {
  FileHeader c = h;
  c.crc32c = 0;
  return std::memcmp(h.magic, magic, 8) == 0 && h.format == kFormatVersion &&
         h.stream == stream && crc32c(&c, sizeof(c)) == h.crc32c &&
         std::memcmp(h.space_uuid, uuid.data(), 16) == 0;
}

FileHeader make_header(const char (&magic)[8], u16 stream, u64 seq, u64 first_lsn,
                       const Uuid& uuid) {
  FileHeader h{};
  std::memcpy(h.magic, magic, 8);
  h.format = kFormatVersion;
  h.stream = stream;
  h.seq = seq;
  h.first_lsn = first_lsn;
  std::memcpy(h.space_uuid, uuid.data(), 16);
  h.crc32c = crc32c(&h, sizeof(h));
  return h;
}

bool all_zero(const u8* p, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i]) return false;
  }
  return true;
}

}  // namespace

void storage_fatal(const std::string& what) {
  std::fprintf(stderr, "snc storage: %s (errno %d)\n", what.c_str(), errno);
  std::abort();
}

// ---- Paths ---------------------------------------------------------------------

std::string stream_dir(const std::string& d, u16 stream) {
  char b[16];
  std::snprintf(b, sizeof(b), "s%03u", stream);
  return d + "/wal/" + b;
}

std::string segment_path(const std::string& d, u16 stream, u64 first_lsn) {
  return stream_dir(d, stream) + "/" + hex16(first_lsn) + ".wal";
}

std::string ckpt_dir(const std::string& d, u64 seq) { return d + "/ckpt/" + hex16(seq); }

std::string part_path(const std::string& d, u64 seq, u16 stream) {
  char b[24];
  std::snprintf(b, sizeof(b), "/part-%03u.ckpt", stream);
  return ckpt_dir(d, seq) + b;
}

void fsync_dir(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) storage_fatal("open dir failed: " + path);
  if (::fsync(fd) != 0) storage_fatal("fsync dir failed: " + path);
  ::close(fd);
}

void make_dirs(const std::string& path) {
  std::string cur;
  std::size_t i = 0;
  while (i <= path.size()) {
    std::size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    cur = path.substr(0, j);
    if (!cur.empty()) {
      if (::mkdir(cur.c_str(), 0755) == 0) {
        std::size_t slash = cur.rfind('/');
        fsync_dir(slash == std::string::npos || slash == 0 ? (slash == 0 ? "/" : ".")
                                                            : cur.substr(0, slash));
      } else if (errno != EEXIST) {
        storage_fatal("mkdir failed: " + cur);
      }
    }
    i = j + 1;
  }
}

namespace {
std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> out;
  DIR* d = ::opendir(path.c_str());
  if (d == nullptr) return out;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n != "." && n != "..") out.push_back(n);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

std::vector<u16> list_streams(const std::string& d) {
  std::vector<u16> out;
  for (const auto& n : list_dir(d + "/wal")) {
    if (n.size() == 4 && n[0] == 's') out.push_back(static_cast<u16>(std::atoi(n.c_str() + 1)));
  }
  return out;
}

std::vector<u64> list_segments(const std::string& d, u16 stream) {
  std::vector<u64> out;
  for (const auto& n : list_dir(stream_dir(d, stream))) {
    if (n.size() == 20 && n.substr(16) == ".wal") {
      out.push_back(std::strtoull(n.substr(0, 16).c_str(), nullptr, 16));
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---- Superblock ------------------------------------------------------------------

Uuid random_uuid() {
  Uuid u;
  if (::getrandom(u.data(), u.size(), 0) != static_cast<ssize_t>(u.size())) {
    storage_fatal("getrandom failed");
  }
  return u;
}

Superblock new_superblock(const Uuid& uuid) {
  Superblock sb{};
  std::memcpy(sb.magic, kSuperMagic, 8);
  sb.format = kFormatVersion;
  sb.generation = 0;
  std::memcpy(sb.space_uuid, uuid.data(), 16);
  sb.next_lease = 1;  // lease 0 is reserved
  sb.active_ckpt = 0;
  return sb;
}

std::optional<Superblock> read_superblock(const std::string& d) {
  std::optional<Superblock> best;
  for (const char* name : {"/superblock.a", "/superblock.b"}) {
    std::vector<u8> buf;
    if (!read_file(d + name, buf) || buf.size() != kBlock) continue;
    Superblock sb;
    std::memcpy(&sb, buf.data(), sizeof(sb));
    u32 stored = sb.crc32c;
    std::memset(buf.data() + offsetof(Superblock, crc32c), 0, 4);
    if (std::memcmp(sb.magic, kSuperMagic, 8) != 0 || sb.format != kFormatVersion ||
        crc32c(buf.data(), kBlock) != stored) {
      continue;
    }
    if (!best || sb.generation > best->generation) best = sb;
  }
  return best;
}

void write_superblock(const std::string& d, const Superblock& sb_in) {
  Superblock sb = sb_in;
  u8* block = aligned_block(kBlock);
  std::memset(block, 0, kBlock);
  sb.crc32c = 0;
  std::memcpy(block, &sb, sizeof(sb));
  sb.crc32c = crc32c(block, kBlock);
  std::memcpy(block, &sb, sizeof(sb));
  std::string path = d + ((sb.generation % 2) ? "/superblock.b" : "/superblock.a");
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) storage_fatal("open superblock failed");
  write_all_sync(fd, block, kBlock, 0, path);
  ::close(fd);
  std::free(block);
  fsync_dir(d);
}

// ---- Manifest ----------------------------------------------------------------------

namespace {
struct Out {
  std::vector<u8> b;
  template <typename T>
  void put(const T& v) {
    const u8* p = reinterpret_cast<const u8*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
  }
  void bytes(const void* p, std::size_t n) {
    const u8* q = static_cast<const u8*>(p);
    b.insert(b.end(), q, q + n);
  }
};
struct In {
  const std::vector<u8>& b;
  std::size_t off = 0;
  bool ok = true;
  template <typename T>
  T get() {
    T v{};
    if (off + sizeof(T) > b.size()) {
      ok = false;
      return v;
    }
    std::memcpy(&v, b.data() + off, sizeof(T));
    off += sizeof(T);
    return v;
  }
  void bytes(void* p, std::size_t n) {
    if (off + n > b.size()) {
      ok = false;
      return;
    }
    std::memcpy(p, b.data() + off, n);
    off += n;
  }
};
}  // namespace

void write_manifest(const std::string& d, const Manifest& m) {
  Out o;
  o.bytes(kManifestMagic, 8);
  o.put(kFormatVersion);
  o.put(m.seq);
  o.bytes(m.space_uuid.data(), 16);
  o.put(m.n_shards);
  o.put(m.slots.epoch);
  o.put(m.slots.n_shards);
  o.bytes(m.slots.owner.data(), sizeof(u16) * kSlots);
  o.put(static_cast<u32>(m.start_lsn.size()));
  for (u64 l : m.start_lsn) o.put(l);
  o.put(static_cast<u32>(m.types.size()));
  for (const auto& t : m.types) {
    o.put(t.type_id);
    o.put(t.schema_ver);
    o.put(static_cast<u16>(t.name.size()));
    o.bytes(t.name.data(), t.name.size());
  }
  o.put(static_cast<u32>(m.parts.size()));
  for (const auto& p : m.parts) {
    o.put(p.stream);
    o.put(p.objects);
    o.put(p.bytes);
  }
  o.put(crc32c(o.b.data(), o.b.size()));

  std::string dir = ckpt_dir(d, m.seq);
  std::string tmp = dir + "/manifest.tmp", fin = dir + "/manifest";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) storage_fatal("open manifest failed");
  write_all_sync(fd, o.b.data(), o.b.size(), 0, tmp);
  ::close(fd);
  if (::rename(tmp.c_str(), fin.c_str()) != 0) storage_fatal("rename manifest failed");
  fsync_dir(dir);
}

std::optional<Manifest> read_manifest(const std::string& d, u64 seq) {
  std::vector<u8> b;
  if (!read_file(ckpt_dir(d, seq) + "/manifest", b) || b.size() < 12) return std::nullopt;
  u32 stored;
  std::memcpy(&stored, b.data() + b.size() - 4, 4);
  if (crc32c(b.data(), b.size() - 4) != stored) return std::nullopt;
  In in{b};
  char magic[8] = {};
  in.bytes(magic, 8);
  if (std::memcmp(magic, kManifestMagic, 8) != 0 || in.get<u16>() != kFormatVersion) {
    return std::nullopt;
  }
  Manifest m;
  m.seq = in.get<u64>();
  in.bytes(m.space_uuid.data(), 16);
  m.n_shards = in.get<u16>();
  m.slots.epoch = in.get<u32>();
  m.slots.n_shards = in.get<u16>();
  in.bytes(m.slots.owner.data(), sizeof(u16) * kSlots);
  m.start_lsn.resize(in.get<u32>());
  for (auto& l : m.start_lsn) l = in.get<u64>();
  m.types.resize(in.get<u32>());
  for (auto& t : m.types) {
    t.type_id = in.get<u32>();
    t.schema_ver = in.get<u16>();
    t.name.resize(in.get<u16>());
    in.bytes(t.name.data(), t.name.size());
  }
  m.parts.resize(in.get<u32>());
  for (auto& p : m.parts) {
    p.stream = in.get<u16>();
    p.objects = in.get<u64>();
    p.bytes = in.get<u64>();
  }
  if (!in.ok || in.off != b.size() - 4) return std::nullopt;
  return m;
}

// ---- Reading -------------------------------------------------------------------------

StreamScan scan_stream(const std::string& d, u16 stream, u64 from_lsn, const Uuid& uuid,
                       const RecordFn& fn) {
  StreamScan out;
  std::vector<u64> segs = list_segments(d, stream);
  u64 expected = 0;  // next LSN the stream must continue with; 0 = not started
  std::vector<u8> buf;
  for (std::size_t si = 0; si < segs.size(); ++si) {
    // Covered entirely by the checkpoint (the next segment starts at or
    // before from_lsn): not read. Deleted after the next checkpoint anyway.
    if (si + 1 < segs.size() && segs[si + 1] <= from_lsn) continue;
    std::string path = segment_path(d, stream, segs[si]);
    read_file(path, buf);
    FileHeader h{};
    if (buf.size() >= sizeof(h)) std::memcpy(&h, buf.data(), sizeof(h));
    if (buf.size() < kBlock || !header_ok(h, kWalMagic, stream, uuid)) {
      // A crash while creating the newest segment leaves it empty or zero.
      if (si + 1 == segs.size() && all_zero(buf.data(), buf.size())) break;
      storage_fatal("bad WAL segment header: " + path);
    }
    if (expected != 0 && h.first_lsn != expected) {
      storage_fatal("WAL gap before segment (spec C4): " + path);
    }
    std::size_t off = kBlock;
    while (off + sizeof(WalRecord) <= buf.size()) {
      u32 len = check_record(buf.data() + off, buf.size() - off);
      if (len == 0) {
        // A whole zero block is the unwritten rest of the segment: flushes
        // are block-padded and only one is in flight per stream, so nothing
        // written can follow it.
        if (off % kBlock == 0 && all_zero(buf.data() + off, std::min(off + kBlock, buf.size()) - off)) {
          break;
        }
        // Zero padding to the end of a flushed block: skip to the next block.
        std::size_t block_end = (off / kBlock + 1) * kBlock;
        if (!all_zero(buf.data() + off, std::min(block_end, buf.size()) - off)) {
          ++out.torn_tails;
          break;
        }
        off = block_end;
        continue;
      }
      WalRecord r;
      std::memcpy(&r, buf.data() + off, sizeof(r));
      if (expected == 0) {
        if (from_lsn > 0 && r.lsn > from_lsn) storage_fatal("WAL missing records before " + path);
      } else if (r.lsn != expected) {
        ++out.torn_tails;
        break;  // hole: the rest of this segment was never acknowledged
      }
      expected = r.lsn + 1;
      out.last_lsn = r.lsn;
      if (r.lsn >= from_lsn) {
        if (out.first_lsn == 0) out.first_lsn = r.lsn;
        ++out.records;
        out.bytes += len;
        fn(r, buf.data() + off + sizeof(WalRecord));
      }
      off += len;
    }
  }
  return out;
}

u64 scan_part(const std::string& d, u64 seq, u16 stream, const Uuid& uuid, const RecordFn& fn) {
  std::string path = part_path(d, seq, stream);
  std::vector<u8> buf;
  if (!read_file(path, buf)) storage_fatal("missing checkpoint part: " + path);
  FileHeader h{};
  if (buf.size() < kBlock) storage_fatal("short checkpoint part: " + path);
  std::memcpy(&h, buf.data(), sizeof(h));
  if (!header_ok(h, kCkptMagic, stream, uuid) || h.seq != seq) {
    storage_fatal("bad checkpoint part header: " + path);
  }
  u64 n = 0;
  std::size_t off = kBlock;
  while (off < buf.size()) {
    u32 len = check_record(buf.data() + off, buf.size() - off);
    if (len == 0) storage_fatal("corrupt checkpoint part: " + path);
    WalRecord r;
    std::memcpy(&r, buf.data() + off, sizeof(r));
    if (r.rec_type != static_cast<u16>(RecType::IMAGE)) storage_fatal("non-image in part: " + path);
    fn(r, buf.data() + off + sizeof(WalRecord));
    ++n;
    off += len;
  }
  return n;
}

// ---- WAL writer -------------------------------------------------------------------

WalStream::WalStream(const WalConfig& cfg, u16 stream, u64 next_lsn)
    : cfg_(cfg), stream_(stream), next_lsn_(next_lsn), durable_lsn_(next_lsn - 1) {
  active_cap_ = flushing_cap_ = 2u << 20;  // fits the largest record (1 MiB payload)
  active_ = aligned_block(active_cap_);
  flushing_ = aligned_block(flushing_cap_);
  make_dirs(stream_dir(cfg_.data_dir, stream_));
  // Recovery found no valid record at or after next_lsn, so a segment that
  // already carries this name holds only a header, zeros or an unacknowledged
  // tail (a crash right after a rollover, or a restart with no writes since
  // the last one). Replace it rather than tripping over O_EXCL.
  ::unlink(segment_path(cfg_.data_dir, stream_, next_lsn_).c_str());
  open_segment(next_lsn_);
}

WalStream::~WalStream() {
  if (fd_ >= 0) ::close(fd_);
  std::free(active_);
  std::free(flushing_);
}

void WalStream::open_segment(u64 first_lsn) {
  if (fd_ >= 0) ::close(fd_);
  std::string path = segment_path(cfg_.data_dir, stream_, first_lsn);
  // O_EXCL: a segment name is a first LSN and is never reused, so an
  // existing file means two writers or a bug.
  int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
  fd_ = -1;
  if (cfg_.direct) {
    fd_ = ::open(path.c_str(), flags | O_DIRECT, 0644);
    if (fd_ < 0 && errno == EINVAL) {
      ::unlink(path.c_str());  // the filesystem refused O_DIRECT; use buffered I/O
    }
  }
  direct_ = fd_ >= 0;
  if (fd_ < 0) fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) storage_fatal("create WAL segment failed: " + path);
  // Reserve the space so steady-state writes do not extend the file. The
  // spec asks for pre-zeroed segments created ahead by the control thread;
  // that is a later optimization (docs/STATUS.md).
  if (::fallocate(fd_, 0, 0, static_cast<off_t>(cfg_.segment_bytes)) != 0 &&
      ::ftruncate(fd_, static_cast<off_t>(cfg_.segment_bytes)) != 0) {
    storage_fatal("size WAL segment failed: " + path);
  }
  u8* block = aligned_block(kBlock);
  std::memset(block, 0, kBlock);
  FileHeader h = make_header(kWalMagic, stream_, 0, first_lsn, cfg_.uuid);
  std::memcpy(block, &h, sizeof(h));
  write_all_sync(fd_, block, kBlock, 0, path);
  std::free(block);
  if (::fsync(fd_) != 0) storage_fatal("fsync WAL segment failed: " + path);
  fsync_dir(stream_dir(cfg_.data_dir, stream_));
  seg_off_ = kBlock;
  ++stats_.segments;
}

void WalStream::ensure_capacity(u32 extra) {
  if (active_len_ + extra + kBlock <= active_cap_) return;
  u32 cap = active_cap_;
  while (active_len_ + extra + kBlock > cap) cap *= 2;
  u8* bigger = aligned_block(cap);
  std::memcpy(bigger, active_, active_len_);
  std::free(active_);
  active_ = bigger;
  active_cap_ = cap;
}

u64 WalStream::append(RecType type, u32 type_id, const ObjectID& id, u64 version, u16 schema_ver,
                      const u8* payload, u32 payload_len) {
  u32 size = record_size(payload_len);
  ensure_capacity(size);
  u64 lsn = next_lsn_++;
  encode_record(active_ + active_len_, type, lsn, type_id, id, version, schema_ver, payload,
                payload_len);
  if (active_len_ == 0) {
    active_first_lsn_ = lsn;
    active_since_ns_ = sri_now_ns();
  }
  active_len_ += size;
  active_last_lsn_ = lsn;
  ++stats_.records;
  stats_.record_bytes += size;
  return lsn;
}

bool WalStream::poll(u64 now_ns, bool force) {
  if (!inflight_ && active_len_ > 0 &&
      (force || active_len_ >= cfg_.flush_bytes ||
       now_ns - active_since_ns_ >= u64{cfg_.group_commit_us} * 1000)) {
    start_flush();
  }
  return active_len_ > 0;
}

void WalStream::start_flush() {
  u32 padded = (active_len_ + kBlock - 1) / kBlock * kBlock;
  std::memset(active_ + active_len_, 0, padded - active_len_);
  if (seg_off_ > kBlock && seg_off_ + padded > cfg_.segment_bytes) open_segment(active_first_lsn_);
  std::swap(active_, flushing_);
  std::swap(active_cap_, flushing_cap_);
  flushing_len_ = padded;
  flushing_last_lsn_ = active_last_lsn_;
  active_len_ = 0;
  inflight_ = true;
  u64 off = seg_off_;
  seg_off_ += padded;
  stats_.written_bytes += padded;
  ++stats_.flushes;
  sri_write(fd_, off, flushing_, padded, cfg_.sync, &WalStream::flush_done, this);
}

void WalStream::flush_done(void* ctx, int result) {
  auto* w = static_cast<WalStream*>(ctx);
  if (result != static_cast<int>(w->flushing_len_)) {
    errno = result < 0 ? -result : EIO;
    storage_fatal("WAL write failed (spec C7: fail-stop)");
  }
  w->inflight_ = false;
  w->durable_lsn_ = w->flushing_last_lsn_;
  if (w->on_durable) w->on_durable(w->on_durable_ctx);
}

}  // namespace snc
