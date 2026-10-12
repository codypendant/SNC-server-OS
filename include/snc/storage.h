// Data directory, WAL streams, checkpoints (spec section 12).
//
//   <data_dir>/superblock.a  superblock.b
//   <data_dir>/wal/s000/<first_lsn:016x>.wal
//   <data_dir>/ckpt/<seq:016x>/manifest  part-<stream:03>.ckpt
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "snc/slot_map.h"
#include "snc/wal_format.h"

namespace snc {

using Uuid = std::array<u8, 16>;

[[noreturn]] void storage_fatal(const std::string& what);  // spec C7: stop, never retry

// ---- Paths and small helpers -------------------------------------------------

std::string stream_dir(const std::string& data_dir, u16 stream);
std::string segment_path(const std::string& data_dir, u16 stream, u64 first_lsn);
std::string ckpt_dir(const std::string& data_dir, u64 seq);
std::string part_path(const std::string& data_dir, u64 seq, u16 stream);
void fsync_dir(const std::string& path);
void make_dirs(const std::string& path);  // mkdir -p, then fsync parents

// Streams present on disk (wal/sNNN), sorted.
std::vector<u16> list_streams(const std::string& data_dir);
// Segment first-LSNs of one stream, sorted ascending.
std::vector<u64> list_segments(const std::string& data_dir, u16 stream);

// ---- Superblock ----------------------------------------------------------------

std::optional<Superblock> read_superblock(const std::string& data_dir);
// Writes the copy not holding `sb.generation - 1`, i.e. never the active copy
// (spec C6), and syncs it.
void write_superblock(const std::string& data_dir, const Superblock& sb);
Superblock new_superblock(const Uuid& uuid);
Uuid random_uuid();

// ---- Manifest -------------------------------------------------------------------

struct Manifest {
  u64 seq = 0;
  Uuid space_uuid{};
  u16 n_shards = 0;
  SlotMap slots;
  std::vector<u64> start_lsn;  // per stream, index = stream id
  struct TypeEntry {
    u32 type_id;
    u16 schema_ver;
    std::string name;
  };
  std::vector<TypeEntry> types;
  struct Part {
    u16 stream;
    u64 objects;
    u64 bytes;
  };
  std::vector<Part> parts;
};

void write_manifest(const std::string& data_dir, const Manifest& m);  // tmp + rename + fsync
std::optional<Manifest> read_manifest(const std::string& data_dir, u64 seq);

// ---- Reading records -----------------------------------------------------------

using RecordFn = std::function<void(const WalRecord& h, const u8* payload)>;

struct StreamScan {
  u64 first_lsn = 0;  // first LSN delivered (0 = none)
  u64 last_lsn = 0;   // last valid LSN in the stream (0 = none)
  u64 records = 0;
  u64 bytes = 0;
  u64 torn_tails = 0;  // segments that ended at a torn or unacknowledged tail
};

// Delivers every valid record with lsn >= from_lsn, in order. Stops a segment
// at its first invalid record (torn tail). Aborts if a later segment does not
// continue the LSN sequence (corruption, spec C4).
StreamScan scan_stream(const std::string& data_dir, u16 stream, u64 from_lsn, const Uuid& uuid,
                       const RecordFn& fn);

// Delivers every IMAGE record of a checkpoint part. Aborts on any corruption:
// a checkpoint named by the superblock must be complete.
u64 scan_part(const std::string& data_dir, u64 seq, u16 stream, const Uuid& uuid,
              const RecordFn& fn);

// ---- WAL writer -----------------------------------------------------------------

struct WalConfig {
  std::string data_dir;
  Uuid uuid{};
  u64 segment_bytes = 64ull << 20;
  u32 flush_bytes = 64u << 10;   // flush when this much is buffered
  u32 group_commit_us = 50;      // or when the oldest record is this old
  bool sync = true;              // false: wal_sync=none (development only)
  bool direct = true;            // O_DIRECT, falls back to buffered if refused
};

// One shard's WAL stream. Append-only, group commit, at most one flush in
// flight (spec C2: durability is a prefix). Lives on its shard's thread.
class WalStream {
 public:
  WalStream(const WalConfig& cfg, u16 stream, u64 next_lsn);
  ~WalStream();
  WalStream(const WalStream&) = delete;
  WalStream& operator=(const WalStream&) = delete;

  // Buffers a record and returns its LSN.
  u64 append(RecType type, u32 type_id, const ObjectID& id, u64 version, u16 schema_ver,
             const u8* payload, u32 payload_len);

  // Call once per loop iteration. Starts a flush when due. Returns true while
  // unflushed records exist, so the loop keeps polling instead of sleeping.
  bool poll(u64 now_ns, bool force);

  u64 durable_lsn() const { return durable_lsn_; }
  u64 next_lsn() const { return next_lsn_; }
  bool idle() const { return !inflight_ && active_len_ == 0; }
  bool direct() const { return direct_; }

  struct Stats {
    u64 records = 0;
    u64 record_bytes = 0;   // logical bytes in records
    u64 written_bytes = 0;  // bytes written including block padding
    u64 flushes = 0;
    u64 segments = 0;
  };
  const Stats& stats() const { return stats_; }

  // Set by the owner: called on the shard thread whenever durable_lsn advances.
  void (*on_durable)(void* ctx) = nullptr;
  void* on_durable_ctx = nullptr;

 private:
  static void flush_done(void* ctx, int result);
  void start_flush();
  void open_segment(u64 first_lsn);
  void ensure_capacity(u32 extra);

  WalConfig cfg_;
  u16 stream_;
  int fd_ = -1;
  bool direct_ = false;
  u64 seg_off_ = 0;      // next write offset in the current segment
  u64 next_lsn_;
  u64 durable_lsn_;

  // Double buffering: `active` fills while `flushing` is in flight.
  u8* active_ = nullptr;
  u32 active_cap_ = 0, active_len_ = 0;
  u64 active_first_lsn_ = 0, active_last_lsn_ = 0, active_since_ns_ = 0;
  u8* flushing_ = nullptr;
  u32 flushing_cap_ = 0;
  u64 flushing_last_lsn_ = 0;
  u32 flushing_len_ = 0;
  bool inflight_ = false;

  Stats stats_;
};

}  // namespace snc
