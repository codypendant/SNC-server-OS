// Data directory, WAL streams, checkpoints (spec section 12).
//
//   <data_dir>/superblock.a  superblock.b
//   <data_dir>/wal/s000/<first_lsn:016x>.wal
//   <data_dir>/ckpt/<seq:016x>/manifest  part-<stream:03>.ckpt
#pragma once

#include "snc/gauges.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
  u32 last_crc = 0;   // that record's crc32c
  u64 records = 0;
  u64 bytes = 0;
  u64 torn_tails = 0;  // segments that ended at a torn or unacknowledged tail
};

// Delivers every valid record with lsn >= from_lsn, in order. Stops a segment
// at its first invalid record (torn tail). Records of an atomic group
// (kRecGroupNext) are delivered only once the group's last record is read;
// a group cut off by a torn tail is dropped whole. Aborts if a later segment does not
// continue the LSN sequence (corruption, spec C4).
StreamScan scan_stream(const std::string& data_dir, u16 stream, u64 from_lsn, const Uuid& uuid,
                       const RecordFn& fn);

// Delivers the raw bytes (header + payload) of every record with
// from_lsn <= lsn < to_lsn, in order, for replication catch-up while the
// stream is being written. Every record in the range must already be on
// disk. Returns false, possibly after delivering some, if the range is not
// all there (a segment was deleted, or a gap): the caller cannot use it.
// fn returns false to stop: the read ends there (no further record, no
// further segment read) and returns false. stop, if set, is polled before
// each segment and while skipping records ahead of from_lsn, so a cancelled
// read ends without delivering anything first (B3).
bool read_stream_range(const std::string& data_dir, u16 stream, u64 from_lsn, u64 to_lsn,
                       const Uuid& uuid, const std::function<bool(const u8* rec, u32 len)>& fn,
                       const std::function<bool()>& stop = {});

// Delivers every IMAGE record of a checkpoint part. Aborts on any corruption:
// a checkpoint named by the superblock must be complete.
u64 scan_part(const std::string& data_dir, u64 seq, u16 stream, const Uuid& uuid,
              const RecordFn& fn);

// ---- WAL writer -----------------------------------------------------------------

class SegmentPreparer;

// WAL active-buffer limits. The buffer's sizes are u32 (and the flush pads
// active_len up to a block in a u32), so the hard maximum stays well below
// 2^31; the default is 1 GiB. The buffer starts at 2 MiB, which fits the
// largest record (1 MiB payload), and doubles up to the maximum. The
// hard maximum also keeps static_cast<int>(flushing_len_) in the flush
// below 2^31. Known limit: a standby applies everything it receives with
// no receive-side bound (replication.cpp Receiver::poll), so a standby
// whose fsync falls far behind a fast link now stops at 1 GiB (fail-stop)
// where before it hung or corrupted at 2 GiB; a standby budget is
// separate work.
inline constexpr u64 kWalActiveInitial = 2ull << 20;
inline constexpr u64 kWalActiveMaxDefault = 1ull << 30;
inline constexpr u64 kWalActiveHardMax = 1ull << 30;

struct WalConfig {
  std::string data_dir;
  Uuid uuid{};
  u64 segment_bytes = 64ull << 20;
  u32 flush_bytes = 64u << 10;   // flush when this much is buffered
  u32 group_commit_us = 50;      // or when the oldest record is this old
  bool sync = true;              // false: wal_sync=none (development only)
  bool direct = true;            // O_DIRECT, falls back to buffered if refused
  SegmentPreparer* spares = nullptr;  // null: create segments on the shard thread
  // The largest the active (unflushed) buffer may grow: the explicit
  // supported maximum. An append that would need more is a fail-stop
  // (storage_fatal) before any buffer or counter changes. At most
  // kWalActiveHardMax, at least kWalActiveInitial.
  u64 max_active_bytes = kWalActiveMaxDefault;
};

// Creates the next WAL segment of each stream ahead of time, on its own
// thread (spec section 12: segments are created and zeroed off the shard
// thread). A spare is `spare.tmp` in the stream directory: allocated,
// synced, directory synced, header still zero. At rollover the shard takes
// the open file, renames it to its first-LSN name and writes the header with
// its first flush, so the shard pays for one rename instead of a create,
// fallocate and three syncs. If the rename is lost in a crash, recovery finds
// the spare by its header (adopt_spare).
class SegmentPreparer {
 public:
  SegmentPreparer(const std::string& data_dir, u16 streams, u64 segment_bytes, bool direct);
  ~SegmentPreparer();
  SegmentPreparer(const SegmentPreparer&) = delete;
  SegmentPreparer& operator=(const SegmentPreparer&) = delete;

  // Shard thread: takes the ready spare of `stream` (fd >= 0) or returns -1
  // when none is ready yet. The spare keeps its name until the caller
  // renames it; request() then lets the preparer make the next one.
  int take(u16 stream, bool* direct);
  void request(u16 stream);

 private:
  void run();
  void prepare(u16 stream);

  struct Slot {
    std::atomic<int> fd{-1};
    std::atomic<bool> direct{false};
    std::atomic<bool> wanted{true};
  };
  std::string dir_;
  u64 segment_bytes_;
  bool direct_;
  std::unique_ptr<Slot[]> slots_;
  u16 n_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;
};

// Path of a stream's spare segment.
std::string spare_path(const std::string& data_dir, u16 stream);

// Recovery, before scanning a stream: a spare whose header is valid was
// renamed into use but the rename was lost in a crash, so it gets its proper
// name back. Any other spare is deleted. Returns true if one was adopted.
bool adopt_spare(const std::string& data_dir, u16 stream, const Uuid& uuid);

// One shard's WAL stream. Append-only, group commit, at most one flush in
// flight (spec C2: durability is a prefix). Lives on its shard's thread.
class WalStream {
 public:
  WalStream(const WalConfig& cfg, u16 stream, u64 next_lsn);
  ~WalStream();
  WalStream(const WalStream&) = delete;
  WalStream& operator=(const WalStream&) = delete;

  // Buffers a record and returns its LSN.
  // Records appended by one op are flushed together; flags may mark an
  // atomic group (kRecGroupNext).
  u64 append(RecType type, u32 type_id, const ObjectID& id, u64 version, u16 schema_ver,
             const u8* payload, u32 payload_len, u16 flags = 0);

  // Replication (docs/REPLICATION.md), standby side: appends a record
  // received from the primary verbatim, keeping its LSN, flags and CRC.
  // `rec` must have passed check_record. Returns false, appending nothing,
  // if its LSN is not next_lsn().
  bool append_raw(const u8* rec);

  // The capacity the active buffer must grow to so that `len` buffered
  // bytes plus `extra` more plus one block of padding fit: doubling from
  // `cap`, clamped to `max`. 0 if they cannot fit within `max`. Pure and
  // overflow-free for any inputs (u64 arithmetic, every value checked
  // against `max` before it is combined): tests reach the boundaries with
  // no allocation.
  static u64 grown_capacity(u64 cap, u64 len, u64 extra, u64 max);

  // Call once per loop iteration. Starts a flush when due. Returns true while
  // unflushed records exist, so the loop keeps polling instead of sleeping.
  bool poll(u64 now_ns, bool force);

  u64 durable_lsn() const { return durable_lsn_; }
  u64 next_lsn() const { return next_lsn_; }
  bool idle() const { return !inflight_ && active_len_ == 0; }
  bool direct() const { return direct_; }
  // Observability (S0): unflushed bytes, buffer capacity, and when the
  // flush in flight started (0 if none).
  u32 active_bytes() const { return active_len_; }
  u64 buffer_cap_bytes() const { return u64{active_cap_} + flushing_cap_; }
  u64 flush_since_ns() const { return inflight_ ? flush_start_ns_ : 0; }

  struct Stats {
    u64 records = 0;
    u64 record_bytes = 0;   // logical bytes in records
    u64 written_bytes = 0;  // bytes written including block padding
    u64 flushes = 0;
    u64 segments = 0;
    u64 spares_used = 0;    // segments that came ready-made from the preparer
    u64 segment_opens = 0;  // segments created on the shard thread (no spare ready)
    // Flush latency, submit to completion: bucket i counts flushes that took
    // [2^i, 2^(i+1)) microseconds.
    u64 flush_us_log2[24] = {};
    u64 flush_us_max = 0;
    u64 rollover_us_max = 0;  // longest segment switch on the shard thread
    // Observability (S0, docs/CHECKPOINT_BACKPRESSURE_DESIGN.md 11).
    u64 active_bytes_peak = 0;  // most unflushed record bytes held at once
    Ewma flush_us_ewma;         // flush latency, submit to completion
  };
  const Stats& stats() const { return stats_; }

  // Set by the owner: called on the shard thread whenever durable_lsn advances.
  void (*on_durable)(void* ctx) = nullptr;
  void* on_durable_ctx = nullptr;
  // Set by the owner: called with the records of each flush as it starts
  // (whole records, in LSN order, no padding), so replication can ship them
  // while the local write is in flight. The bytes are valid only during the
  // call.
  void (*on_flush)(void* ctx, const u8* recs, u32 len, u64 first_lsn, u64 last_lsn) = nullptr;
  void* on_flush_ctx = nullptr;

 private:
  static void flush_done(void* ctx, int result);
  void start_flush();
  void open_segment(u64 first_lsn);
  bool take_spare(u64 first_lsn);
  void ensure_capacity(u64 extra);

  WalConfig cfg_;
  u16 stream_;
  int fd_ = -1;
  bool direct_ = false;
  u64 seg_off_ = 0;      // next write offset in the current segment
  bool header_pending_ = false;  // spare in use: header goes out with the first flush
  u64 active_first_lsn_seg_ = 0;  // first LSN of the segment awaiting its header
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
  u64 flush_start_ns_ = 0;

  Stats stats_;
};

}  // namespace snc
