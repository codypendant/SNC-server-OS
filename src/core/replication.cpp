#include "replication.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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

Sender::Sender(const Config& cfg) : cfg_(cfg) {
  std::memcpy(hello_.magic, kMagic, 8);
  std::memcpy(hello_.space_uuid, cfg_.uuid.data(), 16);
  hello_.shard = cfg_.shard;
  hello_.n_shards = cfg_.n_shards;
  hello_.slot_crc = cfg_.slot_crc;
}

Sender::~Sender() {
  if (fd_ >= 0) ::close(fd_);
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
  // Over budget: drop the oldest unacked records. A standby that still
  // needs them can no longer catch up from this primary's memory.
  while (retained_ > cfg_.retain_bytes && chunks_.size() > 1) {
    if (chunks_.front().last_lsn > acked_ && (state_ != State::LIVE || send_chunk_ == 0)) lost_ = true;
    retained_ -= chunks_.front().bytes.size();
    chunks_.pop_front();
    if (send_chunk_ > 0) --send_chunk_;
    else send_off_ = 0;
  }
  stats_.retained_bytes = retained_;
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

// Finds where to resume for a standby that lacks `next_lsn` and up.
void Sender::position_at(u64 next_lsn) {
  partial_.clear();
  partial_off_ = 0;
  send_chunk_ = 0;
  send_off_ = 0;
  while (send_chunk_ < chunks_.size() && chunks_[send_chunk_].last_lsn < next_lsn) ++send_chunk_;
  if (send_chunk_ == chunks_.size()) return;  // nothing retained it lacks; send live from here
  Chunk& c = chunks_[send_chunk_];
  if (c.first_lsn > next_lsn) {
    lost_ = true;  // the records it needs are no longer here
    return;
  }
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
      if (a.next_lsn > 0 && a.next_lsn - 1 > acked_) {
        acked_ = a.next_lsn - 1;  // it already has these durably
        advanced = true;
      }
      position_at(a.next_lsn);
      if (lost_) {
        std::fprintf(stderr,
                     "snc: standby shard %u needs LSN %llu, no longer retained: reseed it\n",
                     cfg_.shard, (unsigned long long)a.next_lsn);
        close_link(now_ns);
        return advanced;
      }
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
  // Send the resumed partial frame first, then whole chunks in order.
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

bool Receiver::handshake(u64 next_lsn) {
  Hello h;
  std::memcpy(&h, in_.data(), sizeof(h));
  std::memmove(in_.data(), in_.data() + sizeof(h), in_len_ - sizeof(h));
  in_len_ -= sizeof(h);
  HelloAck a{};
  std::memcpy(a.magic, kMagic, 8);
  a.next_lsn = next_lsn;
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

bool Receiver::poll(u64 next_lsn, u64 durable_lsn) {
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
    if (!handshake(next_lsn)) {
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
