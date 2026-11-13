# Audit A (object model, lifecycle/transactions, scheduling) — subagent report; spot-check before citing

Classifications: ObjectID/slot/LocalMap Verified; Registry/types/execute Partially verified; Lifecycle & transaction boundaries Partially verified; Scheduling/worker/cross-core Partially verified; links_generation, placement text, lifecycle gate, stage chain: Implemented with focused tests (not audited in depth).

## 1. ObjectID, slot indirection, LocalMap: Verified
- ObjectID 8-bit format + 56-bit lease + 64-bit seq (include/snc/object_id.h:20-40); per-shard minter (object_id.h:51-60); XXH3-64 (src/core/object_id.cpp:11); slot = top 12 bits (include/snc/slot_map.h:15); owner = s % n_shards (slot_map.h:26), immutable per run; LocalMap open addressing, backward-shift delete (include/snc/local_map.h:104-125). No routing on lease/seq found.
- Tests: test_object_id.cpp (round trip, 10k unique, shard balance within 3% over 400k, hash == fixed formula); test_local_map.cpp (1M random ops vs std::map); test_execute.cpp (found only on owner shard).
- Unverified: hash only self-consistency checked (test_object_id.cpp:53), no golden vector although claimed frozen for persisted placement; cross-process lease disjointness is control-plane (test_control.py).
- Risks: shard lease = lease_base + shard_id (engine.cpp:66) leaks minting shard into the ID (soft tension with invariant 2); LocalMap growth single-step rehash stall (local_map.h:12-15); duplicate mint aborts (shard.cpp:731-734).

## 2. Registry / types / execute: Partially verified
- registry.cpp:17-57 validates; run_exec shard.cpp:540-711 order: fence/standby, session replay, serving, find, type check, followup gate, args, version, validate, admit/reserve, apply, version++, log, queue emits. OpCtx::local() atomic two-object op on one shard (shard.cpp:680-710). Account is the only user type (src/types/account.cpp).
- Unverified: multi-type untested; registry read-only-after-start not enforced (registry.cpp:78-81 mutable static); apply failing after validate is a contract enforced only by debug checks/abort.
- Risks: Registry::type()/index() linear scans (registry.cpp:61-66; type() per op at shard.cpp:632); contract violations abort process even in release (shard.cpp:57-60); no schema migration (schema_ver equality only, engine.cpp:1011); recovery aborts on any image failing type->check (shard.cpp:1798-1802) -> crash loop on a CRC-valid corrupt image.

## 3. Lifecycle & transaction boundaries: Partially verified
- Atomic unit: single object per op + optional same-shard peer via OpCtx::local(), implemented as one WAL group chained with kRecGroupNext (shard.cpp:690, 1241); recovery delivers a group only when closing record read (storage.cpp:417-435); torn group dropped whole; group never spans a segment (storage.cpp:655). docs/TRANSFERS.md:26-27: saga/outbox, "not a transaction"; STATUS.md:455 and CORE_CONTRACTS.md stage table agree.
- create: shard.cpp:715-786, always waits durable even APPLIED (782). mutate: APPLIED in memory first, then WAL append, ack waits durable(last_lsn) (shard.cpp:679-705, complete_local 461-477); other ops on shard see state at APPLIED. delete: tombstone until DELETE record durable (shard.cpp:838-870). Cross-shard/group: debit + outbox durable, credit later via follow-up (shard.cpp:2902-2947), resume hooks re-emit after recovery.
- Tests: test_transfer.cpp (conservation after SIGKILL, torn-group drop ~254-286, remap, create_near ~304-377); test_execute (tombstone not aliasing); test_intake F6. Agent did not read test_transfer/test_crash in depth.
- Unverified: cross-shard atomicity none (reader can see debit without credit; CORE_CONTRACTS s5; design property); power-loss UNPROVEN (CORE_CONTRACTS H1,H3); local follow-ups skip durable wait on WAL-prefix ordering argument (shard.cpp:2961-2966).
- Risks: no multi-object/snapshot/version-pinned read (CORE_CONTRACTS s5); session records store wall-clock ::time(nullptr) (shard.cpp:1236) -> replay nondeterministic for TTL; sessions not migrated with slots (CORE_CONTRACTS s1).

## 4. Scheduling/workers/cross-core: Partially verified
- Pinning via sched_setaffinity (src/native/runtime.cpp:450-464); NUMA node recorded only (466-469). Full mesh SPSC rings + per-destination overflow deques; requests refused E_OVERLOADED past overflow_limit; responses/buffer returns/must_deliver use unbounded overflow (runtime.cpp:119-133,160-170). Entry: Shard::execute (shard.cpp:170-244) local if owner else send_request; sequenced (client_id) requests never forwarded, get E_NOT_OWNER (230-236). Cross-shard requests batched into MULTI_REQ up to 32 (shard.cpp:1456-1500,1541-1575,1577-1640), flushed once per loop iteration (engine.cpp:145).
- Tests: test_runtime.cpp (per-pair FIFO, no loss/dup with forced overflow; header read only); test_execute (75% remote); test_intake (backpressure, deadlock freedom, follow-up ordering); test_lifecycle; test_gauges.cpp:215 only test touching batch_remote=false.
- Measured per docs: CROSSCORE.md:6-8,25-30: 100% remote 0.67x locked baseline at shallow depth, 1.20x at 512 in flight; TRANSFERS.md: SNC slower than locked baseline for random pairs in memory. All on 4-vCPU VM.
- Unverified: NUMA-local placement; scaling beyond a handful of shards; slot map static (slot_map.h:12 "only at boot in v0.1"; remap only at recovery when shard count changes, engine.cpp:1020-1023).

## Cross-cutting hazards (agent-claimed; H1 to be verified by me)
H1. Op-level order not preserved between batched and unbatched cross-shard requests from one origin to one destination: send_request holds batchable EXEC requests in out_batches_ until flush_batches (shard.cpp:1478-1493, engine.cpp:145); a non-batchable request (version-conditioned, args >16 B, CREATE, DELETE) to the same destination goes straight to the ring (shard.cpp:1503-1531) and can overtake earlier batched ops. No test; no ordering contract documented.
H2. docs/CORE_CONTRACTS.md line citations stale (refuse_unserved now shard.cpp:154-168; standby gate 171-173 and 541-558; APPLIED/create durability 782, 703-705). Claims hold, refs need refresh.
H3. ClusterMap never freed: every adopted placement pushed into maps_ (engine.cpp:1604-1606): one map per epoch for process lifetime.
H4. Unbounded overflow for must_deliver (runtime.cpp:128); bound relies on upstream admission (checked structurally only).
H5. tests/test_placement.cpp covers text encoding + witness CAS only (header 1-16), not routing/slot ownership/migration; do not cite as placement-behaviour evidence.
H6. Registry linear lookups on hot path (registry.cpp:61-66; shard.cpp:632).
H7. Process-aborting contract checks in release (shard.cpp:57-60); recovery abort on bad image (shard.cpp:1798-1802).
H8. Invariant 18 holds where looked: replay check precedes E_OVERLOADED (shard.cpp:196-208) and precedes serving/E_MOVED/E_MIGRATING (558-566); E_STANDBY and E_NOT_OWNER come before session lookup (171-173, 230-236). CORE_CONTRACTS labels INFERRED safe -> Partially verified.
