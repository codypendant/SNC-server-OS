# S1.2: replay waiters take the waiter budget (for review)

Work order: `docs/handoff/PR73_S11_Review.txt`. S1 is in
[S1_ADMISSION.md](S1_ADMISSION.md) and S1.1 in
[S1_1_CORRECTIONS.md](S1_1_CORRECTIONS.md); this note covers only the
replay path. No S2 work and no R640 load. Loop order, checkpoint,
recovery, WAL format, `kEmitBacklog` = 1024, CC-1..CC-5 and the wire
format (no new field or status) are unchanged.

## 0. Summary

| Review item | Done | Evidence |
|---|---|---|
| 2: replay waiters bypass the budget | every replay answer passes waiter admission | A16, A18, A20; negative check |
| 3: bounded replay semantics | durable: answer at once; undurable: wait if admitted, else `E_OVERLOADED` marked `replayed`, nothing re-applied | A16, A17 |
| 3: "the client's retry remains safe" | **not true of the shipped client**: it was fixed (section 2) | A21; negative check |
| 4: `waiter_reserved` semantics | one meaning only; every assignment audited (section 3) | |
| 5, A19: remote replay | sequenced requests are never forwarded, so no REMOTE replay waiter exists (section 4) | A19 |
| 7: one-op exception | the bound is stated as budget + one admitted op everywhere (section 5) | A20 |
| 10: follow-up intake backpressure | recorded as a required pre-S6 slice (section 6) | STATUS |

## 1. The hole and the fix

`replay_if_seen` set `waiter_reserved = true` without any admission, and
three sites answered a replay straight through `complete_local`:
- `execute`, the follow-up-backlog path;
- `create`, the `near` path (before the foreign-slot check);
- `followups_refuse`, for a resent create.

The other two sites, `run_exec` and `run_create`, return to `execute` or
`create`, whose `admit_answer_wait` skipped the budget because the flag
was set. So with the session record undurable, every resend of an applied
request added a waiter outside the budget. Many client IDs, each with an
undurable session (an `APPLIED` original needs no waiter of its own),
could hold any number of them. A20 builds exactly that case.

**Fix (`src/core/shard.cpp`):**
- `replay_if_seen` no longer touches `waiter_reserved`.
- The three direct sites call `admit_answer_wait` before
  `complete_local`, as the other two already do.
- So a replay is answered as follows:
  - **record durable:** answered at once, with no waiter;
  - **undurable, room in the budget:** it waits, and is answered once
    the record is durable;
  - **undurable, no room:** `E_OVERLOADED` with `replayed` set. It counts
    under `refused_waiter`, like any other waiter refusal, and also under
    the new `refused_replay` (STATS and the GAUGES gauge
    `refused_replay`; a subset, so not in the `overloaded` sum).
- `stats.replayed` still counts every duplicate detected, refused or
  not.
- A refused replay changes nothing. `replay_if_seen` only reads the
  session, and `admit_answer_wait` only reads the budget. A16, A18 and
  A20 check this: no WAL record, the same version, the same balance.

## 2. Why a plain E_OVERLOADED is not safe, and what the client does

The review's semantics say "return E_OVERLOADED without re-applying; the
client's subsequent retry remains safe because the session record still
proves the operation was already applied." The server half is right.
The client half was not true:
- `docs/RETRIES.md` says a client resends a refused request **as a new
  one**. `E_OVERLOADED` means "not applied".
- `net::Client` drops an answered request from its resend list and hands
  the answer to the caller. A caller that retries calls `send` again,
  which takes the next sequence number.
- So a resent deposit refused this way would be applied a second time
  under a new sequence. That was the original reason replays were exempt.

**Resolution, using fields that already exist:**
- The server keeps `replayed` set on the refusal, so the wire carries
  `kRespReplayed` with status `E_OVERLOADED`.
- `net::Client` (session mode) treats that pair as "applied; answer not
  ready; resend this request as it is":
  - it keeps the request in its unanswered list;
  - it does not hand the answer to the caller;
  - it resends the same bytes (same `req_id`, same sequence) with
    backoff, 2 ms doubling to 64 ms, until a real answer comes;
  - it counts `replay_retries()`;
  - a refusal is **not** progress. A replay refused for longer than
    `request_timeout_ms` counts as a failed node, as an unanswered request
    does: the client fails over and resends (A21 checks this).
  - `poll` caps its wait at the next resend, including `poll(-1)`.
    `next_retry_ms()` exposes it, and `ClusterClient::poll` and any caller
    waiting on `wait_fd()` must wake by then.
  - A refusal whose request is no longer unanswered (a late copy) is
    dropped, never handed to the caller.
  - A failover clears the retry list; the request is resent with the
    other unanswered ones anyway.
- `docs/RETRIES.md` records the rule for any other client: on
  `E_OVERLOADED` with `replayed`, resend the same request; never reissue
  it.

**Edge case (no double apply; the outcome may be lost):**
- If a later sequenced request from the same client is applied before
  the retry lands, the retry is answered `E_REPLAYED_OLD`: decided
  earlier, outcome not kept.
- This happens whenever seq m+1 is applied during m's backoff, not only
  in a failover batch. Examples: m+1 was already in flight; m+1 is
  `APPLIED` (no waiter needed); or the budget drained before m's resend.
  Refusals happen only when the budget is full, so this is the overload
  case.
- For a create, the caller then never learns the new object's ID. The
  object exists but is unreferenced. This is the existing meaning of
  `E_REPLAYED_OLD` for any create resent behind a newer request.
- It is the documented meaning of `E_REPLAYED_OLD`, and the op is never
  applied twice.
- Not built: holding a link's newer sends while a replay waits would
  close it, but it would serialize the client and change ordering. It is
  recorded for review.

This was raised as a decision before the merge, since it changes a
client contract (not the wire format).

## 3. `waiter_reserved`: one meaning, every assignment

It now means only "a successful `admit()` already counted this answer's
waiter bytes". It is never an exemption.

| Assignment | Admission that covers it |
|---|---|
| `run_exec`, after R1 | `admit(wal, sizeof(Waiter) + max_result)` when DURABLE; when APPLIED no waiter is created (`wait_lsn` stays 0) |
| `run_create`, after R1 | `admit(..., sizeof(Waiter) + max_result)` |
| `run_delete`: `= !internal` | client deletes: `admit(record, sizeof(Waiter) × (1 or 2))`, covering the ERASE waiter and the answer's. Internal (session expiry): false, and its ERASE waiter is internal work (S1.1 section 2: one per expired session) |
| `admit_answer_wait`, on success | its own `admit(0, sizeof(Waiter) + result)` |
| `replay_if_seen` | **removed** (it was the hole) |

`admit_answer_wait` is the only place an unreserved answer can become a
waiter. Every waiter is now either budgeted, internal (an ERASE
tombstone), or a forwarded answer that went through `admit_answer_wait`
at the owner (S1.1).

## 4. Remote replays (A19)

A sequenced request is never forwarded:
- `execute` answers `E_NOT_OWNER` when the entry shard is not the
  owner;
- so does `create` for a sequenced create placed near another shard's
  object, and a plain sequenced create mints only local IDs;
- deletes are never sequenced;
- `send_request` is reached only after those checks, so a forwarded
  message never carries `client_id`, and `serve` and `serve_multi`
  never see a replay.

So a replay waiter is always LOCAL. A19 proves it: a sequenced deposit
and a sequenced create sent to the wrong shard are refused there, use no
pending slot, and reach nothing at the owner. The only REMOTE answer
path is the forwarded unsequenced one, which S1.1's A13 and A14 cover.

## 5. The bound, stated with the one-op exception

`admit()` lets one op through when the waiter list (or the active WAL
buffer) is empty, even if that op alone exceeds the budget. That keeps a
small budget from blocking a shard for good. So the real bound per shard
is:

- **waiter bytes** ≤ `waiter_budget_bytes` + one admitted op's worst
  case + session-expiry ERASE tombstones (internal, one per expired
  session);
- **active WAL bytes** ≤ `wal_buffer_budget_bytes` + one admitted op's
  worst case, for client work. Non-refusable follow-ups are outside it:
  S1.1 section 3.

Replays add nothing to it. A20 holds 64 clients × 3 resends against a
budget of 4 replay waiters. Waiter bytes peak at exactly 1,024 (the
budget is 1,032) whatever the client count, and 188 resends are refused
marked replayed. `S1_ADMISSION.md` and `S1_1_CORRECTIONS.md` now point
here for this wording.

## 6. Recorded, not built

- **Follow-up intake backpressure** is a required pre-S6 slice, its own
  reviewed slice, likely between S2 and S3. A destination whose WAL
  stalls keeps accepting non-refusable follow-ups after client admission
  closes; A15 measured 1.6× the client budget from two senders. The
  proposed shape: stop draining follow-up rings at a destination hard
  WAL cap, so they stay unsent and the senders' emit gates bound the
  total. It changes ring draining and scheduling, so it is not in S1.2.
- **The q4 drain rule** (`waiters_now − wal_undurable`) is a harness
  rule for q4's deposit-only workload. It is not a general SNC "drained"
  metric and must be re-derived before any other use.
- **The `max_size` hard cap** (S1.1) is a correctness fix: before it, a
  legal op could leave an image that recovery rejects. The negative
  recovery reproduction stays in
  `bench/results/s11-validation-devvm-20261107/negative/neg1b-A15.log`.
- The 131,072 pending default stays provisional. Its ~10.5 MB is vector
  capacity (up to twice the live slots), not live slots.

## 7. Tests

| Test | Review item | What it proves |
|---|---|---|
| A16 | local replay bound | with the session undurable, one resend waits and the next is `E_OVERLOADED` marked replayed; `refused_waiter` +1, `refused_replay` +1, `replayed` +2; no record, version or balance change |
| A17 | recovery | after durability the original and the admitted replay answer once each, with the same version; the refused resend, sent again, gets the same answer; applied once; seq 2 then runs normally |
| A18 | backlog fast path | gate closed (1,025 held transfers; a new deposit gets `refused_followup`); 3 resends of an applied seq: 1 waits (empty-list rule), 2 are refused by the waiter budget, not the gate; nothing re-applied |
| A19 | remote replay | sequenced requests are never forwarded (section 4) |
| A20 | aggregate pressure | 64 clients × 3 rounds: waiters stop at the budget (4, 1,024 B); 188 refused marked replayed; every deposit applied once |
| A21 | client contract | through the socket front end, a second client on the same session resends seq 1. It is refused and resent with its sequence (backing off), never handed to the caller. A `poll(-1)` still wakes for the resend. Refused past a 300 ms request timeout, the client fails over and resends. Finally it is answered once, `OK` replayed and durable, with the original's version; applied once |

**Negative checks** (`bench/results/s12-validation-devvm-20261107/negative/`):
- With the S1.1 exemption restored in `replay_if_seen`, A16, A18, A20
  and A21 fail.
- With `net::Client`'s retry disabled, A21 fails.

## 8. Independent review

One read-only source review: APPROVE on the server side, which it proved
site by site. It made five client and observability findings, fixed
before the headline runs:
- a blocking `poll(-1)` (and `ClusterClient::poll`) never woke to resend;
- counting refusals as progress meant a replay refused forever never
  timed out or failed over;
- the fixed 2 ms resend interval had no backoff;
- a late copy's refusal could reach the caller as plain `E_OVERLOADED`;
- `refused_replay` was not a gauge. G_REFUSED_REPLAY is added before
  G_PUBLISHED_NS, which shifts its index again; all readers go by name.

Its finding on the outcome-loss edge case is documented in section 2,
and not built.
