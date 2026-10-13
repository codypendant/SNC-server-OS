// Persistent formats (spec section 12): superblock, WAL segment header, WAL
// and checkpoint records. Little-endian, fixed-width, CRC-32C protected.
#pragma once

#include "snc/object_id.h"

namespace snc {

inline constexpr u32 kBlock = 4096;
inline constexpr u16 kFormatVersion = 1;

u32 crc32c(const void* data, std::size_t len, u32 seed = 0);

// Superblock: two 4 KiB copies, written alternately; the valid copy with the
// highest generation wins.
struct Superblock {
  char magic[8];      // "SNCSUPR1"
  u16 format;
  u16 reserved;
  u32 crc32c;         // over the 4 KiB block with this field zeroed
  u64 generation;
  u8 space_uuid[16];
  u64 next_lease;     // section 1
  u64 active_ckpt;    // checkpoint sequence, 0 = none yet
  u64 term;           // replication term: +1 at each promotion (docs/REPLICATION.md)
  // The block of mint leases this node holds from the control plane,
  // [lease_lo, lease_hi), next_lease inside it (docs/CONTROL_PLANE.md
  // section 11). 0, 0: none; leases come from this space's own counter
  // (superblocks written before blocks existed read as that).
  u64 lease_lo;
  u64 lease_hi;
};
static_assert(sizeof(Superblock) == 80);

// First 4 KiB block of every WAL segment and checkpoint part.
struct FileHeader {
  char magic[8];      // "SNCWAL01" or "SNCCKP01"
  u16 format;
  u16 stream;
  u32 crc32c;         // over this struct with this field zeroed
  u64 seq;            // WAL: unused (file name carries first LSN); ckpt: checkpoint seq
  u64 first_lsn;      // WAL: first LSN in the segment; ckpt: stream start_lsn
  u8 space_uuid[16];
};
static_assert(sizeof(FileHeader) == 48);

enum class RecType : u16 { CREATE = 1, UPDATE = 2, DELETE = 3, IMAGE = 5 };

// WalRecord::flags. A record with kRecGroupNext belongs to one atomic group
// with the records after it, up to and including the first record without
// the flag: recovery installs the whole group or none of it.
inline constexpr u16 kRecGroupNext = 1;

struct WalRecord {
  u32 crc32c;        //  0  over bytes [4, total_len)
  u32 total_len;     //  4  header + payload, rounded up to 8
  u64 lsn;           //  8
  u16 rec_type;      // 16
  u16 flags;         // 18
  u32 type_id;       // 20
  ObjectID id;       // 24
  u64 version;       // 40  object version after this record
  u16 schema_ver;    // 48
  u16 reserved;      // 50
  u32 payload_len;   // 52  0 for DELETE
};
static_assert(sizeof(WalRecord) == 56);
static_assert(offsetof(WalRecord, id) == 24 && offsetof(WalRecord, payload_len) == 52);

inline u32 record_size(u32 payload_len) {
  return (static_cast<u32>(sizeof(WalRecord)) + payload_len + 7u) & ~7u;
}

// Writes a record (header + payload + zero padding) to `dst`, which must have
// record_size(payload_len) bytes. Returns that size.
u32 encode_record(u8* dst, RecType type, u64 lsn, u32 type_id, const ObjectID& id, u64 version,
                  u16 schema_ver, const u8* payload, u32 payload_len, u16 flags = 0);

// Validates the record at `p` with `avail` bytes readable. Returns its total
// length, or 0 if it is not a valid record (zeros, torn, or corrupt).
u32 check_record(const u8* p, std::size_t avail);

}  // namespace snc
