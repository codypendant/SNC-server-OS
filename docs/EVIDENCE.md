# Contracts and evidence (2026-11-04)

A consolidation pass over `main` at `7de9e98` (through PR #32), asked for
by Codex's review of the first real-machine runs. It maps what the code,
the tests and the real-machine results establish today. **It changes no
engine behavior.** The one code change is in a test (section 9, item 10). Section 12, added after Codex's review, adds characterization tests and fixes a bug in the `snc_load` benchmark client; it still changes no engine behavior. Where a claim is inferred from reading code rather than from a
test or a run, it says so.

Sources: `src/`, `tools/`, `tests/`, `bench/lab/`,
`bench/results/lab-20261104/` (Vultr P0-P6 run and the 24-round
partition re-run), and the docs each section links.

Lab experiments proposed here are named L0-L7 so they do not clash with
the handoff's E01-E08.

## 0. R01-R14 evidence map

Against "SNC Architecture Contracts and Validation Handoff" (Version 1.0,
3 November 2026; `docs/handoff/SNC_Architecture_Contracts_and_Validation_Handoff_v1.0.pdf`).
Classes: **STRONG** = implemented with strong evidence; **INSUFFICIENT** =
implemented, evidence insufficient; **PARTIAL**; **MISSING** = not
implemented; **SUPERSEDED**. "Inferred" marks a claim read from code with no
test behind it.

| ID | Class | What exists (source) | Tests / docs / real-machine evidence | Exact remaining uncertainty |
| --- | --- | --- | --- | --- |
| R01 Code versions vs durable state | PARTIAL | Every object carries `schema_ver`. The checkpoint manifest records the type catalog, and recovery from a checkpoint **refuses to start** if a type vanished or changed `schema_ver` or name (`engine.cpp:335-347`). Recovery from WAL alone (no checkpoint yet) aborts on an unknown type but does not compare `schema_ver` (`shard.cpp:1220-1224`). Long-running work is an explicit state machine in object payloads (Account outbox/inbox), not stacks or closures, so it resumes at a declared durable boundary by construction | STATUS "Known deviations" (new types accepted); no test of a catalog mismatch found | No op-level identity: follow-ups name an `OpID` whose meaning a new build could change silently. No migration path, only refusal. No mixed-version operation. No rollback point. A refusal test is missing |
| R02 External effects | MISSING | No external executor exists. The internal analogues are inbound client dedupe (sessions) and idempotent follow-ups with outbox/inbox | docs/RETRIES.md, docs/TRANSFERS.md | Everything. The follow-up and outbox pattern (section 3) is the natural home for an effect record |
| R03 Waiting vs worker occupancy | PARTIAL | Handlers run to completion on the owner and cannot block. Cross-object work is a durable follow-up, a message back through the owner. Interleaving is allowed and each step re-checks its preconditions (transfer steps, docs/TRANSFERS.md). Nothing leaves the owner for slow work | `test_transfer`, `test_migrate` | No external or async executor, so no continuation or operation token. The 20 µs handler budget is not measured (STATUS deviation). Entity-level exclusivity while waiting is undefined |
| R04 Read guarantees | PARTIAL | Owner reads wait for durability unless `allow_undurable`. Standby reads only under a lease from a sync primary (`E_STALE` otherwise). Index lookup is shard-local, synchronous with the owner's mutation, durable-only, merged across shards, and **not a snapshot** (`snc.h` comment). Stale routing gets `E_MOVED`/`E_NOT_OWNER`. Reads can see a transfer's debit before its credit | `test_replication` (standby leases), `test_persistence` (index), docs/REPLICATION.md | No multi-object snapshot or consistent cut. No version-specific read. No corpus manifest. The index is rebuilt at recovery, but has no progress watermark |
| R05 Identity and authorization | MISSING | No principal, tenant or capability anywhere in core, wire or control plane. `client_id` is a dedupe key, not an identity | none | Everything. Admin ports (STATUS, PROMOTE, AUDIT) and the witness protocol are unauthenticated plaintext |
| R06 Disaster recovery | PARTIAL (machine failures only) | Crash recovery from checkpoint plus WAL. Divergence detection and reseed. Fencing by term. IDs are not reused after a restart: the superblock's `next_lease` is advanced and written on every start (C12, `engine.cpp:359-370`), and clustered nodes take lease blocks from the control plane (`lease_lo`/`lease_hi`) | `test_crash`, `test_persistence`, `test_cluster`, `test_migrate`; Vultr P3/P5/P6 | No backup, restore manifest, restore-in-isolation, or point-in-time recovery. Nothing stops a restored old copy from competing for authority beyond term fencing. RPO and RTO are undefined |
| R07 Reproducible verification | PARTIAL | Crash injection at defined steps (67 migration scenarios). Lab harness with process and network faults on real hosts. Each run is checked against a conservation invariant (`snc_load check`, AUDIT) | `test_migrate`, `test_crash`, `bench/lab/`, `bench/results/lab-20261104/` | No controlled time, network delivery or storage completion; no deterministic simulation. `--seed` affects only `soak` (PR #31). No history checker beyond money conservation and single ownership. The lab fetches every node and witness log into the run directory (`lab.py` `down`), but `.gitignore` excludes them, so the Vultr logs were lost with host c (section 5) |
| R08 Acknowledgement boundary | INSUFFICIENT | Fully specified in section 2: accept, local commit (atomic WAL group), durable (WAL group commit as an io_uring `RWF_DSYNC` write, `storage.cpp:653`, `sri.h:62-64`; `O_DIRECT` where the filesystem allows it), replicated (standby acks after its fsync), and the client ack per mode | Section 2's tests and runs | Power loss and device write caches (assumes `RWF_DSYNC` reaches media; untested, L7). Async plus witness promotion loses acked writes (**confirmed**, section 12). The result payload is not in the dedupe record |
| R09 Cancellation and time | MISSING | No cancellation and no timers. Time appears as client request timeouts (monotonic), witness `failover_ms` (monotonic), session TTL (**wall clock**, `time(nullptr)`, `shard.cpp:686`, `engine.cpp:629`), and Paxos ballots (wall clock, `paxos.h:396-400`) | `test_replication` `test_session_ttl` | Everything in R09. A wall-clock jump can expire sessions early (inferred), which ties into R11 |
| R10 Bounded work | PARTIAL | Client `EXEC`s refused `E_OVERLOADED` above `kEmitBacklog` follow-ups per shard (creates are never refused for backlog). Bounded rings (`E_OVERLOADED` on the remote path). Outbox bounded (`kAppBusy`), inbox bounded by senders' outboxes. Replication retention budget and a catch-up rate limit. Bounded alloc failure gives `E_NO_MEMORY` for objects | `test_transfer` (`test_resend_overloaded`); P4 backlog | No per-tenant or per-entity fairness. No memory admission: a session allocation that fails **aborts** the process (`record_session`, `shard.cpp:670`). No queue-age metric. Admission metrics are not exported (section 6). No PSI. Hot-entity limits are unmeasured |
| R11 Retention and deletion | PARTIAL, **with a correctness gap** | IDs are never reused (C12). Deleted objects leave tombstones until checkpoint. WAL is kept for an unacked standby (retention budget, then reseed). Sessions (dedupe evidence) are deleted after `session_ttl_s` idle (default 1 h) | `test_session_ttl` | The retry horizon is the TTL, but it is not enforced. A client that resends with its old `client_id` after expiry has the request **run again** (**confirmed**: `test_session_ttl` now asserts the double apply; section 12). RETRIES says it "must start a new client_id"; nothing checks that. There is no other GC or retention policy |
| R12 Durable formats vs runtime layouts | PARTIAL | WAL, checkpoint and superblock have magic, `format` = 1, CRC32C, explicit offsets, little-endian only (`static_assert`, `types.h:17`). Replication, node-link and seed streams have magics (`SNCREPL1`, `SNCL`, `SNCSEED1`). Manifest type catalog | `test_persistence` (torn tail, CRC) | Formats are C++ structs written as bytes (`static_assert` sizes, no spec document). Wire protocol v0 has **no version field**. No compatibility matrix and no feature gates. A magic identifies a stream but carries no negotiated version |
| R13 Module containment | MISSING (no extensions yet) | Types and ops are compiled into the binary and registered in process. Debug builds check handler contracts (`validate` must not modify, no `emit` outside `apply`): cooperative, not isolation | `contract_violation` in `shard.cpp` | Classifying existing modules is trivial today: all are trusted core. The boundary is undefined for any future plugin |
| R14 State plus activation obligation | PARTIAL | The obligation is in the same commit domain as the transition (outbox in the object's payload, one WAL record), and is re-derived by `resume` (section 3). Indexes are authoritative synchronous structures, rebuilt at recovery | `test_transfer`, `test_migrate`, `test_cluster` | Per-type, not a facility. No timers or conditional waits, so no registration/recheck protocol. Resume is a full scan (cost at 1M unmeasured, L2) |

**Answers to the handoff's five questions** (short; details in the
sections cited):

1. *Guarantees and tests:* section 2. Single-object ops are atomic. A
   same-shard `local()` pair plus the session is one WAL group.
   Cross-shard is saga-style: no isolation, eventual settlement.
2. *Suspended execution and compatibility metadata:* explicit payload state
   machines with `resume`; `schema_ver` plus the manifest type catalog
   (refuse on mismatch).
3. *External destinations:* none yet.
4. *Retry horizon, retention, restoring authority:* session TTL (1 h
   default, wall clock, unenforced on the client); no restore process;
   authority by term fencing and placement epoch.
5. *Enforceable limits:* follow-up backlog per shard, ring depth, outbox
   size, replication retention. No tenant limits. First measured
   bottleneck: the follow-up backlog under a partition (P4, about 26K
   unsettled).

**Corrections to the addendum:**

- R04 lists "Index or search" as possibly asynchronous. SNC's index is
  synchronous with the owner's mutation and shard-local, so "lag" does not
  apply. "Not a cross-shard snapshot" and "durable-only visibility" do.
- R06 asks for allocation high-water marks. They exist as control-plane
  mint leases (docs/CONTROL_PLANE.md section 11); a restored node must not
  reuse its old lease block. That is a restore rule to write, not a new
  mechanism.
- R14 offers "consumption of committed WAL records" as a candidate. SNC
  already chose the other candidate, a durable outbox in the same commit
  domain, and should keep it.

## 1. The retry contract

**Contract.** For a sequenced mutation, the owner resolves duplicates
from the client's session *before* any refusal that would lead the
client to treat the request as not accepted and send it again as new
work.

The contract depends on what the client does with each status. These are
the as-built client behaviors:

| Status | `net::Client` | Application (`snc_load`, `test_migrate`) | Same `seq` on resend? |
| --- | --- | --- | --- |
| `E_STANDBY`, `E_FENCED`, request timeout, broken connection | fails over and resends every unanswered request in order, sequenced or not (`src/net/client.cpp:199-219`; failover statuses checked at 272) | sees one answer | **yes**: safe whatever the server did |
| `E_MOVED`, `E_MIGRATING`, `E_OVERLOADED` | passes it up | resends as a new request (`bench/snc_load.cpp:260-272`) | **no**: the contract must hold |
| `E_NOT_OWNER` | passes it up | treated as an error | n/a |
| `E_REPLAYED_OLD` | passes it up | in doubt; resolved by the balance check | n/a |

So `E_MOVED`, `E_MIGRATING` and `E_OVERLOADED` must never be returned
for a sequenced request that was already applied.

### Audit of every refusal on the client path

Read in `src/core/shard.cpp`; numbers are its line numbers at `7de9e98`.

| Path | Refusal | Session checked first? | Verdict |
| --- | --- | --- | --- |
| `execute` 96-97 | `E_STANDBY` (standby without reads, or withdrawn) | no | **safe**: the client resends with the same `seq` to the new primary, which holds the session if it holds the write |
| `execute` 105-107 | `E_MOVED` for a foreign slot | **skipped for sequenced requests** (`!opts.client_id`) | holds; the owner shard checks the session, then refuses (`run_exec`) |
| `execute` 109-128 | `E_OVERLOADED` | **yes** (the PR #28 fix) | holds. Covered by `test_transfer` `test_resend_overloaded`, which fails without the fix |
| `execute` 130-137 | `E_NOT_OWNER` | no | **holds today, with a gap**; see "Sessions across a shard-count change" below |
| `run_exec` 295-312 | `E_STANDBY` (withdrawn, or standby), `E_STALE` (reads only) | no | **safe**, as for `execute` 96 |
| `run_exec` 314-325 | `E_MOVED` / `E_MIGRATING` for a slot this shard owns but does not serve | **yes** (`replay_if_seen` at 317, then `refuse_unserved` at 322) | holds. Exercised by `test_migrate`'s crash matrix ("found by the crash tests", docs/MIGRATION.md), and deterministically by `test_resend_refusals` (sealed, moved, and both after a restart; section 12) |
| `run_exec` 326-357 | `E_NOT_FOUND`, `E_UNKNOWN_OP`, `E_TYPE_MISMATCH`, `E_BAD_ARGS`, `E_VERSION_CONFLICT`, `E_APP` | after the session (317) | holds: a duplicate never reaches them; a new request that fails changes nothing and is not logged |
| `create` 151-189 | `E_STANDBY`, `E_MOVED` (with `near`), `E_NOT_OWNER` | yes with `near` (161-165); otherwise `run_create` checks first (427) | holds |
| `remove` 191-203 | `E_STANDBY`, `E_MOVED` | **deletes are not sequenced** (client sequences `EXEC` and `CREATE` only, `client.cpp:184`; `run_delete` keeps no session) | not covered by the contract; see "Deletes" below |
| remote path (`send_request`, `fail_pending` 945-993) | `E_OVERLOADED`, `E_NO_MEMORY` | n/a | sequenced requests never take the remote path: they are refused `E_NOT_OWNER` instead of forwarded |
| `withdraw` 1406-1430 | `E_FENCED` for every deferred completion (writes waiting on the standby, and any other wait) | n/a | safe: the client resends with the same `seq` |

No new violation was found. Two gaps are worth recording; neither was
changed:

- **Sessions across a shard-count change** (inferred from code, untested).
  A session is per (client, owner shard), stored as an object in a slot of
  its shard. After a restart with a different shard count, sessions move
  with their slots (docs/RETRIES.md, "Costs and limits"), so one new shard
  can hold several sessions for one `client_id` that came from different
  old shards. `find_session` returns the index's first entry
  (`shard.cpp:634`), so a `seq` could be compared against another old
  shard's numbering. Today `net::Client` refuses to reconnect to a node
  with another shard count (`client.cpp:148-152`, "another shard count"),
  so a client cannot carry a `client_id` across a remap. That guard is
  what keeps the contract, and it is not documented as such.
- **Deletes.** A delete is not sequenced. A delete lost in a failover is
  resent by `net::Client` like any unanswered request, but without a
  session. A delete answered `E_MOVED` is passed up. A repeated delete
  cannot apply twice (IDs are never reused, C12) but answers
  `E_NOT_FOUND`. A type's `delete_op` that emits follow-ups would run its
  effects once. So "at most once" holds, but the client may see the wrong
  outcome. No current client sends deletes under load.

## 2. R08: acknowledgement and retry

When an operation passes each stage, as built (`run_exec`, `run_create`,
`complete_local`, `Shard::durable`):

| Stage | When | Code |
| --- | --- | --- |
| **Accepted** | There is no separate accept step. A request is accepted when its owner shard runs `apply` on its worker thread: after the standby, session, serving, existence, type, argument, version and `validate` checks all pass. A refusal before that point means nothing changed | `run_exec` 293-357 |
| **Locally committed** | In the same step: the object's version is bumped and its after-image appended to the shard's WAL buffer, in one atomic group with the `local()` peer and the session record (`kRecGroupNext`). Later ops on the shard see the new state. Readers do not, unless `allow_undurable` (reads wait for the object's last LSN) | `run_exec` 376-418 |
| **Durable** | The group's LSN is fsynced in this shard's WAL stream (group commit over io_uring) | `Shard::durable`, `shard.h:307` |
| **Replicated** | In sync mode, also acked by the standby after its own fsync: `durable(lsn)` = local durable and (`repl_hold_` false or lsn <= standby's acked LSN) | `shard.h:307-311`, docs/REPLICATION.md "Durability rule" |
| **Acknowledged to the client** | In `DURABLE` mode (the default), when `durable(lsn)` holds: the client's OK means durable, and replicated in sync mode. In `APPLIED` mode, at local commit | `complete_local`, `run_exec` 417 |

Durability modes, each separately:

| Mode | An OK means | Lost by |
| --- | --- | --- |
| `DURABLE`, no standby | written with `RWF_DSYNC` locally. Exceptions: `wal_sync=false` (`storage.h:120`), and no data directory, where `durable()` is always true (`shard.h:308`) | media loss (power loss is untested, see section 8) |
| `DURABLE`, sync standby (default with `--replica-of`) | fsynced on both | losing both machines |
| `DURABLE`, sync, **degraded** (standby lost past `replica_timeout_ms`, witness granted) | fsynced on the primary only; the witnesses will not promote while degraded | losing the primary before the standby catches up (the system then waits for it) |
| `DURABLE`, `--async-replica` | fsynced on the primary only | a failover: the standby may lack it. **Confirmed** (`test_async_failover`, section 12): the witness promotes an async standby like a sync one (it only knows about degraded grants, which async never requests), and the promoted node lacks every write acked while its standby was stopped. Not stated in docs/REPLICATION.md |
| `APPLIED` (`kReqApplied`) | applied in memory | any crash before the fsync. A sequenced `APPLIED` write that is lost is never resent, because it was answered |
| creates | always `DURABLE`, whatever was asked | as `DURABLE` |
| unlogged ops | applied in memory, never logged | any restart (by design: benchmark counters) |
| reads | the state read is durable (and replicated in sync mode), unless `allow_undurable` | n/a |

What a client-visible status means for retry:

| Status | Applied? | Retry |
| --- | --- | --- |
| OK, `durable` | yes, as the mode above says | no |
| OK, `replayed` | yes, earlier; the answer waited for the session record to be durable | no |
| `E_REPLAYED_OLD` | decided earlier, applied or failed; outcome not kept | no: in doubt, resolve by reading state |
| `E_FENCED`, `E_STANDBY`, timeout, broken link | unknown | resend with the **same** `seq` (the client library does) |
| `E_MOVED`, `E_MIGRATING`, `E_OVERLOADED` | no (section 1) | resend as new |
| `E_APP`, `E_VERSION_CONFLICT`, `E_NOT_FOUND`, `E_BAD_ARGS`, `E_TYPE_MISMATCH`, `E_UNKNOWN_OP` | no; nothing logged | a retry runs it again |

**One atomicity boundary.** The atomic WAL group of a sequenced mutation
holds: the `local()` peer's after-image (if any), the session record, and
the target's after-image, which carries any obligations in its payload
(e.g. the Account outbox entry). Recovery installs all of them or none
(`kRecGroupNext`), the standby receives the same group, and promotion
keeps it. **The result payload is not in it.** A replayed answer carries
status, version and ID only (docs/RETRIES.md, "Replayed results are status
and version only").

**What OK does not mean for a multi-object op.** A cross-shard or
cross-group transfer's OK means the debit and its outbox entry are durable
and the credit is owed. It does not mean the credit has landed. The credit
follows as a durable follow-up and settles eventually
(docs/TRANSFERS.md). Readers can see the money in flight.

**Evidence.** `test_replication` (sessions, duplicates, checkpoint and
restart, 30K sequenced deposits across a SIGKILL failover);
`test_transfer` (`test_resend_overloaded`); `test_migrate` (67 crash
scenarios with a session client); `bench/run_retries.py` (no loss or
double apply across six failovers); Vultr P3/P5/P6, and the 24-round
partition re-run (corroborating only; see section 4).

**Remaining uncertainty.** Async plus witness promotion: confirmed lossy, contract undecided (section 12). Sessions
through standby seeding then promotion: untested (docs/RETRIES.md lists
it). A second failover during the resends (kill mid-retry): untested.
Power loss: untested.

## 3. R14: what already exists for "state transition + activation obligation = recoverable unit"

SNC already has this pattern; it is specific to each type, not a
general facility.

| Mechanism | State transition | Activation obligation | Where the obligation lives | How it is reactivated |
| --- | --- | --- | --- | --- |
| Durable follow-ups (`OpCtx::emit`) | the emitting op's after-image | the emitted op | **in the emitting object's payload** (e.g. Account outbox/inbox), in the same WAL record as the state change | the engine queue is a volatile cache. Local follow-ups run at once (they log later in the same stream). Remote ones wait for durability (`queue_emits`, `shard.cpp:1747`). After a crash or link reconnect, the type's `resume` hook re-derives them from the payload |
| `resume` hooks | n/a | re-emit what each object's payload says is owed | the object | run on every object after recovery, and again on every node-link connect (`node_resumes`) |
| Atomic WAL groups (`kRecGroupNext`) | several after-images | n/a | one group | all or none at recovery and on the standby |
| Client sessions | session record (last `seq`, status, version) | none: an answer, not an action | in the op's WAL group | `replay_if_seen` |
| Slot-move records (`SlotStateV1`, placement move record) | PREPARED, SEALED, STAGED, COMMITTED, by CAS | the next step of the move | slot state object at the source; move record in the control plane | a second driver finishes or aborts from the recorded step (`test_migrate`) |

**Reusable as the foundation.** The rule "an obligation is part of the
state transition that creates it, and is re-derived from durable state"
is how follow-ups, resume hooks and slot moves already work. A future
Durable Event or Timer can follow it rather than introduce a separate
queue. The obligation is a record in the same atomic group, and
activation is a resume-style pass that re-derives it. Activation is
idempotent, as the Account steps already are.

**What is missing for a generic rule** (not to build now):

- **Each type re-implements the pattern.** There is no engine-level
  obligation record: each type defines its own outbox and inbox, and its
  own resume. A generic Event or Timer needs one.
- **No time.** Nothing activates at a deadline. Timers would need a
  durable due-time and an owner-shard scan or index.
- **Resume is a full scan.** It runs on every object on recovery and on
  every link reconnect. Its cost at 1M objects per shard is unmeasured
  (section 8, the 1M reconnect experiment).
- **No backpressure contract on obligations.** `kEmitBacklog` (1024 per
  shard) throttles new client work, not obligations. Section 6 covers this.

## 4. Real-machine findings kept as architecture

**The double-apply fix.** Evidence, strongest first:

1. The identified mechanism: the overload refusal came before the session
   check, so a resend after promotion could be answered `E_OVERLOADED`
   and sent again as new (docs/REAL_MACHINES.md, Findings).
2. `test_resend_overloaded`, deterministic, fails without the fix.
3. The full suite passes in the RelWithDebInfo build (14/14, rerun for
   this pass after the `test_cluster` fix in section 9). The sanitizer builds have not been rerun since 2026-10-13 (section 9).
4. 24 of 24 partition rounds on fresh hosts passed. This is consistent
   with the fix, not proof: at the old rate (about 1 in 24), a clean 24 has
   about a 36% chance without any fix.

**Slot migration.** Copy ahead, then durable seal, commit placement,
activate. The real-machine numbers support this protocol:

| Move | `--chunk 8` | `--chunk 256` | `E_MIGRATING` window |
| --- | --- | --- | --- |
| `--slot-size 1000` (1,049 objects sealed) | 4.8 s | 0.58 s | 0.1-0.2 s |
| `--slot-size 10000` (10,049) | 43.5 s | 2.9 s | 0.1-0.2 s |
| `--slot-size 100000` (**31,295-31,364 sealed**) | failed after 197 s: driver `STUCK`, slot left sealed, window 0.88 s (INFERRED cause, 2026-11-04: the driver's own 60 s deadline with a stale message, not the control plane; STABILIZATION_REPORT D) | 11.3 s | 0.1-0.2 s |

The "100K" runs moved about 31K objects: the driver logs and the check
show 31,295 (chunk 8) and 31,364 (chunk 256) sealed, although the creator
asked for 100,000. docs/REAL_MACHINES.md quotes them as 100K. **Cause
found** (section 12): a dangling reference in `snc_load create`, so most
"in slot" creates landed near garbage IDs. Fixed; a real 100K slot moved
on loopback (section 12). The 100K real-machine rows still need a rerun. Copy time scales with N/chunk round trips;
on successful moves the unavailable window does not grow with the slot.
The largest-slot chunk-8 failure shows that the admin-line
copy control does not scale. The fix is a bounded, resumable, chunked
stream with backpressure (BEGIN, CHUNK*, VERIFY(count, digest), STAGE;
docs/MIGRATION.md "Next"). It is not a weaker seal or commit.

## 5. Failure detection: kill versus silent partition

Measured (Vultr, `failover_ms` 1000, `poll_ms` 100,
`replica_timeout_ms` 2000, client request timeout 3000 ms):

| Fault | Promote | Client gap |
| --- | --- | --- |
| kill | 1.45 s | 0.5 s |
| stop | 1.63 s | 2.0 s |
| partition a<->b (26 rounds: 2 before the fix, 24 after) | 7.66-8.52 s, median 7.71 | 3.5 s (19 rounds) or 6.5-7.0 s (7 rounds) |
| oneway | 7.86 s | 6.5 s |

In the partition fault, `iptables` drops all traffic between the primary's
host and its standby's host, both ways. Each host can still reach the
third host. The client gap is bimodal and tracks the load's failover
count: 3.5 s with 8-9 client failovers, 6.5-7.0 s with 4.

The detection chain, as built. Constants come from code and the lab's
defaults:

| Stage | Who | Constant | Where |
| --- | --- | --- | --- |
| Primary seen silent | each witness polls STATUS | `poll_ms` 100, applied to connect and to the read separately, so one ask can take up to 200 ms (`ha.h:108,115`) | `snc_witness.cpp` `watch` |
| Declared failed | each witness | `failover_ms` 1000 since the last good STATUS | `watch`, line 478 |
| Promotion chosen | the witness that saw the silence proposes; Paxos majority | 300 ms per broadcast (up to 2x per peer, as above), 1000 ms propose deadline | `paxos.h` `propose`, `broadcast` |
| Promotion blocked if degraded | witnesses | `st.degraded` | `watch`, line 480 |
| Primary loses its standby | primary | `replica_timeout_ms` 2000, then asks the witnesses (1500 ms per witness) to degrade | `snc_server.cpp` 486-552, docs/REPLICATION.md |
| Standby promoted | witness sends PROMOTE | 1000 ms ask | `watch`, line 473 |
| Old primary withdraws | a blocked sync primary asks DEGRADE every 50 ms; once the promotion is chosen, a reachable witness answers "term replaced" and it withdraws **during** the cut. A second path is `sender_->fenced()` (`shard.cpp:605`) | 50 ms loop | `snc_server.cpp` 540-552, `snc_witness.cpp` 270-274. In all 26 rounds it had already withdrawn at the heal (`withdraw_s` 0.001-0.004 s) |
| Client notices | request timeout or broken connection, then probes for the primary | 3000 ms (`snc_load`) | `client.cpp`, docs/RETRIES.md |
| TCP | no keepalive or `TCP_USER_TIMEOUT` on node, replication or client links | kernel default | none set (searched `tools/`, `src/`) |

**Not yet determined: which stage the extra ~6 s of a partition
belongs to.** In this fault, one witness cannot see the primary, two can,
and the primary cannot see its standby. Codex asked for this breakdown
before any timeout is tuned. These constants do not add up to 7.7 s, and
the witness and server logs that would show it were not kept. The lab
fetched them into each run's directory on host c, but `.gitignore`
excludes `*.log`, and host c is gone. Three candidates fit the code
and need the logs to settle:

- **both groups fail over at once.** In the three-host layout both
  groups span a and b, so a cut between them fails over group 0 and
  group 1 together (docs/REAL_MACHINES.md, "dual-promote window"). That
  means two HA machines running Paxos on the same witnesses at the same
  time, and Paxos proposals in one witness process are serialized;
- the primary's degrade request racing the promotion;
- witnesses disagreeing on whether the primary is alive.

**Next step:** one partition round with every witness and node log
kept (section 8, experiment L0). No constant should change before then.

## 6. Backlog and admission (P4)

A 10 s cut between two groups' primaries built about 26K unsettled
transfer entries, which drained 8.1 s after the heal. That number is
`AUDIT`'s outbox-plus-inbox count, which scans every Account. It is not
an engine metric.

Engine metrics that already exist (`ShardStats`, `include/snc/snc.h`):

- `emits_in_flight` (queued or waiting for durability; client work is
  refused above `kEmitBacklog` = 1024 per shard);
- `emits_held_now`;
- `node_emits_dropped`;
- `node_resumes`;
- `overloaded`;
- `deferred_completions`.

None of them is in the admin `STATUS` line, which reports role, term,
uuid, shards, live, degraded, blocked, epoch and leases. Exposing them there is the smallest step
toward the scheduler and admission work. No scheduler change now.

## 7. Layering and the Rust direction (recorded, not acted on)

- **SNC Core** is what exists: objects, shards, WAL, replication,
  placement, sessions, follow-ups.
- **SNC Platform**, later and generic: native documents, durable events,
  timers, conditions and wait sets, activation, security and capabilities,
  audit and provenance.
- **Frameworks** sit on the platform: an agent runtime, an MMO runtime,
  workflows, data services, stateful-edge networking. Agent semantics
  stay out of Core.
- **Rust** is the intended production language. The C++ build is the
  behavioral oracle for that transition. Durable formats and protocol
  semantics must be specified independently of either language's
  in-memory layout. Today the WAL record, checkpoint and wire structs are
  C++ structs written as bytes (`wal_format.h`, `wire.h`, `SessionV1`,
  `SlotStateV1`), so writing their byte formats down is a prerequisite.

## 8. Named future experiments (not scheduled)

| ID | Experiment | Needs | Settles |
| --- | --- | --- | --- |
| L0 | Partition round with all logs kept | 3 hosts; keep the logs the lab already fetches (commit or archive them) | section 5's breakdown |
| L1 | Five hosts: isolate one node link without failing both groups | 5 hosts | P4 on one link; detection per link |
| L2 | 1M-object reconnect and resume | 3 hosts, more RAM | resume-scan cost (section 3) |
| L3 | Cross-datacenter RTT | hosts in 2+ regions | timeouts beyond 0.34 ms RTT; sync cost |
| L4 | Multi-socket NUMA | a 2-socket machine | shard and memory placement |
| L5 | Kill mid-retry: second failover during resends | second witness config; local first | R08 |
| L6 | Sessions through standby seeding, then promotion | local test first | R08 |
| L7 | Power loss and storage durability | real disks with power control, or a fault-injecting block device | the durable stage of R08 |

## 9. Contradictions found

1. `docs/STATUS.md` said the lab had run on loopback only. It is fixed in
   this change.
2. `docs/STATUS.md` said "All seven tests pass". There are 14 ctest tests.
   It is fixed in this change.
3. `docs/STATUS.md`'s replication row quotes the loopback sync cost
   (0.54-0.60x). Across machines it is 0.98-1.05x. Both are now given.
4. docs/RETRIES.md says sessions "move with their slots" on a shard-count
   change "and the index finds them on their new owner". That is true, but
   it omits that sequence spaces from different old shards can then meet
   on one shard. Only the client's shard-count guard keeps that safe
   (section 1).
5. docs/REPLICATION.md describes async mode, but not that a witness
   failover of an async pair can drop acknowledged writes. Confirmed by
   `test_async_failover` (section 12).
6. docs/MIGRATION.md credits the replay-before-`E_MOVED` rule to the crash
   tests. That holds; `test_resend_refusals` now pins it deterministically
   (section 12).
7. docs/REAL_MACHINES.md reports the largest P5 moves as 100K objects. The
   driver logs show about 31K objects sealed (section 4). Cause: a
   `snc_load` bug, fixed (section 12).
8. Invariant 17 was cited in the AGENTS.md handoff log ("New invariant 17
   above") but was never in the list. It is added now, with its wording
   taken from Codex's review as relayed on 2026-10-13. No earlier text of
   it exists in the repository.
9. The sanitizer claim in docs/STATUS.md ("All seven tests pass in ...
   ASan+UBSan and TSan builds") is stale. The local ASan build last ran
   14/14 on 2026-10-13, before PR #28. The TSan build last ran 10 tests
   then, with a `test_replication` failure. Both need a rerun before the
   claim is made again.
10. **QA finding, fixed in this change:** `test_cluster` failed in the
    first full ctest run of this pass (`test_cluster.cpp:1057`, group 2
    acknowledged nothing in a fixed 300 ms window after group 1's old
    primary withdrew). It reproduced in 2 of 6 runs alongside
    `test_migrate`, and passed 15 of 15 runs alone. Measured instead of
    sampled, the gap was 0.00-0.23 s in 18 loaded runs. The check now waits
    up to 10 s for progress and prints the gap. The failed CHECK also
    left the forked members running; they held ctest's output pipe, so
    ctest waited 12 minutes before it reported the failure. The test now
    kills its children when it exits. This change touches the test only.
11. Sessions do not move with a live slot migration (`moves_with_slot` is
    false for system types). That is safe today, because a resend goes to
    the group that applied it, but no doc says so.

## 10. Unresolved contracts, by priority

1. **Failure-detection policy** (section 5). Measure first (L0), then
   write the policy: per-stage budgets, keepalive or `TCP_USER_TIMEOUT` on
   links, and how degrade and promotion interact.
2. **The retry contract as a tested invariant.** Add deterministic tests
   for `E_MOVED` and `E_MIGRATING` ordering. Decide whether deletes join
   sessions. Write down the shard-count guard.
3. **Async replication under witness failover.** Either refuse witness
   promotion for an async pair, or document the loss window.
4. **Language-independent durable formats** (WAL, checkpoint, wire, system
   objects), before any Rust work.
5. **Bounded, resumable migration stream** (section 4).
6. **Obligation and activation as an engine facility** (section 3), only
   after 1-4.
7. **Admission and backlog metrics** in STATUS (section 6).

## 11. Smallest next slice (recommendation)

**L0 plus contract tests, no behavior change:**

- keep the node and witness logs the lab already fetches (commit them for failed or slow rounds, or archive them off the host);
- add deterministic `E_MOVED`/`E_MIGRATING` resend tests next to
  `test_resend_overloaded`;
- add a test that asserts `net::Client` refuses a changed shard count;
- expose `emits_in_flight`, `overloaded` and `node_emits_dropped` in the
  STATUS line.

The tests and the logging can be done locally. One partition round on
three hosts (about an hour of rental) then settles section 5 before any
failover constant changes.

## 12. Verification slice (2026-11-04, after Codex's review of PR #33)

No semantics were changed. Each item below is a test that asserts the
behavior **as observed**, so a later contract change must change the
test with it.

### R08: an async standby promoted without acknowledged writes (confirmed)

`tests/test_async_failover.py` (ctest `test_async_failover`), real
processes on loopback: `snc_server --async-replica --witness`, its
standby, one `snc_witness --failover-ms 500 --poll-ms 50`. Observed
history:

1. 500 accounts created on the primary with the standby live (set A),
   every create answered OK; 1 s for the standby to catch up.
2. Standby `SIGSTOP`. 500 more accounts created (set B): **every create
   answered OK** (creates are always `DURABLE`; async waits for the local
   fsync only).
3. Primary `SIGKILL`, standby `SIGCONT`.
4. Witness, 2.67 s after start: `group 0 failover: primary silent for
   0.50s, standby promoted to term 1 in 0.012s`.
5. On the promoted node, each ID read once (`GetBalance`): set A 0 of 500
   missing; **set B 500 of 500 missing** (`E_NOT_FOUND`).

Control, same history with a sync pair: set B is acknowledged only after
the witness grants `DEGRADE` (`primary term 0 continues without its
standby: OK`), and the witness then does **not** promote (the standby
stays a standby). Nothing acknowledged is lost; the group waits for its
primary instead.

A client that got OK for a set-B write never resends it, so the loss is
silent. Contract choices (not decided):

| Choice | Effect | Cost |
| --- | --- | --- |
| a. Witnesses never auto-promote an async standby; an operator forces it (and accepts the loss) | no silent loss | async gives no automatic availability, only a warm copy |
| b. Bounded-lag async: the primary stops acking (or asks for a grant, as sync does) once the standby is more than K records or T ms behind | loss bounded by K/T, and the witness knows when the bound is suspended | a new knob; the grant path already exists for sync |
| c. Keep promotion and document it: async OK means "durable on the primary"; a failover can lose up to the replication lag | simplest | R08's "acknowledged = survives failover" does not hold for async; needs a client-visible flag on the ack |
| d. Async acks say so (a response flag) and the client library treats them like `APPLIED` | honest per request | every client must handle a weaker OK |

Recommendation, for Codex: (a) as the default, with (b) as the opt-in
way to keep automatic failover, since (b) reuses the sync degrade grant.

### R11: a resend after its session expired applies again (confirmed)

`test_replication` `test_session_ttl` now asserts the balance.
Observed history, in one engine with `session_ttl_s = 1`:

1. Client 9, seq 1, deposit 1 cent: applied (balance 1001); resent:
   `replayed`.
2. A checkpoint before the TTL keeps the session.
3. After 2.1 s idle, a checkpoint expires it (`sessions_expired = 1`).
4. The same request (client 9, seq 1) again: **OK, not replayed, applied
   again** (balance 1002).
5. Sent once more: `replayed` (step 4 opened a new session at seq 1).
   Balance stays 1002.

The server cannot tell "a client I forgot" from "a new client". Choices
(not decided):

| Choice | How | Cost |
| --- | --- | --- |
| a. Client incarnation | the server issues `client_id`s (minted, never reused, like ObjectIDs); a sequenced request from an ID it holds no session for is refused (`E_SESSION_EXPIRED`, nothing applied) unless it is an explicit "open" | an open round trip per client; a refused client must read state to resolve in-doubt requests, as with `E_REPLAYED_OLD` |
| b. Retain dedupe differently | after the TTL keep a tombstone (client, shard, last seq: ~24 bytes) much longer, or until the client closes the session | memory grows with clients ever seen; still a horizon, only longer |
| c. Reject stale sessions | the session carries an epoch the client echoes; an unknown epoch is refused | like (a), without minting IDs |
| d. Bound retry validity | each request carries a deadline (send time + horizon < TTL); late requests are refused | needs comparable clocks (R09); a slow client can still be wrong |

Recommendation, for Codex: (a). It needs no clocks and turns the
silent double apply into a refusal the client can act on.

### Duplicates meeting E_MOVED and E_MIGRATING (now pinned)

`tests/test_resend_refusals.cpp` (ctest `test_resend_refusals`): one
clustered engine (group 0 of 2). On an account in slot S, client 5 applies
two deposits (seq 1, 2) and client 6 a `create_near` (seq 1). At four
stages (S sealed; S given to group 1; restarted from disk, still sealed;
restarted and given to group 1 again), the resends are answered from the
sessions, never refused: the latest of each client `replayed` with its
first version or ID, client 5's older seq 1 `E_REPLAYED_OLD`. New
sequences and unsequenced requests get `E_MIGRATING` or `E_MOVED`. The
account's version and balance never move. Also observed: after a restart
the slot comes back sealed and the configured (older) placement is used
until a newer one is adopted, so the node answers `E_MIGRATING`, not
`E_MOVED`, until its placement is refreshed.

### Why the "100K" slots held about 31K

`snc_load create --in-slot M` passed `ctx.got[0]` **by reference** into
the loop that appends to `ctx.got`. After the vector's first reallocation
the reference dangled, and the remaining creates were placed near garbage
IDs. It printed the requested count (`100001 in slot ...`), not a
measured one. Reproduced on one node: of 100,001 "in slot" IDs, 280 were
in the slot and 53,014 in one unrelated slot, spread over 2,048 slots.
Fixed (copy by value; the message now counts the IDs actually in the
slot): 100,001 of 100,001.

The count follows from the vector's growth (libstdc++ doubles): after
the 100,000 phase-1 accounts its capacity was 131,072, so the reference
held for 31,072 more creates, plus the ~24 phase-1 accounts already in
the slot: about 31.1K, against 31,295 and 31,364 sealed on Vultr and
31,307 on loopback (the rest are later creates whose garbage target
happened to fall in the slot). The 1K and 10K rows never reached the
reallocation, so they are correct (1,049 and 10,049 sealed).

The P5 100K move on loopback, `--chunk 256`, 20 s of load
(`bench/results/lab-20261104/p5-055814` before the fix, `p5-060000`
after):

| | sealed | move | move back | `E_MIGRATING` window | check |
| --- | --- | --- | --- | --- | --- |
| before the fix | 31,307 | 9.4 s | 4.2 s | | OK |
| after the fix | 100,050 | 57.0 s | 11.9 s | ~~0.24 s~~ invalid (see note) | OK (200,000 accounts each served once) |

**Correction (2026-11-04, PR #38).** The `E_MIGRATING` window column is
the lab probe's `migrating_window_s`, which undercounts by about 40x (its
rounds took ~0.5 s, each counted as 20 ms; MIGRATION_STAGING.md A.2).
Measured from node traces, a 100K move's window is about 6.75 s
(MIGRATION_STAGING.md section 14.1). Do not cite 0.24 s; it is kept only
as a record of the measurement defect.

**New observation, not explained yet.** In the fixed run, with no fault
injected, the destination group's primary was replaced during the final
copy: the driver logged `final copy (100050 objects): NO E_FENCED`, the
witnesses `group 0 failover learned: node 1 serves as primary, term 1`
(t = 55.0 s), and the old primary `fenced (a newer primary exists)`. The
driver retried against the new primary and the move completed; the
check passed. Inferred, not verified: staging 100K objects in one step
blocks the destination shard thread longer than the witnesses'
`failover_ms` (1,000 ms here), so they promote. This is a false failover
caused by the move itself, and one more reason for the bounded, chunked
final stage (section 4). The real-machine 100K rows remain unmeasured.

**Fixed for the HA path (2026-11-04, EL3, HEALTH_DECOUPLING.md
section 10).** With every witness polling `HEALTH`, the same 100K move
(`bench/results/lab-20261104/p5-131933`, traced) completed with no
promotion: the first final copy succeeded, and no witness poll failed or
took 20 ms during the 4.8 s final stage. The stage itself is unchanged
(the bounded-staging slice is next).

**Explained (2026-11-04, MEASURED).** Traced in 3 of 3 runs and
reproduced in one process: the final stage runs as one shard task of
3.7-5.0 s, 91% of it (97% of the put phase) re-indexing a posting list shared by every account
(one email), while `STATUS` waits on every shard thread and the witness
treats a poll slower than 100 ms as silence. The promoted node is left
with no replica. Full causal report:
[STABILIZATION_REPORT.md](STABILIZATION_REPORT.md) section D. The
contracts this evidence feeds are in [CORE_CONTRACTS.md](CORE_CONTRACTS.md)
(sections 8-11) and the open contradictions in STABILIZATION_REPORT L.
