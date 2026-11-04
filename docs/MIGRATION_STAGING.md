# Bounded and resumable migration staging (design)

**Status: ACCEPTED WITH MODIFICATIONS (review of PR #38, 2026-11-04 17:22
UTC). Nothing in this document is implemented.**

The review's decisions, which override the text below where they differ:

- **Accepted:** the S1 architecture; no separate durable manifest (durable
  staged records are the progress); a volatile FINALIZING chain; fencing
  checks between bounded units; S1, S2 and S3 as separate slices; unit
  sizing driven by latency and control progress; S3 as the direction that
  supersedes the older scratch-area stream (kept in MIGRATION.md as
  history).
- **Unit budgets:** `T_unit` 1 ms, `B_unit` 1 MiB and `W_stage` 8 MiB are
  **initial sweep points, not architecture constants**. Final values come
  from measured shard monopolization, foreground latency, HEALTH
  responsiveness, replication progress, WAL behavior, memory pressure and
  staging throughput. The goal is bounded execution, not migration
  throughput.
- **`W_window` is UNDECIDED** (decision 4 below is not accepted). The 100K
  move has a ~2.2 s O(slot) floor with an empty delta, so no 1 s target can
  be met before S1 is measured and S3 bounds the remaining work.
- **Order of work changed:** the D1/D7 fail-closed migration safety
  ([MIGRATION_FAIL_CLOSED.md](MIGRATION_FAIL_CLOSED.md)) comes before S1.
  Each step needs review before the next.

Original work order: 2026-11-04 15:57 UTC (review of PR #37).

What this document keeps unchanged:

- The move order: COPY AHEAD, then SEAL, then FINAL STAGING, then PLACEMENT
  COMMIT, then ACTIVATE (docs/MIGRATION.md).
- HEALTH stays decoupled. Option B stands: a shard or main-loop stall is
  diagnostic only, with no `wedge_ms` and no self-withdrawal.
- Protection against false failover holds only while **every** witness of
  a group polls `HEALTH` (HEALTH_DECOUPLING.md section 10).
- **The migration stall is not fixed today.** The final stage of a 100K
  slot still holds the destination shard for 4.8 s (T12). Nothing here
  changes that until a step of section 17 is built.

Evidence labels follow CORE_CONTRACTS section 0:

- MEASURED: from a trace or a run, named.
- CODE: read from the source; the reference is `file:line` at 23be29d.
- INFERRED: reasoned from the code, not run.

The baseline run is `bench/results/lab-20261104/p5-131933`. It was the lab
P5 scenario on loopback, 4 nodes in 2 groups, slot 1326 with 100,049
objects, `chunk 256`, and the safety probe running. Its 20 s transfer load
overlapped only the first ~15 s of the 44 s copy ahead: **no client load
ran during the seal, the final stage, the window or the move back**
(`bench/lab/lab.py` p5; load window t=4.35-24.4 s, seal at t≈53.5 s).

## 0. Summary and decisions for review

**What the code already guarantees (section 2).** Partial staging is
already inert:

- Staged objects at the destination are ordinary WAL records in a slot
  the destination's map gives to another group. The slot is not served
  there and no op runs on it. Index lookups filter those objects out.
- A slot becomes the destination's only when two things hold:
  - a STAGED record exists, written after every staged record in the
    shard's stream;
  - a committed map gives the slot to the destination at the move's epoch
    or later.
- Every staging step is idempotent: same `(id, version, crc)`, same
  object.

The proposal does not need a scratch area or a durable manifest to stay
safe. **The staged objects are their own manifest** (section 4).

**What is wrong today (MEASURED).** The final stage of slot 1326 ran as one
shard task of 4,772.7 ms:

| Phase | Time |
| --- | --- |
| Check of 100,049 entries | 183.6 ms |
| Put of 17,874 changed objects | 4,456.9 ms, of which 4,347.5 ms is index work |
| Scan of a 150,052-entry map | 121.6 ms |

Any other task queued on that shard waits behind it, for all slots, not
just the moving one. In the baseline no client load was running then, so
the client stall is INFERRED; the one request that measured it was the
probe's `INFO`, which waited 4,273 ms. The E_MIGRATING window was about
6.75 s from node traces; section 14 gives the breakdown. Other single tasks proportional
to the slot or to the map also run:

- the seal count/digest (191 ms);
- the exports (233 and 291 ms);
- `HAVE`;
- every reply's `slot_report`;
- `activate_staged` and `resume_slot`.

**Proposal, in three independent steps, smallest first:**

| Step | What | Changes | Bounds | Window |
| --- | --- | --- | --- | --- |
| **S1** | Destination final stage executed as a chain of bounded **units** with fencing checks between units. The request stays one admin line | node-local only; no wire, format or placement change | shard-thread time per unit | unchanged (same work, interleaved) |
| **S2** | **Converging copy ahead**: the driver repeats delta rounds before sealing until the expected final work fits a stated E_MIGRATING budget | driver only; existing verbs | none new | shrinks the final delta |
| **S3** | **Streamed transfer**: bounded requests, cursor walks at source and destination, deltas since an LSN after the seal | new versioned admin verbs, negotiated; old verbs kept | memory, in-flight bytes, request size, driver outstanding; the seal and export scans | removes the O(slot) transfers from the window |

**Adjacent defects found (appendix A).** None is fixed here.

- **D1 (fail-open serving after a lossy promotion).** A destination
  primary that loses an acknowledged STAGED record will serve the slot with
  whatever partial copy it has. This happens through an async promotion
  (R08) or an admin `PROMOTE` of a degraded standby. The placement commits
  the slot to it, and a slot with no slot-state object (`phase NONE`) is
  served. This is the one path by which partial staging **can** become
  serving state today. A fix is proposed in A.1.
- **D2.** The lab's `migrating_window_s` (0.18 s) is wrong by about 40x.
- **D3.** Every STAGE reply runs a full O(map) `slot_report`. Most of the
  44 s copy ahead is spent there (INFERRED).
- **D4.** A driver whose 15 s request timeout expires during a long
  final stage loops on HAVE (O(map) at B) and EXPORT (O(map) at A); two
  concurrent drivers can queue two whole final stages.
- **D5.** Slots above roughly 128 MiB of payload cannot move. The admin
  line cap is 256 MiB and the payload is hex-encoded.
- **D6.** The probe's own `INFO` polling adds an O(map) task per node
  every ~0.5 s.
- **D7 (source side of D1).** A's seal is checked as `durable()`, which is
  local-only when async or degraded. If A's primary is lost after the seal
  and a standby lacking SEALED is promoted (async witness promotion, or
  admin `PROMOTE`), the new primary serves S and acks writes, and the move
  can still commit to B: those writes are lost. This does not serve partial
  staging, but it breaks the move's safety under the same R08/E3
  conditions as D1.

**Decisions requested:**

1. **Accept S1 as the next implementation slice (section 17), alone.**
   Recommended.
2. **The unit budget (section 3.3).** It is a time budget per unit, plus a
   byte cap. Proposed defaults:
   - `T_unit` = 1 ms of shard-thread time;
   - `B_unit` = 1 MiB of payload;
   - `W_stage` = 8 MiB of staging WAL not yet durable.

   The defaults are to be fixed from the S1 latency sweep (section 14),
   not from throughput. Recommended: accept the semantics now and the
   numbers after the sweep.
3. **D1.** Either a fail-closed guard for moved slots (A.1) as its own
   small slice after S1, or fold it into the R08 slice. Recommended:
   its own slice after S1, because it is the only gap in "partial staging
   never serves" and it does not depend on how R08 is decided. D7 needs a
   matching source-side rule; A.1 sketches both.
4. **The S2 budget `W_window`.** This is the E_MIGRATING time an operator
   accepts for the final step. Proposed default: 1 s. The window also has
   an O(slot) floor that only S3 removes: about 2.2 s for 100K objects in
   the move back, with nothing changed (seal to activation, node traces).
5. **The older "Chunked migration stream" plan (MIGRATION.md, Next).**
   Mark it superseded by S3: there is no scratch area, and VERIFY moves
   into the destination's bounded walk (section 16). Needs Codex's
   agreement, since that plan was accepted on 2026-10-13.

## 1. Current final-stage state machine

### 1.1 Placement (unchanged by this design)

`PlacementMove.state` (`include/snc/placement.h`; MIGRATION.md "The state
machine"):

```text
NONE --prepare CAS--> PREPARED --seal at A, CAS--> SEALED --final stage at B, CAS--> STAGED
   --commit CAS (slots[S]=B, epoch=E+1)--> COMMITTED --activate at B, CAS--> NONE
abort CAS (any state but COMMITTED) --> NONE, then UNSEAL at A
```

### 1.2 The driver's two copy steps (CODE)

These are in `tools/migrate.h`.

**`step_prepared` (:554), the copy ahead.**
1. `HAVE` at B returns B's whole list for the slot.
2. `EXPORT final=0` at A takes that list and returns A's durable objects,
   with payloads only where B differs.
3. The driver sends the payload-bearing entries to B in `STAGE final=0`
   requests of `chunk` objects each. The default is 8; the lab uses 256.
4. `SEAL` at A.
5. CAS to SEALED.

**`step_sealed` (:592), the final copy.**
1. `HAVE` at B.
2. `EXPORT final=1` at A. It requires A's seal for this move to be durable
   (`shard.cpp:2248`), and returns all of A's objects in the slot, with
   payloads where B differs, plus count and digest.
3. **One** `STAGE final=1` line to B carries the whole list.
4. CAS to STAGED.

Each request is one admin line each way. `ha::ask` has a 15 s timeout
(`migrate.h:483`). The node side reads `MOVE` lines up to 256 MiB on a
thread of their own (`admin_loop.h`, `ha.h:62`).

**`NodeHandler` STAGE (`migrate.h:229`).** The handler:

1. decodes the whole list;
2. checks `current()`, a linearizable `PLACEMENT SYNC` (MEASURED ~5.1 ms
   per request). The move must be the one in progress, at PREPARED for a
   non-final stage or SEALED for a final one;
3. calls `Engine::stage_slot` (`engine.cpp:876`), which runs
   `Shard::stage_slot` as **one** `on_shard` task, then `wait_lsn` for
   durability, then `slot_report`, a second O(map) task, unconditionally,
   refusals included (`engine.cpp:887`).

### 1.3 `Shard::stage_slot`, final (CODE, `shard.cpp:2327-2420`)

One shard task, in order:

1. **Refusals.**
   - Standby or withdrawn: `E_STANDBY`.
   - Slot state with a newer `move_id`: `E_MIGRATING`.
   - Already STAGED for this move:
     - a non-final request is refused (`E_MIGRATING`);
     - a final request returns OK if count/digest match, else
       `E_VERSION_CONFLICT`.
   - Slot not foreign (B's map gives it to B): `E_MOVED`.
2. **Check** every entry:
   - Slot and type: `E_BAD_ARGS` if wrong.
   - A bare entry must already be present at exactly `(version, crc)`.
   - A payload entry must have a valid type, schema, size, crc and check,
     and must not be older than what B holds.
   - Builds `listed` (a hash map of all IDs).
3. **Put** each payload entry that differs: `put_image` does index erase
   and insert, then logs CREATE or UPDATE.
4. If not final, return here.
5. **Scan** the whole shard map. For objects of the slot, count and digest
   the listed ones and collect the unlisted ones ("gone").
6. On a count or digest mismatch, return `E_VERSION_CONFLICT`. Nothing is
   recorded as staged; the puts stay, inert.
7. **Drop** the gone objects (index erase, DELETE record).
8. **Write STAGED** `{slot, from, move, epoch, count, digest}`. It is the
   last record of this sequence in the shard's single WAL stream.

### 1.4 Slot states (CODE, `src/core/slot_state.h`)

| Phase | Meaning | Served (`Shard::serving`, `shard.h:47`) |
| --- | --- | --- |
| NONE (0) | no slot-state object | yes, if the map gives the slot to this group |
| ACTIVE (1) | activated here by move `move_id` at `epoch` | yes, if the map gives the slot to this group |
| SEALED (2) | moving away to `group` | never |
| STAGED (3) | complete copy of the sealed source, not yet authoritative | never |

Activation (`activate_staged`, `shard.cpp:2096`) runs at `set_map`, at
recovery and at promotion. It turns STAGED into ACTIVE when the map gives
the slot to this group at epoch ≥ `st.epoch`, then runs `resume_slot`.

## 2. Current durability and idempotency (CODE unless marked)

### 2.1 What is durable and what is replicated

| Record | Written by | Ack waits for | Durability class at ack (CORE_CONTRACTS section 4) |
| --- | --- | --- | --- |
| SEALED at A | `seal_slot` | `wait_lsn` (durable = WAL fsync and, if `repl_hold_`, standby ack: `shard.h:312`) | HA_DURABLE when sync and not degraded; LOCAL_DURABLE when degraded or async |
| staged CREATE/UPDATE at B | `put_image` | `wait_lsn` of the request's last LSN | same |
| gone DELETE at B | `drop_object` | the STAGED LSN (later in the stream) | same |
| STAGED at B | `write_slot_state` | `wait_lsn` | same |
| ACTIVE at B | `activate_staged` | nobody waits; the driver's ACTIVATE reply reads the report | none at reply; it is re-derived from STAGED and the map at recovery, so its loss is harmless |

There is one WAL stream per shard. A standby applies whole WAL groups in
order (`replication.cpp:640`, CORE_CONTRACTS section 5). Therefore **any
replica or recovered disk that holds STAGED also holds every staged record
before it** (prefix property). Placement is a separate domain: Paxos at
the witnesses, read with `PLACEMENT SYNC`.

### 2.2 Why partial staging is inert today

A destination object of slot S is unreachable before activation for four
reasons:

- Client ops: `foreign_slot(S)` gives `E_MOVED` (`shard.cpp:105`, `:167`,
  `:194`), and `serving()` refuses them (`:322`, and `refuse_unserved` at
  `:429`, `:492`). A sequenced request is checked against its session
  first (`:316-318`); sessions do not move, so that is harmless.
- Index lookups filter on `serving(slot_of(id))` (`:742`).
- Minting skips non-serving slots (`:172`).
- Follow-ups targeting S are routed by the map to A, where they are held
  while S is sealed (`shard.cpp:1870-1876`; MIGRATION.md, "Follow-ups
  across a move").

**What makes the slot served.** The map gives S to B, and the phase is
NONE or ACTIVE. ACTIVE at B comes only from STAGED plus such a map. NONE
at B with such a map is the D1 case (appendix A.1).

### 2.3 Idempotency

- **A put** of `(id, version, crc)` that B already holds is skipped.
  B refuses an older version, and an equal version means equal content
  because of the crc.
- **A non-final stage** of a move older than the slot state's `move_id` is
  refused. One after STAGED for the same move is refused.
- **A repeated final stage** for a STAGED move returns OK if count/digest
  match.
- **The driver keeps no state.** HAVE re-reads what B has, so work done
  before a crash is not redone.
- **The stage task is not single-flight.** Two final requests for one slot
  (two concurrent drivers) run as two whole tasks, one after the other.
  One driver cannot do this: its retry starts with HAVE, which queues
  behind the running stage, and the later STAGE gets an O(1) OK (D4).

## 3. Proposed bounded state machine

### 3.1 States (derived from the existing protocol)

**No new durable state and no new placement state.** S1 adds **one
volatile per-slot marker** at the destination, the *chain*. A shard
already holds per-slot volatile state such as `phases_` and `moved_at_`.

| State (destination, slot S, move M) | Durable metadata | Volatile | Serving authority | Legal next | On retry | Stale message | Crash / failover recovery |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **IDLE**: S foreign at B, no chain | none new; any earlier slot state (NONE, SEALED, STAGED of an older move) and leftover objects | none | none: foreign | COPYING (non-final puts while placement is PREPARED), FINALIZING | n/a | refused by move ID or `current()` | itself |
| **COPYING(M)**: existing | staged object records | none | none | FINALIZING(M), or IDLE on abort | same request again: idempotent | older M' refused (`current()`, move-ID check); non-final for M after STAGED(M) refused | IDLE plus the staged records, which HAVE finds |
| **FINALIZING(M)**: new, S1 | records written by units so far | chain `{move, token, term, unit, phase, cursor, n, d, gone, mut}` | none | STAGED(M); back to IDLE/COPYING on any guard failure, mismatch or crash | a second final for M while the chain runs: `E_OVERLOADED`, retryable, no work (the S1 driver then backs off and resends STAGE only); a final for a newer move supersedes the chain (it stops at its next unit) | refused as in COPYING; a write to S not made by the chain trips the `mut` guard | chain lost; records stay inert; the driver's retry starts a new chain, and its check finds the puts already done |
| **STAGED(M)**: existing | STAGED record, last in the stream | none | none (phase 3) | ACTIVE(M) when the map gives S at epoch ≥ E; replaced by a newer move's final, or left inert after an abort | final for M with the same count/digest: OK, no work | non-final for M or older refused; final for older refused | the record survives restart and HA-durable failover; recovery and promotion run `activate_staged` |
| **ACTIVE(M)**: existing | ACTIVE record | none | **yes** | SEALED (a later move away) | n/a | foreign checks refuse | re-derived from STAGED and the map |

**Source A** is unchanged in S1: SERVING, then SEALED(M), then either
unsealed (abort) or left sealed after the commit. S3 adds only volatile
state at A (section 3.5).

**FINALIZING is volatile on purpose.** Its loss can only move B back to
COPYING, which is already a legal, inert, recoverable state. There is
nothing to replicate and nothing to recover. Section 4 argues why durable
progress would buy nothing.

### 3.2 The S1 chain

The `STAGE final=1` line, its `current()` check and its reply are
unchanged. `Engine::stage_slot` stops calling one `on_shard` and drives a
**chain of units** from the MOVE handler thread. It uses the same
`on_shard` call that `wait_lsn` already uses in a loop. Between two units
the shard's queue runs whatever arrived, FIFO. That includes client ops
on other slots, follow-ups, `set_map` and replication; the shard's
`poll()`, which applies a withdraw, runs after each batch of queued
tasks (`runtime.cpp:349-363`).

```text
admit  (1 unit)  refusals of 1.3 step 1, unchanged; register the chain for S
                 (single flight); capture the map generation
check  (k units) validate entries in order (1.3 step 2); no writes
put    (k units) put_image for the entries that differ (1.3 step 3)
scan   (k units) run-aligned map walk (3.4): count and digest the listed
                 objects of S, collect gone IDs; restart on map regrowth
decide (in the   n == count and d == digest, else E_VERSION_CONFLICT
        last scan unit)
drop   (k units) drop_object for the gone IDs
stage  (1 unit)  guards; write STAGED (last record)
then             wait_lsn(STAGED lsn), off the shard as today; reply
```

**Every unit starts with the same guards.** Each is a read of local
state, with no round trip:

- **G-fence:** `!withdrawn_ && !repl_cfg_.standby`, and also the
  engine's withdraw flag and `sender_->fenced()` read directly, so a fence
  is seen even before `Shard::poll` has applied it (`shard.cpp:606`).
  Failure gives `E_FENCED` or `E_STANDBY` (section 8). There is no term
  check: a shard has no term field, and a primary's term does not change
  at runtime (promotion happens only on a standby, which admit refuses).
- **G-map:** `foreign_slot(S)` is still true. Failure gives `E_MOVED`; a
  commit is impossible before STAGED, so this indicates misuse.
- **G-chain:** the slot's registry entry still holds this chain's token.
  Failure means it was superseded by a newer move: `E_MIGRATING`.
- **G-mut:** the slot's mutation counter equals what the chain expects
  after its own writes. The counter is bumped by every `put_image` and
  `drop_object` on S. Failure gives `E_VERSION_CONFLICT`, retryable: some
  other writer changed S mid-chain, so the chain's view is void.
- **G-gen** (scan only): the map's rehash generation is unchanged. Failure
  restarts the scan (3.4).

**Between the decision and the STAGED write** come the drop units. G-mut
on every one of them, and on the stage unit, proves that nothing but the
chain's own drops changed S since the decision; the stage unit checks its
guards and writes STAGED with no other task in between.

**Every exit path clears the chain's registry entry**: success, refusal,
guard failure, exception, and `wait_lsn` timeout. A leaked entry would
answer `E_OVERLOADED` for that slot until a restart.

### 3.3 The unit bound

The bound is defined by what it protects. **A unit is the longest delay a
migration may add to any other task already queued on the shard.** A unit:

- starts only after the guards pass;
- processes at least one object;
- stops at the first object boundary after either limit is reached:
  - `T_unit` of shard-thread time (`trace::now_ns`, read per object, about
    20 ns);
  - `B_unit` bytes of payload put or dropped.

The resulting bound on one unit's length is
**`T_unit` + the cost of one object**.

The cost of one object is not bounded by this design. It is the cost of
`put_image` or `drop_object`: a payload copy (≤ the type's `max_size`), a
WAL append, and an index erase/insert that is **linear in the posting
list**. MEASURED ~0.24 ms per put at a 150K posting list (4,347.5 ms /
17,874). Bounding it is the index slice, which is out of scope here
(section 11).

Why these quantities:

- **Time, not objects.** An object count only bounds time when the
  per-object cost is known, and it is not: it ranges from microseconds to
  0.24 ms here, depending on the index. A time budget adapts by itself.
  That is the "adaptive" bound the work order allows.
- **`B_unit`** bounds the WAL and replication burst one unit adds, and the
  memory a unit touches.
- **`T_unit` default 1 ms, pending section 14.** It is about the
  end-to-end p50 of an ordinary op in the baseline run (load p50 1.6 ms).
  At most it doubles the queueing of an op behind a unit, instead of
  adding 4.8 s.

  Per-unit overhead is one cross-thread submit and latch (tens of µs,
  INFERRED). At 1 ms that keeps the overhead to a few percent of the
  chain's time. The sweep in section 14 measures other-slot p99 and p99.9
  against `T_unit` ∈ {0.25, 1, 4} ms, plus the chain's total time. The
  default is picked from that curve, not from throughput.
- **Not 256.** The lab's `chunk 256` is a request size for the copy ahead.
  It is unrelated to unit length and stays a driver option.

### 3.4 Walking the shard map in bounded units

`LocalMap` is open addressing with linear probing and backward-shift
deletion, with one-step rehash on growth (`include/snc/local_map.h`). A
plain index cursor kept across units is **unsafe**: an erase of another
object can shift an entry from behind the cursor to in front of it, so it
is missed, and a rehash moves everything.

**Run-aligned cursor.** A unit may end only **just after an empty
bucket** `e`. The walk starts just after some empty bucket and goes once
around the table. It is a full restart if the rehash generation changed.

**Claim.** Every entry present for the whole walk is visited exactly once.
An entry visited zero or two times was inserted or erased during the
walk.

**Proof sketch.**

1. Entries never move forward. Linear-probing inserts do not move existing
   entries, and backward shift moves an entry only toward its home bucket.
2. At the moment a unit ends at an empty bucket `e`, every entry at a
   position after `e` (cyclically, within the region not yet walked) has
   its home after `e`. Otherwise linear probing would have placed it at or
   before `e`, which was empty.
3. A backward shift never moves an entry past its home. So that entry can
   never move to `e` or before it, whatever is inserted at `e` later. An
   entry at or before `e` never moves forward past it.
4. Hence no entry present at the unit boundary crosses the cursor in
   either direction. One that crossed it would have to be inserted after
   the boundary.

**Use.** The destination's scan walks while nothing of slot S is inserted:

- G-mut covers writes by others;
- the chain's own puts finish before the scan;
- the chain's drops happen after it.

So the scan sees every object of S exactly once. Other slots' entries may
churn freely.

**Assumptions.**

- The unit-end condition needs an empty bucket within reach. The load
  factor is ≤ 0.8 (`local_map.h:54`), so runs are short on average. A run
  longer than the budget makes that unit longer (bounded by the longest
  run).
- Growth restarts the walk. Growth is geometric, so restarts during one
  walk are at most log2 of the map's growth during it.

The property is load-bearing, so it gets its own randomized model test
(test L).

### 3.5 S2 and S3 in this state machine

**S2: converging copy ahead (driver only).** The copy ahead loops:

1. One round is HAVE, `EXPORT final=0` and the STAGE chunks, exactly as
   today.
2. Each round, the driver records Δ (objects sent) and the destination's
   install rate ρ (objects per second of the round's STAGE requests,
   timed by the driver's clock; replies carry no stage time).
3. It seals when **Δ ≤ ρ · `W_window`**. That is, when the expected final
   delta fits the E_MIGRATING budget.

It also seals when:

- Δ did not fall below half of the previous round's Δ (not converging:
  the write rate on the slot is too close to the install rate); or
- `R_max` rounds have run (default 8).

These two rules end the loop under any load. Every reason to seal is
logged (`move_seal_reason`). Rounds use only verbs that exist today and
act only while the placement is PREPARED. So S2 changes nothing about
safety: a round is exactly a copy ahead.

**S3: streamed transfer.** New versioned admin verbs (section 12):

| Verb | At | Does | Bounded by |
| --- | --- | --- | --- |
| `MOVE CAPS` | any | advertises `stage-stream=1` | O(1) |
| `MOVE WALK SLOT MOVE CURSOR SINCE LIMIT` | A or B | one run-aligned walk unit over S. Returns bare entries `(id, version, type, schema, crc)` with `last_lsn > SINCE` (A: durable entries only while not sealed), plus the next cursor and the map generation | unit |
| `MOVE FETCH SLOT MOVE FINAL ID...` | A | payloads for those IDs; `absent` for IDs not in S. With `final=1`, only while sealed and durable for M | request bytes ≤ `B_req` |
| `MOVE PUT SLOT MOVE FROM EPOCH OBJ...` | B | the put half of today's stage, also allowed while the placement is SEALED. Refused after STAGED(≥M) | unit chain |
| `MOVE DROP SLOT MOVE ID...` | B | drops these objects of S. Same refusals as PUT | unit chain |
| `MOVE CLOSE SLOT MOVE FROM EPOCH COUNT DIGEST` | B | the scan, decide and stage tail of the S1 chain. On a mismatch it replies with B's `(n, d)` | unit chain |

The S3 final step:

1. **Delta.** Run WALK at A with `SINCE` = A's durable LSN at the start
   of the last copy-ahead round. Then FETCH and PUT whatever differs.
   That covers every object changed since that round. Objects unchanged
   since then were durable at its start, so that round copied them.
2. **CLOSE** with A's seal count and digest.
3. **On a mismatch, reconcile.** WALK at B, FETCH-check those IDs at A, and
   DROP the absent ones at B. This handles deletions and anything a
   too-optimistic `SINCE` missed. Then a full WALK at A, FETCH and PUT, and
   CLOSE again.

**`SINCE` is valid only within one incarnation of A.** Recovered and
replicated installs set `last_lsn = 0` (`shard.cpp:1240`), so after any
restart or failover of A the delta would miss older changes. A WALK reply
carries A's incarnation, and a change of incarnation voids `SINCE` (the
driver does a full walk). Deletions are never visible to a delta; they
always go through the reconcile.

**Correctness never depends on the delta.** CLOSE compares B's full
count and digest with A's seal, as the final stage does today.

**The seal's own count and digest, at A.** S3 also makes these bounded:

- SEAL writes SEALED at once, which is O(1).
- A walk then counts the sealed slot in units and rewrites SEALED with the
  count and a new flag bit `kSlotCounted`.
- `EXPORT/FETCH final` requires the counted SEALED record to be durable.

The driver passes the seal's count to CLOSE. It is computed by a walk
independent of the one that fed the data. A cursor bug therefore shows as
a mismatch, not as a silently short copy, unless both walks miss the same
object. Test L is what guards against that.

## 4. Durable staging manifest: not needed

A manifest would record which parts of a final stage are done, so that a
crash need not redo them. CORE_CONTRACTS B-1 says work "MUST be chunked
and resumable, with its progress in durable state **where it must survive
a crash**". It need not survive here:

1. **The staged records are the progress.** After a crash or failover, the
   next HAVE (S1, S2) or WALK at B (S3) reads exactly the objects that were
   put. Those are not sent or put again: they become bare entries, and the
   check only confirms them. Only the scan is redone.
2. **Correctness needs no manifest.** The commit condition is STAGED, which
   is written only by a chain that verified B's whole copy (count and digest
   against A's seal), with G-mut proving S unchanged between that check
   and the write. That verification has
   to be fresh anyway: a manifest saying "done" before a crash would not
   prove B's state after the crash.
3. **A manifest would be a new durable format** that has to be replicated,
   recovered, checkpointed, versioned and garbage-collected. Each of those
   is a mixed-version risk the work order asks to avoid.
4. **What crash recovery loses** is at most one chain's check and scan
   time (~0.3 s at 100K, MEASURED 183.6 + 121.6 ms) plus the in-flight
   unit. It is not the put work, which is the expensive part.

**When a manifest would become worth it:** if a scan of B's map ever
costs more than a manifest's upkeep. For example, a destination shard
map in the hundreds of millions of entries. That is not a v0.1 shape.

## 5. Chunk identity and retry semantics

| Thing | Identity | Retry | Duplicate or reordered |
| --- | --- | --- | --- |
| S1 unit | `(slot, move, chain token, unit seq)`, for traces only | units are never retried one by one; **the request is the retry unit** | impossible within a chain; a second chain is refused (single flight) |
| S1 request (`STAGE final`) | `(slot, move, count, digest)` | the driver retries the whole request after any error or timeout; HAVE makes the retry incremental | same move: refused while a chain runs (`E_OVERLOADED`; the driver backs off and resends the same STAGE, without HAVE or EXPORT); once STAGED, OK if count/digest match, else `E_VERSION_CONFLICT` |
| S2 round | `(move, round)`, in driver memory and traces | a dead driver's replacement starts at round 1; it is idempotent, only time is lost | n/a |
| S3 PUT and DROP | content-addressed per entry: `(id, version, crc)` or `(id)` | any number of times, any order | harmless; a put never goes backward (an older version is refused), and a drop of an object A still has is undone by the next reconcile |
| S3 WALK | `(move, side, node incarnation, map generation, cursor)` | a reply with a different generation or incarnation restarts the walk; the driver discards its partial sums | a repeated walk unit returns the same entries |
| S3 CLOSE | `(slot, move, count, digest)` | as S1's request | as S1's request |

**Refusals are idempotent.** Each one is a pure function of local state
and the request, and changes nothing. `E_OVERLOADED` (busy, or over the
in-flight limit) means "same request later". `E_MIGRATING`, `E_MOVED`,
`E_STANDBY` and `E_FENCED` mean "re-read the placement, then decide". The
driver already does both.

## 6. Crash matrix

The S1 destination chain, for move M of slot S from A to B. "Disk" means
B's own WAL. "Standby" means B's sync standby. "Served?" covers any node
of B's group: it is **no in every row before the commit, because S is
foreign to B**. The last column says what happens next.

| # | Crash point | On B's disk | On B's standby (sync, healthy) | Served? | Next |
| --- | --- | --- | --- | --- | --- |
| C0 | request received, before admit | copy-ahead records | same (acked) | no | driver gets no answer, retries; HAVE |
| C1 | during check | same | same | no | same |
| C2 | during put, after k units | + up to k units of puts (durable or not) | the prefix it acked | no (foreign; no STAGED) | retry: HAVE lists what survived; those are bare entries, the rest is sent again |
| C3 | after the last put, before scan | + all puts | prefix | no | same; check confirms them |
| C4 | during scan | same; the scan writes nothing | prefix | no | retry; the scan is redone |
| C5 | after decide = match, during drop | + some DELETEs | prefix | no | retry; the drops left are found again (the gone objects are still unlisted) |
| C6 | STAGED appended, not yet on disk | as C5 | prefix without STAGED | no | retry; the full chain runs again |
| C7 | STAGED on B's disk, not acked by the standby | + STAGED | maybe without STAGED | no: placement is still SEALED and the driver got no OK, since `wait_lsn` needs the standby's ack | restart of B: STAGED is there; the retry gets OK with no work. Failover to the standby: no STAGED there; the retry re-runs the chain |
| C8 | STAGED HA-durable, reply lost | + STAGED | + STAGED | no | retry gets OK (STAGED same move, same digest) |
| C9 | driver has OK, CAS to STAGED not done | same | same | no | the new driver finds placement SEALED, re-runs the final; OK with no work |
| C10 | placement STAGED, commit not done | same | same | no | new driver commits, or aborts (B keeps STAGED, inert) |
| C11 | commit done, ACTIVE not written | same | same | no: the map gives S to B but phase is STAGED | `activate_staged` at the next `set_map`, recovery or promotion; the driver's ACTIVATE asks |
| C12 | ACTIVE appended, not durable | + maybe ACTIVE | maybe | yes, by B's primary, which holds STAGED | after a crash, recovery re-derives ACTIVE from STAGED and the map |
| C13 | after activation | ACTIVE | ACTIVE | yes | done |

**Cross-cutting rows:**

- **Source A crash or restart** at any point after the seal. SEALED is
  durable before any final export (`shard.cpp:2248`): HA_DURABLE when sync
  and healthy, which is the case this row covers. Then the prefix property
  gives the same sealed object set on A's disk and on its standby. Async
  or degraded-and-admin-promoted: D7. The driver re-reads A's primary and continues. A's digest does
  not change, so B's chain for M stays valid (section 7).
- **Driver death mid-chain.** The chain runs to its end. The MOVE handler
  thread keeps driving it after the client hangs up (`MOVE` runs on a
  detached thread; the reply write uses `MSG_NOSIGNAL`,
  `snc_server.cpp:592-616`). The new driver's STAGE is refused while the
  chain runs, then answered from STAGED. Single flight plus the driver's
  backoff is what addresses D4.
- **Fencing mid-chain:** section 8.
- **Placement abort mid-chain**, which is possible because the placement
  is SEALED. The chain may still write STAGED(M). It stays inert, since no
  commit of M can follow an abort (MIGRATION.md proof), and a later move
  replaces or overwrites it. The table in section 3.1 covers the later
  final.

**Degraded and async groups.**

- **Sync but degraded.** Acks are LOCAL_DURABLE, and the witnesses refuse
  promotion while the DEGRADE grant is on record (CORE_CONTRACTS E2). The
  rows hold, with "standby" read as "not promotable".
- **Async witness promotion (E1, R08), or an admin `PROMOTE` of a stale
  standby (E3).** C8 and C9 still recover (the retry re-stages at the new
  primary), unless the original driver survives C9 and does the CAS.
  C10-C13 break: once the commit exists, a promoted node lacking STAGED
  (and ACTIVE) has phase NONE and serves. This is **D1**. S1 does not make
  it worse and does not fix it.

The **S3 rows** are the same with requests in place of units:

- every PUT, DROP or WALK request is a C2/C4-style point;
- CLOSE is C4-C8;
- A's counting walk happens before any final FETCH is allowed, so a crash
  in it leaves SEALED uncounted and the driver waits for it.

## 7. Source and destination failover

**What is replicated (section 2.1):**

- Staged records, drops, SEALED and STAGED are ordinary WAL records. They
  are replicated in stream order, and their acks have the durability
  class of the group's mode.
- Nothing about a chain, an S2 round or an S3 walk is replicated. **No
  volatile state is needed after a failover**, because recovery is "retry
  the request" (section 4).

**Destination failover** (B's primary dies; the witnesses promote its sync
standby):

- The new primary's `Shard::promote` runs `refresh_slots` and
  `activate_staged` (`shard.cpp:1394-1397`). That is correct: it holds a
  prefix of the staged records, and STAGED if STAGED was acked (it may
  also hold an unacked STAGED, C7).
- The old primary's chain dies with it. If it was only fenced and not
  killed, its chain stops at its next unit (section 8).
- The driver finds the new primary through `MOVE ROLE` and retries.

**Source failover** (A's primary dies after the seal):

- When A was sync and healthy, the promoted standby holds SEALED(M) and
  the identical sealed set. Otherwise see D7.
- In S1/S2 the driver just re-asks the new primary.
- In S3, walk cursors carry A's node incarnation. A WALK reply from the
  new primary carries a different incarnation, so the driver restarts the
  walk and its partial sums. A FETCH needs no cursor.

**Source failover before the seal:** the copy ahead exports durable state
only, so nothing staged at B can be ahead of A's new primary.

**Both groups failing over at once:** the same rules apply independently.
Nothing in the design couples A's and B's volatile state.

**CORE_CONTRACTS E9** ("a move in progress is resumable by the new
primary") was UNPROVEN. With S1, tests D and F (section 13) turn it into
a measured claim.

## 8. Fencing

Long work must stop when the node loses authority, between units, by
local state alone:

- **Withdrawn (fenced) primary.** `Engine::withdraw` stores an atomic
  flag (`engine.cpp:904`); each shard applies it in `Shard::poll()`, as
  does a fenced replication sender (`shard.cpp:606-607`). G-fence reads
  both the shard's `withdrawn_` and the two sources directly, so the next
  unit stops whether or not `poll()` has run. The reply is `E_FENCED`, the
  same status `wait_lsn` gives a withdrawn shard today (`engine.cpp:805`).
- **Standby.** G-fence checks `repl_cfg_.standby`; a primary never becomes
  a standby without a restart.
- **Map change.** `set_map` is also a queued task. G-map re-reads
  `foreign_slot(S)` before each unit. A map that gives S to B before
  STAGED exists cannot be produced by the protocol. If it appeared anyway,
  the chain stops with `E_MOVED` rather than staging into a served slot.
- **Superseded move.** G-chain covers it.

**Latency of fencing** = the rest of the current unit, ≤ `T_unit` + one
object. Today it is up to the whole 4.8 s task, after which `wait_lsn`
reports `E_FENCED`.

**No consensus on the hot path.** Guards read shard fields only.
`current()`'s `PLACEMENT SYNC` stays once per request, as today, and is
not repeated per unit.

**What fencing does not stop.** The records a fenced old primary appended
before it noticed are on its own disk only. The term handshake keeps them
off the new primary (CORE_CONTRACTS E7), and the old primary is reseeded
on rejoin.

## 9. Follow-ups

**Nothing changes in follow-up handling.**

- Units do not run ops, emit, hold or release follow-ups. `put_image` and
  `drop_object` write images; they do not execute.
- Follow-ups targeting S are held at A (sealed) and wherever they route by
  the old map. They are released by activation, unseal or a map change,
  exactly as today. There is **no second queue**: a chain is a
  continuation, not a message queue.
- Activation still runs `resume_slot` (re-sending the slot's unsettled
  work stamped with the new epoch) and `release_held` in the activating
  task.

**The held queue (`emit_held_`) grows with the E_MIGRATING window times
the follow-up rate.** It is in memory and unbounded today (MIGRATION.md
Limits). S1 leaves the window unchanged; S2 and S3 shrink it. Section 14
measures the queue's high-water mark (`emits_held`).

## 10. Memory and backpressure bounds

| Resource | Today (CODE / MEASURED) | S1 | S3 |
| --- | --- | --- | --- |
| Request line | whole slot: 12.6 MB final STAGE, 5.5 MB HAVE, 45.4 MB first copy-ahead EXPORT reply (MEASURED); effective cap set by the 500 ms read deadline (D5) | unchanged | ≤ `B_req` (proposed 1 MiB) |
| Destination memory per final | decoded list + `listed` hash + gone list: O(slot) | same, held for the chain's life | O(1) accumulators + one unit |
| Source memory | export list: O(slot) | unchanged | one walk unit + one FETCH |
| Driver memory | HAVE + EXPORT lists: at least 45 MB of text at 100K (the first EXPORT reply) plus decoded copies | unchanged | one unit's lists; partial sums |
| Shard-thread time per task | 4,772.7 ms (MEASURED) | ≤ `T_unit` + one object | same |
| Staging WAL not yet durable (also the replication lag it adds) | all of one request: 17,874 objects in one burst | ≤ `W_stage`: the chain counts the bytes of its own appends with their LSNs, and does not start a put or drop unit while its appended-but-not-durable bytes exceed `W_stage`; it waits with `wait_lsn` on its oldest such LSN | same |
| Queued staging work per shard | any number of final requests (D4) | one chain per slot (single flight); one move per cluster | same |
| Driver outstanding requests | 1 | 1 | ≤ 2 (one WALK or FETCH ahead of one PUT); 1 by default |

**Why `W_stage` matters.**

- Every client write on B's shard acks in LSN order behind the staging
  records. A burst of staging writes therefore delays every client ack on
  the shard by the time the standby takes to absorb it.
- `W_stage` caps that delay at about `W_stage` divided by replication
  bandwidth.
- Proposed default: 8 MiB, a few WAL groups. It is set from the sweep, as
  `T_unit` is.

**S1 does not bound memory.** It keeps the whole list in memory as today.
S1 bounds shard time, fencing latency and the staging WAL burst. Memory
and request size are bounded only by S3.

## 11. Index interaction

What the index does during staging, today and unchanged:

- `put_image` erases the old key and inserts the new one. `drop_object`
  erases the key.
- Staged IDs live in the shard's posting lists from the first put.
  `index_find` filters them out with `serving()`, so they are invisible
  but still scanned.
- After a restart, `rebuild_index` includes them.

**Boundable along with installation.** The unit's time budget counts index
time (it is inside `put_image`). A unit therefore does as many objects as
fit, and index cost limits the rate, not the bound. **The remaining
unbounded term is one object's index erase,** linear in its posting list
(MEASURED ~0.24 ms per put at 150K). Splitting a single erase is the index
slice. Here it is named, not optimized:

- no special case for the shared test email;
- no change to `index_erase`;
- no change to posting lists.

**Pathology carried by the measurements.** The baseline load gives every
account one test email, so a single posting list holds the whole shard.
Section 14 reports results both with that workload and with distinct
emails, so the bound is not judged only on the pathology.

## 12. Compatibility

| Surface | S1 | S2 | S3 | A.1 (D1 guard) |
| --- | --- | --- | --- | --- |
| Durable format (WAL, checkpoint, `SlotStateV1`) | unchanged: same records, same order rule (STAGED last) | unchanged | one new flag bit `kSlotCounted` in `SlotStateV1.flags`; an older binary ignores it (it only reports `count`) | unchanged |
| Admin protocol | unchanged: same verbs, same replies | unchanged | **additive**: new verbs behind `MOVE CAPS`. An old node answers `NO unknown request`, and the driver falls back to S1/S2 verbs, which a new node keeps serving | unchanged |
| Placement encoding | unchanged | unchanged | unchanged | **changes**: a per-slot `moved` epoch. `Placement::decode` rejects unknown keys (`placement.cpp`), so this needs a `v2` encoding written only after every node can read it |
| Replication | unchanged; staging is ordinary records | unchanged | unchanged | unchanged |
| Older node as A or B | n/a (node-local) | works: old verbs | falls back per node | old nodes keep today's fail-open behavior until upgraded |
| Older driver | works: same verbs | n/a | an old driver uses old verbs; a new node serves them | old driver writes v1 |

**Mixed-version corruption risk:**

- **S1-S3: none identified.** No record changes meaning. Every new verb
  is refused by old nodes, and a refusal makes the driver fall back.
- **A.1** needs a staged rollout: every node first, then the encoder.

## 13. Deterministic test plan (A-T)

**Determinism.** All tests are deterministic. They use:

- seeds;
- stop hooks at named unit boundaries. The proposal is a
  `--debug-admin`-only `DEBUG MOVE-STOP <slot> <phase> <unit>` that parks
  the chain, like `DEBUG STALL`;
- `SIGKILL` at those points.

Tests that need real processes extend `tests/test_migrate.cpp`, which
already runs 3 witnesses, 2 groups with standbys, a session client and
the never-two-owners probe.

| ID | Test | Pass criterion |
| --- | --- | --- |
| A | unit bound: final stage of 100K, distinct emails and shared email, probe off | every `stage_unit` span ≤ `T_unit` + max single-object cost (trace); other-slot ops on B's shard complete during the chain. Not "max loop iteration": HAVE, `slot_report` and other O(map) tasks outside S1's scope still run (section 17) |
| B | equivalence: same input to the whole-task path and the S1 chain, random slots with changed, unchanged, leftover and extra objects | identical final objects, slot state, count, digest; same record multiset |
| C | partial never serves: SIGKILL B at every phase and unit boundary (C0-C8), restart | S never served by B (probe); `index_find` never returns S's IDs; ops get `E_MOVED`; move completes; balances exact |
| D | destination failover (sync, healthy) at each of C0-C11 | as C, plus the new primary completes the move |
| E | destination degraded during the chain | witnesses refuse promotion while degraded; the chain completes or is retried; no lost STAGED |
| F | source failover after the seal, during B's chain | same sealed count and digest at the new primary; move completes |
| G | fencing mid-chain: withdraw B's primary between units (set the flag, and separately fence the sender) | chain stops at its next unit (`E_FENCED`); no `stage_unit` trace after the flag was set |
| H | map refresh between units (`MOVE REFRESH`, same and newer unrelated epochs) | chain unaffected, or stops with `E_MOVED` only if S became B's (forced in a unit test) |
| I | duplicate final while a chain runs (second driver, and D4's timeout) | second request `E_OVERLOADED` with no work; later retry OK with the same digest; one chain in traces |
| J | stale writer mid-chain (inject a put to S between units) | G-mut trips: `E_VERSION_CONFLICT`; no STAGED; retry succeeds |
| K | late non-final for M after STAGED(M), and final for an older move | refused, unchanged from today |
| L | run-aligned cursor model test (`test_local_map`): random inserts and erases of other keys and forced growth between units, millions of steps, seeded | every key present for the whole walk visited exactly once; growth always detected |
| M | rehash during B's scan (create objects in other slots between units) | scan restarts; correct result; restart count ≤ log2 of growth |
| N | driver dies mid-chain | chain finishes; new driver gets OK; one STAGED |
| O | placement abort mid-chain | at most an inert STAGED(M) at B; A unsealed; a later move of S to B succeeds and overwrites it |
| P | follow-ups across the chain (transfers to and from S) | exactly-once (existing balance check); `emits_held` high-water reported |
| Q | backpressure: high staging volume against a slow standby | staging WAL not durable ≤ `W_stage` (trace); client ack p99 on B's shard reported |
| R | rollback: chain with S1 on, crash, restart with S1 off; and the reverse | move completes; identical data to B |
| S | S2 convergence under load, and under a write rate above the install rate | Δ shrinks then seals on budget; the non-converging case seals at the half-rule or `R_max`; window measured |
| T | D1 characterization: async group (R08 harness, `test_async_failover`) promotes a standby lacking STAGED after the commit | today: expected-fail, documenting that the slot is served with a partial copy; with A.1: not served (`E_MIGRATING`) until repaired |

Sanitizers: A-R run under ASan+UBSan and TSan as the PR #37 suites did.

## 14. Performance measurements

### 14.1 Baseline (MEASURED, p5-131933, 100,049 objects)

The traces are `driver-move.trace`, `driver-back.trace`, `n1.trace` (A)
and `n3.trace` (B). Times are driver clock seconds from the move's
prepare.

| Phase | Move (17,874 changed in the final) | Move back (0 changed) |
| --- | --- | --- |
| Copy ahead (prepare to "copied") | 44.15 s: 99,984 objects in 391 STAGE requests; request p50 101.5 ms, p99 300 ms; on-shard stage p50 0.7 ms | 0.92 s (HAVE 332 ms, EXPORT 369 ms; nothing to send) |
| Seal request | 359 ms (A on-shard 191.4 ms: O(map) count/digest) | 312 ms (on-shard 141.5 ms) |
| Seal to STAGED OK | 6.53 s: HAVE 245 ms (5.5 MB), EXPORT final 804 ms (12.6 MB), STAGE final 5,223 ms (on-shard 4,772.7: check 183.6, put 4,456.9 [index 4,347.5, WAL 72.7], scan 121.6) | 1.89 s: HAVE 274, EXPORT 543, STAGE 844 (check 313.4, scan 244.1) |
| Placement STAGED and commit | 20 ms | 25 ms |
| Activation (ACTIVATE request; includes the map refresh, `activate_staged`, `resume_slot` and the report) | 202.5 ms | 338.4 ms |
| **E_MIGRATING window** (seal record at A to activation at B, node traces) | **≈ 6.75 s** (seal at 44.35 from n1's `seal` trace; activation at ≈ 51.10, the `set_map` iteration on n3 right after ACTIVATE arrived) | **≈ 2.2 s** (≈ 1.08 to ≈ 3.29) |
| Total | 51.65 s | 3.76 s |
| B's longest shard iteration | 4,777 ms | 589 ms (MEASURED; stage on-shard) |

**Readings.**

- The window has two parts:
  - work on the **delta**: 4.46 s of puts, nearly all of it index;
  - an **O(slot) floor**: seal, HAVE, EXPORT, check, scan, activate. The
    floor was about 2.2 s at 100K with zero changes (the move back).
- S1 removes the stall but neither part. S2 shrinks the delta. S3 shrinks
  the floor.
- **The lab's own window figure is wrong (D2).** `migrating_window_s` was
  0.18 s in `result.json`, but the probe's 129 rounds took ~0.5 s each, so
  it sampled the window a few times at most.
- **The copy-ahead time is mostly not copying (D3, INFERRED).** On-shard
  stage time was p50 0.7 ms, but the request took p50 101.5 ms. Its
  `wait_lsn_ms` trace field (p50 88.8 ms) also covers the reply's
  `slot_report`, an O(map) scan with a CRC of every slot object
  (`engine.cpp:886-890`). B ran 478 injected tasks of ≥ 20 ms (p50
  75.4 ms) against 392 STAGE requests and 129 `INFO`s.

### 14.2 What S1 to S3 will be measured on

Each phase separately, as the work order asks:

- copy ahead (and per S2 round);
- seal to finalize (seal, the final export or walks, the final chain);
- commit;
- activation;
- the E_MIGRATING window;
- total.

Alongside:

- B's and A's max shard iteration and the histogram of iterations
  ≥ 1 ms;
- other-slot op latency p50, p99 and p99.9 on B's and A's shards during
  the final;
- staging WAL not yet durable (max);
- replication lag (max);
- `emits_held` high-water;
- RSS high-water of driver, A and B;
- chain count, units, scan restarts;
- S2 rounds and seal reason.

**The window will be measured from node traces, not from the probe.** It
runs from the seal record's write at A (`seal` trace) to the ACTIVE write
at B (a new `slot_active` trace). D2 is fixed in the lab's metric as part
of the S1 slice only if Codex agrees, since the lab is test tooling.

**Matrix:**

- slot sizes 10K, 100K and (if memory allows) 1M;
- loads none and transfer;
- shared email and distinct emails;
- `T_unit` ∈ {0.25, 1, 4} ms;
- probe off and on (D6).

Each cell is at least 5 runs, reported as median and spread. PR #37's T9
showed 8-16% host noise; a difference smaller than the spread is reported
as none.

**Hypotheses, not claims:**

- S1: the longest final-stage unit falls from 4.8 s to about 1-2 ms with
  distinct emails. B's longest loop iteration falls only to the largest
  remaining O(map) task (HAVE, `slot_report`, `INFO`: about 75-250 ms at
  this map size, MEASURED). Total final-stage time rises slightly from
  interleaving and per-unit overhead.
- S2: the window falls toward the floor plus ρ·`W_window`.
- S3: the floor falls to seal plus walks plus scan (a few hundred ms at
  100K).

## 15. Rollback

- **S1.** A node option `--move-units on|off` selects the executor; `off`
  is today's single task. Both write the same records in the same order
  (test B), so switching, even mid-move across a restart (test R), changes
  no user data and no durable format. Rollback is a restart with `off`.
- **S2.** A driver option `--converge-rounds 0` makes the driver behave as
  today. It is driver-only, so no node state is involved.
- **S3.** A driver option `--stream off`, or any old node (no `CAPS`),
  uses the S1/S2 verbs. Nodes keep serving the old verbs. The one durable
  addition (`kSlotCounted`) is a flag older code ignores. A sealed slot
  counted by S3 is exported by the old path, which counts again itself.
- **A.1.** The guard is read only by new nodes. Rolling back the encoder
  (writing `v1`) restores today's behavior. Once any node depends on
  `moved` epochs, nodes must not be rolled back while `v2` placements are
  live, so A.1 has its own rollout section when it is designed.

**In every case the move protocol, the placement states and the commit
rule are unchanged.** That is why each rollback returns to the
whole-stage path without touching user data.

## 16. Conflicts with CORE_CONTRACTS, FAILURE_MODEL and the migration invariants

| Rule | Relation | Note |
| --- | --- | --- |
| CORE_CONTRACTS B-1 ("chunked and resumable, progress durable where it must survive a crash") | **REFINE** | Chunked and resumable, yes. Progress is durable only as the staged records themselves; no manifest (section 4). B-1's wording allows this ("where it must"), but it should be read that way explicitly |
| B-2, B-3 (HEALTH, option B) | KEEP | Untouched. S1 makes `stall_ms` during a move small, but B-3 still forbids acting on it; a future wedge rule needs its own evidence (B-3 text) |
| B-4 (no timeout tuning to hide B-1) | KEEP | No timeout changes. The driver's 15 s `ask` timeout is not raised; D4 is fixed by single flight instead |
| E9 (promotion and moves in progress) | REFINE: UNPROVEN to tested | tests D and F |
| E1, E2, E3, FAILURE_MODEL C7 / R08 | **CONFLICT, characterized (D1, D7)** | "Partial staging never serves" holds only where acknowledged STAGED cannot be lost: sync, plus the witnesses' degraded-refusal. Async witness promotion or admin `PROMOTE` breaks it (D1), and on the source side lets a promoted A serve after the seal (D7). A.1 closes both on the migration side; R08 remains its own slice |
| FAILURE_MODEL W1 (one unit monopolizes a shard thread) | stays **KNOWN ISSUE** | moves to COVERED for the final stage only when S1 ships with test A; the other inventory items stay as listed |
| MIGRATION.md "STAGED is written after every staged record in the shard's stream" | KEEP | S1's stage unit writes it last; drops precede it |
| MIGRATION.md "final stage is all or nothing for the STAGED record" | KEEP | all or nothing for STAGED; the puts were never all or nothing (an `E_NO_MEMORY` mid-put leaves puts today too) |
| MIGRATION.md "late requests are refused" | KEEP, strengthened | plus single flight and G-chain |
| MIGRATION.md Next: "Chunked migration stream" (scratch area, BEGIN/CHUNK/VERIFY/STAGE; accepted 2026-10-13) | **RECONSIDER** (decision 5) | A scratch area is unnecessary because staged objects are already inert in a foreign slot. VERIFY becomes the destination's bounded walk against A's seal. The stream's transport independence (invariant 10) is kept by S3 being verbs over the same admin transport, replaceable later |
| Invariant 11 (failover is not migration), 12 (epoch and term independent) | KEEP | G-fence uses the term; G-map uses the map; neither stands in for the other |
| Invariant 14 (hot path reads cached control state) | KEEP | guards read shard fields; `PLACEMENT SYNC` once per request |
| Invariant 16 (nothing resumes before ...) | KEEP | resume only at activation, as today |
| Invariant 17 (map adopted node-wide before replay) | KEEP | `set_map` and `map_changed` stay two `run_on_all` passes; units interleave with them as any task does |
| Invariant 18 (duplicate resolved before refusal) | not affected | client path untouched |

## 17. Smallest implementation change (S1)

**Scope:** the destination's final stage only.

**Not in scope:**

- the copy ahead, A's seal and export, HAVE, `slot_report` (D3), and
  activation scans;
- any wire, format or placement change;
- the index;
- R08 and R11, and D1;
- trace rings.

**Changes:**

1. **`include/snc/local_map.h`:** `generation()`, bumped by `rehash`; and
   `walk(cursor, budget, fn) -> cursor`, run-aligned (3.4). There is no
   change to `find`, `upsert` or `erase` beyond the generation increment
   in `rehash`.
2. **`src/core/shard.{h,cpp}`:**
   - split `stage_slot` final into `stage_admit`, `stage_unit(chain&)` and
     the decide/stage tail, sharing today's check and put code (no logic
     fork);
   - a per-slot chain registry, `stage_chain_[slot]`, holding the token;
   - a per-slot mutation counter (`u32` per slot, bumped in `put_image`
     and `drop_object`, which only staging calls);
   - the G-* guards.

   Non-final stages keep today's single task. That is **not** bounded:
   its size is the driver's `chunk`, a client parameter (`chunk 100000`
   reproduces the 4.8 s task). S1 adds a node-side cap: a non-final
   request above `B_unit` of payload is refused `E_BAD_ARGS`, so the
   driver must chunk.
3. **`src/core/engine.cpp`:** `Engine::stage_slot(final)` loops
   `on_shard(stage_unit)` until done or refused. It holds `W_stage` with
   `wait_lsn`, then waits for STAGED as today. Options: `move_units`,
   `move_unit_us`, `move_unit_bytes`, `move_stage_window_bytes`.
4. **`tools/migrate.h` (driver):** on `E_OVERLOADED` or a timeout of the
   final STAGE, back off (1 s, doubling to 8 s) and resend the same STAGE
   line, without HAVE or EXPORT, until a different answer. A cap on
   non-final `chunk` matching the node's.
5. **`tools/snc_server.cpp`:**
   - `--move-units on|off` (default on once tests pass);
   - `--move-unit-us`;
   - `DEBUG MOVE-STOP` behind `--debug-admin`;
   - traces `stage_unit` (phase, objects, bytes, ms, guard) and
     `slot_active`.
6. **Tests:** A, B, C, D, G, I, J, L, M, N, R in this slice. E, F, H, K,
   O, P and Q are extensions of `test_migrate` scenarios and are also in
   the slice if they fit; S belongs to S2 and T to A.1.
7. **Docs:**
   - MIGRATION.md (final copy executed as a chain);
   - CORE_CONTRACTS B-1 note and E9;
   - FAILURE_MODEL W1;
   - TRACING;
   - STATUS;
   - EVIDENCE;
   - AGENTS handoff.

**Rough size (INFERRED):** ~300-400 lines of code and ~500 of tests.

**What S1 delivers.**

- No final-stage unit holds B's shard for more than `T_unit` + one
  object.
- Fencing interrupts the final stage within one unit.
- Duplicate finals no longer queue, and a waiting driver no longer loops
  on HAVE and EXPORT (D4).
- E9 becomes tested.

**What it does not deliver.**

- No shorter E_MIGRATING window.
- No memory bound.
- No bound on the other O(map) tasks: seal, export, HAVE, `slot_report`
  after each STAGE (D3), `INFO`, activation. Each is about 75-300 ms at a
  150K map (MEASURED), so B's longest loop iteration during a move stays
  at that level. D3 could join S1 if the reply-content change is accepted
  (A.3).
- No D1 fix.

These belong to S2, S3, A.1 and later slices, each brought back for
review first.

## Appendix A. Adjacent defects (characterized, not fixed)

### A.1 D1: fail-open serving of a moved slot after a lossy promotion (INFERRED from code, high severity, conditional)

> Superseded by [MIGRATION_FAIL_CLOSED.md](MIGRATION_FAIL_CLOSED.md), which
> found variants D1b-D1d, replaces the `moved` epoch below with
> `arrived = (move_id, epoch)`, and adds a start barrier and a commit gate
> for D7.

**The serving rule.** `serving(S)` is "the map gives S to this group and
the phase ≤ 1" (`shard.h:47`). A group that has never held slot S has no
slot-state object for it, so its phase is NONE (0).

**The failing sequence.**

1. B's primary acks STAGED as LOCAL_DURABLE, under async replication or
   an admin `PROMOTE` (E3).
2. The commit gives S to B.
3. B's primary is lost, and a standby lacking STAGED and some staged
   objects is promoted.
4. That standby's phase for S is NONE and the map gives S to B, so **it
   serves S with a partial or empty copy**.

**The effect.** Clients see `E_NOT_FOUND` for missing objects, and the
slot is open to new writes over an incomplete image. That is silent loss
of a whole slot, not of a write tail. `activate_staged` is not involved.

**Where it cannot happen.**

- Sync and healthy: STAGED is HA_DURABLE.
- Sync and degraded: witness promotion is refused (an admin `PROMOTE`
  is not, E3).

So D1 is a consequence of R08/E3. It is reported here because it breaks
this work order's "never serves" requirement through failover.

**Proposed guard (design sketch, decision 3).**

- The placement records, per slot, the epoch of the commit that last
  changed its group (`moved`).
- A node serves S with phase NONE only if `moved[S] == 0`; otherwise it
  requires ACTIVE with `epoch ≥ moved[S]`.
- A slot failing that is reported in `INFO` and `STATUS` as
  `unstaged-owner`, and answers `E_MIGRATING`. Data is never invented;
  repair is an operator action (re-run the move from a surviving copy).
- Placement encoding v2 (section 12).
- Test T.
- **D7 (source side).** The same guard does not cover a promoted A that
  lost SEALED, because S was never moved away from A, and A cannot tell
  locally that it lost a seal. The candidate rule: the driver re-checks
  that A's current primary reports SEALED(M), durable, immediately before
  the commit CAS, and does not commit otherwise. That narrows D7 to a
  promotion between that check and the commit; closing it fully needs
  R08's eligibility rule. To be designed with A.1.

### A.2 D2: the lab's E_MIGRATING window metric undercounts (MEASURED)

`Probe.window()` (`bench/lab/lab.py`) multiplies unserved rounds by
20 ms. In p5-131933 the rounds took ~0.5 s each: 129 rounds, because
`INFO` waits for an O(map) task on every node (D6). It reported 0.18 s
for a window of about 6.75 s. **The figure should not be quoted.**
Fix: time-stamp each round and sum real intervals.

### A.3 D3: every STAGE, SEAL and ACTIVATE reply runs a full `slot_report` (CODE; cost INFERRED)

`Engine::stage_slot` calls `slot_report` after `wait_lsn`
(`engine.cpp:887`), refusals included; SEAL, UNSEAL and ACTIVATE replies
also carry one. It walks the shard map and CRCs every object of the
slot for `live_count` and `live_digest`. That is about 75 ms on B per
copy-ahead chunk, and its time is reported inside the `wait_lsn_ms` trace
field.

**Likely the larger part of the 44 s copy ahead.** Only
`tests/test_migrate.cpp` reads `live_count`, and it reads it from `INFO`.
Replies to STAGE, SEAL and ACTIVATE could report the slot state without
the live walk. That would be a reply-content change (driver and tests
audited first), so it is not in S1.

### A.4 D4: a driver timeout during a long final stage queues duplicates (INFERRED)

`ha::ask` gives up after 15 s (`migrate.h:483`). The handler's task keeps
running, and the driver retries from the top: `ROLE`, HAVE at B (an
O(map) task queued behind the running stage), EXPORT final at A (O(map),
12.6 MB at 100K), then STAGE, which gets an O(1) OK once the first task
has written STAGED. So one driver adds O(map) tasks every timeout; two
concurrent drivers can queue two whole final stages. S1's single flight
plus the driver's backoff-and-resend (section 17) fixes both.

### A.5 D5: slot size limit (INFERRED)

A `MOVE` request line must arrive within `kReadMs` = 500 ms of accept
(`admin_loop.h:44`, `:257`, `finish_line` at `:61`). At the measured
loopback read rate (12.6 MB in 36.7 ms, ~340 MB/s) that caps a line at
~170 MB, and at ~60 MB on 1 GbE; payloads are hex, so the payload limit
is about half of that. The driver's reply reader has its own 2^28-byte
cap (`ha.h:62`). The first copy-ahead EXPORT reply carries every payload
(45.4 MB at 100K, MEASURED), so it reaches the limit first. Beyond it the
move retries until its deadline. S3 removes the limit.

### A.6 D6: probe and `INFO` load (MEASURED counts, INFERRED share)

Each `INFO` is an O(map) task: n2, a standby with only `INFO` traffic, ran
129 injected tasks of ≥ 20 ms, p50 172 ms. The lab probe asks every node
every round. During p5-131933 that is ~21-24 s of shard time on each node
of group 0 over the run. Baselines taken with the probe on overstate
every migration phase on those nodes. Section 14 adds probe-off runs.
