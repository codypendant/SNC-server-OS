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

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
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
  u64 term;         // the primary's term: a standby refuses an older one
};
struct HelloAck {
  char magic[8];
  u32 status;       // 0 = accepted; otherwise why not (kRefused*)
  u32 last_crc;     // crc32c of record next_lsn - 1 (0 = unknown): detects divergence
  u64 next_lsn;     // first LSN the standby does not have
};
struct RecordsHeader {
  u32 len;          // bytes of records that follow
  u32 kind;         // kFrameRecords, or kFrameLease (len 0, first_lsn = echoed lease_ts)
  u64 first_lsn;
  u64 last_lsn;
};
inline constexpr u32 kFrameRecords = 0;
inline constexpr u32 kFrameLease = 1;
// Standby to primary. With lease_ns set, the standby asks for a read lease
// (docs/REPLICATION.md, "Reads on the standby"): a sync primary that
// receives it echoes lease_ts and does not continue without this standby
// for lease_ns from receipt. The standby, counting from lease_ts (its own
// clock, before the primary's receipt), may serve reads until then.
struct Ack {
  u64 durable_lsn;  // the standby's WAL is durable up to here
  u64 lease_ts;     // the standby's clock when sent (0: no lease asked)
  u64 lease_ns;
};
inline constexpr u32 kRefusedSpace = 1;   // other space, shard count or slot map
inline constexpr u32 kRefusedTerm = 3;    // a primary from an older term (fenced)

// Tests only (snc_server --debug-admin, DEBUG REPL-PAUSE): while set, every
// Sender in this process ships nothing more and does not time out its
// standby, so a test decides exactly which records a standby lacks.
// Process-wide: one engine per process (snc_server).
void set_debug_pause(bool on);
bool debug_paused();

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
    u64 term = 0;
    int epoll_fd = -1;       // if set, sockets are added here for idle wake-ups
    // This shard's WAL: records the standby lacks that are no longer in
    // memory are read back from here (catch-up from disk).
    std::string data_dir;
    u64 start_lsn = 1;       // the WAL stream's next LSN when the Sender starts
    // Catch-up from disk sends at most this many bytes a second (0: no
    // limit), so a joining standby costs this shard's clients less.
    u64 catchup_bytes_per_s = 0;
    // A live link whose standby acks nothing new for this long while
    // records wait is treated as down (a hung or stopped standby keeps its
    // socket open). 0: never.
    u64 ack_timeout_ns = 0;
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
  // The standby needs records that are neither in memory nor on disk: it
  // must be reseeded. The link stays down until then.
  bool lost() const { return lost_; }
  // A member of this group refused our term: it was promoted over this
  // primary, which is fenced for good (it stops reconnecting).
  bool fenced() const { return fenced_; }
  // First LSN the standby may still need from disk (0: none, it is lost):
  // its next record, and the one before it, which proves its log is ours.
  // Checkpoints keep the WAL segments from here on.
  u64 retain_from() const { return lost_ ? 0 : std::max<u64>(acked_, 1); }
  bool catching_up() const { return disk_ != nullptr; }
  // Whether this primary currently holds client acks for the standby (sync,
  // not degraded): only then does it grant read leases.
  void set_in_sync(bool on) { in_sync_ = on; }
  // A sync primary must not continue without the standby before this.
  u64 lease_until_ns() const { return lease_until_; }
  int fd() const { return fd_; }

  struct Stats {
    u64 bytes_sent = 0;
    u64 frames_sent = 0;
    u64 connects = 0;
    u64 retained_bytes = 0;
    u64 disk_bytes = 0;      // sent from WAL files rather than memory
  };
  const Stats& stats() const { return stats_; }

 private:
  // Catch-up from WAL files on a helper thread, which fills frames that
  // the shard thread sends. Shared so a cancelled reader can finish alone.
  struct DiskRead {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::vector<u8>> frames;
    std::size_t bytes = 0;
    bool done = false, ok = true, cancel = false, diverged = false;
  };
  // Reads [from, to). With verify_crc, first checks that record from - 1
  // has that crc (the standby's last record matches ours).
  void start_disk(u64 from, u64 to, u32 verify_crc = 0);
  void diverged(u64 now_ns, u64 lsn);
  void stop_disk();
  u64 mem_first() const { return chunks_.empty() ? next_expected_ : chunks_.front().first_lsn; }
  bool send_disk(u64 now_ns, bool& advanced);
  void apply_verified(bool& advanced);
  enum class State { DOWN, CONNECTING, HELLO, LIVE };
  struct Chunk {
    u64 first_lsn, last_lsn;
    std::vector<u8> bytes;  // RecordsHeader + records
  };
  void close_link(u64 now_ns);
  void start_connect(u64 now_ns);
  bool read_acks(u64 now_ns);
  void position_at(u64 next_lsn, u32 tail_crc, u64 now_ns);
  void trim();

  Config cfg_;
  State state_ = State::DOWN;
  int fd_ = -1;
  u64 retry_at_ = 0;
  u64 down_since_ = 0;
  u64 progress_ns_ = 0;    // live: last time acks advanced or nothing waited
  // Read leases: the latest lease_ts to echo, and the echo frame being sent.
  bool in_sync_ = false;
  u64 lease_until_ = 0;
  u64 echo_ts_ = 0;
  std::vector<u8> echo_;
  std::size_t echo_off_ = 0;
  bool lost_ = false;
  bool fenced_ = false;
  u64 acked_ = 0;
  std::deque<Chunk> chunks_;
  u64 retained_ = 0;
  // Send position: chunk index and offset within its bytes. A chunk may
  // start mid-way when catch-up resumes inside it.
  std::size_t send_chunk_ = 0, send_off_ = 0;
  std::vector<u8> partial_;  // a resumed chunk's header + tail, sent first
  std::size_t partial_off_ = 0;
  u64 next_expected_ = 1;  // LSN after the last record queued by on_flush
  std::shared_ptr<DiskRead> disk_;
  u64 disk_to_ = 0;
  u64 verify_ack_ = 0;  // the standby's tail, acked once the disk reader has checked it
  std::vector<u8> disk_frame_;
  std::size_t disk_off_ = 0;
  // Token bucket for catchup_bytes_per_s.
  u64 disk_tokens_ = 0;
  u64 disk_refill_ns_ = 0;
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
    u64 term = 0;            // primaries from an older term are refused
    int epoll_fd = -1;       // if set, sockets are added here for idle wake-ups
    u64 lease_ns = 0;        // ask for read leases this long (0: none)
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
  // `tail_crc` is the crc32c of record next_lsn - 1 (0 = unknown).
  bool poll(u64 next_lsn, u64 durable_lsn, u32 tail_crc, u64 now_ns);
  bool connected() const { return conn_ >= 0; }
  // Reads may be served: the primary granted a lease that has not expired.
  bool lease_valid(u64 now_ns) const { return conn_ >= 0 && now_ns < lease_until_; }
  int listen_fd() const { return listen_; }
  int conn_fd() const { return conn_; }
  // Promotion: install nothing more, and from now on answer every primary
  // that connects with a refusal, kRefusedTerm if its term is below
  // `new_term`. The old primary, if it is alive and reaches this listener,
  // learns that it was replaced and withdraws (docs/CLUSTER.md).
  void fence(u64 new_term);

  struct Stats {
    u64 records = 0;
    u64 frames = 0;
    u64 connects = 0;
    u64 refused = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void drop();
  bool handshake(u64 next_lsn, u32 tail_crc);
  bool fencing_ = false;

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
  u64 lease_until_ = 0;
  u64 lease_sent_ = 0;     // when the last lease request went out
  Stats stats_;
};

// crc32c of a slot map's owner table, to check both sides route alike.
u32 slot_map_crc(const SlotMap& m);

}  // namespace snc::repl
