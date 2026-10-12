// SNC Runtime Interface (spec: Runtime and transport boundaries).
// Everything SNC core needs from an execution runtime. Core code calls only
// these functions; a backend (native today, Seastar later) implements them.
// Backends are chosen at link time, so the interface costs no indirection.
#pragma once

#include "snc/allocator.h"
#include "snc/msg.h"

namespace snc {

struct ShardTopology {
  u16 shard = 0;
  int cpu = -1;        // core the worker runs on, -1 if unknown
  int numa_node = -1;  // -1 if unknown
  bool pinned = false;
};

// Callbacks the backend runs on each worker thread.
struct ShardHooks {
  void (*init)(void* user) = nullptr;                  // after pinning, before any message
  void (*on_message)(void* user, const Msg&) = nullptr;
  bool (*poll)(void* user) = nullptr;                  // per-iteration work; true if it did work
  void (*fini)(void* user) = nullptr;                  // after the loop stops
  void* user = nullptr;
};

// Valid only on a worker thread.
u16 sri_shard_id();
u16 sri_shard_count();
const ShardTopology& sri_topology();
ShardAllocator& sri_allocator();

// Enqueue to ring (self, dst). Requests may be refused (returns false) when
// the ring and its bounded overflow are both full. Responses and buffer
// returns (`must_deliver`) are never refused: they are bounded by the
// requests already admitted.
bool sri_send(u16 dst, const Msg& msg, bool must_deliver = false);

u64 sri_now_ns();

}  // namespace snc
