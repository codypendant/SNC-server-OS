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
  static std::unique_ptr<LinksGeneration> open_file(const std::string& path, bool fresh, u64 min);
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

 private:
  LinksGeneration() = default;
  bool store(u64 v);
  Source source_ = Source::FILE;
  std::string path_;
  mutable std::mutex mu_;
  u64 value_ = 0;
};

}  // namespace snc
