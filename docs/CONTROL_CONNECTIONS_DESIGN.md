# Persistent control-plane connections: design (2026-11-15, revision 2, for review)

Status: **design only; authorizes no code.**

This is the slice recommended in the PR #129 review (`docs/handoff/PR129_Connection_Churn_Review_Approval.md`):
"design persistent, reusable control-plane connections, preserving witness liveness detection, fencing, and
failover semantics." Background: `docs/TEST_MIGRATE_CONNECTION_CHURN.md` (C1). Revision 2 follows three
independent reviews (section 8).

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
   - **Opt-in keep-alive**, negotiated per connection by an explicit handshake that old servers refuse
     quickly and harmlessly.
   - **One request at a time**, each tagged with a sequence number that its reply echoes.
   - **No reuse after a failure:** the connection closes, with RST, on any timeout, error or mismatch.
   - **Retry only for read-only verbs**, and only when the reused connection gave no reply byte.
   - **Bounded idle connections**, evicted least recently used and never by refusing active requests.
   - **A kill switch.**
   - **Unchanged:** liveness, fencing and failover still rest on per-request deadlines and on the terms,
     epochs and ballots in the payloads (section 4).
5. **Slices.** K0 is test-only and switches the probe, so the test's churn falls about 5× with no product
   risk. K1 is the library and the servers, K2 the product callers, K3 the measurement. Each is its own PR,
   stopped for review (section 6).

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
| Paxos (`paxos.h` `broadcast`), peers including the witness itself | READ (`read_latest`); PREPARE, ACCEPT, COMMIT (`propose`, `read_linear`) | per decision and per read | 300 ms per ask, inside a 1000 or 3000 ms loop |
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
| **A failed connect means not sent** | A request whose connect fails was never delivered | one connection per request |
| **HEALTH is never behind heavy work** | HEALTH (exact match) is answered inline on the admin loop; STATUS, AUDIT, PROMOTE and others go in order to one worker (`kQueueMax` 16, then `NO busy`); MOVE gets its own thread | `admin_loop.h`, `docs/HEALTH_DECOUPLING.md` |
| **Accept path in the health check** | Every poll exercises accept: a node out of descriptors (EMFILE) or with a stuck loop reads as silent | one connection per request |
| **Bounded server state** | Admin: `kHalfMax` 256 half-read connections, each with a line due within `kReadMs` (500 ms) of accept; `kQueueMax`; `kOutMax` 64 waiting replies. Witness: one thread per connection, **no cap** | `admin_loop.h`; `snc_witness.cpp` |
| **Fencing** | Terms, epochs, ballots and space UUIDs are in payloads and checked by the receiver. Exception: PROMOTE carries no term (`snc_server.cpp`: if a standby, SIGUSR1); its safety rests on the Paxos decision before it and on reaching the right process | handlers |
| **Ambiguous outcome** | A request whose reply is lost may or may not have taken effect; callers treat false as unknown (table in 4.7) | callers |

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
  - The health lane never waits for it. HEALTH stays one-shot to a peer until a handshake on a separate
    connection has succeeded.
- **Requests on a kept connection.** Each request is `K <seq> <line>`, where `seq` counts up per
  connection, and the reply is `<seq> <reply>`. A reply whose sequence number does not match closes the
  connection. That gives defense in depth for the "no stale replies" property (review S7).
- **Unchanged.** Bare requests from old clients work as now. Verbs and replies are unchanged.
- **No newlines in replies.** No reply may contain a newline; the server asserts it.

### 4.2 Client (`ha::Conn`, `ha::Pool`)

- **Pool and lanes.** The pool is keyed by (host, port, lane). The lanes are health, control, move and
  probe, so HEALTH never shares a connection with a long request.
  - Each key holds a small free list, created on demand and capped at 4.
  - A caller never waits for a checkout: if the list is empty and at the cap, it uses a one-shot `ha::ask`.
    Paxos `read_latest` broadcasts from several serve threads therefore never queue behind each other or
    behind a promotion (review S3).
- **One request per connection.** A connection holds at most one request.
- **Same deadline as today.** `timeout_ms` still covers connect (or handshake, when a new connection is
  made), send and the full reply.
- **Close on failure, with RST.** On timeout, short reply, EOF, error or sequence mismatch, the connection
  is closed with RST (`SO_LINGER {1, 0}`) and never reused. The RST stops the kernel from retransmitting an
  unanswered request after the caller has given up.
- **`TCP_USER_TIMEOUT`** is set to the request deadline on kept sockets. The kernel then drops a connection
  whose data is unacknowledged past it.
- **Socket options.** Kept sockets set `TCP_NODELAY`, and each request is one `send`.
- **Reuse check.** Before reuse, an idle connection is polled with a 0 timeout. If it is readable (EOF, RST
  or stray bytes) or in error, it is closed and replaced within the same deadline.
- **Retry.** After a request has been written there is no retry, except one on a fresh connection within
  the same deadline. That retry needs all three of:
  - the verb is in the read-only allowlist (4.7);
  - the connection was reused, not new;
  - no reply byte arrived.

  This covers a server's idle close crossing the request (review S5). Every other failure returns false,
  as today.
- **Maximum age on the health lane.** Health connections are closed and reopened every 5 s, so the accept
  path and descriptor exhaustion are still exercised (review S2). The health lane still gets about a 50×
  reduction in connections. Other lanes close after 20 s idle.
- **Kill switch.** `SNC_HA_KEEP=0` (environment) and a `--no-keep` flag on servers turn keep-alive off end
  to end. The full suite runs both ways.

### 4.3 Admin server (`admin::Loop`)

- **Routing.** A connection whose first line is `KEEP 1` is marked kept. On a kept connection, `K <seq> `
  is stripped before routing, so `MOVE ` is still recognized by prefix and streamed as now. A test checks
  that `K <seq> MOVE INFO` reaches the MOVE path (review hazard 3).
- **Line deadline.** It starts at a request's first byte (`line_started_ns`), not at accept. The MOVE
  thread's deadline does too. An idle connection has its own idle deadline (30 s).
- **One handback path.**
  - Problem today: the worker and the MOVE thread each write and then close.
  - On a kept connection, both instead pass the fd and its sequence number back to the loop through a
    handback queue. They write with a write deadline (the worker's write has none today).
  - An eventfd in the poll set wakes the loop at once, so no request waits for the loop's 100 ms tick.
  - The loop restores O_NONBLOCK and puts the fd back in its read set.
  - The inline and POLLOUT reply paths do the same instead of closing.
- **Idle connections.**
  - They have their own cap, `kIdleMax`, sized against RLIMIT_NOFILE (proposed: the smaller of 512 and
    a quarter of the limit).
  - They are evicted least recently used, past the cap and also on EMFILE (today only half-read
    connections are freed on EMFILE).
  - `stop()` closes idle and handback fds.
- **Bytes after the request line.** A per-connection buffer keeps them. Any bytes after a request line,
  before its reply is sent, are a protocol error: the connection is closed.

### 4.4 Witness server (`serve()`)

- **Thread per connection.** A kept connection's thread loops: read the next line (deadline from its first
  byte, idle deadline 30 s), answer, repeat.
- **Bounds.**
  - Idle threads are bounded separately from active ones. Past the idle cap, the least recently used idle
    connection is closed (review B2).
  - Requests are never refused because of idle connections.
  - Slots are reserved for the peer witnesses and the witness itself, since Paxos asks itself over TCP.
  - If even active threads hit a hard cap, the witness answers `NO busy`.
- **`ask_witness`** in `snc_server` treats `NO busy` like `NO unknown request` and tries the next witness,
  instead of taking it as final (review hazard 8).

### 4.5 Liveness, fencing, failover

- **Detection.** A SIGKILLed process sends FIN or RST, which the reuse check or the read sees at once. A
  SIGSTOPped process, a dead host, a one-way partition or a half-open connection all fail the request at its
  deadline. In each case the connection is then closed. The watcher counts silence exactly as today.
  Detection stays within `failover_ms` plus up to 2 × `poll_ms`, as now.
- **Accept path.** Kept health connections would no longer exercise accept. The 5 s maximum age keeps a
  node out of descriptors, or with a stuck accept, visible within 5 s.
  - This is a deliberate trade: a node whose loop answers on an existing connection but cannot accept is
    reported up for at most 5 s.
  - **For decision** at review, in the style of HEALTH_DECOUPLING's decisions.
- **Late delivery (new).** A request written into a silently partitioned, established connection can be
  delivered after the caller gave up, until `TCP_USER_TIMEOUT` and the RST close end it. Today that window
  does not exist, because a connect would fail.
  - Every current verb tolerates late delivery (4.7). The design states it, and a test black-holes an
    established connection in each direction.
- **Fencing.** No request gains authority from its connection, and every term, epoch and ballot check
  stays in its handler.
  - PROMOTE has no term, so it stays one-shot. A PROMOTE is never sent on a connection that might reach
    another process: a kept connection cannot outlive its process, and a one-shot reconnects by address,
    as today.
- **Failover.** The decision path is unchanged: `failover_ms` of silence, then a Paxos decision, then
  PROMOTE until the standby serves.
- **Paxos.** Asks keep their 300 ms per-peer deadline. `broadcast` joins every per-peer thread, and a
  timed-out ask closes its connection, so a late PROMISE or ACCEPTED is never read in a later phase.
  Sequence numbers back that up. Ballot checks are untouched.

### 4.6 What it costs

- **Server resources.** One descriptor per idle admin connection; one thread per idle witness connection.
  Witness counts grow with the number of nodes (placement and readiness lanes): about 2 to 3 per node per
  witness. The caps and LRU eviction bound them.
- **Middleboxes.** Ones that drop idle flows silently cause one timed-out request, then a reconnect. The
  idle and age limits keep flows short.
  - **Open:** deployments' middlebox idle timeouts.
- **Complexity.** A pool, a handshake, a handback path and sequence numbers. Sections 4.2 to 4.4 put each
  in one place.

### 4.7 Verbs: read-only, idempotent, or not (**Observed** from the handlers)

| Verb | Class | If a lost reply hides that it ran |
|---|---|---|
| HEALTH, STATUS, GAUGES, MOVE INFO, MOVE ROLE, READ, PLACEMENT GET, PLACEMENT SYNC | read-only (retry allowed, 4.2) | nothing |
| COMMIT, PLACEMENT INIT (same body), MOVE STAGE/EXPORT/SEAL/ACTIVATE/UNSEAL/REFRESH/HAVE | idempotent | nothing; the caller re-reads |
| PREPARE, ACCEPT | repeat at the same ballot is NACKed | liveness only |
| PLACEMENT CAS | version-checked | the retry reads `NO version`; the caller re-reads, as today |
| LEASES TAKE | not idempotent | a block of leases is wasted |
| NOTREADY, DEGRADE | each chooses a new version, by design | pessimistic; READY names `after=N` |
| READY, SYNCED | conditional on `after=N` | refused if overtaken |
| PROMOTE | no term in the payload | stays one-shot; Paxos decides first |

## 5. Acceptance criteria

| # | Criterion | How |
|---|---|---|
| A1 | `test_migrate` connections per run fall at least 10× (conntrack peak from a drained table, from about 100k), with descriptor and thread counts reported | hosted, 6 runners, as the churn document's arm A |
| A2 | Detection-time distribution of a stopped primary (SIGSTOP, not only SIGKILL) unchanged, before and after the K2 watcher switch | new test, run on main and on K2 |
| A3 | A stopped, killed or black-holed peer on a kept connection is detected at the same deadline as on a new one; a black-holed request is not delivered after `TCP_USER_TIMEOUT` | new deterministic tests, each direction |
| A4 | No stale reply or desynchronized connection: a late reply, a wrong sequence number or bytes after a line each close the connection, and the next request gets its own reply | new deterministic tests |
| A5 | Idle eviction, a server restart, a handshake to an old server: at most one failed request, never a false failover; read-only verbs recover by the retry | new tests |
| A6 | HEALTH is never delayed by a heavy request, on another lane or while the same node's worker is stalled; a new witness polling an old node under `DEBUG STALL` and a full heavy queue never promotes | `test_admin_loop.cpp`; mixed-version test |
| A7 | Witness under more than its idle cap of clients still completes a promotion and a grant | new test |
| A8 | With keep-alive on, these pass: `test_health` (T1, T3, T6, T10b, T11), the grant-vs-promotion race in `test_witness.py`, `test_paxos_ballots`, `test_ready_order`, q10 (`test_baseline_move.py`), `test_async_failover.py`, `test_migrate`, and the full suite. The same suite also passes with `SNC_HA_KEEP=0` | CI |

## 6. Slices (each its own PR, stopped for review)

- **K0, test only.** The `test_migrate` probe and `node_info` keep one connection per node and lane for the
  scenario, using the bare protocol where a node allows it.
  - Not possible today: the admin server closes after each reply.
  - So K0 needs only the admin server's side of K1 (4.3), applied first, plus the probe switch.
  - That removes about 80% of the test's churn, and no product caller changes behavior.
- **K1, library and servers.** `ha::Conn`, `ha::Pool`, the handshake, sequence numbers, the admin handback
  path and the witness changes. Tests A3 to A7. No product caller changes.
- **K2, product callers.** In order of churn: Paxos peers, the engine's placement poll, then the witness
  watcher's health lane. Each is switched behind the pool, and its tests (A2, A8) run before the next one
  is switched.
- **K3, measurement.** Hosted A1 and A2, before and after. If A1 holds, lower the PR #129 guard's per-scenario
  figure, with evidence.

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
| `K HEALTH` to an old node fails the inline exact match (`snc_server.cpp` `line == "HEALTH"`) and queues behind heavy verbs; it times out without `NO unknown request`, so the client never learns, and a false failover returns | blocking | Per-connection `KEEP 1` handshake; nothing prefixed to unconfirmed peers; the health lane is never held by a handshake (4.1) |
| A witness cap that refuses new connections would refuse Paxos peers and readiness once idle clients fill it | blocking | LRU eviction of idle connections; reserved peer slots; `ask_witness` skips `NO busy` (4.4) |
| `K MOVE` misses the `MOVE ` prefix routing; the K1 probe switch would silently fall back | high | Strip `K <seq>` before routing; test (4.3) |
| Heavy worker had no handback; deadlines keyed to accept time; no wake on handback; LRU eviction and EMFILE | high | One handback path with an eventfd; line deadline from the first byte; LRU on cap and on EMFILE (4.3) |
| Late delivery on established connections is new | should-fix | `TCP_USER_TIMEOUT`, RST on failure, stated in 4.5, test in A3 |
| The health check stops exercising accept and EMFILE | should-fix | 5 s maximum age on health connections; recorded for decision (4.5) |
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

## Classification

**RECONSIDER** (proposed): one TCP connection per control request.

**KEEP**: every verb, payload and fencing check.

**REFINE** (proposed): `ha::ask` gains an opt-in pooled connection, and the admin and witness servers gain
keep-alive.

Nothing is authorized until reviewed.
