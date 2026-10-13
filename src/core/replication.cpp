#include "replication.h"

#include "snc/sri.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <thread>
#include <cstdio>
#include <cstring>

namespace snc::repl {

namespace {

constexpr u64 kRetryNs = 100'000'000;  // reconnect every 100 ms

void set_nonblock(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }

void set_nodelay(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Writes as much of [p, p + n) as the socket takes. Returns bytes written,
// or -1 if the connection failed.
long write_some(int fd, const u8* p, std::size_t n) {
  std::size_t done = 0;
  while (done < n) {
    ssize_t w = ::send(fd, p + done, n - done, MSG_NOSIGNAL);
    if (w > 0) {
      done += static_cast<std::size_t>(w);
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (w < 0 && errno == EINTR) continue;
    return -1;
  }
  return static_cast<long>(done);
}

// Wakes the shard from its idle sleep when `fd` is readable. A closed fd
// leaves the set by itself.
void watch(int epfd, int fd) {
  if (epfd < 0 || fd < 0) return;
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

}  // namespace

u32 slot_map_crc(const SlotMap& m) {
  return crc32c(m.owner.data(), m.owner.size() * sizeof(m.owner[0]), m.n_shards);
}

// ---- Sender -------------------------------------------------------------------

Sender::Sender(const Config& cfg) : cfg_(cfg), next_expected_(cfg.start_lsn) {
  std::memcpy(hello_.magic, kMagic, 8);
  std::memcpy(hello_.space_uuid, cfg_.uuid.data(), 16);
  hello_.shard = cfg_.shard;
  hello_.n_shards = cfg_.n_shards;
  hello_.slot_crc = cfg_.slot_crc;
}

Sender::~Sender() {
  stop_disk();
  if (fd_ >= 0) ::close(fd_);
}

// ---- Catch-up from WAL files ----------------------------------------------------

namespace {
constexpr std::size_t kDiskFrame = 1u << 20;      // records per frame read from disk
constexpr std::size_t kDiskQueued = 16u << 20;    // read ahead at most this much
}  // namespace

void Sender::start_disk(u64 from, u64 to, u32 verify_crc) {
  auto d = std::make_shared<DiskRead>();
  disk_ = d;
  disk_to_ = to;
  std::thread([d, dir = cfg_.data_dir, shard = cfg_.shard, uuid = cfg_.uuid, from, to, verify_crc] {
    // Not on the shard's core, which this thread inherited, and behind
    // the shards wherever it runs.
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int i = 0; i < CPU_SETSIZE && i < static_cast<int>(std::thread::hardware_concurrency()); ++i) {
      CPU_SET(i, &all);
    }
    pthread_setaffinity_np(pthread_self(), sizeof(all), &all);
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::gettid()), 10);
    std::vector<u8> f;
    u64 first = 0, last = 0;
    bool checking = verify_crc != 0, bad = false;
    auto push = [&] {
      if (f.empty()) return;
      RecordsHeader h{static_cast<u32>(f.size() - sizeof(RecordsHeader)), 0, first, last};
      std::memcpy(f.data(), &h, sizeof(h));
      std::unique_lock<std::mutex> g(d->mu);
      d->cv.wait(g, [&] { return d->bytes < kDiskQueued || d->cancel; });
      d->bytes += f.size();
      d->frames.push_back(std::move(f));
      f.clear();
    };
    bool ok = read_stream_range(dir, shard, checking ? from - 1 : from, to, uuid,
                                [&](const u8* rec, u32 len) {
      if (bad) return;
      if (checking) {
        u32 crc;
        std::memcpy(&crc, rec, 4);  // WalRecord::crc32c
        bad = crc != verify_crc;
        checking = false;
        return;
      }
      if (f.empty()) {
        f.resize(sizeof(RecordsHeader));
        std::memcpy(&first, rec + offsetof(WalRecord, lsn), 8);
      }
      std::memcpy(&last, rec + offsetof(WalRecord, lsn), 8);
      f.insert(f.end(), rec, rec + len);
      // Cut frames only between atomic groups: the standby applies whole frames.
      u16 flags;
      std::memcpy(&flags, rec + offsetof(WalRecord, flags), 2);
      if (f.size() >= kDiskFrame && !(flags & kRecGroupNext)) push();
    });
    if (!bad) push();
    std::lock_guard<std::mutex> g(d->mu);
    d->ok = ok && !bad;
    d->diverged = bad;
    d->done = true;
  }).detach();
}

void Sender::stop_disk() {
  if (disk_) {
    std::lock_guard<std::mutex> g(disk_->mu);
    disk_->cancel = true;
    disk_->cv.notify_all();
  }
  disk_.reset();
  disk_frame_.clear();
  disk_off_ = 0;
}

// Sends frames read from disk. Returns true once the catch-up is complete
// (the link may then go on to memory), false while it is still going or the
// link failed.
void Sender::apply_verified(bool& advanced) {
  if (verify_ack_ > acked_) {
    acked_ = verify_ack_;
    advanced = true;
  }
  verify_ack_ = 0;
}

bool Sender::send_disk(u64 now_ns, bool& advanced) {
  while (true) {
    if (disk_off_ < disk_frame_.size()) {
      long w = write_some(fd_, disk_frame_.data() + disk_off_, disk_frame_.size() - disk_off_);
      if (w < 0) {
        close_link(now_ns);
        return false;
      }
      disk_off_ += static_cast<std::size_t>(w);
      stats_.bytes_sent += static_cast<u64>(w);
      stats_.disk_bytes += static_cast<u64>(w);
      if (disk_off_ < disk_frame_.size()) return false;  // socket full
      ++stats_.frames_sent;
    }
    bool done, ok, div;
    {
      std::lock_guard<std::mutex> g(disk_->mu);
      if (!disk_->frames.empty()) {
        disk_frame_ = std::move(disk_->frames.front());
        disk_->frames.pop_front();
        disk_->bytes -= disk_frame_.size();
        disk_->cv.notify_all();
        disk_off_ = 0;
        apply_verified(advanced);  // the reader checked the tail before the first frame
        continue;
      }
      done = disk_->done;
      ok = disk_->ok;
      div = disk_->diverged;
    }
    if (!done) return false;
    stop_disk();
    if (div) {
      diverged(now_ns, 0);
      return false;
    }
    if (ok) apply_verified(advanced);
    if (!ok) {
      std::fprintf(stderr,
                   "snc: standby shard %u needs WAL records no longer on disk: reseed it\n",
                   cfg_.shard);
      lost_ = true;
      close_link(now_ns);
      return false;
    }
    // Records dropped from memory while this ran are on disk too.
    if (mem_first() > disk_to_) {
      start_disk(disk_to_, mem_first());
      continue;
    }
    return true;
  }
}

void Sender::on_flush(const u8* recs, u32 len, u64 first_lsn, u64 last_lsn) {
  if (len == 0) return;
  Chunk c;
  c.first_lsn = first_lsn;
  c.last_lsn = last_lsn;
  c.bytes.resize(sizeof(RecordsHeader) + len);
  RecordsHeader h{len, 0, first_lsn, last_lsn};
  std::memcpy(c.bytes.data(), &h, sizeof(h));
  std::memcpy(c.bytes.data() + sizeof(h), recs, len);
  retained_ += c.bytes.size();
  chunks_.push_back(std::move(c));
  next_expected_ = last_lsn + 1;
  // Over budget: drop the oldest records from memory. Every dropped chunk's
  // flush has completed (one is in flight at a time), so the standby can
  // still get them from disk.
  bool restart = false;
  u64 unsent_from = 0;
  while (retained_ > cfg_.retain_bytes && chunks_.size() > 1) {
    if (state_ == State::LIVE && !disk_ && send_chunk_ == 0 && chunks_.front().last_lsn > acked_) {
      if (send_off_ > 0 || partial_off_ < partial_.size()) restart = true;
      else if (unsent_from == 0) unsent_from = chunks_.front().first_lsn;
    }
    retained_ -= chunks_.front().bytes.size();
    chunks_.pop_front();
    if (send_chunk_ > 0) --send_chunk_;
    else send_off_ = 0;
  }
  stats_.retained_bytes = retained_;
  if (restart) {
    // A frame half sent is gone: start over from the standby's handshake.
    close_link(sri_now_ns());
  } else if (unsent_from != 0) {
    // Live, with unsent records gone from memory: send them from disk first.
    start_disk(unsent_from, mem_first());
  }
}

void Sender::trim() {
  while (!chunks_.empty() && chunks_.front().last_lsn <= acked_ && send_chunk_ > 0) {
    retained_ -= chunks_.front().bytes.size();
    chunks_.pop_front();
    --send_chunk_;
  }
  stats_.retained_bytes = retained_;
}

void Sender::close_link(u64 now_ns) {
  stop_disk();
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  if (state_ == State::LIVE) down_since_ = now_ns;
  state_ = State::DOWN;
  retry_at_ = now_ns + kRetryNs;
  in_len_ = 0;
  partial_.clear();
  partial_off_ = 0;
}

void Sender::start_connect(u64 now_ns) {
  if (down_since_ == 0) down_since_ = now_ns;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  std::string port = std::to_string(cfg_.port);
  if (::getaddrinfo(cfg_.host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
    retry_at_ = now_ns + kRetryNs;
    return;
  }
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  set_nonblock(fd_);
  set_nodelay(fd_);
  int r = ::connect(fd_, res->ai_addr, res->ai_addrlen);
  ::freeaddrinfo(res);
  if (r != 0 && errno != EINPROGRESS) {
    close_link(now_ns);
    return;
  }
  state_ = State::CONNECTING;
  watch(cfg_.epoll_fd, fd_);
  hello_off_ = 0;
  ++stats_.connects;
}

void Sender::diverged(u64 now_ns, u64 lsn) {
  std::fprintf(stderr,
               "snc: standby shard %u has records this primary does not%s%llu (a primary crash "
               "lost them after shipping): reseed it\n",
               cfg_.shard, lsn ? " past LSN " : "", (unsigned long long)lsn);
  lost_ = true;
  close_link(now_ns);
}

// Finds where to resume for a standby that lacks `next_lsn` and up, after
// checking that its last record (crc `tail_crc`) is ours.
void Sender::position_at(u64 next_lsn, u32 tail_crc, u64 now_ns) {
  partial_.clear();
  partial_off_ = 0;
  send_chunk_ = 0;
  send_off_ = 0;
  if (next_lsn > next_expected_) {
    diverged(now_ns, next_expected_ - 1);
    return;
  }
  const u64 tail = next_lsn - 1;
  if (tail != 0 && tail_crc != 0 && tail >= mem_first()) {
    // Its last record is in memory: check it here.
    for (const Chunk& c : chunks_) {
      if (tail < c.first_lsn || tail > c.last_lsn) continue;
      std::size_t off = sizeof(RecordsHeader);
      while (off < c.bytes.size()) {
        WalRecord r;
        std::memcpy(&r, c.bytes.data() + off, sizeof(r));
        if (r.lsn == tail) {
          if (r.crc32c != tail_crc) return diverged(now_ns, tail - 1);
          break;
        }
        off += r.total_len;
      }
      break;
    }
  } else if (tail != 0 && tail_crc != 0) {
    // On disk: the reader checks it before sending anything.
    start_disk(next_lsn, mem_first(), tail_crc);
    return;
  }
  if (next_lsn < mem_first()) {
    // Older than what memory holds: from disk first, then memory.
    start_disk(next_lsn, mem_first());
    return;
  }
  while (send_chunk_ < chunks_.size() && chunks_[send_chunk_].last_lsn < next_lsn) ++send_chunk_;
  if (send_chunk_ == chunks_.size()) return;  // nothing retained it lacks; send live from here
  Chunk& c = chunks_[send_chunk_];
  if (c.first_lsn == next_lsn) return;
  // Resume inside the chunk: skip whole records up to next_lsn.
  std::size_t off = sizeof(RecordsHeader);
  while (off < c.bytes.size()) {
    WalRecord r;
    std::memcpy(&r, c.bytes.data() + off, sizeof(r));
    if (r.lsn == next_lsn) break;
    off += r.total_len;
  }
  RecordsHeader h{static_cast<u32>(c.bytes.size() - off), 0, next_lsn, c.last_lsn};
  partial_.resize(sizeof(h) + h.len);
  std::memcpy(partial_.data(), &h, sizeof(h));
  std::memcpy(partial_.data() + sizeof(h), c.bytes.data() + off, h.len);
  ++send_chunk_;  // the partial frame replaces this chunk
}

bool Sender::read_acks(u64 now_ns) {
  bool advanced = false;
  while (true) {
    ssize_t r = ::recv(fd_, in_ + in_len_, sizeof(in_) - in_len_, 0);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      close_link(now_ns);
      return advanced;
    }
    if (r < 0) return advanced;
    in_len_ += static_cast<std::size_t>(r);
    if (state_ == State::HELLO) {
      if (in_len_ < sizeof(HelloAck)) continue;
      HelloAck a;
      std::memcpy(&a, in_, sizeof(a));
      std::memmove(in_, in_ + sizeof(a), in_len_ - sizeof(a));
      in_len_ -= sizeof(a);
      if (std::memcmp(a.magic, kMagic, 8) != 0 || a.status != 0) {
        std::fprintf(stderr, "snc: standby refused shard %u (status %u)\n", cfg_.shard, a.status);
        close_link(now_ns);
        retry_at_ = now_ns + 10 * kRetryNs;
        return advanced;
      }
      verify_ack_ = 0;
      position_at(a.next_lsn, a.last_crc, now_ns);
      if (lost_) return advanced;
      // It already has up to next_lsn - 1 durably: those count as acked once
      // they are known to be ours (at once, unless the disk reader checks).
      verify_ack_ = a.next_lsn > 0 ? a.next_lsn - 1 : 0;
      if (!disk_) apply_verified(advanced);
      state_ = State::LIVE;
      down_since_ = 0;
    }
    while (in_len_ >= sizeof(Ack)) {
      Ack k;
      std::memcpy(&k, in_, sizeof(k));
      std::memmove(in_, in_ + sizeof(k), in_len_ - sizeof(k));
      in_len_ -= sizeof(k);
      if (k.durable_lsn > acked_) {
        acked_ = k.durable_lsn;
        advanced = true;
      }
    }
  }
}

bool Sender::poll(u64 now_ns) {
  if (lost_) {
    if (fd_ >= 0) close_link(now_ns);  // so a sync primary times out and degrades
    return false;
  }
  bool advanced = false;
  switch (state_) {
    case State::DOWN:
      if (now_ns >= retry_at_) start_connect(now_ns);
      return false;
    case State::CONNECTING: {
      // Connected once the Hello can be written.
      long w = write_some(fd_, reinterpret_cast<const u8*>(&hello_) + hello_off_,
                          sizeof(hello_) - hello_off_);
      if (w < 0) {
        close_link(now_ns);
        return false;
      }
      hello_off_ += static_cast<std::size_t>(w);
      if (hello_off_ == sizeof(hello_)) state_ = State::HELLO;
      return false;
    }
    case State::HELLO:
      return read_acks(now_ns);
    case State::LIVE:
      break;
  }
  advanced |= read_acks(now_ns);
  if (state_ != State::LIVE) return advanced;
  // Catch-up from disk first, then the resumed partial frame, then whole
  // chunks in order.
  if (disk_ && !send_disk(now_ns, advanced)) return advanced;
  while (partial_off_ < partial_.size()) {
    long w = write_some(fd_, partial_.data() + partial_off_, partial_.size() - partial_off_);
    if (w < 0) {
      close_link(now_ns);
      return advanced;
    }
    partial_off_ += static_cast<std::size_t>(w);
    stats_.bytes_sent += static_cast<u64>(w);
    if (w == 0) return advanced;
  }
  while (send_chunk_ < chunks_.size()) {
    Chunk& c = chunks_[send_chunk_];
    long w = write_some(fd_, c.bytes.data() + send_off_, c.bytes.size() - send_off_);
    if (w < 0) {
      close_link(now_ns);
      return advanced;
    }
    send_off_ += static_cast<std::size_t>(w);
    stats_.bytes_sent += static_cast<u64>(w);
    if (send_off_ < c.bytes.size()) break;  // socket full
    ++stats_.frames_sent;
    ++send_chunk_;
    send_off_ = 0;
  }
  trim();
  return advanced;
}

// ---- Receiver -----------------------------------------------------------------

Receiver::Receiver(const Config& cfg, Apply apply, void* ctx)
    : cfg_(cfg), apply_(apply), ctx_(ctx) {
  listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(cfg_.port);
  if (::inet_pton(AF_INET, cfg_.host.c_str(), &a.sin_addr) != 1 ||
      ::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listen_, 4) != 0) {
    std::fprintf(stderr, "snc: standby cannot listen on %s:%u: %s\n", cfg_.host.c_str(), cfg_.port,
                 std::strerror(errno));
    std::abort();
  }
  set_nonblock(listen_);
  watch(cfg_.epoll_fd, listen_);
  in_.resize(4u << 20);
}

Receiver::~Receiver() {
  if (conn_ >= 0) ::close(conn_);
  if (listen_ >= 0) ::close(listen_);
}

void Receiver::drop() {
  if (conn_ >= 0) ::close(conn_);
  conn_ = -1;
  greeted_ = false;
  in_len_ = 0;
  out_.clear();
}

bool Receiver::handshake(u64 next_lsn, u32 tail_crc) {
  Hello h;
  std::memcpy(&h, in_.data(), sizeof(h));
  std::memmove(in_.data(), in_.data() + sizeof(h), in_len_ - sizeof(h));
  in_len_ -= sizeof(h);
  HelloAck a{};
  std::memcpy(a.magic, kMagic, 8);
  a.next_lsn = next_lsn;
  a.last_crc = tail_crc;
  if (std::memcmp(h.magic, kMagic, 8) != 0 || std::memcmp(h.space_uuid, cfg_.uuid.data(), 16) != 0 ||
      h.shard != cfg_.shard || h.n_shards != cfg_.n_shards || h.slot_crc != cfg_.slot_crc) {
    a.status = kRefusedSpace;
    ++stats_.refused;
  }
  const u8* p = reinterpret_cast<const u8*>(&a);
  out_.insert(out_.end(), p, p + sizeof(a));
  greeted_ = a.status == 0;
  acked_ = next_lsn - 1;
  return greeted_;
}

bool Receiver::poll(u64 next_lsn, u64 durable_lsn, u32 tail_crc) {
  bool did = false;
  {
    // Accept a primary. A new connection replaces the old one: it is a
    // primary reconnecting after its old link died.
    int c = ::accept(listen_, nullptr, nullptr);
    if (c >= 0) {
      set_nonblock(c);
      set_nodelay(c);
      if (conn_ >= 0) drop();
      conn_ = c;
      watch(cfg_.epoll_fd, conn_);
      ++stats_.connects;
      did = true;
    }
  }
  if (conn_ < 0) return did;
  // Read.
  while (true) {
    if (in_len_ == in_.size()) in_.resize(in_.size() * 2);
    ssize_t r = ::recv(conn_, in_.data() + in_len_, in_.size() - in_len_, 0);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      drop();
      return true;
    }
    if (r < 0) break;
    in_len_ += static_cast<std::size_t>(r);
    did = true;
  }
  if (!greeted_) {
    if (in_len_ < sizeof(Hello)) return did;
    if (!handshake(next_lsn, tail_crc)) {
      write_some(conn_, out_.data(), out_.size());
      drop();
      return true;
    }
  }
  // Apply whole frames only, so an atomic group never splits.
  std::size_t off = 0;
  while (in_len_ - off >= sizeof(RecordsHeader)) {
    RecordsHeader h;
    std::memcpy(&h, in_.data() + off, sizeof(h));
    if (in_len_ - off < sizeof(h) + h.len) break;
    const u8* p = in_.data() + off + sizeof(h);
    std::size_t at = 0;
    while (at < h.len) {
      u32 n = check_record(p + at, h.len - at);
      if (n == 0 || !apply_(ctx_, p + at)) {
        std::fprintf(stderr, "snc: standby shard %u: bad record from primary, dropping link\n",
                     cfg_.shard);
        drop();
        return true;
      }
      at += n;
      ++stats_.records;
    }
    ++stats_.frames;
    off += sizeof(h) + h.len;
  }
  if (off > 0) {
    std::memmove(in_.data(), in_.data() + off, in_len_ - off);
    in_len_ -= off;
  }
  // Ack what is durable here.
  if (durable_lsn > acked_) {
    Ack k{durable_lsn};
    const u8* p = reinterpret_cast<const u8*>(&k);
    out_.insert(out_.end(), p, p + sizeof(k));
    acked_ = durable_lsn;
  }
  if (!out_.empty()) {
    long w = write_some(conn_, out_.data(), out_.size());
    if (w < 0) {
      drop();
      return true;
    }
    out_.erase(out_.begin(), out_.begin() + w);
    did = true;
  }
  return did;
}

}  // namespace snc::repl
