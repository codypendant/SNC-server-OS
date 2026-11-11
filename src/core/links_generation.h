// The links incarnation generation (docs/CROSS_NODE_EXCHANGE_DESIGN.md 2.1,
// rev4): a u64 that strictly increases across every node-links instance a
// member ever creates. Peers accept only a higher one (2.12 C), so a
// restarted member is always newer and a delayed connection from a retired
// instance is always refused, whatever the wall clock does. Nothing here
// reads a clock or draws a random number.
//
// Kept in the file `links_generation` of the data directory (or the file
// --links-generation-file names, for an engine with no data directory):
//   u32 magic "SNCG", u32 version 1, u64 generation, u32 crc32c, u32 zero.
// Each allocation increments it (checked) and makes it durable (write a
// temporary, fsync it, rename it over, fsync the directory) before the
// value is returned, so a value is never on the wire before it is durable,
// and a crash before then can only bring back the previous value, whose
// successor was never used. One mutex serializes allocations: one counter
// serves every shard of the engine.
//
// Exclusive across processes (2.1): one allocator per file, ever. In a data
// directory, the engine's data-directory lock (invariant M, held from
// recovery until stop) already makes this process the file's only user, so
// the file is opened with `own_lock` false. An explicit
// --links-generation-file has no such owner: the allocator takes
// flock(LOCK_EX | LOCK_NB) on the stable file `<path>.lock` (never the
// generation file itself, whose inode each store replaces by rename) and
// holds it for its lifetime. Held by another allocator (another process,
// or another engine of this one): open_file throws, and the engine refuses
// to start. The lock file is never deleted.
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "snc/types.h"

namespace snc {

class LinksGeneration {
 public:
  enum class Source { FILE, TEST };
  // A file. Missing: created at 0 only when `fresh` (a directory this
  // process just initialized or seeded) or when `min` > 0 (the operator's
  // explicit floor, --links-generation-min). Otherwise, or if the file is
  // truncated or fails its check: throws (the engine refuses to start).
  // The stored value is raised to `min` durably before any allocation.
  // `own_lock`: take and hold `<path>.lock` (above) before reading it.
  static std::unique_ptr<LinksGeneration> open_file(const std::string& path, bool fresh, u64 min, bool own_lock);
  // In-process tests: one process-wide counter shared by every engine.
  static std::unique_ptr<LinksGeneration> test_allocator();

  // The next generation, durable before it returns; false (with a message)
  // if it cannot be stored: the links instance is not created.
  bool next(u64& out);
  u64 last() const;
  const std::string& path() const { return path_; }

  // Tests only (X29): fail the next allocation after `stage` (1: the
  // temporary written, 2: it is fsynced, 3: renamed; 0: off), as a crash
  // there would leave the files.
  static void debug_crash_after(int stage);
  // Tests only (X29): a fake wall clock, in ms. Nothing here reads it; it
  // exists so X29 can step the clock (same millisecond, backward, forward
  // then back) and show that the incarnations do not depend on it (the
  // rev3 clock scheme, negative ND37, does read it, and fails).
  static u64 (*debug_clock_ms)();

  ~LinksGeneration();
  LinksGeneration(const LinksGeneration&) = delete;
  LinksGeneration& operator=(const LinksGeneration&) = delete;

 private:
  LinksGeneration() = default;
  bool store(u64 v);
  int lock_fd_ = -1;  // `<path>.lock`, flocked, while this allocator lives (own_lock)
  Source source_ = Source::FILE;
  std::string path_;
  mutable std::mutex mu_;
  u64 value_ = 0;
};

}  // namespace snc
