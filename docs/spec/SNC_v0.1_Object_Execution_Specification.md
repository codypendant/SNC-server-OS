<!--
PROVENANCE (added on import; not part of the original text)
- Source: Claude Docs document "SNC v0.1 Object & Execution Specification",
  id 8a25e96e-09ae-43ef-9e42-d6763e1eebd9, single tab, revision 22.
  Linked from README.md and docs/STATUS.md as the design.
- Exported to Markdown 2026-11-16 UTC through the Claude Docs connector.
  Export size 49639 bytes, SHA-256 c0a8ab56ea1bf3dc21070672893b53a00577583ba599ee180b5c860373f7e801.
- Changes from the export, and only these: line 3's author mention is replaced by
  "[byline redacted]" (owner's decision, 2026-11-16, public repository).
  Text below this comment has SHA-256 486c153570f203d4a0e288cd0cda664dbd6c66aa0fa6ca11b37ffe3f24e609f7.
- Export limits: one embedded diagram ("execute() path · local and cross-shard") was
  replaced by a text placeholder; a date chip and a mention chip were rendered as text.
- Whether revision 22 differs from the text the prototype was built against is UNKNOWN.
- STATUS: historical design input (Codex, docs/handoff/AFR001_Handoff1_Codex_Review.md item 2).
  NOT authoritative until reconciled; it does not override docs/STATUS.md, docs/CORE_CONTRACTS.md
  or any later approved design. Do not edit the text below; record deviations elsewhere.
-->
# SNC v0.1 Object & Execution Specification

Oct 12, 2026 · [byline redacted]

This spec fixes the exact structures and semantics SNC v0.1 needs to meet the handoff milestone: one million persistent objects on pinned shards, `execute(ObjectID, op)` with local and cross-shard dispatch, one secondary index, and WAL, checkpoint and crash recovery on a single node. It stays inside the handoff's v0.1 scope and non-goals; nothing here requires DPDK, a custom Linux image, distribution or migration. Where the handoff left a choice open, the draft picks a default and lists it below for cody's call.

## Decisions for cody

Each row is a default this draft already builds on. Changing one changes the named section.

| # | Decision | Default picked | Main alternative | Section |
| --- | --- | --- | --- | --- |
| D1 | Implementation language | C++20, Linux x86-64 and AArch64, little-endian only | Rust (weaker Seastar story) | Conventions |
| D2 | Runtime backend order | Build a small native backend (pthreads, SPSC rings, io\_uring) behind the runtime interface first; evaluate Seastar as a second backend against it | Seastar first, as the handoff's initial candidate | Runtime boundary |
| D3 | How an ObjectID finds its owner | Hash of the ID picks one of 4,096 virtual slots; a slot map assigns slots to shards | Explicit per-object directory entry (allows choosing placement at create time) | 3, 4 |
| D4 | ObjectID minting | 8-bit format + 56-bit mint lease + 64-bit sequence; a lease is granted per shard per boot; IDs are never reused | Random 120-bit IDs (no coordination, tiny collision risk) | 1 |
| D5 | Handler shape | Two-phase handler: read-only `validate()`, then in-place `apply()` that cannot fail | Copy-on-write scratch image with rollback | 6 |
| D6 | WAL content | Full after-image of the object per mutation (physical redo) | Logical log of op + args (smaller, needs deterministic handlers forever) | 12 |
| D7 | Read visibility default | Reads wait until the object's last write is durable, so no caller ever sees state a crash could undo | Return immediately and flag the result `durable=false` | 6, 9 |
| D8 | Secondary index | Shard-local, non-unique equality index; lookup broadcasts to all shards | Key-partitioned global index with unique keys (needs a cross-shard atomicity protocol) | 11 |
| D9 | Allocator | Proven per-thread heap (mimalloc first-class heaps) behind a `ShardAllocator` interface; custom slabs only if benchmarks demand | Custom size-class slab allocator from day one | 10 |
| D10 | Benchmark ingress | In-process load driver on each shard as the primary harness; a simple socket ingress added for fair comparison with Redis and PostgreSQL | Socket-only, to match baselines everywhere | Benchmark |

## At a glance

&#91;embedded content: execute() path · local and cross-shard\]

Every op runs on its owner shard against the object in place. A remote op costs one request message and one reply; a local op costs neither. Completion waits for the WAL record to be durable by default.

## Conventions and scope

All persistent and in-memory formats in this spec are little-endian, naturally aligned, and fixed-width; nothing persistent contains a pointer, CPU number, NUMA node, PCI address or host name.

- **Language (D1).** C++20. Structures below are written as C structs with explicit offsets; `u8`..`u64` are unsigned fixed-width integers.
- **Platforms.** Linux on bare metal or a VM. Pinning, isolated cores and HugeTLB are used where available and reported when not; the system must still run correctly without them.
- **Limits for v0.1.** Shards: 1 to 256. Object payload: at most 1 MiB. Op arguments: at most 64 KiB. Op result: at most 64 KiB. Index key: at most 256 bytes. Each limit is a named constant, so raising one is a config change, not a format change, unless marked as a format field.
- **Format versioning.** Every persistent file starts with an 8-byte magic and a `u16` format version. A reader refuses any version it does not know.
- **Checksums.** CRC-32C (Castagnoli) everywhere, chosen for hardware support on x86 (SSE4.2) and ARMv8.
- **Hash.** `snc_hash64(ObjectID)` = XXH3-64 over the 16 ID bytes, seed 0. It decides slot placement, so it is frozen as part of the format.
- **Single-object scope.** Every v0.1 operation reads or mutates exactly one object. Multi-object atomicity, MVCC and distributed anything are out of scope (handoff §9, §23).
- **Words.** *Shard*: the unit of ownership, one per worker. *Owner*: the shard that holds an object's authoritative state. *Entry shard*: the shard where a request first arrives. *Durable*: the WAL record is on stable storage after a completed `fdatasync` or FUA write.

## 1. ObjectID

An ObjectID is 128 bits made of a format byte, a 56-bit mint lease and a 64-bit sequence; it is unique forever, never reused, and carries no location (D4).

```c
typedef struct {
  u64 hi;   // bits 63..56: format (0x01 in v0.1); bits 55..0: mint lease
  u64 lo;   // sequence within the lease, starts at 1
} ObjectID; // 16 bytes, 8-aligned; all-zero = SNC_NULL_ID
```

- **Mint lease.** The superblock holds `next_lease` (u64). At every boot, before READY, the control thread reserves one lease per shard by advancing `next_lease` and making the superblock durable. Lease 0 is reserved. 2^56 leases at one per shard per boot will not run out.
- **Minting.** The entry shard mints: `id = {0x01 << 56 | lease, ++seq}`. No shared counter, no atomic, no message. A failed create leaves a gap in the sequence, which is harmless.
- **No location.** The lease records which minting event produced the ID, not where the object lives. Placement uses only `snc_hash64(id)` (section 3). No component may route, shard or infer anything from the lease or sequence fields; this keeps future migration and distribution free to move objects.
- **Comparison.** Equality is the only semantic comparison. `(hi, lo)` order exists for sorting and determinism only; it does not mean creation order across shards.
- **Text form.** 32 lowercase hex digits, `hi` then `lo`, with one dash: `0100000000000003-00000000000f4240`. Parsers reject any other form.
- **Stale handles.** Because IDs are never reused, an ID of a deleted object can only ever return `E_NOT_FOUND`; it cannot alias a newer object. A caller holding stale *state* (not a stale ID) uses `expected_version` (section 9) and gets `E_VERSION_CONFLICT`. In-memory object pointers never leave a single handler invocation, so there is no stale-pointer case to detect.
- **Future distribution.** A cluster authority grants leases instead of the local superblock. The ID format does not change.

## 2. Object header and layout

An object is one contiguous allocation: a 48-byte header followed by its payload, and the payload bytes are identical in memory, in the WAL and in checkpoints.

```c
typedef struct {          // offset
  ObjectID id;            //  0  16  the object's identity
  u64      version;       // 16   8  1 at create, +1 per successful mutation
  u64      last_lsn;      // 24   8  WAL LSN that produced this version (runtime only)
  u32      type_id;       // 32   4  registered TypeID, never 0
  u32      length;        // 36   4  payload bytes in use
  u32      capacity;      // 40   4  payload bytes available without reallocating
  u16      flags;         // 44   2  see below
  u16      schema_ver;    // 46   2  payload schema version of type_id
} ObjectHeader;           // 48 bytes; payload starts at offset 48, 16-aligned
```

- **Flags.** `DELETED` (bit 0): tombstone waiting for its delete record to become durable. `DIRTY` (bit 1): reserved for incremental checkpoints, unused in v0.1. `INDEXED` (bit 2): has an entry in the type's secondary index. Other bits are zero and must be preserved.
- **Runtime-only fields.** `last_lsn`, `capacity` and `flags` are not trusted from disk; recovery recomputes them. `id`, `version`, `type_id`, `length`, `schema_ver` and the payload are persistent.
- **Payload rules.** The payload is a fixed, documented byte layout per type and schema version: plain data, little-endian, no pointers, no padding with undefined contents. References to other objects are stored as ObjectIDs. A payload must be valid after a `memcpy` to any address, because recovery, resize and future migration all move it.
- **Size.** A typical 200-byte account payload uses 248 bytes plus allocator overhead, so one million such objects need roughly 256 MB, well inside one NUMA node.

### Type and schema metadata

Types are compiled into the binary in v0.1 and registered at startup; there is no runtime type definition language.

```c
typedef struct {
  u32         type_id;        // stable number assigned in the type registry source; never reused
  const char* name;           // "Account"
  u16         schema_ver;     // current payload schema version
  u32         initial_size;   // payload bytes allocated at create
  u32         max_size;       // ≤ 1 MiB
  bool      (*check)(const u8* payload, u32 len);   // structural validation, used at recovery and in debug builds
  const OpDescriptor*    ops;     u16 n_ops;
  const IndexDescriptor* indexes; u16 n_indexes;    // v0.1: at most one per type
} TypeDescriptor;
```

- Every checkpoint manifest stores the type catalog it was written with: `(type_id, name, schema_ver)` for every type. At recovery the runtime compares it with the compiled registry and refuses to start on any mismatch. Schema upgrade functions are deferred; v0.1 treats a schema change as a new data directory.
- TypeIDs 0 to 255 are reserved for the runtime (for example a future system catalog object).

## 3. Object directory

The directory has two levels: a small, read-only slot map answers "which shard owns this ID" with no shared mutable state, and each shard's private local map answers "where is this object in memory" (D3).

```c
#define SNC_SLOTS 4096                       // format constant
static inline u16 snc_slot(ObjectID id) { return snc_hash64(id) >> 52; }

typedef struct {
  u32 epoch;                 // +1 whenever the assignment changes (only at boot in v0.1)
  u16 n_shards;
  u16 owner[SNC_SLOTS];      // slot -> ShardID; 8 KiB
} SlotMap;

// owner_of(id) = slot_map.owner[snc_slot(id)]
```

- **Slot map.** Built at boot, immutable while running. Each shard keeps its own copy in NUMA-local memory, so resolving an owner is one hash and one load from a local, read-only, 8 KiB table. Default assignment: `owner[s] = s % n_shards`. The slot map is persisted in each checkpoint manifest.
- **Local map.** Each shard has an open-addressing hash table from ObjectID to `ObjectHeader*`, written and read only by its owning worker, so it needs no locks or atomics. It is keyed by the full 16-byte ID and uses the low bits of `snc_hash64(id)` (the slot uses the high bits). Load factor stays at or below 0.8; growth doubles the table incrementally over several loop iterations, so no single operation pays the full rehash.
- **Resolution path.** `execute(id, …)` computes `owner_of(id)`. If that is the current shard, it looks up the local map directly. Otherwise it dispatches (section 7) and the owner does the local lookup. There is no global hash table and no lock anywhere on this path.
- **Why slots, not a per-object directory.** A per-object global map costs memory and a shared structure on every lookup, and gives nothing v0.1 needs. Slots keep ownership computable from the ID while still letting a future rebalancer move ownership in 1/4096 steps, with no change to the ID. The cost: the creator cannot choose an object's shard. Co-location of related objects, if benchmarks show it matters, would add a placement-group key to `create()`, not to the ObjectID.
- **Reserved slot states (future).** A later version can mark a slot `MIGRATING` and forward requests; v0.1 has no migration, so every slot is always in the stable state.

## 4. Shard ownership

A shard is the single owner of everything needed to execute against its objects, and only its worker ever mutates them.

| A shard owns | Shared with other shards? |
| --- | --- |
| Its local object map and every object in it | No |
| Its memory heap (section 10) | No; buffers that cross shards are returned for freeing by their origin |
| Its partition of the secondary index (section 11) | No |
| Its WAL stream and checkpoint part files (section 12) | No |
| Its mint lease and sequence counter (section 1) | No |
| Its inbound and outbound rings (section 7) | One producer and one consumer per ring |
| Its copy of the slot map | Read-only copy |

- **Identity.** `ShardID` is a `u16` from 0 to `n_shards - 1`. It names a role in this run, not a core: the shard-to-core-to-NUMA mapping lives in the runtime config and is never persisted.
- **Count.** `n_shards` is fixed for the life of a process. The default is one shard per isolated core, minus one core kept for the control thread and the OS.
- **Assignment.** An object belongs to `owner[snc_slot(id)]` from creation until the process stops. There is no live migration in v0.1 (handoff non-goal).
- **Rebalancing (v0.1 = boot only).** If a restart uses a different shard count or a different slot map, recovery rebuilds the slot map, loads each object into its new owner, and takes a full checkpoint before READY (section 12, rule C9). This is how v0.1 answers "shard assignment and rebalancing semantics": rebalancing exists only as a restart, which is cheap because recovery reloads everything into memory anyway.
- **Mutation rule.** Only the owner mutates an object. Other shards never read an object's memory directly either, even read-only; they send an op. This keeps cross-shard cost visible in the message counters (handoff rule 6) instead of hidden in cache misses.

## 5. Worker model

Each shard is one OS thread pinned to one core, running a cooperative run-to-completion loop; handlers never block, and only the loop itself touches I/O.

**Threads.** `n_shards` worker threads plus one control thread. The control thread does boot, recovery coordination, superblock and manifest writes, checkpoint orchestration and stats export. It never executes ops and never touches object memory.

**Pinning.** A worker pins itself with `sched_setaffinity` before allocating anything, so first-touch puts its heap on the local NUMA node. Where pinning fails (some VMs, containers), the worker runs unpinned and the benchmark report states it. Recommended bare-metal setup: `isolcpus`/`nohz_full` on worker cores and IRQ affinity away from them; these are host settings, not SNC requirements.

**Loop.** One iteration of a worker:

1. Drain inbound rings, round-robin across sources, at most `batch_in` messages per ring (default 32), executing each request or completing each response.
2. Run locally generated work: ops issued by the in-process load driver or by an ingress handler on this shard.
3. Flush the WAL buffer if group-commit conditions are met (section 12) and reap I/O completions; advance `durable_lsn` and release acknowledgements waiting on it.
4. Publish outbound ring tails (one release store per ring that has new messages).
5. Run one bounded slice of background work: checkpoint scan, local-map growth, stats.
6. If nothing happened: spin for `idle_spin_us` (default 50 µs), then sleep on an eventfd until a producer or I/O completion wakes it. Benchmarks may set spin to infinite.

**Handler constraints.** A handler runs inside step 1 or 2 on the owner shard. It must not block, sleep, take locks, make syscalls, allocate outside the provided API, touch any object but its target, or keep any pointer after returning. Each handler call has a wall-clock budget (default 20 µs); overruns are counted per op and reported, not preempted, since a cooperative loop cannot preempt.

**Fairness and backpressure.** All queues are bounded. When an outbound ring is full, the issuing shard keeps the request in a small per-destination overflow list (default 256); when that is full too, the request fails with `E_OVERLOADED`. Nothing grows without bound.

## 6. execute() semantics

`execute()` is asynchronous, runs the op on the owner shard against the authoritative object in place, and completes once the result meets the requested durability.

### Application API

```c
typedef u32 OpID;            // (type_id << 16 | op_code) is NOT assumed; OpIDs are globally unique registry numbers

typedef enum { DUR_DURABLE = 0, DUR_APPLIED = 1 } Durability;

typedef struct {
  u64        expected_version;  // 0 = unconditional
  Durability durability;        // mutations: when to complete. Default DURABLE
  bool       allow_undurable;   // reads: return without waiting for durability. Default false
} ExecOptions;

typedef struct {
  Status      status;
  u64         version;          // object version after the op (or current version for reads)
  bool        durable;          // that version's WAL record is durable
  ByteView    result;           // ≤ 64 KiB; valid until the completion callback returns
} ExecResult;

void snc_execute(ObjectID target, OpID op, ByteView args, ExecOptions opts,
                 void (*done)(void* ctx, const ExecResult*), void* ctx);
void snc_create (u32 type_id, ByteView init_args, ExecOptions opts,
                 void (*done)(void* ctx, ObjectID id, const ExecResult*), void* ctx);
void snc_delete (ObjectID target, ExecOptions opts,
                 void (*done)(void* ctx, const ExecResult*), void* ctx);
```

The callback runs on the calling shard. Callback style is chosen because it maps directly onto both a native loop and Seastar futures; a C++ coroutine wrapper may sit on top, but the core contract is the callback.

### Op registration and the safety model (D5)

```c
typedef enum { OP_READ, OP_MUTATE, OP_CREATE, OP_DELETE } OpKind;

typedef struct {
  OpID        op_id;
  u32         type_id;        // op runs only on objects of this type
  const char* name;
  OpKind      kind;
  u32         max_args, max_result;
  // Phase 1: read-only. May reject. Must not modify the object.
  Status (*validate)(OpCtx*, const ObjectHeader*, const u8* payload, ByteView args);
  // Phase 2: in-place mutation. Must succeed once validate returned OK.
  void   (*apply)(OpCtx*, ObjectHeader*, u8* payload, ByteView args, ResultWriter*);
} OpDescriptor;
```

- **Two phases.** `validate` decides; `apply` mutates in place and cannot fail. This gives all-or-nothing behaviour without copying the object first. Debug builds checksum the object around `validate` and abort if it changed. For `OP_READ`, `apply` only writes the result.
- **What `apply` may do.** Write payload bytes, call `ctx->resize(new_len)` up to the type's `max_size` (validate must first reserve that size with ctx->reserve(new\_len), so the memory exists before apply runs; the runtime may move the object and updates the local map), write the result. Nothing else.
- **Trust.** Handlers are compiled into the same binary and are trusted code; v0.1 has no sandbox. A handler crash takes down the process, and recovery handles it like any crash. Isolation for untrusted handlers (WASM, eBPF, process boundary) is deferred.
- **Determinism.** Not required for correctness, because the WAL logs after-images (D6), but encouraged.

### Execution sequence on the owner shard

1. Look up `target` in the local map. Missing, or flagged `DELETED` → `E_NOT_FOUND`.
2. Check `op.type_id == hdr.type_id` → else `E_TYPE_MISMATCH`. Check args size → else `E_BAD_ARGS`.
3. If `expected_version != 0` and differs from `hdr.version` → `E_VERSION_CONFLICT`.
4. Run `validate`. Non-OK → complete with that status; nothing changed.
5. For index-bearing types, extract the old index key.
6. Run `apply`. For mutations: `version += 1`, extract the new key and update the index if it changed, append a WAL record (after-image), set `last_lsn`.
7. Complete: a mutation with `DUR_DURABLE` waits until `durable_lsn ≥ last_lsn`. A read waits until the object's `last_lsn` is durable, unless `allow_undurable` (D7). Then the result travels back to the entry shard.

Later mutations on the same object do not wait for earlier ones to become durable; they pipeline, and the WAL's order keeps them correct.

### Create and delete

- `snc_create` mints the ID on the entry shard, sends `CREATE` to `owner_of(id)`, and the owner allocates `initial_size` zeroed bytes, runs the type's `OP_CREATE` descriptor (validate, then apply on the fresh payload), sets `version = 1`, inserts into the local map and index, and logs a `CREATE` record.
- `snc_delete` runs the type's `OP_DELETE` descriptor if it has one (to allow "cannot delete a non-empty account" checks), then follows section 8.

### Status codes

| Code | Meaning | Object changed? |
| --- | --- | --- |
| `OK` | Done | Mutations: yes |
| `E_NOT_FOUND` | No live object with this ID | No |
| `E_TYPE_MISMATCH` | Op registered for another type | No |
| `E_UNKNOWN_OP` | OpID not registered | No |
| `E_BAD_ARGS` | Args too large or rejected by size check | No |
| `E_VERSION_CONFLICT` | `expected_version` did not match | No |
| `E_APP(code)` | `validate` rejected; 16-bit app code | No |
| `E_TOO_LARGE` | Resize past `max_size`, or result past `max_result` | No (checked in validate) |
| `E_NO_MEMORY` | Shard heap limit reached | No |
| `E_OVERLOADED` | Rings and overflow full | No |
| `E_SHUTTING_DOWN` | Runtime stopping | No |

A WAL write or `fdatasync` failure is not a status: the process stops (section 12, rule C7).

## 7. Cross-shard dispatch

Shards talk only through a full mesh of single-producer, single-consumer rings of fixed 128-byte slots; when the owner is the current shard, there is no message at all.

- **Local fast path.** `owner_of(id) == self` → the op runs inline in the caller's loop iteration, with no copy of args and no ring. This is the path the handoff calls "keep simple" (rule 5).
- **Topology.** One ring per ordered pair `(src, dst)`, `n_shards × (n_shards − 1)` rings. Each ring has `ring_slots` slots (default 256, a power of two). With 16 shards that is 240 rings × 32 KiB = 7.5 MiB. Rings live in the *consumer's* NUMA node by default; the producer-side alternative is a benchmark variant.
- **Ordering.** FIFO per ring. Two requests from one shard to one object execute in issue order. There is no ordering across different source shards.
- **Batching.** The producer writes slots, then publishes its tail with one release store per loop iteration; the consumer reads the tail once per drain. Head and tail sit on separate cache lines.
- **Responses** travel on the reverse ring `(dst, src)` and are matched by `req_id`.

### Message format

```c
typedef struct {            // offset
  u8       kind;            //  0  EXEC_REQ, CREATE_REQ, DELETE_REQ, RESP, INDEX_QUERY,
                            //     INDEX_RESP, BUF_RETURN, CTRL
  u8       flags;           //  1  ARGS_INLINE, ARGS_EXTERNAL, ALLOW_UNDURABLE, DUR_APPLIED
  u16      src_shard;       //  2
  u32      op_id;           //  4  or type_id for CREATE_REQ, or index_id for INDEX_QUERY
  u64      req_id;          //  8  unique per source shard
  ObjectID target;          // 16
  u64      expected_version;// 32  or, in RESP: resulting version
  u32      len;             // 40  args (REQ) or result (RESP) length
  u16      status;          // 44  RESP only
  u16      reserved;        // 46
  union {                   // 48
    u8  inline_data[80];    //     len ≤ 80: bytes live in the slot
    struct { u8* ptr; } ext;//     len > 80: pointer to a buffer in the sender's heap
  };
} Msg;                      // 128 bytes = 2 cache lines
```

- **Large payloads.** Args or results over 80 bytes go in a buffer from the sender's heap. The receiver reads it in place, then sends `BUF_RETURN` (batched) so the sender frees it on its own heap. The pointer never reaches disk or another node; future transports replace this with copy or RDMA.
- **Creates.** `CREATE_REQ` carries the freshly minted ID in `target` and `type_id` in `op_id`.
- **CTRL.** Control thread to shard messages (begin checkpoint, stop) use a separate ring per shard so they are never stuck behind data traffic.
- **No retries or idempotency keys** are needed in-process: rings do not lose messages. `req_id` is kept so a future network transport can add dedup without changing the format.
- **Counters**, per ring: messages, bytes inline, bytes external, full events, and a sampled enqueue-to-dequeue latency. These feed the report's "cross-core messages" and "bytes copied" columns.

## 8. Object lifetime

Objects exist from a create to an explicit delete; there is no garbage collection or reference counting, and an ID once deleted stays dead forever.

| State | Entered when | Visible to ops as | Leaves when |
| --- | --- | --- | --- |
| Live, not yet durable | `CREATE` applied on owner | The object (reads wait for durability by default) | Its `CREATE` record becomes durable |
| Live | Its latest record is durable | The object | A delete is applied |
| Tombstone | `DELETE` applied: payload freed, header kept with `DELETED` flag, index entry removed | `E_NOT_FOUND` (by default after the delete is durable) | The `DELETE` record becomes durable |
| Gone | Tombstone reclaimed | `E_NOT_FOUND` | Never; the ID is not reused |

- **Reclamation is immediate and local.** No pointer to an object survives a handler call and no other shard reads object memory, so the owner frees the payload when it applies the delete. The 48-byte tombstone exists only so that durable-by-default reads of a just-deleted ID wait for the delete record rather than reporting a deletion a crash could undo. Once `durable_lsn` passes it, the tombstone is erased from the local map.
- **Resize** may move an object to a new allocation; the local map entry is updated in the same handler call. Object addresses are never stable across ops.
- **References between objects** are ObjectIDs and are weak: deleting X does not touch objects that mention X, and following a dangling reference returns `E_NOT_FOUND`. Referential integrity is application logic.
- **Crash during lifetime.** A create or delete that was not durable at the crash is absent after recovery; it was never acknowledged, so no caller relied on it. Recovery never resurrects an object whose durable `DELETE` exists, because replay is version-gated (section 12).

## 9. Concurrency and versioning

Each object is linearizable on its own, across crashes, because its owner executes ops one at a time and, by default, nothing is acknowledged or shown until it is durable; there are no guarantees spanning two objects.

- **Serial per object.** The owner runs every op on an object to completion before the next, so handlers see no concurrency and use no locks. Objects on different shards run in parallel; objects on the same shard interleave only between ops.
- **Version.** `u64`, 1 after create, +1 for every successful `OP_MUTATE`, and the `DELETE` record carries `version + 1`. Reads and rejected ops do not change it. It never goes backwards, including across recovery, and it is never reset.
- **Linearization point** is `apply` on the owner. With the defaults (durable mutations, durable reads), every state a caller can observe is durable, so a crash cannot roll back anything a caller saw. That property is what makes "objects are the authoritative state" safe to claim.
- **Optimistic checks.** `expected_version` gives compare-and-set on one object. Read-modify-write across round trips should use it.
- **What callers can rely on across objects.** Nothing beyond per-ring FIFO: two ops a shard sends to different objects may complete in either order, and there is no cross-object snapshot. Index lookups are not snapshot-consistent either (section 11).
- **MVCC.** None. One current version per object in memory; older versions exist only in the WAL until a checkpoint retires it.
- **Deferred, on purpose.** Multi-object transactions. The v0.1 benchmark workload is chosen so it does not need them (section "Benchmark workloads"). The first likely extension is a two-object op on the same shard, which needs no protocol; cross-shard atomicity comes after.

## 10. Allocator and memory layout

Every shard allocates from its own NUMA-local heap with no locks, objects use ordinary pages by default, and only small, hot runtime structures ask for HugeTLB; the page-size question is answered by benchmark, not by this spec (D9).

```c
typedef struct ShardAllocator ShardAllocator;   // one per shard, created on the pinned worker thread
void*  sa_alloc  (ShardAllocator*, size_t bytes, size_t align);
void*  sa_resize (ShardAllocator*, void* p, size_t new_bytes);  // may move
void   sa_free   (ShardAllocator*, void* p);                     // only by the owning shard
size_t sa_usable (ShardAllocator*, void* p);                     // sets ObjectHeader.capacity
void   sa_stats  (ShardAllocator*, AllocStats*);
```

- **First implementation.** A per-shard mimalloc first-class heap (`mi_heap_new` on the worker thread). It is proven, thread-local, fast for small sizes and reports usable size, which becomes `capacity`. A custom size-class slab allocator replaces it only if benchmarks show allocator time or fragmentation matter (handoff rules 2 and 8). Under a Seastar backend, Seastar's per-shard allocator implements the same interface.
- **No cross-shard frees.** A buffer is freed by the shard that allocated it (section 7, `BUF_RETURN`).
- **Limits.** Each shard has a configured byte limit; reaching it returns `E_NO_MEMORY` from `validate` (resizes reserve their memory in `validate`, so `apply` cannot fail).
- **Fragmentation.** No compaction in v0.1. The report includes live bytes versus resident bytes per shard.

### Page backing, per structure

| Structure | Size at 1M objects, 16 shards | Default backing | Benchmark variants |
| --- | --- | --- | --- |
| Slot map copies | 8 KiB per shard | Normal pages (fits in L2) | None |
| Local object maps | About 32 MiB total | 2 MiB HugeTLB if reserved, else THP `madvise` | 4 KiB, THP, HugeTLB 2 MiB |
| Cross-shard rings | About 7.5 MiB total | 2 MiB HugeTLB if reserved, else THP `madvise` | 4 KiB, HugeTLB |
| Index partitions | Tens of MiB | THP `madvise` | 4 KiB, THP |
| Object heap | About 256 MiB | 4 KiB pages | THP, HugeTLB 2 MiB, HugeTLB 1 GiB |
| WAL buffers | 2 × 1 MiB per shard, 4 KiB aligned | 4 KiB pages, locked | None |

The sizes are estimates for a 200-byte payload and a 16-byte key per entry; the benchmark records the real ones. Every allocation of these structures happens on its owning worker after pinning, so first-touch puts it on that worker's node; the control thread checks placement with `move_pages` in debug runs and reports any remote pages.

## 11. Indexing

The v0.1 secondary index (`Account.email → ObjectID`) is a shard-local, non-unique equality index, updated synchronously inside the mutating op and rebuilt at recovery rather than logged; a lookup asks every shard (D8).

```c
typedef struct {
  u32         index_id;
  u32         type_id;
  const char* name;                                    // "Account.email"
  // Returns key length (0 = object not indexed); writes at most 256 bytes to key_out.
  u32 (*extract)(const u8* payload, u32 len, u8* key_out);
} IndexDescriptor;

void snc_index_lookup(u32 index_id, ByteView key, u32 max_results,
                      void (*done)(void* ctx, Status, const ObjectID* ids, u32 n), void* ctx);
```

- **Structure.** Per shard, a hash multimap from key bytes to a small vector of ObjectIDs. Entries store IDs, not pointers, so resizing an object never touches the index; a lookup that wants the object resolves the ID through the local map.
- **Update.** In step 5 and 6 of execution (section 6), the owner extracts the key before and after `apply`. If it changed, it removes the old entry and inserts the new one in the same handler call. Create inserts; delete removes. Index changes are never visible without the object change and vice versa.
- **Lookup.** The entry shard sends `INDEX_QUERY` to every shard (and runs its own part inline), each shard returns matching IDs, and the entry shard merges them. Each shard skips entries whose object is not yet durable, consistent with durable reads. Cost: `n_shards − 1` round trips per lookup, which the report shows on its own line.
- **Consistency.** The result is not a snapshot: a lookup racing with an email change may miss the object or see it under both keys on different shards. Callers that need certainty re-read the object and check the field.
- **Uniqueness.** Not enforced in v0.1. Enforcing unique emails needs either a key-partitioned index with an atomic two-shard update or a reservation protocol, both of which are cross-shard atomicity, which the handoff defers. The workload generator produces unique emails, and the correctness checker verifies them.
- **Persistence.** Indexes are not written to the WAL or checkpoints. Recovery rebuilds each shard's partition by scanning its local map after replay; the report shows index rebuild time separately so the trade-off is measured.

## 12. WAL, checkpoint and recovery

Each shard writes its own WAL stream of full after-images with group commit; checkpoints are fuzzy and per shard; recovery loads the newest checkpoint, replays each stream with version-gated redo, rebuilds indexes, and only then reports READY (D6).

### On-disk layout

```text
<data_dir>/
  superblock.a  superblock.b            4 KiB each, written alternately
  wal/s000/0000000000000001.wal         per-stream segments, 64 MiB, pre-zeroed
  wal/s001/...
  ckpt/0000000000000007/manifest        checkpoint 7
  ckpt/0000000000000007/part-000.ckpt   one part per stream
```

```c
typedef struct {                 // 4 KiB block, rest zero
  u8  magic[8];                  // "SNCSUPR1"
  u16 format; u16 reserved; u32 crc32c;   // crc over the 4 KiB with this field zeroed
  u64 generation;                // +1 per write; the valid copy with the highest wins
  u8  space_uuid[16];            // identifies this object space; checked in every file header
  u64 next_lease;                // section 1
  u64 active_ckpt;               // checkpoint sequence, 0 = none yet
} Superblock;
```

### WAL streams and records

One stream per shard; LSNs are per stream, start at 1, and increase by exactly 1 per record. A segment starts with a 4 KiB header block (magic `SNCWAL01`, format, stream, segment number, first LSN, `space_uuid`, CRC).

```c
typedef struct {          // offset
  u32      crc32c;        //  0  over bytes [4, total_len)
  u32      total_len;     //  4  header + payload, rounded up to 8
  u64      lsn;           //  8
  u16      rec_type;      // 16  CREATE=1, UPDATE=2, DELETE=3, IMAGE=5 (checkpoint parts only)
  u16      flags;         // 18
  u32      type_id;       // 20
  ObjectID id;            // 24
  u64      version;       // 40  object version after this record
  u16      schema_ver;    // 48
  u16      reserved;      // 50
  u32      payload_len;   // 52  0 for DELETE
} WalRecord;              // 56 bytes, then payload_len bytes of after-image
```

- **After-images (D6).** `CREATE` and `UPDATE` carry the whole payload after the op. Replay needs no handler code, so changing a handler can never corrupt recovery, at the cost of write amplification for large objects. The report measures bytes logged per op; delta or logical logging is a v0.2 option.
- **Group commit.** Records go into the shard's active 1 MiB buffer. A flush starts when the buffer holds 64 KiB, or the oldest unflushed record is `group_commit_us` old (default 50 µs), or the loop is idle with callers waiting. A flush zero-pads to the next 4 KiB boundary, writes with `O_DIRECT` through io\_uring, then `fdatasync`s; the next flush starts at that boundary. At most one flush is in flight per stream, and the other buffer fills meanwhile.
- **Readers** treat `total_len == 0` as "skip to the next 4 KiB boundary". Padding wastes space on small flushes; the report shows it as write amplification. Rewriting a partial tail block is allowed later only on devices with a verified atomic write unit of at least 4 KiB.
- **Segments** are created and zero-filled ahead of need by the control thread, so steady-state `fdatasync` has no file metadata to flush.
- **Sync modes.** `wal_sync = fdatasync` (default) or `none` for development VMs; `none` is printed on every benchmark report.

### Durability acknowledgement

A mutation with `DUR_DURABLE` completes when its stream's `durable_lsn` (the last LSN of the newest completed flush) is at or past its record. `DUR_APPLIED` completes after `apply` and may be lost in a crash. Reads follow D7 (section 6).

### Checkpoint

1. The control thread picks sequence `C` and sends `CKPT_BEGIN(C)` to every shard.
2. Each shard records `start_lsn` = its next LSN, then walks its local map in bounded slices (default 256 objects per loop iteration) between normal ops, appending an `IMAGE` record (same layout as above) for every live object to `part-<stream>.ckpt`. Tombstones are skipped.
3. When the walk ends, the shard waits until its `durable_lsn` covers the highest `last_lsn` it wrote into the part, then `fdatasync`s the part and replies with `start_lsn`, object count, byte count and the part's CRC.
4. The control thread writes the manifest (sequence, `space_uuid`, slot map, `n_shards`, per-stream `start_lsn`, type catalog, index descriptors, part list with CRCs), syncs it and the directory, then writes the older superblock copy with `active_ckpt = C` and syncs it.
5. Only after that, WAL segments whose last LSN is below their stream's `start_lsn` and the previous checkpoint directory are deleted.

Defaults: every 60 s or when any stream holds more than 1 GiB of WAL. Objects that change during the walk are fine: the part may hold an older or newer image, and replay from `start_lsn` with the version rule below converges on the latest durable state.

### Recovery

1. Read both superblock copies; use the valid one with the highest generation. If neither is valid, start only with an explicit `--init` on an empty directory.
2. Load the manifest of `active_ckpt`. Compare its type catalog with the compiled registry; refuse to start on mismatch.
3. Build the slot map: the manifest's if `n_shards` is unchanged, otherwise the default for the new count (a remap).
4. Start and pin workers. Assign parts and WAL streams to shards round-robin; each reader routes each record to `owner_of(id)` over the rings (or applies it directly when it owns it).
5. Load checkpoint images, then replay each stream from its `start_lsn` to its end of log, applying the version rule.
6. Rebuild each shard's index partition from its local map.
7. Reserve new mint leases in the superblock (section 1).
8. If the slot map changed, take a full checkpoint now.
9. Report READY with a timing breakdown: superblock and manifest, checkpoint load, WAL replay, index rebuild, final checkpoint.

**Version rule (redo is idempotent).** `CREATE`, `UPDATE`, `IMAGE`: install the image if the object is absent or its in-memory version is lower. `DELETE`: remove the object if present with a lower version, otherwise ignore. Every record carries a full image, so an `UPDATE` for an absent object simply installs it.

**End of log.** A stream ends at the first record whose CRC fails, whose `lsn` is not the expected next value, or whose header is all zero after skipping to the next block and finding nothing valid there.

### Crash-consistency rules

| Rule | Statement |
| --- | --- |
| C1 | No `DUR_DURABLE` mutation, and by default no read, completes before the record it depends on is durable. |
| C2 | Within a stream, durability is a prefix: a flush starts only after the previous one completed. |
| C3 | Every record is self-validating (CRC plus LSN continuity). |
| C4 | An invalid record followed later in the stream by a valid record with a higher LSN is corruption, not a torn tail: refuse to start and report it. |
| C5 | A checkpoint part contains only state whose WAL record is already durable. |
| C6 | A checkpoint exists only once the superblock names it; the active superblock copy is never overwritten. |
| C7 | Any write or `fdatasync` error stops the process immediately. No retry, because the page cache state after a failed sync is unknowable. |
| C8 | Creating, renaming or deleting a file is followed by `fsync` of its directory before anything relies on it. |
| C9 | A slot-map change at boot is followed by a full checkpoint before READY, so no object's history ever spans two streams. |
| C10 | WAL is deleted only after a checkpoint whose `start_lsn` is past it is named by the superblock. |
| C11 | Replay is idempotent under the version rule; replaying any suffix twice gives the same state. |
| C12 | New mint leases are durable before READY, so IDs are never minted twice. |
| C13 | Startup never initializes over an existing data directory without `--init`. |

## Runtime and transport boundaries

SNC core code (directory, execution, index, WAL, recovery) calls only the small runtime interface below, so Seastar, a native backend, or anything later can sit underneath without changing SNC's abstractions (handoff rule 3; D2).

```c
// SNC Runtime Interface (SRI): everything the core needs from an execution runtime
u16   sri_shard_id(void);
u16   sri_shard_count(void);
void  sri_send(u16 dst, const Msg*);                 // enqueue to ring (self, dst); may report full
void  sri_on_message(void (*fn)(const Msg*));        // called from the shard loop
void  sri_poll_hook(bool (*fn)(void));               // per-iteration work (WAL flush, ckpt slice)
u64   sri_now_ns(void);
void  sri_timer(u64 at_ns, void (*fn)(void*), void* ctx);
// File I/O, completion-based, owned by the calling shard
int   sri_open_direct(const char* path, int flags);
void  sri_write(int fd, u64 off, const void* buf, u32 len, void (*done)(void*, int), void* ctx);
void  sri_fdatasync(int fd, void (*done)(void*, int), void* ctx);
void  sri_read(int fd, u64 off, void* buf, u32 len, void (*done)(void*, int), void* ctx);
ShardAllocator* sri_allocator(void);
const ShardTopology* sri_topology(void);             // core, NUMA node, pinned or not
```

- **Native backend (recommended first).** pthreads with `sched_setaffinity`, the SPSC ring mesh from section 7, io\_uring for file I/O, eventfd for idle wakeup, mimalloc heaps. It is a few thousand lines, every cost in it is visible, and it is the baseline Seastar has to beat to earn its dependency weight.
- **Seastar backend (evaluated second).** `sri_send` maps to Seastar's cross-shard queues (or the same rings via a reactor poller), file I/O to Seastar's DMA file API, timers and allocator to Seastar's own. Seastar's futures stay inside the adapter; no Seastar type crosses the interface. The evaluation compares both backends on the same benchmark and records lines of adapter code, latency and CPU per op.
- **Transport boundary (future).** Ingress is a separate interface: `ingress_submit(entry_shard, ObjectID, OpID, args, completion)`. The in-process load driver implements it in v0.1; a socket front end implements it for baseline comparisons. A future wire protocol should put the 16-byte ObjectID in a fixed position in the first 64 bytes of a request so a NIC-side or RX-worker classifier can find the owner without parsing more (handoff §17). The networking OS would later implement this interface; nothing in SNC depends on it now.

## Benchmark workloads and correctness checks

The v0.1 workload is a bank-style `Account` object, chosen because every op touches exactly one object yet it exercises create, read, mutate, reject, index update and index lookup.

### The Account type (schema 1, 200-byte payload)

```c
typedef struct {
  i64  balance_cents;   //   0
  u64  op_count;        //   8
  u64  created_ns;      //  16  set by the caller's args, not the clock, so replay is exact
  u64  updated_ns;      //  24  same
  u8   email_len;       //  32
  u8   name_len;        //  33
  u16  flags;           //  34
  u32  reserved;        //  36
  char email[64];       //  40  indexed by Account.email
  char name[96];        // 104
} AccountV1;            // 200 bytes
```

| Op | Kind | Validate rejects | Notes |
| --- | --- | --- | --- |
| `create(email, name, balance)` | CREATE | Bad lengths | Inserts the index entry |
| `get()` | READ | None | Returns the 200 bytes |
| `deposit(amount)` | MUTATE | Overflow | The common write |
| `withdraw(amount)` | MUTATE | Insufficient funds (`E_APP`) | Exercises rejection without state change |
| `set_email(email)` | MUTATE | Bad length | Moves the index entry |
| `lookup(email)` | Index lookup | None | Broadcast to all shards |

### Runs

1. **Load.** Create 1,000,000 accounts spread over all shards. Measures create throughput and WAL rate.
2. **Mixed.** 50% `get`, 50% `deposit`, uniform keys.
3. **Skewed.** 95% `get`, 5% `deposit`, Zipfian keys (θ = 0.99).
4. **Locality sweep.** Run 2 with targets forced 0%, 50% and 100% cross-shard, and with shard pairs on the same versus different NUMA nodes. This isolates the cost of section 7 (handoff §11).
5. **Index.** 90% `lookup`, 10% `set_email`.
6. **Restart.** Run 2, stop cleanly, restart, verify. Then run 2 again and `kill -9` at a random moment, restart, verify, and record recovery time. This is the handoff milestone.

Each run is done with `DUR_DURABLE` and with `DUR_APPLIED`, closed-loop for peak throughput and open-loop at fixed rates for latency (open-loop avoids hiding queueing delay).

### Correctness checker

The driver logs every acknowledged `(id, version, payload hash)`. After any restart, for every ID: the object exists if its create was acknowledged and its delete was not; its recovered version is at least the highest acknowledged version and at most the highest issued version; if the versions are equal, the payload hash matches. Index lookups for a sample of emails must match a full scan.

### Baselines

Same logical ops, same durability (sync on commit), same machine:

- **Embedded conventional:** RocksDB with synced WAL, driven by the same in-process driver: get, deserialize, modify, serialize, put. This isolates the database-to-application boundary cost the thesis is about.
- **Redis** with `appendfsync always`, over loopback, through the socket ingress.
- **PostgreSQL** with `synchronous_commit = on`, prepared `UPDATE … RETURNING`, over loopback.

### What the report measures

Ops/sec; p50, p95, p99, p99.9 latency per op type (HDR histograms); cycles, instructions, cache and TLB misses per op (`perf stat`); cache-line transfers (`perf c2c`); remote NUMA accesses; memory per object; allocations per op; context switches and syscalls per op; bytes copied and cross-shard messages per op (SNC counters); WAL bytes per logical byte changed; recovery time by phase. Every report prints the host, kernel, pinning status, page backing, `wal_sync` mode and backend.

## Open questions from the handoff

All 19 open questions in handoff §29 have an answer in this draft; two are answered only for v0.1 and stay open for later versions.

| Handoff question | Answer in this spec | Where |
| --- | --- | --- |
| Exact 128-bit ObjectID format and stale-handle detection | Format byte + 56-bit lease + 64-bit sequence; never reused, so stale IDs return `E_NOT_FOUND`; stale state caught by `expected_version` | 1 |
| Object header and payload representation | 48-byte header, relocatable plain-data payload, same bytes in memory and on disk | 2 |
| Type/schema metadata | Compiled-in `TypeDescriptor` registry; catalog checked at recovery; no upgrades in v0.1 | 2 |
| Ownership-table representation | 4,096-slot map plus per-shard local hash maps | 3 |
| Shard assignment and rebalancing semantics | Hash slot at create; rebalancing only at restart, followed by a checkpoint. Live rebalancing stays open for v0.2+ | 4 |
| Exact `execute()` ABI/API | Callback-based C API with `ExecOptions` and `ExecResult` | 6 |
| Operation registration and safety model | `OpDescriptor` with two-phase validate/apply; trusted compiled-in handlers. Untrusted handlers stay open | 6 |
| Cross-shard message format | 128-byte `Msg` on SPSC rings, external buffers for large args | 7 |
| Memory allocator strategy | Per-shard mimalloc heaps behind `ShardAllocator`; page backing per structure, benchmarked | 10 |
| Object lifetime and reclamation | Explicit create/delete, immediate local free, short-lived tombstones, no ID reuse | 8 |
| Versioning semantics | u64 per object, +1 per mutation, monotonic across recovery | 9 |
| Index update semantics | Synchronous in the mutating op, shard-local, rebuilt at recovery | 11 |
| WAL record format | 56-byte header + after-image, per-shard streams | 12 |
| Durability acknowledgment semantics | `DUR_DURABLE` default after `fdatasync`; reads wait for durability by default | 6, 12 |
| Checkpoint format | Per-stream parts of `IMAGE` records plus a manifest named by an A/B superblock | 12 |
| Crash-consistency rules | C1 to C13 | 12 |
| Benchmark workload(s) | Account workload, six runs, three baselines | Benchmark |
| Seastar integration boundary | SNC Runtime Interface; Seastar as an adapter behind it | Runtime boundary |
| Future transport interface boundary | `ingress_submit`; ObjectID early in a fixed-size header | Runtime boundary |

**Next step after cody signs off:** create the repository with the runtime interface, the native backend skeleton and the Account type, following handoff §24 steps 1 to 7, and measure the local fast path before building anything else.
