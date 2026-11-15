# Persistent control-plane connections: design (2026-11-15, revision 4, for review)

Status: **design only; authorizes no code.**

This is the slice recommended in the PR #129 review (`docs/handoff/PR129_Connection_Churn_Review_Approval.md`):
"design persistent, reusable control-plane connections, preserving witness liveness detection, fencing, and
failover semantics." Background: `docs/TEST_MIGRATE_CONNECTION_CHURN.md` (C1). Revision 2 follows three
independent reviews (section 8). Revision 3 applies cody's review of PR #130
(`docs/handoff/PR130_Control_Connections_Review_Corrections.md`): HEALTH and the whole HA decision path stay
one-shot, and ambiguous request outcomes are stated as such (section 9). Revision 4 applies three
independent QA reviews of revision 3, which found that promotion itself depends on placement and lease
calls, and that idle connections could mask descriptor exhaustion (section 10).

Labels: **Observed** (source, log or counter), **Inferred**, **Open**.

## Summary

1. **Today every control request opens its own connection.** Each one is a `ha::ask`: open a TCP
   connection, send one line, read one line, close. The servers also close after one reply: `admin::Loop`,
   its worker and MOVE threads, and the witness's `serve()`. **Observed.**
2. **In `test_migrate` the test's own probe is most of the traffic.**
   - Counting every `ha::ask` in one untraced run, the `MOVE INFO` probe made 81% of the calls: 101,811 of
     125,695 in 49 s, about 2,000 a second. An independent rerun gave 100,806 of 124,616 (80.9%).
   - The product's own control traffic in that run was about 24,000 calls: witness Paxos, node placement
     polls and migration steps. **Observed.**
   - This corrects the strace-based attribution in `docs/TEST_MIGRATE_CONNECTION_CHURN.md` 2.1 (section 2.3).
3. **In production the steady churn comes from two sources.** Both are **inferred** from the code:
   - Health polls: each witness polls the primary of each group it watches about every 100 ms, so about
     10 × G connections a second per witness host for G groups. Each primary receives about 30 a second
     from 3 witnesses.
   - Placement polls: each node polls placement once a second (every 50 ms while an epoch hint is
     pending). Each poll costs 1 connection to a witness plus 3 Paxos READs among the witnesses.
4. **Proposed design.**
   - **The HA decision path stays one-shot.** Nothing used to detect a failure or to decide, carry out or
     complete a promotion changes (review decision, 4.5). That includes HEALTH, STATUS, PROMOTE, the
     readiness verbs, and the placement and lease calls a standby makes while promoting or passing its start
     barrier.
   - **Only listed requests may be kept** (4.2): the test probe, the migration driver's MOVE steps, the
     engine's periodic placement poll, and the Paxos READ broadcast of the PLACEMENT GET handler that answers
     it. Every other Paxos request stays one-shot, including the READ round that begins every `propose` and
     `read_linear`.
   - **Opt-in keep-alive**, negotiated per connection by an explicit handshake that old servers refuse quickly
     and harmlessly, and that never runs on a request's path.
   - **One request at a time**, each tagged with a sequence number that its reply echoes.
   - **No reuse after a failure:** the connection closes, with RST, on any timeout, error or mismatch.
   - **Ambiguous outcomes stay ambiguous.** A request that times out may still have executed, on a kept
     connection as on a one-shot one. Callers keep treating false as unknown; only read-only verbs are ever
     retried by the library (4.7).
   - **Bounded idle connections**, small enough to keep descriptor headroom, evicted least recently used and
     never by refusing active requests or by freeing descriptors for accept.
   - **A kill switch.**
   - **Unchanged:** liveness, fencing and failover still rest on per-request deadlines and on the terms,
     epochs and ballots in the payloads (section 4).
5. **Slices.** K0 switches the test probe; it needs the admin server's kept-connection handling, so it is not
   test-only, and the HEALTH baseline tests land first (section 6). K1 is the library and the servers, K2 the
   eligible product callers (HEALTH and the HA path excluded), K3 the measurement. Each is its own PR, stopped
   for review.
6. **What it does not reduce.** Witness health polls (about 10 × G connections a second per witness host)
   and every Paxos write stay one-shot, so in a deployment with many groups they may remain the largest
   steady source. Changing that needs its own evidence-based proposal.

## 1. Scope

**In scope:**
- `ha::ask`;
- every server that answers it: the node admin port (`admin::Loop` with its worker and MOVE threads) and
  the witness port (`serve()`, which also carries Paxos, placement and leases);
- the test harness's use of both.

**Out of scope:**
- replication and node links (already persistent);
- client data connections;
- any change to a verb's meaning, payload, term, epoch or ballot.

## 2. What opens connections today

### 2.1 Call sites (**Observed**)

| Caller | Verb(s) | Rate | Deadline |
|---|---|---|---|
| Witness watcher, per group (`snc_witness.cpp` `watch`) | HEALTH (STATUS fallback) to the current primary | about every `--poll-ms` (100 ms default; the sleep follows the ask) | `poll_ms` |
| Witness watcher, failover | STATUS to the standby (`ask_node`) | on failover only | `poll_ms` |
| Witness watcher, failover | PROMOTE to the promoted standby | at most every 50 ms while promoting | 1000 ms |
| Paxos (`paxos.h` `broadcast`), peers including the witness itself | READ (`read_latest`); PREPARE, ACCEPT, COMMIT (`propose`, `read_linear`) | per decision and per read | 300 ms per ask, inside a 3000 ms loop (every production call) |
| Engine placement poll (`control.h`) | PLACEMENT GET, answered with `read_latest` (one READ broadcast, no PREPARE) | every `placement_poll_ms` (1000 ms default; 300 in the test; 50 ms while an epoch hint is pending) | 2000 ms |
| Engine and harness | PLACEMENT SYNC (`read_linear`), INIT, CAS (`propose`) | on start and placement changes | 4000 to 5000 ms |
| Engine leases | LEASES TAKE | on demand | 4000 ms |
| `snc_server` readiness (`ask_witness`) | DEGRADE, NOTREADY, READY, SYNCED | on state changes | 1500 ms |
| `snc_server` start (rejoin, `--adopt-role`) | HEALTH, STATUS, MOVE ROLE to peers | at start only | 500 ms |
| Migration driver (`migrate.h`) | MOVE STAGE, EXPORT, SEAL, ... | per migration step | 15000 ms |
| `test_migrate` probe and `node_info` | MOVE INFO | tight loop, unpaced | 1000 / 2000 ms |

### 2.2 Counted (**Observed**)

`ha::ask` was instrumented to log every call (process, first two words of the verb); the instrumentation is
not committed. One untraced full `test_migrate` run (67 scenarios, 2 lanes, 49.1 s) made 125,695 calls:

| Process | Verb | Calls | Share |
|---|---|---|---|
| test (probe, `node_info`) | MOVE INFO | 101,811 | 81.0% |
| witness (Paxos) | READ placement | 8,661 | 6.9% |
| witness (Paxos) | PREPARE placement | 4,458 | 3.5% |
| node | PLACEMENT GET | 2,965 | 2.4% |
| witness (Paxos) | ACCEPT, COMMIT placement | 2,352 | 1.9% |
| witness (Paxos) | READ, PREPARE, ACCEPT, COMMIT leases | 1,800 | 1.4% |
| test, node | PLACEMENT SYNC | 1,161 | 0.9% |
| test | MOVE ROLE, STAGE, REFRESH, HAVE, EXPORT, SEAL, ACTIVATE, UNSEAL; PLACEMENT CAS, INIT | 2,337 | 1.9% |
| node | LEASES TAKE | 150 | 0.1% |

Reading the table:

- **Paxos READs.** A PLACEMENT GET costs one READ to each of the 3 witnesses (self included). 8,661 READs is
  slightly fewer than 3 × 2,965, so some GETs did not reach a live witness.
- **Paxos PREPAREs.** They come from SYNC, CAS and INIT, not from GET.
- **No health polls.** The test's witnesses run placement only (no `--pair`), so there are no HEALTH polls.
- **Independent rerun.** A rerun from a separate copy of the tree reproduced every row within 1.4%.

### 2.3 Correction to the churn document

`docs/TEST_MIGRATE_CONNECTION_CHURN.md` 2.1 used two `strace -f` runs and said the probe made "about a
quarter" of the connections. It made 81%.

- **Why the trace was wrong.** The tracer stops every syscall of the probe's tight loop, which throttles it
  far more than the paced callers. The traced run took 95 s, against 49 s untraced.
- **Rows with no real caller.** Its caller table credits connects to "witness → node admin (health polls)"
  and "node → node admin (peer health)". Neither happens in this test, so those rows were misattributed,
  most likely to the probe's threads.
- **What carries the hosted load.** The hosted conntrack load (about 2,000 new connections a second, about
  100k per run) is mostly the probe. **Inferred** from the counts; the rate matches: 101,811 / 49 s ≈ 2,080
  a second.

## 3. Semantics to preserve

| Property | Today | Where it lives |
|---|---|---|
| **Silence is failure** | A request without a full reply by its deadline returns false; the witness counts that toward `failover_ms` | `ha::ask`; `watch` |
| **No stale replies** | Structural: each request has its own socket, so a late reply is never read | one connection per request |
| **A failed connect means not sent** | A request whose connect fails was never delivered. Any failure after the connect (timeout, short reply) is ambiguous | one connection per request |
| **HEALTH is never behind heavy work** | HEALTH (exact match) is answered inline on the admin loop; STATUS, AUDIT, PROMOTE and others go in order to one worker (`kQueueMax` 16, then `NO busy`); MOVE gets its own thread | `admin_loop.h`, `docs/HEALTH_DECOUPLING.md` |
| **Accept path in the health check** | Every poll exercises accept: a node out of descriptors (EMFILE) or with a stuck loop reads as silent | one connection per request |
| **Bounded server state** | Admin: `kHalfMax` 256 half-read connections, each with a line due within `kReadMs` (500 ms) of accept; `kQueueMax`; `kOutMax` 64 waiting replies. Witness: one thread per connection, **no cap** | `admin_loop.h`; `snc_witness.cpp` |
| **Fencing** | Terms, epochs, ballots and space UUIDs are in payloads and checked by the receiver. Exception: PROMOTE carries no term (`snc_server.cpp`: if a standby, SIGUSR1); its safety rests on the Paxos decision before it and on reaching the right process | handlers |
| **Ambiguous outcome** | A request whose reply is lost or late may or may not have taken effect: the server may have received and executed it before the reply was lost. Callers treat false as unknown, never as "not executed" (table in 4.7); the migration driver re-reads state before retrying a step (`migrate.h`, `"retry"` paths) | callers |

## 4. Proposed design

### 4.1 Wire

- **Handshake per connection.** A client wanting keep-alive opens a connection and sends `KEEP 1` as its
  first line.
  - A new server answers `OK keep` and keeps the connection.
  - Any other answer means the server does not support it, and the connection is closed. That includes
    `NO unknown request`, `NO busy`, a closed connection or a timeout.
  - The client then uses one-shot `ha::ask` for that peer. It caches that as "one-shot" until the next
    handshake attempt, at most once per 60 s.
  - Nothing is ever sent prefixed to a server not known, on that connection, to support it. That closes
    the rollout hazards found in review: `K HEALTH` landing in an old node's heavy queue, and a large
    `K MOVE` refused as too long (B1, S4).
- **Handshake on new admin servers.** `KEEP` is an inline verb there, answered on the loop without touching
  a shard.
- **Handshake on old admin nodes.** `KEEP` is unknown and goes to the heavy queue, so its answer may be slow
  or `NO busy`. Either way, the handshake fails and costs only that one attempt.
- **Never on a request's path.** A request with no free confirmed kept connection goes one-shot, exactly
  as today. The pool runs handshakes in the background (one at a time per peer and lane, at most once per
  60 s after a failure) and only later requests use the result. So a handshake can never add latency to a
  request, and in particular never to a witness decision.
- **Who handshakes with a node's admin port.** Only the eligible admin callers (the test probe and the
  migration driver, 4.2). Witnesses and the HA path never send `KEEP 1` to a node, so during a rollout no
  handshake can sit in an old node's heavy queue ahead of a witness's STATUS or PROMOTE.
- **Requests on a kept connection.** Each request is `K <seq> <line>`, where `seq` counts up per
  connection, and the reply is `<seq> <reply>`. A reply whose sequence number does not match closes the
  connection. That gives defense in depth for the "no stale replies" property (review S7).
- **Unchanged.** Bare requests from old clients work as now. Verbs and replies are unchanged.
- **No newlines in replies.** No reply may contain a newline; the server asserts it.

### 4.2 Client (`ha::Conn`, `ha::Pool`)

- **Eligible requests (closed list).** Only these may use the pool; every other `ha::ask` call stays
  one-shot, and a new caller is one-shot unless a reviewed change adds it:
  - the `test_migrate` probe and `node_info` (MOVE INFO);
  - the migration driver's MOVE steps (`migrate.h`);
  - the engine's periodic placement poll and epoch-hint polls (PLACEMENT GET, `control.h`), but not the
    GET in `resolve` at node start, the PLACEMENT SYNC of the start barrier or of a promotion, nor LEASES
    TAKE (4.5);
  - the READ broadcast that the witness's PLACEMENT GET handler makes through `read_latest`. `propose` and
    `read_linear` also begin with `read_latest` (`paxos.h`), so the choice cannot sit in `read_latest`
    itself: it is a `pooled` parameter on `read_latest`, false by default, that only the PLACEMENT GET
    handler sets. Every READ inside `propose` and `read_linear`, on every machine, stays one-shot.
- **Pool and lanes.** The pool is keyed by (host, port, lane). The lanes are control, move and probe, so a
  short request never shares a connection with a long one.
  - Each key holds a small free list, filled only by background handshakes and capped at 4.
  - A caller never waits for a checkout: if no confirmed connection is free, it uses a one-shot `ha::ask`.
    Paxos `read_latest` broadcasts from several serve threads therefore never queue behind each other.
- **One request per connection.** A connection holds at most one request.
- **Deadline.** Today `ha::ask` waits up to `timeout_ms` for connect, then sends with a blocking
  `write_all` that has no deadline, then reads the reply with a fresh `timeout_ms` (`ha.h`), so its real
  budget is up to twice the timeout plus an unbounded send. On a kept connection there is no connect; the
  send is non-blocking and bounded by `timeout_ms`, and the reply then has `timeout_ms` from the end of the
  send. That keeps today's reply budget and removes the unbounded send. One-shot `ha::ask` is unchanged.
- **Close on failure, with RST.** On timeout, short reply, EOF, error or sequence mismatch, the connection
  is closed with RST (`SO_LINGER {1, 0}`) and never reused. This is hygiene, not a guarantee: the RST ends
  the client's side and stops its retransmissions, but a request the server already received may still run
  (4.5).
- **`TCP_USER_TIMEOUT`** is set to the request deadline on kept sockets, so a connection whose data stays
  unacknowledged is dropped rather than retransmitted for minutes. It bounds how long the client keeps
  trying to deliver; it does not bound when, or whether, the server executes what it already received.
- **Socket options.** Kept sockets set `TCP_NODELAY`, and each request is one `send`.
- **Reuse check.** Before reuse, an idle connection is polled with a 0 timeout. If it is readable (EOF, RST
  or stray bytes) or in error, it is closed, and the request goes one-shot.
- **Retry.** After a request has been written there is no retry, except one, sent one-shot within the
  same deadline. That retry needs all three of:
  - the verb is in the read-only allowlist (4.7: MOVE INFO, MOVE ROLE, MOVE HAVE, READ, PLACEMENT GET); no
    other verb, idempotent or not, is ever retried by the library;
  - the connection was reused, not new;
  - no reply byte arrived.

  This covers a server's idle close crossing the request (review S5). Every other failure returns false,
  as today, and false still means "unknown", never "not executed". The client closes idle connections
  (20 s) before the server does (30 s), so this race arises only from a server restart or eviction.
- **Idle limit.** Kept connections close after 20 s idle.
- **Kill switch.** `SNC_HA_KEEP=0` (environment) and a `--no-keep` flag on servers turn keep-alive off end
  to end. The full suite runs both ways.

### 4.3 Admin server (`admin::Loop`)

- **Routing.** A connection whose first line is `KEEP 1` is marked kept. On a kept connection, `K <seq> `
  is stripped before routing, so `MOVE ` is still recognized by prefix and streamed as now. A test checks
  that `K <seq> MOVE INFO` reaches the MOVE path (review hazard 3).
- **Line deadline.** On a connection that is not kept, which includes every HEALTH, STATUS and PROMOTE,
  nothing changes: its line is due within `kReadMs` of accept, it holds a `kHalfMax` slot until then, and
  the connection closes after its reply. The `KEEP 1` line is also due within `kReadMs` of accept. Only on a
  kept connection, after the handshake, does a request's deadline start at its first byte
  (`line_started_ns`), the MOVE thread's too, with a separate idle deadline (30 s). Idle kept connections
  live in their own set, never in the half-read set, so they cannot take a `kHalfMax` slot from a one-shot
  HEALTH.
- **One handback path** (kept connections only; one-shot replies still close as today).
  - Problem today: the worker and the MOVE thread each write and then close.
  - On a kept connection, both instead pass the fd and its sequence number back to the loop through a
    handback queue. They write with a write deadline (the worker's write has none today).
  - An eventfd in the poll set wakes the loop at once, so no request waits for the loop's 100 ms tick.
  - The loop restores O_NONBLOCK and puts the fd back in its read set.
  - The inline and POLLOUT reply paths do the same instead of closing.
- **Idle connections.**
  - They have their own small cap, `kIdleMax`, sized against RLIMIT_NOFILE (proposed: the smaller of 64
    and a sixteenth of the limit). Only the probe and the migration driver keep admin connections, so a few
    are expected.
  - Past the cap they are evicted least recently used. On EMFILE nothing changes: the loop frees the oldest
    half-read connection or backs off, as today, and never closes an idle kept connection to make room for
    an accept. A node out of descriptors therefore stays silent to one-shot HEALTH exactly as today.
  - `stop()` closes idle and handback fds.
- **Bytes after the request line.** A per-connection buffer keeps them. Any bytes after a request line,
  before its reply is sent, are a protocol error: the connection is closed.

### 4.4 Witness server (`serve()`)

- **Thread per connection.** A kept connection's thread loops: read the next line (deadline from its first
  byte, idle deadline 30 s), answer, repeat.
- **Bounds.**
  - Idle kept connections are bounded separately from active ones, by a cap sized against RLIMIT_NOFILE and
    the thread limit, with headroom kept for one-shot traffic (proposed: the smaller of 64 and a sixteenth
    of the descriptor limit). Past the cap, the least recently used idle connection is closed (review B2).
    Only nodes' placement polls and peers' READs are kept, so about one per node and two per peer.
  - Requests are never refused because of idle connections. Active connections stay unbounded, as today, so
    no new `NO busy` reply exists and `ask_witness` is unchanged.
  - Self-asks over TCP count toward the idle bound like any other client; the witness's own Paxos writes
    are one-shot and never idle.
- **Pre-existing gaps, not changed here.** The witness's accept loop has no EMFILE handling and spins
  (`snc_witness.cpp`), and `ha::ask` does not check `socket()`'s result (`ha.h`). Both are current behavior on
  the HA path; they are recorded for their own slice rather than folded into this one.

### 4.5 Liveness, fencing, failover

- **The HA decision path is unchanged (review decision).** These stay one-shot `ha::ask` calls with today's
  deadlines, and K2 does not touch them:
  - the witness watcher: HEALTH with its STATUS fallback, STATUS to the standby, PROMOTE (`watch`);
  - every Paxos request of `propose` and `read_linear` on every machine (their opening READ round, PREPARE,
    ACCEPT and COMMIT), to peers and to self.
    That covers the `ha/<group>` decisions of `watch`, readiness and start, and the `placement` and `leases`
    writes;
  - `snc_server`'s readiness requests (`ask_witness`: DEGRADE, NOTREADY, READY, SYNCED);
  - the standby's promotion and start barrier: PROMOTE raises SIGUSR1 and the engine's `promote` first runs
    PLACEMENT SYNC (the start barrier) and LEASES TAKE (`engine.cpp`, through `control.h`). Either failing
    refuses the promotion, so both stay one-shot, and so do the `read_linear` on `placement` and the
    `propose` on `leases` that serve them at the witness;
  - the start-time rejoin asks (HEALTH, STATUS, MOVE ROLE to peers).

  Every HEALTH still opens a connection, so it still exercises accept: a node out of descriptors, with a full
  backlog or a stalled accept loop reads as silent exactly as today (4.3 keeps today's EMFILE rule). One
  difference is stated: idle kept connections hold up to `kIdleMax` descriptors, so a process near its limit
  reaches exhaustion that many descriptors sooner. On a node that makes one-shot HEALTH fail earlier; on a
  witness it makes its own outbound requests fail earlier. The small caps bound it, and A2 tests it.
  Detection stays within `failover_ms` plus up to 2 × `poll_ms`, with no added window.
- **No pooling delay on witness decisions or on promotion.** No checkout, handshake, reuse check, eviction or
  retry is on a failure-detection, decision or promotion path, because no request on those paths is in the
  eligible list (4.2), including the READ round inside every `propose` and `read_linear`. One existing
  coupling is unchanged and stated: one `paxos::Node` per witness serializes `read_linear` and `propose` for
  all machines on one mutex (`one_at_a_time_`), so an `ha/*` decision can already wait behind a placement or
  lease decision. The pooled READ of the PLACEMENT GET handler runs outside that mutex and never exceeds its
  300 ms per-ask deadline.
- **Server-side changes do not reach one-shot requests.** K1's admin and witness changes apply to kept
  connections only (4.3, 4.4); a connection that does not send `KEEP 1` is handled byte for byte as today.
  The one shared resource, descriptors, is protected by the small idle caps.
- **Ambiguous outcomes (corrected).** No timeout, RST or `TCP_USER_TIMEOUT` proves a request did not run. A
  request the server has received may execute after the caller gave up, and its reply may be lost or arrive
  late. That is true today for one-shot requests, and on a kept connection it also covers a request written
  into an established connection that is then partitioned. What the design does guarantee:
  - **No misattributed reply.** A reply is read only on the connection that sent its request, only for the
    sequence number it echoes; a late reply after a timeout is never read, because the connection was closed,
    and a mismatched one closes the connection.
  - **No reuse after failure.** A connection that timed out or erred is never used again.
  - **No silent retry.** The library retries only read-only verbs (4.7). Every other failure is returned to
    the caller as unknown, and the caller's own logic, unchanged, decides. That logic already includes
    caller-level retries that rely on idempotence: the migration driver resends an identical STAGE after no
    answer (`migrate.h` `stage_resend`) and re-reads placement on its `"retry"` paths.
  - **Fencing still decides.** A request that executes late is judged by its handler's check, as a delayed
    one-shot request is today, and is rejected when it has been overtaken:
    - a late PREPARE is NACKed if a higher or equal ballot was promised since; a late ACCEPT is NACKed if a
      higher ballot was promised since, answered `LATE` if its version is chosen, and otherwise accepted,
      which Paxos permits (`paxos.h`);
    - a late CAS fails its version check (`NO version`);
    - a late MOVE step is judged against the placement read when it runs: SEAL and STAGE are checked
      against the move id and a linearizable read, a final EXPORT needs the slot sealed by that move and
      durable, and UNSEAL is refused while its move is still in progress (`migrate.h`). A non-final EXPORT
      is not fenced and only reads; ACTIVATE and REFRESH are local refreshes, harmless to repeat.
- **Fencing.** No request gains authority from its connection, and every term, epoch, ballot and move-id check
  stays in its handler. PROMOTE has no term and stays one-shot.

### 4.6 What it costs

- **Server resources.** One descriptor per idle admin connection; one thread per idle witness connection.
  Witness counts grow with the number of nodes (placement polls; readiness and leases stay one-shot): about 1 to 2 per node per
  witness. The caps and LRU eviction bound them.
- **Middleboxes.** Ones that drop idle flows silently cause one timed-out request per pooled connection,
  then a reconnect. The idle limit keeps flows short.
  - **Open:** deployments' middlebox idle timeouts.
- **Complexity.** A pool, a handshake, a handback path and sequence numbers. Sections 4.2 to 4.4 put each
  in one place.

### 4.7 Verbs: read-only, idempotent, or not (**Observed** from the handlers)

| Verb | Class | If a lost reply hides that it ran |
|---|---|---|
| HEALTH, STATUS, GAUGES | read-only; never pooled (HA path) | nothing |
| MOVE INFO, MOVE ROLE, MOVE HAVE, READ, PLACEMENT GET | read-only (retry allowed, 4.2); GET also records a learned value locally, which is monotone | nothing |
| PLACEMENT SYNC | safe to repeat, but writes (promises, may finish an accepted value); serialized per witness; never pooled or retried by the library | nothing; a repeat may queue behind the first |
| COMMIT, ACCEPT (same ballot), PLACEMENT INIT (same body), MOVE STAGE/EXPORT/SEAL/ACTIVATE/REFRESH | idempotent | nothing; the caller re-reads. The migration driver already resends STAGE |
| MOVE UNSEAL | conditional: refused while its move is in progress, else unseals once | nothing; a repeat finds the slot not sealed |
| PREPARE | repeat at the same ballot is NACKed | liveness only |
| PLACEMENT CAS | version-checked | the retry reads `NO version`; the caller re-reads, as today |
| LEASES TAKE | not idempotent | a block of leases is wasted; never retried by the library |
| NOTREADY, DEGRADE | each chooses a new version, by design | pessimistic; READY names `after=N` |
| READY | conditional on `after=N` | refused if overtaken |
| SYNCED | always refused today (no causal base) | nothing |
| PROMOTE | no term in the payload | stays one-shot (HA path); Paxos decides first |

## 5. Acceptance criteria

Each criterion states something the implementation can establish and a test can check.

| # | Criterion | How |
|---|---|---|
| A1 | `test_migrate` connections per run fall at least 10× (conntrack peak from a drained table, from about 100k), with descriptor and thread counts reported. **Inferred** from 2.2: the eligible requests made about 116k of 126k calls, so about 12× is expected | hosted, 6 runners, as the churn document's arm A |
| A2 | **HEALTH and failover unchanged.** (a) Every request on the HA path (4.5: HEALTH, STATUS, PROMOTE, readiness verbs, rejoin asks, promotion and start-barrier PLACEMENT SYNC and LEASES TAKE, every READ, PREPARE, ACCEPT and COMMIT of `propose` and `read_linear`) is one connection, counted per verb before and after each slice. (b) For SIGKILL, SIGSTOP, a partition, a stalled accept loop and descriptor exhaustion (EMFILE), the witness's suspect and promotion events have the same outcome as on main, and their times stay within `failover_ms` + 2 × `poll_ms` (suspect) and within main's measured range (promotion), over the same number of trials on the same runner class. (c) A healthy primary is never promoted while kept connections are idle, evicted or reset. (d) Near its descriptor limit with the idle set full, a primary's one-shot HEALTH and a witness's outbound requests behave as on main with the same number of extra descriptors held | (a) a per-verb, per-caller connect counter in `ha::ask`, compiled only into test builds, so the READ inside a `propose` is counted apart from the PLACEMENT GET handler's. (b) `test_health` T4 (SIGKILL), T5 (SIGSTOP) and T6 (partition, its `Proxy`) reused, plus a clustered variant with placement and leases that records detection and promotion times; the stalled accept uses a `DEBUG ACCEPT-HOLD` hook behind `--debug-admin`; EMFILE is induced in the admin loop and asserted on its evicted and EMFILE counters as well as on promotion, run with the idle set populated. The hook and the baseline land first, against main (section 6) |
| A3 | **Ambiguous outcomes handled, not denied.** (a) Late reply: a reply delayed past its deadline is never returned to a later request; the connection is closed. (b) Half-open: a peer that vanished without FIN or RST fails the request at its deadline, including a request larger than the socket buffer, and the connection is not reused. (c) Partition, both directions, on an established kept connection: the request fails at its deadline; after healing, the next request uses a new connection and gets its own reply. (d) Execute, then lose the reply: the request has taken effect, the caller got false, no library retry was sent for a non-read-only verb, and a re-read shows the effect once. (e) A request delivered late is judged by its handler's check and rejected when overtaken (a higher ballot promised first, a newer version, a finished move) | a test-controlled TCP relay that can delay, drop or black-hole each direction (no firewall changes); for (e) it keeps forwarding held client-to-server bytes after the client has given up and reset. The relay terminates TCP, so it does not exercise `TCP_USER_TIMEOUT` or kernel retransmission; no criterion claims those. A debug hook drops a reply after executing, behind `--debug-admin` on nodes and `--debug 1` on witnesses |
| A4 | No stale reply or desynchronized connection: a wrong sequence number or bytes after a line each close the connection, and the next request gets its own reply | new deterministic tests |
| A5 | Idle eviction, a server restart, a handshake to an old server: at most one failed request per pooled connection, on eligible paths only, never a false failover; read-only verbs recover by the retry | new tests |
| A6 | HEALTH is never delayed by a heavy request or by kept connections: with the idle set full, the heavy worker stalled (`DEBUG STALL`) and the queue full, a one-shot HEALTH is answered inline; a new witness polling an old node never promotes | `test_admin_loop.cpp`; mixed-version test |
| A7 | Witness with more than its idle cap of kept clients still completes a promotion and a grant | new test |
| A8 | Passing with keep-alive on and, separately, with `SNC_HA_KEEP=0`: `test_health` (T1 to T8, T10, T10b, T11), the grant-vs-promotion race in `test_witness.py`, `test_paxos_ballots`, `test_ready_order`, q10 (`test_baseline_move.py`), `test_async_failover.py`, `test_ckpt_failover.py`, `test_migrate`, the A2 and A3 tests, and the full suite | CI |

## 6. Slices (each its own PR, stopped for review)

- **Before K0: HEALTH baseline.** A small PR adds the `DEBUG ACCEPT-HOLD` hook (behind `--debug-admin`),
  the A2 per-verb counter for test builds, and the A2 and A6 tests, all run against main, so the baseline is
  recorded before anything changes. It changes no request handling. (This adds a step before K0; it is the
  only change to the review's order and is put to review.)
- **K0, test probe.** The `test_migrate` probe and `node_info` keep one connection per node for the scenario.
  - It needs the admin server's kept-connection handling (4.3), which is product server code, and a
    client. K0 therefore brings `ha::Conn` (one kept connection, handshake, sequence numbers, close on
    failure) used only by the test probe, plus the admin side of 4.3 and `--no-keep`. One-shot handling,
    HEALTH included, is unchanged, and the baseline tests from the step before must pass unchanged.
  - Its acceptance includes A4 and A6 with the idle set populated, since it ships the sequence numbers, the
    handback and kept routing.
  - That removes about 80% of the test's churn, and no product caller changes behavior.
- **K1, library and servers.** `ha::Pool`, the eligible list, the witness changes and the rest of 4.3. Then
  A3, A5 and A7, and A4 and A6 again. No product caller changes.
- **K2, eligible product callers.** In order of churn: the PLACEMENT GET handler's READ broadcast, the
  engine's periodic placement poll, then the migration driver's MOVE steps. Each is switched behind the pool,
  and A2 and A8 run before the next one is switched. HEALTH and the HA path (4.5) are excluded.
- **K3, measurement.** Hosted A1 and A2, before and after: connection counts, request latency, descriptor and
  thread use, and correctness. If A1 holds, lower the PR #129 guard's per-scenario figure, with evidence.

Each slice is its own PR with its own independent QA; none is combined with another.

## 7. Alternatives considered

| Alternative | Why not, or not alone |
|---|---|
| Close with RST on every request | Shortens conntrack's hold to about 10 s but keeps the connect rate. Arm D of the churn document shows a short time-wait alone still filled a 50000 table. It also changes close behavior for every peer |
| Pace the test's probe | It would cut the test's churn with no product change, but it loses detection of two serving owners between probes; not measured. K0 gets most of the gain without that loss |
| Multiplexed protocol (request ids, pipelining) | Needs per-request routing and per-id head-of-line rules on both ends. Much larger, for no need shown at these rates |
| gRPC or another framework | A new dependency and a rewrite of every verb (Invariant 6); not justified by the measured need |

## 8. Independent QA (revision 1 → 2)

Three reviewers had no brief beyond their area: the design against the code, the distributed-systems
semantics, and the evidence (one rebuilt and recounted). Every finding below was checked in the source
before it was applied.

| Finding | Severity | Applied |
|---|---|---|
| `K HEALTH` to an old node fails the inline exact match (`snc_server.cpp` `line == "HEALTH"`) and queues behind heavy verbs; it times out without `NO unknown request`, so the client never learns, and a false failover returns | blocking | Per-connection `KEEP 1` handshake; nothing prefixed to unconfirmed peers; handshakes never on a request's path (4.1); revision 3 keeps HEALTH one-shot |
| A witness cap that refuses new connections would refuse Paxos peers and readiness once idle clients fill it | blocking | LRU eviction of idle connections (4.4). Revision 3: no active cap and no `NO busy`, so `ask_witness` is unchanged |
| `K MOVE` misses the `MOVE ` prefix routing; the K1 probe switch would silently fall back | high | Strip `K <seq>` before routing; test (4.3) |
| Heavy worker had no handback; deadlines keyed to accept time; no wake on handback; LRU eviction and EMFILE | high | One handback path with an eventfd; line deadline from the first byte; LRU on cap and on EMFILE (4.3). Superseded in revisions 3 and 4: first-byte deadlines for kept connections only, and no eviction on EMFILE |
| Late delivery on established connections is new | should-fix | `TCP_USER_TIMEOUT`, RST on failure, stated in 4.5, test in A3. Reframed in revision 3: late execution happens with one-shot requests too; RST and `TCP_USER_TIMEOUT` are hygiene only (4.5) |
| The health check stops exercising accept and EMFILE | should-fix | Revision 2: 5 s maximum age. Superseded in revision 3: HEALTH stays one-shot (section 9) |
| One connection per key would queue concurrent `read_latest` broadcasts | should-fix | Small free list per key; never wait for a checkout (4.2) |
| "No stale replies" rests on discipline only | should-fix | Sequence numbers echoed; newline assertion; buffered readers (4.1, 4.3) |
| A5 "no false failure" cannot hold | should-fix | Restated; read-only retry allowlist (4.2, A5) |
| No idempotency table; PROMOTE has no term | should-fix | Table 4.7; section 3 corrected; PROMOTE stays one-shot |
| No kill switch | should-fix | `SNC_HA_KEEP=0`, `--no-keep`; suite both ways (4.2, A8) |
| PLACEMENT GET is `read_latest`, not linear; PREPAREs come from SYNC, CAS and INIT | wrong claim | 2.1, 2.2 |
| Production rates per witness host were 3× high (30 × G is what primaries receive); 4, not 5, connections per placement poll | wrong claim | Summary 3 |
| Deadlines: standby STATUS is `poll_ms`; Paxos is 300 ms per ask | wrong claim | 2.1 |
| "Accept queue full, request still reaches a running loop" was wrong: a running loop drains its accept queue every turn | wrong claim | Removed; EMFILE handled in 4.5 |
| The churn document's caller rows for health polls have no real caller | understated | 2.3 |
| A2's bound and kill-only test; missing tests | gaps | A2 to A8 rewritten |

## 9. Review of revision 2 and revision 3 changes

cody's review (`docs/handoff/PR130_Control_Connections_Review_Corrections.md`) approved the direction and
required two corrections before any implementation.

| Review requirement | Revision 3 |
|---|---|
| Keep HEALTH one-shot; no five-second window; HEALTH on its fast path | Health lane and maximum age removed (4.2); HEALTH, STATUS, PROMOTE one-shot (4.5); the admin loop's one-shot handling is unchanged (4.3) |
| Preserve witness liveness and failover timing | The whole HA decision path stays one-shot, including `ha/*` Paxos and readiness (4.5); A2 compares against main |
| Pooling must not delay witness decisions | Watcher and `ha/*` machines never use the pool; handshakes never run on a request's path (4.1, 4.5) |
| Regression coverage: stalled accept, EMFILE, SIGSTOP, SIGKILL, partitions | A2(b), landed first in K1 and run against main (revision 4: in its own step before K0, section 6) |
| A timeout is not proof of non-execution | Stated in sections 3, 4.2 and 4.5; RST and `TCP_USER_TIMEOUT` described as hygiene, not guarantees |
| Preserve fencing; never retry non-idempotent operations; retries only for classified safe verbs | Fencing unchanged (4.5); library retry only for the read-only allowlist (4.2, 4.7) |
| Keep sequence-number validation | 4.1, 4.5, A3(a), A4 |
| Test late replies, half-open, partitions, reconnection, execute-then-lose-reply | A3(a) to (e) |
| Acceptance criteria must be establishable | A2, A3, A8 rewritten |
| Keep K0 to K3 as separate PRs; HEALTH one-shot in K2 | Section 6 |

Consistency changes found while applying the review, each checked in the code:
- **One-shot deadlines must not move.** Revision 2 moved the admin line deadline from accept to the first
  byte for every connection. A connection that sends nothing would then have held a `kHalfMax` slot for up
  to 30 s instead of 500 ms (`admin_loop.h`, `kReadMs` from `accepted_ns`), so idle clients could crowd out
  one-shot HEALTH. Revision 3 keeps today's deadline for every connection that is not kept (4.3).
- **No new witness refusal.** Revision 2 had a hard active cap answering `NO busy` and a matching
  `ask_witness` change on the readiness path. Revision 3 drops both: active witness connections stay
  unbounded as today, and only idle kept connections are bounded (4.4).
- **The witness's promotion uses Paxos.** `watch` decides a promotion with `propose` on `ha/<group>`
  (`snc_witness.cpp`), so pooling Paxos peers would have put the pool on the failover path. Revision 3 makes
  the pool a per-machine choice that excludes `ha/*` (4.5). Revision 4 narrows it further to the READ broadcasts
  of `read_latest` only (section 10).

## 10. Independent QA of revision 3 (revision 4 changes)

Three reviewers, briefed separately on the HA path, on ambiguous outcomes and fencing, and on compliance and
consistency. Each finding was checked in the code before it was applied.

| Finding | Severity | Applied |
|---|---|---|
| Promotion depends on placement and lease calls: PROMOTE → SIGUSR1 → `Engine::promote` runs PLACEMENT SYNC (start barrier) and LEASES TAKE, and either failing refuses the promotion (`engine.cpp`); revision 3 pooled exactly those | blocking | They join the HA path; every Paxos write stays one-shot; the pool takes only a closed list of requests (4.2, 4.5) |
| Idle kept connections freed on EMFILE would let a node out of descriptors answer HEALTH, which today stays silent (`admin_loop.h` EMFILE path) | blocking | Today's EMFILE rule kept; idle connections never freed for accept; small idle caps; A2 EMFILE run with the idle set populated (4.3) |
| The witness's descriptor and thread budget is shared with the HA path and its idle cap was unsized | should-fix | Cap sized against RLIMIT_NOFILE and threads (4.4). The witness accept loop's EMFILE spin and `ha::ask`'s unchecked `socket()` are pre-existing and recorded for their own slice |
| One `paxos::Node` mutex serializes all machines' `read_linear` and `propose` | should-fix | Stated in 4.5; only the PLACEMENT GET handler's READ, outside the mutex, is pooled |
| Handshakes could queue ahead of PROMOTE in an old node's heavy queue during rollout | should-fix | Witnesses and the HA path never handshake with a node (4.1) |
| 4.2 still put handshakes and replacements inline on a request's path | should-fix | All of them go one-shot (4.2) |
| `ha::ask`'s real budget is connect T, an unbounded blocking send, then a fresh T for the reply | should-fix | Stated; kept connections bound the send and keep the reply budget (4.2); A3(b) includes a large request |
| Late ACCEPT is accepted when no higher ballot was promised; no MOVE step is fenced by epoch; same-ballot ACCEPT is idempotent; PLACEMENT SYNC writes; MOVE HAVE is read-only; SYNCED is always refused | wrong claims | 4.5 and 4.7 corrected; SYNC removed from the retry list |
| A2 named new tests that `test_health` T4 to T6 already provide, missed the clustered case, the full HA verb list and a tolerance; the EMFILE test must assert counters; the stalled-accept hook does not exist on main | should-fix | A2 rewritten; hook and baseline in their own step before K0 (section 6) |
| A3(e) overclaimed rejection; the relay cannot exercise `TCP_USER_TIMEOUT`; it must keep forwarding held bytes; hook gates unnamed | should-fix | A3 rewritten |
| A5 "at most one failed request" is per pooled connection | should-fix | A5 |
| K0 is not test-only and landed before the HEALTH baseline; its client was unnamed | should-fix | Section 6; the extra baseline step is put to review |
| Leftovers: "age limits", lane names, `test_health` test list, a section 8 row, the churn document's "changes how silence and failover are detected" | nits | Fixed; the churn document points here |
| Paxos production deadline is 3000 ms | nit | 2.1 |

**Confirmation pass on revision 4** (one reviewer, all four checked in the code and applied):

| Finding | Severity | Applied |
|---|---|---|
| `propose` and `read_linear` begin with `read_latest`, so pooling "the `broadcast` in `read_latest`" would have pooled the READ round of every `ha/*` promotion, READY, and the promotion's PLACEMENT SYNC and LEASES TAKE | blocking | A `pooled` flag on `read_latest`, false by default, set only by the PLACEMENT GET handler; A2(a) counts per caller (4.2, 4.5) |
| Idle descriptors make exhaustion arrive sooner, on nodes and witnesses | should-fix | Stated in 4.5; A2(d) |
| UNSEAL is refused while its move is in progress; non-final EXPORT is unfenced | should-fix | 4.5, 4.7 |
| K0 ships sequence numbers and handback without A4 and A6 | should-fix | Section 6 |
| `resolve` at node start also uses GET | should-fix | Stays one-shot (4.2) |
| Section 8 and 9 rows superseded | nits | Marked |

Checked and confirmed correct by the reviewers: HEALTH's inline handler and the one-shot `kReadMs` deadline
from accept are untouched; `ask_witness`, rejoin and watcher deadlines; the detection bound; CAS, INIT, COMMIT,
LEASES TAKE, NOTREADY and DEGRADE classifications; sequence numbers prevent misattributed replies.

## Classification

**RECONSIDER** (proposed): one TCP connection per control request.

**KEEP**: every verb, payload and fencing check.

**REFINE** (proposed): `ha::ask` gains an opt-in pooled connection, and the admin and witness servers gain
keep-alive.

Nothing is authorized until reviewed.
