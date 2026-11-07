# S1: resource-specific admission (for review)

*Authorized by `docs/handoff/PR71_Harness_Review.txt` sections 6-15.
Design: `docs/CHECKPOINT_BACKPRESSURE_DESIGN.md` section 6 (approved
direction A). Slice S1 only. S2 has not started.*

## 0. Summary

Before S1, one gate refused every client `execute` with E_OVERLOADED while
the shard's follow-up backlog was above `kEmitBacklog` (1,024). Reads were
refused too, and creates and deletes had no gate at all.

S1 makes admission resource-specific:
- **Owner-local READs** are no longer refused for the follow-up backlog.
  A READ whose answer must wait for durability is bounded by the waiter
  budget instead.
- **New mutations, creates and deletes** are gated by the follow-up
  backlog. Before S1, creates and deletes were not gated.
- **Client mutations, creates and deletes** also pass the R1 check, after
  validation and before their first change. R1 refuses when the op's
  worst-case WAL bytes would exceed the WAL buffer budget, or its waiter
  bytes the waiter budget.
- **Unchanged:**
  - `kEmitBacklog` stays 1,024.
  - The wire status of every refusal stays E_OVERLOADED.
  - The cause is counted in `refused_followup`, `refused_ring`,
    `refused_waiter` or `refused_wal_buffer` (ShardStats and the S0
    gauges).
  - Follow-ups, standby apply, recovery and migration are never refused.
- **Rollback:** `--admission legacy` (`Engine::Config::admission_legacy`)
  restores the pre-S1 gate exactly.

**Not changed:**
- the loop order (`runtime.cpp` iteration and `Shard::poll`);
- the checkpoint (walk, writes, sync, commit, reclamation);
- recovery;
- the WAL format and its flush policy;
- `kEmitBacklog`;
- the protocol status codes.

CC-1..CC-5 (`docs/CORE_CONTRACTS.md` 13A) are not touched (section 7).
There is no foreground time quantum: the review keeps scheduling out of
S1 (section 11 there). No R640 run.

## 1. Code

| Where | What |
|---|---|
| `src/core/shard.cpp` `Shard::execute` | follow-up gate skipped for owner-local READs; after the run, an owner-local READ that would wait passes `admit(0, waiter)` |
| `Shard::create`, `Shard::remove` | `followups_refuse`: the follow-up gate, with the sequenced-resend exemption (a resend already applied is answered) |
| `Shard::admit` | R1: a pure check, returns false and counts the cause on refusal |
| `Shard::check_reserved` | debug builds: the admitted op appended no more than reserved and no flush ran inside it |
| `Shard::run_exec` | R1 after `validate`, before `apply`, for client (not follow-up) logged mutations |
| `Shard::run_create` | R1 after the type and argument checks, before the object is allocated and inserted |
| `Shard::run_delete` | R1 after the delete op's `validate`, before its `apply` and the index erase |
| `Shard::set_admission`, `Engine::Config` | `admission_legacy`, `waiter_budget_bytes`, `wal_buffer_budget_bytes` |
| `tools/snc_server.cpp` | `--admission s1\|legacy`, `--waiter-budget-bytes N`, `--wal-buffer-budget-bytes N` |
| `Engine::debug_hold_wal` | tests only: a shard starts no WAL flush while held, so admission can be filled deterministically |

## 2. Admission matrix

| Class | Before S1 | S1 | Bounded by |
|---|---|---|---|
| READ, owner shard, no wait (object durable, or `allow_undurable`) | follow-up gate | **admitted** | CPU; the connection's 8 MiB output limit |
| READ, owner shard, object not yet durable | follow-up gate | **waiter budget** (`refused_waiter`) | waiter bytes |
| READ forwarded to another shard | follow-up gate at the origin | follow-up gate at the origin (**unchanged**) | the origin's ring and overflow (`refused_ring`). The pending slot is not otherwise bounded, so the gate stays |
| READ served for another shard | never refused at the owner | never refused at the owner | **not bounded** (known gap, section 9): the REMOTE waiter it may add is not refused, and the origin's pending slots are unbounded |
| MUTATE, client (local or forwarded) | follow-up gate at the origin | follow-up gate at the origin, **plus R1 at the owner** (WAL, then waiter) | backlog, WAL buffer bytes, waiter bytes |
| Resent sequenced MUTATE already applied | answered from its session | unchanged (answered before any gate) | n/a |
| CREATE, client | **no gate** | follow-up gate, **plus R1** | as MUTATE |
| Resent sequenced CREATE already applied | answered | answered (checked first in `followups_refuse`) | n/a |
| DELETE, client | **no gate** | follow-up gate, **plus R1** (one DELETE record; the tombstone's ERASE waiter and the answer's waiter) | as MUTATE |
| Unlogged MUTATE (`OpDescriptor::unlogged`) | follow-up gate | follow-up gate; no R1 (no record, no waiter) | backlog |
| Follow-up apply (`run_emit`, `client = false`) | never refused | never refused | the senders' follow-up gate |
| Standby apply, recovery replay, migration install, slot seal and unseal | never refused | never refused (they do not pass through `admit`) | their own protocols |
| Session expiry (`expire_sessions`, at checkpoint start) | never refused | never refused: its deletes call `run_delete` with `internal = true`, which skips `admit` (test A9) | the TTL |
| INDEX lookup and gather | gather pool and ring | unchanged | gather pool, ring |

Where several checks apply, the first that refuses names the cause, in
this order:
1. the follow-up gate (at the origin);
2. the WAL buffer;
3. waiters.

`admit` checks the WAL before waiters. An op is counted once.

Two cases always admit, so a budget smaller than one op cannot block a
shard for good:
- an empty active WAL buffer admits the WAL check;
- an empty waiter list admits the waiter check.

## 3. Reserve before mutate (proof)

**Claim.** A client op that S1 refuses has changed nothing:
- not the object, its version or its index entry;
- not a session;
- not the WAL;
- not the follow-up queue.

An admitted op appends at most its reservation.

**R1: placement.** Each `admit` call sits after the op's last read-only
check and before its first change:
- `run_exec` (MUTATE):
  - Before it run the standby, withdrawn and session-replay checks, slot
    serving, `map_.find`, the op, type and argument checks, the expected
    version, and `validate_checked`. In debug builds that last call
    hashes the object to prove `validate` did not modify it.
  - The first change is `apply`, called right after `admit`, with
    `c.applying_ = true`.
  - Unlogged ops and follow-ups (`client == false`) skip R1.
- `run_create`:
  - Before it run the replay check, slot serving, the type and argument
    checks, and the duplicate-ID abort.
  - The first change is the allocation and `map_.upsert`, after `admit`.
  - The minted ID is consumed whether or not the op is admitted. IDs are
    unique and never reused, so skipping one is allowed (C12, as
    `Shard::create` already does).
- `run_delete`:
  - Before it run slot serving, `map_.find`, the expected version, and
    the delete op's `validate_checked`.
  - The first change is the delete op's `apply` or the index erase,
    after `admit`.
  - Session expiry calls it with `internal = true`: no `admit`, no
    `check_reserved` (section 5).
- Owner-local READ: a READ changes nothing (`validate` cannot, and a READ
  op's `apply` only writes its result). So refusing it after it ran is the
  same as refusing it before. Only the answer would have needed a waiter.
- `admit` itself only reads counters and, on refusal, increments
  `stats_` counters and sets `out.r.status`.

**R2: the worst case.** Every record the call appends synchronously is
counted:
- **Object:** `record_size(type.max_size)`. `OpCtx::reserve` refuses
  growth past `max_size`, but `prepare_ctx` sets the reservation to the
  object's allocated `capacity`, which the allocator rounds up. So
  `OpCtx::resize` can, without a `reserve` call, grow the object past
  `max_size` up to its capacity. This is pre-existing behaviour, not
  changed by S1. The bytes past `max_size` are less than one allocator
  size class, and the local-peer term below (the largest record of any
  type, 33 KB) is reserved for every mutation, used or not. Debug builds
  check the total on every admitted op (`check_reserved`), and no test
  or suite has hit it. Making `max_size` a hard cap in `resize` is a
  later fix, outside S1.
- **Local peer:** `peer_record_max_`, the largest registered type's
  record. The peer's type is chosen inside `apply` (`OpCtx::local(id,
  type_id)`), and its length cannot change: `OpCtx::resize` acts only on
  the op's own object, and debug builds check the peer's length in
  `run_exec`. So its record is at most this.
- **Session:** `record_size(sizeof(SessionV1))` = 104 when the op is
  sequenced. `record_session` appends one CREATE or UPDATE.
- **Create:** the object's record plus the session's.
- **Delete:** `record_size(0)`.
- **Waiters** (`waiter_cost`): `sizeof(Waiter)` + the result. A LOCAL
  waiter copies the result; a REMOTE one holds the response, external when
  over 16 bytes. Both are at most `sizeof(Waiter) + op.max_result`.
  - A delete holds two waiters: the ERASE waiter, and the answer's waiter
    when it waits.
  - A create always waits.
  - A mutation waits only when DURABLE.
- **Not synchronous:** cross-shard effects are follow-ups, appended later
  on their target shard and never refused.

**R3: no yield between the check and the appends.** `admit` and every
append of the op happen inside one call on the shard thread:
- `apply` can call `local`, `emit`, `reserve` and `set_result`; none of
  them yields, waits for I/O or enters admission.
- Index maintenance and `record_session` are in-memory.
- `WalStream::append` copies into the active buffer and never starts a
  flush; flushes start only from `WalStream::poll` in `Shard::poll`, after
  the op has returned.
- So no other op can use the reservation in between, and nothing can
  remove bytes from the active buffer. The check is transient, with no
  held counter (allowed by the review, section 10, while this holds).
- **Checked, not only argued:** in debug builds (the ASan+UBSan and TSan
  suites), `check_reserved` runs after every admitted op's last append.
  It aborts if the active buffer shrank (a flush ran) or grew by more
  than the reservation.

**R4: appends never fail.** `ensure_capacity` grows the buffer. The budget
is an admission bound, not an append limit, so a refusal can only happen
at R1, where nothing has changed.

**R5: sessions stay idempotent.**
- The replay check runs before every gate: in `execute`'s follow-up path,
  in `followups_refuse`, and at the top of `run_exec` and `run_create`.
- A resend of an applied op is answered from its session.
- A new sequenced op refused at R1 has no session record, so its retry
  with the same sequence number runs once as a new op. Tests A2 and A4
  cover this.

**R6: nothing acknowledged is lost.** A refused op was never applied
(R1, R4). An admitted op is logged in full and answered after durability,
exactly as before.

## 4. Budget derivation (provisional; revisit before S6)

**Measured on the dev VM, this commit** (exact):

| Quantity | Value |
|---|---|
| `sizeof(Waiter)` | 256 B |
| Waiter of a balance op (deposit, transfer, get_balance) | 256 + 16 = 272 B |
| Waiter of `Account.get` | 256 + 200 = 456 B |
| Deposit record (an account with no entries) | 256 B (`record_size(200)`) |
| Account worst-case record | 33,024 B (`max_size` 32,968) |
| Session record | 104 B |
| R1 reservation: deposit or transfer | 66,048 B (object + peer); 66,152 B sequenced |
| Closed-loop dev-VM run (2 shards, depth 64, checkpoints every 2 s) | `waiters_peak` 103-107, `waiter_bytes_peak` 26-27 KB (256 B per waiter), `wal_active_bytes_peak` 25-28 KB, flush EWMA 240 µs, flush max 0.3 s |

**R640 inputs:** `docs/CHECKPOINT_BACKPRESSURE_DESIGN.md` section 2, from
`CAUSAL_REPORT.md`:
- the durability stall in a checkpoint's fdatasync lasts 1.13-1.26 s;
- durable capacity during it falls to about 0.35-0.57M ops/s;
- the pilot mix is read 70, deposit 15, transfer 10, index 5;
- 8 shards.

**Per-shard rates:**

| Total rate | ops/s per shard | mutations/s per shard |
|---|---|---|
| R_2 = 436,646 | 54,600 | 13,600 (25%) |
| 2,183,233 | 272,900 | 68,200 |

**WAL bytes per client mutation:**
- A deposit appends 1 record of 256 B.
- A transfer appends its debit (about 288 B: one OutEntry) here, plus a
  credit and a settle later. The later records are follow-ups, but they
  share the buffer.
- So the 15:10 mix averages 45 records per 25 mutations, about 490 B per
  client mutation.

**Rules for the defaults:**

1. **No new refusal source at R_2.** Take the worst case: nothing becomes
   durable for the whole measured stall (1.26 s), and every op waits.
   - Waiters: 54,600 × 1.26 × 272 B = 18.7 MB.
   - WAL: 13,600 × 1.26 × 490 B = 8.4 MB.
   - Defaults with at least 1.5× margin over those:
     - `waiter_budget_bytes` = **32 MiB** (about 123,000 balance-op
       waiters).
     - `wal_buffer_budget_bytes` = **30 MiB**. 30 rather than 32 keeps
       an active buffer at the budget plus one worst-case op (66 KB)
       inside its 32 MiB power-of-two capacity (`ensure_capacity`
       doubles).
   - At R_2, S1 removes the follow-up gate for owner-local reads, about
     70% of the traffic, and adds no budget refusal.
2. **Backpressure where the stall exceeds the budget.** At 2,183,233 the
   same stall queues up to 272,900 × 1.26 s ≈ 344,000 ops per shard
   (about 94 MB of waiters). The budgets refuse there, as the follow-up
   gate does today. That is the intended outcome: S2 to S4 address the
   stall, not S1.
3. **Memory envelope per shard:**
   - waiters: at most 32 MiB plus one op's worst case;
   - WAL: active and flushing buffers of at most 32 MiB each, while
     follow-ups do not overshoot by more than 2 MiB (see below);
   - total at most about 96 MiB per shard. For 8 shards that is 768 MiB,
     0.23% of the R640's 320 GB.
   - Follow-up overshoot: follow-ups are not budgeted, but each sender's
     backlog is at most 1,025. With 8 senders and typical 300 B records
     that is about 2.4 MB; with worst-case records it is more. The buffer
     then doubles to 64 MiB, which this envelope states and does not hide.
4. **Drain and tail latency:**
   - **WAL drain:** a full WAL budget is 30 MiB per shard, 240 MiB for 8
     shards on one device. The SAS SSD's sequential write rate is not yet
     measured; it is a hypothesis to measure in S6. At an assumed
     1 GB/s, the drain takes about 0.25 s after the stall ends.
   - **Waiters:** they drain as durability resumes. The longest wait an
     admitted op can see is about the stall plus the WAL drain. At R_2 a
     full waiter budget is 123,000 ops, about 2.25 s of that shard's
     traffic: an upper bound, never reached in the measured stall
     (18.7 MB < 32 MiB).
   - **Normal operation:** the dev-VM peaks are a thousandth of the
     budgets (27 KB against 32 MiB), so the budgets add no latency.

**Hypotheses to measure before the defaults freeze (S6, R640):**
- the SSD's write rate under WAL plus checkpoint load;
- `waiter_bytes_peak` and `wal_active_bytes_peak` (S0 gauges) during a
  checkpoint at R_2 and above;
- whether the 1.5× margin holds once S2 to S4 change the stall.

Tests use tiny explicit budgets (A2, A3, A6).

## 5. Internal work drains (no deadlock)

None of the paths that drain a constrained resource passes through
`admit` or the follow-up gate:
- WAL flush and completion: `WalStream::poll` and `flush_done`;
- `release_waiters`;
- follow-up dispatch and apply: `run_emits`, `serve_emit`, `run_emit`
  with `client = false`;
- standby apply, recovery and migration;
- session expiry: its deletes pass `internal = true` to `run_delete`,
  which skips `admit` (they are the shard's own work, counted in
  `sessions_expired` only when they succeed).

Client refusals do not consume these resources: a refused op adds no
record, waiter or follow-up. So a closed admission cannot keep the
backlog, the buffer or the waiters from draining. A6 shows follow-ups
applied on a shard whose WAL budget refuses clients, past that budget,
and everything settling once durability resumes. A9 shows the same for
session expiry.

## 6. Tests (`tests/test_s1_admission.cpp`)

| Test | Review section 12 item | What it shows |
|---|---|---|
| A1 | owner-local reads while backlog > 1,024; mutation and create refused | 20/20 local reads answered. Deposit, create, delete and a forwarded read are refused, all as `refused_followup` |
| A2 | waiter budget refuses before admission | Budget: 4 deposit waiters plus 260 B. 4 deposits admitted, 2 refused with no record and no version change. Also refused: an undurable read, a deposit forwarded from shard 1 (refused at the owner), a durable delete (2 waiters, 512 B), a sequenced create (no object, no session). An APPLIED delete (1 waiter, 256 B) is admitted, which shows the delete sizing. All refusals `refused_waiter`; the budget reopens after the release |
| A3 | WAL budget refuses before the first mutation; refused mutation leaves all state untouched | 4 admitted (1,024 B active). A deposit, a cross-shard transfer and a sequenced deposit are refused. No new record, version, object, session or follow-up; all `refused_wal_buffer` |
| A4 | sequenced retry survives refusal | the refused sequenced deposit and create each run once on retry (`replayed` false); a resend is replayed |
| A5 | local-peer + session + object sizing | a sequenced local transfer appends exactly 3 records, 616 B, within its 66,152 B reservation. Debug builds check every admitted op (`check_reserved`) |
| A6 | internal work drains while admission is closed | Shard 1's WAL held and its WAL budget refusing clients (`refused_wal_buffer`). 200 credits are applied anyway and grow the buffer far past the budget, so a gated follow-up would have been refused. All settle once durability resumes; money conserved |
| A7 | read flood does not starve backlog progress | shard 0's backlog of 1,025 drains while owner-local reads flood both shards; 0 reads refused |
| A8 | legacy switch restores old behaviour | local reads refused, create and delete not gated, as before S1. With 1-byte budgets and the WAL held, 8 deposits are all admitted: no `refused_waiter` or `refused_wal_buffer` |
| A9 | internal work: session expiry | 4 expired sessions are deleted at checkpoint start while the WAL budget refuses clients; 0 refusals counted. With expiry gated (the reviewed draft), the test fails |
| All | refused_by identifies the cause | every refusal is matched to exactly one `refused_*` counter, in ShardStats and in the gauges |

**The rest of the review's item 12:** the persistence, session,
transfer, replication, migration and crash suites run unchanged in all
three builds. Evidence: `bench/results/s1-validation-devvm-20261107/`.

## 7. CC-1..CC-5

**Not changed.** S1 touches no checkpoint, recovery, WAL format, flush,
sync or reclamation code:
- CC-1 coverage;
- CC-2 redo completeness;
- CC-3 no future state (C5);
- CC-4 idempotent redo;
- CC-5 atomic publication and reclamation.

The only WAL interaction is reading `active_bytes()`.

An admitted op logs exactly the records it logged before S1, in the same
order with the same group flags, so replay is unchanged. A refused op
logs nothing.

## 8. Loop and checkpoint unchanged

`runtime.cpp`'s iteration and `Shard::poll` keep their order:
- `reap_io` stays after the ring drain;
- `checkpoint_step` stays inside `Shard::poll`, before `app_poll`.

The only change to `Shard::poll` is the tests-only `debug_hold_wal`
check, which is off unless a test sets it. The checkpoint code is not
modified.

## 9. Known limits

- **Forwarded READs keep the follow-up gate.** Their origin's pending
  slots are not otherwise bounded, so removing the gate would remove
  their only bound. A pending-slot budget is a later step.
- **Known gap: a READ served for another shard is not bounded at the
  owner.** `serve` and `serve_multi` may add a REMOTE waiter for an
  undurable object, and the owner does not refuse it. The origin's
  `pending_` is unbounded (`emplace_back`), so the origins' gates do not
  bound it either: the follow-up gate counts the origin's follow-ups,
  not its pending reads. This is unchanged from before S1. Bounding it
  needs a pending-slot or remote-waiter budget, a protocol change for a
  later slice.
- **The local-peer reservation is the largest registered type** (33 KB
  for accounts), not the op's actual peer. Ops do not declare a peer
  type. This is conservative: it refuses at most 66 KB early in a 30 MiB
  budget.
- **The waiter budget counts `Waiter` and result bytes, not the deque's
  block overhead**, as the S0 gauge does.
- **The defaults are provisional (section 4).**
- **Under a backlog, a create near a moved object, and a delete of one,
  answer `E_OVERLOADED` before `E_MOVED`.** The follow-up gate in
  `create` and `remove` runs before the slot check, as in `execute`. A
  client retries either way; the retry gets `E_MOVED` once the backlog
  drains.
- **A refused owner-local READ still counts in `ops_local`**: it was
  routed and ran here. Its answer carries no result and version 0.

## 10. Independent review

An independent source review of the first draft found two major issues
and several minor ones. All are fixed or recorded here:

| Finding | Resolution |
|---|---|
| Session expiry deletes went through `admit`: they could be refused, counted as client refusals, and `sessions_expired` overcounted | `run_delete(..., internal = true)` skips `admit` and `check_reserved`; `sessions_expired` counts only deletes that succeed. Test A9 (fails on the draft) |
| A6 passed vacuously: follow-ups are APPLIED and need no waiter | A6 now closes shard 1 by its WAL budget with the WAL held, so each credit grows the buffer past the budget |
| Forwarded reads at the owner add unbounded REMOTE waiters; the design doc's "bounded by the origins' pending pools" was wrong | Recorded as a known gap (section 9); design doc 6.1 corrected |
| R2 said an object cannot pass `max_size`; `resize` can grow to the allocator-rounded capacity | R2 corrected (section 3); the peer slack covers it; a hard cap is a later fix |
| `followups_refuse` tried a replay for deletes, which are never sequenced | It replays only for creates; a refusal carries the target's ID where known |
| A2 refused the delete even at one waiter; A8 set no tiny budgets | A2's budget admits one waiter and refuses two, with an APPLIED delete admitted; A8 uses 1-byte budgets with the WAL held |
| `--waiter-budget-bytes` and `--wal-buffer-budget-bytes` accepted `-5` and overflow | Leading `-` and `ERANGE` are rejected |
| Nits: refused READ kept its version; hard-coded `record_size(48)`; redundant `wal_ ?` ternary; the debug hold bit aliases at 64 shards | Fixed, or documented in the code |
