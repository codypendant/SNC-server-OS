# Core stabilization report (work order of 2026-11-04)

This report answers `docs/handoff/SNC_Core_Stabilization_Complete_Work_Order.txt`.
It carries the deliverables that are reports rather than specifications:

- D: the causal report on the 100K false failover;
- J: the resource and backpressure inventory;
- K: the exit criteria for Baseline 0.2;
- L: contradictions;
- the correctness dimensions (Part X);
- the sanitizer baseline (Part XXI);
- N: the next slice;
- P: the tests still needed;
- the review and QA findings.

The specifications are separate documents:

| Deliverable | Where |
| --- | --- |
| A, core contracts | [CORE_CONTRACTS.md](CORE_CONTRACTS.md) |
| F, R08 async contract | [CORE_CONTRACTS.md](CORE_CONTRACTS.md) section 9 |
| G, R11 session incarnation | [CORE_CONTRACTS.md](CORE_CONTRACTS.md) section 10 |
| H, acknowledgement matrix | [CORE_CONTRACTS.md](CORE_CONTRACTS.md) section 6 |
| I, recovery barriers | [CORE_CONTRACTS.md](CORE_CONTRACTS.md) section 12 |
| B, failure model | [FAILURE_MODEL.md](FAILURE_MODEL.md) |
| C and M, interaction matrix and ranked risks | [INTERACTION_MATRIX.md](INTERACTION_MATRIX.md) |
| E, causal tracing design | [TRACING.md](TRACING.md) |

Every claim is tagged with one of these labels:

- **OBSERVED**: seen in a run.
- **MEASURED**: a number from instrumentation.
- **INFERRED**: read from code, not tested.
- **PROPOSED**: a contract not yet accepted.
- **UNPROVEN**: no evidence either way.
- **ACCEPTED**: decided by cody or Codex.

Evidence levels follow Part XIX. They are written EL0-EL6 so they do not
clash with EVIDENCE.md's experiment names L0-L7:

| Level | Kind of evidence |
| --- | --- |
| EL0 | code inference |
| EL1 | unit or characterization test |
| EL2 | integration test |
| EL3 | fault injection |
| EL4 | real machine |
| EL5 | long run |
| EL6 | formal proof |

## D. The 100K false failover: measured causal report

### Question

In the corrected 100,050-object loopback move (PR #34,
`bench/results/lab-20261104/p5-060000`), the destination group's primary
was replaced during the final copy with no injected fault. Where did
health and control-plane progress stop, for how long, and why?

### Method

Causal instrumentation was added. It is off unless `SNC_TRACE=1`
([TRACING.md](TRACING.md)). Every line carries a `CLOCK_MONOTONIC`
timestamp. All processes of a loopback run share that clock, so their
lines merge into one timeline (`bench/lab/trace_merge.py`). The lines
come from:

- Shard loop iterations of 20 ms or more, broken into ring drain,
  injected control tasks, I/O reaping, network poll and publish
  (`src/native/runtime.cpp`).
- `STATUS` answers of 20 ms or more, with the time spent in
  `Engine::replication_status` (`tools/snc_server.cpp`).
- Every admin `MOVE` request in and out: bytes, read time, handling
  time.
- `stage_slot`, `seal_slot` and `export_slot`: the time on the shard
  thread and the time waiting for durability (`src/core/engine.cpp`).
- Inside a final stage: validation, `put_image` (with its index and WAL
  shares), and the map scan (`src/core/shard.cpp`).
- Every witness `STATUS` poll that failed or took 20 ms or more,
  suspicion, and the promotion proposal (`tools/snc_witness.cpp`).
- The driver's steps and each request it sent (`tools/migrate.h`).
- Per-thread CPU and run queue every 100 ms (`bench/lab/cpu_sample.py`).

The P5 move was run three more times with tracing:
`SNC_TRACE=1 lab.py loopback p5 --slot-size 100000 --chunk 256 --seconds 20`.
The runs are `p5-063245`, `p5-063511` and `p5-063717`, with their
`*.trace` files committed. The mechanism was then reproduced in one
process, without the lab, by `bench/stage_stall.cpp`.

### Result: the false failover reproduced in 3 of 3 traced runs (4 of 4 with PR #34's)

| Run | Final stage on the destination shard thread | Witness silence at suspicion | Promotion proposed after stage start |
| --- | --- | --- | --- |
| p5-063245 | 4,647.6 ms (one loop iteration) | 1,149 ms | 1.09 s |
| p5-063511 | 3,693.9 ms | 1,135 ms | 1.03 s |
| p5-063717 | 4,977.5 ms | ~1,135 ms | 1.04 s |

### The timeline (p5-063717, MEASURED)

Seconds are from the first trace line.

| t (s) | Where | Event |
| --- | --- | --- |
| 56.521 | n3 (destination primary, group 1) | `STAGE` final, 100,049 objects: decoded in 114.7 ms, control-plane check 6.5 ms; `stage_slot` starts as **one task** on shard 0 |
| 56.651, 56.851, 57.050, 57.250 | w0, w1, w2 | `STATUS` to n3 fails at the 100 ms poll timeout (`ok=0 ms=99.3`); every poll from then on fails the same way |
| ~57.55 | w0, w1, w2 | n3 silent for ~1.14 s, more than `failover_ms` 1,000; promotion proposed |
| 57.564 | w1 | `witness_propose_promote group=1 to=1 term=1 result=OK` (Paxos, 13.5 ms) |
| 57.6-57.7 | n4 (standby) | promoted to term 1 |
| 61.482 | n3 | `stage_slot_phases`: check 203.5 ms, **put 19,808 objects 4,642.9 ms, of which index 4,527.0 ms and WAL 73.8 ms**, scan 114.4 ms |
| 61.499 | n3 | `shard_long_iter shard=0 total_ms=4977.5 inject_ms=4970.7`; `admin_status_slow total_ms=4946.9 repl_status_ms=4946.9` |
| 61.578 | n3 | durability wait ends `E_FENCED` (the new primary refused n3's term); the driver gets `NO E_FENCED` |
| 61.670 | w0 | n4 answers `role=n term=1`: the promoted node runs **with no replica** |
| 63.004-68.871 | n4 | the driver repeats the final copy on n4: one loop iteration of **5,867 ms (index 5,346 ms)**; witnesses suspect n4 for 5+ s but cannot promote (no standby left) |
| 69.738 | driver | `move ... done: OK`, then snc_ctl prints `STUCK no control member answered` |

### Where progress stopped, and why (MEASURED unless marked)

1. **The destination's shard 0 thread ran one task for 3.7-5.0 s.**
   `Engine::stage_slot` submits the whole final stage as one task
   (`on_shard`). The loop cannot drain rings, reap I/O, poll the network
   or run other control tasks until the task returns.
2. **Secondary-index maintenance was 97% of the put phase and 91% of
   the whole task** (4,527 of 4,643 ms, and of 4,977 ms). For each of the
   ~20K objects changed since the copy-ahead (the load writes the slot
   throughout), `put_image` erases and re-inserts the object's
   `Account.email` key. It does this even when the key is unchanged.
   `Shard::index_erase` removes from a `std::vector` posting list with
   `std::remove`, which is linear in the posting list
   (`src/core/shard.cpp`, `index_erase`).
   - `snc_load` and `lab.py` create every account with the same email
     (`"e"`). So one posting list holds all ~150K accounts of the shard
     (code read, `bench/snc_load.cpp` `cmd_create`), and each erase
     scans it.
   - Measured: index 4,527 ms of 4,643 ms put time (run 3); 5,346 of
     5,495 ms on the retry.
3. **`STATUS` is coupled to every shard thread.**
   - `snc_server` answers `STATUS` from `Engine::replication_status()`.
     That function submits a task to every shard and waits for all of
     them.
   - The admin accept loop is a single thread, so every later `STATUS`
     connection queues behind it.
   - Measured: one `STATUS` took 4,946.9 ms, all inside
     `replication_status`.
4. **The witness treats a `STATUS` slower than `poll_ms` as silence.**
   - `snc_witness` asks with `timeout = poll_ms` (100 ms in the lab).
   - After `failover_ms` (1,000 ms) of failed polls it proposes
     promotion, provided the standby answers as a standby and no
     degrade grant is on record.
   - Both conditions held. The standby was idle and the sync pair was
     not degraded.
5. **Contention made it worse, on loopback only.** Every `snc_server`
   pins shard *i* to CPU *i* (`NativeRuntime` default `pin = true`, no
   `--cpus`). So all four nodes' shard 0 threads share CPU 0.
   - Measured: during the stage, n3's shard thread got 40-88% of a CPU,
     and the run queue held 3-9 runnable threads.
   - The lab's own probe (`MOVE INFO` on every node, every ~100 ms)
     adds load. `slot_report` scans the shard's whole map, about
     75-250 ms of shard time each.
   - Outside the final stage, these scans and the other control tasks
     made 136-143 witness polls per run fail against the source
     group's primary.
   - The longest such streak was about 1.1 s, in run 1 at t ≈ 79 s,
     on group 0's primary while it received the move back. No
     promotion followed.
   - INFERRED (not traced): the promotion check also needs one more
     failed poll after the threshold and the standby to answer as a
     standby, so a streak just over 1 s may not reach a proposal.
   - So a group came close to a false failover without any final
     stage.

### Verified beyond the first plausible cause

Two first explanations were tested and rejected:

- The 13 MB `STAGE` line blocking the admin thread: it was read in
  22 ms.
- The WAL or disk: 74-100 ms of WAL append, and 0 ms I/O reaping in the
  long iteration.

`bench/stage_stall.cpp` reproduces the mechanism in one process. It uses
one engine, no lab, no other processes and no CPU sharing. A thread polls
`replication_status` every 5 ms while a final stage runs (MEASURED, EL1):

| Final stage | Stage time | Slowest `replication_status` during it |
| --- | --- | --- |
| 100,000 accounts, 20,000 changed, one shared email | 2,033 ms | 1,960 ms |
| 100,000 accounts, 20,000 changed, unique emails | 331 ms | 259 ms |
| 100,000 accounts, 0 changed, shared email | 324 ms | 257 ms |
| 300,000 accounts, 60,000 changed, unique emails | 1,197 ms | 940 ms |

So the shared index key is the amplifier: about 6x on 100K. The
structural cause is independent of it. One task does work proportional
to the slot, and health answers wait behind it. Even with unique keys, a
300K-object final stage holds `STATUS` for 0.94 s. That is above a
100 ms poll timeout, and close to a 1 s failover threshold.

### Consequences observed (classified per Part X)

- **Safety: PASS (OBSERVED, EL2).** There were never two serving groups
  (probe: 0 violations in all runs). The old primary was fenced by term
  and its stage answered `E_FENCED`.
- **Consistency: PASS (OBSERVED, EL2).** All 200,000 accounts were served
  exactly once, and the balances matched the acknowledged transfers.
- **Durability: PASS for acknowledged writes (OBSERVED, EL2).** One
  caveat follows from the next item.
- **Availability: DEGRADED (MEASURED).**
  - The destination group lost its primary to a false failover.
  - After that, the promoted node answered `role=n`: it runs with **no
    replica**. Its old primary is fenced for good until an operator
    rejoins it (`Engine::withdraw` is permanent).
  - From then on, every acknowledgement of that group is LOCAL_DURABLE
    only, and nothing reports this as a fault.
  - The retry on the new primary stalled its shard for 5.9 s
    (`STATUS` silent 5+ s). A group with no standby cannot fail over,
    so this was an outage that no promotion could end.
- **Liveness: PASS but fragile (OBSERVED).**
  - The moves finished (DONE) after one driver retry.
  - In run 3, the 60 s `snc_ctl` deadline expired just after the move
    completed, and the driver reported `STUCK`.
- **Performance: requires work (MEASURED).**
  - The forward move took 57-60 s, against 9.4 s for the ~31K-object
    move.
  - The copy-ahead is 391 sequential 256-object requests, each waiting
    for durability (18-30 ms each, mostly the durability wait).
- **Idempotency: not exercised.** The load client got no in-doubt
  answers (`0 of 0 in doubt`).

### A second defect found on the way: the driver's STUCK message (fixed)

`snc_ctl move` printed `STUCK no control member answered` in run 3. The
move had in fact committed and activated, and `done: OK` was logged.

- The real cause is the driver's own 60 s deadline (`Driver::deadline_ms`
  is 60,000 in `snc_ctl`). It expired before the loop could observe the
  move record cleared.
- The text came from `migrate::sync()`, which sets `why` before it tries
  and does not clear it on success.
- The PR #27 Vultr row "`--chunk 8` failed after 197 s: driver `STUCK no
  control member answered`" was attributed to a control-plane timeout.
  INFERRED: it was the same deadline and the same stale text (a 31K-object
  copy-ahead at 8 objects per request runs past 60 s inside one step).
  The real-machine rerun will confirm it.
- **Fixed**: `why` is cleared after a successful sync, and `STUCK` now
  names the deadline and the move state. No timeout was changed.
  **For review:** this changes the output of an operator tool
  (`tools/migrate.h`, used by `snc_ctl`), which is at the edge of "test,
  bench and instrumentation fixes". It changes no decision the driver
  makes. Revert it if Codex reads the authorization more narrowly.

### What this report does not establish

- No real-machine trace. On separate hosts, CPU sharing does not apply.
  INFERRED: the stall would be about the single-process numbers. The
  index-amplified 100K case would still exceed a 1 s threshold.
- Whether other single tasks share the property. INFERRED candidates:
  - `export_slot` final, 145-320 ms;
  - `slot_report`, 75-250 ms;
  - checkpoint, recovery, `rebuild_index`;
  - a non-final stage request of large size.

  All of these run as one task on a shard thread, and `STATUS` waits
  for every shard.
- Nothing here changes `failover_ms`, `poll_ms` or any timeout (Part
  XXXIII). The causal chain points instead at three separable fixes.
  None is implemented; see N.
  1. Bounded work per task: the final stage chunked and resumable
     (Part XI).
  2. Health answers that do not wait on shard threads.
  3. The index posting-list erase. Its cost is linear in the posting
     list, and `put_image` re-indexes an unchanged key.

## J. Resource and backpressure inventory

INFERRED (EL0, code reading) unless marked. For each resource: the bound,
what happens at it, whether the answer is retry-safe, and whether an
operator can see it. "STATUS" is the admin line a witness and an operator
read.

| Resource | Bound | At the bound | Retry-safe? | Visible? |
| --- | --- | --- | --- | --- |
| Follow-up backlog (`kEmitBacklog` 1024, `shard.h:270`) | counts ready + waiting follow-ups; not `emit_held_` or rings | only `Shard::execute` refuses `E_OVERLOADED`; creates, deletes, index lookups, forwarded requests and node input are **not gated** | yes: the duplicate check runs first [INV 18] | `ShardStats` only, not STATUS |
| Runtime rings (256, 1024 in `snc_server`) and overflow deque | the deque is bounded only for non-must-deliver messages; responses are must-deliver and **unbounded** | `E_OVERLOADED` before send | yes | `RingStats` only |
| Injected task queue (`NativeRuntime::submit`) | **unbounded**; drained completely in one iteration | slow tasks delay everything behind them (D) | n/a | `SNC_TRACE` long iterations |
| Account outbox (`kMaxOutbox` 64) / entries (1024) | per object | `E_APP kAppBusy`; near 1024 entries `E_TOO_LARGE` | yes (nothing recorded) | AUDIT `pending` |
| Target inbox full | per object | `kSettleRetry`: the source re-emits at once, **no backoff** | n/a (an obligation) | none |
| Replication retention | 1 GiB memory, 16 GiB disk per shard (disk enforced at checkpoint) | the standby must be reseeded; link stays down | n/a | `repl_lost` in `ShardStats`, **not in STATUS** |
| Sync blocked on the witness (`repl_blocked_`) | none | waiters, held follow-ups, client pipelining grow **without limit** | n/a | STATUS `blocked` |
| WAL active buffer | no limit below a `u32` capacity (doubles; `storage.cpp:561-570`) | a stalled disk gives no backpressure; INFERRED: past 2^31 bytes the doubling overflows (an endless loop or a too-small buffer) before memory runs out | n/a | flush histogram |
| WAL or checkpoint I/O error, ENOSPC at segment creation, WAL buffer OOM | — | **process abort** (`storage_fatal`, fail-stop by design) | nothing acknowledged is lost | stderr |
| Client connections, unanswered requests per connection | **none** (output capped at 8 MiB per connection pauses reading) | — | — | none |
| Cross-shard result allocation fails (`fill_resp`) | allocator failure for an out-of-line result | `E_NO_MEMORY` **after the op ran** (unsequenced only) | **no** | none |
| Migration final `STAGE` / `EXPORT` | one line (≤ 2^28 B) and **one shard task** for the whole slot | shard thread held for seconds (MEASURED, D) | the verbs are idempotent | `SNC_TRACE` |
| `wait_lsn` timeout in seal/stage | — | `E_OVERLOADED` though the step **was applied** | yes (idempotent verbs), but the status misleads | none |
| Sessions | **no count bound**; TTL only at checkpoint, and `snc_server` checkpoints only with `--checkpoint-s` | grow forever on a default server | — | `sessions_expired` |
| Session allocation | allocator failure | **process abort** after `apply` already changed the object (`shard.cpp:670-674`; INFERRED, EL0, from reading the code; reachable only with a heap limit or a real allocator failure) | the change was not logged, so recovery drops it; the client gets no answer | stderr |
| Follow-up messages dropped (each relies on the source's durable outbox and a later resume pass): link down (`shard.cpp:1831-1836`, counted `node_emits_dropped`); stale epoch (1780-1784, 1865-1868, counted `emits_stale`); misrouted (1325, 1785-1791, counted `node_emits_misrouted`); withdrawn shard (1860, **no counter**); any `emit_result` status other than NOT_FOUND / TYPE_MISMATCH / UNKNOWN_OP (1990, **no counter**) | none | dropped; re-emitted only on restart, link up, map change, unseal or activate | the obligation is durable; delivery is not | counters in `ShardStats` and AUDIT, not STATUS; two drops silent |
| Slot state allocation, resize after reserve, standby install OOM | allocator failure | **process abort** (`shard.cpp:2041-2043`, 1694, 1231-1233) | — | stderr |
| Object map | grows ×2 at load 0.8 | **one-step O(n) rehash on the shard thread** | — | none |
| Index posting list | **none**; erase is linear | the amplifier in D | — | none |
| Admin port | one thread, one connection at a time; AUDIT is O(objects) twice; MOVE handler threads unbounded | STATUS queues behind them | — | none |
| Witness | a thread per connection; one Paxos at a time per witness | INFERRED: a 3 s placement or lease proposal can delay DEGRADE for any group | — | none |

Classified per Part XVIII (impact: S safety, L liveness, P performance):

| Gap | Monopolizes a thread? | Starves the control plane? | Can refuse a durable obligation? | Impact |
| --- | --- | --- | --- | --- |
| O(map)/O(slot) shard tasks (stage, export, seal, slot_report, resume_all, rehash, expire_sessions, stats) | yes | **yes** (STATUS waits) | no | L (false failover), then S-adjacent (no replica) |
| Unbounded growth while durability stalls | no | no | no | L (memory) |
| Abort paths | n/a | n/a | n/a | L (failover recovers) |
| Silent follow-up drops | no | no | no (dropped, not refused; outbox keeps it) | L (unbounded settle delay) |
| `kSettleRetry` without backoff | partly (hot loop) | no | no | P, L |
| Admin thread: AUDIT, MOVE line reads | yes (admin thread) | **yes** | no | L |
| Witness: one Paxos at a time | yes (per witness) | **yes** (DEGRADE behind a 3 s proposal) | no | L |
| Misleading statuses | no | no | no | S for unsequenced `E_NO_MEMORY` (a resend applies twice) |

Most dangerous, in order:

1. **Health answers wait on shard work** (D, MEASURED). Every O(map) or
   O(slot) task delays STATUS; this is the false-failover mechanism.
2. **Unbounded growth while durability stalls**: waiters, held
   follow-ups, the WAL buffer, client pipelining and must-deliver rings.
   A blocked sync primary under load runs out of memory instead of
   refusing (INFERRED).
3. **Abort paths reachable from a client write** (session allocation),
   which a heap limit would make easy to hit.
4. **Silent obligation drops** recovered only by unrelated events (the
   follow-up row above; CORE_CONTRACTS section 13).
5. **Statuses that misstate what happened**: `E_NO_MEMORY` after the op
   ran; `E_OVERLOADED` from `wait_lsn` after the step was applied.
6. INFERRED **checkpoint/DEGRADE deadlock** with a witness: the main
   thread waits for durability that needs the standby, and is also the
   thread that would ask for the grant (INTERACTION_MATRIX row 13).

## K. Baseline 0.2 exit criteria (PROPOSED)

Baseline 0.2 is reached when every item holds, each with the evidence
level named. None holds today unless marked.

| # | Criterion | Evidence required |
| --- | --- | --- |
| K1 | CORE_CONTRACTS sections 5-13 reviewed; every PROPOSED rule ACCEPTED, changed or rejected; no UNDECIDED left on a path that can lose or duplicate an acknowledged write | review record |
| K2 | R08: an async standby is never promoted automatically; forced promotion requires the loss acknowledgement | EL2 (`test_async_failover` inverted), EL4 (one real-machine kill) |
| K3 | R11: an expired or unknown session never executes; no clock in that decision | EL2 (`test_session_ttl` inverted, clock-jump variant) |
| K4 | No false failover from the system's own work: the 100K move (and a 300K one) completes on loopback and on real machines with no promotion | EL4, traced |
| K5 | No shard task longer than the bounded-work budget under the P5 workload | MEASURED with `SNC_TRACE` |
| K6 | A node acknowledging below its configured class says so in STATUS (lost standby, degraded, async) | EL2 |
| K7 | Every status that says "refused" means "not applied" (`fill_resp`, `wait_lsn`) | EL1 |
| K8 | Failover during a slot move (INTERACTION_MATRIX M rank 1) and resend after `E_MOVED` (rank 2) tested | EL3 |
| K9 | ASan+UBSan and TSan clean on the full suite, including server child processes, on the commit under review | **holds today** for both: ASan+UBSan and TSan Python suites rerun with `log_path`, 0 reports from server children |
| K10 | The documentation contradictions in L fixed or recorded as accepted | review |
| K11 | Every invariant (1-18) mapped to at least one test, or marked UNPROVEN in EVIDENCE.md, with its strongest evidence level (Part XIX) | EVIDENCE.md |
| K12 | A deterministic regression for every distributed failure found nondeterministically (Part XX): today the R08, R11 and overload ones exist; the false failover (`bench/stage_stall` as a test with a budget) and the driver message do not | ctest |
| K13 | Clean RelWithDebInfo build and suite on the commit under review; fresh ASan+UBSan; TSan characterized | ctest logs naming the commit |
| K14 | Recovery barriers implemented as the accepted contract (CORE_CONTRACTS section 12) | EL2 |
| K15 | Every resource gap in J classified (accepted, bounded, or scheduled) | review |
| K16 | EVIDENCE.md, STATUS.md and AGENTS.md reconciled with the contracts; no silent contract/implementation contradiction | review |
| K17 | Real-machine reruns after the semantic changes (R08, R11, bounded stage) | EL4 |

## L. Contradictions

Between documents and code, or between documents (each was checked
against the source in this pass unless marked INFERRED).

| # | Where | Says | Code does | Severity |
| --- | --- | --- | --- | --- |
| L-1 | `REPLICATION.md:132-138` | "a write is acknowledged only if the node that will serve next has it"; a cut-off primary "is fenced twice" | an async primary acknowledges writes the standby lacks and the witness still promotes it; an async primary never asks DEGRADE, so it is not fenced by the witness | **safety** (R08) |
| L-2 | `REPLICATION.md:262` | a standby read "sees every write the primary acknowledged" | holds only for DURABLE, sync, non-degraded acks; not for `Durability::APPLIED` acks or `unlogged` ops | consistency |
| L-3 | `RETRIES.md:153-154` | a shard-count change "moves sessions with their slots … the index finds them on their new owner" | sessions move by their own ID's slot; one shard can hold several sessions per client and `find_session` reads only the first | idempotency (guarded only by the client's refusal) |
| L-4 | `RETRIES.md:30` | "Reads carry no sequence" | `net::Client` sequences every EXEC including reads; the server ignores it | harmless |
| L-5 | `include/snc/wire.h:3-5` | routing is "never a correctness requirement" | sequenced requests to a non-owner are refused `E_NOT_OWNER`, not forwarded | docs |
| L-6 | `RETRIES.md` | "MUTATE ops are sequenced" (deduplicated) | `unlogged` MUTATE ops are not deduplicated | idempotency |
| L-7 | EVIDENCE.md / PR #27 Vultr row | chunk-8 failure "STUCK no control member answered" attributed to the control plane | INFERRED: the driver's 60 s deadline with a stale message (D) | evidence |
| L-8 | `fill_resp` (`shard.cpp:1137-1141`) | `E_NO_MEMORY` reads as "refused" | the op ran | idempotency (unsequenced) |
| L-9 | `Engine::wait_lsn` | `E_OVERLOADED` reads as "refused, resend" | the seal or stage was applied | misleading; idempotent verbs make it safe |
| L-10 | `engine.cpp:48-49` comment | follow-ups wait until "Engine::recover, step 7" | they are released at the end of `Engine::start` | docs |
| L-11 | `finish_recovery` comment | a standby "serves no lookups" | true for index lookups; standby READs are served under a lease | docs |
| L-12 | `--rejoin` with `--control` | (no doc) | INFERRED: the directory is moved aside with the stored placement file and the node guard file. The NodeID survives (it comes from `--node-id`); a restart with the control plane down then fails with "no map is stored" (`control.h:170-172`) | operability |
| L-18 | CORE_CONTRACTS R-2 (proposed): a read reflects every acknowledged write | — | a cut-off old primary answers default reads of already-durable objects at once, from state the new primary may have overwritten | consistency (stale reads during a partition) |
| L-19 | FAILURE_MODEL S2/S6: "torn tail" handling | a bad CRC means a torn tail | a bad record in the middle of the last segment silently drops later acknowledged records | durability |
| L-13 | work order Part VII ("correctness MUST NOT depend on synchronized clocks") | — | session TTL (R11) and the standby read lease depend on clocks | safety (R11), consistency (lease) |
| L-14 | [INV 16] no externally visible computation before the recovery barrier | — | replication shipping, node links and `activate_staged` start before it (CORE_CONTRACTS I-b) | INFERRED safe for shipping; needs a decision (I-2) |
| L-15 | [INV 18] duplicates resolved before refusals | covers `E_MOVED`, `E_MIGRATING`, `E_OVERLOADED` | `E_STANDBY` and `E_NOT_OWNER` are returned before the duplicate check | INFERRED safe (the same sequence goes to the session holder); the invariant should name them as exempt |
| L-16 | FAILURE_MODEL G5 "serves nothing before the recovery barrier" | — | the space-UUID check runs after serving starts; in-memory mode has no barrier (I-c, I-h) | G1 briefly (operator error) |
| L-17 | the stale-term rule (CORE_CONTRACTS section 2) | an old term acknowledges nothing after a newer term is chosen | an async primary cut off from its standby keeps acknowledging LOCAL_DURABLE writes until it learns the new term | safety (R08) |

## Part X: correctness dimensions, reported separately

Across the whole core (not one run), as of this report:

| Dimension | Verdict | Evidence | Open |
| --- | --- | --- | --- |
| Safety (single owner, fencing) | **holds** in every test and run | EL3-EL4: `test_migrate` 67 crash points, Vultr P3/P5, probe 0 violations | failover during a move (M1), wrong data directory before the UUID check (I-c) |
| Consistency (single-object reads, transfers balance) | **holds** in every test; INFERRED gap: stale reads on a cut-off old primary (L-18) | EL2-EL4 balance checks in every lab run | standby lease clock assumption, index visibility rules undecided |
| Durability (acknowledged writes survive) | **holds for sync healthy** in every test; **violated for async** (R08); silently weakened after a false failover (no replica, D); INFERRED gap: mid-log WAL corruption in the last segment drops acknowledged records silently (L-19) | `test_async_failover`, D | K2, K6 |
| Idempotency (at most once) | **holds within the session horizon**; **violated past it** (R11) | `test_session_ttl`; 30K resends EL3 | K3, M2, L-3, L-6 |
| Liveness (progress after a fault) | holds for covered faults; obligations depend on events, no timer | P4 settled 26K after a cut | O-2 |
| Availability | **degraded by the system's own work** (false failover) | D, MEASURED | K4, K5 |
| Performance | **requires work** (MEASURED, D): the 100K move took 57-60 s; the copy-ahead is 391 sequential round trips; benchmarks of the hot path were not re-run | D | the bounded final stage and posting-list fix (N) |

## Sanitizer baseline (Part XXI)

Debug builds, `build-asan-b02` (`-fsanitize=address,undefined`,
`detect_leaks=1`, `halt_on_error=1`) and `build-tsan-b02`
(`-fsanitize=thread`). MEASURED, 2026-11-04.

| Build | Result | Classification |
| --- | --- | --- |
| ASan + UBSan | **16/16 pass**. The three Python suites rerun with `log_path` set so a report from any server child process would be written to a file: **0 reports** | clean |
| TSan, first run | 15/16; `test_cluster` failed: 12 data-race reports, all at `tests/test_cluster.cpp:773`, and the parent's CHECK on the child's exit status | **test defect**: `transfer_rounds` counted acknowledgements in a plain `u64` from completions that run on every shard's thread. The counter (`acked` in `transfer_rounds`) feeds the progress, fenced and status files and one `CHECK(... > 0)`, not the balance check; a lost increment could not change any earlier verdict. Fixed with `std::atomic<u64>`; no suppression |
| TSan, after the fix | **16/16 pass**, `test_cluster` 0 reports; the three Python suites rerun with `log_path`: **0 reports** from server child processes | clean |

Limits: the sanitizers saw only what the suite executes. They did not run
under the lab's fault injection or the 100K move.

## N. Recommended next slice (PROPOSED, not started)

**R08, option F-a: make an async group ineligible for automatic
promotion.**

- **Why this one.** It is the only measured loss of acknowledged
  writes in a covered single fault (500/500). Its contract is the work
  order's own default. It is small: one field in the witness's group HA
  state, set at `snc_ctl init`, and one check before a promotion is
  proposed, plus the operator path (`PROMOTE` with an expected term and
  an explicit loss acknowledgement).
- **What it does not touch.** The data plane, sessions, WAL, migration.
- **Proof.** CORE_CONTRACTS section 9's four tests, the existing
  witness and replication suites unchanged, and one real-machine kill.
- **Prerequisite.** Review and acceptance of CORE_CONTRACTS sections 8
  and 9 (F-1..F-4, PE-1, PE-2).

Next after it, in order: R11 session incarnation (section 10; larger,
needs the wire-version decision); then health decoupled from shard
threads (B-2), once the wedge rule B-3 is decided; then the chunked
final stage and the posting-list fix. The last two are the cheapest
fixes for D but are performance and availability, not safety.

## P. Tests and instrumentation still needed

Tests (none written; each proves a contract above):

1. `test_async_failover` inverted and the forced-promotion pair
   (CORE_CONTRACTS section 9).
2. `test_session_ttl` inverted, with clock-jump and restart variants
   (section 10).
3. Failover of the destination at each move step (M rank 1).
4. Resend to the new owner after `E_MOVED` (M rank 2).
5. Primary death while the standby seeds (M rank 4c); WAL bit rot mid-segment (4); stale reads on a cut-off primary (4b).
6. Two failovers during a resend storm (M rank 6).
7. Shard-count change with live sessions (M rank 7).
8. Checkpoint with a lost standby under a witness (M rank 9).
9. Fault injection for WAL EIO and ENOSPC (FAILURE_MODEL S3).
10. Flapping links on the lab (N7).
11. A regression test for the stage stall: `bench/stage_stall` turned
    into a CI check with a budget, once B-1 sets one (Part XX).
12. INTERACTION_MATRIX rows 32 (wrong data directory), 27 (checkpoint ×
    migration), 33 (promotion × index lookup), 17 (clock jump × lease),
    36-37 (allocation and disk faults), 14 (blocked sync × load), 19 and
    26 (witness majority loss): the tests named in its M table.
13. A deterministic regression for the driver's STUCK message (a driver
    against a fake control plane whose deadline expires after `sync()`
    succeeded).
14. Replication and WAL events in the stopgap trace (send, receive,
    durable ack; flush and fsync), and queue depths (injected tasks,
    rings): Part XII asked for them and this pass did not need them to
    find the cause, so they are not traced yet.

Instrumentation:

1. The trace rings of TRACING.md section 2, with a dump on promotion.
2. `repl_lost`, overload counters, ring counters, and "acknowledging
   below configured class" in STATUS.
3. A per-shard loop heartbeat readable without a shard task (B-2).

## Review and QA findings (Part XXXII)

Three-way discipline: this report and the specifications were written
first; an independent reviewer then checked every as-built claim against
the source; a QA pass then checked labels, contradictions, MUST/SHOULD
use, current-versus-intended confusion and work-order coverage. Both ran
on the drafts, with no access to the author's reasoning.

**Source review: 20 problems and 4 omissions, all applied.** The ones
that changed a conclusion:

1. A cut-off old sync primary **does** answer default reads of objects
   whose last write was already durable; the draft said those reads
   wait. Now a known gap (CORE_CONTRACTS section 5, L-18, M 4b).
2. A bad record in the middle of the last WAL segment is treated as a
   torn tail, so later acknowledged records are dropped **silently**;
   the draft called corruption "detected". Now a known gap (FAILURE_MODEL
   S6, L-19, M 4).
3. A restarted sync primary counts nothing as durable until its standby
   reconnects, and the recovery checkpoint after a shard-count change
   can wait for it inside `Engine::start`: a possible restart hang with
   a witness (CORE_CONTRACTS I-e2, row 19c). Omitted from the draft.
4. Sessions do expire at recovery when it checkpoints (shard-count
   change); session objects never move with their slot; `ClusterClient`
   resends after `E_MOVED` with the new group's own sequence, as new
   work. The draft had each of these wrong or loose.
5. A seeding or recovering standby cannot be promoted, because `STATUS`
   is served only after `Engine::start` (E6, E8): settled at EL0, moved
   down the ranking.
6. Smaller corrections: C5 (a node starts provisionally from its stored
   map), P3 (no checkpoint at stop), NodeID survives `--rejoin` (the
   stored map does not), lease blocks without `--control`, parallel
   promotion of shards, which follow-up drops are counted, the
   `fill_resp` trigger, creates ignoring `APPLIED`, the `test_session_ttl`
   scenario, the TSan counter's uses, witness-majority loss blocking a
   sync primary without a standby, the WAL buffer's `u32` bound.

**QA: 52 findings.** Applied: every label, contradiction, MUST/SHOULD
and current-versus-intended finding (for example: INV 18 does not cover
`E_STANDBY`/`E_NOT_OWNER`, now L-15; B-1 restored to the work order's
MUST with no unmeasured number; O-1 restated as Part XVII's atomicity
rule; I-2 flagged as an invariant 16 amendment, L-14; evidence levels
renamed EL0-EL6 to stop clashing with EVIDENCE.md's experiment names;
the interaction matrix reclassified with Part IX's labels and extended
with the missing prioritized pairs; the security and tenancy
placeholders of Parts XXVII-XXVIII written out; K extended to Part
XXIII's gate).

**Open after QA** (recorded, not done in this pass):

- Part XIX: strongest evidence level per major EVIDENCE.md claim (K11).
- Part XX: deterministic regressions for the false failover and the
  driver message (P 11, 13); the policy itself is in K12.
- Part XII: replication and WAL events and queue depths in the trace
  (P 14).
- The driver message fix's scope, above in D.
