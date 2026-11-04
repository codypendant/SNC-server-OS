# Causal tracing (deliverable E)

Status: **PROPOSED** design, plus one **as-built stopgap** that this work
order used to measure the 100K false failover
([STABILIZATION_REPORT.md](STABILIZATION_REPORT.md) section D).

This is engineering observability: enough to explain a failover, a
migration, a recovery or a retry after the fact. It is not an audit log.
It holds no contract state, and losing it never changes behavior.

## 1. As built: `SNC_TRACE=1` (stopgap)

`include/snc/trace.h`: `SNC_TRACE(fmt, ...)` writes one line with one
`write()` to stderr:

```
T <CLOCK_MONOTONIC ns> <pid> <event> k=v ...
```

- **Enabled** by `SNC_TRACE=1` in the environment, read once into a
  function-local static.
- **Disabled cost:** INFERRED (not measured, section 3): one load and one branch. There is no formatting and
  no clock read; each call site guards its own `now_ns()` with
  `trace::enabled()`.
- **Enabled cost:** `snprintf` plus a `write` syscall per line. So it is
  used only on control paths and for rare events: loop iterations of
  20 ms or more, `STATUS` answers of 20 ms or more, witness polls that
  failed or were slow, admin requests, migration steps. It is never used
  per operation.
- **Merging:** `bench/lab/trace_merge.py RUN_DIR` merges every process's
  lines into one timeline. This works only for processes on one host,
  since `CLOCK_MONOTONIC` is per host.
  - `bench/lab/cpu_sample.py` adds per-thread CPU and run queue every
    100 ms to the same timeline.
  - `lab.py` starts the sampler and keeps the trace lines (`*.trace`)
    when `SNC_TRACE=1`.

Events emitted today:

| Event | Where | Fields |
| --- | --- | --- |
| `shard_long_iter` | `src/native/runtime.cpp` loop, iteration ≥ 20 ms | shard, total, drain, inject (+count), io, poll, publish (ms) |
| `admin_status_slow` | `tools/snc_server.cpp`, `replication_status` ≥ 20 ms inside a STATUS | `replication_status` (ms). Before HEALTH it also carried total and read ms; those are now in `admin_heavy_slow` |
| `admin_heavy_slow` | `tools/admin_loop.h` worker, a queued verb (STATUS, AUDIT, PROMOTE ...) ≥ 20 ms from accept to answer | verb, total, queue wait, run (ms) |
| `admin_inline_slow` | `tools/admin_loop.h`, an inline verb (HEALTH, DEBUG) ≥ 20 ms from accept to answer | verb, total (ms) |
| `admin_move_in` / `admin_move_out` / `admin_read_fail` | admin port (`admin_read_fail`: no whole line within 500 ms, or closed first) | verb, bytes, read / handle / write ms, reply prefix |
| `ask_witness` | primary → witness (DEGRADE, SYNCED) | port, request, ok, ms, reply |
| `stage_begin` / `stage_end` / `seal` / `export` | `src/core/engine.cpp` | slot, move, final, objects, on-shard ms, durability-wait ms, status |
| `stage_unit` | `Engine::stage_slot`, each unit of a stage chain (S1, MIGRATION_STAGING.md 18) | slot, move, final, phase the unit started in, unit µs, puts and drops so far, bytes appended not yet durable, status. (Replaces `stage_slot_phases` of the one-task final stage.) |
| `slot_active` | `Shard::activate_staged`, the ACTIVE record written at the destination | slot, move, epoch (the end of the move's `E_MIGRATING` window, with `seal` at the source its start) |
| `move_stage_req` | `tools/migrate.h` node handler | decode ms, control-plane check ms |
| `driver`, `driver_ask` | `tools/migrate.h` driver | step text; each request's verb, bytes, ms, reply |
| `witness_ask` | `tools/snc_witness.cpp`, a HEALTH or STATUS poll that failed or took ≥ 20 ms (was `witness_status`, STATUS only) | group, node, verb, ok, ms, reply |
| `witness_suspect` / `witness_propose_promote` | witness | silence ms, degraded; proposal result, ms |
| `cpu_thread` / `cpu_host` | `bench/lab/cpu_sample.py` | tid, %CPU, state, CPU; run queue, PSI |

Known limits:

- Text through stderr.
- One host only.
- No correlation IDs beyond slot, move, port and pid.
- Long iterations are reported only when they end, so a stall that
  never ends is invisible. The `witness_*` lines show it from outside.

## 2. Proposed design: bounded per-thread event rings

### Record

```cpp
struct TraceEvent {          // 64 bytes, one cache line
  u64 t_ns;                  // CLOCK_MONOTONIC
  u32 type;                  // event type (enum, below)
  u16 node;                  // NodeID low bits (0 = unclustered)
  u16 shard;                 // shard, or 0xFFFF for non-shard threads
  u64 corr;                  // correlation ID (below)
  u64 a, b, c, d;            // type-specific fields: ObjectID halves, slot, group, LSN, ms, status
  u32 e;                     // type-specific
  u32 seq;                   // per-ring sequence, to detect overwrite on dump
};
```

### Correlation IDs

- **Migration:** the move ID (already global; it is the placement
  version that created the move).
- **HA:** `(group << 48) | term`.
- **Placement:** epoch.
- **Client request:** `client_id` and `seq` (sequenced) or `req_id`.
- **Follow-up:** the emit's origin `(ObjectID, outbox index)`.
- **Recovery:** the process start time.

The two sides of an interaction log the same ID, so a merge can join
them without trusting clocks across hosts.

### Storage

- **Shard threads:** one ring per shard thread, a single writer with no
  locks. A power-of-two capacity, overwrite-oldest. Default 64K events,
  which is 4 MiB per shard.
- **Other threads** (admin, MOVE handlers, replication sender/receiver,
  witness, main): each has its own ring, registered on creation.
- **Write path:** take the slot, store, publish `seq` with a release
  store. No allocation, no syscall, no format.
- **Reader** (dump only): copies the rings and drops slots whose `seq`
  changed during the copy.

### Enablement and cost budget (to be measured before fixing numbers)

- **Compile-time:** `SNC_TRACE_RINGS=0` removes it.
- **Runtime:** a per-category bitmask (shard loop, admin, HA, migration,
  replication, recovery, client).
  - Disabled cost target: one predictable branch.
  - Enabled cost target: tens of ns per event (a TSC or vDSO clock read
    plus a 64-byte store).
- **Measure first:** `snc_bench` with the categories off, then on.

Per-operation events (accept, apply, commit, ack) stay off by default,
even when enabled. They are a separate category, so a 1M ops/s shard
does not pay for them unless asked.

### Clock

- `CLOCK_MONOTONIC` (vDSO).
- Each ring's header holds one `(monotonic, realtime)` anchor pair taken
  at dump. A cross-host merge aligns hosts by anchors, then by
  correlation pairs (a send and its receive bound the offset).
- Correctness never reads trace time.

### Dump

- On demand: admin `TRACE DUMP`, and `snc_ctl trace`.
- On `SIGUSR2`.
- From the fatal paths: `std::abort` sites, failed CHECKs, and the
  sanitizer `__sanitizer_set_death_callback`.
- On a witness promotion: the witness asks the old and new primaries to
  dump, best effort.
- **Format:** the raw rings to `data_dir/trace-<pid>-<n>.bin`, plus a
  converter to the as-built text form, so `trace_merge.py` keeps working.

### Events (minimum set for failover, migration, recovery, retry)

| Category | Events |
| --- | --- |
| Shard loop | iteration over budget (drain/inject/io/poll/publish split); injected task begin/end with its kind; run-queue depth samples |
| Admin / control | request in (verb, bytes), handled (ms), reply; `STATUS` served (ms) |
| HA | witness poll result; suspicion start/end; proposal; decided state version; PROMOTE sent/received; promotion steps; term refused; withdraw (fence); DEGRADE/SYNCED asked/answered |
| Replication | batch shipped (first, last LSN); standby durable ack (LSN); primary `repl_hold_` blocked/unblocked; catch-up start/end; reseed |
| WAL | group commit submitted (LSN range, bytes); completed (latency); fsync error |
| Migration | every driver step; PREPARE/SEAL/EXPORT/STAGE/COMMIT/ACTIVATE begin/end per node with move ID, slot, objects, ms, status |
| Recovery | each barrier step of [CORE_CONTRACTS.md](CORE_CONTRACTS.md) section 12, begin/end, with counts |
| Retry/session | session hit (replayed / `E_REPLAYED_OLD`), session expiry (client, shard, last seq), refusal before session check (should never happen: invariant 18) |
| Follow-ups | outbox entry emitted, resent, settled, backlog over limit |

### What it is not

- Not an audit trail: it is overwritten and lossy.
- Not a metrics system. The counters stay in `ShardStats` and the
  `STATUS` line.
- Not an enterprise tracing protocol: there is no OpenTelemetry in the
  core. An exporter can live outside.

## 3. Next steps (not started)

1. Measure the disabled and enabled cost of the stopgap and of a
   prototype ring with `snc_bench`.
2. Replace the shard-loop and HA stopgap lines with ring events first.
   Those are the ones the 100K investigation needed.
3. Add the dump on witness promotion. A false failover then leaves both
   sides' rings behind without anyone re-running with `SNC_TRACE=1`.
