// Seeding a standby (docs/REPLICATION.md): the primary sends its active
// checkpoint's files; the standby writes them, then catches up from the
// primary's WAL over the replication link.
#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "snc/snc.h"

namespace snc {

// Wire format, little-endian, one TCP connection per seed:
//   standby -> primary: SeedRequest
//   primary -> standby: SeedHeader, then per file SeedFile + name + bytes
inline constexpr char kSeedMagic[8] = {'S', 'N', 'C', 'S', 'E', 'E', 'D', '1'};
struct SeedRequest {
  char magic[8];
  u8 space_uuid[16];  // all zero: any space (a new standby)
};
struct SeedHeader {
  char magic[8];
  u32 status;         // 0 ok, 1 another space, 2 no checkpoint, 3 not yet: retry
                      // (docs/SYNC_READINESS.md: the primary is first
                      // recording NOT READY with its witnesses)
  u32 n_files;
  u8 space_uuid[16];
  u64 seq;            // the checkpoint's sequence number
  u64 next_lease;
  u64 term;           // the standby starts in the primary's term
};
struct SeedFile {
  u32 name_len;       // relative to the checkpoint's directory
  u32 pad;
  u64 size;
};

struct Engine::Seeding {
  std::mutex mu;                    // guards everything below and sb_.active_ckpt
  std::map<u64, int> ckpt_pins;     // checkpoints being sent: not deleted yet
  std::vector<u64> deferred;        // checkpoints to delete once unpinned
  std::vector<u64> wal_pin;         // per shard: keep WAL from here (0 = no pin)
  std::chrono::steady_clock::time_point wal_pin_until{};
  std::atomic<bool> stop{false};
  int listen_fd = -1;
  std::thread thread;
};

}  // namespace snc
