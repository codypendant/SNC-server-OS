# S4: staggered final checkpoint sync (node-level K tokens)

Status: **S4 core approved (PR #81, `docs/handoff/PR81_S4_Review.txt`); S4.1 (section 9: admin reply delivery, the fairness wording in section 2, `waiting_peak` in section 5) for review.**
Work order: `docs/handoff/PR80_S33_Review_S4_Authorization.txt` (PR #80
approved, S3 complete, S4 authorized and only S4). Design: option D of
[CHECKPOINT_BACKPRESSURE_DESIGN.md](CHECKPOINT_BACKPRESSURE_DESIGN.md)
section 3; writeback before it: [S3_WRITEBACK.md](S3_WRITEBACK.md).

## 0. Summary

A shard whose checkpoint part is ready for its final `fdatasync` (every
chunk write complete, its WAL durable here up to the part's `max_lsn`)
takes a ticket from the node's `SyncTokens` and then, at each step of its
loop, asks whether its ticket is inside the window of K. While it is not,
the step returns and the shard goes on serving; when it is, the shard
holds a token and issues the sync. The sync's completion returns the
token, before anything else, including the fatal path. At most K final
syncs are in flight on the node at once.

`Config::ckpt_sync_tokens` (default **0 = unlimited**, the behaviour
before S4), `--ckpt-sync-tokens K`, 0..256. K is a hypothesis: the tests
run K = 1, 2, 4, 8 and unlimited; no value is chosen here, and none may
be chosen from dev-VM numbers (work order sections 6 and 15).

Nothing else changes: what is written, the chunk size and window, the
`RWF_DSYNC` writes, the phase model, the file formats, recovery, the
manifest and superblock, the commit rule, S1 admission, S2 pacing,
lifecycle, replication and promotion (section 6).

Not in S4: S5, S6, the follow-up intake slice, B2, any R640, CP-1A or
comparator run.

## 1. Conditions of the work order, and where each is met

| Section | Condition | Where |
|---|---|---|
| 5 | ready part requests a token; only K hold one; holder syncs; completion releases; next waiter proceeds; commit after every part | `Shard::checkpoint_step` final block; `checkpoint_sync_complete`; `Engine::checkpoint` unchanged (waits for every part's `done`) |
| 6 | K configurable, tested at 1, 2, 4, 8/unlimited; not frozen | `Config::ckpt_sync_tokens`, `--ckpt-sync-tokens`; T1 (1, 2, 4, 8, unlimited), T5 (1, 2, 8) |
| 7 | must not change | section 6 |
| 8 | atomicity unchanged | section 6 (CC-5); T5 |
| 9 I1 | at most K active | allocator proof, section 2; T1 counts the syncs themselves |
| 9 I2 | no leak; release on success and fatal path | `checkpoint_sync_complete` releases first; section 3; T2, T6 (read at the abort), T9 |
| 9 I3 | no starvation | section 2; T3, T9 |
| 9 I4 | no shard-thread blocking | the wait is a failed check at a step; section 3; T4 |
| 9 I5 | every shard completes, commit after all | section 3; T1-T9 all commit |
| 9 I6 | unlimited restores S3 | section 4; T7; the full suite runs at the default 0 |
| 10 | fairness, documented progress argument | section 2 |
| 11 | observability; HEALTH tells token wait / syncing / stalled elsewhere without changing phases | section 5 |
| 12 | tests T1-T10 | section 7 |
| 13 | CC-1..CC-5 map | section 6 |
| 14 | validation gate | section 8 |

## 2. The allocator (`src/core/sync_tokens.h`)

Three monotonic counters, one per Engine, never reset: `requests`
(tickets handed out), `grants` and `releases`.

- `take_ticket()`: `t = requests.fetch_add(1)`. Tickets are unique and
  consecutive, in the order parts became ready.
- `try_acquire(t)`: if `K != 0` and `t >= releases + K`, refuse (the
  caller retries at its next step). Otherwise `grants.fetch_add(1)` and
  grant.
- `release()`: `releases.fetch_add(1)`.

**Bound (S4-I1).** A grant of ticket t happens only after reading some
release count R with t < R + K. Releases only grow, so at any moment
every ticket granted so far is below R_now + K. Tickets are unique, so at
most R_now + K have been granted, of which R_now were released: at most
K hold a token. (A release counts only a completed grant: each release
is made by the shard that holds the token, once, in
`checkpoint_sync_complete`.)

**Order and progress (S4-I3), corrected in S4.1.** What the allocator
orders is *eligibility*, not grants. (Before S4.1 this section, the
header comments, the PR and T3's output said tokens are "granted in
ticket order"; that was stronger than the code. Review:
`docs/handoff/PR81_S4_Review.txt` section 7.)

- **Tickets** are handed out in the order parts became ready (one
  `fetch_add`).
- **Eligibility** follows ticket order: ticket t is eligible once
  t < R + K. The frontier R + K only moves forward, so a later ticket
  never becomes eligible before an earlier one, and an eligible ticket
  stays eligible.
- **Grants are not FIFO.** An eligible ticket acquires when its shard
  next polls. A later eligible ticket can acquire first, and can even
  complete and release before an earlier one has polled; the frontier
  then moves and admits the next ticket, which may also acquire before
  the earlier one. Example, K = 2: tickets 0 and 1 are eligible; 1
  acquires, syncs and releases (R = 1) before 0 polls; ticket 2 is now
  eligible (2 < 1 + 2) and may acquire before 0. The K bound is not
  affected (it never depends on who holds the tokens).
- **No starvation.** Every eligible ticket belongs to a shard whose loop
  calls `checkpoint_step` on every iteration while its checkpoint is
  active (an unstalled shard keeps polling), so it acquires at its next
  poll, and eligibility is never lost. Every granted sync completes (it waits only on the device: no
  lock, no other token, no WAL) and releases. A ticket j places behind
  the frontier becomes eligible after at most j releases. Overtaking is
  finite: a shard takes one ticket per checkpoint
  (`Checkpoint::ticketed`), a checkpoint has one ticket per shard, and
  the next checkpoint cannot begin until every part of this one has
  completed (`Engine::checkpoint` waits for all and holds `control_mu_`),
  so at most n - 1 tickets can overtake any ticket, and none can lap it.

So the guarantee is: ordered ticket assignment, ordered eligibility,
the K bound, and eventual acquisition by every waiter. Strict FIFO grant
order is not guaranteed and is not needed for S4-I3; the allocator is
unchanged in S4.1.

**No abandoned tickets.** A ticket taken is always granted later: once a
part has its ticket there is no cancellation. `Engine::stop()` waits for
`checkpoint()` (its permit), and a storage fatal ends the process. (An
abandoned ticket would shrink the window by one for the Engine's life;
none can exist.)

**Unlimited (K = 0).** `try_acquire` always grants. The counters still
run, so the concurrency the bound would limit stays visible (T7).

**Syncs in flight.** `inflight` is incremented just before the
`fdatasync` is issued and decremented first thing in its completion;
`inflight_peak` is exact (from the `fetch_add` result). It sits in the
same branch as the grant, so it is not independent of the shard's code
path, but it is counted from the syncs, not from the grant and release
counts: a bypassed `try_acquire` (mutation N1) or a grant past K (N2)
shows there. It is the quantity S4-I1 bounds.

**Readings.** `Engine::sync_tokens()` reads grants, then releases and
requests, then grants again, and retries while grants changed (bounded
to 64 tries): active = grants - releases and waiting = requests - grants
come from one stable reading of grants. Releases and requests are read
just after it, so while counts move, active can read low and waiting
high by what changed meanwhile; neither passes its bound. The tests
compare counts only while held still (two equal readings around the
gauges). Peaks are maxima updated by CAS:
`inflight_peak` exact, `active_peak` and `waiting_peak` at most the true
value (the second read can only lower them). Any thread, no lock.

## 3. The shard (`src/core/shard.cpp`)

`checkpoint_step` already runs on every loop iteration while a checkpoint
is active. Its final block was:

```
if (writes_inflight == 0 && lsn_local_durable(max_lsn)) { syncing = true; enter SYNC; fdatasync }
```

It is now:

```
if (writes_inflight == 0 && lsn_local_durable(max_lsn)) {
  if (!ticketed) { ticket = take_ticket(); ticketed = true; token_request_ns = now; }
  if (!try_acquire(ticket)) return;                      // wait: back to the loop
  token_held = true; record wait (last, max, EWMA, acquired); inflight++
  syncing = true; enter SYNC; fdatasync                 // unchanged
}
```

- **Nonblocking (S4-I4).** Waiting is the `try_acquire` check failing;
  the step returns and the loop goes on polling WAL completions, ring
  messages, follow-ups, I/O and control work. There is no mutex,
  condition variable or sleep anywhere on this path (T4).
- **CC-3 before the sync.** The ticket is taken only once the condition
  holds, and the whole condition is evaluated again at every step up to
  and including the grant; `fdatasync` is issued in the same step as a
  true evaluation. Durability here is monotonic, so it stays true.
- **Release (S4-I2).** `checkpoint_sync_complete` first decrements
  `inflight`, then, if `token_held`, clears it and releases, and only
  then checks the result: a failed sync calls `storage_fatal` (abort)
  with its token already back. It is the only end of a part's sync, and
  every final sync reaches it, through `checkpoint_sync_done` or, when
  a test holds completions, `checkpoint_process_held`.
- **Completion (S4-I5).** The part's `done` (and so `Engine::checkpoint`'s
  latch) is reached only after the sync, as before; the commit waits for
  every part.
- **Timing gauges.** The durable wait ends when the token is requested
  (`ckpt_enter`'s DURABLE_WAIT case uses `token_request_ns`); the token
  wait is reported apart; `last_sync_us` is still the sync itself (grant
  to completion). With K = 0 request and grant are the same step, so all
  three keep their S3 values.

## 4. Unlimited is the S3 path (S4-I6)

With K = 0 the first `try_acquire` at the step where the part becomes
ready grants, so `fdatasync` is issued in the same step as before S4. The
added work on that step is three atomic increments and a few stores.
T7 checks that every part's token wait is exactly 0 and that all eight
parts are in their sync at once with completions held; the whole suite
(ctest) runs at the default 0.

## 5. Observability

Per shard (`include/snc/gauges.h`, GAUGES):

| Gauge | Meaning |
|---|---|
| `ckpt_sync_tokens` | K (configured; 0 = unlimited) |
| `ckpt_sync_state` | 0 not at the final sync, 1 waiting for a token, 2 syncing |
| `ckpt_token_wait_since_ns` | start of the token wait in progress; 0 if none |
| `ckpt_token_wait_last_us`, `_max_us`, `_ewma_us` | token request to grant (EWMA 7/8, like the write latency) |
| `ckpt_token_acquired` | tokens acquired since start |
| `ckpt_token_ticket` | the latest request's ticket + 1 (0 = none) |

The final sync's own duration stays `ckpt_last_sync_us`.

Node (`Engine::sync_tokens()`, GAUGES header line): K, requests, grants,
releases, active, waiting, active and waiting peaks, the longest token
wait, syncs in flight and their peak.

`waiting` (requests - grants) and `waiting_peak` count tickets taken and
not yet granted. A ticket is taken just before its first `try_acquire`,
so even a grant at that first poll is "waiting" for an instant, and
`waiting_peak` can be above 0 (up to the shard count) when no shard was
ever refused a token, for example at K = 0. They do not mean a shard
experienced a denied wait; the per-shard `ckpt_token_wait_*` gauges and
`ckpt_sync_state` = 1 are the signal for that (S4.1 clarification,
review section 8).

HEALTH appends `ckpt_sync_k`, `ckpt_sync_active`, `ckpt_sync_waiting`
and `ckpt_slow_sync` (the slowest shard's sync state: `token`, `syncing`
or `none`). The phase model is unchanged: a part waiting for a token is
still in `durable_wait`. So:

- `ckpt_slow_phase=durable_wait ckpt_slow_sync=token`: waiting for a
  token;
- `ckpt_slow_phase=sync` (`ckpt_slow_sync=syncing`): syncing;
- any other phase, or `durable_wait` with `ckpt_slow_sync=none`: stalled
  elsewhere (the walk, the window, the WAL).

T3 shows all three states on the gauges (WAL held: 0; token: 1; sync: 2).

## 6. CC-1..CC-5, and what does not change

| | Status | Why |
|---|---|---|
| CC-1 coverage | unchanged | the walk, the snapshot and which IDs are visited are untouched |
| CC-2 redo completeness | unchanged | WAL retention and reclamation run after the commit, as before |
| CC-3 no future state | unchanged | `lsn_local_durable(max_lsn)` is checked before the ticket and again at the grant step; the sync is issued only in a step where it holds (section 3) |
| CC-4 idempotent replay | unchanged | records, formats and replay are untouched |
| CC-5 atomic publication | unchanged | parts may become durable at different times, which was already allowed; nothing names the checkpoint until the superblock; the commit waits for every part. T5 crashes with none, some and all parts synced, between manifest and superblock, and after it |

Not changed (work order section 7): S1 admission, S2 pacing (`pace()`
runs every step in every phase, as before), `RWF_DSYNC` chunks, chunk
size, window, checkpoint and WAL formats, recovery, manifest and
superblock contents and order, `kEmitBacklog`, placement, replication,
promotion, Engine lifecycle, follow-up intake, B2.

Test-only additions: `debug_ckpt_release_syncs` (counted release of held
sync completions), `debug_ckpt_fail_sync` (a shard's next final sync
completes with -EIO), `debug_ckpt_hold_superblock` (a wait between the
manifest and the superblock, for T5), and `SNC_CKPT_SYNC_TOKENS` (runs
a whole suite under a K, announced on stderr, like `SNC_CKPT_IO`).

## 7. Tests (`tests/test_s4_sync_tokens.cpp`, eight shards)

| Test | What it proves |
|---|---|
| T1 | K = 1, 2, 4, 8, unlimited: with every part ready and completions held, exactly min(K, 8) syncs in flight (the independent count) at 400 observations, the rest waiting; then commit and exact recovery |
| T2 | K = 1, 2: each released completion lets exactly one waiting part in, eight times |
| T3 | K = 1, 2: parts made ready in the order 3 7 0 5 1 6 2 4 (WAL flush held per shard) take tickets in that order; after each released completion, exactly the parts inside the eligibility frontier have acquired (all of them, and none outside); every waiter acquires; sync_state 0 / 1 / 2 told apart. With completions held one at a time this is ordered eligibility and eventual acquisition, not a strict-FIFO test (section 2) |
| T4 | K = 1, seven parts waiting: 800 durable deposits and 200 reads complete; every shard's WAL keeps flushing |
| T5 | K = 1, 2, 8: crash with none synced, some synced, all synced before the manifest, manifest written but not the superblock, after the superblock; exact recovery each time, then a new checkpoint. A process crash (`_Exit`), not a power loss: "synced" is the shard's view (the completion processed), and with completions held the kernel may already have finished the sync; the page cache survives. So T5 tests the commit and superblock logic under staggering, not device durability, as S3 T7 does (S3_WRITEBACK.md section 9) |
| T6 | K = 1: -EIO on the last part's sync with its token held; the abort (read from a SIGABRT handler) sees grants = releases and none in flight; previous checkpoint authoritative |
| T7 | unlimited: zero token wait, eight syncs at once (in T1) |
| T8 | requests, grants, releases, active, waiting, in flight and the per-shard gauges reconcile exactly (in T1, T2, T3, T9) |
| T9 | K = 1, 2: 30 checkpoints each under a deposit load: one token per shard per checkpoint, nothing drifts |
| T10 | negative mutations (bypass, K + 1, leak, fatal-path leak, unordered grant, CC-3 check dropped): `bench/results/s4-validation-devvm-20261109/negative` |
| TC | K = 257 refused, 256 accepted |

## 8. Validation, open items

Open: `ckpt_slow_sync` reports the shard in the lowest phase only, like
`ckpt_slow_phase`. (The GAUGES truncation this section listed as open
is fixed in S4.1, section 9.)


Dev VM, regression only (no performance claim): full serial Release,
ASan+UBSan, TSan and legacy (`SNC_CKPT_IO=legacy`), plus the whole
suite under K = 1 (`SNC_CKPT_SYNC_TOKENS=1`). Every run, failed or not,
is listed in `bench/results/s4-validation-devvm-20261109/README.md`.

## 9. S4.1: admin replies complete or explicitly cut

**The defect (review section 6, confirmed).** `admin::Loop::reply_close`
sent each inline reply (HEALTH, GAUGES) with one
`send(..., MSG_DONTWAIT)` and closed the fd. A positive partial write or
`EAGAIN` was not handled, so a reply larger than what the socket took at
once was cut silently. GAUGES for eight shards is about 20 KB.

**The fix (`tools/admin_loop.h`).** Inline replies (and the loop's own
"NO busy" / "NO line too long") go through `Loop::reply`:

1. it sends what the socket takes now, without blocking;
2. if all of it went, the fd is closed, as before;
3. otherwise the rest waits in `out_` (loop thread only). The loop polls
   those fds for `POLLOUT` next to the listener and the half-read
   connections, and sends more whenever the client has read;
4. a reply not finished within the write deadline (`kWriteMs`, 5 s), or
   whose send fails, is closed unfinished and counted
   (`write_timeouts`, `write_failed`);
5. at most `kOutMax` (64) replies wait; one more closes the oldest
   waiting (`write_evicted`).

Every reply ends in its newline, so a client tells a cut reply from a
complete one: a cut one has no newline. The loop never waits on a
client, so HEALTH stays immediate behind any number of slow GAUGES
readers. Memory is bounded by `kOutMax` replies. HEALTH appends
`adm_out`, `adm_out_hw`, `adm_write_timeouts`, `adm_write_failed` and
`adm_write_evicted`. The heavy worker's replies (STATUS, AUDIT, ...)
already used a blocking write-all on their own thread and are
unchanged. No engine code changes in S4.1.

**Tests (`tests/test_admin_loop.cpp`, a real `admin::Loop` on loopback).**
On loopback the socket takes about 2.8 MB at once, so GAUGES itself
never needs a second send there. The test therefore uses an inline verb
`BIG n` of the same kind with a 16 MiB reply and a 4 KiB client receive
buffer:

| Test | What it proves |
|---|---|
| A1 | slow reader: the reply waits for POLLOUT and arrives whole, ending in its newline; 20 HEALTH meanwhile, each prompt |
| A2 | stalled reader: closed at the write deadline; what it read has no newline; `write_timeouts` = 1; HEALTH prompt meanwhile; nothing left waiting |
| A3 | bound: one more than the limit closes the oldest (`write_evicted` = 1); HEALTH prompt |
| A4 | small replies go out at once; nothing waits |

Mutation N7 restores the one-call send and close; A1 must fail.

