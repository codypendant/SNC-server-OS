# Sources for the direction comparison plan (collected 2026-11-22)

Primary-source facts behind `docs/DIRECTION_COMPARISON_PLAN.md`. Two research passes (Opus) read vendor docs repos and
source at pinned commits; nothing was executed. **No performance figures are recorded here, by rule.**

Labels: **EXT** = vendor docs (the file in the docs repo is the published page); **EXT-SRC** = product source;
**UNVERIFIED** = not confirmed. Paths are relative to the pinned repo.

## Pins

| System | Docs | Code |
|---|---|---|
| DBOS Transact | `dbos-inc/dbos-docs@182933f` (docs.dbos.dev) | `dbos-transact-py` 3.6.0 (MIT); Rust crate `dbos` 0.6.0 |
| Restate | `restatedev/docs-restate@1e57259` (docs.restate.dev) | server `restatedev/restate@2aaabe8` (1.8.2-dev; release v1.8.1); `sdk-rust@968ecd0` (0.13.0) |
| Temporal | `temporalio/documentation@5f59f68` (docs.temporal.io) | server v1.32.2; sdk-python 1.36.1 |
| LangGraph | `langchain-ai/docs@1d18652` (docs.langchain.com) | `langchain-ai/langgraph@c6335a4` (langgraph 1.2.24, checkpoint-postgres 3.2.1) |
| Garnet | `microsoft/garnet@ef3c20df65aa` website/docs | same |
| Orleans | `dotnet/docs` main (no SHA) | `dotnet/orleans@6a0e937f544c` |
| Dragonfly | | `dragonflydb/dragonfly@66de9e6885f7` |
| TigerBeetle | | `tigerbeetle@a2a3c7064519` |
| PostgreSQL | postgresql.org/docs/current (PG 19 pages) | |

## Track A systems

### DBOS Transact
- **Recovery (EXT).** "it resumes the workflow from the last completed step"; steps "are tried at least once but are
  never re-executed after they complete" (`docs/python/tutorials/workflow-tutorial.md`). Restart recovers `PENDING`
  workflows of the same application version (`docs/production/workflow-recovery.md`). Divergent step results raise
  `DBOSStepNondeterminismError`. Code changes: app version (hash of source), `DBOS.patch()`; auto-upgrade of in-flight
  workflows is Enterprise (`upgrading-workflows.md`).
- **Zombies (EXT).** Each checkpoint checks an ownership token in the same transaction: "steps get at-least-once
  guarantees and workflow outcomes are persisted exactly-once" (`docs/explanations/concurrent-executions.md`).
- **Entity state (EXT).** No keyed K/V primitive. Documented pattern: long-lived objects in your own tables,
  short-lived workflows, partitioned queues (`migrating-from-temporal.md`). One writer per key is opt-in via
  `partition_concurrency` = 1 (`queue-tutorial.md`).
- **Effects (EXT).** Step retries off by default (`step-tutorial.md`). Datasource transactions "commit exactly once",
  outcome recorded "atomically in the same database transaction" (`transaction-tutorial.md`).
- **History (EXT).** Inputs and every step output in system tables; list, fork, rewind (`workflow-management.md`).
  Retention unbounded unless a policy is set; retention policies are Enterprise (`transact-enterprise.md`).
- **Events (EXT).** send/recv ("guaranteed to be able to recv"), set/get event, streams, durable sleep, queues
  (`workflow-communication.md`).
- **Ops (EXT).** A library, "No Orchestration Server"; Postgres recommended, SQLite default without a URL
  (`configuration.md`). Durability comes from the database. Rust 0.6: "a pre-production release". MIT core;
  Conductor/Enterprise under a proprietary licence.

### Restate
- **Recovery (EXT).** Journal replay skips completed steps (`docs/foundations/key-concepts.mdx`); a journaled step
  "won't be re-executed"; epoch fencing rejects events from older attempts (`docs/references/architecture.mdx`).
  Divergence fails with RT0016 (`errors.mdx`). Immutable deployments; old ones must stay up until their invocations
  finish (`docs/services/versioning.mdx`).
- **Entity state (EXT).** Virtual Object: "Built-in K/V state, single-writer consistency"; "At most one handler with
  write access can run at a time per object key"; state "retained indefinitely" (`docs/foundations/services.mdx`).
- **Effects.** `ctx.run` results are journaled (EXT). An effect whose entry was not journaled before a crash re-runs,
  so external effects are at-least-once (INF from the architecture page; wording UNVERIFIED). Retry default 70
  attempts, then pause; idempotency keys kept 24 h (`docs/services/configuration.mdx`).
- **History (EXT).** Completed journals kept 24 h by default (`default-journal-retention`, configurable); SQL
  introspection (`sql-introspection.mdx`). Durable record = Virtual Object state.
- **Events (EXT).** Signals (stored before or after the wait), awakeables, workflow promises, durable timers, one-way
  and delayed sends, Kafka (`external-events.mdx`, `service-communication.mdx`).
- **Ops.** "a single binary that persists data to disk"; "Restate fsyncs committed data to disk before acknowledging
  network messages" (EXT, `docs/server/overview.mdx`). Log writer: `set_sync(!rocksdb_disable_wal_fsync)`, tail
  advanced only "after the batch is durably committed" (EXT-SRC `crates/log-server/src/rocksdb_logstore/writer.rs`);
  fsync on by default. Defaults 24 partitions, replication 1 (EXT-SRC `crates/types/src/config/common.rs`). Handlers
  run in a separate SDK service process. End-to-end reply-after-fsync for a finished invocation: UNVERIFIED. Server
  BSL 1.1 (no public hosted service), SDKs MIT. Rust SDK: "in active development, and might break across releases"
  (EXT-SRC README). Inactivity timeout 1 min, abort 10 min by default.

### Temporal
- **Recovery (EXT).** Workflow tasks replay on another worker; activities on a crashed worker "time out and retry"
  (`docs/encyclopedia/durable-execution.mdx`); a crash is detected only by Start-To-Close or Heartbeat timeouts
  (`detecting-activity-failures.mdx`). Deterministic workflow code; patching and Worker Versioning.
- **Entity state (EXT).** Entity-workflow pattern; one open run per Workflow Id; Signals/Updates/Queries
  (`entity-pattern-loyalty-points.mdx`, `workflowid-runid.mdx`).
- **Effects (EXT).** Activity retries unlimited by default; idempotent activities advised; MaxAttempts=1 for
  at-most-once; Update-ID dedup within one run (`retry-policies.mdx`, `activity-definition.mdx`,
  `handling-messages.mdx`). Atomic app-write plus history: none found (UNVERIFIED).
- **History (EXT).** Event History is authoritative and replayed; closed executions deleted after namespace retention;
  optional archival.
- **Limits (EXT).** 51,200 events or 50 MB per execution, then terminated; 2 MB payload; Continue-As-New
  (`dynamic-configuration.mdx`, `limits.mdx`).
- **Ops (EXT).** Server = four services plus Cassandra, PostgreSQL or MySQL, plus workers; `start-dev` on SQLite is
  "not production usage". MIT.

### LangGraph
- **Recovery (EXT).** Checkpoints at super-step boundaries; an interrupted or retried node "runs again from the start
  of its function" (`src/oss/langgraph/graph-api.mdx`). Durability modes `exit`, `async`, `sync`; only `sync` (Python)
  writes each checkpoint before the next step (`checkpointers.mdx`). Default `async` (EXT-SRC
  `libs/langgraph/langgraph/pregel/main.py`). OSS has no supervisor: re-invoke with the same `thread_id`.
- **Entity state.** Threads plus a namespaced Store (EXT, `stores.mdx`). No cross-process single writer per thread in
  OSS (EXT-SRC `libs/checkpoint-postgres`, inference UNVERIFIED); Agent Server has double-texting strategies (EXT).
- **Effects (EXT).** Node retry is "at-least-once ... does not make external side effects happen exactly once";
  idempotency keys need `sync` durability (`fault-tolerance.mdx`). Retries opt-in.
- **History (EXT).** Full checkpoint chain; replay "re-executes nodes"; fork (`use-time-travel.mdx`). Checkpoints are
  state snapshots, not a log of external calls.
- **Events.** `interrupt()` / `Command(resume=...)` (EXT). No durable timers or inter-thread messaging in OSS (none
  found, UNVERIFIED).
- **Ops.** OSS library + Postgres saver, MIT (EXT-SRC). Agent Server needs Postgres, Redis and a licence key (EXT).

## Track B baselines

- **Garnet.** Shared Tsavorite hash index, no core ownership (EXT README). `--aof --aof-commit-freq 0
  --aof-commit-wait`: "Wait for AOF to flush the commit before returning results to client" (EXT
  `website/docs/getting-started/configuration.md`; EXT-SRC `libs/server/Resp/RespServerSession.cs:1588`). **Caveat:**
  the default Linux `Native` device opens with `O_DIRECT` only, no `O_DSYNC`/`fsync` (EXT-SRC
  `libs/storage/Tsavorite/cc/src/device/file_linux.cc`); `--device-type FileStream` uses `FileOptions.WriteThrough`
  (EXT-SRC `ManagedLocalStorageDevice.cs`; mapping to `O_SYNC` UNVERIFIED). Server-side C#
  `CustomTransactionProcedure` (Prepare declares and locks keys, Main runs under 2PL, AOF markers for atomic recovery),
  invoked with `RUNTXP` (EXT `website/docs/extensions/transactions.md`). Whether readers can see a write not yet
  committed to the AOF: UNVERIFIED (likely yes). MIT.
- **PostgreSQL.** With no standby, `synchronous_commit` other than `off` waits for local WAL flush; `commit_delay`,
  `commit_siblings` for group commit (EXT runtime-config-wal). A deposit is one `UPDATE ... RETURNING`; a transfer is
  one call to a PL/pgSQL function. PostgreSQL License.
- **Restate** (scheduling baseline): see above. One writer per key, durable one-way and delayed sends, per-key order,
  partitions with one leader each, log group commit with fsync.
- **Excluded, with reasons.**
  - Dragonfly: per-op AOF is a "design proposal. Nothing described here is implemented yet" (EXT-SRC `docs/aof.md`);
    snapshots only. DSAL licence. Cannot meet equal durability.
  - VoltDB/H-Store (closest prior art: single-threaded partitions running stored procedures): synchronous command
    logging is commercial-only; the AGPL edition has snapshots only (EXT docs.voltactivedata.com CmdLogConfig;
    DBOS-project/voltdb README); upstream repo no longer public (observed).
  - Orleans: inter-grain messages at-most-once and not durable (EXT `messaging-delivery-guarantees.md`); the
    prerelease `Orleans.Durable` package has Azure, S3 and Redis providers only (EXT-SRC).
  - TigerBeetle: single node supported, fsync-safe, but a fixed accounts/transfers schema with no general handlers
    (EXT-SRC `docs/start.md`, `docs/internals/ARCHITECTURE.md`). Apache-2.0. Not a general object baseline.
  - Redpanda, Akka Persistence, ScyllaDB: a log, a BSL actor toolkit with plugin-dependent durability, and an LSM
    database without object handlers (ScyllaDB is already in `docs/CONVENTIONAL.md`).
