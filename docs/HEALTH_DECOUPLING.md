# HA health-path decoupling: design proposal

Status: **PROPOSED, for review. Nothing here is implemented.**

The review of PR #35 (cody, relaying ChatGPT, 2026-11-04 12:40 UTC)
asked for this proposal. That review put this slice **before** the R08
slice that `STABILIZATION_REPORT.md` section N recommended, so this
document follows the review's order:

1. health-path decoupling (this slice);
2. bounded/resumable migration staging;
3. the index posting-list fix;
4. R08 and R11, each a separate slice.

Scope: health observation only. Every item below is tagged:

| Tag | Meaning |
| --- | --- |
| **OBSERVED** | seen in a run |
| **MEASURED** | a number from instrumentation |
| **INFERRED** | read from code at commit `c53ddbe` (evidence level EL0) |
| **PROPOSED** | not accepted yet |
| **UNPROVEN** | no evidence either way |
| **ACCEPTED** | decided by cody or the reviewer |

A rule tagged `[PROPOSED]` binds nothing until it is accepted. Paths are
relative to the repository root.

## 0. Summary

**Today (INFERRED).** The witness decides that a primary is alive from
its `STATUS` answer. `snc_server` builds that answer by asking every
shard thread for a replication summary and waiting for all of them. A
long task on one shard therefore makes a live node silent, and the
witness promotes its standby. PR #35 section D MEASURED this mechanism.

**What the witness actually reads (INFERRED).** From `STATUS` it uses
only `role`, `term` and `uuid`:

- `role` depends on the shards only to tell `p` from `n`, and the
  witness treats `p` and `n` the same.
- `term` and `uuid` never come from a shard.
- No replication field (`live`, `degraded`, `blocked`) feeds any witness
  decision, for liveness or for promotion.

**Proposal.**

1. **A `HEALTH` verb** on the admin port. It is answered from state that
   is published atomically and from a background progress sampler, with
   no shard task. It carries liveness and progress facts and no
   replication state.
2. **The witness and `--rejoin` poll `HEALTH`.** They fall back to
   `STATUS` against a node that does not know the verb.
3. **The admin thread becomes a small non-blocking loop.** It accepts
   connections and reads request lines as they arrive, answers `HEALTH`
   inline, and passes every other verb to one worker thread, in order,
   as today. Nothing slow then sits in front of `HEALTH`, and the other
   verbs keep their current one-at-a-time behavior.
4. **One decision for the reviewer (section 3.5): a live node whose
   shard stops making progress (a "wedge").** Decoupling health removes
   today's accidental 1 s wedge detection, together with the false
   failovers. Something must replace it. The options and their costs
   are set out in section 3.5.

`STATUS` keeps its meaning. Promotion eligibility can only get stricter.

## 1. Current STATUS: fields, sources and readers (INFERRED)

`STATUS` is answered at `tools/snc_server.cpp:468-484`. The admin thread
is a single thread: it accepts a connection, reads its line with a
500 ms timeout (`ha::read_line`, `:455`), and answers inline. `MOVE` is
the only verb handed to a thread, and only after its line has been read
(`:458-475`).

| Field | Source | Touches shard threads? | Read by |
| --- | --- | --- | --- |
| `role` | `is_standby` atomic gives `s`; else `eng.fenced()` atomic gives `f`; else `replication_status()` gives `p` (at least one shard has a sender, the max over shards, `engine.cpp:916`) or `n` | **only to tell `p` from `n`** | witness (`p`/`n` treated alike: `tools/snc_witness.cpp:439, 466`; `s` checked for the standby: `:479, 493`); rejoin (`snc_server.cpp:316-331`); `bench/lab/lab.py`; `tests/test_control.py` (asserts `p`); `tests/test_async_failover.py` |
| `term` | `term` atomic, stored by the main thread after `eng.promote()` returns (`:547`) | no | witness (`:466, 479, 495`); lab.py; tests |
| `uuid` | `uuid_hex`, constant after start | no | witness (`:479, 493`); rejoin |
| `shards` | config | no | nobody |
| `live` / `degraded` / `blocked` | `replication_status()` | **yes** | no automated reader. A repository-wide search finds only the fake reply in `tests/test_witness.py:69`. Operators read them |
| `epoch` | `eng.cluster().epoch`. The atomic map pointer is stored before the shards adopt the map (`engine.cpp:763-767`), so this is the epoch **adopted by the engine**, possibly not yet in effect on every shard | no | lab.py, test_control.py |
| `leases` | `lease_lo` / `lease_hi` atomics | no | test_control.py |

`Engine::replication_status()` (`src/core/engine.cpp:906-927`) submits a
task to every shard and waits on a latch with no timeout. MEASURED in
PR #35 D: one `STATUS` took 4,946.9 ms, all of it inside
`replication_status`.

The witness's own decisions (INFERRED):

| Decision | Inputs |
| --- | --- |
| Liveness | `role ∈ {p, n}` and `term` = the decided term, within `poll_ms` |
| Promotion eligibility | its Paxos state: not degraded, same state version (`snc_witness.cpp:492, 500`); the standby's `role = s`, `uuid` and `term` (`:493-495`) |
| Degraded mode | the primary's DEGRADE and SYNCED requests. These are decided on the primary's main thread from `replication_status()`, never from STATUS fields |

**Paths that wait synchronously on shard threads (INFERRED):**

| Path | Thread | Effect today |
| --- | --- | --- |
| `STATUS` | admin | the false-failover mechanism (MEASURED) |
| `AUDIT` | admin | O(objects) on every shard, twice (`snc_server.cpp:95-111`). Every later admin request waits behind it |
| a `MOVE` line read | admin | read inline before handoff: up to the 500 ms read timeout for a slow sender. A 13 MB line took 22 ms (MEASURED) |
| DEGRADE / SYNCED decision | main (`:560-598`) | `replication_status()`. A busy shard delays the grant request. INFERRED: availability only, because without a grant the primary does not run alone |
| `--report-ms` | main (`:607-612`) | `eng.stats(s)` per shard, each a shard task that walks the map |
| checkpoint | main (`:600`) | waits for durability (STABILIZATION_REPORT J) |

## 2. Semantics (PROPOSED)

Five concepts that one `STATUS` answer folds together today:

| Concept | Question | Needed by | Source after this slice | What it does **not** prove |
| --- | --- | --- | --- | --- |
| **Process liveness** | is the process running and is its admin loop scheduled? | witness, rejoin | any `HEALTH` answer within the poll timeout | that the main thread is alive (it handles DEGRADE, SYNCED and the promotion signal) |
| **Control-plane responsiveness** | can the admin port answer now? | witness, operators | the same answer | that heavy admin verbs are fast |
| **Shard progress** | has each shard's loop advanced recently? | witness (section 3.5), operators | a per-shard loop counter, sampled in the background (3.2) | that useful work completes. A loop that keeps iterating while an io_uring write never completes counts as progressing: a hung disk under io_uring is invisible here, and invisible to `STATUS` today (INFERRED) |
| **Main-thread progress** | is the main loop running? | operators (reported, not acted on) | the main loop's iteration time, published | — |
| **Replication state** (live, degraded, blocked, lag) | what is replication doing, as observed now? | operators; the primary's DEGRADE/SYNCED logic | `STATUS` and `--report-ms`, both unchanged and synchronous | — |
| **Promotion eligibility** | may this standby be promoted? | witness | unchanged, plus one input that can only make it stricter (3.5) | — |

Rules (all `[PROPOSED]`):

- **HL-1.** A `HEALTH` answer MUST contain only:
  - (a) values its writer publishes atomically (`role`, `term`, `uuid`,
    adopted `epoch`, `leases`, main-loop time);
  - (b) shard progress, computed from the shard counters read at answer
    time and compared with the background sampler's last snapshot
    (3.2).

  It MUST NOT contain replication state. `role=p` is a role fact (this
  node has started a replication sender), not a replication state (it
  does not say whether the link is up).
- **HL-2.** `STATUS` keeps its meaning: replication fields observed by
  every shard at the time of the answer. It MAY be slow. It stops being
  a liveness signal for the witness and for rejoin. `[AS-BUILT]` for the
  meaning; `[PROPOSED]` for the change of role.
- **HL-3.** No `HEALTH` field MAY make a replica eligible that is not
  eligible today. A `HEALTH` field MAY make promotion stricter (3.5).
  Every input to eligibility today is either the witness's own Paxos
  state or a value that `HEALTH` reads directly. This slice adds no
  stale or derived input. A future slice that adds LSNs to eligibility
  (R08 option F-5) must define their freshness then.
- **HL-4.** Whether a stalled shard on a live process counts as failure
  is the decision in section 3.5. Until it is made, no rule is proposed.

## 3. Smallest implementation change (PROPOSED)

### 3.1 The `HEALTH` verb

```
HEALTH -> role=<s|f|p|n> term=<T> uuid=<U> epoch=<E> leases=<lo>-<hi> shards=<N>
          stall_ms=<M> stall_shard=<i> main_ms=<K>
```

- **`role`.** Derived as in `STATUS`, except that `p` versus `n` comes
  from an atomic `replicating` flag in the engine's shared state. A
  shard sets that flag when it creates its sender
  (`Shard::start_sender`, `src/core/shard.cpp:1362`). This gives the
  same answer as `STATUS`'s max over shards (INFERRED). `test_control.py`
  asserts `p`, so the flag is required.
- **`term`, `uuid`, `epoch`, `leases`.** The same atomics `STATUS` uses.
  `epoch` is documented as the engine-adopted epoch (section 1).
- **`stall_ms` / `stall_shard`.** The longest time any shard's loop has
  gone without advancing, and which shard that is (3.2).
- **`main_ms`.** Time since the main loop last ran an iteration. It is
  reported only; no decision uses it in this slice.
- **Cost.** About ten atomic loads and one `snprintf`. No shard task and
  no lock that a shard thread holds.

### 3.2 Shard progress: published counters, background sampler

**Published.** Each worker gets a cache-line-aligned
`std::atomic<u64> iterations`, incremented with a relaxed store once per
iteration of `NativeRuntime::Impl::loop` (`src/native/runtime.cpp:324`).

**Why a still counter means a stall.** INFERRED: an idle loop still
iterates about every 1 ms. It spins for `idle_spin_us` (50 µs), then
calls `poll(..., 1)` (`runtime.cpp:384, 400`). So a counter that stops
means the thread is inside one iteration, not idle.

**Sampler.** A background thread in `snc_server` wakes every 10 ms. For
each shard it reads the counter and keeps `(last_count, changed_at)`.

- At answer time, `HEALTH` reads the counter again:
  - If it differs from the sampler's `last_count`, `stall = 0`.
  - Otherwise `stall = now - changed_at`.
- So the answer is fresh even when nobody polled before. The reviewer's
  item 3 showed that a responder-only sampler reads 0 on its first look
  at a wedged shard; this design does not.
- If the sampler itself lags, it can only make `stall` look longer,
  never shorter.
- `changed_at` starts at the sampler's first sample. The admin port opens
  only after `Engine::start` returns (`snc_server.cpp:370, 431`), so no
  `HEALTH` is answered during recovery.

**Costs.**

- Hot path: one relaxed increment per iteration, on a cache line only
  that shard writes. The sampler reads it 100 times a second. INFERRED
  negligible; to be MEASURED (T9).
- A clock read per iteration was the alternative considered and
  rejected as costlier (INFERRED: a vDSO call per iteration against an
  increment).

**Runtime interface.**

- `include/snc/sri.h` gains `u64 progress(u16 shard) const`, plus a
  `bool` saying whether it is supported.
- Native: the counter.
- Seastar: "unsupported" in this slice. `HEALTH` then omits `stall_ms`,
  and the witness uses silence only on such a node.
- That is option C of section 3.5 for Seastar builds, which are not the
  default (invariant 7). This is stated rather than hidden.

### 3.3 The admin loop does no blocking work

The admin thread becomes a `poll()` loop over the listening socket and
the connections whose line is still incomplete, all non-blocking:

| Request | Handling |
| --- | --- |
| `HEALTH` | answered inline at once |
| `MOVE` | as soon as `MOVE ` is seen, the fd and the bytes read so far go to the existing move thread, which reads the rest. A large line no longer passes through the loop |
| `STATUS`, `AUDIT`, `PROMOTE`, anything else | queued to **one** worker thread and answered in order, as today. The queue is bounded at 16 PROPOSED; when it is full, the connection is closed with `NO busy` |

- A connection that has sent no complete line within 500 ms is closed,
  as today's read timeout does.
- The number of half-read connections is bounded at 256 PROPOSED. Above
  that, the oldest is closed. A `HEALTH` connection completes within
  microseconds, so it is never the oldest for long.

Why this, rather than a thread per connection (the first draft):

- Concurrency is unchanged: heavy verbs still run one at a time. No
  concurrent `AUDIT`, no repeated `PROMOTE`, no concurrent
  `replication_status`.
- `STATUS` callers that give up while a shard is stalled pile up in a
  bounded queue, not in threads. They cannot use up a cap that `HEALTH`
  needs, because `HEALTH` never enters the queue. Both reviews found the
  first draft's cap of 64 reachable in about 2 s of stall from STATUS
  pollers alone.
- The shutdown order is extended. Stop accepting; answer queued
  requests `NO stopping`; join the worker before `eng.stop()`. Today
  only MOVE threads are waited for (`snc_server.cpp:632-638`). No shard
  task is submitted after stop begins.

A separate health port was considered. It would need a new port in every
witness's and operator's configuration. A port shared by a non-blocking
loop is smaller, and keeps `HEALTH` behind the same startup ordering as
`STATUS`, which E6 and E8 rely on (section 5).

### 3.4 Witness and rejoin

**Witness.**

- It polls `HEALTH` for the primary and the standby.
- A reply that does not begin with `role=` triggers the fallback: the
  witness polls that node with `STATUS` and tries `HEALTH` again every
  10 s. A node upgraded later is then picked up without a witness
  restart.
- Old nodes answer `NO unknown request`; the fake nodes in
  `test_witness.py` answer `NO`. Both trigger the fallback.

**Rejoin.** `--rejoin` reads only `role` and `uuid`, so it moves to
`HEALTH`, with the same fallback.

- INFERRED today: if the peer's shard is busy for more than about 3 s,
  rejoin decides "unreachable" (5 tries of 500 ms, `snc_server.cpp:315-318`)
  and starts as a primary with old data.
- That is fenced in sync mode, but in async mode its local
  acknowledgements before the fence are lost.

### 3.5 The wedge decision

**What decoupling removes.** A shard thread that never returns (a bug,
a deadlock, synchronous file I/O on a hung disk when io_uring is
unavailable) makes `STATUS` silent today. The witness then promotes
after `failover_ms`. That 1 s wedge detection is the same coupling that
produces the false failovers. INFERRED: no design can keep the 1 s wedge
detection and also remove the false failovers while shard tasks can run
for seconds legitimately. Bounding the tasks is the next slice.

**Legitimately long single iterations today.** These are what any wedge
threshold must sit above until they are bounded:

| Task | Length | Evidence |
| --- | --- | --- |
| 100K shared-key final stage on loopback | 3.7-5.0 s on the original destination; 5.9 s for the driver's retry on the promoted node | MEASURED, PR #35 D |
| 300K unique-key final stage, one process | 1.2 s (it held `replication_status` 0.94 s) | MEASURED, EL1 |
| `resume_all` after promotion and at each node-link connect | O(map) | INFERRED, `shard.cpp:1355, 1393` |
| `AUDIT` | O(map) | INFERRED |
| `--report-ms` `stats()` | O(map) | INFERRED |
| `begin_checkpoint` | O(map) | INFERRED, `shard.cpp:1538` |
| synchronous WAL writes and the checkpoint `fdatasync` without io_uring | seconds possible | INFERRED, `runtime.cpp:191-193, 215`, `shard.cpp:1623` |

The bounded-work slice as scoped covers only the migration stage. The
other rows need their own bounding before any threshold can be small.

**Options:**

| Option | Behavior | Cost |
| --- | --- | --- |
| **A. Witness-side wedge threshold (`wedge_ms`)** | A `HEALTH` answer whose `stall_ms ≥ wedge_ms` counts as no answer. Promotion follows `failover_ms` later, under the existing eligibility checks | A wedge is detected after `wedge_ms + failover_ms` instead of `failover_ms`. **This is a new timeout, and its value is driven by unbounded work: exactly what CORE_CONTRACTS B-4 warns against.** It is offered as an explicit, temporary trade, not hidden: false failovers from busy shards go away now, and wedge detection is slower until bounded work lets `wedge_ms` shrink |
| **B. No wedge detection in this slice** | Silence only | A wedged primary stays primary until an operator acts. Its other shards keep serving. Smallest change; an explicit, temporary regression for wedges only |
| **C. Node self-withdraws** | `Engine::withdraw` once a shard passes the threshold | Rejected. `withdraw` is permanent until restart (`engine.cpp:900-901`), takes the healthy shards down too (`Shard::withdraw` fails their waiters, `shard.cpp:1407-1431`), and the node cannot know whether a standby is eligible. It also needs the same threshold as A |

**Recommendation: A, with the value decided by the reviewer.** The
proposal does not pick a number to fit a measurement. If a number is
wanted now, it must exceed the longest row above with margin; 10 s would
cover the measured rows.

**Standby eligibility (stricter only).**

- Today's coupling requires every standby shard to run a task within
  `poll_ms` before promotion (the standby's `STATUS` must finish in
  100 ms).
- `HEALTH` does not wait on shards, so that requirement would silently
  disappear.
- Proposed: the witness refuses to promote a standby whose `stall_ms ≥
  failover_ms`. A wedged standby would otherwise hang `Engine::promote`
  in `run_on_all` after `stop_standby`, leaving the group down
  (INFERRED, `engine.cpp:574`).
- This adds an input to eligibility that can only refuse (HL-3).

### 3.6 Compatibility and rollout

INFERRED: **each witness decides alone that a primary is silent and
proposes the promotion** (`snc_witness.cpp:488-507`). Paxos orders
proposals; it does not require a majority to agree that the primary is
silent. Consequences:

- One witness still polling `STATUS` (old, or `--health-verb status`)
  can still cause the false failover for the whole group.
- The witness with the smallest `wedge_ms` decides for all of them.

Rollout:

1. Upgrade the nodes. `HEALTH` is additive and inert until asked.
2. Upgrade every witness, all with the same `--wedge-ms`.
3. The fix takes effect only once every witness of a group polls
   `HEALTH`. T10 tests a mixed set to pin this down.

There is no durable format, wire protocol or Paxos state change
(INFERRED: the change touches only admin-port text and witness polling).

## 4. Failure modes this introduces or leaves

| # | Failure | Effect | Mitigation |
| --- | --- | --- | --- |
| HF1 | Zombie primary: admin loop alive, a shard stuck inside one iteration | option A: failover after `wedge_ms + failover_ms`; option B: none until an operator acts | the decision in 3.5; bounded work |
| HF2 | A shard busy longer than `failover_ms` but shorter than the wedge threshold | no failover; that shard's clients wait | accepted. A failover cannot help: in D the promoted node redid the same work and stalled 5.9 s (MEASURED) |
| HF3 | A data plane that iterates but completes nothing (an io_uring write that never completes) | not detected | not a regression: `STATUS` answers in this case today (INFERRED). Recorded as a gap |
| HF4 | Main thread stuck (on `replication_status` behind a wedged shard, a checkpoint waiting on durability, `ask_witness` up to 1.5 s per witness) | `HEALTH` stays green while DEGRADE/SYNCED and the promotion signal wait | same as today; `main_ms` makes it visible; no decision uses it in this slice |
| HF5 | The heavy-verb queue is full | `STATUS`/`AUDIT` refused with `NO busy` | bounded; `HEALTH` is unaffected |
| HF6 | Mixed or misconfigured witnesses | the false failover persists | rollout in 3.6; T10 |
| HF7 | The admin loop is starved of CPU by spinning shard threads (P2 starvation) | silence, so a failover | better than today, which needs the admin thread **and** every shard thread. Unchanged in kind |
| HF8 | Sampler resolution | `stall_ms` may read up to 10 ms too long | thresholds in seconds |
| HF9 | Seastar builds | no progress, so silence only | stated in 3.2 |

**Unchanged (INFERRED):**

- Partition: silence gives a promotion after `failover_ms`. FAILURE_MODEL
  N2 records 7.7-8.5 s to service on real machines, with the detection
  time not yet explained.
- Process death, and SIGSTOP of the whole process: silence.
- Fencing: once promoted, the standby refuses the old term, and the old
  primary's senders are refused and withdraw (`replication.cpp:413`,
  `shard.cpp:606`). A shard wedged during promotion withdraws at its
  next `poll()`.
- R08: still open. Fewer promotions only reduce its exposure.

The source review found no path where ignoring a busy primary loses an
acknowledged write or allows two serving primaries that today's
coupling prevented. Not promoting cannot create a second primary, and
degraded and sync behavior do not depend on `STATUS`. INFERRED.

## 5. Relation to the contract documents

| Document item | Effect, if this proposal is accepted |
| --- | --- |
| CORE_CONTRACTS section 11, B-2 ("liveness health MUST NOT wait on shard threads … MAY read a per-shard heartbeat") | the mechanism proposed here |
| CORE_CONTRACTS B-3 (both its sentences) | conflicts. B-3 says the witness "MUST NOT promote on a slow answer from a process that proves it is alive, only on silence", and that past a wedge limit "the primary withdraws itself". Option A promotes a live process after `wedge_ms`; option C (self-withdraw) is rejected. B-3 is rewritten to the option chosen |
| CORE_CONTRACTS section 11, B-4 (no timeouts as a substitute for causality) | option A is in tension with it, as stated in 3.5; the reviewer decides |
| CORE_CONTRACTS section 8, PE-3 (silence measured on a path not dependent on shard threads) | met for liveness. Under option A, shard progress again feeds suspicion, but through a published counter, not by waiting on the shard |
| CORE_CONTRACTS section 8, E1-E11 | not widened. One stricter input (3.5). E6 and E8's evidence ("the admin thread starts only after `Engine::start`") still holds, because `HEALTH` is served on the same admin port, opened at the same point; the implementation MUST keep that ordering |
| FAILURE_MODEL P2 (self-stall: KNOWN ISSUE) and class W1 (one unit of work monopolizes a shard) | stalls shorter than the wedge threshold stop causing failovers once T1-T3 pass. The coverage is bounded by that threshold until bounded work lands; W1 stays a KNOWN ISSUE for availability of the stalled shard |
| FAILURE_MODEL G6 (service resumes without operator action) | for wedges: after `wedge_ms + failover_ms` under option A; not at all under option B. A real change, not a restatement |
| INTERACTION_MATRIX row 11 (long task × witness health) | from KNOWN ISSUE to TESTED, once T1-T3 pass |
| TRACING | `admin_status_slow` kept; an `admin_health_slow` line at ≥ 20 ms added |

Invariants: invariant 14 (no consensus on the hot path) holds; the only
hot-path cost is a local relaxed increment. No other invariant is
touched (INFERRED).

## 6. Deterministic test plan

**Stall injection.**

- A debug admin verb, `DEBUG STALL <shard> <ms>`, submits a task that
  spins on that shard. It is a real shard-thread stall in a real
  process, which a fake node cannot give.
- It is refused unless `snc_server` starts with `--debug-admin`.
- A runtime flag was chosen over a build option so that the tested
  binary is the shipped one.

**Timing.**

- Assertions are on decisions: promotion or none, state version, which
  term serves.
- Latency bounds (for example "`HEALTH` within `poll_ms`") are checked
  in the RelWithDebInfo run only. Under ASan and TSan they are logged,
  not asserted.

| # | Test | Asserts |
| --- | --- | --- |
| T1 | **Regression for the original mechanism.** A real sync pair and 3 witnesses (`failover_ms` 1000, `poll_ms` 100), under write load. `DEBUG STALL 0 3000` on the primary | the witnesses' HA state version is unchanged and not degraded (so no promotion, and none blocked by a grant); `stall_ms` rises to ≥ 2,500 and returns to 0; `STATUS` is slow meanwhile (the coupling is still in `STATUS`, as intended); no acknowledged write lost |
| T2 | T1 with every witness on `--health-verb status` | a promotion happens. This shows the fix is the verb, not a timing change |
| T3 | Wedge (option A): `--wedge-ms 2000`, `DEBUG STALL 0 6000`, under write load | promotion after about `wedge_ms + failover_ms`; no acknowledged write lost; the old primary acknowledges nothing after the promotion is decided; the stalled shard's waiters get `E_FENCED` |
| T4 | SIGKILL the primary | promotion within `failover_ms` plus the existing slack (as `test_witness.py`) |
| T5 | SIGSTOP the primary | promotion; after SIGCONT the old primary is fenced |
| T6 | Partition through a TCP proxy that stops forwarding everything to and from the primary (admin, replication, client ports) | promotion after `failover_ms` plus slack. The old primary is fenced when its sender reaches the promoted standby, which refuses its term. No acknowledgement from the old primary after promotion (sync) |
| T7 | Eligibility with real nodes: a standby still seeding (E6) is not promoted; with fake nodes: degraded on record, wrong term, wrong space, standby `stall_ms ≥ failover_ms` | no promotion in any case |
| T8 | Rejoin while the peer primary has `DEBUG STALL 0 5000` | rejoin sees the peer as primary and starts as its standby |
| T9 | Cost: `snc_bench` in memory and durable, before and after | throughput within a bar the reviewer sets (PROPOSED ±2%) |
| T10 | Mixed witnesses: 2 on `HEALTH` and 1 on `STATUS`, with T1's stall | a promotion **does** happen. This pins down the rollout requirement in 3.6. Also: a fake node answering `NO` to `HEALTH` triggers the fallback, and a later upgrade is picked up within 10 s |
| T11 | Admin loop: 300 connections that never send a line, and an `AUDIT` running; then `HEALTH` | answered (half-read connections are bounded by evicting the oldest; `AUDIT` is in the worker). `STATUS` beyond the queue bound gets `NO busy` |
| T12 | Lab P5 100K move with `SNC_TRACE=1`, on loopback | no promotion; the move completes; `HEALTH` is answered throughout the 3.7-5 s stage. A lab check, not the regression, since it depends on the machine |
| T13 | Full suite, ASan+UBSan and TSan (the counters, the sampler and the admin loop are new shared state) | clean |

## 7. Rollback

| Part | How to roll back |
| --- | --- |
| Witness polling | `--health-verb status` on **every** witness of a group restores today's polling (3.6). Reverting the witness binary does the same |
| Node: `HEALTH`, the counter and the sampler | additive, and inert when nobody asks |
| Node: the admin loop | a separate change that reverts on its own. `--health-verb status` does not undo it. Its observable behavior for heavy verbs is unchanged (one at a time) |
| Wedge rule | `--wedge-ms 0` turns it off (option B) |
| Data | no data or format migration in either direction |

## 8. What this slice does not do

- It does not make `STATUS` faster or cache it.
- It does not change `failover_ms` or `poll_ms`.
- It does not touch migration staging, the index, R08 or R11.
- It does not decouple the main thread's DEGRADE/SYNCED loop (HF4).
- It does not build trace rings or switch lab.py's probe.

## 9. Decisions for the reviewer

1. **The wedge decision (3.5):** option A (`wedge_ms`, with its stated
   tension with B-4) or option B (no wedge detection until bounded work).
   If A, the value.
2. **The bounds:** the heavy-verb queue (16) and the half-read
   connection cap (256).
3. **`DEBUG STALL`** behind `--debug-admin` in the shipped binary,
   versus a test-only build.
4. **The cost bar** for T9.
