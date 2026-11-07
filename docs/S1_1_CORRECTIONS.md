# S1.1: corrections to S1 admission (for review)

Work order: `docs/handoff/PR72_S1_Review.txt`, sections 2-13. S1 itself
is described in [S1_ADMISSION.md](S1_ADMISSION.md); this note covers only
what S1.1 changes and proves. No S2 work, no R640 load.

## 0. Summary

| Blocker (review) | S1.1 |
|---|---|
| 1. The WAL reservation was not worst-case | **Option B:** `max_size` is a hard cap on every op's object. The S1 shortfall also hid a **pre-existing recovery bug**, now closed (section 1) |
| 2. Remote-read pending slots and REMOTE waiters were unbounded | Origin **pending budget** (`refused_pending`); every unreserved answer that waits, at the owner or locally, takes a waiter under the **waiter budget** (section 2) |
| 3. A6 could pass on a partial send | A6 asserts every count exactly (section 4) |
| 4. q4's drain measurement was ambiguous | A coherent, preregistered rule on published gauges, plus a self-test (section 5) |
| 5. The non-refusable WAL bound was average-record math | The real bound, stated, with its next architectural limit (section 3) |

Unchanged:
- `kEmitBacklog` = 1024 and `E_OVERLOADED` on the wire;
- the loop order, and the checkpoint, recovery and WAL format code;
- CC-1..CC-5 (section 7).

The wire protocol is not changed either. Refusals are additive, with the
existing `E_OVERLOADED`.

## 1. Reservation: option B, a hard cap at `max_size`

**The defect.**
- `prepare_ctx` set `reserved_len_ = obj->capacity`, the allocator's
  rounded-up size.
- So `OpCtx::resize` let an op grow its object past `type.max_size`
  without `reserve()`, up to the capacity.
- S1 reserved `record_size(max_size)`, so such an op appended more than its
  reservation.

**Why option A (reserve the capacity) is unsound.** Every image loader
rejects a payload larger than `max_size`:
- `Shard::install` (`shard.cpp`, used by checkpoint load, WAL replay and
  standby apply) aborts with "invalid object image";
- migration install refuses it (`E_BAD_ARGS`).

An object past `max_size` is therefore not a legal state. Reserving for it
would have admitted a write that recovery cannot read back.

**This was reachable before S1, with no test object.**
- `Account.xfer_credit` reserves only while the account has fewer than
  `kMaxEntries` (1,024) entries (`account.cpp`, `credit_validate`), then
  checks `reserved_len()` in apply.
- A full account is at exactly `max_size` (32,968 B), and its block's
  capacity is larger (40,912 B here). The capacity passed that check, so
  the next credit grew the account past `max_size`.
- A restart then aborted.
- A15 with the S1 line restored reproduces it: `snc recovery: invalid
  object image ... (type 256)`. The log is
  `bench/results/s11-validation-devvm-20261107/negative/neg1b-A15.log`.

**The fix** (`shard.cpp`, `prepare_ctx`):

```cpp
c.reserved_len_ = obj->capacity < t->max_size ? obj->capacity : t->max_size;
```

- An op may still grow within the capacity without `reserve()`, up to
  `max_size`.
- Past it, `resize` is a handler contract violation, the same check as
  before ("resize beyond reserved length"). It aborts before anything is
  logged.
- `reserve()` already refused past `max_size` (`E_TOO_LARGE`).
- The account's full-inbox credit now answers "retry", as
  `credit_apply` intends ("A full or oversized account answers retry").

**Application semantics.**
- Only ops that would have written an unrecoverable image are affected.
  The registered types have one such path (above), and it now takes its
  intended retry branch.
- No op that stays within `max_size` behaves differently.
- This is the broader change the review asked to flag for extra review:
  **please review it as such.**

**The reservation is now worst-case, by construction:**
- **Object:** its length is at most `max_size` after any op. Every
  object starts at `initial_size` <= `max_size` (registry check), and
  every growth goes through `resize`, which is now capped. So the
  object's record is at most `record_size(max_size)`.
- **Local peer:** its length cannot change during the op (debug-checked
  in `run_exec`) and is at most its type's `max_size` by the same
  argument. So `peer_record_max_` (the largest type's record) bounds it.
- **Session:** `record_size(sizeof(SessionV1))`.
- **Create:** `record_size(max_size)` plus the session.
- **Delete:** `record_size(0)`.

`check_reserved` (debug builds) is now a proof check. A10-A12 drive each
term to its maximum:

| Test | Case | Appended | Reserved |
|---|---|---|---|
| A10 | Blob at `max_size` 33,000 with capacity 40,912 (the S1 shortfall case), with and without `reserve()` | 33,056 B, 1 record | 66,112 B |
| A10 | the same object, grown past `max_size` inside its capacity, in a child process | aborts at `resize`, before any record | n/a |
| A10 | restart with the object at `max_size` | recovers | n/a |
| A11 | sequenced create that grows to `max_size` in apply | 33,160 B, 2 records | 33,160 B (exact) |
| A11 | create past `max_size` | `E_TOO_LARGE`, 0 records | n/a |
| A12 | sequenced op: max object + max local peer + session | 66,216 B, 3 records | 66,216 B (exact) |
| A15 | account inbox at `kMaxEntries` while credits keep arriving | stops at 1,024 entries; the restart recovers | n/a |

With S1's line restored, A10 and A15 fail (`negative/neg1-*.log`).

## 2. Remote reads: a global bound

**Origin: the pending budget.**
- `Shard::pending_refuse` runs before `send_request` in all three client
  forward paths: `execute`, `create` and `remove`. `send_request` has no
  other caller. Follow-ups use `run_emits` and are not affected.
- When `pending_live() >= pending_budget`, the request is refused with
  `E_OVERLOADED` and counted in `refused_pending`. Nothing is created.
- A slot is freed by every answer:
  - `complete` and `complete_multi`;
  - `fail_pending` when a link is lost.

**Owner: every waiting answer is budgeted.**
- `RunOut::waiter_reserved` marks an answer whose waiter R1 already
  reserved: an admitted mutation, create or delete.
- Every other answer that must wait goes through
  `Shard::admit_answer_wait`:
  - a READ of an undurable object;
  - a not-found answer waiting on a delete;
  - one that arrived by forwarding.
- It takes `sizeof(Waiter)` plus the result under the waiter budget, or
  is refused there and then (`refused_waiter`). Refusing changes nothing,
  because the op made no change.
- It is called:
  - in `execute`, `create` and `remove` for local answers;
  - in `serve` and `serve_multi` for forwarded ones.
- A refused forwarded answer still goes back at once, so the origin's
  slot is freed.
- Replays (`replay_if_seen`) are exempt, as before S1. A client told
  `E_OVERLOADED` would send them again as new requests. They are bounded
  by the resends the clients' windows and the pending budget allow.

**The bound:**
- **Pending slots:** at most `pending_budget` per shard. A slot is 40 B
  (Completion 8, ctx 8, gen 4 plus 4 padding, target 16), and the vector
  may hold up to twice its live size. The default 131,072 is therefore at
  most about 10.5 MB per shard.
- **Waiters:** every waiter (LOCAL, REMOTE, ERASE) is created by a path
  that is either budgeted (R1, or `admit_answer_wait`) or bounded by its
  own source:
  - replays, as above;
  - session-expiry tombstones: one per expired session.

  Waiter bytes per shard are therefore at most `waiter_budget_bytes`,
  plus one op (an empty list admits), plus those two.
- **System-wide:** at most N × `pending_budget` slots, and N × (budget +
  one op) waiter bytes.

**No deadlock.**
- Answers are sent with `must_deliver`, and nothing at the owner waits on
  the origin.
- A refused request holds nothing.
- The pending budget does not apply to answers or follow-ups. So a full
  pending pool drains as its answers come back.

**Default derivation (provisional, as in S1 section 4):**
- At R_2, about 7/8 of a shard's 54,600 ops/s are forwarded with 8
  shards: about 47,800/s.
- If every forwarded op waited for the whole measured stall (1.26 s),
  about 60,200 would be pending. 131,072 gives more than 2× margin.
- The owners' waiter budgets (about 123,000 balance-op waiters each)
  refuse first in practice.

**Tests:**
- **A13:**
  - Pending stops at the budget (8): 12, then 2 more, are refused at the
    origin.
  - The owner's REMOTE waiters stop at its waiter budget (3 reads beside
    2 deposits): 5, then 5 more, are refused there.
  - The causes and gauges are exact.
- **A14:** durability resumes.
  - The 3 held reads and 2 deposits are answered exactly once each, with
    no late or duplicate answer.
  - Pending and waiters return to 0.
  - 20 new forwarded reads are all admitted.

Negative checks:
- Without the owner's check in `serve` and `serve_multi`, A13 fails
  (`negative/neg2-A13.log`).
- Without the pending budget, A13 fails (`negative/neg3-A13.log`).

## 3. The real WAL memory bound (non-refusable work)

The client budget is **not** a memory bound. `wal_buffer_budget_bytes`
(30 MiB) bounds only what client admission adds: an op is admitted while
active + its worst case <= budget, or when the buffer is empty.

Work that is never refused appends on top of it:

| Source | Per item | How many while this shard's WAL stalls |
|---|---|---|
| Follow-up apply (`run_emit`, `client = false`) | one record, at most `record_size(max_size)`, plus a local peer's (at most `peer_record_max_`) | see below: **not bounded by this shard** |
| Session expiry | one DELETE record (56 B) | sessions past their TTL |
| Slot seal and unseal, migration install | small records; installs bounded by `move_window_bytes` | the move in progress |
| Recovery replay, standby apply | not client-facing; replay runs before serving, and a standby has no clients | n/a |

**Follow-ups are the large term, and they are not bounded today.**
- A sender's follow-up gate counts only its unsent backlog
  (`emits_in_flight` = queued + waiting on the sender's own durability).
  A sent follow-up leaves that count.
- So while a destination's WAL stalls and the senders' WALs are healthy,
  the senders keep admitting and sending. The destination applies every
  follow-up (never refused) and logs it.
- For accounts, every credit corresponds to an open outbox entry at its
  sender, and the destination's settles wait on its own WAL. So the
  credits a stalled destination can receive are at most the open entries
  that target it: 64 (`kMaxOutbox`) × the source accounts.
- Each is logged at up to the account's `record_size(max_size)` =
  33,024 B. A full account's credits are logged as "retry" at that size.
- With a million accounts, that bound is not useful: about 2 TB.
- In practice the bound is the senders' rate times the stall. At R_2 one
  destination shard receives about 5,500 transfers/s; over 1.26 s that is
  about 6,900 credits. That is about 2 MB with typical records and up to
  228 MB if every record were worst-case.

**A15 characterizes it on the dev VM:**
- Two sender shards fill their gates (1,025 each: the gate plus one op's
  single credit) with transfers to one account. That account's shard
  then stalls, and the credits are released.
- The destination applies all 2,050 credits: 2,050 records, 50.9 MB,
  **1.6× the client budget**, from two senders and one account. That is
  within the stated bound of 2,050 × 33,024 B.
- The inbox stops at 1,024 entries (the S1.1 cap). Then everything
  settles, money is conserved, and the restart recovers.
- The count matches the senders' gates only because the test held the
  credits unsent until the stall began. Without that hold the senders
  keep sending, as above.

**The next architectural bound** (not built; it needs its own design and
review):
- **Follow-up intake backpressure.** When a shard's active WAL passes a
  hard cap above the client budget, it stops taking follow-up messages
  off its rings. Unconsumed follow-ups stay with their senders as unsent
  emits, which their gates already count.
- The system-wide bound then becomes N senders × (1,024 + one op's
  emits) × the largest follow-up record.
- This touches ring draining, so it is a loop change, which S1 excludes.
  It also needs answers and requests separated from follow-ups in the
  drain. Proposed as a later slice, before S6.

**The envelope per shard, stated honestly:**
- client WAL: at most 30 MiB plus one worst-case op;
- internal follow-up WAL: unbounded structurally, rate-bounded as above;
- waiters: at most 32 MiB plus one op, plus replays;
- pending: at most about 10.5 MB.

`S1_ADMISSION.md` section 4.3 is corrected to point here.

## 4. A6, tightened

A6 now fails unless every step happened exactly:
1. All 400 transfers return OK. There is no partial-send pass.
2. Shard 0 emits exactly 400 credits.
3. Shard 1's client admission is closed (2 deposits refused,
   `refused_wal_buffer`).
4. Shard 0 sends exactly 400 and shard 1 receives exactly 400.
5. Shard 1 logs exactly 400 credit records.
6. Its active buffer passes the client budget (269,312 B > 66,380 B).
7. A new client deposit is still refused.
8. Once durability resumes: 0 follow-ups in flight, all money conserved,
   and the destinations hold exactly 16 × 1,000 + 400 + 2.

## 5. q4 measurement (harness H1)

**The defect:**
- `test_baseline_ha` q4 judged "drained" from one RESOURCES sample while
  the load ran.
- RESOURCES sums `eng.stats(s)` one shard at a time, which is not a
  snapshot, so a sum can exceed the clients' window.

**What the corrected measurement found:**
- Under a closed-loop load bounded by sync speed, nearly every client slot
  waits for the next local sync at any instant.
- The q4_normal gauges show a median of 237-254 waiting answers against a
  window of 256, both when drained and when blocked.
- So "few answers wait" was never the right meaning of drained, and the
  old pass was luck of sampling.

**The rule** (preregistered in `tests/test_baseline_ha.py`, `drain_rule`):
- **Source:** published gauges (GAUGES verb, S0). They are read without a
  shard task, and each shard's block is one consistent copy.
- **Per shard,** in that one block, `excess = max(0, waiters_now -
  wal_undurable)`: answers held for something other than this shard's
  own WAL. Each q4 deposit is one record with one answer. So drained
  means excess is 0, and blocked on the standby means excess is near the
  window (the answers wait on records already durable here).
- **Drained**, over the last 5 s with at least 10 consistent samples
  (the sampler runs about 10/s in Release, at least 2/s under sanitizers):
  - the median excess is 0;
  - at most 20% of samples show any excess;
  - the durable LSN advanced on **every shard that had waiting answers**
    in the interval. A shard whose own sync stalls shows no excess, and a
    summed LSN would let the other shards hide it (independent review).
- **Bounded:**
  - the summed undurable median is <= the window;
  - it is not growing: the last-half mean is <= the first-half mean plus
    10% of the window.
- One sample is never a verdict.

**Self-test (`h1_measurement`, in ctest, no server):**
- The sequential sum reads 286 while the true total is 256.
- The old rule fails a drained series whose last sample is 270, the
  failed ASan run's shape.
- The rule passes that series, and fails blocked, stalled, growing,
  one-shard-stalled and too-short series. (The check caught its own
  fixture once: the stalled series pinned only the summed LSN until it
  was made per shard.)
- It feeds hand-made series only; the waiters-versus-undurable premise is
  checked on the real runs below, not here.

**On real runs (Release, `q4-rule-check/`):**
- **q4_normal:** drained (excess 0 in 48/48 samples), bounded.
- **q4_delayed10, while blocked:** 93 samples, median excess 256 and
  excess in 98.9% of them, so **not drained**. After the grant: excess 0
  in 47/47, drained, bounded.

The rule therefore separates the two states on real data. The old
single-sample values are still recorded (`end_sample_*`).

## 6. Tests

| Test | Review item | Result |
|---|---|---|
| A6 | 8 | tightened (section 4) |
| A10 | A10 size-class boundary | section 1 |
| A11 | A11 create boundary | section 1 |
| A12 | A12 local peer + session | section 1, exact |
| A13 | A13 remote-read bound | section 2 |
| A14 | A14 remote-read recovery | section 2 |
| A15 | A15 internal overshoot | section 3 |
| H1 | coherent q4 measurement | section 5 |

A1-A9 are unchanged and pass.

Review follow-ups in the tests:
- A10 forks its abort child only after checking that no engine thread
  survived (`thread_count()`, allowing TSan's own background thread), so
  TSan's die_after_fork cannot turn the expected SIGABRT into exit 66.
- A2 and A13 size exact waiter budgets from a 256-byte `Waiter`;
  `shard.cpp` now `static_assert`s that size (outside `_GLIBCXX_DEBUG`),
  so a layout change breaks the build instead of shifting the counts.

Gauge layout note: G_REFUSED_PENDING, G_PENDING_NOW and G_PENDING_PEAK
were inserted before G_PUBLISHED_NS, shifting its index. Every reader in
the repo goes by name (`kGaugeNames`, the GAUGES verb); a binary reading
the block by index must be rebuilt with this one.

`pending_budget = 0` refuses every forwarded request (the CLI rejects 0;
the library accepts it, like the other budgets). `test_s1_admission A10 A15` runs only the
named tests; the negative logs used that.

## 7. CC-1..CC-5, loop, checkpoint

- **CC-1..CC-5: not changed.**
  - No checkpoint, recovery, WAL format, flush, sync or reclamation code
    changed.
  - Recovery's `max_size` check is unchanged. S1.1 makes the runtime obey
    it, so every image written can be read back. That strengthens CC-2
    (redo completeness) in practice, without changing the contract.
- **The loop order is not changed.**
- **The only other runtime change** is the `prepare_ctx` cap (section 1).

## 8. Defaults (review section 7)

- 32 MiB of waiters and 30 MiB of WAL admission are **provisional**, not
  validated production defaults. So is the pending budget of 131,072.
- They are to be revisited with physical evidence after S2-S4 and before
  S6.
- The 1 GB/s drain figure in `S1_ADMISSION.md` section 4 is
  **hypothetical** (not measured).
