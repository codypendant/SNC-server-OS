// WAL replication, one primary to one standby (docs/REPLICATION.md).
//
// Each primary shard runs a Sender that connects to the standby's shard of
// the same number (port base + shard) and ships the records of every WAL
// flush, in LSN order, as the flush starts. Each standby shard runs a
// Receiver that hands the records to its shard, which installs them and
// appends them to its own WAL with the same LSNs, and that acks what its
// WAL has made durable. Both are polled from the shard's loop and use
// non-blocking sockets; neither blocks the shard.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "snc/storage.h"

namespace snc::repl {

// Frames, little-endian, on one TCP connection per shard.
//   primary -> standby: Hello, then Records frames.
//   standby -> primary: HelloAck, then Ack frames.
inline constexpr char kMagic[8] = {'S', 'N', 'C', 'R', 'E', 'P', 'L', '1'};
struct Hello {
  char magic[8];
  u8 space_uuid[16];
  u16 shard;
  u16 n_shards;
  u32 slot_crc;     // crc32c of the slot map's owner table
};
struct HelloAck {
  char magic[8];
  u32 status;       // 0 = accepted; otherwise why not (kRefused*)
  u32 pad;
  u64 next_lsn;     // first LSN the standby does not have
};
struct RecordsHeader {
  u32 len;          // bytes of records that follow
  u32 pad;
  u64 first_lsn;
  u64 last_lsn;
};
struct Ack {
  u64 durable_lsn;  // the standby's WAL is durable up to here
};
inline constexpr u32 kRefusedSpace = 1;   // other space, shard count or slot map

// Primary side, one per shard.
class Sender {
 public:
  struct Config {
    std::string host;
    u16 port = 0;            // this shard's port (base + shard)
    Uuid uuid{};
    u16 shard = 0;
    u16 n_shards = 0;
    u32 slot_crc = 0;
    u64 retain_bytes = 1ull << 30;  // unacked records kept for catch-up
  };
  explicit Sender(const Config& cfg);
  ~Sender();
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  // WAL flush hook: queue a copy of the records for the standby.
  void on_flush(const u8* recs, u32 len, u64 first_lsn, u64 last_lsn);
  // Connects, handshakes, writes and reads acks. Returns true if the acked
  // LSN advanced (the shard then releases waiters).
  bool poll(u64 now_ns);

  u64 acked_lsn() const { return acked_; }
  bool live() const { return state_ == State::LIVE; }
  // Nanoseconds since the link was last live (0 while live).
  u64 down_since_ns() const { return down_since_; }
  // The standby needs records already dropped from memory: it must be
  // reseeded. The link stays down until then.
  bool lost() const { return lost_; }
  int fd() const { return fd_; }

  struct Stats {
    u64 bytes_sent = 0;
    u64 frames_sent = 0;
    u64 connects = 0;
    u64 retained_bytes = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  enum class State { DOWN, CONNECTING, HELLO, LIVE };
  struct Chunk {
    u64 first_lsn, last_lsn;
    std::vector<u8> bytes;  // RecordsHeader + records
  };
  void close_link(u64 now_ns);
  void start_connect(u64 now_ns);
  bool read_acks(u64 now_ns);
  void position_at(u64 next_lsn);
  void trim();

  Config cfg_;
  State state_ = State::DOWN;
  int fd_ = -1;
  u64 retry_at_ = 0;
  u64 down_since_ = 0;
  bool lost_ = false;
  u64 acked_ = 0;
  std::deque<Chunk> chunks_;
  u64 retained_ = 0;
  // Send position: chunk index and offset within its bytes. A chunk may
  // start mid-way when catch-up resumes inside it.
  std::size_t send_chunk_ = 0, send_off_ = 0;
  std::vector<u8> partial_;  // a resumed chunk's header + tail, sent first
  std::size_t partial_off_ = 0;
  u8 in_[64];
  std::size_t in_len_ = 0;
  Hello hello_{};
  std::size_t hello_off_ = 0;
  Stats stats_;
};

// Standby side, one per shard.
class Receiver {
 public:
  struct Config {
    std::string host = "0.0.0.0";
    u16 port = 0;
    Uuid uuid{};
    u16 shard = 0;
    u16 n_shards = 0;
    u32 slot_crc = 0;
  };
  // `apply` runs for each record of a whole frame, in order; it returns
  // false if the record cannot follow the WAL (an LSN gap), which drops the
  // connection.
  using Apply = bool (*)(void* ctx, const u8* rec);
  Receiver(const Config& cfg, Apply apply, void* ctx);
  ~Receiver();
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  // Accepts, reads frames, applies them, and acks `durable_lsn` when it
  // moved. `next_lsn` is the first LSN this shard lacks (for the
  // handshake). Returns true if it did work.
  bool poll(u64 next_lsn, u64 durable_lsn);
  bool connected() const { return conn_ >= 0; }
  int listen_fd() const { return listen_; }
  int conn_fd() const { return conn_; }

  struct Stats {
    u64 records = 0;
    u64 frames = 0;
    u64 connects = 0;
    u64 refused = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void drop();
  bool handshake(u64 next_lsn);

  Config cfg_;
  Apply apply_;
  void* ctx_;
  int listen_ = -1;
  int conn_ = -1;
  bool greeted_ = false;
  std::vector<u8> in_;
  std::size_t in_len_ = 0;
  std::vector<u8> out_;  // pending HelloAck / Ack bytes
  u64 acked_ = 0;
  Stats stats_;
};

// crc32c of a slot map's owner table, to check both sides route alike.
u32 slot_map_crc(const SlotMap& m);

}  // namespace snc::repl
