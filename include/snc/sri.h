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
  bool io_uring = false;  // false: file writes fall back to synchronous pwritev2
};

// Callbacks the backend runs on each worker thread.
struct ShardHooks {
  void (*init)(void* user) = nullptr;                  // after pinning, before any message
  void (*on_message)(void* user, const Msg&) = nullptr;
  // Optional: sees each batch drained from one ring before on_message runs
  // for any of them (for prefetching their targets).
  void (*on_batch)(void* user, const Msg* const* msgs, u32 n) = nullptr;
  bool (*poll)(void* user) = nullptr;                  // per-iteration work; true if it did work
  // During stop: true once the shard has no work in flight. The loop keeps
  // running until it returns true. Null = always quiesced.
  bool (*quiesced)(void* user) = nullptr;
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
// The same in two steps, so the hot paths write a message in place instead
// of building it on the stack and copying it (copying a struct just written
// field by field stalls on store forwarding). sri_claim returns where to
// write, or null when sri_send would refuse; sri_commit sends it. Nothing
// else may send to `dst` in between.
Msg* sri_claim(u16 dst, bool must_deliver = false);
void sri_commit(u16 dst, Msg* msg);
// Make what was sent to `dst` visible now rather than at the end of this
// loop iteration (the wake-up of a sleeping consumer still happens there).
void sri_publish(u16 dst);
// Messages this shard has queued behind full rings (the overflow of every
// destination, `must_deliver` ones included): a resource gauge for tests
// (docs/EXPERIMENTAL_BASELINE.md T-Q4).
u64 sri_overflow_queued();

u64 sri_now_ns();
// Backend name for reports: "native" or "seastar". Valid on any thread.
const char* sri_backend();

// Asynchronous positional file write, completed on this shard's loop.
// `result` is bytes written or -errno. With `dsync` the data is on stable
// storage when the completion runs (RWF_DSYNC: the fdatasync is part of the
// write). Buffers must stay valid until completion; with O_DIRECT they must
// be 4 KiB aligned.
using IoDone = void (*)(void* ctx, int result);
void sri_write(int fd, u64 off, const void* buf, u32 len, bool dsync, IoDone done, void* ctx);
// Asynchronous fdatasync of everything written to `fd` so far.
void sri_fdatasync(int fd, IoDone done, void* ctx);

// Wake this shard from its idle wait when `fd` becomes readable (for example
// an epoll fd holding the shard's sockets). The shard still discovers the
// work in its poll hook; this only keeps an idle shard from sleeping through
// it. Call on the shard's own thread.
void sri_watch_fd(int fd);

}  // namespace snc
