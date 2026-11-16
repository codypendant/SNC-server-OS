# SNC-AFR-001 Phase 1: architecture inventory and immediately critical findings

- **Assignment:** SNC-AFR-001, "Architecture Foundation Review" (cody, 2026-11-15), as amended by cody's five
  amendments and Codex's review of Handoff 1 (`docs/handoff/AFR001_Handoff1_Codex_Review.md`).
- **Phase:** 1 only (reconnaissance and inventory). This document is **Deliverable 1** plus the
  **immediately critical findings** the assignment asks for before broad recommendations.
- **Baseline:** `main` at `24006ee` (PR #135 merged). Clean RelWithDebInfo build of that commit succeeded.
- **Nature:** read-only. No product code, test, format or benchmark file is changed by this PR. No experiment
  in this PR changed repository state; experiments ran against the scratch build in scratch directories.
- **Authority:** nothing here is accepted. Classifications, severities and proposed slices are
  **proposals for cody and Codex**. The Guarantees Specification, the rulings register (AFR-R), broad
  recommendations and the roadmap are **not started** and wait for explicit authorization of Phase 2.

## 0. How this was produced, and how far to trust it

| Role | Who | Output |
|---|---|---|
| Bounded reviewers (4) | independent agents, one scope each, file:line evidence required | `docs/afr-001-working-notes/A_core.md`, `B_distributed.md`, `C_interfaces.md`, `D_verification_docs.md` |
| Author | Claude (this document) | re-read the source for every critical finding (marked **[V]**), synthesis |
| Independent QA (2) | fresh agents that were **not** shown the reviewer notes or this document; given the claims as neutral questions | `docs/afr-001-working-notes/qa/QA1_REPORT.md` (persistence, sessions, ordering), `QA2_REPORT.md` (network, HA) |

Labels used throughout: **VERIFIED** (code read at the cited line, or an experiment run), **DOCUMENTED** (a doc
says it; not checked), **INFERRED** (reasoned from verified code), **UNKNOWN**. Passing tests are evidence for
the scenario tested, never for a wider guarantee. The reviewers, QA and author are the same model family;
Codex's review through cody is the external check (amendment 5).

**Identifiers.** New identifiers carry the `AFR-` prefix (Codex, Handoff 1 review): `AFR-F-nn` findings,
`AFR-G-nn` guarantees (Phase 2), `AFR-R-nn` rulings (Phase 2). Older identifiers are cited with their source
document, because several are reused with different meanings (section 8.3): for example "D2" is
*silent WAL loss* in `ARCHITECTURE_EVIDENCE_AUDIT_20261113.md` and *default shard count* in
`CPU_PLACEMENT_R1.md`. In this document **audit-D1..D4** means the 2026-11-13 audit's confirmed defects.

---

## 1. Repository at a glance (VERIFIED)

| Area | Size | Notes |
|---|---|---|
| `src/`, `include/` | about 18.8 k lines C++20 | one static library `snc_core` (CMakeLists.txt:18-45) |
| `tools/` | 6.1 k lines | `snc_server.cpp` 1,637; `snc_witness.cpp` 885; `snc_ctl.cpp` 200; header-only `admin_loop.h`, `ha.h`, `paxos.h`, `migrate.h`, `control.h` |
| `tests/` | 30.8 k lines | 61 registered ctest tests: 28 C++ (one inline foreach, CMakeLists.txt:61-65), 8 variants, 25 Python; 10 labelled `slow` |
| `bench/` | 13.9 k lines of code; about 3.9 k result files | benchmarks, campaign drivers, lab harness, frozen configs |
| `docs/` | about 29 k lines in 66 top-level docs, plus `adr/`, `handoff/` (about 90 files), `research/` | one ADR (SNC-ADR-RUST-001) |
| Dependencies | mimalloc, xxHash (system library), liburing; optional Seastar | CMakeLists.txt:13-15, 46-58 |

The heaviest concentration is `src/core/shard.cpp` (4,347 lines, one class) and `src/core/engine.cpp` (2,075
lines). Their internal responsibility maps are in `A_core.md` section 1.

---

## 2. Subsystem inventory

### 2.1 Single-node core (detail: `A_core.md` sections 1-7)

| Subsystem | Location | Responsibility | Depends on |
|---|---|---|---|
| Types and status | `include/snc/types.h` | ints, 19 status codes, limits (payload 1 MiB, args/result 64 KiB, 256 shards), little-endian static asserts | none |
| Identity | `include/snc/object_id.h`, `src/core/object_id.cpp` | 128-bit ObjectID: 8-bit format, 56-bit lease, 64-bit sequence; per-shard Minter; XXH3-64 hash | system xxHash |
| Intra-node placement | `include/snc/slot_map.h` | 4096 slots, slot = top 12 hash bits, slot to shard `owner[]`, epoch | identity |
| Object layout | `include/snc/object.h` | 48-byte header (id, version, last_lsn, type, length, capacity, flags, schema_ver) | identity |
| Registry | `include/snc/registry.h`, `src/core/registry.cpp` | compiled-in Type, Op and Index descriptors; ids 0-255 reserved (Session = 1, SlotState = 2) | none |
| Local map, allocator, pages | `local_map.h`, `allocator.*`, `pages.*` | per-shard open-addressing map, mimalloc heap per shard, optional huge-page arena | mimalloc, mmap |
| Runtime interface (SRI) | `include/snc/sri.h`, `msg.h` | send/claim/commit/publish, async write and fdatasync, wake, time | backend chosen at link time |
| Native runtime | `src/native/runtime.cpp`, `ring.h` | one pinned thread per shard, SPSC ring mesh with overflow deques, io_uring, io_yield | liburing |
| Seastar runtime | `src/seastar/runtime.cc` | same interface, off by default, benchmark only | Seastar |
| Shard | `src/core/shard.{h,cpp}` | entry API, owner execution, WAL logging, durability waiters, sessions, index, cross-shard dispatch and batching, recovery install, replication glue, checkpoint walk, follow-ups, slot moves | almost everything |
| Engine | `include/snc/snc.h:350-977`, `src/core/engine.cpp` | lifecycle, config validation, recovery orchestration, checkpoint commit, promotion, placement adoption, slot-move control | Shard, storage, runtime, seed, replication |
| Storage | `wal_format.h`, `wal_format.cpp`, `storage.h`, `storage.cpp` | CRC32C, WAL records and segments, group commit, superblock A/B, manifest, checkpoint parts | SRI |
| Sessions | `src/core/session.*` | client dedup objects (client_id, last_seq, last result, wall-clock touch time) | registry |
| Checkpoint control | `ckpt_pacer.h`, `sync_tokens.h`, admission in `shard.cpp:282-354` | S1 admission, S2 pacing, S3 DSYNC window, S4 sync tokens | shard |
| Application type | `include/snc/types/account.h`, `src/types/account.cpp` | the only type: Account, with transfer outbox/inbox follow-ups | public API |

### 2.2 Distributed layer (detail: `B_distributed.md` sections 1-2, 6)

| Component | Location | Responsibility |
|---|---|---|
| Replication sender/receiver | `src/core/replication.{h,cpp}` | primary to standby WAL shipping per shard, term handshake, acks, catch-up from disk, standby read-lease echo, divergence detection |
| HA loop | `tools/snc_server.cpp:1319-1529` | NOTREADY/READY/DEGRADE to witnesses, withdraw on `NO term`, SIGUSR1 promotion, `--rejoin`, `--adopt-role` |
| Witness and Paxos | `tools/snc_witness.cpp`, `tools/paxos.h` | per-group HA state machine, placement map, mint leases; single-decree Paxos per version; persisted acceptor state |
| Placement and cluster map | `include/snc/placement.h`, `src/core/placement.cpp`, `cluster.*` | slot to replication group, epoch, at most one move; text-encoded |
| Placement adoption | `engine.cpp:1609-1653` | adopt strictly newer epoch, two passes over all shards (invariant 17) |
| Migration | `tools/migrate.h`, `shard.cpp:3551-4316` | seal, staged copy, commit gate, activate, unseal, start-barrier reseal; driven by stateless `snc_ctl move` |
| Node links | `src/core/node_link.*`, `links_generation.*` | cross-group follow-ups, wire v2 with a version field, incarnation fencing, member steering |
| Seed | `src/core/seed.*` | checkpoint transfer to a new standby |
| Cluster client | `src/net/cluster_client.cpp` | one `Client` per group, routed by `group_of`, refresh on `E_MOVED` |

**Processes.** `snc_server` (primary or standby role), `snc_witness` (3 or 5 for Paxos), and `snc_ctl` as an
operator tool. There is no controller process: the slot-move state machine runs inside `snc_ctl` (VERIFIED,
`migrate.h:420-1009`).

**Membership is static** (VERIFIED): witness `--peers`, HA pairs (`--group G=P:ADMIN,S:ADMIN`) and placement
(`snc_ctl init`) are configured separately, never change at runtime, and are not cross-checked. The witness
models a group as a **pair** (`snc_witness.cpp:531-534`) while placement allows N members
(`placement.h:44-48`).

### 2.3 Where architectural responsibilities are mixed (VERIFIED, for Phase 2)

- `Shard` holds execution, durability, sessions, dispatch, checkpointing, replication glue, follow-ups and
  migration in one class (`A_core.md` 1, table of `shard.cpp` line ranges).
- Management semantics live in clients: `snc_ctl` runs the slot-move protocol itself; HEALTH/STATUS replies are
  parsed by four separate hand-written parsers (`snc_server` rejoin and adopt-role, `snc_witness`, `lab.py`)
  (`C_interfaces.md` 6).
- HA membership (witness flags) and data placement (Paxos map) are two sources of truth for the same groups.

---

## 3. Interfaces

### 3.1 Application and internal APIs (detail: `A_core.md` 2)

- **Application API** (`include/snc/snc.h`): `execute(target, op, args, opts, done)`, `create`, `create_near`,
  `remove`, `index_lookup`, `ExecOptions` (expected_version, DURABLE or APPLIED, allow_undurable, client_id and
  seq), `OpCtx` (`reserve`, `resize`, `reject`, `emit`, `local`, `set_result`), `OpDescriptor` and
  `TypeDescriptor` (`registry.h`). Not stated anywhere: ordering between calls, request binding of dedup
  entries, schema migration.
- **SRI** (`sri.h`): FIFO per (source, destination) holds in the native backend in practice
  (`runtime.cpp:126-193`) but is not stated as a contract.
- **Storage** (`storage.h:91-95`): `scan_stream` documents "stops at the first invalid record (torn tail)"; it
  does not distinguish a torn tail from corruption of acknowledged data (AFR-F-01).

### 3.2 Executables and CLI (detail: `C_interfaces.md` 1-2, 6)

| Executable | Class | Grammar and help | Exit codes |
|---|---|---|---|
| `snc_server` | production | hand-rolled `--flag value`; more than 50 flags; no `--help` ("unknown flag --help", exit 2); usage only as a source comment, which omits 6 parsed flags | 0, 1 refusal, 2 usage; 134 on uncaught exceptions (bad `--cluster`, hostname in `--host`, port in use, bad `SNC_*` env value) |
| `snc_ctl` | production (operator) | `get`, `init`, `move`, `move --abort`, `placement backfill`; the only tool with real usage text (exit 2) | 0, 1, 2; `move abc 1` aborts (uncaught `stoul`) |
| `snc_witness` | production | flags; `--help` gives "missing value for --help" | 0, 2, 3 (state save failure) |
| `snc_load`, `net_bench`, `snc_bench`, `baseline_*`, `stage_stall`, `ckpt_*` | bench | per-tool grammar; `snc_load` and `stage_stall` silently ignore unknown flags | inconsistent (`snc_load` 1 for everything, `net_bench` 3 for an invalid retry claim) |

Nine environment variables change server behaviour (`SNC_CKPT_IO`, `SNC_CKPT_SYNC_TOKENS`,
`SNC_FOLLOWUP_INTAKE`, `SNC_FOLLOWUP_WINDOW`, `SNC_EXCHANGE_BUDGET`, `SNC_LINK_HOLD_MAX`,
`SNC_XNODE_ACCOUNTING`, `SNC_TRACE`, `SNC_HA_KEEP`). No output is machine-readable by contract: admin replies
are `key=value` or positional text, errors are `NO <English text>`, and callers match the English.

### 3.3 Network listeners (VERIFIED; `C_interfaces.md` 3, QA2)

| Listener | Opened at | Bind address | Configurable | Protocol | Auth |
|---|---|---|---|---|---|
| Client data, one port per shard | `src/net/conn_set.cpp` | `127.0.0.1`; `0.0.0.0` under `--control` without `--host` | `--host` | binary wire "v0", no version field | none |
| Admin | `tools/ha.h` `listen_on`, `snc_server.cpp` | **all interfaces** | no | text line protocol | none |
| Witness | `snc_witness.cpp` via `ha.h` | **all interfaces** | no | text line protocol + Paxos verbs | none |
| Replication (standby) | `replication.cpp` | `0.0.0.0` default | `--standby-host` | binary `SNCREPL1` | none |
| Seed | `engine.cpp` | **all interfaces** | no | binary `SNCSEED1`; an all-zero uuid means "any space" | none |
| Node link | `node_link.cpp` | **all interfaces** | no | binary wire v2 (the only internal protocol with a version) | none |

No listener has TLS. All outbound clients accept IPv4 literals only.

### 3.4 Protocols and error model (detail: `C_interfaces.md` 4-5)

- **Client wire:** 48-byte request and response headers, little-endian, kinds EXEC, CREATE, DELETE, TOPOLOGY,
  CLUSTER, INDEX, RESP. **No version, magic or handshake**; reserved fields are not checked. An unknown kind is
  answered `E_BAD_ARGS`; a bad length closes the connection silently.
- **Admin protocol:** one line in, one line out; opt-in `KEEP 1` mode (K0). Verbs: HEALTH, GAUGES, STATUS,
  RESOURCES, AUDIT [PATH], PROMOTE, MOVE ROLE/INFO/HAVE/STATES/REFRESH/SEAL/UNSEAL/EXPORT/STAGE/ACTIVATE, DEBUG
  (only with `--debug-admin`). State-changing verbs are reachable by any peer (section 9, AFR-F-02).
- **Witness protocol:** DEGRADE, NOTREADY, READY, SYNCED (refused), PLACEMENT GET/SYNC/INIT/CAS, LEASES TAKE,
  and the raw Paxos verbs READ, PREPARE, ACCEPT, COMMIT on the same port. Compatibility is by verb: an older
  peer answers `NO unknown request`.
- **Replication, seed:** versioned only by magic string. **Node links:** explicit version 2; mismatches refused
  and counted.
- **Errors:** 19 `Status` codes (`types.h`) on the data path; free English text everywhere else.

---

## 4. Persistent formats inventory (VERIFIED; `A_core.md` 5)

| Format | Version | Integrity | Reader behaviour on damage or unknown version |
|---|---|---|---|
| Superblock A/B (80 B in 4 KiB) | magic + `format` = 1 | CRC32C | a bad copy is **skipped silently**; the valid copy with the higher generation wins; both bad with data present: refuse to start |
| WAL segment header | magic `SNCWAL01` + format | CRC32C | bad header fatal, except an all-zero newest segment |
| WAL record (56-B header + payload) | **none per record**; `rec_type` in {1, 2, 3, 5} | CRC32C | an invalid or unknown record ends the newest segment silently (torn-tail path); earlier segments: abort on the resulting LSN gap |
| Atomic group | `kRecGroupNext` flag | per record | incomplete group at a segment end dropped |
| Checkpoint part | magic `SNCCKP01` | header and record CRC | any problem fatal; object count checked against the manifest |
| Manifest | magic `SNCMANI1` + u16 format; lists (type_id, schema_ver, name) of every registered type | trailing CRC32C | bad: fatal if the superblock names it; startup refuses a type whose schema_ver or name changed (no migration path) |
| Session, slot-state payloads | "V1" by name | via record | length check only |
| Role file, links_generation | none | none | malformed role forces the start barrier |
| Placement map (in Paxos) | text v1/v2 | none | witness treats the body as opaque and accepts any CAS body |

One `kFormatVersion = 1` covers every file (`wal_format.h:10`); any other value is "invalid", with no upgrade
path. The WAL is **state-based**: each update record carries the full object image and no op id, so op tables
can change across restarts but every mutation writes the whole object. Slot placement depends on XXH3-64 from
the **system** xxHash library with **no pinned test vector** (`tests/test_object_id.cpp:52-53` compares the
hash with itself).

---

## 5. Execution, durability and consistency as built (summary; detail in `A_core.md` 4, 6 and `B_distributed.md` 3, 5)

- **Acknowledgement:** DURABLE completes after the local WAL flush (`RWF_DSYNC`, 64 KiB or 50 µs group commit,
  one flush in flight) and, under healthy sync replication, after the standby's ack (`shard.h:555-559`).
  APPLIED returns at once. Every WAL I/O error is fail-stop (abort).
- **Reads:** wait for the object's last LSN to be durable, then answer. There is no primary-side read lease.
  Standby reads need a lease from a sync, non-degraded primary and assume clock-rate drift under 10%.
- **Ordering:** per-origin FIFO holds for same-shard calls and single-ring traffic; **not** across batched and
  non-batched requests to the same shard (audit-D1, still present). No ordering contract exists in any document.
- **Multi-object:** one same-shard peer object per op, logged as one atomic WAL group; cross-shard work is
  follow-ups (at-least-once re-emission from durable state, idempotence per type).
- **Dedup:** sessions keyed by (client_id, seq) per shard; see AFR-F-03.
- **Write safety across failover:** holds in **sync mode with witnesses** only; async mode and sync mode without
  witnesses can run two acknowledging primaries (AFR-F-04).
- **Two groups serving one slot:** designed out (durable seal, commit gate, exact arrival, start-barrier
  reseal), except where the source group itself is split-brained (the commit gate sees only members that answer,
  `migrate.h:700-737`).

---

## 6. Verification estate (detail: `D_verification_docs.md` 1-4)

- **Tests:** 61 registered; PRs run the 51 fast ones, `main`/nightly the full 61, serially on hosted
  `ubuntu-24.04` (`.github/workflows/ci.yml`). `test_pilot` is green since PR #122. `test_xnode` is very likely
  still flaky (X16 recorded as a test race, X3/X26 as possible liveness defects, fixes F3/F4 not started per
  `STATUS.md:391`; the test file is unchanged since the diagnosis). No hosted full run of all 61 at `24006ee`
  is on record.
- **Fault injection present:** SIGKILL/SIGSTOP; a link fault proxy (test_xnode only); checkpoint write and
  fsync EIO injection; about 30 `Engine::debug_*` hooks; admin and witness `DEBUG` verbs; EMFILE; lab
  iptables/netem on real hosts; `SNC_TRACE`.
- **Absent (checked by grep):** WAL I/O-error injection; power-loss or media-corruption harness; model
  checking; a history or linearizability checker (only money conservation and single ownership); deterministic
  simulation; fuzzing of wire, WAL or parsers; sanitizer CI; coverage.
- **For Rust differential testing** (`D_verification_docs.md` 8): no in-repo specification, no reference model
  or recorded histories, no on-disk or wire golden corpus, and the Python suites depend on C++ log text and
  C++-internal debug hooks.

## 7. Observability and operations (detail: `D_verification_docs.md` 5)

State is exposed only through text admin verbs (HEALTH, GAUGES, STATUS, RESOURCES, AUDIT); logging is free text
on stderr (about 149 sites); there is no metrics export, structured log, config file, install target, service
unit, container, or backup and restore tooling. `torn_tails` (silent WAL truncation) is never shown to an
operator (VERIFIED: it appears only in `engine.cpp` and tests).

---

## 8. Governing documents

### 8.1 Authority map (detail: `D_verification_docs.md` 6)

| Kind | Documents | Status |
|---|---|---|
| Accepted decision | `docs/adr/SNC-ADR-RUST-001.md` | Accepted 2026-11-15; makes "the specification" authoritative; authorizes no Rust work until stabilization and a readiness review |
| Coordination contract | `AGENTS.md` (invariants 1-18, change discipline, handoff log) | in force |
| Contract drafts | `docs/CORE_CONTRACTS.md` (DRAFT), `docs/FAILURE_MODEL.md` (guarantees G1-G6 PROPOSED), `docs/INTERACTION_MATRIX.md`, `docs/EVIDENCE.md` | none accepted as a whole |
| Scope freeze | `docs/EXPERIMENTAL_VALIDATION.md`, `docs/STATUS.md` | in force; the GO/REFINE/NARROW/STOP decision is open |
| Approved designs | for example `CHECKPOINT_BACKPRESSURE_DESIGN.md`, `CROSS_NODE_EXCHANGE_DESIGN.md`, `CONTROL_CONNECTIONS_DESIGN.md`, `MIGRATION_FAIL_CLOSED.md` | approvals recorded in `docs/handoff/` and `STATUS.md`; many doc headers lag their approvals |
| Authorize nothing | `docs/FUTURE_ARCHITECTURE.md`, `docs/STATE_NATIVE_COMPUTING_LINEAGE.md`, `docs/handoff/AGENT_SNC_ARCHITECTURE_V0_1.md` | each says so |
| Prior audit | `docs/ARCHITECTURE_EVIDENCE_AUDIT_20261113.md` (PR #121, approved) | its confirmed defects audit-D1 to D4 are still open (section 9) |

### 8.2 The external v0.1 specification

`README.md:9` and `STATUS.md:4` link the "SNC v0.1 Object & Execution Specification" as the design; no copy is
in the repository (VERIFIED by grep), nor is `SNC_Project_Handoff_v0.1.md`. I located the original: it is a
Claude Docs document (id `8a25e96e-09ae-43ef-9e42-d6763e1eebd9`, one tab, **revision 22**, dated 2026-10-12).
A Markdown export of revision 22 is 49,639 bytes, SHA-256
`c0a8ab56ea1bf3dc21070672893b53a00577583ba599ee180b5c860373f7e801`. The export drops one embedded diagram
(replaced by a placeholder) and renders a date and an author mention as text. Revision 22 is the current
revision; whether it differs from the text the prototype was built against is **UNKNOWN**. It is imported as
`docs/spec/SNC_v0.1_Object_Execution_Specification.md`, verbatim except that the author mention is replaced by
"[byline redacted]" (cody's decision, 2026-11-16, because this repository is public); a provenance header records
the source, revision, both checksums and the export limits. It is classified as **historical design input, not
authoritative** (Codex, Handoff 1 review, item 2).

### 8.3 Identifier collisions (VERIFIED; full list `D_verification_docs.md` 6.5)

D1-D5, F1-F10, S1, S6, P1, X1, G2, C1-C5 and several single-letter series each mean different things in
different documents. Examples: "D2" = silent WAL loss (audit) and default shard count (`CPU_PLACEMENT_R1.md`);
"F9" = witness state loss (`PAXOS_BALLOTS.md`) and a follow-up intake test (`FOLLOWUP_INTAKE_DESIGN.md`); "S1" =
destination staging (`test_s1.py`) and resource admission (`S1_ADMISSION.md`). Phase 2 should cite every older
identifier with its source document.

### 8.4 Stale or contradictory statements (VERIFIED, minor)

README layout table versus the tree; STATUS "All 16 tests pass" and 40/49 counts; `CI.md` slow list omits
`test_health_baseline` and says no TSAN run is on record although TSan logs exist in `bench/results`;
`include/snc/wire.h:5` and README say routing is "never a correctness requirement", which audit-D1 contradicts;
code comments (128-byte messages versus 64; "reserved" INDEX kinds that are used; two object flags never used).

---

## 9. Status of the recorded gaps (re-checked against `24006ee`)

These are **status checks only**. The proposed DEFECT / ACCEPTED-BEHAVIOR / UNRESOLVED rulings (AFR-R) are
Phase 2 work.

| Recorded gap (source) | Status at `24006ee` | Evidence |
|---|---|---|
| audit-D1 cross-shard overtaking (`ARCHITECTURE_EVIDENCE_AUDIT_20261113.md` 0.1) | still present | `shard.cpp:1473-1531`, batches flushed only when full or at loop end (`engine.cpp:146`); QA1 P8 |
| audit-D2 silent loss on newest-segment corruption | still present, **wider than recorded** (AFR-F-01) | `storage.cpp:388-411`, `:522-527`; reproduced by reviewer A and QA1 |
| audit-D3 `client_id` reuse returns false `OK, replayed` | still present; root cause is that dedup entries are not bound to the request (AFR-F-03) | `shard.cpp:1192-1199`; reproduced in process by reviewer A and QA1 |
| audit-D4 no authentication; listeners on all interfaces | still present, **wider than recorded** (AFR-F-02) | `ha.h`, `paxos.h:142-167`, `replication.cpp:697-704`, `snc_server.cpp:205-226`; QA2 |
| Cut-off primary serves stale reads (`CORE_CONTRACTS.md:182`) | still present; cited lines moved to `shard.cpp:612-615`, `:461-466`; no primary-side lease | reviewer B 4a; QA2 N7 |
| R08 async standby promoted without acknowledged writes (`EVIDENCE.md` 12) | still present; witness state has no sync/async mode, async readiness is "link up", PROMOTE unguarded | `shard.cpp:2761-2764`, `snc_witness.cpp:112-138`, `snc_server.cpp:1278-1282` |
| Standby receive unbounded, stops at 1 GiB (memory) | still present, **wider**: receive buffer doubles with no cap and trusts a u32 frame length from the wire | `replication.cpp:712-722`, `:741`; `storage.cpp:623-628` |
| Paxos F8 (`PAXOS_BALLOTS.md`) | unchanged; liveness only | `paxos.h:464-475`, `:174` |
| Paxos F9 (witness acceptor state loss) | guarded against a deleted state file; **not** against restoring an older one | `snc_witness.cpp:649-674`; `PAXOS_BALLOTS.md:21` |
| F6 degraded mode has no back-pressure (`BASELINE_READINESS.md`) | still present | `replication.cpp:221-258`, `:313-335` |
| F7 readiness latency, one shared proposal queue (`SYNC_READINESS.md` 7.5) | still present; aggravated by a withdrawn primary sending NOTREADY every 50 ms forever | `paxos.h:326-328`; QA2 N8 |
| O1 resumed old primary reports `role=p` at its old term (`HEALTH_BASELINE.md`) | **narrowed**: in an unclustered sync pair it reported `role=p` for about 0.1 s, then `role=f`; from code, `role=p` lasts indefinitely when the new primary's in-memory fence listener is unreachable (partition, restart) and, in async mode, forever. Clustered variant not re-run | reviewer B 4e (experiment) |
| No-witness degrade in sync mode | still present (default `replica_timeout_ms` 2000) | `snc_server.cpp:512`, `engine.cpp:872`, `snc.h:387` |
| Sessions do not move with slots; cross-group resend routes | still present | `shard.cpp:3578`, `cluster_client.cpp:85-89` |
| Session TTL evaluated only in checkpoints (audit 0.2) | still present, now `engine.cpp:1432-1437`; wall clock on both sides | AFR-F-03 |
| test_xnode X16 flake (`CI_FAILURE_DIAGNOSIS_20261114.md`) | **not re-verified** in Phase 1 (a 55-run reproduction is a Phase 2 experiment); fixes F3/F4 not started | `D_verification_docs.md` 1.3 |

---

## 10. Immediately critical findings

Each finding was found by a bounded reviewer, re-read in source by the author (**[V]**), and put to an
independent QA agent as a neutral claim. QA verdicts are quoted from `qa/QA1_REPORT.md` and `qa/QA2_REPORT.md`.
Severity and reversibility are **proposed**.

### AFR-F-01. Acknowledged durable writes are lost silently when the newest WAL segment is damaged

- **Subsystem and files:** persistence and recovery; `src/core/storage.cpp:370-435` (`scan_stream`),
  `:515-527` (`WalStream` constructor), `src/core/wal_format.cpp:29-59` (`check_record`),
  `src/core/engine.cpp:1100-1110`, `tools/snc_server.cpp:818-824`. Older ID: audit-D2.
- **Current behaviour (VERIFIED, reproduced independently by reviewer A and QA 1):**
  1. A byte flipped mid-record in the newest segment: the next start succeeds (exit 0) with every record from the
     damaged one onward missing (QA 1: 49 of 100 acknowledged creates survive; reviewer A: 117 of 200).
  2. The damaged record is the first of the newest segment: recovery **unlinks that segment and recreates a file
     of the same name** (`storage.cpp:526`, whose comment assumes the file holds no acknowledged records). Every
     acknowledged record in it is lost (reviewer A: 4,032 of 8,000 survive; QA 1: 100 of 150).
  3. A damaged 4 KiB block that reads as all zeros is treated as the unwritten rest of the segment
     (`storage.cpp:393-395`): truncation with `torn_tails = 0` (QA 1).
  4. A record with a valid CRC but an unknown `rec_type` is rejected before the CRC check and takes the same
     torn-tail path (QA 1, `rec_type = 9`). Records carry no format version, so a WAL written by a newer binary
     is truncated silently by an older one.
  5. New writes **reuse the lost records' LSNs** with different content (QA 1, reviewer A).
  6. The only trace, `torn_tails`, is in `RecoveryReport` only. `snc_server` prints nothing, and HEALTH and GAUGES
     carry no such key. The trace disappears after the next checkpoint or restart (QA 1: `segments_deleted=1`,
     then `torn_tails=0`).
  7. Damage in an older segment aborts (SIGABRT, exit 134 through `snc_server`) with a misleading "WAL gap" message
     that names the next file; damage in a segment wholly covered by a checkpoint is never read (QA 1, P4).
- **Against the full acknowledgement and recovery contract (Codex requirement):** a DURABLE acknowledgement is
  issued after the local `RWF_DSYNC` flush and, under healthy sync replication, the standby's ack
  (`shard.h:555-559`). Recovery cannot honour that promise under damage because **nothing durable records how far
  acknowledgements went**: the scan's only evidence of the end of the log is the first invalid record, so an
  unacknowledged torn tail and the loss of acknowledged records are indistinguishable by design
  (`storage.cpp:388-411`). Two consequences follow, both **INFERRED** from verified code and not yet run:
  - With sync replication the standby still holds the lost records. The restarted primary's sender sees a standby
    that is ahead, prints "a primary crash lost them after shipping: reseed it" and stops shipping
    (`replication.cpp:397-403`, `:412-414`). The message describes the lost records as unacknowledged, and
    following it would overwrite the only intact copy of acknowledged data with the damaged primary's state.
  - A cross-shard follow-up applied on its destination after the source record became durable survives on the
    destination when the source record is lost, so an Account transfer can keep the credit without the debit
    (reviewer A, N2).
- **Intended contract:** documented intent is that acknowledged durable writes survive restart (`docs/STATUS.md`,
  `docs/CORE_CONTRACTS.md` section 4, DRAFT). cody's PR #121 review already ruled the outcome unacceptable and set
  the preferred policy: "fail-stop on detected corruption that could conceal acknowledged data. Do not silently
  truncate and continue." (`docs/handoff/PR121_Audit_Final_Approval_PR122_M1-0.md`).
- **Classification (proposed):** REDESIGN of the recovery end-of-log rule; KEEP of the fail-stop policy on older
  segments.
- **Severity:** critical (permanent, silent loss of acknowledged data; evidence erased).
- **Reversibility:** the C++ policy fix is local and reversible. The Rust-side decision is expensive to reverse:
  how the end of the acknowledged log is made durable (a durable watermark, per-flush trailers, or both) and how
  record kinds are versioned are on-disk format decisions.
- **Dependencies and downstream:** replication catch-up and divergence handling, checkpoint reclaim (which deletes
  the evidence), follow-up conservation, operator tooling. The Rust WAL format must not be designed until this is
  ruled.
- **Corrective action proposed (not started):** the authorized audit-D2 slice, widened to cover all seven
  behaviours above: never unlink or overwrite a segment that held a valid record; distinguish a torn tail from
  damage before the acknowledged end (for example scan past the damage for a later valid record, as the audit's
  M1-c proposed, and fail closed with a named error); treat an unknown record type as corruption; never reuse an
  LSN; surface `torn_tails` in the log, HEALTH and GAUGES.
- **Required verification:** a fault-injection test per behaviour (fails on `main`, passes after); experiments
  E-01 (sync standby after primary WAL damage) and E-02 (transfer conservation after source-record loss).

### AFR-F-02. Every management and internal port is unauthenticated, and several mutate state or the file system

- **Subsystem and files:** management and control plane; `tools/ha.h:97-110` (`listen_on`, `INADDR_ANY`),
  `tools/snc_server.cpp:1082`, `:205-226` (`audit`), `:1277-1282`; `tools/paxos.h:142-167`;
  `src/core/replication.cpp:697-745`; `src/core/engine.cpp:1247` (seed), `src/core/node_link.cpp:203`;
  `tools/snc_witness.cpp:678`. Older ID: audit-D4.
- **Current behaviour (VERIFIED; experiments by QA 2 on loopback unless marked "code"):**
  1. The admin port binds all interfaces whatever `--host` says (`/proc/net/tcp`: data port `127.0.0.1`, admin
     `0.0.0.0`).
  2. `AUDIT <PATH>` opens PATH with mode `"w"` and writes every served account's id and balance to it: any peer can
     create or truncate any file the server's user can write (QA 2 truncated a 29-byte file to 0). INFERRED, not
     exercised: that includes the server's own data directory, which would turn into AFR-F-01.
  3. `PROMOTE` has no term or identity guard; QA 2 promoted a standby with it and the old primary fenced itself.
  4. The witness accepts an unauthenticated `COMMIT` (and a PREPARE's base value) with a higher version, for any
     machine name, and saves it as chosen. QA 2 forged `placement`, `ha/0` and an invented machine; each became
     chosen and persisted. Paxos messages carry no cluster identity. From code: other witnesses adopt the highest
     version a majority reports, so one forged witness may be enough to spread a value.
  5. The standby's replication receiver drops its live primary as soon as any new connection is accepted, before a
     Hello is read (code). After a matching Hello, a peer can send a lease frame whose wire timestamp extends the
     standby's read lease (`replication.cpp:742-745`, code), so a standby with `--standby-reads` serves stale
     reads.
  6. The only identity checks that exist (space uuid, term) use values that the unauthenticated `HEALTH` verb
     publishes. The seed port serves a full checkpoint to an all-zero uuid ("any space") (code). `MOVE HAVE` and
     `MOVE EXPORT` return object payloads under `--control` (code).
- **Intended contract:** none exists; security was excluded by the scope freeze (`docs/STATUS.md`). cody's PR #121
  review asked that restricting administrative and internal listeners be prioritized and "not deferred
  unnecessarily".
- **Classification (proposed):** REDESIGN (trust model) for production; immediate C++ mitigation needed because the
  exposure is live on any LAN where `snc_server` or `snc_witness` runs (the R640 lab is one).
- **Severity:** critical on any network that is not fully trusted; at minimum a misconfiguration hazard (a
  misdirected `--peers` entry can rewrite HA or placement state).
- **Reversibility:** the interim mitigation (bind to a configured address, default loopback; remove or confine the
  AUDIT path argument) is cheap and reversible. Authentication and cluster identity in every protocol are wire
  decisions and expensive to reverse in Rust.
- **Corrective action proposed (not started):** the audit-D4 slice, scheduled now, widened to the six behaviours
  above; separately, a decision record on the production trust model before any Rust protocol design.
- **Required verification:** a test per listener that it is not reachable on a non-loopback address without an
  explicit flag; a test that AUDIT cannot write outside an allowed location; forged-COMMIT and forged-Hello tests
  that must be refused once authentication exists.

### AFR-F-03. Duplicate suppression is not bound to the request and expires on the wall clock

- **Subsystem and files:** execution and retry; `src/core/shard.cpp:1187-1261`, `:404`, `:700-706`;
  `src/core/engine.cpp:1432-1437`; `src/core/session.h:17-26`. Older IDs: audit-D3, audit 0.2 (TTL), R11
  (`docs/CORE_CONTRACTS.md` section 10).
- **Current behaviour (VERIFIED; experiments by reviewer A and QA 1):**
  1. `replay_if_seen` compares only the sequence number. Reusing (client_id, seq) against another object, op, args
     or a create returns `OK replayed=1` with the **original** target's id, and nothing runs. The stored
     `last_status` is hard-coded OK and `app_code` 0 (`shard.cpp:1237`).
  2. Session expiry uses `time()` for both recording and expiry, and runs only inside `Engine::checkpoint()` on a
     primary. With the clock stepped forward 3,700 s, the first checkpoint expired the session and a resend applied
     a second time (QA 1: balance 2,103 to 3,103).
  3. Replays and failed ops do not refresh `touched_s`, so a client that keeps resending loses its session after
     the TTL with no clock step.
  4. Failed sequenced mutations are not recorded (resend executes again; a later success makes the failed seq answer
     `E_REPLAYED_OLD` although it never applied), and deletes are never sequenced. **Both are documented design**
     (`docs/RETRIES.md:42-45`; comment at `shard.cpp:404`), so they are documented limitations, not violations.
- **Intended contract:** `docs/CORE_CONTRACTS.md` D-5 calls `client_id` reuse "out of contract"; section 10 (R11,
  DRAFT) proposes a session-incarnation contract. No accepted guarantee exists.
- **Classification (proposed):** REDESIGN (dedup identity: server-issued or incarnation-qualified client identity,
  a request fingerprint in the dedup entry, and an expiry clock that is neither wall time nor checkpoint-driven).
- **Severity:** high (double application or false success under specific, realistic triggers: client restart with a
  reused id, clock step, long resend loops).
- **Reversibility:** the session record, the wire `SeqPrefix` and replication of sessions are formats: expensive to
  reverse.
- **Corrective action proposed (not started):** the audit-D3 slice cody listed, widened to items 1 to 3; items in 4
  go to Phase 2 rulings.
- **Required verification:** tests that a mismatched (target, op, args) under a seen seq is refused, not replayed;
  that a wall-clock step does not expire a live session; that resend loops keep the session.

### AFR-F-04. A node's authority is an unverified, in-memory belief (shared cause of stale reads, O1 and the fencing gaps)

Codex asked whether stale reads, resumed-primary role reporting and fencing share an architectural cause. The
evidence says they do.

- **Subsystem and files:** HA, replication, read path; `src/core/engine.cpp:1948-1952` (`withdraw` sets an atomic
  flag), `src/core/shard.cpp:540-575`, `:612-615`, `:461-466` (read gate), `:2119` (fence listener lives in the
  promoted process only), `:2250-2271`, `:2761-2767`; `tools/snc_server.cpp:1278-1282`, `:1443-1538`;
  `src/core/node_link.cpp:30-43` (no term in the hello).
- **The common cause (VERIFIED in code; INFERRED as a cause):** a node is primary because it believes it is. Nothing
  bounds that belief in time (no primary-side lease), nothing persists its end (withdrawal is an in-memory flag),
  and it ends only when the node happens to contact a peer that refuses it (the new primary's in-memory fence
  listener, or a witness answering `NO term`). The term is checked only at the replication handshake and in
  witness proposals; it is not in node-link hellos, client acknowledgements or WAL records. Each symptom is this
  one property seen from a different side:
  - **Stale reads:** the read gate on a primary is only the withdrawn flag; a cut-off primary answers reads of
    durable objects at once (QA 2: about 330 reads answered by a partitioned term-0 primary).
  - **O1:** HEALTH reports the belief, not a checked fact. Narrowed by reviewer B: `role=p` lasted about 0.1 s in an
    unclustered sync pair, but from code it lasts indefinitely when the fence listener is unreachable, and forever
    for an async primary.
  - **Fencing:** after a restart the fenced node is primary again at its old term. QA 2 restarted a fenced old
    primary (sync mode without witnesses) with its replication target unreachable; both it (term 0) and the
    promoted node (term 1) acknowledged about 3,000 deposits and ended with different totals (53,161 versus
    52,928). Narrowed: reaching the new primary's replication port, `--rejoin`, or any witness re-fences it.
  - **Async and no-witness modes** (R08, no-witness degrade) are the same property with the remaining external check
    removed; `PROMOTE` has no term guard (AFR-F-02).
- **Documented status (Codex requirement to keep the distinction):** async split brain and stale reads are
  **documented limitations and exclusions** (R08 in `docs/EVIDENCE.md` 12; Q7 and Q13 in
  `docs/EXPERIMENTAL_BASELINE.md`; the no-witness case as operator responsibility in `docs/REPLICATION.md:104-111`).
  They conflict with FAILURE_MODEL G1 ("never two primaries of one group serving"), which is **PROPOSED**, so no
  accepted guarantee is violated today. The restart path (fencing not durable) is not recorded as a limitation in
  any document found.
- **Classification (proposed):** REDESIGN for Rust (an explicit authority model: time-bounded leadership, durable
  fencing, the term carried in every protocol that can act on authority); UNDECIDED for C++ until Phase 2 rulings.
- **Severity:** high; critical for any deployment that runs async or without witnesses.
- **Reversibility:** the authority model shapes every protocol and the read path; it is the most expensive of these
  decisions to reverse after Rust crate design.
- **Corrective action:** none requested as an immediate slice; a Phase 2 ruling per symptom and an authority-model
  decision record before Rust design. A narrow C++ candidate exists (persist withdrawal) if cody wants one.
- **Required verification:** netns partition experiments for stale reads and async split brain (reviewer B table
  7.2, S1 and S2), and a durable-fencing restart test.

### AFR-F-05. The standby receive path buffers without bound, starting before the peer is identified

- **Files:** `src/core/replication.cpp:708-722` (buffer doubles until EAGAIN, before the Hello is checked), `:738-741`
  (frame length from the wire trusted for buffering), `src/core/storage.cpp:623-628` (fail-stop at the 1 GiB WAL
  buffer maximum).
- **Current behaviour (VERIFIED in code by reviewer B and QA 2):** any peer that connects to the replication port can
  make the standby grow memory without limit; a real primary under load can drive the standby to the 1 GiB
  fail-stop. QA 2 corrected the reviewer: records are bounds-checked (`:751-753`), so the impact is memory
  exhaustion and a fail-stop, not memory corruption.
- **Classification (proposed):** REFINE (credit-based flow control and a bounded pre-handshake buffer). Severity
  high (remote denial of service of the standby, which in sync mode blocks the primary). Reversible in C++; the
  flow-control design carries into the Rust replication protocol.
- **Corrective action proposed (not started):** a small dedicated slice, separate from AFR-F-02.

### AFR-F-06. Cross-shard request ordering has no contract and is violated by batching

- **Files:** `src/core/shard.cpp:1473-1531`, `src/core/engine.cpp:146`. Older ID: audit-D1.
- **Current behaviour (VERIFIED; QA 1 20 of 20 in-process trials per case):** a non-batchable request overtakes an
  earlier batchable EXEC to the same shard (a delete answered before the earlier withdraw; a version-conditioned
  deposit answered before the deposit it depends on). QA 1 found a second variant: when a batch cannot get a ring
  slot, the earlier ops fail with `E_OVERLOADED` after the later one may already have succeeded. Sequenced requests
  are exempt; the shipped clients route to the owner.
- **Status:** unchanged since the audit; cody listed it as D1 for a dedicated slice. Severity high for in-process
  applications and unsequenced pipelined clients; the ordering contract is a Phase 2 guarantee clause.

### AFR-F-07. There is no accepted specification to judge correctness against

- **Evidence (VERIFIED):** SNC-ADR-RUST-001 makes "the specification" authoritative, but the v0.1 specification is
  outside the repository until this PR, and is imported here only as historical design input (section 8.2); `docs/CORE_CONTRACTS.md` is a DRAFT; `docs/FAILURE_MODEL.md`'s guarantees
  G1 to G6 are PROPOSED.
- **Consequence:** today no behaviour can be called a violation of an accepted guarantee; findings AFR-F-01 to 06
  are measured against documented intent and owner rulings. Differential testing (ADR verification clause) has
  nothing authoritative to compare C++ and Rust against.
- **Classification (proposed):** the Guarantees Specification (amendment 2) is the corrective action, in Phase 2
  when authorized. Severity: critical for Rust readiness, none for current safety.

---

## 11. Requested dedicated slices and experiments (none started)

| ID | What | Basis | Why now |
|---|---|---|---|
| AFR-S-1 | audit-D2 WAL damage slice, widened to AFR-F-01 items 1-7 | cody's PR #121 review (D2, preferred fail-stop policy) | permanent silent loss of acknowledged data; evidence erased |
| AFR-S-2 | audit-D4 listener exposure slice, widened to AFR-F-02 items 1-6; interim: configured bind address (default loopback) for admin, witness, seed, node-link; remove or confine `AUDIT PATH` | cody's PR #121 review (D4, "do not defer") | the exposure is live on the lab LAN today |
| AFR-S-3 | bounded standby receive (AFR-F-05) | new | remote denial of service |
| (listed) | audit-D3 dedup slice widened per AFR-F-03; audit-D1 ordering slice per AFR-F-06 | cody's PR #121 review | already in cody's order |
| E-01 | sync pair; damage the primary's newest segment; restart; record what the sender and the standby do | AFR-F-01 inferred consequence | decides whether the reseed guidance destroys the intact copy |
| E-02 | cross-shard transfer, then damage the source record; check balance conservation | AFR-F-01 inferred consequence | conservation under storage damage |
| E-03 | reproduce test_xnode X16 at `24006ee` (55 runs, as in the diagnosis) | gap table | confirm the recorded test-race classification before it is ruled |

Each slice would follow the project's F1 discipline (root cause first, a deterministic test that fails before and
passes after, three-way QA, one PR per defect, stop for review). Order is cody's call; my suggestion is AFR-S-2
first (exposure is live and the mitigation is small), then AFR-S-1, both isolated from the D3-D5 shard-count work
and K1.

## 12. What this phase did not do

- No Guarantees Specification, no AFR-R rulings, no findings register beyond section 10, no roadmap, no readiness
  criteria, and no Rust work: all wait for explicit authorization of Phase 2.
- The CLI, developer experience and observability received an inventory only (amendment 1).
- Not re-run here: audit-D1 over TCP; the clustered O1 variant; test_xnode X16; netns partition experiments;
  power loss, EIO and ENOSPC on the WAL (no harness exists).
- The v0.1 specification is imported as historical design input only; it is not reconciled with the code (section 8.2).
